#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace logx {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_pending;  // lines logged before the file was opened

struct Clock {
    LARGE_INTEGER start{}, freq{};
    Clock() {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
};

const Clock& clock() {
    static const Clock c;  // thread-safe initialization
    return c;
}

void emit_locked(const char* line) {
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    } else {
        g_pending += line;
    }
    OutputDebugStringA(line);
}

}  // namespace

void open(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = _wfsopen(path.c_str(), L"ab", _SH_DENYWR);
    if (g_file && !g_pending.empty()) {
        fputs(g_pending.c_str(), g_file);
        fflush(g_file);
        g_pending.clear();
    }
}

void write(const char* fmt, ...) {
    const Clock& c = clock();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    char line[2048];
    int n = snprintf(line, sizeof(line), "[%10.3f] [%5lu] ",
                     double(now.QuadPart - c.start.QuadPart) / double(c.freq.QuadPart), GetCurrentThreadId());
    va_list args;
    va_start(args, fmt);
    int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    va_end(args);
    size_t len = size_t(n) + size_t(m < 0 ? 0 : (m > int(sizeof(line)) - n - 2 ? int(sizeof(line)) - n - 2 : m));
    line[len++] = '\n';
    line[len] = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    emit_locked(line);
}

void flush_at_exit() {
    if (g_mutex.try_lock()) {
        if (g_file) fflush(g_file);
        g_mutex.unlock();
    }
}

}  // namespace logx
