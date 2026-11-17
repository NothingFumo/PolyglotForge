/*
 * Rust 阶段的 C 侧自测：只通过 contracts/qf.h 调用 qf_plan_build，
 * 验证 gcc 能链接 libqf_rust.a，并检查列解析、类型、GROUP BY 与 ORDER BY。
 */
#include "qf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static void* t_alloc(void* ctx, uint64_t size, uint64_t align) {
    (void)ctx;
    (void)align;
    return calloc(1, (size_t)(size ? size : 1));
}

static qf_str t_str(void* ctx, const char* src, uint32_t len) {
    (void)ctx;
    qf_str s;
    s.ptr = NULL;
    s.len = 0;
    if (len == 0) return s;
    char* p = (char*)malloc((size_t)len);
    if (!p) return s;
    memcpy(p, src, len);
    s.ptr = p;
    s.len = len;
    return s;
}

static qf_str S(const char* lit) {
    qf_str s;
    s.ptr = lit;
    s.len = (uint32_t)strlen(lit);
    return s;
}

static int str_is(qf_str s, const char* lit) {
    size_t n = strlen(lit);
    return s.len == n && (n == 0 || memcmp(s.ptr, lit, n) == 0);
}

static int str_has(qf_str s, const char* needle) {
    if (!s.ptr || s.len == 0) return 0;
    char* buf = (char*)malloc((size_t)s.len + 1);
    if (!buf) return 0;
    memcpy(buf, s.ptr, s.len);
    buf[s.len] = '\0';
    int found = strstr(buf, needle) != NULL;
    free(buf);
    return found;
}

static void node_init(qf_node* n, uint8_t kind) {
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->a = -1;
}

static void col_node(qf_node* n, const char* name) {
    node_init(n, QF_N_COL);
    n->name = S(name);
}

static void lit_int_node(qf_node* n, int64_t v) {
    node_init(n, QF_N_LIT);
    n->lit.type = QF_T_INT;
    n->lit.as.i = v;
}

static void agg_node(qf_node* n, uint8_t op, int32_t arg, int32_t argc) {
    node_init(n, QF_N_AGG);
    n->op = op;
    n->b = arg;
    n->argc = argc;
}

static void ast_init(qf_ast* ast, qf_node* nodes, uint32_t node_count,
                     int32_t* select, uint32_t select_count, qf_str* select_names) {
    memset(ast, 0, sizeof *ast);
    ast->nodes = nodes;
    ast->node_count = node_count;
    ast->select = select;
    ast->select_count = select_count;
    ast->select_names = select_names;
    ast->from_table = S("people");
    ast->where_root = -1;
    ast->limit = -1;
}

