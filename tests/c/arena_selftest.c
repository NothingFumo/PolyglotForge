/* C 宿主自测：arena 的分配、对齐、字符串与生命周期。
   表达式语义已由 C# 阶段与 e2e 覆盖，这里只验证宿主内存层。 */
#include "engine.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    int fail = 0;
    qf_arena* a = qf_arena_new(4096);
    if (!a) {
        printf("FAIL: arena_new\n");
        return 1;
    }

    void* p = qf_arena_alloc(a, 24, 8);
    if (!p) {
        printf("FAIL: alloc\n");
        fail = 1;
    } else if (((uintptr_t)p % 8) != 0) {
        printf("FAIL: align\n");
        fail = 1;
    }

    qf_str s = qf_arena_str(a, "abc", 3);
    if (!s.ptr || s.len != 3 || memcmp(s.ptr, "abc", 3) != 0 || s.ptr[3] != '\0') {
        printf("FAIL: arena_str\n");
        fail = 1;
    }

    qf_str empty = qf_arena_str(a, NULL, 0);
    if (!empty.ptr || empty.len != 0) {
        printf("FAIL: empty str\n");
        fail = 1;
    }

    if (qf_arena_used(a) == 0) {
        printf("FAIL: used\n");
        fail = 1;
    }

    qf_arena_free(a);
    if (fail) {
        printf("c stage FAILED\n");
        return 1;
    }
    printf("c stage OK: arena\n");
    return 0;
}
