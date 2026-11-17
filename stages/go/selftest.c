/* Go 阶段自测：用一个最小宿主把真实的 qf_csv_scan 走通。
 * 覆盖类型推断、列名写回、引号/转义/引号内换行/CRLF、显式列顺序、
 * 分片一致性，以及错误码与行列诊断。 */
#include "qf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Go 会把包目录下所有 .c 文件当作 cgo 的一部分编译，这个自测程序自带 main，
 * 直接编进 c-archive 会污染符号。因此只有 test.sh 手动编译时才展开主体。 */
#ifdef QF_SELFTEST_BUILD

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                          \
        }                                                                        \
    } while (0)

/* ---------- 最小宿主：逐块 malloc，退出时统一释放 ---------- */

typedef struct test_block {
    struct test_block* next;
} test_block;

typedef struct test_host {
    test_block* head;
} test_host;

static void* th_alloc(void* ctx, uint64_t size, uint64_t align) {
    test_host* th = (test_host*)ctx;
    if (align == 0) align = 8;
    if (align > 16) align = 16;
    size_t n = size ? (size_t)size : 1;
    unsigned char* raw = (unsigned char*)malloc(sizeof(test_block) + (size_t)align - 1 + n);
    if (!raw) return NULL;
    uintptr_t base = (uintptr_t)(raw + sizeof(test_block));
    uintptr_t p = (base + (uintptr_t)align - 1) & ~((uintptr_t)align - 1);
    test_block* blk = (test_block*)raw;
    blk->next = th->head;
    th->head = blk;
    memset((void*)p, 0, n);
    return (void*)p;
}

static qf_str th_str(void* ctx, const char* src, uint32_t len) {
    qf_str s = {0, 0};
    char* p = (char*)th_alloc(ctx, (uint64_t)len + 1, 1);
    if (!p) return s;
    if (len && src) memcpy(p, src, len);
    p[len] = '\0';
    s.ptr = p;
    s.len = len;
    return s;
}

static void th_free(test_host* th) {
    test_block* b = th->head;
    while (b) {
        test_block* next = b->next;
        free(b);
        b = next;
    }
    th->head = NULL;
}

/* ---------- 断言辅助 ---------- */

static int write_file(const char* path, const char* body) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = strlen(body);
    size_t w = fwrite(body, 1, n, f);
    int closed = fclose(f) == 0;
    return closed && w == n;
}

static qf_str cstr(const char* s) {
    qf_str out;
    out.ptr = s;
    out.len = (uint32_t)strlen(s);
    return out;
}

static const qf_value* cell_at(const qf_batch* b, uint32_t row, uint32_t col) {
    return &b->values[(size_t)row * b->cols + col];
}

static int is_text(const qf_value* v, const char* s) {
    size_t n = strlen(s);
    return v->type == QF_T_TEXT && v->as.s.len == (uint32_t)n && v->as.s.ptr != NULL &&
           (n == 0 || memcmp(v->as.s.ptr, s, n) == 0) && v->as.s.ptr[v->as.s.len] == '\0';
}

static int is_int(const qf_value* v, int64_t i) {
    return v->type == QF_T_INT && v->as.i == i;
}

static int is_double(const qf_value* v, double d) {
    return v->type == QF_T_DOUBLE && v->as.d == d;
}

static int col_same(const qf_col* a, const qf_col* b) {
    return a->type == b->type && a->name.len == b->name.len &&
           (a->name.len == 0 ||
            (a->name.ptr && b->name.ptr && memcmp(a->name.ptr, b->name.ptr, a->name.len) == 0));
}

static int col_name_is(const qf_col* c, const char* s) {
    size_t n = strlen(s);
    return c->name.ptr != NULL && c->name.len == (uint32_t)n &&
           (n == 0 || memcmp(c->name.ptr, s, n) == 0);
}

static int value_same(qf_value a, qf_value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
    case QF_T_NULL:
        return 1;
    case QF_T_INT:
        return a.as.i == b.as.i;
    case QF_T_DOUBLE:
        return a.as.d == b.as.d;
    case QF_T_TEXT:
        return a.as.s.len == b.as.s.len &&
               (a.as.s.len == 0 || (a.as.s.ptr && b.as.s.ptr &&
                                    memcmp(a.as.s.ptr, b.as.s.ptr, a.as.s.len) == 0));
    default:
        return 0;
    }
}

