/* 宿主内部接口：只在 stages/c 内部使用，不属于跨语言契约。 */
#ifndef QF_ENGINE_H
#define QF_ENGINE_H

#include "qf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef qf_err (*qf_filter_fn)(void* ctx, const qf_batch* batch, uint8_t* keep, qf_diag* diag);

qf_err qf_engine_execute(const qf_host* host, qf_arena* arena, const qf_ast* ast,
                         const qf_schema* table, const qf_schema* out_schema,
                         const qf_batch* in, qf_filter_fn filter, void* fctx,
                         qf_result* out, qf_diag* diag);

qf_err qf_engine_explain(const qf_host* host, qf_arena* arena, const qf_ast* ast,
                         const qf_schema* table, qf_str* out, qf_diag* diag);

/* arena.c */
qf_arena* qf_arena_new(uint64_t reserve_bytes);
void*     qf_arena_alloc(qf_arena* a, uint64_t size, uint64_t align);
qf_str    qf_arena_str(qf_arena* a, const char* src, uint32_t len);
uint64_t  qf_arena_used(const qf_arena* a);
void      qf_arena_free(qf_arena* a);

#ifdef __cplusplus
}
#endif

#endif /* QF_ENGINE_H */
