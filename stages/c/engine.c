/*
 * 执行引擎：过滤、投影、聚合、排序、截断。
 * 语义约定（与 CONTEXT.md 一致）：
 *   - 空值不是零：任何算术/比较遇到空值都产出空值；
 *   - 三值逻辑：AND/OR 对 unknown 按 SQL 规则传播；
 *   - 数值语义与 stages/csharp/native/Val.cs 保持一致：int64 比较不降精度，
 *     整数除法保持整数，SUM 对整数输入保持整数；两处改动必须同步。
 *   - 排序对空值一律排在最后（升序与降序都是），避免宿主间实现漂移。
 */
#include "qf.h"
#include "engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct qf_engine_ctx {
    const qf_host* host;
    qf_arena* arena;
    const qf_ast* ast;
    const qf_schema* table;
    qf_err err; /* 求值期间的粘性错误；非 OK 时中止整条查询 */
} qf_engine_ctx;

static qf_value v_null(void) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_NULL;
    return v;
}

static qf_value v_int(int64_t i) {
    qf_value v = v_null();
    v.type = QF_T_INT;
    v.as.i = i;
    return v;
}

static qf_value v_dbl(double d) {
    qf_value v = v_null();
    v.type = QF_T_DOUBLE;
    v.as.d = d;
    return v;
}

static int str_cmp(qf_str a, qf_str b) {
    uint32_t n = a.len < b.len ? a.len : b.len;
    int c = n ? memcmp(a.ptr, b.ptr, n) : 0;
    if (c != 0) return c;
    if (a.len == b.len) return 0;
    return a.len < b.len ? -1 : 1;
}

/* 数值化：整数与浮点互相提升，文本返回 0（调用方先做类型检查）。 */
static double as_num(qf_value v) {
    return v.type == QF_T_INT ? (double)v.as.i : (v.type == QF_T_DOUBLE ? v.as.d : 0.0);
}

static int is_num(qf_value v) {
    return v.type == QF_T_INT || v.type == QF_T_DOUBLE;
}

/* 三值真值，与 stages/csharp/native/Val.cs 的 Truth 保持一致：
   1 真、0 假、-1 未知（空值、文本、其它类型）。 */
static int truth(qf_value v) {
    if (v.type == QF_T_INT) return v.as.i != 0 ? 1 : 0;
    if (v.type == QF_T_DOUBLE) return v.as.d != 0.0 ? 1 : 0;
    return -1;
}

/* 失败路径统一写诊断：任何非零返回都必须带错误码与可读消息。
   host 不可用时退回静态字面量，生命周期与进程一致。 */
static qf_err engine_fail(const qf_engine_ctx* c, qf_diag* diag, qf_err code,
                          const char* msg) {
    if (diag) {
        memset(diag, 0, sizeof *diag);
        diag->code = code;
        diag->message.ptr = msg;
        diag->message.len = (uint32_t)strlen(msg);
        if (c && c->host && c->host->str) {
            qf_str copied = c->host->str(c->host->ctx, msg, diag->message.len);
            if (copied.ptr && copied.len) diag->message = copied;
        }
    }
    return code;
}

static qf_err uf_call(qf_engine_ctx* c, const qf_node* n, const qf_value* row,
                      qf_value* out, qf_diag* diag);

