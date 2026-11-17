/*
 * Go c-archive 在 MSVC 装配器下的运行时初始化钩子。
 *
 * 现象：用 gcc 链接 Go 的 -buildmode=c-archive 产物正常；用 link.exe 链接
 * 同样能过，但第一次调用任何导出函数（qf_csv_scan）会永久挂起。
 *
 * 机制（Go 1.25，runtime/rt0_windows_amd64.s + runtime/cgo/gcc_libinit_windows.c）：
 *   - 静态库模式下 Go 不在 main 里自启动，而是把 `_rt0_amd64_windows_lib`
 *     的地址放进 go.o 的 `.ctors` 段，交给 C 运行时在 main 之前调用；
 *   - 该函数只负责起一个线程去跑 runtime·rt0_go，随后立即返回；
 *   - 每个导出函数入口都会先调 `_cgo_wait_runtime_init_done()`，它在
 *     `while (!_cgo_is_runtime_initialized()) WaitForSingleObject(runtime_init_wait, INFINITE);`
 *     上等初始化线程发出信号。
 *
 * `.ctors` 是 MinGW/gcc 的约定。MSVC 的 link.exe 只认 `.CRT$XC*` 初始化表，
 * `.ctors` 被当成普通数据段原样搬进镜像，永远没人调用 —— 初始化线程从未启动，
 * 于是首次调用永远等不到 `SetEvent`，表现为无输出的死锁。
 *
 * 修复：用 MSVC 的初始化段补上同一个调用。`_rt0_amd64_windows_lib` 的签名就是
 * `void(void)`，可以直接当 C 初始化函数用；gcc 侧通过 `.ctors` 做的正是同一件事，
 * 所以两条装配路径的启动顺序一致。
 *
 * 本文件只编译进 MSVC 装配器（scripts/msvc-*.bat），MinGW 主干链接时不需要。
 */
#include <stdint.h>

void _rt0_amd64_windows_lib(void);

#if defined(_MSC_VER)
/* /OPT:REF 不会丢弃 .CRT 段里的贡献：链接器把它们当作初始化表的一部分。 */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU"))
static void (*qf_go_runtime_init_hook)(void) = _rt0_amd64_windows_lib;
#else
/* gcc 已经处理 .ctors，这里若被误编进去会重复初始化 Go 运行时（abort）。 */
#error "go_crt_init.c 只用于 MSVC 装配器；MinGW 走 go.o 自带的 .ctors"
#endif
