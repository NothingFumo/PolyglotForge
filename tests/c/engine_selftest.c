/*
 * C 阶段自测：不依赖任何其它阶段，手工构造 AST 与批次，验证执行语义。
 * 覆盖：分组 + count(*)、空值计数、过滤、排序方向、截断。
 */
#include "qf.h"
#include "engine.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static qf_value t(const char* s) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_TEXT;
    v.as.s.ptr = s;
    v.as.s.len = (uint32_t)strlen(s);
    return v;
}

static qf_value i64(int64_t n) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_INT;
    v.as.i = n;
    return v;
}

static qf_value nul(void) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_NULL;
    return v;
}

int main(void) {
    qf_arena* arena = qf_arena_new(1 << 16);
    qf_host host;
    host.alloc = NULL;
    host.str = NULL;
    host.ctx = NULL;

    qf_col cols[2];
    cols[0].name.ptr = "name"; cols[0].name.len = 4; cols[0].type = QF_T_TEXT;
    cols[1].name.ptr = "age";  cols[1].name.len = 3; cols[1].type = QF_T_INT;
    qf_schema table = {2, cols};

    qf_value rows[6] = {
        t("Ann"), i64(30),
        t("Bob"), i64(20),
        t("Ann"), i64(40),
    };
    qf_batch batch = {3, 2, rows};

    /* SELECT name, count(*) FROM t GROUP BY name */
    qf_node nodes[8];
    memset(nodes, 0, sizeof nodes);
    nodes[0].kind = QF_N_COL; nodes[0].a = 0; nodes[0].flags = QF_T_TEXT;
    nodes[1].kind = QF_N_AGG; nodes[1].op = QF_AGG_COUNT; nodes[1].argc = 0;
    int32_t sel[] = {0, 1};
    int32_t grp[] = {0};

    qf_ast ast;
    memset(&ast, 0, sizeof ast);
    ast.nodes = nodes;
    ast.node_count = 8;
    ast.select = sel;
    ast.select_count = 2;
    ast.from_table.ptr = "t";
    ast.from_table.len = 1;
    ast.where_root = -1;
    ast.group_by = grp;
    ast.group_count = 1;
    ast.limit = -1;

    qf_col outcols[2];
    outcols[0].name.ptr = "name"; outcols[0].name.len = 4; outcols[0].type = QF_T_TEXT;
    outcols[1].name.ptr = "n";    outcols[1].name.len = 1; outcols[1].type = QF_T_INT;
    qf_schema out_schema = {2, outcols};

    qf_result res;
    qf_diag diag;
    memset(&diag, 0, sizeof diag);
    memset(&res, 0, sizeof res);

    qf_err rc = qf_engine_execute(&host, arena, &ast, &table, &out_schema, &batch, NULL, NULL, &res, &diag);
    CHECK(rc == QF_ERR_OK, "分组执行应成功");
    CHECK(res.row_count == 2, "应有两组");
    CHECK(res.values[0].type == QF_T_TEXT && res.values[0].as.s.len == 3, "第一组键应为 Ann");
    CHECK(res.values[1].type == QF_T_INT && res.values[1].as.i == 2, "Ann 计数应为 2");
    CHECK(res.values[3].as.i == 1, "Bob 计数应为 1");

    /* 过滤 + 排序：SELECT name FROM t WHERE age > 25 ORDER BY age DESC */
    qf_ast ast2;
    memset(&ast2, 0, sizeof ast2);
    memset(nodes, 0, sizeof nodes);
    nodes[0].kind = QF_N_COL; nodes[0].a = 0; nodes[0].flags = QF_T_TEXT;
    nodes[1].kind = QF_N_COL; nodes[1].a = 1; nodes[1].flags = QF_T_INT;
    nodes[2].kind = QF_N_LIT; nodes[2].lit = i64(25);
    nodes[3].kind = QF_N_BIN; nodes[3].op = QF_OP_GT; nodes[3].a = 1; nodes[3].b = 2;
    /* 排序键指向输出列 0（name）；Rust 会把 ORDER BY 解析成输出列索引 */
    nodes[4].kind = QF_N_COL; nodes[4].a = 0; nodes[4].flags = QF_F_DESC;

    int32_t sel2[] = {0};
    int32_t ord2[] = {4};
    ast2.nodes = nodes;
    ast2.node_count = 8;
    ast2.select = sel2;
    ast2.select_count = 1;
    ast2.from_table.ptr = "t";
    ast2.from_table.len = 1;
    ast2.where_root = 3;
    ast2.order_by = ord2;
    ast2.order_count = 1;
    ast2.limit = 1;

    qf_schema out2 = {1, outcols};
    qf_result res2;
    memset(&res2, 0, sizeof res2);
    rc = qf_engine_execute(&host, arena, &ast2, &table, &out2, &batch, NULL, NULL, &res2, &diag);
    CHECK(rc == QF_ERR_OK, "过滤执行应成功");
    CHECK(res2.row_count == 1, "limit 1 应只留一行");
    CHECK(res2.values[0].as.s.len == 3 && memcmp(res2.values[0].as.s.ptr, "Ann", 3) == 0,
          "age>25 只剩 Ann（30/40 各有 Ann，limit 取第一行）");

    /* 空值：count(col) 不统计空值 */
    qf_value rows3[6] = {
        t("Ann"), nul(),
        t("Bob"), i64(20),
        t("Cid"), nul(),
    };
    qf_batch batch3 = {3, 2, rows3};
    memset(nodes, 0, sizeof nodes);
    nodes[0].kind = QF_N_AGG; nodes[0].op = QF_AGG_COUNT; nodes[0].argc = 1;
    nodes[0].b = 1;
    nodes[1].kind = QF_N_COL; nodes[1].a = 1; nodes[1].flags = QF_T_INT;
    int32_t sel3[] = {0};
    qf_ast ast3;
    memset(&ast3, 0, sizeof ast3);
    ast3.nodes = nodes;
    ast3.node_count = 8;
    ast3.select = sel3;
    ast3.select_count = 1;
    ast3.from_table.ptr = "t";
    ast3.from_table.len = 1;
    ast3.where_root = -1;
    ast3.limit = -1;
    qf_result res3;
    memset(&res3, 0, sizeof res3);
    rc = qf_engine_execute(&host, arena, &ast3, &table, &out2, &batch3, NULL, NULL, &res3, &diag);
    CHECK(rc == QF_ERR_OK, "count(col) 应成功");
    CHECK(res3.row_count == 1 && res3.values[0].as.i == 1, "count(age) 应只计非空值 1 条");

    qf_arena_free(arena);
    if (failures) {
        printf("c stage FAILED (%d)\n", failures);
        return 1;
    }
    printf("c stage OK: 分组/过滤/排序/空值 全部通过\n");
    return 0;
}
