#include "core/thread_dump.h"

#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "core/log.h"

namespace thread_dump {
namespace {

struct Module {
    uintptr_t lo, hi;
    char name[24];
};

std::vector<Module> list_modules() {
    std::vector<Module> out;
    HMODULE mods[512];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) return out;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 512; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        Module m{reinterpret_cast<uintptr_t>(mi.lpBaseOfDll), reinterpret_cast<uintptr_t>(mi.lpBaseOfDll) + mi.SizeOfImage,
                 {}};
        char name[MAX_PATH] = "";
        GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH);
        snprintf(m.name, sizeof(m.name), "%s", name);
        if (char* dot = strrchr(m.name, '.')) *dot = 0;
        out.push_back(m);
    }
    return out;
}

const Module* find_module(const std::vector<Module>& mods, uintptr_t v) {
    for (const Module& m : mods)
        if (v >= m.lo && v < m.hi) return &m;
    return nullptr;
}

// No C++ objects in these two: SEH guards the reads.
int copy_stack(uint32_t* dst, uintptr_t esp, int max_dwords) {
    int copied = 0;
    __try {
        for (; copied < max_dwords; ++copied) dst[copied] = reinterpret_cast<const uint32_t*>(esp)[copied];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return copied;
}

bool follows_call(uintptr_t v) {
    __try {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(v);
        if (p[-5] == 0xE8) return true;                            // call rel32
        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;  // call [disp32] / [reg+disp32]
        if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;  // call [reg+disp8]
        if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;  // call reg / [reg]
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

std::string where(const std::vector<Module>& mods, uintptr_t v) {
    char buf[48];
    if (const Module* m = find_module(mods, v))
        snprintf(buf, sizeof(buf), "%s+0x%X", m->name, unsigned(v - m->lo));
    else
        snprintf(buf, sizeof(buf), "0x%08X", unsigned(v));
    return buf;
}

}  // namespace

void log_thread(unsigned long tid, const char* label) {
    static uint32_t stack[8192];  // 32 KB
    if (!tid) {
        LOG("hang:   thread ? (%s): not known", label);
        return;
    }
    const std::vector<Module> mods = list_modules();
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) {
        LOG("hang:   thread %lu (%s): cannot open (error %lu)", tid, label, GetLastError());
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int n = 0;
    bool ok = false;
    if (SuspendThread(h) != DWORD(-1)) {
        ok = GetThreadContext(h, &ctx) != FALSE;
        if (ok) n = copy_stack(stack, ctx.Esp, int(std::size(stack)));
        ResumeThread(h);
    }
    CloseHandle(h);
    if (!ok) {
        LOG("hang:   thread %lu (%s): no context", tid, label);
        return;
    }
    LOG("hang:   thread %lu (%s): eip %s esp %08lX", tid, label, where(mods, ctx.Eip).c_str(), ctx.Esp);
    std::string line;
    int shown = 0, on_line = 0;
    for (int i = 0; i < n && shown < 64; ++i) {
        const uintptr_t v = stack[i];
        if (!find_module(mods, v) || !follows_call(v)) continue;
        line += ' ' + where(mods, v);
        ++shown;
        if (++on_line == 16) {
            LOG("hang:     %s", line.c_str());
            line.clear();
            on_line = 0;
        }
    }
    if (!line.empty()) LOG("hang:     %s", line.c_str());
}

}  // namespace thread_dump
