#include "telemetry/render_timeline.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"
#include "telemetry/telemetry.h"

namespace render_timeline {
namespace {

constexpr uint32_t kSceneStartEvent = 0x4BB24, kSceneDoneEvent = 0x4BB28;  // rl_scene_renderer members
constexpr uint32_t kAppHandoffEvent = 0x64C;  // keen::rfg::Application: the sim thread finished the handoff

struct Row {
    uint32_t render_frame;
    uint32_t sim_frame;
    int32_t state;
    float t_ms;                   // start of the render frame (do_game_render_frame), ms since plugin start
    float frame_ms;               // do_game_render_frame
    float scene_ms;               // rl_scene_renderer::render
    float before_kick_ms;         // scene render start -> scene thread woken
    float rt_parallel_ms;         // woken -> the render thread starts waiting for the scene thread
    float join_wait_ms;           // the render thread waiting for the scene thread
    float scene_thread_ms;        // the scene thread's part (starts until done)
    float scene_thread_delay_ms;  // woken -> the scene thread starts
    float shadows_ms;             // rl_scene_renderer::clb_render (render thread)
    float gpu_vis_ms;             // rl_gpu_vis_render (render thread)
    float sim_wait_ms;            // the render thread waiting for the sim thread's handoff
    uint32_t draws_render, draws_scene;
};

std::vector<Row> g_rows;
SRWLOCK g_rows_lock = SRWLOCK_INIT;
uint32_t g_render_frame = 0;

std::atomic<uint8_t*> g_scene{nullptr};  // the rl_scene_renderer (learned from its render call)
std::atomic<DWORD> g_render_tid{0}, g_scene_tid{0};

// Render thread, current render frame.
double r_frame_start_ms = 0;
int64_t r_frame_start = 0, r_scene_start = 0, r_scene_end = 0, r_kick = 0, r_join_start = 0, r_join_end = 0;
double r_shadows_ms = 0, r_gpu_vis_ms = 0, r_sim_wait_ms = 0;
// The render thread waits for the sim thread's handoff (keen::rfg::Application::render) just before each render
// frame starts: collected here and counted in the frame that follows.
double r_pending_sim_wait_ms = 0;
// Scene thread: its last part, written before it signals done (the render thread reads it after the join).
int64_t s_start = 0, s_end = 0;

inline double ms(int64_t ticks) { return session::ticks_to_ms(ticks); }
inline uintptr_t app_instance() { return uintptr_t(rfg::addr(rfg::rva::s_appInstance)); }

void(__fastcall* o_scene_render)(void*, void*, void*, void*, void*, void*, void*, void*);
void __fastcall d_scene_render(void* scene, void* edx, void* a, void* b, void* c, void* d, void* e, void* f) {
    g_scene.store(static_cast<uint8_t*>(scene), std::memory_order_relaxed);
    g_render_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    r_scene_start = session::qpc();
    o_scene_render(scene, edx, a, b, c, d, e, f);
    r_scene_end = session::qpc();
}

void(__fastcall* o_wait)(uint8_t*, void*);
void __fastcall d_wait(uint8_t* ev, void* edx) {
    uint8_t* scene = g_scene.load(std::memory_order_relaxed);
    const bool scene_start = scene && ev == scene + kSceneStartEvent;
    const bool scene_done = scene && ev == scene + kSceneDoneEvent;
    const bool handoff = uintptr_t(ev) == app_instance() + kAppHandoffEvent;
    if (!scene_start && !scene_done && !handoff) {
        o_wait(ev, edx);
        return;
    }
    const int64_t t0 = session::qpc();
    o_wait(ev, edx);
    const int64_t t1 = session::qpc();
    if (scene_start) {  // the scene thread is woken: its part of the frame begins
        g_scene_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        s_start = t1;
    } else if (scene_done) {
        r_join_start = t0;
        r_join_end = t1;
    } else if (GetCurrentThreadId() == g_render_tid.load(std::memory_order_relaxed)) {
        r_pending_sim_wait_ms += ms(t1 - t0);
    }
}

void(__fastcall* o_signal)(uint8_t*, void*);
void __fastcall d_signal(uint8_t* ev, void* edx) {
    uint8_t* scene = g_scene.load(std::memory_order_relaxed);
    if (scene && ev == scene + kSceneStartEvent) {
        r_kick = session::qpc();
    } else if (scene && ev == scene + kSceneDoneEvent) {
        s_end = session::qpc();
    }
    o_signal(ev, edx);
}

void(__fastcall* o_clb_render)(void*, void*, void*, void*, void*, void*, void*, void*, int);
void __fastcall d_clb_render(void* scene, void* edx, void* a, void* b, void* c, void* d, void* e, void* f, int g) {
    const int64_t t0 = session::qpc();
    o_clb_render(scene, edx, a, b, c, d, e, f, g);
    r_shadows_ms += ms(session::qpc() - t0);
}

void(__cdecl* o_gpu_vis)(void*, void*);
void __cdecl d_gpu_vis(void* renderer, void* camera) {
    const int64_t t0 = session::qpc();
    o_gpu_vis(renderer, camera);
    r_gpu_vis_ms += ms(session::qpc() - t0);
}

void write(bool at_exit) {
    if (at_exit) {
        if (!TryAcquireSRWLockExclusive(&g_rows_lock)) return;
    } else {
        AcquireSRWLockExclusive(&g_rows_lock);
    }
    const std::wstring path = session::run_dir() + L"\\render_timeline.csv";
    if (FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR)) {
        fputs("render_frame,sim_frame,state,t_ms,frame_ms,scene_ms,before_kick_ms,rt_parallel_ms,join_wait_ms,"
              "scene_thread_ms,scene_thread_delay_ms,shadows_ms,gpu_vis_ms,sim_wait_ms,draws_render,draws_scene\n", f);
        for (const Row& r : g_rows)
            fprintf(f, "%u,%u,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u\n", r.render_frame,
                    r.sim_frame, r.state, r.t_ms, r.frame_ms, r.scene_ms, r.before_kick_ms, r.rt_parallel_ms,
                    r.join_wait_ms, r.scene_thread_ms, r.scene_thread_delay_ms, r.shadows_ms, r.gpu_vis_ms,
                    r.sim_wait_ms, r.draws_render, r.draws_scene);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&g_rows_lock);
}

}  // namespace

