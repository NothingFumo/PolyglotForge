/*
 * JS 阶段自测：表格渲染与 where 沙箱。
 * 只依赖 stages/js/out/libqf_js.a（含 QuickJS）与 contracts/qf.h。
 */
#include "qf.h"
#include "qf_js_ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void* t_alloc(void* ctx, uint64_t size, uint64_t align) {
    (void)ctx;
    (void)align; /* malloc 在 x86_64 上至少 16 字节对齐 */
    return malloc(size ? (size_t)size : 1);
}

static qf_str t_str(void* ctx, const char* src, uint32_t len) {
    qf_str r;
    char*  p;
    (void)ctx;
    p = (char*)malloc((size_t)len + 1);
    if (!p) {
        r.ptr = NULL;
        r.len = 0;
        return r;
    }
    if (len) memcpy(p, src, len);
    p[len] = '\0';
    r.ptr = p;
    r.len = len;
    return r;
}

static int has(const qf_str* s, const char* lit) {
    size_t n = strlen(lit);
    uint32_t i;
    if (!s->ptr || s->len < n) return 0;
    for (i = 0; (size_t)i + n <= s->len; i++) {
        if (memcmp(s->ptr + i, lit, n) == 0) return 1;
    }
    return 0;
}

static qf_str S(const char* s) {
    qf_str r;
    r.ptr = s;
    r.len = (uint32_t)strlen(s);
    return r;
}

static qf_value v_text(const char* s) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_TEXT;
    v.as.s = S(s);
    return v;
}

static qf_value v_int(int64_t x) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_INT;
    v.as.i = x;
    return v;
}

static qf_value v_null(void) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_NULL;
    return v;
}

static void test_format_table(const qf_host* host) {
    qf_col   cols[2];
    qf_value vals[6];
    qf_result res;
    qf_str   out;
    qf_diag  diag;
    qf_err   rc;

    cols[0].name = S("name");
    cols[0].type = QF_T_TEXT;
    cols[1].name = S("age");
    cols[1].type = QF_T_INT;

    vals[0] = v_text("Ann");
    vals[1] = v_int(30);
    vals[2] = v_text("Bob");
    vals[3] = v_int(7);
    vals[4] = v_text("Cid");
    vals[5] = v_null();

    memset(&res, 0, sizeof res);
    res.schema.count = 2;
    res.schema.cols = cols;
    res.row_count = 3;
    res.values = vals;

    memset(&diag, 0, sizeof diag);
    memset(&out, 0, sizeof out);
    rc = qf_js_format(host, &res, &out, &diag);
    check(rc == QF_ERR_OK, "3x2 结果渲染应成功");
    if (rc != QF_ERR_OK) {
        fprintf(stderr, "  diag: %.*s\n", (int)diag.message.len,
                diag.message.ptr ? diag.message.ptr : "");
        return;
    }

    check(has(&out, "name   age\n"), "表头应按列宽对齐");
    check(has(&out, "----  ----\n"), "表头下应有等宽分隔线");
    check(has(&out, "Ann     30\n"), "数值列应右对齐");
    check(has(&out, "Bob      7\n"), "较窄数值仍应右对齐");
    check(has(&out, "(3 rows)\n"), "末尾应输出行数汇总");
    check(out.ptr != NULL && out.len > 0 && out.ptr[out.len] == '\0',
          "格式化结果应以 NUL 结尾");
}

static void test_format_null_and_truncate(const qf_host* host) {
    qf_col    col;
    qf_value  vals[2];
    qf_result res;
    qf_str    out;
    qf_diag   diag;
    qf_err    rc;

    col.name = S("v");
    col.type = QF_T_TEXT;
    vals[0] = v_null();
    vals[1] = v_text("0123456789012345678901234567890123456789"); /* 40 字符 */

    memset(&res, 0, sizeof res);
    res.schema.count = 1;
    res.schema.cols = &col;
    res.row_count = 2;
    res.values = vals;

    memset(&diag, 0, sizeof diag);
    memset(&out, 0, sizeof out);
    rc = qf_js_format(host, &res, &out, &diag);
    check(rc == QF_ERR_OK, "含空值与超宽文本的结果渲染应成功");
    if (rc != QF_ERR_OK) return;

    check(has(&out, "NULL"), "空值应渲染为 NULL");

    /* 40 字符截断为 31 字符 + 省略号（列宽上限 32） */
    check(has(&out, "0123456789012345678901234567890\u2026\n"),
          "超宽单元格应截断到 32 字符并追加省略号");
    check(!has(&out, "0123456789012345678901234567890123456789"),
          "超宽单元格不应完整输出");
}

