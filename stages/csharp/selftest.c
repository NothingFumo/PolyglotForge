/*
 * C# 阶段自测：用宿主 C 代码构造 AST 与批次，验证 qf_cs_* 的编译与求值。
 *
 * 覆盖：
 *   1. WHERE age > 30，3 行批次，keep 必须是 1/0/1；
 *   2. 空值参与比较 → 该行不保留（过滤语义）；
 *   3. 文本比较按 ordinal；
 *   4. 编译失败返回 QF_ERR_TYPE 并带中文诊断。
 */
#include "qf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("  [FAIL] %s\n", (msg));                     \
            g_fail = 1;                                         \
        }                                                       \
    } while (0)

/* ---------- 宿主：测试里用 malloc，进程结束即回收 ---------- */

static void* host_alloc(void* ctx, uint64_t size, uint64_t align) {
    (void)ctx;
    (void)align;
    return malloc(size ? (size_t)size : 1);
}

static qf_str host_str(void* ctx, const char* src, uint32_t len) {
    qf_str s;
    char* p = (char*)malloc(len ? len : 1);
    (void)ctx;
    if (len && p) memcpy(p, src, len);
    s.ptr = p;
    s.len = len;
    return s;
}

static qf_str lit_str(const char* text, uint32_t len) {
    qf_str s;
    s.ptr = text;
    s.len = len;
    return s;
}

#define STR(text) lit_str(text, (uint32_t)(sizeof(text) - 1))

static void print_diag(const qf_diag* d) {
    if (d->message.ptr && d->message.len) {
        printf("  diag: %.*s\n", (int)d->message.len, d->message.ptr);
    }
}

static void base_ast(qf_ast* ast, qf_node* nodes, uint32_t count, int32_t root) {
    memset(ast, 0, sizeof *ast);
    memset(nodes, 0, sizeof(qf_node) * count);
    ast->nodes = nodes;
    ast->node_count = count;
    ast->where_root = root;
    ast->limit = -1;
}

/* ---------- 1. WHERE age > 30 ---------- */

static void test_age_gt_30(const qf_host* host) {
    qf_node nodes[3];
    qf_ast ast;
    qf_col cols[1];
    qf_schema table;
    qf_diag diag;
    qf_pred* pred = NULL;
    qf_err rc;

    printf("[case] WHERE age > 30\n");
    base_ast(&ast, nodes, 3, 2);

    nodes[0].kind = QF_N_COL;
    nodes[0].a = 0;
    nodes[0].flags = (uint16_t)((QF_T_INT << QF_F_TYPE_SHIFT) | QF_F_RESOLVED);

    nodes[1].kind = QF_N_LIT;
    nodes[1].lit.type = QF_T_INT;
    nodes[1].lit.as.i = 30;

    nodes[2].kind = QF_N_BIN;
    nodes[2].op = QF_OP_GT;
    nodes[2].a = 0;
    nodes[2].b = 1;

    memset(cols, 0, sizeof cols);
    cols[0].name = STR("age");
    cols[0].type = QF_T_INT;
    table.count = 1;
    table.cols = cols;

    memset(&diag, 0, sizeof diag);
    rc = qf_cs_compile(host, &ast, ast.where_root, &table, &pred, &diag);
    CHECK(rc == QF_ERR_OK, "编译失败");
    CHECK(pred != NULL, "编译成功后 qf_pred 不应为空");
    if (rc != QF_ERR_OK || !pred) { print_diag(&diag); return; }

    {
        qf_value vals[3];
        qf_batch batch;
        uint8_t keep[3];

        memset(vals, 0, sizeof vals);
        vals[0].type = QF_T_INT;
        vals[0].as.i = 40;
        vals[1].type = QF_T_INT;
        vals[1].as.i = 20;
        vals[2].type = QF_T_INT;
        vals[2].as.i = 35;

        batch.rows = 3;
        batch.cols = 1;
        batch.values = vals;
        memset(keep, 0xAA, sizeof keep);

        rc = qf_cs_eval(pred, &batch, keep, &diag);
        CHECK(rc == QF_ERR_OK, "求值失败");
        if (rc != QF_ERR_OK) print_diag(&diag);
        CHECK(keep[0] == 1, "第 1 行(40) 应保留");
        CHECK(keep[1] == 0, "第 2 行(20) 应丢弃");
        CHECK(keep[2] == 1, "第 3 行(35) 应保留");
        printf("  keep = %u/%u/%u (期望 1/0/1)\n", keep[0], keep[1], keep[2]);
    }

    qf_cs_free(pred);
}

/* ---------- 2. 空值参与比较 ---------- */

