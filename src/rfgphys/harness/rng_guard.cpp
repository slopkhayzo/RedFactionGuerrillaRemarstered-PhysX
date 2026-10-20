#include "harness/rng_guard.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/gameloop.h"
#include "core/log.h"
#include "game/rfg.h"
#include "harness/rec_file.h"
#include "hooks/hooks.h"

#pragma intrinsic(_ReturnAddress)

namespace rng_guard {
namespace {

constexpr uint32_t kIndexOffset = 0x9C4;  // Vlib_rand_instance: table index; the 2048-entry table follows at +0x9C8
constexpr uint32_t kTableSize = 2048;
const char* const kNames[] = {"vrand",
                              "fl_rand",
                              "bool_rand",
                              "int_rand_range",
                              "fl_rand_range",
                              "int_rand_chance",
                              "fl_rand_chance",
                              "vector::rand_sphere",
                              "vector4::rand_sphere",
                              "vector::rand_around_z",
                              "vector4::rand_around_z",
                              "timestamp::set_rand",
                              "timestamp_realtime::set_rand",
                              "crt rand"};

SRWLOCK g_lock = SRWLOCK_INIT;  // guards the shared table index and the stream maps
std::atomic<bool> g_isolate{false};
uint32_t g_seed = 1;
// Game generator streams: call site RVA -> table index; call sites on other threads (loader, render) are keyed
// with kOtherThread so they never share a stream with the sim thread.
std::unordered_map<uint32_t, uint32_t> g_vlib_streams;
constexpr uint32_t kOtherThread = 0x80000000u;
std::unordered_map<uint32_t, uint32_t> g_crt_streams;   // sim thread: call site RVA -> LCG state
int g_cells_depth = 0;                                  // sim thread, inside create_cells

thread_local int t_depth = 0;

struct Entry {
    int32_t frame;
    uint16_t id;
    uint16_t index;   // stream position before the draw (CRT: low bits of the stream state)
    uint32_t caller;  // return address RVA
};
std::vector<Entry> g_trace;  // sim thread only
std::atomic<bool> g_tracing{false};
int32_t g_frame = -1;
constexpr size_t kMaxTrace = 4'000'000;

std::mutex g_other_mutex;
std::map<uint64_t, uint32_t> g_other;  // (id << 32 | caller RVA) -> draws on threads other than the sim thread

uint32_t mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

uint32_t& shared_index() {
    return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rfg::addr(rfg::rva::Vlib_rand_instance)) + kIndexOffset);
}

void trace(uint32_t id, uint16_t index, uint32_t caller) {
    if (g_tracing.load(std::memory_order_relaxed) && g_trace.size() < kMaxTrace)
        g_trace.push_back({g_frame, uint16_t(id), index, caller});
}

