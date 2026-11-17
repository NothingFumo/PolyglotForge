/*
 * Python UDF 阶段：把静态 CPython 嵌进宿主进程，用脚本插件注册 UDF。
 *
 * 契约（contracts/qf.h）：
 *   qf_py_load     初始化解释器（关掉 site）、把插件目录加入 sys.path、执行插件文件；
 *   qf_py_call     按名字从插件命名空间取可调用对象，qf_value 与 Python 对象互转；
 *   qf_py_shutdown 结束解释器并释放插件引用。
 *
 * 约束：
 *   - 只暴露 C ABI，跨边界不传 PyObject，也不让宿主看到解释器内部状态。
 *   - 输出文本由 host->alloc 拷进 arena 并补 NUL，生命周期归宿主，Python 侧引用立即释放。
 *   - 静态库没有编译期 home：解释器从 PYTHONHOME 环境变量定位标准库（单文件产物运行时
 *     由载荷解包目录提供），插件目录则始终显式插到 sys.path 首位。
 *   - 插件用顶层 `def score(row): ...` 注册；qf_py_call 直接按名字查模块字典。
 */
#define Py_NO_ENABLE_SHARED 1
#include <Python.h>

#include "qf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef QF_HAVE_PY_PAYLOAD
#include <direct.h>
#include <windows.h>

/* ld -r -b binary 对 out/payload/lib.zip 生成的符号。 */
extern const unsigned char _binary_payload_lib_zip_start[];
extern const unsigned char _binary_payload_lib_zip_end[];

static uint64_t fnv1a(const unsigned char* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int file_has_size(const char* path, long long want) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long long sz = _ftelli64(f);
    fclose(f);
    return sz == want;
}

/*
 * 把内嵌的标准库 zip 落到 %LOCALAPPDATA%\PolyglotForge\payload\<hash>\lib.zip。
 * 返回 1 表示路径可用（写入 out，UTF-8，NUL 结尾）。
 * 以"内容指纹 + 长度"做目录名：换构建即换目录，避免读到旧标准库。
 */
static int payload_materialize(char* out, size_t cap) {
    const unsigned char* data = _binary_payload_lib_zip_start;
    const size_t size = (size_t)(_binary_payload_lib_zip_end - _binary_payload_lib_zip_start);
    if (size == 0) return 0;

    const char* base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) return 0;

    uint64_t h = fnv1a(data, size < 4096 ? size : 4096) ^ (uint64_t)size;
    char dir[1024];
    int n = snprintf(dir, sizeof dir, "%s\\PolyglotForge\\payload\\%016llx",
                     base, (unsigned long long)h);
    if (n <= 0 || (size_t)n >= sizeof dir) return 0;

    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char* p = tmp; *p; ++p) {
        if (*p == '\\') {
            *p = '\0';
            _mkdir(tmp);
            *p = '\\';
        }
    }
    _mkdir(tmp);

    snprintf(out, cap, "%s\\lib.zip", dir);
    if (file_has_size(out, (long long)size)) return 1;

    FILE* f = fopen(out, "wb");
    if (!f) return 0;
    size_t wrote = fwrite(data, 1, size, f);
    fclose(f);
    if (wrote != size) {
        remove(out);
        return 0;
    }
    return 1;
}
#endif

/* 解释器与插件状态。UDF 求值在单进程内共享一个解释器。 */
static int g_initialized;
static int g_loaded;
static PyObject* g_module; /* 插件模块，持有 globals 字典 */
static PyObject* g_ns;     /* 借自 g_module 的 globals 字典 */

/* 没有 host->str 时诊断文本的落点。单线程求值，静态缓冲足够。 */
static char g_diag_buf[512];