static void test_null_compare(const qf_host* host) {
    qf_node nodes[3];
    qf_ast ast;
    qf_col cols[1];
    qf_schema table;
    qf_diag diag;
    qf_pred* pred = NULL;
    qf_value vals[4];
    qf_batch batch;
    uint8_t keep[4];
    qf_err rc;

    printf("[case] 空值参与比较\n");
    base_ast(&ast, nodes, 3, 2);

    nodes[0].kind = QF_N_COL;
    nodes[0].a = 0;
    nodes[0].flags = (uint16_t)((QF_T_INT << QF_F_TYPE_SHIFT) | QF_F_RESOLVED);
    nodes[1].kind = QF_N_LIT;
    nodes[1].lit.type = QF_T_INT;
    nodes[1].lit.as.i = 30;
    nodes[2].kind = QF_N_BIN;
    nodes[2].op = QF_OP_GT;
    nodes[2].a = 0;
    nodes[2].b = 1;

    memset(cols, 0, sizeof cols);
    cols[0].name = STR("age");
    cols[0].type = QF_T_INT;
    table.count = 1;
    table.cols = cols;

    memset(&diag, 0, sizeof diag);
    rc = qf_cs_compile(host, &ast, ast.where_root, &table, &pred, &diag);
    CHECK(rc == QF_ERR_OK && pred != NULL, "编译失败");
    if (rc != QF_ERR_OK || !pred) { print_diag(&diag); return; }

    memset(vals, 0, sizeof vals);
    vals[0].type = QF_T_INT;
    vals[0].as.i = 40;
    vals[1].type = QF_T_NULL;
    vals[2].type = QF_T_INT;
    vals[2].as.i = 35;
    vals[3].type = QF_T_INT;
    vals[3].as.i = 20;

    batch.rows = 4;
    batch.cols = 1;
    batch.values = vals;
    memset(keep, 0xAA, sizeof keep);

    rc = qf_cs_eval(pred, &batch, keep, &diag);
    CHECK(rc == QF_ERR_OK, "求值失败");
    CHECK(keep[0] == 1 && keep[1] == 0 && keep[2] == 1 && keep[3] == 0,
          "空值行应被丢弃 (期望 1/0/1/0)");
    printf("  keep = %u/%u/%u/%u (期望 1/0/1/0)\n", keep[0], keep[1], keep[2], keep[3]);

    qf_cs_free(pred);
}

/* ---------- 3. 文本 ordinal 比较 ---------- */

static void test_text_ordinal(const qf_host* host) {
    qf_node nodes[3];
    qf_ast ast;
    qf_col cols[1];
    qf_schema table;
    qf_diag diag;
    qf_pred* pred = NULL;
    qf_value vals[3];
    qf_batch batch;
    uint8_t keep[3];
    qf_err rc;

    printf("[case] 文本比较按 ordinal\n");
    base_ast(&ast, nodes, 3, 2);

    nodes[0].kind = QF_N_COL;
    nodes[0].a = 0;
    nodes[0].flags = (uint16_t)((QF_T_TEXT << QF_F_TYPE_SHIFT) | QF_F_RESOLVED);
    nodes[1].kind = QF_N_LIT;
    nodes[1].lit.type = QF_T_TEXT;
    nodes[1].lit.as.s = STR("bob");
    nodes[2].kind = QF_N_BIN;
    nodes[2].op = QF_OP_EQ;
    nodes[2].a = 0;
    nodes[2].b = 1;

    memset(cols, 0, sizeof cols);
    cols[0].name = STR("name");
    cols[0].type = QF_T_TEXT;
    table.count = 1;
    table.cols = cols;

    memset(&diag, 0, sizeof diag);
    rc = qf_cs_compile(host, &ast, ast.where_root, &table, &pred, &diag);
    CHECK(rc == QF_ERR_OK && pred != NULL, "编译失败");
    if (rc != QF_ERR_OK || !pred) { print_diag(&diag); return; }

    memset(vals, 0, sizeof vals);
    vals[0].type = QF_T_TEXT;
    vals[0].as.s = STR("bob");
    vals[1].type = QF_T_TEXT;
    vals[1].as.s = STR("Bob"); /* ordinal 下 'B'(0x42) != 'b'(0x62) */
    vals[2].type = QF_T_TEXT;
    vals[2].as.s = STR("bobby");

    batch.rows = 3;
    batch.cols = 1;
    batch.values = vals;
    memset(keep, 0xAA, sizeof keep);

    rc = qf_cs_eval(pred, &batch, keep, &diag);
    CHECK(rc == QF_ERR_OK, "求值失败");
    CHECK(keep[0] == 1 && keep[1] == 0 && keep[2] == 0, "文本相等应严格 ordinal (期望 1/0/0)");
    printf("  keep = %u/%u/%u (期望 1/0/0)\n", keep[0], keep[1], keep[2]);

    qf_cs_free(pred);
}

/* ---------- 4. 编译期诊断 ---------- */