/* ---------- 数据 ---------- */

static const char CSV_BODY[] =
    "id,name,age,city,score\r\n"
    "1,Ann,30,SH,1.5\r\n"
    "2,\"Bob, Jr\",20,BJ,2\r\n"
    "3,\"Ci\"\"d\",,SZ,3.25\r\n"
    "4,\"Multi\r\nLine\",41,,4\r\n"
    "5,\"Quote \"\"inside\"\" field\",0,ZZ,5\r\n";

static const char BAD_QUOTE_BODY[] =
    "a,b\r\n"
    "1,\"oops\r\n"
    "2,3\r\n";

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: selftest <csv-path>\n");
        return 2;
    }

    test_host th = {NULL};
    qf_host host;
    host.alloc = th_alloc;
    host.str = th_str;
    host.ctx = &th;

    const char* csv_path = argv[1];
    CHECK(write_file(csv_path, CSV_BODY));

    qf_str path = cstr(csv_path);
    qf_diag diag;
    memset(&diag, 0, sizeof diag);

    /* 1. 推断模式：表头决定列名与类型，分 3 片。 */
    qf_schema table;
    memset(&table, 0, sizeof table);
    qf_batch batch;
    memset(&batch, 0, sizeof batch);
    qf_err rc = qf_csv_scan(&host, path, &table, 3, &batch, &diag);
    CHECK(rc == QF_ERR_OK);
    CHECK(table.count == 5);
    CHECK(table.cols != NULL);
    if (table.cols) {
        CHECK(col_name_is(&table.cols[0], "id"));
        CHECK(col_name_is(&table.cols[1], "name"));
        CHECK(col_name_is(&table.cols[2], "age"));
        CHECK(col_name_is(&table.cols[3], "city"));
        CHECK(col_name_is(&table.cols[4], "score"));
        CHECK(table.cols[0].type == QF_T_INT);
        CHECK(table.cols[1].type == QF_T_TEXT);
        CHECK(table.cols[2].type == QF_T_INT);
        CHECK(table.cols[3].type == QF_T_TEXT);
        CHECK(table.cols[4].type == QF_T_DOUBLE);
    }
    CHECK(batch.rows == 5);
    CHECK(batch.cols == 5);
    CHECK(batch.values != NULL);
    if (batch.values && batch.cols == 5 && batch.rows == 5) {
        CHECK(is_int(cell_at(&batch, 0, 0), 1));
        CHECK(is_int(cell_at(&batch, 4, 0), 5));
        CHECK(is_text(cell_at(&batch, 0, 1), "Ann"));
        CHECK(is_text(cell_at(&batch, 1, 1), "Bob, Jr"));
        CHECK(is_text(cell_at(&batch, 2, 1), "Ci\"d"));
        CHECK(is_text(cell_at(&batch, 3, 1), "Multi\r\nLine"));
        CHECK(is_text(cell_at(&batch, 4, 1), "Quote \"inside\" field"));
        CHECK(is_int(cell_at(&batch, 1, 2), 20));
        CHECK(cell_at(&batch, 2, 2)->type == QF_T_NULL);
        CHECK(is_text(cell_at(&batch, 1, 3), "BJ"));
        CHECK(cell_at(&batch, 3, 3)->type == QF_T_NULL);
        CHECK(is_double(cell_at(&batch, 0, 4), 1.5));
        CHECK(is_double(cell_at(&batch, 1, 4), 2.0));
        CHECK(is_double(cell_at(&batch, 2, 4), 3.25));
        CHECK(is_double(cell_at(&batch, 3, 4), 4.0));
    }

    /* 2. 显式表：按给定列顺序读取（取 CSV 前三列），表头仍然跳过。 */
    qf_col picked[3];
    picked[0].name = cstr("id");
    picked[0].type = QF_T_INT;
    picked[1].name = cstr("name");
    picked[1].type = QF_T_TEXT;
    picked[2].name = cstr("age");
    picked[2].type = QF_T_INT;
    qf_schema explicit_table;
    explicit_table.count = 3;
    explicit_table.cols = picked;
    qf_batch picked_batch;
    memset(&picked_batch, 0, sizeof picked_batch);
    diag = (qf_diag){0};
    rc = qf_csv_scan(&host, path, &explicit_table, 8, &picked_batch, &diag);
    CHECK(rc == QF_ERR_OK);
    CHECK(explicit_table.count == 3);
    CHECK(picked_batch.rows == 5);
    CHECK(picked_batch.cols == 3);
    if (picked_batch.values && picked_batch.rows == 5 && picked_batch.cols == 3) {
        CHECK(is_int(cell_at(&picked_batch, 0, 0), 1));
        CHECK(is_text(cell_at(&picked_batch, 0, 1), "Ann"));
        CHECK(is_int(cell_at(&picked_batch, 0, 2), 30));
        CHECK(cell_at(&picked_batch, 2, 2)->type == QF_T_NULL);
        CHECK(is_text(cell_at(&picked_batch, 4, 1), "Quote \"inside\" field"));
    }

    /* 3. 分片数不同，结果必须逐格一致。 */
    qf_schema t1, t16;
    memset(&t1, 0, sizeof t1);
    memset(&t16, 0, sizeof t16);
    qf_batch b1, b16;
    memset(&b1, 0, sizeof b1);
    memset(&b16, 0, sizeof b16);
    diag = (qf_diag){0};
    CHECK(qf_csv_scan(&host, path, &t1, 1, &b1, &diag) == QF_ERR_OK);
    diag = (qf_diag){0};
    CHECK(qf_csv_scan(&host, path, &t16, 16, &b16, &diag) == QF_ERR_OK);
    CHECK(b1.rows == b16.rows && b1.cols == b16.cols);
    CHECK(t1.count == t16.count);
    if (t1.count == t16.count && t1.cols && t16.cols) {
        for (uint32_t i = 0; i < t1.count; i++) {
            CHECK(col_same(&t1.cols[i], &t16.cols[i]));
        }
    }
    if (b1.values && b16.values && b1.rows == b16.rows && b1.cols == b16.cols) {
        for (uint32_t r = 0; r < b1.rows; r++) {
            for (uint32_t c = 0; c < b1.cols; c++) {
                CHECK(value_same(*cell_at(&b1, r, c), *cell_at(&b16, r, c)));
            }
        }
    }

    /* 4. 引号未闭合：QF_ERR_IO，且行列指向开引号。 */
    const char* bad_path = "stages/go/out/bad_quote.csv";
    CHECK(write_file(bad_path, BAD_QUOTE_BODY));
    qf_schema tbad;
    memset(&tbad, 0, sizeof tbad);
    qf_batch bbad;
    memset(&bbad, 0, sizeof bbad);
    qf_diag dbad;
    memset(&dbad, 0, sizeof dbad);
    rc = qf_csv_scan(&host, cstr(bad_path), &tbad, 2, &bbad, &dbad);
    CHECK(rc == QF_ERR_IO);
    CHECK(dbad.line == 2);
    CHECK(dbad.col == 3);
    CHECK(dbad.message.ptr != NULL && dbad.message.len > 0);

    /* 5. 文件打不开：QF_ERR_IO。 */
    qf_schema tmiss;
    memset(&tmiss, 0, sizeof tmiss);
    qf_batch bmiss;
    memset(&bmiss, 0, sizeof bmiss);
    qf_diag dmiss;
    memset(&dmiss, 0, sizeof dmiss);
    rc = qf_csv_scan(&host, cstr("stages/go/out/__missing__.csv"), &tmiss, 4, &bmiss, &dmiss);
    CHECK(rc == QF_ERR_IO);
    CHECK(dmiss.message.ptr != NULL && dmiss.message.len > 0);

    remove(bad_path);
    remove(csv_path);
    th_free(&th);

    if (failures) {
        fprintf(stderr, "%d 项断言失败\n", failures);
        return 1;
    }
    printf("go selftest OK\n");
    return 0;
}

#endif /* QF_SELFTEST_BUILD */
