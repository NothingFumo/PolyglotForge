/*
 * Python 阶段自测：加载 fixtures/score.py，验证 qf_value 与 Python 对象互转、
 * 契约类型映射以及错误路径（未知 UDF / 插件内异常）。
 *
 * 只用 C 与 MSVC 链接，覆盖阶段自身行为；跨 CRT 的最终链接由集成负责。
 */
#include "qf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 测试用极简 arena：阶段只要求 host 提供 alloc/str，不需要真实 chunk 管理。 */
typedef struct t_arena {
    unsigned char* base;
    size_t cap;
    size_t used;
} t_arena;

static void* t_alloc(void* ctx, uint64_t size, uint64_t align) {
    t_arena* a = (t_arena*)ctx;
    if (align < 1) align = 1;
    if (align > 16) align = 16;
    size_t start = (a->used + (size_t)align - 1) & ~((size_t)align - 1);
    if (start + (size_t)size > a->cap) return NULL;
    void* p = a->base + start;
    a->used = start + (size_t)size;
    memset(p, 0, (size_t)size);
    return p;
}

static qf_str t_str(void* ctx, const char* src, uint32_t len) {
    qf_str out = {0, 0};
    char* p = (char*)t_alloc(ctx, (uint64_t)len + 1, 1);
    if (!p) return out;
    if (len) memcpy(p, src, len);
    p[len] = '\0';
    out.ptr = p;
    out.len = len;
    return out;
}

static int failures;

static void check(int cond, const char* what) {
    if (!cond) {
        printf("FAIL %s\n", what);
        ++failures;
    }
}

static qf_str S(const char* s) {
    qf_str r;
    r.ptr = s;
    r.len = (uint32_t)strlen(s);
    return r;
}

static qf_value v_int(int64_t i) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_INT;
    v.as.i = i;
    return v;
}

static qf_value v_dbl(double d) {
    qf_value v;
    memset(&v, 0, sizeof v);
    v.type = QF_T_DOUBLE;
    v.as.d = d;
    return v;
}

/* MSVC 没有 memmem：在 (ptr,len) 里找子串。 */
static int contains(qf_str hay, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0 || hay.len < n) return 0;
    for (uint32_t i = 0; i + n <= hay.len; ++i) {
        if (memcmp(hay.ptr + i, needle, n) == 0) return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: python_selftest <插件路径>\n");
        return 2;
    }

#ifdef QF_TEST_PYTHONHOME
    /* 静态库没有编译期 home，测试用 test.sh 传进来的标准库目录钉住 PYTHONHOME。
       真实运行时由集成方在启动前设置同一变量（指向载荷解包目录）。 */
    _putenv_s("PYTHONHOME", QF_TEST_PYTHONHOME);
#endif

    t_arena arena;
    arena.cap = 1u << 20;
    arena.used = 0;
    arena.base = (unsigned char*)malloc(arena.cap);
    if (!arena.base) {
        printf("FAIL 测试 arena 分配\n");
        return 1;
    }

    qf_host host;
    host.alloc = t_alloc;
    host.str = t_str;
    host.ctx = &arena;

    qf_diag diag;
    memset(&diag, 0, sizeof diag);

    qf_err rc = qf_py_load(&host, S(argv[1]), &diag);
    if (rc != QF_ERR_OK) {
        printf("FAIL 加载插件 rc=%d msg=%.*s\n", (int)rc,
               (int)diag.message.len, diag.message.ptr ? diag.message.ptr : "");
        qf_py_shutdown();
        free(arena.base);
        return 1;
    }

    /* 整数入参 → 整数返回值：score(30) == 60。 */
    qf_value arg = v_int(30);
    qf_value out;
    memset(&out, 0, sizeof out);
    rc = qf_py_call(&host, S("score"), &arg, 1, &out, &diag);
    check(rc == QF_ERR_OK, "score 返回码");
    check(out.type == QF_T_INT && out.as.i == 60, "score(30) == 60");

    /* 两参 + 浮点：total(30, 1.5) == 45.0。 */
    qf_value args2[2];
    args2[0] = v_int(30);
    args2[1] = v_dbl(1.5);
    memset(&out, 0, sizeof out);
    rc = qf_py_call(&host, S("total"), args2, 2, &out, &diag);
    check(rc == QF_ERR_OK && out.type == QF_T_DOUBLE && out.as.d == 45.0, "total(30,1.5) == 45.0");

    /* 文本返回值拷进 arena 并补 NUL。 */
    qf_value arg60 = v_int(60);
    memset(&out, 0, sizeof out);
    rc = qf_py_call(&host, S("label"), &arg60, 1, &out, &diag);
    check(rc == QF_ERR_OK, "label 返回码");
    check(out.type == QF_T_TEXT && out.as.s.len == 8 &&
              out.as.s.ptr && memcmp(out.as.s.ptr, "value=60", 8) == 0,
          "label(60) == \"value=60\"");
    check(out.as.s.ptr && out.as.s.ptr[out.as.s.len] == '\0', "文本返回值补 NUL");

    /* None → 空值。 */
    memset(&out, 0, sizeof out);
    out.type = QF_T_INT;
    rc = qf_py_call(&host, S("nothing"), &arg, 1, &out, &diag);
    check(rc == QF_ERR_OK && out.type == QF_T_NULL, "None 映射成空值");

    /* 未知函数必须是 QF_ERR_UDF，而不是悄悄成功。 */
    memset(&out, 0, sizeof out);
    memset(&diag, 0, sizeof diag);
    rc = qf_py_call(&host, S("does_not_exist"), &arg, 1, &out, &diag);
    check(rc == QF_ERR_UDF, "未知 UDF 返回 QF_ERR_UDF");
    check(diag.code == QF_ERR_UDF && diag.message.len > 0, "未知 UDF 带诊断");

    /* 标准库可用：UDF 里 import json，返回值仍是契约文本。 */
    memset(&out, 0, sizeof out);
    memset(&diag, 0, sizeof diag);
    rc = qf_py_call(&host, S("stdlib"), &arg, 1, &out, &diag);
    check(rc == QF_ERR_OK && out.type == QF_T_TEXT && contains(out.as.s, "{\"n\": 30}"),
          "UDF 内可 import 标准库");

    /* 插件内异常同样映射成 QF_ERR_UDF，诊断里保留中文前缀与原始消息。 */
    memset(&out, 0, sizeof out);
    memset(&diag, 0, sizeof diag);
    rc = qf_py_call(&host, S("boom"), &arg, 1, &out, &diag);
    check(rc == QF_ERR_UDF, "插件异常返回 QF_ERR_UDF");
    {
        static const char prefix[] = "Python UDF";
        check(diag.message.len >= sizeof prefix - 1 &&
                  diag.message.ptr &&
                  memcmp(diag.message.ptr, prefix, sizeof prefix - 1) == 0,
              "异常诊断带中文前缀");
        check(contains(diag.message, "插件内部错误"), "异常诊断保留原始消息");
    }

    qf_py_shutdown();
    free(arena.base);

    if (failures) {
        printf("python stage FAIL (%d)\n", failures);
        return 1;
    }
    printf("python stage OK\n");
    return 0;
}