int main(void) {
    qf_host host;
    host.alloc = t_alloc;
    host.str = t_str;
    host.ctx = NULL;

    qf_col table_cols[3];
    table_cols[0].name = S("name");
    table_cols[0].type = QF_T_TEXT;
    table_cols[1].name = S("age");
    table_cols[1].type = QF_T_INT;
    table_cols[2].name = S("score");
    table_cols[2].type = QF_T_DOUBLE;
    qf_schema table;
    table.count = 3;
    table.cols = table_cols;

    /* ---------- 成功路径 ----------
     * SELECT name, count(*) AS n, avg(age) AS a
     *   FROM people GROUP BY name ORDER BY n DESC, name
     * 别名由 select_names 承载，node.name 只表示列名/函数名。
     */
    {
        qf_node nodes[6];
        col_node(&nodes[0], "name");
        agg_node(&nodes[1], QF_AGG_COUNT, 0, 0);
        agg_node(&nodes[2], QF_AGG_AVG, 3, 1);
        col_node(&nodes[3], "age");
        col_node(&nodes[4], "n");
        nodes[4].flags |= QF_F_DESC;
        col_node(&nodes[5], "name");

        int32_t sel[3] = {0, 1, 2};
        int32_t grp[1] = {0};
        int32_t ord[2] = {4, 5};
        qf_str names[3];
        names[0] = S("");
        names[1] = S("n");
        names[2] = S("a");

        qf_ast ast;
        ast_init(&ast, nodes, 6, sel, 3, names);
        ast.group_by = grp;
        ast.group_count = 1;
        ast.order_by = ord;
        ast.order_count = 2;

        qf_schema out;
        memset(&out, 0, sizeof out);
        qf_diag diag;
        memset(&diag, 0, sizeof diag);
        qf_err rc = qf_plan_build(&host, &ast, &table, &out, &diag);

        CHECK(rc == QF_ERR_OK, "成功计划应返回 QF_ERR_OK");
        CHECK(diag.code == QF_ERR_OK, "成功时 diag 应清零");
        CHECK(out.count == 3, "输出列数应为 3");
        if (out.count == 3 && out.cols) {
            CHECK(str_is(out.cols[0].name, "name"), "第 1 列名应为列名 name");
            CHECK(str_is(out.cols[1].name, "n"), "第 2 列名应为别名 n");
            CHECK(str_is(out.cols[2].name, "a"), "第 3 列名应为别名 a");
            CHECK(out.cols[0].type == QF_T_TEXT, "第 1 列类型应为文本");
            CHECK(out.cols[1].type == QF_T_INT, "count 类型应为整数");
            CHECK(out.cols[2].type == QF_T_DOUBLE, "avg 类型应为浮点");
        }

        /* 列节点回填 */
        CHECK(nodes[0].a == 0, "name 应解析为第 0 列");
        CHECK((nodes[0].flags & QF_F_TYPE_MASK) == QF_T_TEXT, "name 类型位应为文本");
        CHECK((nodes[0].flags & QF_F_RESOLVED) != 0, "列节点应置 QF_F_RESOLVED");
        CHECK(nodes[3].a == 1, "age 应解析为第 1 列");
        CHECK((nodes[3].flags & QF_F_TYPE_MASK) == QF_T_INT, "age 类型位应为整数");

        /* ORDER BY 改写成输出列引用，DESC 保留 */
        CHECK(nodes[4].kind == QF_N_COL, "ORDER BY 项应改写为 QF_N_COL");
        CHECK(nodes[4].a == 1, "ORDER BY n 应映射到输出第 1 列");
        CHECK((nodes[4].flags & QF_F_DESC) != 0, "ORDER BY DESC 位应保留");
        CHECK((nodes[4].flags & QF_F_TYPE_MASK) == QF_T_INT, "排序项类型应取输出列类型");
        CHECK(nodes[5].kind == QF_N_COL, "ORDER BY 别名项应改写为 QF_N_COL");
        CHECK(nodes[5].a == 0, "ORDER BY name 应映射到输出第 0 列");
    }

    /* ---------- 未知列 ---------- */
    {
        qf_node nodes[1];
        col_node(&nodes[0], "nope");
        int32_t sel[1] = {0};
        qf_str names[1];
        names[0] = S("");
        qf_ast ast;
        ast_init(&ast, nodes, 1, sel, 1, names);
        qf_schema out;
        memset(&out, 0, sizeof out);
        qf_diag diag;
        memset(&diag, 0, sizeof diag);
        qf_err rc = qf_plan_build(&host, &ast, &table, &out, &diag);
        CHECK(rc == QF_ERR_TYPE, "未知列应返回 QF_ERR_TYPE");
        CHECK(str_has(diag.message, "nope"), "诊断 message 应包含列名");
        CHECK(diag.hint.len > 0, "诊断 hint 不应为空");
    }

    /* ---------- 类型不匹配：文本 + 整数 ---------- */
    {
        qf_node nodes[3];
        node_init(&nodes[0], QF_N_BIN);
        nodes[0].op = QF_OP_ADD;
        nodes[0].a = 1;
        nodes[0].b = 2;
        col_node(&nodes[1], "name");
        lit_int_node(&nodes[2], 1);
        int32_t sel[1] = {0};
        qf_str names[1];
        names[0] = S("");
        qf_ast ast;
        ast_init(&ast, nodes, 3, sel, 1, names);
        qf_schema out;
        memset(&out, 0, sizeof out);
        qf_diag diag;
        memset(&diag, 0, sizeof diag);
        qf_err rc = qf_plan_build(&host, &ast, &table, &out, &diag);
        CHECK(rc == QF_ERR_TYPE, "文本参与算术应返回 QF_ERR_TYPE");
        CHECK(diag.message.len > 0, "类型错误应有 message");
    }

    /* ---------- GROUP BY 校验 ---------- */
    {
        qf_node nodes[3];
        col_node(&nodes[0], "age");
        agg_node(&nodes[1], QF_AGG_COUNT, 0, 0);
        col_node(&nodes[2], "name");
        int32_t sel[2] = {0, 1};
        int32_t grp[1] = {2};
        qf_str names[2];
        names[0] = S("");
        names[1] = S("");
        qf_ast ast;
        ast_init(&ast, nodes, 3, sel, 2, names);
        ast.group_by = grp;
        ast.group_count = 1;
        qf_schema out;
        memset(&out, 0, sizeof out);
        qf_diag diag;
        memset(&diag, 0, sizeof diag);
        qf_err rc = qf_plan_build(&host, &ast, &table, &out, &diag);
        CHECK(rc == QF_ERR_PLAN, "非分组列应返回 QF_ERR_PLAN");
        CHECK(str_has(diag.message, "GROUP BY"), "诊断应提到 GROUP BY");
    }

    /* ---------- ORDER BY 无法对应 ---------- */
    {
        qf_node nodes[2];
        col_node(&nodes[0], "age");
        col_node(&nodes[1], "zzz");
        int32_t sel[1] = {0};
        int32_t ord[1] = {1};
        qf_str names[1];
        names[0] = S("");
        qf_ast ast;
        ast_init(&ast, nodes, 2, sel, 1, names);
        ast.order_by = ord;
        ast.order_count = 1;
        qf_schema out;
        memset(&out, 0, sizeof out);
        qf_diag diag;
        memset(&diag, 0, sizeof diag);
        qf_err rc = qf_plan_build(&host, &ast, &table, &out, &diag);
        CHECK(rc == QF_ERR_TYPE, "ORDER BY 无法对应应返回 QF_ERR_TYPE");
        CHECK(str_has(diag.message, "zzz"), "诊断应包含 ORDER BY 目标名");
    }

    /* ---------- ORDER BY 序号 ---------- */
    {
        qf_node nodes[3];
        col_node(&nodes[0], "name");
        col_node(&nodes[1], "age");
        lit_int_node(&nodes[2], 2);
        int32_t sel[2] = {0, 1};
        int32_t ord[1] = {2};
        qf_str names[2];
        names[0] = S("");
        names[1] = S("");
        qf_ast ast;
        ast_init(&ast, nodes, 3, sel, 2, names);
        ast.order_by = ord;
        ast.order_count = 1;
        qf_schema out;
        memset(&out, 0, sizeof out);
        qf_diag diag;
        memset(&diag, 0, sizeof diag);
        qf_err rc = qf_plan_build(&host, &ast, &table, &out, &diag);
        CHECK(rc == QF_ERR_OK, "ORDER BY 序号应成功");
        CHECK(nodes[2].kind == QF_N_COL, "序号排序项应改写为 QF_N_COL");
        CHECK(nodes[2].a == 1, "ORDER BY 2 应映射到第 1 个输出列");
        if (out.count == 2 && out.cols) {
            CHECK(str_is(out.cols[0].name, "name"), "第 1 列名应为 name");
            CHECK(str_is(out.cols[1].name, "age"), "第 2 列名应为 age");
            CHECK(out.cols[0].type == QF_T_TEXT, "name 类型应为文本");
            CHECK(out.cols[1].type == QF_T_INT, "age 类型应为整数");
        }
    }

    if (failures) {
        printf("rust selftest: %d 项失败\n", failures);
        return 1;
    }
    printf("rust selftest passed\n");
    return 0;
}
