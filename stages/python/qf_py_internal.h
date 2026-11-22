/*
 * Python 阶段的内部 seam：只在本阶段的多个 .c 之间使用，不进 contracts/qf.h。
 *
 * 拆出来之前，qf_py.c 一个文件承担诊断、生命周期、插件加载、值转换与 UDF 分发；
 * 值转换（Python 对象 ↔ qf_value）只能连同解释器一起测。这里把诊断与值转换
 * 各自做成可独立编译的模块，外部接口仍是 qf_py_load / qf_py_call / qf_py_shutdown。
 */
#ifndef QF_PY_INTERNAL_H
#define QF_PY_INTERNAL_H

#include <Python.h>

#include "qf.h"

/* 诊断：code + "前缀：消息"。host 没有 str 服务时退回静态缓冲。 */
void qf_py_set_diag(const qf_host* host, qf_diag* diag, qf_err code,
                    const char* prefix, const char* msg);

/* 取走当前 Python 异常并转成诊断；调用方负责返回 QF_ERR_UDF。 */
void qf_py_diag_from_python(const qf_host* host, qf_diag* diag, const char* prefix);

/* qf_value → Python；调用方持有 GIL。 */
PyObject* qf_py_value_to_py(const qf_value* v);

/* Python → qf_value；文本拷进宿主 arena，Python 对象随后丢弃。 */
qf_err qf_py_value_from_py(const qf_host* host, PyObject* v, qf_value* out, qf_diag* diag);

#endif /* QF_PY_INTERNAL_H */
