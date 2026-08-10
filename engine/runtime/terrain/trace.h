#pragma once
#include <cstdio>
#include <cstdarg>

namespace opennova {
extern FILE* g_trace;
inline void trace_log(const char* fmt, ...) {
    if (!g_trace) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_trace, fmt, ap);
    va_end(ap);
    fputc('\n', g_trace);
    fflush(g_trace);
}
}