// Wraps one generator call. Nested calls (a generator function calling another) are covered by the outer guard.
class Guard {
public:
    Guard(uint32_t id, void* ret) {
        if (t_depth++ > 0) return;
        const bool sim = gameloop::on_sim_thread();
        caller_ = uint32_t(reinterpret_cast<uintptr_t>(ret) - rfg::base());
        if (!sim) {
            std::lock_guard<std::mutex> lock(g_other_mutex);
            ++g_other[(uint64_t(id) << 32) | caller_];
        }
        if (g_isolate.load(std::memory_order_acquire)) {
            AcquireSRWLockExclusive(&g_lock);
            locked_ = true;
            key_ = sim ? caller_ : (caller_ | kOtherThread);
            auto it = g_vlib_streams.find(key_);
            if (it == g_vlib_streams.end())
                it = g_vlib_streams.emplace(key_, mix(g_seed ^ mix(key_)) % kTableSize).first;
            saved_ = shared_index();
            shared_index() = it->second;
            swapped_ = true;
        }
        if (sim) trace(id, uint16_t(shared_index()), caller_);
    }
    ~Guard() {
        if (--t_depth > 0) return;
        if (swapped_) {
            g_vlib_streams[key_] = shared_index();
            shared_index() = saved_;
        }
        if (locked_) ReleaseSRWLockExclusive(&g_lock);
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    bool locked_ = false, swapped_ = false;
    uint32_t saved_ = 0, caller_ = 0, key_ = 0;
};

uint32_t(__cdecl* o_vrand)();
float(__cdecl* o_fl_rand)();
bool(__cdecl* o_bool_rand)();
int(__cdecl* o_int_rand_range)(int, int);
float(__cdecl* o_fl_rand_range)(float, float);
bool(__cdecl* o_int_rand_chance)(uint32_t);
bool(__cdecl* o_fl_rand_chance)(float);
// __thiscall methods, hooked through __fastcall (ecx = this, edx unused).
void(__fastcall* o_vec_sphere)(void*, void*);
void(__fastcall* o_vec4_sphere)(void*, void*);
void(__fastcall* o_vec_around_z)(void*, void*, float, float);
void(__fastcall* o_vec4_around_z)(void*, void*, float, float);
void(__fastcall* o_ts_set_rand)(void*, void*, int, int);
void(__fastcall* o_tsr_set_rand)(void*, void*, int, int);

uint32_t __cdecl d_vrand() {
    Guard g(0, _ReturnAddress());
    return o_vrand();
}
float __cdecl d_fl_rand() {
    Guard g(1, _ReturnAddress());
    return o_fl_rand();
}
bool __cdecl d_bool_rand() {
    Guard g(2, _ReturnAddress());
    return o_bool_rand();
}
int __cdecl d_int_rand_range(int lo, int hi) {
    Guard g(3, _ReturnAddress());
    return o_int_rand_range(lo, hi);
}
float __cdecl d_fl_rand_range(float lo, float hi) {
    Guard g(4, _ReturnAddress());
    return o_fl_rand_range(lo, hi);
}
bool __cdecl d_int_rand_chance(uint32_t n) {
    Guard g(5, _ReturnAddress());
    return o_int_rand_chance(n);
}
bool __cdecl d_fl_rand_chance(float p) {
    Guard g(6, _ReturnAddress());
    return o_fl_rand_chance(p);
}
void __fastcall d_vec_sphere(void* self, void* edx) {
    Guard g(7, _ReturnAddress());
    o_vec_sphere(self, edx);
}
void __fastcall d_vec4_sphere(void* self, void* edx) {
    Guard g(8, _ReturnAddress());
    o_vec4_sphere(self, edx);
}
void __fastcall d_vec_around_z(void* self, void* edx, float a, float b) {
    Guard g(9, _ReturnAddress());
    o_vec_around_z(self, edx, a, b);
}
void __fastcall d_vec4_around_z(void* self, void* edx, float a, float b) {
    Guard g(10, _ReturnAddress());
    o_vec4_around_z(self, edx, a, b);
}
void __fastcall d_ts_set_rand(void* self, void* edx, int lo, int hi) {
    Guard g(11, _ReturnAddress());
    o_ts_set_rand(self, edx, lo, hi);
}
void __fastcall d_tsr_set_rand(void* self, void* edx, int lo, int hi) {
    Guard g(12, _ReturnAddress());
    o_tsr_set_rand(self, edx, lo, hi);
}

// Order-independent hash of the sim thread's streams (other threads' streams, e.g. the render thread's, advance
// with timing and are left out).
uint32_t map_hash(const std::unordered_map<uint32_t, uint32_t>& m) {
    uint32_t h = 0;
    for (const auto& [key, value] : m)
        if (!(key & kOtherThread)) h += mix(key * 0x9E3779B9u ^ mix(value));
    return h;
}

}  // namespace

