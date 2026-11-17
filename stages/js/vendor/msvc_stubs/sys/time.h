#pragma once
#include <winsock2.h>
#include <windows.h>
static __inline int gettimeofday(struct timeval *tv, void *tz)
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
