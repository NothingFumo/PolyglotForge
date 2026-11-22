/*
 * 值转换：qf_value（契约）与 Python 对象之间的双向映射。
 *
 * 只依赖 contracts/qf.h 与 Python C API，不碰解释器生命周期——可以脱开
 * Py_Initialize 单独编译与审查。真正的边界测试仍走 selftest.c 的公共接口。
 */
#include <string.h>

#include "qf_py_internal.h"

/* qf_value → Python。空值映射成 None。 */
PyObject* qf_py_value_to_py(const qf_value* v) {
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
qf_err qf_py_value_from_py(const qf_host* host, PyObject* v, qf_value* out, qf_diag* diag) {
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
            qf_py_diag_from_python(host, diag, "Python UDF 返回值超出整数范围");
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
            qf_py_diag_from_python(host, diag, "Python UDF 返回值无法编码为 UTF-8");
            return QF_ERR_UDF;
        }
        if (n < 0 || (uint64_t)n > 0xFFFFFFFFu) {
            qf_py_set_diag(host, diag, QF_ERR_TYPE, "Python UDF 返回文本过长", "超过 4GiB");
            return QF_ERR_TYPE;
        }
        char* p = (char*)host->alloc(host->ctx, (uint64_t)n + 1, 1);
        if (!p) {
            qf_py_set_diag(host, diag, QF_ERR_EXEC, "Python UDF 返回值写入失败", "宿主分配器返回空");
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
    qf_py_set_diag(host, diag, QF_ERR_TYPE, "Python UDF 返回了契约外的类型", tn);
    return QF_ERR_TYPE;
}