/* 求值：row 为输入列的值数组（可能为 NULL，仅当表达式不引用列时）。 */
static qf_value eval(qf_engine_ctx* c, int32_t idx, const qf_value* row, qf_diag* diag) {
    if (c->err != QF_ERR_OK) return v_null();
    const qf_node* n = &c->ast->nodes[idx];
    switch (n->kind) {
    case QF_N_LIT:
        return n->lit;
    case QF_N_COL:
        if (!row || (uint32_t)n->a >= c->table->count) return v_null();
        return row[n->a];
    case QF_N_BIN: {
        qf_value l = eval(c, n->a, row, diag);
        qf_value r = eval(c, n->b, row, diag);
        /* AND/OR 先按 SQL 三值规则处理：未知与假/真组合仍能定值。 */
        if (n->op == QF_OP_AND || n->op == QF_OP_OR) {
            int lt = truth(l), rt = truth(r);
            if (n->op == QF_OP_AND) {
                if (lt == 0 || rt == 0) return v_int(0);
                if (lt < 0 || rt < 0) return v_null();
                return v_int(1);
            }
            if (lt == 1 || rt == 1) return v_int(1);
            if (lt < 0 || rt < 0) return v_null();
            return v_int(0);
        }
        if (l.type == QF_T_NULL || r.type == QF_T_NULL) return v_null();
        switch (n->op) {
        case QF_OP_ADD:
        case QF_OP_SUB:
        case QF_OP_MUL:
        case QF_OP_DIV:
        case QF_OP_MOD: {
            if (!is_num(l) || !is_num(r)) return v_null();
            int both_int = (l.type == QF_T_INT && r.type == QF_T_INT);
            if (both_int) {
                int64_t a = l.as.i, b = r.as.i;
                /* 与 C# unchecked 算术一致：溢出按 uint64 回绕，避免 C 的有符号 UB。 */
                switch (n->op) {
                case QF_OP_ADD: return v_int((int64_t)((uint64_t)a + (uint64_t)b));
                case QF_OP_SUB: return v_int((int64_t)((uint64_t)a - (uint64_t)b));
                case QF_OP_MUL: return v_int((int64_t)((uint64_t)a * (uint64_t)b));
                case QF_OP_DIV:
                    if (b == 0) return v_null();
                    if (a == INT64_MIN && b == -1) return v_int(INT64_MIN);
                    return v_int(a / b); /* 整数除法保持整数，与 sqlite 一致 */
                default:
                    if (b == 0) return v_null();
                    if (a == INT64_MIN && b == -1) return v_int(0);
                    return v_int(a % b);
                }
            }
            double a = as_num(l), b = as_num(r);
            if ((n->op == QF_OP_DIV || n->op == QF_OP_MOD) && b == 0.0) return v_null();
            switch (n->op) {
            case QF_OP_ADD: return v_dbl(a + b);
            case QF_OP_SUB: return v_dbl(a - b);
            case QF_OP_MUL: return v_dbl(a * b);
            case QF_OP_DIV: return v_dbl(a / b);
            default: return v_dbl(fmod(a, b));
            }
        }
        default: {
            int cmp = 0;
            if (l.type == QF_T_INT && r.type == QF_T_INT) {
                int64_t a = l.as.i, b = r.as.i;
                cmp = (a < b) ? -1 : (a > b ? 1 : 0);
            } else if (is_num(l) && is_num(r)) {
                double a = as_num(l), b = as_num(r);
                cmp = (a < b) ? -1 : (a > b ? 1 : 0);
            } else if (l.type == QF_T_TEXT && r.type == QF_T_TEXT) {
                cmp = str_cmp(l.as.s, r.as.s);
            } else {
                return v_null();
            }
            int res = 0;
            switch (n->op) {
            case QF_OP_EQ: res = cmp == 0; break;
            case QF_OP_NE: res = cmp != 0; break;
            case QF_OP_LT: res = cmp < 0; break;
            case QF_OP_LE: res = cmp <= 0; break;
            case QF_OP_GT: res = cmp > 0; break;
            case QF_OP_GE: res = cmp >= 0; break;
            default: return v_null();
            }
            return v_int(res);
        }
        }
    }
    case QF_N_UNARY: {
        qf_value v = eval(c, n->a, row, diag);
        if (n->op == QF_OP_NOT) {
            int t = truth(v);
            return t < 0 ? v_null() : v_int(t == 0);
        }
        if (v.type == QF_T_NULL) return v_null();
        if (n->op == QF_OP_NEG) {
            if (v.type == QF_T_INT) return v_int(-v.as.i);
            if (v.type == QF_T_DOUBLE) return v_dbl(-v.as.d);
        }
        return v_null();
    }
    case QF_N_CALL: {
        qf_value out = v_null();
        qf_err rc = uf_call(c, n, row, &out, diag);
        if (rc != QF_ERR_OK) {
            c->err = rc;
            if (diag && diag->code == QF_ERR_OK) {
                engine_fail(c, diag, rc, "UDF 执行失败");
            }
            return v_null();
        }
        return out;
    }
    default:
        return v_null();
    }
}