/* 把一段字节交给宿主分配器，并保证 NUL 结尾。 */
static qf_str host_text(const qf_host* host, const char* src, size_t len) {
    if (host && host->str) return host->str(host->ctx, src, (uint32_t)len);

    char* p = NULL;
    if (host && host->alloc) {
        p = (char*)host->alloc(host->ctx, (uint64_t)len + 1, 1);
        if (p) {
            if (len) memcpy(p, src, len);
            p[len] = '\0';
            qf_str r;
            r.ptr = p;
            r.len = (uint32_t)len;
            return r;
        }
    }

    size_t n = len < sizeof(g_diag_buf) - 1 ? len : sizeof(g_diag_buf) - 1;
    if (n) memcpy(g_diag_buf, src, n);
    g_diag_buf[n] = '\0';
    qf_str r;
    r.ptr = g_diag_buf;
    r.len = (uint32_t)n;
    return r;
}

/* 诊断：code + "前缀：消息"。前缀固定为中文，消息保持原文。 */
static void set_diag(const qf_host* host, qf_diag* diag, qf_err code,
                     const char* prefix, const char* msg) {
    if (!diag) return;
    diag->code = code;
    diag->line = 0;
    diag->col = 0;
    diag->hint.ptr = NULL;
    diag->hint.len = 0;

    size_t pl = strlen(prefix);
    size_t ml = msg ? strlen(msg) : 0;
    char* tmp = (char*)malloc(pl + 3 + ml + 1);
    if (!tmp) {
        diag->message = host_text(host, prefix, pl);
        return;
    }
    memcpy(tmp, prefix, pl);
    tmp[pl] = '\xEF'; /* UTF-8 全角冒号 U+FF1A */
    tmp[pl + 1] = '\xBC';
    tmp[pl + 2] = '\x9A';
    if (ml) memcpy(tmp + pl + 3, msg, ml);
    tmp[pl + 3 + ml] = '\0';
    diag->message = host_text(host, tmp, pl + 3 + ml);
    free(tmp);
}

/*
 * 取走当前 Python 异常并转成诊断。异常文本优先用 str(exc)，取不到退到异常类型名。
 * 调用方负责返回 QF_ERR_UDF。
 */
static void diag_from_python(const qf_host* host, qf_diag* diag, const char* prefix) {
    PyObject* type = NULL;
    PyObject* value = NULL;
    PyObject* tb = NULL;
    const char* text = NULL;
    PyObject* str = NULL;

    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    if (value) {
        str = PyObject_Str(value);
        if (str) text = PyUnicode_AsUTF8(str);
    }
    if (!text && type && PyType_Check(type)) {
        text = ((PyTypeObject*)type)->tp_name;
    }
    set_diag(host, diag, QF_ERR_UDF, prefix, text ? text : "(未知异常)");

    Py_XDECREF(str);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    PyErr_Clear();
}

