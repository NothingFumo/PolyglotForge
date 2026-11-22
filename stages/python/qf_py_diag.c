/*
 * Python 阶段的诊断：把错误码、固定前缀与可变消息折成 qf_diag。
 * 从 qf_py.c 抽出，使值转换模块不必依赖解释器生命周期。
 */
#include <stdlib.h>
#include <string.h>

#include "qf_py_internal.h"

/* 没有 host->str 时诊断文本的落点。单线程求值，静态缓冲足够。 */
static char g_diag_buf[512];

/* 把一段字节交给宿主分配器，并保证 NUL 结尾。 */
static qf_str host_text(const qf_host* host, const char* src, size_t len) {
    if (host && host->str) return host->str(host->ctx, src, (uint32_t)len);
    size_t n = len < sizeof(g_diag_buf) - 1 ? len : sizeof(g_diag_buf) - 1;
    if (n) memcpy(g_diag_buf, src, n);
    g_diag_buf[n] = '\0';
    qf_str r;
    r.ptr = g_diag_buf;
    r.len = (uint32_t)n;
    return r;
}

/* 诊断：code + "前缀：消息"。前缀固定为中文，消息保持原文。 */
void qf_py_set_diag(const qf_host* host, qf_diag* diag, qf_err code,
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
void qf_py_diag_from_python(const qf_host* host, qf_diag* diag, const char* prefix) {
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
    qf_py_set_diag(host, diag, QF_ERR_UDF, prefix, text ? text : "(未知异常)");

    Py_XDECREF(str);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    PyErr_Clear();
}
