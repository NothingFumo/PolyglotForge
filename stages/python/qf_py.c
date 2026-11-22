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
 *   - 标准库来自内嵌 zip（构建期冻结的 encodings + 内存 finder），运行期不落盘；
 *     插件目录始终显式插到 sys.path 首位。
 *   - 插件用顶层 `def score(row): ...` 注册；qf_py_call 直接按名字查模块字典。
 */
#define Py_NO_ENABLE_SHARED 1
#include <Python.h>

#include "qf.h"
#include "qf_py_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef QF_HAVE_PY_PAYLOAD
#include "out/zip_finder_src.h"

/* ld -r -b binary 对 out/payload/lib.zip 生成的符号（只读 .rdata 段）。 */
extern const unsigned char _binary_payload_lib_zip_start[];
extern const unsigned char _binary_payload_lib_zip_end[];
#endif

/* 解释器与插件状态。UDF 求值在单进程内共享一个解释器。 */
static int g_initialized;
static int g_loaded;
static PyObject* g_module; /* 插件模块，持有 globals 字典 */
static PyObject* g_ns;     /* 借自 g_module 的 globals 字典 */

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

    /* encodings 在导入机制可用之前就要能导入：它是构建期冻结进
       Python/frozen.c 的（scripts/freeze_encodings.py --install，见 ADR 0008）。
       运行期替换 _PyImport_FrozenStdlib 指针不生效，因为 FrozenImporter
       读的是编译期数组。 */

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
     * 静态库没有编译期 PREFIX：PYTHONHOME 仅作为本地调试时的显式指定（指向
     * vendor 源码树）。正式产物不设也可，标准库来自内嵌 zip。
     */
    if (!PyStatus_Exception(st)) {
        const char* home = getenv("PYTHONHOME");
        if (home && home[0] != '\0') {
            st = PyConfig_SetBytesString(&cfg, &cfg.home, home);
        }
    }
    if (!PyStatus_Exception(st)) st = Py_InitializeFromConfig(&cfg);
    if (PyStatus_Exception(st)) {
        const char* err = st.err_msg ? st.err_msg : "解释器初始化失败";
        snprintf(msg, sizeof msg, "%s", err);
    }
    PyConfig_Clear(&cfg);

    if (msg[0] != '\0') {
        qf_py_set_diag(host, diag, QF_ERR_EXEC, "Python 初始化失败", msg);
        return QF_ERR_EXEC;
    }
    g_initialized = 1;

#ifdef QF_HAVE_PY_PAYLOAD
    /*
     * 把内嵌 zip 交给内存 finder：不落盘、不设搜索路径。
     * finder 只用内建模块（sys/_struct/zlib），所以此时不需要标准库。
     */
    {
        const unsigned char* zb = _binary_payload_lib_zip_start;
        const size_t zn = (size_t)(_binary_payload_lib_zip_end - _binary_payload_lib_zip_start);
        PyObject* bytes = PyBytes_FromStringAndSize((const char*)zb, (Py_ssize_t)zn);
        PyObject* builtins = PyEval_GetBuiltins();
        if (bytes && builtins) {
            PyDict_SetItemString(builtins, "__qf_zip__", bytes);
        }
        Py_XDECREF(bytes);
        if (PyRun_SimpleString(qf_zip_finder_src) != 0) {
            PyErr_Clear();
            qf_py_set_diag(host, diag, QF_ERR_EXEC, "Python 标准库导入器初始化失败",
                     "内嵌 zip 无法安装为 meta_path finder");
            return QF_ERR_EXEC;
        }
    }