bool install() {
    namespace r = rfg::rva;
    bool ok = hooks::create(r::rl_scene_renderer__render, &d_scene_render, &o_scene_render, "rl_scene_renderer::render");
    ok &= hooks::create(r::keen__Event__sleepUntilSignaled, &d_wait, &o_wait, "keen::Event::sleepUntilSignaled");
    ok &= hooks::create(r::keen__Event__signal, &d_signal, &o_signal, "keen::Event::signal");
    ok &= hooks::create(r::rl_scene_renderer__clb_render, &d_clb_render, &o_clb_render, "rl_scene_renderer::clb_render");
    ok &= hooks::create(r::rl_gpu_vis_render, &d_gpu_vis, &o_gpu_vis, "rl_gpu_vis_render");
    g_rows.reserve(40000);
    session::add_flush_handler(&write);
    return ok;
}

unsigned long scene_thread_id() { return g_scene_tid.load(std::memory_order_relaxed); }

void begin_render_frame() {
    r_frame_start_ms = session::since_start_ms();
    r_frame_start = session::qpc();
    r_scene_start = r_scene_end = r_kick = r_join_start = r_join_end = 0;
    r_shadows_ms = r_gpu_vis_ms = 0;
    r_sim_wait_ms = r_pending_sim_wait_ms;
    r_pending_sim_wait_ms = 0;
}

void end_render_frame(uint32_t draws_render, uint32_t draws_scene) {
    const int64_t now = session::qpc();
    Row row{};
    row.render_frame = g_render_frame++;
    row.sim_frame = telemetry::current_frame();
    row.state = telemetry::current_state();
    row.t_ms = float(r_frame_start_ms);
    row.frame_ms = float(ms(now - r_frame_start));
    if (r_scene_start && r_scene_end) row.scene_ms = float(ms(r_scene_end - r_scene_start));
    if (r_kick && r_scene_start) row.before_kick_ms = float(ms(r_kick - r_scene_start));
    if (r_join_start && r_kick) row.rt_parallel_ms = float(ms(r_join_start - r_kick));
    if (r_join_start) row.join_wait_ms = float(ms(r_join_end - r_join_start));
    if (s_start && s_end > s_start && r_kick) {
        row.scene_thread_ms = float(ms(s_end - s_start));
        row.scene_thread_delay_ms = float(ms(s_start - r_kick));
    }
    row.shadows_ms = float(r_shadows_ms);
    row.gpu_vis_ms = float(r_gpu_vis_ms);
    row.sim_wait_ms = float(r_sim_wait_ms);
    row.draws_render = draws_render;
    row.draws_scene = draws_scene;
    s_start = s_end = 0;
    AcquireSRWLockExclusive(&g_rows_lock);
    g_rows.push_back(row);
    ReleaseSRWLockExclusive(&g_rows_lock);
}

}  // namespace render_timeline