/* 非 NUL 结尾的 qf_str 转成可传给 fopen / PyRun_FileExFlags 的窄字符串。 */
static char* dup_bytes(const char* p, uint32_t n) {
    char* s = (char*)malloc((size_t)n + 1);
    if (!s) return NULL;
    if (n) memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

/* 启动解释器：等价 `python -S`，不跑 site、不写 .pyc、不装信号处理器。 */
static qf_err py_start(const qf_host* host, qf_diag* diag) {
    if (g_initialized) return QF_ERR_OK;

    PyConfig cfg;
    PyConfig_InitPythonConfig(&cfg);
    cfg.parse_argv = 0;
    cfg.site_import = 0;
    cfg.user_site_directory = 0;
    cfg.install_signal_handlers = 0;
    cfg.write_bytecode = 0;
    /* 标准库来自内嵌 zip，getpath 找不到 PREFIX 属预期，不要再刷警告。 */
    cfg.pathconfig_warnings = 0;

    char msg[512];
    msg[0] = '\0';
    PyStatus st = PyConfig_SetBytesString(&cfg, &cfg.program_name, "queryforge");
    /*
     * 静态库没有编译期 PREFIX：必须自己把 home 钉住，否则 getpath 会按 exe 位置
     * 瞎猜并打印 "Could not find platform independent libraries"。
     * 单文件产物运行时把 PYTHONHOME 指向载荷解包目录（含 Lib/），源码树里则是
     * vendor/Python-3.11.9 本身。
     */
    if (!PyStatus_Exception(st)) {
        const char* home = getenv("PYTHONHOME");
        if (home && home[0] != '\0') {
            st = PyConfig_SetBytesString(&cfg, &cfg.home, home);
        }
    }
#ifdef QF_HAVE_PY_PAYLOAD
    /*
     * 标准库随 exe 内嵌：释放到本地缓存后，直接把 lib.zip 挂到模块搜索路径。
     * zipimport 原生支持 zip，不需要解压；显式设置搜索路径后，
     * getpath 不再按 exe 位置猜测 PREFIX，也就不会打印 <prefix> 警告。
     */
    if (!PyStatus_Exception(st)) {
        char zip_path[1024];
        wchar_t wpath[1024];
        if (payload_materialize(zip_path, sizeof zip_path) &&
            MultiByteToWideChar(CP_UTF8, 0, zip_path, -1, wpath, 1024) > 0) {
            cfg.module_search_paths_set = 1;
            PyStatus ast = PyWideStringList_Append(&cfg.module_search_paths, wpath);
            if (PyStatus_Exception(ast)) st = ast;
        }
    }
#endif
    if (!PyStatus_Exception(st)) st = Py_InitializeFromConfig(&cfg);
    if (PyStatus_Exception(st)) {
        const char* err = st.err_msg ? st.err_msg : "解释器初始化失败";
        snprintf(msg, sizeof msg, "%s", err);
    }
    PyConfig_Clear(&cfg);

    if (msg[0] != '\0') {
        set_diag(host, diag, QF_ERR_EXEC, "Python 初始化失败", msg);
        return QF_ERR_EXEC;
    }
    g_initialized = 1;
    return QF_ERR_OK;
}

/* 把插件所在目录插到 sys.path 首位，让插件能 import 同目录的兄弟模块。 */
static int sys_path_add(const char* path, uint32_t len) {
    uint32_t cut = len;
    while (cut > 0 && path[cut - 1] != '/' && path[cut - 1] != '\\') --cut;

    const char* dir = path;
    uint32_t dlen = cut;
    if (cut == 0) {
        dir = ".";
        dlen = 1;
    } else if (cut > 1) {
        dlen = cut - 1; /* 去掉尾部斜杠 */
    }

    PyObject* sys_path = PySys_GetObject("path");
    if (!sys_path) return -1;
    PyObject* entry = PyUnicode_DecodeFSDefaultAndSize(dir, (Py_ssize_t)dlen);
    if (!entry) return -1;

    int found = PySequence_Contains(sys_path, entry);
    if (found == 0) found = PyList_Insert(sys_path, 0, entry) == 0 ? 1 : -1;
    Py_DECREF(entry);
    return found;
}

qf_err qf_py_load(const qf_host* host, qf_str plugin_path, qf_diag* diag) {
    if (!host || !host->alloc || !plugin_path.ptr || plugin_path.len == 0) {
        set_diag(host, diag, QF_ERR_ARG, "Python 插件参数无效", "需要非空路径与宿主分配器");
        return QF_ERR_ARG;
    }

    qf_err rc = py_start(host, diag);
    if (rc != QF_ERR_OK) return rc;

    char* path = dup_bytes(plugin_path.ptr, plugin_path.len);
    if (!path) {
        set_diag(host, diag, QF_ERR_EXEC, "Python 插件路径分配失败", "内存不足");
        return QF_ERR_EXEC;
    }

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        char why[128];
        snprintf(why, sizeof why, "%s", strerror(errno));
        set_diag(host, diag, QF_ERR_IO, "无法打开 Python 插件", why);
        free(path);
        return QF_ERR_IO;
    }

    PyGILState_STATE gil = PyGILState_Ensure();

    PyObject* module = PyModule_New("qf_plugin");
    if (!module) {
        fclose(fp);
        diag_from_python(host, diag, "Python 插件模块创建失败");
        PyGILState_Release(gil);
        free(path);
        return QF_ERR_UDF;
    }
    PyObject* ns = PyModule_GetDict(module); /* 借用 */

    PyObject* file_u = PyUnicode_DecodeFSDefault(path);
    if (!file_u || PyDict_SetItemString(ns, "__file__", file_u) != 0) {
        Py_XDECREF(file_u);
        Py_DECREF(module);
        fclose(fp);
        diag_from_python(host, diag, "Python 插件模块创建失败");
        PyGILState_Release(gil);
        free(path);
        return QF_ERR_UDF;
    }
    Py_DECREF(file_u);

    if (sys_path_add(plugin_path.ptr, plugin_path.len) < 0) {
        Py_DECREF(module);
        fclose(fp);
        diag_from_python(host, diag, "Python 插件目录无法加入 sys.path");
        PyGILState_Release(gil);
        free(path);
        return QF_ERR_UDF;
    }

    /* closeit=1：执行结束由 Python 关闭文件。 */
    PyObject* ran = PyRun_FileExFlags(fp, path, Py_file_input, ns, ns, 1, NULL);
    if (!ran) {
        diag_from_python(host, diag, "Python 插件执行失败");
        Py_DECREF(module);
        PyGILState_Release(gil);
        free(path);
        return QF_ERR_UDF;
    }
    Py_DECREF(ran);

    Py_XSETREF(g_module, module);
    g_ns = ns;
    g_loaded = 1;

    PyGILState_Release(gil);
    free(path);
    return QF_ERR_OK;
}