/* UDF：参数先求值，再交给 Python 阶段。失败向上传播，不再静默产出空值。 */
static qf_err uf_call(qf_engine_ctx* c, const qf_node* n, const qf_value* row,
                      qf_value* out, qf_diag* diag) {
    if (n->argc > 16) {
        return engine_fail(c, diag, QF_ERR_UDF, "UDF 参数过多");
    }
    qf_value args[16];
    for (int32_t i = 0; i < n->argc; ++i) {
        args[i] = eval(c, n->b + i, row, diag);
        if (c->err != QF_ERR_OK) return c->err;
    }
#ifdef QF_HAVE_PYTHON
    return qf_py_call(c->host, n->name, args, (uint32_t)n->argc, out, diag);
#else
    (void)args;
    (void)out;
    return engine_fail(c, diag, QF_ERR_UDF, "构建未包含 Python 阶段");
#endif
}

/* ---------- 聚合 ---------- */

typedef struct agg_state {
    int64_t count;
    double sum;      /* AVG 始终用 double；SUM 遇到浮点或溢出后也用它 */
    int64_t isum;    /* SUM 的全整数累加值 */
    int has_value;
    int sum_is_int;  /* 1 = isum 有效，0 = sum 有效 */
    int sum_overflow;
    qf_value best;
} agg_state;

static void agg_apply(agg_state* s, uint8_t op, qf_value v) {
    switch (op) {
    case QF_AGG_COUNT:
        /* argc==0（count(*)）时引擎传入非空哨兵，因此只跳过真正的空值。 */
        if (v.type != QF_T_NULL) s->count++;
        break;
    case QF_AGG_SUM:
    case QF_AGG_AVG:
        if (v.type == QF_T_NULL || !is_num(v)) return;
        s->count++;
        s->has_value = 1;
        if (op == QF_AGG_AVG) {
            s->sum += as_num(v);
            return;
        }
        /* SUM：全整数输入保持 int64；出现浮点或溢出后退回 double。 */
        if (s->sum_is_int && v.type == QF_T_INT && !s->sum_overflow) {
            int64_t next;
            if (!__builtin_add_overflow(s->isum, v.as.i, &next)) {
                s->isum = next;
                return;
            }
            s->sum_overflow = 1;
            s->sum = (double)s->isum + (double)v.as.i;
            s->sum_is_int = 0;
            return;
        }
        if (s->sum_is_int) {
            s->sum = (double)s->isum;
            s->sum_is_int = 0;
        }
        s->sum += as_num(v);
        break;
    case QF_AGG_MIN:
    case QF_AGG_MAX:
        if (v.type == QF_T_NULL) return;
        if (!s->has_value) {
            s->best = v;
            s->has_value = 1;
            return;
        }
        {
            int cmp;
            if (v.type == QF_T_INT && s->best.type == QF_T_INT) {
                int64_t a = v.as.i, b = s->best.as.i;
                cmp = (a < b) ? -1 : (a > b ? 1 : 0);
            } else if (is_num(v) && is_num(s->best)) {
                double a = as_num(v), b = as_num(s->best);
                cmp = (a < b) ? -1 : (a > b ? 1 : 0);
            } else if (v.type == QF_T_TEXT && s->best.type == QF_T_TEXT) {
                cmp = str_cmp(v.as.s, s->best.as.s);
            } else {
                return;
            }
            if ((op == QF_AGG_MIN && cmp < 0) || (op == QF_AGG_MAX && cmp > 0)) s->best = v;
        }
        break;
    default:
        break;
    }
}

