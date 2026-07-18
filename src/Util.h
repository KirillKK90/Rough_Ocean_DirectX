#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <stdexcept>

// Throw on failed HRESULT with file/line context.
inline void ThrowIfFailedImpl(HRESULT hr, const char* expr, const char* file, int line)
{
    if (FAILED(hr))
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "HRESULT 0x%08X\n  %s\n  %s:%d", (unsigned)hr, expr, file, line);
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
        fprintf(stderr, "%s\n", buf);
        throw std::runtime_error(buf);
    }
}
#define HR(x) ThrowIfFailedImpl((x), #x, __FILE__, __LINE__)

inline void LogF(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    fputs(buf, stdout);
    fflush(stdout);
}

template <typename T>
constexpr T AlignUp(T value, T alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