/* qf_value → Python。空值映射成 None。 */
static PyObject* value_to_py(const qf_value* v) {
    switch (v->type) {
    case QF_T_INT: return PyLong_FromLongLong((long long)v->as.i);
    case QF_T_DOUBLE: return PyFloat_FromDouble(v->as.d);
    case QF_T_TEXT:
        return PyUnicode_FromStringAndSize(v->as.s.ptr, (Py_ssize_t)v->as.s.len);
    case QF_T_NULL:
    default:
        Py_INCREF(Py_None);
        return Py_None;
    }
}

/* Python → qf_value。文本拷进 arena，Python 对象随后丢弃。 */
static qf_err value_from_py(const qf_host* host, PyObject* v, qf_value* out, qf_diag* diag) {
    memset(out, 0, sizeof *out);
    out->type = QF_T_NULL;

    if (v == Py_None) return QF_ERR_OK;

    if (PyBool_Check(v)) {
        out->type = QF_T_INT;
        out->as.i = (v == Py_True) ? 1 : 0;
        return QF_ERR_OK;
    }

    if (PyLong_Check(v)) {
        long long i = PyLong_AsLongLong(v);
        if (i == -1 && PyErr_Occurred()) {
            diag_from_python(host, diag, "Python UDF 返回值超出整数范围");
            return QF_ERR_UDF;
        }
        out->type = QF_T_INT;
        out->as.i = (int64_t)i;
        return QF_ERR_OK;
    }

    if (PyFloat_Check(v)) {
        out->type = QF_T_DOUBLE;
        out->as.d = PyFloat_AS_DOUBLE(v);
        return QF_ERR_OK;
    }

    if (PyUnicode_Check(v)) {
        Py_ssize_t n = 0;
        const char* s = PyUnicode_AsUTF8AndSize(v, &n);
        if (!s) {
            diag_from_python(host, diag, "Python UDF 返回值无法编码为 UTF-8");
            return QF_ERR_UDF;
        }
        if (n < 0 || (uint64_t)n > 0xFFFFFFFFu) {
            set_diag(host, diag, QF_ERR_TYPE, "Python UDF 返回文本过长", "超过 4GiB");
            return QF_ERR_TYPE;
        }
        char* p = (char*)host->alloc(host->ctx, (uint64_t)n + 1, 1);
        if (!p) {
            set_diag(host, diag, QF_ERR_EXEC, "Python UDF 返回值写入失败", "宿主分配器返回空");
            return QF_ERR_EXEC;
        }
        if (n) memcpy(p, s, (size_t)n);
        p[n] = '\0';
        out->type = QF_T_TEXT;
        out->as.s.ptr = p;
        out->as.s.len = (uint32_t)n;
        return QF_ERR_OK;
    }

    /* 契约只有整数/浮点/文本/空值，其它 Python 类型不跨边界。 */
    const char* tn = Py_TYPE(v)->tp_name;
    set_diag(host, diag, QF_ERR_TYPE, "Python UDF 返回了契约外的类型", tn);
    return QF_ERR_TYPE;
}

