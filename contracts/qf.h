/*
 * 跨语言契约。九种语言唯一共享的东西就是这个头文件里的类型与函数签名。
 * 规则：
 *   1. 只使用 C ABI；C++ 侧必须在 extern "C" 内实现。
 *   2. 跨边界不传异常、不传 C++ 对象、不传带 GC 的引用。
 *   3. 所有分配都通过 qf_host 下发，避免每个语言各持一份内存来源。
 *   4. 文本一律 (ptr, len)，不假设以 NUL 结尾。
 */
#ifndef QF_H
#define QF_H

#include "qf_layout.h" /* gen/ 需在 include path 中 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 宿主服务 ---------- */

typedef struct qf_arena qf_arena;

typedef struct qf_host {
    void*  (*alloc)(void* ctx, uint64_t size, uint64_t align);
    qf_str (*str)(void* ctx, const char* src, uint32_t len);
    void*  ctx;
} qf_host;

/* ---------- 诊断 ---------- */

typedef struct qf_diag {
    qf_err   code;
    uint32_t line;
    uint32_t col;
    qf_str   message;
    qf_str   hint;
} qf_diag;

/* ---------- 表与结果 ---------- */

typedef struct qf_col {
    qf_str  name;
    qf_type type;
} qf_col;

typedef struct qf_schema {
    uint32_t count;
    qf_col*  cols;
} qf_schema;

/* 计划：经过语义分析与列解析之后的 AST。Rust 在节点 flags 里回填类型。 */
typedef struct qf_ast {
    qf_node* nodes;
    uint32_t node_count;

    int32_t* select;
    uint32_t select_count;

    qf_str   from_table;
    int32_t  where_root; /* -1 表示无 */

    int32_t* group_by;
    uint32_t group_count;

    int32_t* order_by;
    uint32_t order_count;

    int64_t  limit; /* -1 表示无 */
} qf_ast;

/* 批次：一次跨语言传递的行集合，行主序。 */
typedef struct qf_batch {
    uint32_t  rows;
    uint32_t  cols;
    qf_value* values;
} qf_batch;

typedef struct qf_result {
    qf_schema schema;
    uint64_t  row_count;
    qf_value* values; /* row_count * schema.count，行主序 */
} qf_result;

/* ---------- 阶段：C++ 词法与语法 ---------- */

qf_err qf_sql_parse(const qf_host* host, qf_str sql, qf_ast** out, qf_diag* diag);

/* ---------- 阶段：Rust 语义分析与计划 ---------- */

qf_err qf_plan_build(const qf_host* host, qf_ast* ast, const qf_schema* table,
                     qf_schema* out_schema, qf_diag* diag);

/* ---------- 阶段：Go CSV 并行扫描 ---------- */

qf_err qf_csv_scan(const qf_host* host, qf_str path, const qf_schema* table,
                   uint32_t shards, qf_batch* out, qf_diag* diag);

/* ---------- 阶段：C# 谓词编译求值 ---------- */

typedef struct qf_pred qf_pred;

qf_err qf_cs_compile(const qf_host* host, const qf_ast* ast, int32_t root,
                     const qf_schema* table, qf_pred** out, qf_diag* diag);
qf_err qf_cs_eval(qf_pred* pred, const qf_batch* batch, uint8_t* keep, qf_diag* diag);
void   qf_cs_free(qf_pred* pred);

/* ---------- 阶段：Kotlin DSL 前端与协程批处理 ---------- */

qf_err qf_dsl_build(const qf_host* host, qf_str src, qf_ast** out, qf_diag* diag);

typedef qf_err (*qf_run_fn)(void* ctx, const char* sql, uint64_t* rows_out, qf_diag* diag);

/* 用协程并发执行多条查询；run 是宿主提供的单查询执行器。 */
qf_err qf_kt_batch(const qf_host* host, qf_run_fn run, void* run_ctx,
                   const char* const* sqls, uint32_t n, uint64_t* rows_out, qf_diag* diag);

/* ---------- 阶段：Python UDF ---------- */

qf_err qf_py_load(const qf_host* host, qf_str plugin_path, qf_diag* diag);
qf_err qf_py_call(const qf_host* host, qf_str fn, const qf_value* args,
                  uint32_t argc, qf_value* out, qf_diag* diag);
void   qf_py_shutdown(void);

/* ---------- 阶段：JavaScript 输出与 where 沙箱 ---------- */

qf_err qf_js_init(void);
qf_err qf_js_format(const qf_host* host, const qf_result* result, qf_str* out, qf_diag* diag);
qf_err qf_js_where(const qf_host* host, qf_str expr, const qf_batch* batch,
                   uint8_t* keep, qf_diag* diag);

/* ---------- 宿主 ---------- */

uint32_t qf_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif /* QF_H */
