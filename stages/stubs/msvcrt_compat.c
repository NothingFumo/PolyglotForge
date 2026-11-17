/*
 * MSVC 运行时兼容层：MinGW 主干链接 MSVC 产出的静态库（C#/CPython）时，
 * 后者会引用 /GS 与 SEH 相关的运行时符号。这些符号在 gcc 的 CRT 里没有，
 * 但语义可以被安全替代：
 *   - 安全 cookie：本项目不处理不可信指针覆盖，校验恒通过；
 *   - __report_rangecheckfailure：越界检查失败，直接 abort；
 *   - __GSHandlerCheck：正常栈展开路径不会进入，返回 ExceptionContinueSearch。
 * 这是 ADR 0003 里记录的混链代价，任何真实异常路径都会走到 abort 而不是静默继续。
 */
#include <stdint.h>
#include <stdlib.h>

uintptr_t __security_cookie = 0x00002B992DDFA232ull;

/* MSVC 目标文件在用到浮点时会引用这个哨兵符号。 */
int _fltused = 0;

void __security_check_cookie(uintptr_t cookie) {
    if (cookie != __security_cookie && cookie != 0) {
        abort();
    }
}

void __report_rangecheckfailure(void) {
    abort();
}

/* EXCEPTION_DISPOSITION=1 表示 ExceptionContinueSearch */
int __GSHandlerCheck(void* record, void* frame, void* context, void* dispatch) {
    (void)record;
    (void)frame;
    (void)context;
    (void)dispatch;
    return 1;
}

void _guard_check_icall_nop(void) {}
