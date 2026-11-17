/*
 * C++ 阶段自测：只链接 libqf_cpp.a，不依赖其它阶段的实现。
 *
 * 自带一个最小 arena 包装出 qf_host，验证：
 *   a) 复杂查询的节点数、select/group/order 数量、DESC 标志、别名与 limit；
 *   b) 缺 FROM、非法字符、字符串未闭合分别得到 QF_ERR_PARSE 与合理的 line/col。
 */
#include "qf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        failures++; \
    } \
} while (0)

/* ---------- 最小 arena：只做追加分配，够本轮测试用 ---------- */

typedef struct test_arena {
    unsigned char* buf;
    size_t cap;
    size_t used;
} test_arena;

static void* test_alloc(void* ctx, uint64_t size, uint64_t align) {
    test_arena* a = (test_arena*)ctx;
    size_t start;
    if (align == 0) align = 1;
    if (a->used > a->cap) return NULL;
    start = a->used;
    if (start % (size_t)align != 0) start += (size_t)align - (start % (size_t)align);
    if (size == 0) size = 1;
    if (start > a->cap || (uint64_t)(a->cap - start) < size) return NULL;
    a->used = start + (size_t)size;
    return a->buf + start;
}

static qf_str test_str(void* ctx, const char* src, uint32_t len) {
    qf_str out;
    char* p = (char*)test_alloc(ctx, (uint64_t)len + 1, 1);
    if (!p) {
        out.ptr = NULL;
        out.len = 0;
        return out;
    }
    if (len) memcpy(p, src, len);
    p[len] = '\0';
    out.ptr = p;
    out.len = len;
    return out;
}

static qf_str S(const char* s) {
    qf_str r;
    r.ptr = s;
    r.len = (uint32_t)strlen(s);
    return r;
}

static int str_is(qf_str s, const char* lit) {
    size_t n = strlen(lit);
    if (s.len != (uint32_t)n) return 0;
    if (n == 0) return 1;
    return s.ptr != NULL && memcmp(s.ptr, lit, n) == 0;
}

static void reset_arena(test_arena* a) { a->used = 0; }

/* ---------- 用例 ---------- */

