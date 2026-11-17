/* 契约自检：只编译不链接，用来在改动签名时立刻暴露漂移。 */
#include "qf.h"

static qf_err (*const p_sql_parse)(const qf_host*, qf_str, qf_ast**, qf_diag*) = qf_sql_parse;
static qf_err (*const p_plan_build)(const qf_host*, qf_ast*, const qf_schema*, qf_schema*, qf_diag*) = qf_plan_build;
static qf_err (*const p_csv_scan)(const qf_host*, qf_str, const qf_schema*, uint32_t, qf_batch*, qf_diag*) = qf_csv_scan;
static qf_err (*const p_cs_compile)(const qf_host*, const qf_ast*, int32_t, const qf_schema*, qf_pred**, qf_diag*) = qf_cs_compile;
static qf_err (*const p_dsl_build)(const qf_host*, qf_str, qf_ast**, qf_diag*) = qf_dsl_build;
static qf_err (*const p_py_load)(const qf_host*, qf_str, qf_diag*) = qf_py_load;
static qf_err (*const p_js_format)(const qf_host*, const qf_result*, qf_str*, qf_diag*) = qf_js_format;

int main(void) {
    const void* const all[] = {
        (const void*)p_sql_parse, (const void*)p_plan_build, (const void*)p_csv_scan,
        (const void*)p_cs_compile, (const void*)p_dsl_build, (const void*)p_py_load,
        (const void*)p_js_format,
    };
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        if (all[i]) ++n;
    }
    return n == 7 ? 0 : 1;
}