static qf_value agg_finish(const agg_state* s, uint8_t op) {
    switch (op) {
    case QF_AGG_COUNT: return v_int(s->count);
    case QF_AGG_SUM:
        if (!s->has_value) return v_null();
        return s->sum_is_int ? v_int(s->isum) : v_dbl(s->sum);
    case QF_AGG_AVG: return s->count > 0 ? v_dbl(s->sum / (double)s->count) : v_null();
    case QF_AGG_MIN:
    case QF_AGG_MAX: return s->has_value ? s->best : v_null();
    default: return v_null();
    }
}

/* ---------- 辅助 ---------- */

static int select_has_agg(const qf_ast* ast) {
    for (uint32_t i = 0; i < ast->select_count; ++i) {
        if (ast->nodes[ast->select[i]].kind == QF_N_AGG) return 1;
    }
    return 0;
}

static qf_err row_keep(qf_engine_ctx* c, const qf_batch* in, const qf_ast* ast,
                       qf_filter_fn filter, void* fctx, uint8_t** out_keep, qf_diag* diag) {
    *out_keep = (uint8_t*)qf_arena_alloc(c->arena, in->rows ? in->rows : 1, 1);
    if (!*out_keep) return engine_fail(c, diag, QF_ERR_EXEC, "宿主分配过滤掩码失败");
    if (filter) {
        qf_err rc = filter(fctx, in, *out_keep, diag);
        if (rc != QF_ERR_OK && (!diag || diag->code == QF_ERR_OK)) {
            return engine_fail(c, diag, rc, "过滤阶段失败");
        }
        return rc;
    }
    if (ast->where_root < 0) {
        memset(*out_keep, 1, in->rows ? in->rows : 1);
        return QF_ERR_OK;
    }
    for (uint32_t r = 0; r < in->rows; ++r) {
        qf_value v = eval(c, ast->where_root, &in->values[r * in->cols], diag);
        if (c->err != QF_ERR_OK) return c->err;
        (*out_keep)[r] = (uint8_t)(truth(v) == 1);
    }
    return QF_ERR_OK;
}

typedef struct sort_ctx {
    qf_value* values;
    uint32_t cols;
    const int32_t* keys;
    const uint8_t* desc;
    uint32_t nkeys;
} sort_ctx;

static int cmp_idx(const sort_ctx* s, uint32_t a, uint32_t b) {
    for (uint32_t k = 0; k < s->nkeys; ++k) {
        int32_t col = s->keys[k];
        if (col < 0) continue;
        qf_value va = s->values[(size_t)a * s->cols + (uint32_t)col];
        qf_value vb = s->values[(size_t)b * s->cols + (uint32_t)col];
        if (va.type == QF_T_NULL && vb.type == QF_T_NULL) continue;
        if (va.type == QF_T_NULL) return 1; /* 空值恒排在最后，与升降序无关 */
        if (vb.type == QF_T_NULL) return -1;
        int cmp;
        if (va.type == QF_T_INT && vb.type == QF_T_INT) {
            int64_t x = va.as.i, y = vb.as.i;
            cmp = (x < y) ? -1 : (x > y ? 1 : 0);
        } else if (is_num(va) && is_num(vb)) {
            double x = as_num(va), y = as_num(vb);
            cmp = (x < y) ? -1 : (x > y ? 1 : 0);
        } else if (va.type == QF_T_TEXT && vb.type == QF_T_TEXT) {
            cmp = str_cmp(va.as.s, vb.as.s);
        } else {
            cmp = 0;
        }
        if (cmp != 0) return s->desc[k] ? -cmp : cmp;
    }
    return (a < b) ? -1 : (a > b ? 1 : 0); /* 稳定 */
}

