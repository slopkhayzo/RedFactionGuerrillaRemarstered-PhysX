#include "telemetry/profiler.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "telemetry/render_timeline.h"
#include "telemetry/telemetry.h"

namespace profiler {
namespace {

constexpr int kMaxRet = 8;
constexpr int kStackDwords = 512;  // 2 KB of stack scanned per sample

#pragma pack(push, 1)
struct Sample {
    uint32_t t_100us;  // time since plugin start, 0.1 ms units
    uint32_t frame;    // main-loop frame number (telemetry.csv)
    uint8_t thread;    // 0 = sim, 1 = render, 2 = scene (render_timeline.h, with Telemetry.RenderCensus=1)
    uint8_t nret;
    int8_t state;
    uint8_t pad;
    uint32_t eip;
    uint32_t ret[kMaxRet];
};
struct FileHeader {
    char magic[8];  // "RFGPROF1"
    uint32_t sample_size;
    uint32_t exe_base;
    uint32_t text_lo, text_hi;  // absolute
    uint32_t interval_us;
};
#pragma pack(pop)

struct Target {
    HANDLE handle = nullptr;
    DWORD tid = 0;
    uint8_t role = 0;
};

Config g_cfg;
uintptr_t g_text_lo = 0, g_text_hi = 0;
std::mutex g_file_mutex;
FILE* g_out = nullptr;
std::vector<Sample> g_buf;
std::atomic<bool> g_stop{false};
uint64_t g_samples = 0;

bool looks_like_return(uintptr_t v) {
    if (v < g_text_lo + 6 || v >= g_text_hi) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(v);
    if (p[-5] == 0xE8) return true;                             // call rel32
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;   // call [disp32] / [reg+disp32]
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;   // call [reg+disp8]
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;   // call reg / [reg]
    return false;
}

// No C++ objects here: the copy is guarded by SEH because the stack may end inside the window.
int copy_stack(uint32_t* dst, uintptr_t esp) {
    int copied = 0;
    __try {
        for (; copied < kStackDwords; ++copied) dst[copied] = reinterpret_cast<const uint32_t*>(esp)[copied];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return copied;
}

// While the target is suspended this takes no locks and allocates nothing (it may hold the heap lock).
bool sample_thread(const Target& t, Sample& s, uint32_t* stack, int& stack_len) {
    if (SuspendThread(t.handle) == DWORD(-1)) return false;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    const bool ok = GetThreadContext(t.handle, &ctx) != FALSE;
    if (ok) stack_len = copy_stack(stack, ctx.Esp);
    ResumeThread(t.handle);
    if (!ok) return false;
    s.eip = ctx.Eip;
    return true;
}

// In another build than GOG's the samples' rfg.exe code addresses are written as the GOG build's, so that
// tools/profile_report.py names them from the GOG PDB as usual (0 where the GOG build has no such code).
void as_gog_build(uint32_t& v) {
    if (v >= g_text_lo && v < g_text_hi) {
        const uint32_t gog = rfg::gog_rva(uint32_t(v - rfg::base()));
        v = gog ? uint32_t(rfg::base()) + gog : 0;
    }
}

void write_buffer_locked() {
    if (rfg::build() > 0)
        for (Sample& s : g_buf) {
            as_gog_build(s.eip);
            for (int i = 0; i < s.nret && i < kMaxRet; ++i) as_gog_build(s.ret[i]);
        }
    if (g_out && !g_buf.empty()) {
        fwrite(g_buf.data(), sizeof(Sample), g_buf.size(), g_out);
        fflush(g_out);
    }
    g_buf.clear();
}

void write_modules() {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    std::wstring path = session::run_dir() + L"\\profile_modules.csv";
    FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (!f) return;
    fputs("base,size,name\n", f);
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; ++i) {
        MODULEINFO mi{};
        char name[MAX_PATH] = {};
        if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) &&
            GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH))
            fprintf(f, "%08lX,%lu,%s\n", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mi.lpBaseOfDll)),
                    mi.SizeOfImage, name);
    }
    fclose(f);
}

DWORD WINAPI profiler_main(LPVOID) {
    SetThreadDescription(GetCurrentThread(), L"rfgphys profiler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    Target targets[3];
    targets[0].role = 0;
    targets[1].role = 1;
    targets[2].role = 2;
    static const char* const kRoleNames[3] = {"sim", "render", "scene"};
    bool modules_written = false;
    uint32_t stack[kStackDwords];

    while (!g_stop.load()) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -LONGLONG(g_cfg.profiler_interval_us) * 10;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
        const DWORD tids[3] = {telemetry::sim_thread_id(), telemetry::render_thread_id(),
                               render_timeline::scene_thread_id()};
        for (Target& t : targets) {
            if (!t.handle && tids[t.role]) {
                t.tid = tids[t.role];
                t.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                      t.tid);
                LOG("profiler: sampling %s thread %lu (%s)", kRoleNames[t.role], t.tid,
                    t.handle ? "ok" : "OpenThread failed");
            }
        }
        const int state = telemetry::current_state();
        if (g_cfg.profiler_gameplay_only && state != 1 /* GS_GAMEPLAY */) continue;
        if (!modules_written) {
            write_modules();
            modules_written = true;
        }
        for (const Target& t : targets) {
            if (!t.handle) continue;
            Sample s{};
            int stack_len = 0;
            if (!sample_thread(t, s, stack, stack_len)) continue;
            s.t_100us = uint32_t(session::since_start_ms() * 10.0);
            s.frame = telemetry::current_frame();
            s.thread = t.role;
            s.state = int8_t(state);
            for (int i = 0; i < stack_len && s.nret < kMaxRet; ++i)
                if (looks_like_return(stack[i])) s.ret[s.nret++] = stack[i];
            std::lock_guard<std::mutex> lock(g_file_mutex);
            g_buf.push_back(s);
            ++g_samples;
            if (g_buf.size() >= 4096) write_buffer_locked();
        }
    }
    return 0;
}

void flush(bool at_exit) {
    g_stop = true;
    if (at_exit) {
        if (!g_file_mutex.try_lock()) return;
    } else {
        g_file_mutex.lock();
    }
    write_buffer_locked();
    if (g_out) fclose(g_out);
    g_out = nullptr;
    LOG("profiler: %llu samples written", g_samples);
    g_file_mutex.unlock();
}

}  // namespace

bool start(const Config& cfg) {
    g_cfg = cfg;
    auto base = reinterpret_cast<const uint8_t*>(rfg::base());
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            g_text_lo = rfg::base() + sec[i].VirtualAddress;
            g_text_hi = g_text_lo + sec[i].Misc.VirtualSize;
        }
    }
    std::wstring path = session::run_dir() + L"\\profile.bin";
    g_out = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (!g_out) {
        LOG("profiler: cannot create %ls", path.c_str());
        return false;
    }
    FileHeader h{};
    memcpy(h.magic, "RFGPROF1", 8);
    h.sample_size = sizeof(Sample);
    h.exe_base = uint32_t(rfg::base());
    h.text_lo = uint32_t(g_text_lo);
    h.text_hi = uint32_t(g_text_hi);
    h.interval_us = uint32_t(cfg.profiler_interval_us);
    fwrite(&h, sizeof(h), 1, g_out);
    g_buf.reserve(4096);
    session::add_flush_handler(&flush);
    HANDLE th = CreateThread(nullptr, 128 * 1024, profiler_main, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    LOG("profiler: started, interval %d us, gameplay only %d", cfg.profiler_interval_us, cfg.profiler_gameplay_only);
    return th != nullptr;
}

}  // namespace profiler
