/*
 * MinGW 静态库（winpthread / mingwex / libgcc）被 link.exe 消费时，
 * 少数符号只在 MinGW 自己的运行时里存在。这里提供最小替身：
 *   - __mingw_raise_matherr：MinGW 的数学异常钩子，默认无操作；
 *   - _gnu_exception_handler：MinGW 的 SEH 过滤器，返回 0（继续搜索）。
 * 两者在本项目里都不会被真实触发——QuickJS 只用 winpthread 的锁与条件变量。
 */
int __mingw_raise_matherr(int type, const char* name, double a1, double a2, double r) {
    (void)type;
    (void)name;
    (void)a1;
    (void)a2;
    (void)r;
    return 0;
}

int _gnu_exception_handler(void* exception_info) {
    (void)exception_info;
    return 0;
}
