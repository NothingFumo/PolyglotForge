/* MSVC 链接 MinGW 侧静态库时的最小 CRT/线程垫片。
   Kotlin/Native 自带线程与调度，这些 pthread 入口不会被真正调用，
   但 libstdc++ 的 thread.o/condition_variable.o 会引用它们。 */
#include <windows.h>
#include <time.h>
#include <stdlib.h>

void __chkstk_ms(void) {}
void ___chkstk_ms(void) {}

static int qf_rand_s(unsigned int *p) { *p = (unsigned int)rand(); return 0; }
int (*__imp_rand_s)(unsigned int *) = qf_rand_s;

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)rem;
    if (!req) return -1;
    Sleep((DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000));
    return 0;
}


#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include <setjmp.h>
#include <string.h>

void *(*__imp__aligned_malloc)(size_t, size_t) = _aligned_malloc;
void (*__imp__aligned_free)(void *) = _aligned_free;
void (*__imp_longjmp)(void *, int) = (void (*)(void *, int))longjmp;

int __mingw_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return _vsnprintf_s(buf, n, _TRUNCATE, fmt, ap);
}

int sleep(unsigned seconds) { Sleep(seconds * 1000u); return 0; }

int gettimeofday(struct timeval *tv, void *tz)
{
    FILETIME ft;
    ULARGE_INTEGER ui;
    (void)tz;
    GetSystemTimeAsFileTime(&ft);
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    ui.QuadPart = (ui.QuadPart - 116444736000000000ULL) / 10ULL;
    tv->tv_sec = (long)(ui.QuadPart / 1000000ULL);
    tv->tv_usec = (long)(ui.QuadPart % 1000000ULL);
    return 0;
}

long StringCchCopyNA(char *dst, size_t cch, const char *src, size_t n)
{
    size_t i = 0;
    if (!dst || cch == 0) return -2147024809L;
    while (i < n && i + 1 < cch && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return 0;
}
