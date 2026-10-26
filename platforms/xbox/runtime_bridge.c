#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>

int __mingw_sprintf(char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}

int __mingw_snprintf(char* buffer, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}

int __mingw_vsnprintf(char* buffer, size_t size, const char* format, va_list args) {
    return vsnprintf(buffer, size, format, args);
}

int __mingw_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vprintf(format, args);
    va_end(args);
    return result;
}

int __mingw_sscanf(const char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsscanf(buffer, format, args);
    va_end(args);
    return result;
}

struct timezone;
int dht_gettimeofday(struct timeval* tv, struct timezone* tz) {
    (void)tz;
    FILETIME file_time;
    ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&file_time);
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    const uint64_t unix_ticks = ticks.QuadPart - UINT64_C(116444736000000000);
    tv->tv_sec = (long)(unix_ticks / UINT64_C(10000000));
    tv->tv_usec = (long)((unix_ticks % UINT64_C(10000000)) / 10);
    return 0;
}
