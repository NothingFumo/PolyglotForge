/* 由 schema/qf.ts + codegen/gen.ts 生成，勿手改 */

#ifndef QF_LAYOUT_H
#define QF_LAYOUT_H
#include <stdint.h>

#define QF_ABI_VERSION 1

typedef int32_t qf_err;
enum {
    QF_OK = 0,
    QF_ARG = 1,
    QF_IO = 2,
    QF_PARSE = 3,
    QF_TYPE = 4,
    QF_PLAN = 5,
    QF_EXEC = 6,
    QF_UDF = 7,
    QF_PANIC = 8,
    QF_UNSUPPORTED = 9
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

typedef struct qf_str { const char* ptr; uint32_t len; } qf_str;

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

#if defined(__cplusplus)
#define QF_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define QF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define QF_STATIC_ASSERT(cond, msg)
#endif

QF_STATIC_ASSERT(sizeof(qf_str) == 16, "qf_str 布局漂移");
QF_STATIC_ASSERT(sizeof(qf_value) == 24, "qf_value 布局漂移");
QF_STATIC_ASSERT(sizeof(qf_node) == 56, "qf_node 布局漂移");

#endif /* QF_LAYOUT_H */
