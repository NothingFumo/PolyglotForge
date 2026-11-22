/*
 * 缺阶段占位：构建脚本按 QF_HAVE_<STAGE> 决定是否启用真实实现。
 * 这样每个语言的提交都能独立构建，未实现的能力以明确的错误码暴露，
 * 而不是靠弱符号或运行时探测。
 */
#include "qf.h"

#include <string.h>

/* 九个阶段全就位时这个函数没有调用者（所有占位实现都被 QF_HAVE_* 排除了）。 */
#if defined(__GNUC__)
__attribute__((unused))
#endif
static qf_err unsupported(qf_diag* diag, const char* what, uint32_t len) {
    if (diag) {
        uint32_t n = (uint32_t)strlen(what);
        (void)len;
        diag->code = QF_ERR_UNSUPPORTED;
        diag->message.ptr = what;
        diag->message.len = n;
    }
    return QF_ERR_UNSUPPORTED;
}

#ifndef QF_HAVE_CPP
qf_err qf_sql_parse(const qf_host* host, qf_str sql, qf_ast** out, qf_diag* diag) {
    (void)host; (void)sql; (void)out;
    return unsupported(diag, "缺少 C++ 解析阶段", 22);
}
#endif

#ifndef QF_HAVE_RUST
qf_err qf_plan_build(const qf_host* host, qf_ast* ast, const qf_schema* table,
                     qf_schema* out_schema, qf_diag* diag) {
    (void)host; (void)ast; (void)table; (void)out_schema;
    return unsupported(diag, "缺少 Rust 计划阶段", 20);
}
#endif

#ifndef QF_HAVE_GO
qf_err qf_csv_scan(const qf_host* host, qf_str path, const qf_schema* table,
                   uint32_t shards, qf_batch* out, qf_diag* diag) {
    (void)host; (void)path; (void)table; (void)shards; (void)out;
    return unsupported(diag, "缺少 Go 扫描阶段", 20);
}
#endif

#ifndef QF_HAVE_CSHARP
qf_err qf_cs_compile(const qf_host* host, const qf_ast* ast, int32_t root,
                     const qf_schema* table, qf_pred** out, qf_diag* diag) {
    (void)host; (void)ast; (void)root; (void)table; (void)out;
    return unsupported(diag, "缺少 C# 谓词阶段", 20);
}
qf_err qf_cs_eval(qf_pred* pred, const qf_batch* batch, uint8_t* keep, qf_diag* diag) {
    (void)pred; (void)batch; (void)keep;
    return unsupported(diag, "缺少 C# 谓词阶段", 20);
}
void qf_cs_free(qf_pred* pred) { (void)pred; }
#endif

#ifndef QF_HAVE_KOTLIN
qf_err qf_dsl_build(const qf_host* host, qf_str src, qf_ast** out, qf_diag* diag) {
    (void)host; (void)src; (void)out;
    return unsupported(diag, "缺少 Kotlin DSL 阶段", 22);
}
qf_err qf_kt_batch(const qf_host* host, qf_run_fn run, void* run_ctx,
                   const char* const* sqls, uint32_t n, uint64_t* rows_out, qf_diag* diag) {
    (void)host; (void)run; (void)run_ctx; (void)sqls; (void)n; (void)rows_out;
    return unsupported(diag, "缺少 Kotlin 批处理阶段", 24);
}
#endif

#ifndef QF_HAVE_PYTHON
qf_err qf_py_load(const qf_host* host, qf_str plugin_path, qf_diag* diag) {
    (void)host; (void)plugin_path;
    return unsupported(diag, "缺少 Python 阶段", 18);
}
qf_err qf_py_call(const qf_host* host, qf_str fn, const qf_value* args,
                  uint32_t argc, qf_value* out, qf_diag* diag) {
    (void)host; (void)fn; (void)args; (void)argc; (void)out;
    return unsupported(diag, "缺少 Python 阶段", 18);
}
void qf_py_shutdown(void) {}
#endif

#ifndef QF_HAVE_JAVASCRIPT
qf_err qf_js_init(void) { return QF_ERR_OK; }
qf_err qf_js_format(const qf_host* host, const qf_result* result, qf_str* out, qf_diag* diag) {
    (void)host; (void)result; (void)out;
    return unsupported(diag, "缺少 JavaScript 阶段", 22);
}
qf_err qf_js_where(const qf_host* host, qf_str expr, const qf_batch* batch,
                   uint8_t* keep, qf_diag* diag) {
    (void)host; (void)expr; (void)batch; (void)keep;
    return unsupported(diag, "缺少 JavaScript 阶段", 22);
}
qf_err qf_js_where_named(const qf_host* host, qf_str expr, const qf_schema* cols,
                         const qf_batch* batch, uint8_t* keep, qf_diag* diag) {
    (void)host; (void)expr; (void)cols; (void)batch; (void)keep;
    return unsupported(diag, "缺少 JavaScript 阶段", 22);
}
#endif
