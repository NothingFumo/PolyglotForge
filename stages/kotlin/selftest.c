/*
 * Kotlin 阶段自测：C 侧只依赖 contracts/qf.h，验证三个跨语言契约点。
 *   1) DSL 文本经 qf_dsl_build 产出的 AST 形状（select/group/order 数量与 DESC 标志）；
 *   2) 非法 DSL 返回 QF_ERR_PARSE 且不产出 AST；
 *   3) qf_kt_batch 用协程并发调用 C 回调，逐条写回行数。
 * 宿主分配器用进程级 malloc：测试进程结束即释放，不需要 arena。
 */
#include "qf.h"

#include <malloc.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Kotlin 的 Abi.kt 用裸偏移写 qf_node.lit/name 与 qf_ast.from_table；
   ABI 结构一变这里就先炸，避免静默错位。 */
_Static_assert(offsetof(qf_node, lit) == 16, "Kotlin 侧 qf_node.lit 偏移");
_Static_assert(offsetof(qf_node, name) == 40, "Kotlin 侧 qf_node.name 偏移");
_Static_assert(offsetof(qf_ast, select_names) == 32, "Kotlin 侧 qf_ast.select_names 偏移");
_Static_assert(offsetof(qf_ast, from_table) == 40, "Kotlin 侧 qf_ast.from_table 偏移");
_Static_assert(offsetof(qf_diag, message) == 16, "Kotlin 侧 qf_diag.message 偏移");
_Static_assert(offsetof(qf_diag, hint) == 32, "Kotlin 侧 qf_diag.hint 偏移");

#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what);      \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static void* test_alloc(void* ctx, uint64_t size, uint64_t align) {
    (void)ctx;
    size_t n = size ? (size_t)size : 1;
    size_t a = align ? (size_t)align : 8;
    void* p = _aligned_malloc(n, a);
    if (p) memset(p, 0, n);
    return p;
}

static qf_str test_str(void* ctx, const char* src, uint32_t len) {
    (void)ctx;
    qf_str out = {0, 0};
    char* p = (char*)malloc((size_t)len + 1);
    if (!p) return out;
    if (len && src) memcpy(p, src, len);
    p[len] = '\0';
    out.ptr = p;
    out.len = len;
    return out;
}

static qf_str S(const char* s) {
    qf_str r = {s, (uint32_t)strlen(s)};
    return r;
}

static int str_is(qf_str s, const char* lit) {
    size_t n = strlen(lit);
    return s.ptr != NULL && (size_t)s.len == n && memcmp(s.ptr, lit, n) == 0;
}

/* 回调：按 SQL 文本返回固定行数；ctx 只是一个调用计数器。 */
static qf_err test_run(void* ctx, const char* sql, uint64_t* rows_out, qf_diag* diag) {
    (void)diag;
    atomic_int* calls = (atomic_int*)ctx;
    atomic_fetch_add(calls, 1);
    if (strcmp(sql, "q1") == 0) *rows_out = 2;
    else if (strcmp(sql, "q2") == 0) *rows_out = 3;
    else if (strcmp(sql, "q3") == 0) *rows_out = 5;
    else *rows_out = 0;
    return QF_ERR_OK;
}

