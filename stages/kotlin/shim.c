/*
 * qf.h 的 qf_dsl_build 收 qf_str（结构体按值）。Kotlin/Native 导出函数只支持
 * 指针/标量参数：CValue 结构体参数会按指针传递，而 C 侧按值传递，运行期必然崩。
 * 因此 Kotlin 侧导出 qf_dsl_build_impl(host, ptr, len, out, diag)，
 * 这里补一个最小的 C ABI 适配层，符号与 contracts/qf.h 完全一致。
 *
 * 本文件由 build.sh 编译成 qf_shim.o 并用 ar 并入 libqf_kt.a。
 */
#include "qf.h"

qf_err qf_dsl_build_impl(const qf_host* host, const char* src, uint32_t len,
                         qf_ast** out, qf_diag* diag);

qf_err qf_dsl_build(const qf_host* host, qf_str src, qf_ast** out, qf_diag* diag) {
    return qf_dsl_build_impl(host, src.ptr, src.len, out, diag);
}