static void test_where(const qf_host* host) {
    qf_col   cols[2];
    qf_value vals[6];
    qf_batch batch;
    uint8_t  keep[3];
    qf_diag  diag;
    qf_err   rc;

    cols[0].name = S("name");
    cols[0].type = QF_T_TEXT;
    cols[1].name = S("age");
    cols[1].type = QF_T_INT;

    vals[0] = v_text("Ann");
    vals[1] = v_int(40);
    vals[2] = v_text("Bob");
    vals[3] = v_int(20);
    vals[4] = v_text("Cid");
    vals[5] = v_int(50);

    memset(&batch, 0, sizeof batch);
    batch.rows = 3;
    batch.cols = 2;
    batch.values = vals;

    memset(&diag, 0, sizeof diag);
    memset(keep, 0xAA, sizeof keep);
    rc = qf_js_where_named(host, S("row.age > 30"), &(qf_schema){2, cols}, &batch,
                           keep, &diag);
    check(rc == QF_ERR_OK, "row.age > 30 应求值成功");
    check(keep[0] == 1 && keep[1] == 0 && keep[2] == 1,
          "row.age > 30 应得到 1/0/1");

    memset(&diag, 0, sizeof diag);
    memset(keep, 0xAA, sizeof keep);
    rc = qf_js_where(host, S("row.c1 > 30"), &batch, keep, &diag);
    check(rc == QF_ERR_OK, "契约入口应按 c0..cN 命名列");
    check(keep[0] == 1 && keep[1] == 0 && keep[2] == 1,
          "契约入口 row.c1 > 30 应得到 1/0/1");
}

static void test_where_error(const qf_host* host) {
    qf_col   cols[2];
    qf_value vals[2];
    qf_batch batch;
    uint8_t  keep[1];
    qf_diag  diag;
    qf_err   rc;

    cols[0].name = S("name");
    cols[0].type = QF_T_TEXT;
    cols[1].name = S("age");
    cols[1].type = QF_T_INT;
    vals[0] = v_text("Ann");
    vals[1] = v_int(30);

    memset(&batch, 0, sizeof batch);
    batch.rows = 1;
    batch.cols = 2;
    batch.values = vals;

    memset(&diag, 0, sizeof diag);
    rc = qf_js_where_named(host, S("row.age >"), &(qf_schema){2, cols}, &batch, keep,
                           &diag);
    check(rc == QF_ERR_EXEC, "语法错误的表达式应返回 QF_ERR_EXEC");
    check(diag.code == QF_ERR_EXEC && diag.message.ptr && diag.message.len > 0,
          "错误应带诊断");
    check(has(&diag.message, "表达式"), "诊断应为中文说明");

    memset(&diag, 0, sizeof diag);
    rc = qf_js_where_named(host, S("row.nope > 1"), &(qf_schema){2, cols}, &batch,
                           keep, &diag);
    check(rc == QF_ERR_EXEC, "读取未知列应返回 QF_ERR_EXEC");
    check(has(&diag.message, "未知列"), "未知列诊断应说明列不存在");

    memset(&diag, 0, sizeof diag);
    rc = qf_js_where_named(host, S("(() => { throw new Error('boom'); })()"),
                           &(qf_schema){2, cols}, &batch, keep, &diag);
    check(rc == QF_ERR_EXEC, "表达式抛错应返回 QF_ERR_EXEC");
}

int main(void) {
    qf_host host;

    host.alloc = t_alloc;
    host.str = t_str;
    host.ctx = NULL;

    check(qf_js_init() == QF_ERR_OK, "qf_js_init 应成功");

    test_format_table(&host);
    test_format_null_and_truncate(&host);
    test_where(&host);
    test_where_error(&host);

    if (failures) {
        fprintf(stderr, "js selftest: %d 项失败\n", failures);
        return 1;
    }
    printf("js selftest: 全部通过\n");
    return 0;
}
