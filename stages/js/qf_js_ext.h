/*
 * stages/js 内部扩展接口，不属于跨语言契约。
 *
 * 契约里的 qf_batch 只有行与列数，没有列名。qf_js_where 因此用 c0..cN 作为
 * 列名；需要按真实列名求值（例如 row.age > 30）时用 qf_js_where_named。
 */
#ifndef QF_JS_EXT_H
#define QF_JS_EXT_H

#include "qf.h"

#ifdef __cplusplus
extern "C" {
#endif

qf_err qf_js_where_named(const qf_host* host, qf_str expr, const qf_schema* cols,
                         const qf_batch* batch, uint8_t* keep, qf_diag* diag);

#ifdef __cplusplus
}
#endif

#endif /* QF_JS_EXT_H */