int main(void) {
    test_arena arena;
    qf_host host;
    qf_diag diag;
    qf_ast* ast;
    qf_err rc;

    arena.cap = 1u << 20;
    arena.used = 0;
    arena.buf = (unsigned char*)malloc(arena.cap);
    if (!arena.buf) {
        printf("FAIL: 无法申请测试 arena\n");
        return 1;
    }
    host.alloc = test_alloc;
    host.str = test_str;
    host.ctx = &arena;

    /* (a) 复杂查询 */
    {
        const char* sql =
            "SELECT name AS n, count(*) AS c, sum(age) + 1 AS s FROM people "
            "WHERE age >= 18 AND (name <> 'Bob' OR score / 2 > 1.5) "
            "GROUP BY name, dept "
            "ORDER BY count(*) DESC, name ASC "
            "LIMIT 10";
        reset_arena(&arena);
        ast = (qf_ast*)0x1;
        memset(&diag, 0, sizeof diag);
        rc = qf_sql_parse(&host, S(sql), &ast, &diag);

        CHECK(rc == QF_ERR_OK, "复杂查询应解析成功");
        CHECK(diag.code == QF_ERR_OK, "成功时 diag.code 应为 OK");
        CHECK(ast != NULL, "成功时 ast 不应为 NULL");

        if (rc == QF_ERR_OK && ast) {
            CHECK(ast->node_count == 23, "复杂查询节点数应为 23");
            CHECK(ast->select_count == 3, "select 项数应为 3");
            CHECK(ast->group_count == 2, "GROUP BY 项数应为 2");
            CHECK(ast->order_count == 2, "ORDER BY 项数应为 2");
            CHECK(ast->limit == 10, "LIMIT 应为 10");
            CHECK(ast->where_root >= 0 && (uint32_t)ast->where_root < ast->node_count,
                  "where_root 应指向有效节点");
            CHECK(str_is(ast->from_table, "people"), "from_table 应为 people");

            if (ast->where_root >= 0 && (uint32_t)ast->where_root < ast->node_count) {
                const qf_node* root = &ast->nodes[ast->where_root];
                CHECK(root->kind == QF_N_BIN, "where_root 应为二元节点");
                CHECK(root->op == QF_OP_AND, "where_root 应为 AND");
            }

            if (ast->select_count == 3) {
                const qf_node* n0 = &ast->nodes[ast->select[0]];
                const qf_node* n1 = &ast->nodes[ast->select[1]];
                const qf_node* n2 = &ast->nodes[ast->select[2]];
                CHECK(n0->kind == QF_N_COL, "第 1 个 select 项应为列");
                CHECK(str_is(n0->name, "name"), "别名不得覆盖列名");
                CHECK(ast->select_names != NULL, "select_names 应存在");
                if (ast->select_names) {
                    CHECK(str_is(ast->select_names[0], "n"), "第 1 个输出列名应为 n");
                    CHECK(str_is(ast->select_names[1], "c"), "count(*) 输出列名应为 c");
                    CHECK(str_is(ast->select_names[2], "s"), "加法项输出列名应为 s");
                }
                CHECK(n1->kind == QF_N_AGG && n1->op == QF_AGG_COUNT && n1->argc == 0,
                      "第 2 个 select 项应为 count(*)");
                CHECK(n2->kind == QF_N_BIN && n2->op == QF_OP_ADD,
                      "第 3 个 select 项应为加法节点");
            }

            if (ast->order_count == 2) {
                CHECK((ast->nodes[ast->order_by[0]].flags & QF_F_DESC) != 0,
                      "第 1 个 ORDER BY 项应置 QF_F_DESC");
                CHECK((ast->nodes[ast->order_by[1]].flags & QF_F_DESC) == 0,
                      "第 2 个 ORDER BY 项不应置 QF_F_DESC");
            }
        }
    }

    /* 最小查询：可选子句缺省值 */
    {
        reset_arena(&arena);
        ast = (qf_ast*)0x1;
        memset(&diag, 0, sizeof diag);
        rc = qf_sql_parse(&host, S("SELECT a FROM t"), &ast, &diag);
        CHECK(rc == QF_ERR_OK, "最小查询应解析成功");
        if (rc == QF_ERR_OK && ast) {
            CHECK(ast->node_count == 1, "最小查询节点数应为 1");
            CHECK(ast->select_count == 1 && ast->nodes[ast->select[0]].kind == QF_N_COL,
                  "最小查询应选择一个列");
            CHECK(ast->where_root == -1, "无 WHERE 时 where_root 应为 -1");
            CHECK(ast->group_count == 0, "无 GROUP BY 时 group_count 应为 0");
            CHECK(ast->order_count == 0, "无 ORDER BY 时 order_count 应为 0");
            CHECK(ast->limit == -1, "无 LIMIT 时 limit 应为 -1");
        }
    }

    /* (b1) 缺 FROM */
    {
        reset_arena(&arena);
        ast = (qf_ast*)0x1;
        memset(&diag, 0, sizeof diag);
        rc = qf_sql_parse(&host, S("SELECT a"), &ast, &diag);
        CHECK(rc == QF_ERR_PARSE, "缺 FROM 应返回 QF_ERR_PARSE");
        CHECK(ast == NULL, "解析失败时不应给出 AST");
        CHECK(diag.code == QF_ERR_PARSE, "缺 FROM 时 diag.code 应为 QF_ERR_PARSE");
        CHECK(diag.line == 1 && diag.col == 9, "缺 FROM 应定位到输入末尾 1:9");
        CHECK(diag.message.len > 0 && diag.message.ptr != NULL, "缺 FROM 应有中文说明");
        CHECK(diag.hint.len > 0 && diag.hint.ptr != NULL, "缺 FROM 应有期望 token 提示");
    }

    /* (b2) 非法字符 */
    {
        reset_arena(&arena);
        ast = (qf_ast*)0x1;
        memset(&diag, 0, sizeof diag);
        rc = qf_sql_parse(&host, S("SELECT a FROM t # 注释"), &ast, &diag);
        CHECK(rc == QF_ERR_PARSE, "非法字符应返回 QF_ERR_PARSE");
        CHECK(ast == NULL, "非法字符时不应给出 AST");
        CHECK(diag.code == QF_ERR_PARSE, "非法字符时 diag.code 应为 QF_ERR_PARSE");
        CHECK(diag.line == 1 && diag.col == 17, "非法字符应定位到 1:17");
        CHECK(diag.message.len > 0 && diag.message.ptr != NULL, "非法字符应有中文说明");
        CHECK(diag.hint.len > 0 && diag.hint.ptr != NULL, "非法字符应有期望 token 提示");
    }

    /* (b3) 字符串未闭合 */
    {
        reset_arena(&arena);
        ast = (qf_ast*)0x1;
        memset(&diag, 0, sizeof diag);
        rc = qf_sql_parse(&host, S("SELECT name FROM t WHERE name = 'Bob"), &ast, &diag);
        CHECK(rc == QF_ERR_PARSE, "字符串未闭合应返回 QF_ERR_PARSE");
        CHECK(ast == NULL, "字符串未闭合时不应给出 AST");
        CHECK(diag.code == QF_ERR_PARSE, "字符串未闭合时 diag.code 应为 QF_ERR_PARSE");
        CHECK(diag.line == 1 && diag.col == 33, "字符串未闭合应定位到开引号 1:33");
        CHECK(diag.message.len > 0 && diag.message.ptr != NULL, "字符串未闭合应有中文说明");
        CHECK(diag.hint.len > 0 && diag.hint.ptr != NULL, "字符串未闭合应有期望 token 提示");
    }

    /* (b4) 跨行位置：第二行的未闭合字符串 */
    {
        reset_arena(&arena);
        ast = (qf_ast*)0x1;
        memset(&diag, 0, sizeof diag);
        rc = qf_sql_parse(&host, S("SELECT a\nFROM t WHERE name = 'x"), &ast, &diag);
        CHECK(rc == QF_ERR_PARSE, "跨行未闭合字符串应返回 QF_ERR_PARSE");
        CHECK(diag.line == 2 && diag.col == 21, "跨行错误应定位到 2:21");
    }

    free(arena.buf);

    if (failures != 0) {
        printf("cpp selftest: %d 项失败\n", failures);
        return 1;
    }
    printf("cpp selftest: 全部通过\n");
    return 0;
}
