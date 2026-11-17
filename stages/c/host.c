/* 宿主服务：把 arena 包装成跨语言可调用的分配器。 */
#include "qf.h"
#include "engine.h"

static void* host_alloc(void* ctx, uint64_t size, uint64_t align) {
    return qf_arena_alloc((qf_arena*)ctx, size, align);
}

static qf_str host_str(void* ctx, const char* src, uint32_t len) {
    return qf_arena_str((qf_arena*)ctx, src, len);
}

qf_host qf_host_make(qf_arena* a) {
    qf_host h;
    h.alloc = host_alloc;
    h.str = host_str;
    h.ctx = a;
    return h;
}

uint32_t qf_abi_version(void) {
    return QF_ABI_VERSION;
}