qf_err qf_py_call(const qf_host* host, qf_str fn, const qf_value* args,
                  uint32_t argc, qf_value* out, qf_diag* diag) {
    if (!host || !host->alloc || !out || !fn.ptr || fn.len == 0) {
        set_diag(host, diag, QF_ERR_ARG, "Python UDF 参数无效", "需要宿主、函数名与输出槽");
        return QF_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    out->type = QF_T_NULL;

    if (argc > 0 && !args) {
        set_diag(host, diag, QF_ERR_ARG, "Python UDF 参数无效", "argc 非零但 args 为空");
        return QF_ERR_ARG;
    }
    if (!g_loaded) {
        set_diag(host, diag, QF_ERR_UDF, "Python 插件未加载", "先调用 qf_py_load");
        return QF_ERR_UDF;
    }

    char* name = dup_bytes(fn.ptr, fn.len);
    if (!name) {
        set_diag(host, diag, QF_ERR_EXEC, "Python UDF 名字分配失败", "内存不足");
        return QF_ERR_EXEC;
    }

    qf_err rc = QF_ERR_OK;
    PyGILState_STATE gil = PyGILState_Ensure();

    PyObject* callable = PyDict_GetItemString(g_ns, name); /* 借用 */
    if (!callable) {
        set_diag(host, diag, QF_ERR_UDF, "Python UDF 不存在", name);
        rc = QF_ERR_UDF;
        goto done;
    }
    if (!PyCallable_Check(callable)) {
        set_diag(host, diag, QF_ERR_UDF, "Python UDF 不是可调用对象", name);
        rc = QF_ERR_UDF;
        goto done;
    }

    PyObject* argv = PyTuple_New((Py_ssize_t)argc);
    if (!argv) {
        diag_from_python(host, diag, "Python UDF 参数打包失败");
        rc = QF_ERR_UDF;
        goto done;
    }
    for (uint32_t i = 0; i < argc; ++i) {
        PyObject* a = value_to_py(&args[i]);
        if (!a) {
            Py_DECREF(argv);
            diag_from_python(host, diag, "Python UDF 参数转换失败");
            rc = QF_ERR_UDF;
            goto done;
        }
        PyTuple_SET_ITEM(argv, (Py_ssize_t)i, a); /* 偷引用 */
    }

    PyObject* res = PyObject_Call(callable, argv, NULL);
    Py_DECREF(argv);
    if (!res) {
        diag_from_python(host, diag, "Python UDF 执行失败");
        rc = QF_ERR_UDF;
        goto done;
    }
    rc = value_from_py(host, res, out, diag);
    Py_DECREF(res);

done:
    PyGILState_Release(gil);
    free(name);
    return rc;
}

void qf_py_shutdown(void) {
    if (!g_initialized) return;
    Py_CLEAR(g_module);
    g_ns = NULL;
    g_loaded = 0;
    Py_FinalizeEx();
    g_initialized = 0;
}