/* 归并排序：上下文显式传入，不依赖 qsort 的全局状态；--batch 并发调用时安全。
   按输入下标决胜，因此与 qsort + 下标比较的旧实现排序结果一致。 */
static void sort_idx(const sort_ctx* s, uint32_t* idx, uint32_t* tmp, uint32_t n) {
    for (size_t width = 1; width < (size_t)n; width *= 2) {
        for (size_t base = 0; base < (size_t)n; base += width * 2) {
            size_t mid = base + width;
            size_t end = base + width * 2;
            if (mid > (size_t)n) mid = n;
            if (end > (size_t)n) end = n;
            size_t l = base, r = mid, k = base;
            while (l < mid && r < end) {
                tmp[k++] = cmp_idx(s, idx[l], idx[r]) <= 0 ? idx[l++] : idx[r++];
            }
            while (l < mid) tmp[k++] = idx[l++];
            while (r < end) tmp[k++] = idx[r++];
        }
        memcpy(idx, tmp, (size_t)n * sizeof *idx);
    }
}

qf_err qf_engine_execute(const qf_host* host, qf_arena* arena, const qf_ast* ast,
                         const qf_schema* table, const qf_schema* out_schema,
                         const qf_batch* in, qf_filter_fn filter, void* fctx,
                         qf_result* out, qf_diag* diag) {
    qf_engine_ctx c;
    memset(&c, 0, sizeof c);
    c.host = host;
    c.arena = arena;
    c.ast = ast;
    c.table = table;

    if (!in) return engine_fail(&c, diag, QF_ERR_ARG, "qf_engine_execute 收到空批次");
    if (in->cols != table->count) {
        return engine_fail(&c, diag, QF_ERR_ARG, "批次列数与表定义不一致");
    }

    uint8_t* keep = NULL;
    qf_err rc = row_keep(&c, in, ast, filter, fctx, &keep, diag);
    if (rc != QF_ERR_OK) return rc;

    uint32_t rows = 0;
    for (uint32_t r = 0; r < in->rows; ++r) rows += keep[r] ? 1u : 0u;

    out->schema = *out_schema;
    out->row_count = 0;
    out->values = (qf_value*)qf_arena_alloc(
        arena, (uint64_t)(rows ? rows : 1) * out_schema->count * sizeof(qf_value), 8);
    if (!out->values) return engine_fail(&c, diag, QF_ERR_EXEC, "宿主分配结果集失败");

    int has_agg = select_has_agg(ast);

    if (has_agg || ast->group_count > 0) {
        /* 分组：线性扫描；组数按需翻倍扩容，不设固定上限。 */
        uint32_t cap = 64, ngroups = 0;
        uint32_t* group_first = (uint32_t*)qf_arena_alloc(
            arena, (uint64_t)cap * sizeof(uint32_t), 4);
        if (!group_first) return engine_fail(&c, diag, QF_ERR_EXEC, "宿主分配分组索引失败");

        for (uint32_t r = 0; r < in->rows; ++r) {
            if (!keep[r]) continue;
            const qf_value* row = &in->values[(size_t)r * in->cols];
            uint32_t g = 0;
            for (; g < ngroups; ++g) {
                uint32_t fr = group_first[g];
                const qf_value* frow = &in->values[(size_t)fr * in->cols];
                uint32_t k = 0;
                for (; k < ast->group_count; ++k) {
                    qf_value a = eval(&c, ast->group_by[k], row, diag);
                    qf_value b = eval(&c, ast->group_by[k], frow, diag);
                    if (c.err != QF_ERR_OK) return c.err;
                    if (a.type != b.type) break;
                    if (a.type == QF_T_TEXT) {
                        if (str_cmp(a.as.s, b.as.s) != 0) break;
                    } else if (a.type == QF_T_INT) {
                        if (a.as.i != b.as.i) break;
                    } else if (a.type == QF_T_DOUBLE) {
                        if (a.as.d != b.as.d) break;
                    }
                }
                if (k == ast->group_count) break;
            }
            if (g == ngroups) {
                if (ngroups == cap) {
                    uint64_t newcap = (uint64_t)cap * 2;
                    if (newcap > UINT32_MAX) {
                        return engine_fail(&c, diag, QF_ERR_EXEC, "分组数超出 uint32 范围");
                    }
                    uint32_t* grown = (uint32_t*)qf_arena_alloc(
                        arena, newcap * sizeof(uint32_t), 4);
                    if (!grown) return engine_fail(&c, diag, QF_ERR_EXEC, "宿主分配分组索引失败");
                    memcpy(grown, group_first, (size_t)ngroups * sizeof(uint32_t));
                    group_first = grown;
                    cap = (uint32_t)newcap;
                }
                group_first[ngroups++] = r;
            }
        }

        for (uint32_t g = 0; g < ngroups; ++g) {
            uint32_t first_row = group_first[g];
            const qf_value* first = &in->values[(size_t)first_row * in->cols];
            for (uint32_t si = 0; si < ast->select_count; ++si) {
                int32_t ni = ast->select[si];
                const qf_node* n = &ast->nodes[ni];
                qf_value v;
                if (n->kind == QF_N_AGG) {
                    agg_state st;
                    memset(&st, 0, sizeof st);
                    st.sum_is_int = 1;
                    for (uint32_t r = first_row; r < in->rows; ++r) {
                        if (!keep[r]) continue;
                        const qf_value* row = &in->values[(size_t)r * in->cols];
                        uint32_t gk = 0;
                        for (; gk < ast->group_count; ++gk) {
                            qf_value a = eval(&c, ast->group_by[gk], row, diag);
                            qf_value b = eval(&c, ast->group_by[gk], first, diag);
                            if (c.err != QF_ERR_OK) return c.err;
                            int same = (a.type == b.type) &&
                                       ((a.type == QF_T_TEXT && str_cmp(a.as.s, b.as.s) == 0) ||
                                        (a.type == QF_T_INT && a.as.i == b.as.i) ||
                                        (a.type == QF_T_DOUBLE && a.as.d == b.as.d) ||
                                        (a.type == QF_T_NULL));
                            if (!same) break;
                        }
                        if (gk != ast->group_count) continue;
                        if (n->argc > 0) {
                            qf_value arg = eval(&c, n->b, row, diag);
                            if (c.err != QF_ERR_OK) return c.err;
                            agg_apply(&st, n->op, arg);
                        } else {
                            agg_apply(&st, n->op, v_int(1));
                        }
                    }
                    v = agg_finish(&st, n->op);
                } else {
                    v = eval(&c, ni, first, diag);
                    if (c.err != QF_ERR_OK) return c.err;
                }
                out->values[(size_t)out->row_count * out_schema->count + si] = v;
            }
            out->row_count++;
        }

        /* 全局聚合没有输入行时仍应产出一行：count=0，sum/avg/min/max=NULL。
           GROUP BY 无行时保持 0 行（SQL 语义）。 */
        if (ngroups == 0 && ast->group_count == 0 && has_agg) {
            for (uint32_t si = 0; si < ast->select_count; ++si) {
                int32_t ni = ast->select[si];
                const qf_node* n = &ast->nodes[ni];
                qf_value v = v_null();
                if (n->kind == QF_N_AGG) {
                    agg_state st;
                    memset(&st, 0, sizeof st);
                    st.sum_is_int = 1;
                    v = agg_finish(&st, n->op);
                }
                out->values[si] = v;
            }
            out->row_count = 1;
        }
    } else {
        for (uint32_t r = 0; r < in->rows; ++r) {
            if (!keep[r]) continue;
            const qf_value* row = &in->values[(size_t)r * in->cols];
            for (uint32_t si = 0; si < ast->select_count; ++si) {
                qf_value v = eval(&c, ast->select[si], row, diag);
                if (c.err != QF_ERR_OK) return c.err;
                out->values[(size_t)out->row_count * out_schema->count + si] = v;
            }
            out->row_count++;
        }
    }

    /* 排序：order_by 已被 Rust 解析为输出列索引（node->a）。 */
    if (ast->order_count > 0 && out->row_count > 1) {
        uint32_t n = (uint32_t)out->row_count;
        uint32_t* idx = (uint32_t*)qf_arena_alloc(arena, (uint64_t)n * sizeof(uint32_t), 4);
        uint32_t* swap = (uint32_t*)qf_arena_alloc(arena, (uint64_t)n * sizeof(uint32_t), 4);
        qf_value* tmp = (qf_value*)qf_arena_alloc(
            arena, (uint64_t)n * out_schema->count * sizeof(qf_value), 8);
        if (!idx || !swap || !tmp) {
            return engine_fail(&c, diag, QF_ERR_EXEC, "宿主分配排序缓冲失败");
        }
        int32_t keys[16];
        uint8_t desc[16];
        uint32_t nk = ast->order_count > 16 ? 16 : ast->order_count;
        for (uint32_t i = 0; i < nk; ++i) {
            int32_t col = ast->nodes[ast->order_by[i]].a;
            keys[i] = (col >= 0 && (uint32_t)col < out_schema->count) ? col : -1;
            desc[i] = (ast->nodes[ast->order_by[i]].flags & QF_F_DESC) ? 1u : 0u;
        }
        for (uint32_t i = 0; i < n; ++i) idx[i] = i;
        sort_ctx sc;
        sc.values = out->values;
        sc.cols = out_schema->count;
        sc.keys = keys;
        sc.desc = desc;
        sc.nkeys = nk;
        sort_idx(&sc, idx, swap, n);
        for (uint32_t i = 0; i < n; ++i) {
            memcpy(tmp + (size_t)i * out_schema->count,
                   out->values + (size_t)idx[i] * out_schema->count,
                   sizeof(qf_value) * out_schema->count);
        }
        memcpy(out->values, tmp, sizeof(qf_value) * out_schema->count * n);
    }

    if (c.err != QF_ERR_OK) return c.err;
    if (ast->limit >= 0 && (uint64_t)ast->limit < out->row_count) out->row_count = (uint64_t)ast->limit;
    return QF_ERR_OK;
}