int main(void) {
    qf_host host;
    host.alloc = test_alloc;
    host.str = test_str;
    host.ctx = NULL;

    /* ---------- (a) DSL 文本 → AST ---------- */
    const char* dsl =
        "from(users) {\n"
        "  select(\"name\" as \"who\", count(\"*\") as \"n\")\n"
        "  where { col(\"age\") gt 30 }\n"
        "  groupBy(\"name\")\n"
        "  orderBy(\"n\" desc)\n"
        "  limit(3)\n"
        "}\n";

    qf_diag diag;
    memset(&diag, 0, sizeof diag);
    qf_ast* ast = NULL;
    qf_err rc = qf_dsl_build(&host, S(dsl), &ast, &diag);
    CHECK(rc == QF_ERR_OK, "DSL 应解析成功");
    CHECK(ast != NULL, "ast 不应为 NULL");
    CHECK(ast->select_count == 2, "select 数量应为 2");
    CHECK(ast->group_count == 1, "groupBy 数量应为 1");
    CHECK(ast->order_count == 1, "orderBy 数量应为 1");
    CHECK(ast->limit == 3, "limit 应为 3");
    CHECK(str_is(ast->from_table, "users"), "表名应为 users");

    CHECK(ast->nodes[ast->order_by[0]].kind == QF_N_COL, "order 节点应为列");
    CHECK((ast->nodes[ast->order_by[0]].flags & QF_F_DESC) != 0, "order 应带 DESC 标志");
    CHECK(str_is(ast->nodes[ast->order_by[0]].name, "n"), "order 列应为 n");

    CHECK(ast->nodes[ast->select[0]].kind == QF_N_COL, "select[0] 应为列");
    CHECK(str_is(ast->nodes[ast->select[0]].name, "name"), "select[0] 的 node.name 应保留列名");
    CHECK(str_is(ast->select_names[0], "who"), "select_names[0] 应为列别名 who");
    CHECK(ast->nodes[ast->select[1]].kind == QF_N_AGG, "select[1] 应为聚合");
    CHECK(ast->nodes[ast->select[1]].op == QF_AGG_COUNT, "select[1] 应为 count");
    CHECK(str_is(ast->nodes[ast->select[1]].name, "count"), "聚合的 node.name 应保留函数名");
    CHECK(str_is(ast->select_names[1], "n"), "select_names[1] 应为别名 n");

    CHECK(str_is(ast->nodes[ast->group_by[0]].name, "name"), "group 列应为 name");

    CHECK(ast->where_root >= 0, "where 根不应缺失");
    {
        const qf_node* wr = &ast->nodes[ast->where_root];
        CHECK(wr->kind == QF_N_BIN && wr->op == QF_OP_GT, "where 应为 GT 二元表达式");
        CHECK(ast->nodes[wr->a].kind == QF_N_COL, "GT 左侧应为列");
        CHECK(str_is(ast->nodes[wr->a].name, "age"), "GT 左列应为 age");
        CHECK(ast->nodes[wr->b].kind == QF_N_LIT, "GT 右侧应为字面量");
        CHECK(ast->nodes[wr->b].lit.type == QF_T_INT, "GT 字面量应为整数");
        CHECK(ast->nodes[wr->b].lit.as.i == 30, "GT 字面量应为 30");
    }

    /* ---------- (b) 非法 DSL ---------- */
    qf_ast* bad = (qf_ast*)(size_t)1;
    qf_diag bad_diag;
    memset(&bad_diag, 0, sizeof bad_diag);
    rc = qf_dsl_build(&host, S("from(users) { select(\"name\" }"), &bad, &bad_diag);
    CHECK(rc == QF_ERR_PARSE, "非法 DSL 应返回 QF_ERR_PARSE");
    CHECK(bad == NULL, "非法 DSL 不应产出 AST");
    CHECK(bad_diag.code == QF_ERR_PARSE, "诊断码应为 QF_ERR_PARSE");
    CHECK(bad_diag.line >= 1, "诊断应带行号");

    /* ---------- (c) 协程批处理 ---------- */
    static const char* const sqls[3] = {"q1", "q2", "q3"};
    uint64_t rows[3] = {0, 0, 0};
    atomic_int calls = 0;
    qf_diag batch_diag;
    memset(&batch_diag, 0, sizeof batch_diag);
    rc = qf_kt_batch(&host, test_run, &calls, sqls, 3, rows, &batch_diag);
    if (rc != QF_ERR_OK) {
        fprintf(stderr, "batch 失败: rc=%d code=%d line=%u col=%u msg=%.*s\n",
                rc, batch_diag.code, batch_diag.line, batch_diag.col,
                (int)batch_diag.message.len,
                batch_diag.message.ptr ? batch_diag.message.ptr : "");
    }
    CHECK(rc == QF_ERR_OK, "batch 应成功");
    CHECK(atomic_load(&calls) == 3, "回调应被调用 3 次");
    CHECK(rows[0] == 2, "第 1 条应为 2 行");
    CHECK(rows[1] == 3, "第 2 条应为 3 行");
    CHECK(rows[2] == 5, "第 3 条应为 5 行");
    CHECK(rows[0] + rows[1] + rows[2] == 10, "行数之和应为 10");

    return 0;
}