#endif
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
        qf_py_set_diag(host, diag, QF_ERR_ARG, "Python 插件参数无效", "需要非空路径与宿主分配器");
        return QF_ERR_ARG;
    }

    qf_err rc = py_start(host, diag);
    if (rc != QF_ERR_OK) return rc;

    char* path = dup_bytes(plugin_path.ptr, plugin_path.len);
    if (!path) {
        qf_py_set_diag(host, diag, QF_ERR_EXEC, "Python 插件路径分配失败", "内存不足");
        return QF_ERR_EXEC;
    }

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        char why[128];
        snprintf(why, sizeof why, "%s", strerror(errno));
        qf_py_set_diag(host, diag, QF_ERR_IO, "无法打开 Python 插件", why);
        free(path);
        return QF_ERR_IO;
    }

    PyGILState_STATE gil = PyGILState_Ensure();

    PyObject* module = PyModule_New("qf_plugin");
    if (!module) {
        fclose(fp);
        qf_py_diag_from_python(host, diag, "Python 插件模块创建失败");
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
        qf_py_diag_from_python(host, diag, "Python 插件模块创建失败");
        PyGILState_Release(gil);
        free(path);
        return QF_ERR_UDF;
    }
    Py_DECREF(file_u);

    if (sys_path_add(plugin_path.ptr, plugin_path.len) < 0) {
        Py_DECREF(module);
        fclose(fp);
        qf_py_diag_from_python(host, diag, "Python 插件目录无法加入 sys.path");
        PyGILState_Release(gil);
        free(path);
        return QF_ERR_UDF;
    }

    /* closeit=1：执行结束由 Python 关闭文件。 */
    PyObject* ran = PyRun_FileExFlags(fp, path, Py_file_input, ns, ns, 1, NULL);
    if (!ran) {
        qf_py_diag_from_python(host, diag, "Python 插件执行失败");
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

qf_err qf_py_call(const qf_host* host, qf_str fn, const qf_value* args,
                  uint32_t argc, qf_value* out, qf_diag* diag) {
    if (!host || !host->alloc || !out || !fn.ptr || fn.len == 0) {
        qf_py_set_diag(host, diag, QF_ERR_ARG, "Python UDF 参数无效", "需要宿主、函数名与输出槽");
        return QF_ERR_ARG;
    }
    memset(out, 0, sizeof *out);
    out->type = QF_T_NULL;

    if (argc > 0 && !args) {
        qf_py_set_diag(host, diag, QF_ERR_ARG, "Python UDF 参数无效", "argc 非零但 args 为空");
        return QF_ERR_ARG;
    }
    if (!g_loaded) {
        qf_py_set_diag(host, diag, QF_ERR_UDF, "Python 插件未加载", "先调用 qf_py_load");
        return QF_ERR_UDF;
    }

    char* name = dup_bytes(fn.ptr, fn.len);
    if (!name) {
        qf_py_set_diag(host, diag, QF_ERR_EXEC, "Python UDF 名字分配失败", "内存不足");
        return QF_ERR_EXEC;
    }

    qf_err rc = QF_ERR_OK;
    PyGILState_STATE gil = PyGILState_Ensure();

    PyObject* callable = PyDict_GetItemString(g_ns, name); /* 借用 */
    if (!callable) {
        qf_py_set_diag(host, diag, QF_ERR_UDF, "Python UDF 不存在", name);
        rc = QF_ERR_UDF;
        goto done;
    }
    if (!PyCallable_Check(callable)) {
        qf_py_set_diag(host, diag, QF_ERR_UDF, "Python UDF 不是可调用对象", name);
        rc = QF_ERR_UDF;
        goto done;
    }

    PyObject* argv = PyTuple_New((Py_ssize_t)argc);
    if (!argv) {
        qf_py_diag_from_python(host, diag, "Python UDF 参数打包失败");
        rc = QF_ERR_UDF;
        goto done;
    }
    for (uint32_t i = 0; i < argc; ++i) {
        PyObject* a = qf_py_value_to_py(&args[i]);
        if (!a) {
            Py_DECREF(argv);
            qf_py_diag_from_python(host, diag, "Python UDF 参数转换失败");
            rc = QF_ERR_UDF;
            goto done;
        }
        PyTuple_SET_ITEM(argv, (Py_ssize_t)i, a); /* 偷引用 */
    }

    PyObject* res = PyObject_Call(callable, argv, NULL);
    Py_DECREF(argv);
    if (!res) {
        qf_py_diag_from_python(host, diag, "Python UDF 执行失败");
        rc = QF_ERR_UDF;
        goto done;
    }
    rc = qf_py_value_from_py(host, res, out, diag);
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
