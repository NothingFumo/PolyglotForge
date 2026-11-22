/*
 * qf.h 的 qf_dsl_build 收 qf_str（结构体按值）。Kotlin/Native 导出函数只支持
 * 指针/标量参数：CValue 结构体参数会按指针传递，而 C 侧按值传递，运行期必然崩。
 * 因此 Kotlin 侧导出 qf_dsl_build_impl(host, ptr, len, out, diag)，
 * 这里补一个最小的 C ABI 适配层，符号与 contracts/qf.h 完全一致。
 *
 * 本文件由 build.sh 编译成 qf_shim.o 并用 ar 并入 libqf_kt.a。
 */
#include "qf.h"

#include <string.h>

qf_err qf_dsl_build_impl(const qf_host* host, const char* src, uint32_t len,
                         qf_ast** out, qf_diag* diag);

/* 宿主前置条件在 C 侧统一判定：缺 alloc/str 时返回错误码，而不是让 Kotlin 的
   IllegalStateException 穿过 FFI 变成进程终止。诊断文本用静态字面量，
   因为此路径上 host->str 不可用。 */
static qf_err qf_host_gate(const qf_host* host, qf_diag* diag) {
    if (host && host->alloc && host->str) return QF_ERR_OK;
    if (diag) {
        static const char msg[] = "host 缺少 alloc/str 服务";
        diag->code = QF_ERR_ARG;
        diag->line = 0;
        diag->col = 0;
        diag->message.ptr = msg;
        diag->message.len = (uint32_t)(sizeof(msg) - 1);
        diag->hint.ptr = NULL;
        diag->hint.len = 0;
    }
    return QF_ERR_ARG;
}

qf_err qf_dsl_build(const qf_host* host, qf_str src, qf_ast** out, qf_diag* diag) {
    qf_err gate = qf_host_gate(host, diag);
    if (gate != QF_ERR_OK) return gate;
    return qf_dsl_build_impl(host, src.ptr, src.len, out, diag);
}
