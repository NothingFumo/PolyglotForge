/*
 * MSVC C++ ABI 的窄接口替身。
 *
 * MSVC 编译的 AOT/CPython 目标文件里有少量 C++ 运行时入口，MinGW 的
 * libstdc++ 用的是 Itanium 修饰名，两边名字对不上（`??_V@YAXPEAX@Z` vs
 * `_ZdaPv`）。本项目不跨边界传 C++ 对象，这些入口只会被 malloc/free
 * 级别的实现调用，所以按语义给出最小实现即可。
 *
 * 修饰名不能写成 C 标识符，因此本文件里的函数用 qf_msvc_* 命名，
 * 由 build.sh 在编译后用 objcopy --redefine-sym 改名为 MSVC 修饰名
 * （`-Wl,--defsym` 不接受含 `?`/`@` 的符号名，实测报 syntax error）。
 *
 * 三个非 C++ 符号：
 *   - __isa_available：MSVC 的 CPU 分派哨兵。0 = 基线（SSE2），
 *     CPython/MSVC STL 的 lzcnt/popcnt 快路径会因此走通用实现；
 *   - __tls_guard：GC 的 thread_local 随机数状态在 MSVC 下用“按需初始化”
 *     保护字节，代码用 gs:[0x58] + SECREL 读取。放在 .tls$ 节里，偏移就落在
 *     本模块 TLS 块内，读到的恒为 0（每次多做一次空调用，但绝不解引用脏地址）；
 *   - __dyn_tls_on_demand_init：上面那个“按需初始化”回调。本项目的 MSVC 侧
 *     没有 TLS 动态初始化器要跑（唯一那个已被重命名到 .CRT$XCU），所以是空实现。
 */
#include <stdlib.h>

/* 运算符 new/delete：只做 malloc/free。nothrow 变体失败返回 NULL，
   其余变体在本项目里永远不会遇到 OOM，不做 throw 展开。 */
void* qf_msvc_new(unsigned long long n) { return malloc(n ? n : 1); }
void* qf_msvc_new_nothrow(unsigned long long n) { return malloc(n ? n : 1); }
void* qf_msvc_new_array(unsigned long long n) { return malloc(n ? n : 1); }
void* qf_msvc_new_array_nothrow(unsigned long long n) { return malloc(n ? n : 1); }
void qf_msvc_delete(void* p) { free(p); }
void qf_msvc_delete_sized(void* p, unsigned long long n) { (void)n; free(p); }
void qf_msvc_delete_array(void* p) { free(p); }
void qf_msvc_delete_array_sized(void* p, unsigned long long n) { (void)n; free(p); }

/* std::nothrow_t 的常量对象：只有一个字节的 tag，全零即可。 */
const char qf_msvc_nothrow_tag[8] = {0, 0, 0, 0, 0, 0, 0, 0};

int __isa_available = 0;

__attribute__((section(".tls$qf"), used)) char __tls_guard;

void __dyn_tls_on_demand_init(void) {}
