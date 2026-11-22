/* 由 schema/qf.ts + codegen/gen.ts 生成，勿手改 */

#ifndef QF_LAYOUT_H
#define QF_LAYOUT_H
#include <stdint.h>
#include <stddef.h>

#define QF_ABI_VERSION 3

typedef int32_t qf_err;
enum {
    QF_ERR_OK = 0,
    QF_ERR_ARG = 1,
    QF_ERR_IO = 2,
    QF_ERR_PARSE = 3,
    QF_ERR_TYPE = 4,
    QF_ERR_PLAN = 5,
    QF_ERR_EXEC = 6,
    QF_ERR_UDF = 7,
    QF_ERR_PANIC = 8,
    QF_ERR_UNSUPPORTED = 9
};

typedef int32_t qf_type;
enum { QF_T_NULL = 0 };
enum { QF_T_INT = 1 };
enum { QF_T_DOUBLE = 2 };
enum { QF_T_TEXT = 3 };

typedef enum {
    QF_N_LIT = 1,
    QF_N_COL = 2,
    QF_N_BIN = 3,
    QF_N_UNARY = 4,
    QF_N_CALL = 5,
    QF_N_AGG = 6,
    QF_N_STAR = 7
} qf_node_kind;

typedef enum {
    QF_OP_NONE = 0,
    QF_OP_EQ = 1,
    QF_OP_NE = 2,
    QF_OP_LT = 3,
    QF_OP_LE = 4,
    QF_OP_GT = 5,
    QF_OP_GE = 6,
    QF_OP_AND = 7,
    QF_OP_OR = 8,
    QF_OP_NOT = 9,
    QF_OP_ADD = 10,
    QF_OP_SUB = 11,
    QF_OP_MUL = 12,
    QF_OP_DIV = 13,
    QF_OP_MOD = 14,
    QF_OP_NEG = 15
} qf_op;

typedef enum {
    QF_AGG_COUNT = 1,
    QF_AGG_SUM = 2,
    QF_AGG_AVG = 3,
    QF_AGG_MIN = 4,
    QF_AGG_MAX = 5
} qf_agg;

enum {
    QF_F_TYPE_SHIFT = 0,
    QF_F_TYPE_MASK = 15,
    QF_F_DESC = 16,
    QF_F_RESOLVED = 32
};

typedef struct qf_str {
    const char* ptr;
    uint32_t len;
} qf_str;

typedef struct qf_value {
    qf_type type;
    union { int64_t i; double d; qf_str s; } as;
} qf_value;

typedef struct qf_node {
    uint8_t kind;
    uint8_t op;
    uint16_t flags;
    int32_t a;
    int32_t b;
    int32_t argc;
    qf_value lit;
    qf_str name;
} qf_node;

typedef struct qf_col {
    qf_str name;
    qf_type type;
} qf_col;

typedef struct qf_schema {
    uint32_t count;
    qf_col* cols;
} qf_schema;

typedef struct qf_batch {
    uint32_t rows;
    uint32_t cols;
    qf_value* values;
} qf_batch;

typedef struct qf_result {
    qf_schema schema;
    uint64_t row_count;
    qf_value* values;
} qf_result;

typedef struct qf_diag {
    qf_err code;
    uint32_t line;
    uint32_t col;
    qf_str message;
    qf_str hint;
} qf_diag;

typedef struct qf_ast {
    qf_node* nodes;
    uint32_t node_count;
    int32_t* select;
    uint32_t select_count;
    qf_str* select_names;
    qf_str from_table;
    int32_t where_root;
    int32_t* group_by;
    uint32_t group_count;
    int32_t* order_by;
    uint32_t order_count;
    int64_t limit;
} qf_ast;

#if defined(__cplusplus)
#define QF_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define QF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define QF_STATIC_ASSERT(cond, msg)
#endif

QF_STATIC_ASSERT(sizeof(qf_str) == 16, "qf_str 尺寸漂移");
QF_STATIC_ASSERT(offsetof(qf_str, ptr) == 0, "qf_str.ptr 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_str, len) == 8, "qf_str.len 偏移漂移");
QF_STATIC_ASSERT(sizeof(qf_value) == 24, "qf_value 尺寸漂移");
QF_STATIC_ASSERT(offsetof(qf_value, type) == 0, "qf_value.type 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_value, as) == 8, "qf_value.as 偏移漂移");
QF_STATIC_ASSERT(sizeof(qf_node) == 56, "qf_node 尺寸漂移");
QF_STATIC_ASSERT(offsetof(qf_node, kind) == 0, "qf_node.kind 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_node, op) == 1, "qf_node.op 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_node, flags) == 2, "qf_node.flags 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_node, a) == 4, "qf_node.a 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_node, b) == 8, "qf_node.b 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_node, argc) == 12, "qf_node.argc 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_node, lit) == 16, "qf_node.lit 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_node, name) == 40, "qf_node.name 偏移漂移");
QF_STATIC_ASSERT(sizeof(qf_col) == 24, "qf_col 尺寸漂移");
QF_STATIC_ASSERT(offsetof(qf_col, name) == 0, "qf_col.name 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_col, type) == 16, "qf_col.type 偏移漂移");
QF_STATIC_ASSERT(sizeof(qf_schema) == 16, "qf_schema 尺寸漂移");
QF_STATIC_ASSERT(offsetof(qf_schema, count) == 0, "qf_schema.count 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_schema, cols) == 8, "qf_schema.cols 偏移漂移");
QF_STATIC_ASSERT(sizeof(qf_batch) == 16, "qf_batch 尺寸漂移");
QF_STATIC_ASSERT(offsetof(qf_batch, rows) == 0, "qf_batch.rows 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_batch, cols) == 4, "qf_batch.cols 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_batch, values) == 8, "qf_batch.values 偏移漂移");
QF_STATIC_ASSERT(sizeof(qf_result) == 32, "qf_result 尺寸漂移");
QF_STATIC_ASSERT(offsetof(qf_result, schema) == 0, "qf_result.schema 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_result, row_count) == 16, "qf_result.row_count 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_result, values) == 24, "qf_result.values 偏移漂移");
QF_STATIC_ASSERT(sizeof(qf_diag) == 48, "qf_diag 尺寸漂移");
QF_STATIC_ASSERT(offsetof(qf_diag, code) == 0, "qf_diag.code 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_diag, line) == 4, "qf_diag.line 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_diag, col) == 8, "qf_diag.col 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_diag, message) == 16, "qf_diag.message 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_diag, hint) == 32, "qf_diag.hint 偏移漂移");
QF_STATIC_ASSERT(sizeof(qf_ast) == 104, "qf_ast 尺寸漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, nodes) == 0, "qf_ast.nodes 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, node_count) == 8, "qf_ast.node_count 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, select) == 16, "qf_ast.select 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, select_count) == 24, "qf_ast.select_count 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, select_names) == 32, "qf_ast.select_names 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, from_table) == 40, "qf_ast.from_table 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, where_root) == 56, "qf_ast.where_root 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, group_by) == 64, "qf_ast.group_by 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, group_count) == 72, "qf_ast.group_count 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, order_by) == 80, "qf_ast.order_by 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, order_count) == 88, "qf_ast.order_count 偏移漂移");
QF_STATIC_ASSERT(offsetof(qf_ast, limit) == 96, "qf_ast.limit 偏移漂移");

#endif /* QF_LAYOUT_H */
