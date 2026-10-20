#include "physx/render_routing.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace render_routing {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// rl_destroyable_model_instance's vtable (its constructor stores it); checked against slot 1, its release.
constexpr uint32_t kDestroyableVtable = 0xEC0F54;
constexpr uint32_t kInstFlags = 0x40, kInstSortMethod = 0x100;

enum Fn { kBatch, kDeferred, kRender, kRenderDeferred, kFnCount };
const char* const kFnNames[kFnCount] = {"add_to_batch", "add_to_deferred", "render", "render_for_deferred"};

bool g_on = false;
uintptr_t g_vtable = 0;

struct Site {
    std::atomic<uint32_t> rva{0};
    std::atomic<uint32_t> n{0};
};
constexpr int kSites = 32;
struct Counters {  // a static: zero-initialised
    std::atomic<uint32_t> calls[2][kFnCount];                               // [ours][fn]
    std::atomic<uint32_t> bit5[kFnCount], bit7[kFnCount], sort[kFnCount];  // ours, by flag
    Site sites[kFnCount][kSites];                                          // ours, by call site
};
Counters g_c;

// The offload's pieces: two sorted copies, the published one searched by the render threads.
struct Pieces {
    std::vector<const void*> v;
};
Pieces g_buf[2];
std::atomic<const Pieces*> g_pieces{nullptr};
int g_next_buf = 0;

bool is_ours(const void* inst) {
    const Pieces* p = g_pieces.load(std::memory_order_acquire);
    return p && std::binary_search(p->v.begin(), p->v.end(), inst);
}

void note_site(Fn fn, uint32_t rva) {
    Site* sites = g_c.sites[fn];
    for (int i = 0; i < kSites; ++i) {
        uint32_t cur = sites[i].rva.load(std::memory_order_relaxed);
        if (cur == 0 && sites[i].rva.compare_exchange_strong(cur, rva)) cur = rva;
        if (cur == rva) {
            sites[i].n.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void count(Fn fn, const void* self, void* ret) {
    if (!g_on || *static_cast<const uintptr_t*>(self) != g_vtable) return;
    const bool ours = is_ours(self);
    g_c.calls[ours][fn].fetch_add(1, std::memory_order_relaxed);
    if (!ours) return;
    const uint16_t flags = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(self) + kInstFlags);
    if (flags & 0x20) g_c.bit5[fn].fetch_add(1, std::memory_order_relaxed);
    if (flags & 0x80) g_c.bit7[fn].fetch_add(1, std::memory_order_relaxed);
    if (*reinterpret_cast<void* const*>(static_cast<const uint8_t*>(self) + kInstSortMethod))
        g_c.sort[fn].fetch_add(1, std::memory_order_relaxed);
    note_site(fn, uint32_t(reinterpret_cast<uintptr_t>(ret) - rfg::base()));
}

// __thiscall with N stack arguments, hooked as __fastcall (this in ecx, edx unused).
using Fn3 = void(__fastcall*)(void*, void*, void*, void*, void*);
using Fn2 = void(__fastcall*)(void*, void*, void*, void*);
using Fn4 = void(__fastcall*)(void*, void*, void*, void*, void*, void*);
Fn3 o_batch = nullptr, o_render = nullptr;
Fn2 o_deferred = nullptr;
Fn4 o_render_deferred = nullptr;

void __fastcall d_batch(void* self, void* edx, void* a, void* b, void* c) {
    count(kBatch, self, _ReturnAddress());
    o_batch(self, edx, a, b, c);
}
void __fastcall d_deferred(void* self, void* edx, void* a, void* b) {
    count(kDeferred, self, _ReturnAddress());
    o_deferred(self, edx, a, b);
}
void __fastcall d_render(void* self, void* edx, void* a, void* b, void* c) {
    count(kRender, self, _ReturnAddress());
    o_render(self, edx, a, b, c);
}
void __fastcall d_render_deferred(void* self, void* edx, void* a, void* b, void* c, void* d) {
    count(kRenderDeferred, self, _ReturnAddress());
    o_render_deferred(self, edx, a, b, c, d);
}

uint32_t g_frames = 0;
double g_window_start = 0;

void report() {
    const double f = g_frames ? double(g_frames) : 1.0;
    std::string line;
    for (int fn = 0; fn < kFnCount; ++fn) {
        const uint32_t ours = g_c.calls[1][fn].exchange(0), other = g_c.calls[0][fn].exchange(0);
        const uint32_t b5 = g_c.bit5[fn].exchange(0), b7 = g_c.bit7[fn].exchange(0), so = g_c.sort[fn].exchange(0);
        char buf[200];
        snprintf(buf, sizeof(buf), "%s%s: debris %.0f (bit7 %.0f, bit5 %.0f, sort %.0f), other destroyables %.0f",
                 fn ? "; " : "", kFnNames[fn], ours / f, b7 / f, b5 / f, so / f, other / f);
        line += buf;
    }
    LOG("render routing, per frame over %u frames: %s", g_frames, line.c_str());
    for (int fn = 0; fn < kFnCount; ++fn) {
        std::vector<std::pair<uint32_t, uint32_t>> sites;
        for (Site& s : g_c.sites[fn]) {
            const uint32_t n = s.n.exchange(0), rva = s.rva.load();
            if (n) sites.emplace_back(n, rva);
        }
        if (sites.empty()) continue;
        std::sort(sites.rbegin(), sites.rend());
        std::string text;
        for (size_t i = 0; i < sites.size() && i < 6; ++i) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%srfg+0x%X %.0f", i ? ", " : "", sites[i].second, sites[i].first / f);
            text += buf;
        }
        LOG("render routing:   debris %s called from %s", kFnNames[fn], text.c_str());
    }
    g_frames = 0;
}

void on_frame(int game_state) {
    if (game_state != kGameplay) return;
    const double now = session::since_start_ms();
    if (!g_window_start) g_window_start = now;
    ++g_frames;
    if (now - g_window_start >= 5000) {
        report();
        g_window_start = now;
    }
}

}  // namespace

bool install(const Config& cfg) {
    if (!cfg.render_routing) return true;
    g_vtable = rfg::base() + kDestroyableVtable;
    const uintptr_t release = static_cast<const uintptr_t*>(reinterpret_cast<const void*>(g_vtable))[1];
    if (release != rfg::base() + r::rl_destroyable_model_instance__release) {
        LOG("render routing: unexpected rl_destroyable_model_instance vtable; diagnostic off");
        return true;
    }
    bool ok = hooks::create(r::rl_compound_mesh_instance__add_to_batch, &d_batch, &o_batch, "add_to_batch");
    ok &= hooks::create(r::rl_compound_mesh_instance__add_to_deferred, &d_deferred, &o_deferred, "add_to_deferred");
    ok &= hooks::create(r::rl_destroyable_model_instance__render, &d_render, &o_render, "destroyable render");
    ok &= hooks::create(r::rl_compound_mesh_instance__render_for_deferred, &d_render_deferred, &o_render_deferred,
                        "render_for_deferred");
    if (!ok) return false;
    g_on = true;
    session::add_frame_callback(&on_frame);
    LOG("render routing diagnostic armed: destroyable-model render calls are counted (debris pieces vs the rest)");
    return true;
}

bool enabled() { return g_on; }

void publish_pieces(std::vector<const void*>&& sorted) {
    Pieces& p = g_buf[g_next_buf];
    p.v = std::move(sorted);
    g_pieces.store(&p, std::memory_order_release);
    g_next_buf ^= 1;
}

}  // namespace render_routing
