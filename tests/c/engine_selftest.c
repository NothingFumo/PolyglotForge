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

    /* ---------- 语义统一：三值逻辑 / int64 / 整数除法 / SUM ---------- */

    /* 1. int64 相等：不能借 double 比较，否则 2^53 以上会误判。 */
    {
        qf_value big[6] = {
            t("a"), i64(9007199254740993LL),
            t("b"), i64(9007199254740992LL),
            t("c"), i64(9007199254740994LL),
        };
        qf_batch bb = {3, 2, big};
        memset(nodes, 0, sizeof nodes);
        nodes[0].kind = QF_N_COL; nodes[0].a = 1; nodes[0].flags = QF_T_INT;
        nodes[1].kind = QF_N_LIT; nodes[1].lit = i64(9007199254740993LL);
        nodes[2].kind = QF_N_BIN; nodes[2].op = QF_OP_EQ; nodes[2].a = 0; nodes[2].b = 1;
        int32_t sb[] = {2};
        qf_ast astb;
        memset(&astb, 0, sizeof astb);
        astb.nodes = nodes; astb.node_count = 8;
        astb.select = sb; astb.select_count = 1;
        astb.from_table.ptr = "t"; astb.from_table.len = 1;
        astb.where_root = -1; astb.limit = -1;
        outcols[0].name.ptr = "b"; outcols[0].name.len = 1; outcols[0].type = QF_T_INT;
        qf_schema osb = {1, outcols};
        qf_result rb;
        memset(&rb, 0, sizeof rb);
        rc = qf_engine_execute(&host, arena, &astb, &table, &osb, &bb, NULL, NULL, &rb, &diag);
        CHECK(rc == QF_ERR_OK && rb.row_count == 3, "int64 相等查询应成功");
        CHECK(rb.values[0].as.i == 1 && rb.values[1].as.i == 0 && rb.values[2].as.i == 0,
              "int64 相等不得降精度");
    }

    /* 2. 大整数排序：同样必须按 int64 比较。 */
    {
        qf_value big[6] = {
            t("a"), i64(9007199254740993LL),
            t("b"), i64(9007199254740992LL),
            t("c"), i64(9007199254740994LL),
        };
        qf_batch bb = {3, 2, big};
        memset(nodes, 0, sizeof nodes);
        nodes[0].kind = QF_N_COL; nodes[0].a = 1; nodes[0].flags = QF_T_INT;
        nodes[1].kind = QF_N_COL; nodes[1].a = 0;
        int32_t sb[] = {0};
        int32_t ob[] = {1};
        qf_ast astb;
        memset(&astb, 0, sizeof astb);
        astb.nodes = nodes; astb.node_count = 8;
        astb.select = sb; astb.select_count = 1;
        astb.from_table.ptr = "t"; astb.from_table.len = 1;
        astb.where_root = -1; astb.order_by = ob; astb.order_count = 1;
        astb.limit = -1;
        outcols[0].name.ptr = "v"; outcols[0].name.len = 1; outcols[0].type = QF_T_INT;
        qf_schema osb = {1, outcols};
        qf_result rb;
        memset(&rb, 0, sizeof rb);
        rc = qf_engine_execute(&host, arena, &astb, &table, &osb, &bb, NULL, NULL, &rb, &diag);
        CHECK(rc == QF_ERR_OK && rb.row_count == 3, "大整数排序应成功");
        CHECK(rb.values[0].as.i == 9007199254740992LL &&
              rb.values[1].as.i == 9007199254740993LL &&
              rb.values[2].as.i == 9007199254740994LL,
              "排序必须按 int64 精度");
    }

    /* 3. 整数除法：7/2 = 3、-7/2 = -3（向零截断）。 */
    {
        qf_value divs[4] = { t("p"), i64(7), t("n"), i64(-7) };
        qf_batch bd = {2, 2, divs};
        memset(nodes, 0, sizeof nodes);
        nodes[0].kind = QF_N_COL; nodes[0].a = 1; nodes[0].flags = QF_T_INT;
        nodes[1].kind = QF_N_LIT; nodes[1].lit = i64(2);
        nodes[2].kind = QF_N_BIN; nodes[2].op = QF_OP_DIV; nodes[2].a = 0; nodes[2].b = 1;
        int32_t sd[] = {2};
        qf_ast astd;
        memset(&astd, 0, sizeof astd);
        astd.nodes = nodes; astd.node_count = 8;
        astd.select = sd; astd.select_count = 1;
        astd.from_table.ptr = "t"; astd.from_table.len = 1;
        astd.where_root = -1; astd.limit = -1;
        outcols[0].name.ptr = "q"; outcols[0].name.len = 1; outcols[0].type = QF_T_INT;
        qf_schema osd = {1, outcols};
        qf_result rd;
        memset(&rd, 0, sizeof rd);
        rc = qf_engine_execute(&host, arena, &astd, &table, &osd, &bd, NULL, NULL, &rd, &diag);
        CHECK(rc == QF_ERR_OK && rd.row_count == 2, "整数除法应成功");
        CHECK(rd.values[0].type == QF_T_INT && rd.values[0].as.i == 3, "7/2 应为整数 3");
        CHECK(rd.values[1].type == QF_T_INT && rd.values[1].as.i == -3, "-7/2 应为整数 -3");
    }

    /* 4. 三值逻辑：UNKNOWN OR TRUE = TRUE，UNKNOWN AND FALSE = FALSE。 */
    {
        qf_value tv[4] = { t("a"), nul(), t("b"), i64(50) };
        qf_batch bt = {2, 2, tv};
        memset(nodes, 0, sizeof nodes);
        nodes[0].kind = QF_N_COL; nodes[0].a = 1; nodes[0].flags = QF_T_INT;
        nodes[1].kind = QF_N_LIT; nodes[1].lit = i64(100);
        nodes[2].kind = QF_N_BIN; nodes[2].op = QF_OP_GT; nodes[2].a = 0; nodes[2].b = 1;
        nodes[3].kind = QF_N_LIT; nodes[3].lit = i64(1);
        nodes[4].kind = QF_N_LIT; nodes[4].lit = i64(1);
        nodes[5].kind = QF_N_BIN; nodes[5].op = QF_OP_EQ; nodes[5].a = 3; nodes[5].b = 4;
        nodes[6].kind = QF_N_BIN; nodes[6].op = QF_OP_OR; nodes[6].a = 2; nodes[6].b = 5;
        int32_t st[] = {6};
        qf_ast astt;
        memset(&astt, 0, sizeof astt);
        astt.nodes = nodes; astt.node_count = 8;
        astt.select = st; astt.select_count = 1;
        astt.from_table.ptr = "t"; astt.from_table.len = 1;
        astt.where_root = -1; astt.limit = -1;
        outcols[0].name.ptr = "v"; outcols[0].name.len = 1; outcols[0].type = QF_T_INT;
        qf_schema ost = {1, outcols};
        qf_result rt;
        memset(&rt, 0, sizeof rt);
        rc = qf_engine_execute(&host, arena, &astt, &table, &ost, &bt, NULL, NULL, &rt, &diag);
        CHECK(rc == QF_ERR_OK && rt.row_count == 2, "OR 三值逻辑应成功");
        CHECK(rt.values[0].as.i == 1 && rt.values[1].as.i == 1,
              "UNKNOWN OR TRUE 应为 TRUE");

        nodes[4].lit = i64(0); /* 1 = 0 */
        nodes[6].op = QF_OP_AND;
        memset(&rt, 0, sizeof rt);
        rc = qf_engine_execute(&host, arena, &astt, &table, &ost, &bt, NULL, NULL, &rt, &diag);
        CHECK(rc == QF_ERR_OK && rt.row_count == 2, "AND 三值逻辑应成功");
        CHECK(rt.values[0].as.i == 0 && rt.values[1].as.i == 0,
              "UNKNOWN AND FALSE 应为 FALSE");
    }

    /* 5. SUM 保 int64；全空输入返回 NULL。 */
    {
        qf_value sums[4] = {
            t("a"), i64(9007199254740993LL),
            t("b"), i64(1),
        };
        qf_batch bs = {2, 2, sums};
        memset(nodes, 0, sizeof nodes);
        nodes[0].kind = QF_N_AGG; nodes[0].op = QF_AGG_SUM; nodes[0].argc = 1;
        nodes[0].b = 1;
        nodes[1].kind = QF_N_COL; nodes[1].a = 1; nodes[1].flags = QF_T_INT;
        int32_t ss[] = {0};
        qf_ast asts;
        memset(&asts, 0, sizeof asts);
        asts.nodes = nodes; asts.node_count = 8;
        asts.select = ss; asts.select_count = 1;
        asts.from_table.ptr = "t"; asts.from_table.len = 1;
        asts.where_root = -1; asts.limit = -1;
        outcols[0].name.ptr = "s"; outcols[0].name.len = 1; outcols[0].type = QF_T_INT;
        qf_schema oss = {1, outcols};
        qf_result rs;
        memset(&rs, 0, sizeof rs);
        rc = qf_engine_execute(&host, arena, &asts, &table, &oss, &bs, NULL, NULL, &rs, &diag);
        CHECK(rc == QF_ERR_OK && rs.row_count == 1, "SUM 应成功");
        CHECK(rs.values[0].type == QF_T_INT && rs.values[0].as.i == 9007199254740994LL,
              "SUM(INT) 必须保持 int64 精度");

        qf_value nulls[2] = { t("a"), nul() };
        qf_batch bn = {1, 2, nulls};
        memset(&rs, 0, sizeof rs);
        rc = qf_engine_execute(&host, arena, &asts, &table, &oss, &bn, NULL, NULL, &rs, &diag);
        CHECK(rc == QF_ERR_OK && rs.row_count == 1, "全空 SUM 应产出一行");
        CHECK(rs.values[0].type == QF_T_NULL, "全空 SUM 应为 NULL");
    }

    /* 6. 分组数突破旧上限：65 组按需扩容。 */
    {
        qf_value grow[65 * 2];
        char names[65][4];
        for (int gi = 0; gi < 65; ++gi) {
            snprintf(names[gi], sizeof names[gi], "g%d", gi);
            grow[gi * 2] = t(names[gi]);
            grow[gi * 2 + 1] = i64(gi);
        }
        qf_batch bg = {65, 2, grow};
        memset(nodes, 0, sizeof nodes);
        nodes[0].kind = QF_N_COL; nodes[0].a = 0; nodes[0].flags = QF_T_TEXT;
        nodes[1].kind = QF_N_AGG; nodes[1].op = QF_AGG_COUNT; nodes[1].argc = 0;
        nodes[2].kind = QF_N_COL; nodes[2].a = 0; nodes[2].flags = QF_T_TEXT;
        int32_t sg[] = {0, 1};
        int32_t grpg[] = {2};
        qf_ast astg;
        memset(&astg, 0, sizeof astg);
        astg.nodes = nodes; astg.node_count = 8;
        astg.select = sg; astg.select_count = 2;
        astg.from_table.ptr = "t"; astg.from_table.len = 1;
        astg.where_root = -1;
        astg.group_by = grpg; astg.group_count = 1;
        astg.limit = -1;
        qf_col gcols[2];
        gcols[0].name.ptr = "g"; gcols[0].name.len = 1; gcols[0].type = QF_T_TEXT;
        gcols[1].name.ptr = "n"; gcols[1].name.len = 1; gcols[1].type = QF_T_INT;
        qf_schema og = {2, gcols};
        qf_result rg;
        memset(&rg, 0, sizeof rg);
        rc = qf_engine_execute(&host, arena, &astg, &table, &og, &bg, NULL, NULL, &rg, &diag);
        CHECK(rc == QF_ERR_OK, "65 组分组应成功");
        CHECK(rg.row_count == 65, "65 组应全部保留");
        CHECK(rg.values[0 * 2 + 1].as.i == 1 && rg.values[64 * 2 + 1].as.i == 1,
              "每组计数应为 1");
    }

    /* 7. 空输入全局聚合：仍产出一行（count=0，sum=NULL）。 */
    {
        memset(nodes, 0, sizeof nodes);
        nodes[0].kind = QF_N_AGG; nodes[0].op = QF_AGG_COUNT; nodes[0].argc = 0;
        nodes[1].kind = QF_N_AGG; nodes[1].op = QF_AGG_SUM; nodes[1].argc = 1;
        nodes[1].b = 2;
        nodes[2].kind = QF_N_COL; nodes[2].a = 1; nodes[2].flags = QF_T_INT;
        int32_t se[] = {0, 1};
        qf_ast aste;
        memset(&aste, 0, sizeof aste);
        aste.nodes = nodes; aste.node_count = 8;
        aste.select = se; aste.select_count = 2;
        aste.from_table.ptr = "t"; aste.from_table.len = 1;
        aste.where_root = -1; aste.limit = -1;
        qf_col ecols[2];
        ecols[0].name.ptr = "n"; ecols[0].name.len = 1; ecols[0].type = QF_T_INT;
        ecols[1].name.ptr = "s"; ecols[1].name.len = 1; ecols[1].type = QF_T_INT;
        qf_schema oe = {2, ecols};
        qf_batch be = {0, 2, NULL};
        qf_result re;
        memset(&re, 0, sizeof re);
        rc = qf_engine_execute(&host, arena, &aste, &table, &oe, &be, NULL, NULL, &re, &diag);
        CHECK(rc == QF_ERR_OK, "空输入聚合应成功");
        CHECK(re.row_count == 1, "空输入聚合应产出一行");
        CHECK(re.values[0].type == QF_T_INT && re.values[0].as.i == 0, "count(*) 应为 0");
        CHECK(re.values[1].type == QF_T_NULL, "sum 应为 NULL");
    }

    /* 8. 空输入 GROUP BY：保持 0 行。 */
    {
        memset(nodes, 0, sizeof nodes);
        nodes[0].kind = QF_N_COL; nodes[0].a = 0; nodes[0].flags = QF_T_TEXT;
        nodes[1].kind = QF_N_AGG; nodes[1].op = QF_AGG_COUNT; nodes[1].argc = 0;
        nodes[2].kind = QF_N_COL; nodes[2].a = 0; nodes[2].flags = QF_T_TEXT;
        int32_t sn[] = {0, 1};
        int32_t grpn[] = {2};
        qf_ast astn;
        memset(&astn, 0, sizeof astn);
        astn.nodes = nodes; astn.node_count = 8;
        astn.select = sn; astn.select_count = 2;
        astn.from_table.ptr = "t"; astn.from_table.len = 1;
        astn.where_root = -1;
        astn.group_by = grpn; astn.group_count = 1;
        astn.limit = -1;
        qf_col ncols[2];
        ncols[0].name.ptr = "g"; ncols[0].name.len = 1; ncols[0].type = QF_T_TEXT;
        ncols[1].name.ptr = "n"; ncols[1].name.len = 1; ncols[1].type = QF_T_INT;
        qf_schema on = {2, ncols};
        qf_batch bn = {0, 2, NULL};
        qf_result rn;
        memset(&rn, 0, sizeof rn);
        rc = qf_engine_execute(&host, arena, &astn, &table, &on, &bn, NULL, NULL, &rn, &diag);
        CHECK(rc == QF_ERR_OK && rn.row_count == 0, "空输入 GROUP BY 应为 0 行");
    }

    /* 9. 失败路径必须带错误码与可读消息。 */
    {
        qf_diag bad;
        qf_result rbad;
        memset(&bad, 0, sizeof bad);
        memset(&rbad, 0, sizeof rbad);
        rc = qf_engine_execute(&host, arena, &ast, &table, &out_schema,
                               NULL, NULL, NULL, &rbad, &bad);
        CHECK(rc == QF_ERR_ARG, "空批次应返回 QF_ERR_ARG");
        CHECK(bad.code == QF_ERR_ARG, "空批次应写入错误码");
        CHECK(bad.message.ptr != NULL && bad.message.len > 0, "空批次应写入诊断消息");

        qf_value one[2] = { t("Ann"), i64(30) };
        qf_batch wrong = {1, 1, one};
        memset(&bad, 0, sizeof bad);
        rc = qf_engine_execute(&host, arena, &ast, &table, &out_schema,
                               &wrong, NULL, NULL, &rbad, &bad);
        CHECK(rc == QF_ERR_ARG, "列数不一致应返回 QF_ERR_ARG");
        CHECK(bad.code == QF_ERR_ARG && bad.message.ptr != NULL && bad.message.len > 0,
              "列数不一致应写入诊断");
    }

    /* 10. UDF 失败必须中止查询，而不是产出 NULL。 */
    {
        qf_value ude[2] = { t("Ann"), i64(30) };
        qf_batch bu = {1, 2, ude};
        memset(nodes, 0, sizeof nodes);
        nodes[0].kind = QF_N_CALL;
        nodes[0].name.ptr = "boom"; nodes[0].name.len = 4;
        nodes[0].argc = 1; nodes[0].b = 1;
        nodes[1].kind = QF_N_COL; nodes[1].a = 1; nodes[1].flags = QF_T_INT;
        int32_t su[] = {0};
        qf_ast astu;
        memset(&astu, 0, sizeof astu);
        astu.nodes = nodes; astu.node_count = 8;
        astu.select = su; astu.select_count = 1;
        astu.from_table.ptr = "t"; astu.from_table.len = 1;
        astu.where_root = -1; astu.limit = -1;
        outcols[0].name.ptr = "b"; outcols[0].name.len = 1; outcols[0].type = QF_T_NULL;
        qf_schema osu = {1, outcols};
        qf_result ru;
        qf_diag ud;
        memset(&ud, 0, sizeof ud);
        memset(&ru, 0, sizeof ru);
        rc = qf_engine_execute(&host, arena, &astu, &table, &osu, &bu, NULL, NULL, &ru, &ud);
        CHECK(rc == QF_ERR_UDF, "UDF 失败应中止查询");
        CHECK(ud.code == QF_ERR_UDF && ud.message.ptr != NULL && ud.message.len > 0,
              "UDF 失败应带诊断");
    }

    qf_arena_free(arena);
    if (failures) {
        printf("c stage FAILED (%d)\n", failures);
        return 1;
    }
    printf("c stage OK: 分组/扩容/空聚合/过滤/排序/空值/三值逻辑/int64/整数除法/SUM/UDF 失败 全部通过\n");
    return 0;
}
