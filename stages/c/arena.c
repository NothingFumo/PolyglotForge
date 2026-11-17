/*
 * arena：追加式分配器。全流程只增不减，退出时一次性释放。
 * 设计理由见 docs/adr/0002：跨语言边界不允许"谁分配谁释放"的模糊约定，
 * 因此所有阶段都从同一个 arena 取内存，生命周期由宿主独占。
 */
#include "qf.h"

#include <stdlib.h>
#include <string.h>

typedef struct qf_chunk {
    struct qf_chunk* prev;
    size_t cap;
    size_t used;
    unsigned char* data;
} qf_chunk;

struct qf_arena {
    qf_chunk* head;
    uint64_t reserved_total;
};

static size_t align_up(size_t n, size_t align) {
    return (n + (align - 1)) & ~(align - 1);
}

static qf_chunk* chunk_new(size_t cap) {
    qf_chunk* c = (qf_chunk*)calloc(1, sizeof(qf_chunk));
    if (!c) return NULL;
    c->data = (unsigned char*)malloc(cap);
    if (!c->data) {
        free(c);
        return NULL;
    }
    c->cap = cap;
    c->used = 0;
    return c;
}

qf_arena* qf_arena_new(uint64_t reserve_bytes) {
    size_t cap = reserve_bytes < 4096 ? 4096 : (size_t)reserve_bytes;
    qf_arena* a = (qf_arena*)calloc(1, sizeof(qf_arena));
    if (!a) return NULL;
    a->head = chunk_new(cap);
    if (!a->head) {
        free(a);
        return NULL;
    }
    return a;
}

void* qf_arena_alloc(qf_arena* a, uint64_t size, uint64_t align_) {
    if (!a) return NULL;
    if (align_ == 0) align_ = 8;
    if (align_ > 16) align_ = 16;
    size_t need = align_up((size_t)size, (size_t)align_);

    qf_chunk* c = a->head;
    size_t start = align_up(c->used, (size_t)align_);
    if (start + need > c->cap) {
        size_t cap = c->cap * 2;
        while (cap < need + (size_t)align_) cap *= 2;
        qf_chunk* n = chunk_new(cap);
        if (!n) return NULL;
        n->prev = c;
        a->head = n;
        a->reserved_total += cap;
        c = n;
        start = 0;
    }
    void* p = c->data + start;
    c->used = start + need;
    memset(p, 0, need);
    return p;
}

qf_str qf_arena_str(qf_arena* a, const char* src, uint32_t len) {
    qf_str out = {0, 0};
    if (!a) return out;
    char* p = (char*)qf_arena_alloc(a, (uint64_t)len + 1, 1);
    if (!p) return out;
    if (len && src) memcpy(p, src, len);
    p[len] = '\0';
    out.ptr = p;
    out.len = len;
    return out;
}

uint64_t qf_arena_used(const qf_arena* a) {
    if (!a) return 0;
    uint64_t total = 0;
    for (const qf_chunk* c = a->head; c; c = c->prev) total += c->used;
    return total;
}

void qf_arena_free(qf_arena* a) {
    if (!a) return;
    qf_chunk* c = a->head;
    while (c) {
        qf_chunk* prev = c->prev;
        free(c->data);
        free(c);
        c = prev;
    }
    free(a);
}