bool install() {
    namespace r = rfg::rva;
    bool ok = true;
    ok &= hooks::create(r::vrand, &d_vrand, &o_vrand, "vrand");
    ok &= hooks::create(r::fl_rand, &d_fl_rand, &o_fl_rand, "fl_rand");
    ok &= hooks::create(r::bool_rand, &d_bool_rand, &o_bool_rand, "bool_rand");
    ok &= hooks::create(r::int_rand_range, &d_int_rand_range, &o_int_rand_range, "int_rand_range");
    ok &= hooks::create(r::fl_rand_range, &d_fl_rand_range, &o_fl_rand_range, "fl_rand_range");
    ok &= hooks::create(r::int_rand_chance, &d_int_rand_chance, &o_int_rand_chance, "int_rand_chance");
    ok &= hooks::create(r::fl_rand_chance, &d_fl_rand_chance, &o_fl_rand_chance, "fl_rand_chance");
    ok &= hooks::create(r::vector__rand_sphere, &d_vec_sphere, &o_vec_sphere, "vector::rand_sphere");
    ok &= hooks::create(r::vector4__rand_sphere, &d_vec4_sphere, &o_vec4_sphere, "vector4::rand_sphere");
    ok &= hooks::create(r::vector__rand_around_z, &d_vec_around_z, &o_vec_around_z, "vector::rand_around_z");
    ok &= hooks::create(r::vector4__rand_around_z, &d_vec4_around_z, &o_vec4_around_z, "vector4::rand_around_z");
    ok &= hooks::create(r::timestamp__set_rand, &d_ts_set_rand, &o_ts_set_rand, "timestamp::set_rand");
    ok &= hooks::create(r::timestamp_realtime__set_rand, &d_tsr_set_rand, &o_tsr_set_rand,
                        "timestamp_realtime::set_rand");
    return ok;
}

void set_isolation(bool on) { g_isolate.store(on, std::memory_order_release); }

void reseed(uint32_t seed, bool session_start) {
    AcquireSRWLockExclusive(&g_lock);
    rfg::fn<void(__cdecl*)(uint32_t)>(rfg::rva::rand_regenerate)(seed);  // same seed, same table
    g_seed = seed;
    if (session_start) {
        g_vlib_streams.clear();
    } else {
        // Loading frames: restart the sim thread's streams (how many loading frames run varies); a loader thread
        // keeps its streams, so its draws depend only on its own order and not on when sim frames begin.
        for (auto it = g_vlib_streams.begin(); it != g_vlib_streams.end();)
            it = (it->first & kOtherThread) ? std::next(it) : g_vlib_streams.erase(it);
    }
    g_crt_streams.clear();
    ReleaseSRWLockExclusive(&g_lock);
}

bool crt_rand(uintptr_t caller, int& value) {
    if (!g_isolate.load(std::memory_order_acquire) || g_cells_depth > 0) return false;
    const uint32_t site = uint32_t(caller - rfg::base());
    auto it = g_crt_streams.find(site);
    if (it == g_crt_streams.end()) it = g_crt_streams.emplace(site, mix(g_seed + 0x6A09E667u) ^ mix(site)).first;
    trace(kCrtRand, uint16_t(it->second >> 16), site);
    it->second = it->second * 214013u + 2531011u;  // MSVC rand()
    value = int((it->second >> 16) & 0x7FFF);
    return true;
}

void enter_cells() { ++g_cells_depth; }
void leave_cells() { --g_cells_depth; }

uint32_t vlib_hash() {
    AcquireSRWLockExclusive(&g_lock);
    const uint32_t h = map_hash(g_vlib_streams);
    ReleaseSRWLockExclusive(&g_lock);
    return h;
}

uint32_t crt_hash() { return map_hash(g_crt_streams); }

void set_trace(bool on) {
    if (on && g_trace.capacity() < (1u << 20)) g_trace.reserve(1u << 20);
    g_tracing = on;
}

void set_frame(int32_t frame) { g_frame = frame; }

void write_trace(const std::wstring& path) {
    rec::TextFile f;
    if (f.open(path)) {
        f.write("frame,fn,caller_rva,index\n");
        for (const Entry& e : g_trace) f.printf("%d,%s,%08X,%u\n", e.frame, kNames[e.id], e.caller, e.index);
        f.close();
    }
    LOG("rng: %zu sim-thread draws traced into %ls", g_trace.size(), path.c_str());
    g_trace.clear();
}

std::string other_threads_summary() {
    std::vector<std::pair<uint32_t, uint64_t>> rows;
    {
        std::lock_guard<std::mutex> lock(g_other_mutex);
        for (const auto& [key, n] : g_other) rows.push_back({n, key});
        g_other.clear();
    }
    if (rows.empty()) return "none";
    std::sort(rows.rbegin(), rows.rend());
    std::string out;
    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
        char b[96];
        snprintf(b, sizeof(b), "%s%s@%08X x%u", i ? ", " : "", kNames[rows[i].second >> 32],
                 uint32_t(rows[i].second & 0xFFFFFFFFu), rows[i].first);
        out += b;
    }
    return out;
}

}  // namespace rng_guard