/* ---------- explain ---------- */

static void append(qf_arena* a, qf_str* s, const char* text) {
    uint32_t n = (uint32_t)strlen(text);
    char* p = (char*)qf_arena_alloc(a, (uint64_t)s->len + n + 1, 1);
    if (!p) return;
    if (s->len) memcpy(p, s->ptr, s->len);
    memcpy(p + s->len, text, n);
    p[s->len + n] = '\0';
    s->ptr = p;
    s->len += n;
}

qf_err qf_engine_explain(const qf_host* host, qf_arena* arena, const qf_ast* ast,
                         const qf_schema* table, qf_str* out, qf_diag* diag) {
    (void)host;
    (void)diag;
    qf_str s = {NULL, 0};
    append(arena, &s, "Limit\n  Sort\n    Project\n");
    if (ast->group_count > 0 || select_has_agg(ast)) append(arena, &s, "      Aggregate\n");
    if (ast->where_root >= 0) append(arena, &s, "        Filter\n");
    char line[160];
    snprintf(line, sizeof line, "          Scan %.*s  (%u cols)\n",
             (int)ast->from_table.len, ast->from_table.ptr ? ast->from_table.ptr : "?",
             table ? table->count : 0);
    append(arena, &s, line);
    *out = s;
    return QF_ERR_OK;
}