static void test_compile_error(const qf_host* host) {
    qf_node nodes[2];
    qf_ast ast;
    qf_col cols[1];
    qf_schema table;
    qf_diag diag;
    qf_pred* pred = NULL;
    qf_err rc;

    printf("[case] 不支持的节点返回 QF_ERR_TYPE\n");
    base_ast(&ast, nodes, 2, 1);

    nodes[0].kind = QF_N_LIT;
    nodes[0].lit.type = QF_T_INT;
    nodes[0].lit.as.i = 1;

    nodes[1].kind = QF_N_CALL; /* 谓词里不允许 UDF */
    nodes[1].name = STR("f");
    nodes[1].argc = 1;
    nodes[1].b = 0;

    memset(cols, 0, sizeof cols);
    cols[0].name = STR("age");
    cols[0].type = QF_T_INT;
    table.count = 1;
    table.cols = cols;

    memset(&diag, 0, sizeof diag);
    rc = qf_cs_compile(host, &ast, ast.where_root, &table, &pred, &diag);
    CHECK(rc == QF_ERR_TYPE, "应返回 QF_ERR_TYPE");
    CHECK(pred == NULL, "失败时不应写出 qf_pred");
    CHECK(diag.code == QF_ERR_TYPE, "诊断码应同步");
    CHECK(diag.message.ptr != NULL && diag.message.len > 0, "诊断应带说明文本");
    if (diag.message.ptr) printf("  diag: %.*s\n", (int)diag.message.len, diag.message.ptr);
}

/* ---------- 5. 通用表达式求值 ---------- */

static void test_eval_values(const qf_host* host) {
    qf_node nodes[5];
    qf_ast ast;
    qf_col cols[1];
    qf_schema table;
    qf_diag diag;
    qf_value vals[3];
    qf_value out[3];
    qf_batch batch;
    qf_err rc;

    printf("[case] qf_cs_eval_values\n");
    base_ast(&ast, nodes, 5, -1);

    /* age * 2 + 1 */
    nodes[0].kind = QF_N_COL;
    nodes[0].a = 0;
    nodes[0].flags = (uint16_t)((QF_T_INT << QF_F_TYPE_SHIFT) | QF_F_RESOLVED);
    nodes[1].kind = QF_N_LIT;
    nodes[1].lit.type = QF_T_INT;
    nodes[1].lit.as.i = 2;
    nodes[2].kind = QF_N_BIN;
    nodes[2].op = QF_OP_MUL;
    nodes[2].a = 0;
    nodes[2].b = 1;
    nodes[3].kind = QF_N_LIT;
    nodes[3].lit.type = QF_T_INT;
    nodes[3].lit.as.i = 1;
    nodes[4].kind = QF_N_BIN;
    nodes[4].op = QF_OP_ADD;
    nodes[4].a = 2;
    nodes[4].b = 3;

    memset(cols, 0, sizeof cols);
    cols[0].name = STR("age");
    cols[0].type = QF_T_INT;
    table.count = 1;
    table.cols = cols;

    memset(vals, 0, sizeof vals);
    vals[0].type = QF_T_INT;
    vals[0].as.i = 40;
    vals[1].type = QF_T_NULL;
    vals[2].type = QF_T_INT;
    vals[2].as.i = 7;
    batch.rows = 3;
    batch.cols = 1;
    batch.values = vals;

    memset(&diag, 0, sizeof diag);
    memset(out, 0, sizeof out);
    rc = qf_cs_eval_values(host, &ast, 4, &table, &batch, NULL, NULL, out, &diag);
    CHECK(rc == QF_ERR_OK, "通用求值失败");
    if (rc != QF_ERR_OK) print_diag(&diag);
    CHECK(out[0].type == QF_T_INT && out[0].as.i == 81, "40*2+1 应为 81");
    CHECK(out[1].type == QF_T_NULL, "空值应传播");
    CHECK(out[2].type == QF_T_INT && out[2].as.i == 15, "7*2+1 应为 15");

    /* 文本输出必须经 host->str 拷回 */
    memset(vals, 0, sizeof vals);
    vals[0].type = QF_T_TEXT;
    vals[0].as.s = STR("Ann");
    batch.rows = 1;
    batch.values = vals;
    memset(&diag, 0, sizeof diag);
    memset(out, 0, sizeof out);
    rc = qf_cs_eval_values(host, &ast, 0, &table, &batch, NULL, NULL, out, &diag);
    CHECK(rc == QF_ERR_OK, "文本求值失败");
    CHECK(out[0].type == QF_T_TEXT && out[0].as.s.len == 3 &&
          out[0].as.s.ptr && memcmp(out[0].as.s.ptr, "Ann", 3) == 0,
          "文本输出应为 Ann");
}

int main(void) {
    qf_host host;
    host.alloc = host_alloc;
    host.str = host_str;
    host.ctx = NULL;

    test_age_gt_30(&host);
    test_null_compare(&host);
    test_text_ordinal(&host);
    test_compile_error(&host);
    test_eval_values(&host);

    qf_cs_free(NULL); /* 空句柄必须安全 */

    if (g_fail) {
        printf("csharp selftest FAILED\n");
        return 1;
    }
    printf("csharp stage OK\n");
    return 0;
}
