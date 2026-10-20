#include "physx/bridge.h"

#include <windows.h>

#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"

namespace pxbridge {
namespace {

namespace ipc = rfgphysx;

constexpr uint32_t kRingSize = 4u << 20;         // a burst of 50,000 body creations fits
constexpr uint32_t kControlBytes = 4096;
constexpr uint32_t kQueryBytes = 64u << 10;       // kMaxRays distances
constexpr DWORD kStallMs = 3000;                  // a Ready host whose heartbeat stops this long is ended
// The heartbeat comes from a thread of its own in the host: it says the process lives. Progress is a new snapshot
// (a finished step); cooking a big static world (the open world: 2,000+ bodies) can hold the steps back for seconds.
constexpr DWORD kNoStepMs = 60000;
constexpr DWORD kStartTimeoutMs = 30000;          // PhysX and CUDA start-up

bool g_on = false;
State g_state = State::Off;
HANDLE g_mapping = nullptr, g_process = nullptr, g_job = nullptr;
uint8_t* g_base = nullptr;
ipc::Control* g_ctl = nullptr;
uint8_t* g_ring = nullptr;
uint32_t g_head = 0;
uint32_t g_front = 2;  // the snapshot buffer the game holds (the host starts with 0, latest with 1)
uint32_t g_frame = 0;
const ipc::SnapshotHeader* g_snapshot = nullptr;
bool g_fresh = false;
uint32_t g_last_heartbeat = 0;
DWORD g_heartbeat_tick = 0, g_start_tick = 0;
DWORD g_step_tick = 0;  // when the last fresh snapshot (a finished step) was taken
FrameStats g_stats{};

ipc::SnapshotHeader* snapshot_at(uint32_t index) {
    return reinterpret_cast<ipc::SnapshotHeader*>(g_base + g_ctl->snapshot_offset + size_t(index) * g_ctl->snapshot_stride);
}

void lose(const char* why) {
    if (g_state == State::Lost || g_state == State::Failed) return;
    LOG("PhysX host lost: %s; the game goes on without it", why);
    g_state = State::Lost;
    if (g_process) TerminateProcess(g_process, 1);
}

bool write(const void* record, uint32_t size) {
    if (g_state != State::Starting && g_state != State::Ready) {
        ++g_stats.cmd_dropped;
        return false;
    }
    const uint32_t padded = (size + 7u) & ~7u;
    const uint32_t tail = g_ctl->ring_tail.load(std::memory_order_acquire);
    uint32_t off = g_head & (kRingSize - 1);
    const uint32_t to_end = kRingSize - off;
    const uint32_t need = padded <= to_end ? padded : padded + to_end;
    if (kRingSize - (g_head - tail) < need) {
        ++g_stats.cmd_dropped;
        return false;
    }
    if (padded > to_end) {
        const ipc::CmdHeader pad{uint16_t(ipc::Cmd::Pad), 0, to_end};
        memcpy(g_ring + off, &pad, sizeof(pad));
        g_head += to_end;
        off = 0;
    }
    memcpy(g_ring + off, record, size);
    if (padded > size) memset(g_ring + off + size, 0, padded - size);
    reinterpret_cast<ipc::CmdHeader*>(g_ring + off)->size = padded;
    g_head += padded;
    g_ctl->ring_head.store(g_head, std::memory_order_release);
    g_stats.cmd_bytes += padded;
    return true;
}

template <typename T>
bool send(ipc::Cmd type, T& record) {
    record.h.type = uint16_t(type);
    record.h.reserved = 0;
    record.h.size = sizeof(T);
    return write(&record, sizeof(T));
}

void watch_host() {
    const DWORD now = GetTickCount();
    if (g_process && WaitForSingleObject(g_process, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(g_process, &code);
        if (g_state == State::Starting || g_state == State::Ready) {
            char why[96];
            snprintf(why, sizeof(why), "the host process exited (code %lu)", code);
            lose(why);
        }
        return;
    }
    const auto host_state = ipc::HostState(g_ctl->host_state.load(std::memory_order_acquire));
    if (g_state == State::Starting) {
        if (host_state == ipc::HostState::Ready) {
            g_state = State::Ready;
            g_stats.gpu = uint8_t(g_ctl->host_gpu.load(std::memory_order_relaxed));
            LOG("PhysX host ready after %lu ms: %s (pid %u)", now - g_start_tick, g_ctl->host_message,
                g_ctl->host_pid.load(std::memory_order_relaxed));
            g_last_heartbeat = g_ctl->host_heartbeat.load(std::memory_order_relaxed);
            g_heartbeat_tick = now;
        } else if (host_state == ipc::HostState::Failed) {
            g_state = State::Failed;
            LOG("PhysX host failed: %s", g_ctl->host_message);
        } else if (now - g_start_tick > kStartTimeoutMs) {
            lose("it did not become ready in 30 s");
        }
        return;
    }
    if (g_state != State::Ready) return;
    const uint32_t hb = g_ctl->host_heartbeat.load(std::memory_order_relaxed);
    if (hb != g_last_heartbeat) {
        g_last_heartbeat = hb;
        g_heartbeat_tick = now;
    } else if (now - g_heartbeat_tick > kStallMs) {
        lose("its heartbeat stopped for 3 s");
        return;
    }
    if (!g_step_tick) g_step_tick = now;
    if (now - g_step_tick > kNoStepMs) lose("it finished no step for 60 s");
}

void take_snapshot() {
    g_fresh = false;
    if (g_state != State::Ready) return;
    const int64_t t0 = session::qpc();
    if (g_ctl->snapshot_latest.load(std::memory_order_acquire) & ipc::kFresh) {
        const uint32_t prev = g_ctl->snapshot_latest.exchange(g_front, std::memory_order_acq_rel);
        g_front = prev & ipc::kIndexMask;
        const ipc::SnapshotHeader* s = snapshot_at(g_front);
        g_step_tick = GetTickCount();
        if (s->sequence) {
            g_snapshot = s;
            g_fresh = true;
            ++g_stats.snapshots;
            g_stats.sequence = s->sequence;
            g_stats.age_ms = float(session::ticks_to_ms(t0 - s->publish_qpc));
            g_stats.step_ms = s->step_ms;
            g_stats.work_ms = s->work_ms;
            g_stats.bodies = s->bodies;
            g_stats.awake = s->awake;
            g_stats.frozen = s->frozen;
        }
    }
    g_stats.take_us += float(session::ticks_to_ms(session::qpc() - t0) * 1000.0);
}

// Simulation thread, end of frame (before the consumers registered after it).
void on_frame(int) {
    if (!g_on) return;
    g_ctl->game_heartbeat.store(++g_frame, std::memory_order_relaxed);
    watch_host();
    take_snapshot();
    if (g_state == State::Ready) {
        ipc::Frame f{};
        f.frame = g_frame;
        f.qpc = session::qpc();
        send(ipc::Cmd::Frame, f);
    }
}

void on_exit(bool) {
    if (g_ctl) g_ctl->quit.store(1, std::memory_order_release);
}

std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

}  // namespace

bool install(const Config& cfg) {
    if (cfg.exp_physx <= 0) return true;
    const uint32_t max_bodies = uint32_t(cfg.exp_physx_max_bodies < 1024     ? 1024
                                         : cfg.exp_physx_max_bodies > 262144 ? 262144
                                                                             : cfg.exp_physx_max_bodies);
    const uint32_t stride = ipc::snapshot_stride(max_bodies);
    const uint32_t total = kControlBytes + kRingSize + kQueryBytes + 3 * stride;
    wchar_t name[96];
    swprintf_s(name, L"Local\\rfgphysx-%lu-%lu", GetCurrentProcessId(), GetTickCount());
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, total, name);
    if (g_mapping) g_base = static_cast<uint8_t*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, total));
    if (!g_base) {
        LOG("experiment PhysX: cannot create %u MB of shared memory (error %lu)", total >> 20, GetLastError());
        return true;
    }
    g_ctl = new (g_base) ipc::Control{};
    g_ctl->magic = ipc::kMagic;
    g_ctl->version = ipc::kVersion;
    g_ctl->total_size = total;
    g_ctl->ring_offset = kControlBytes;
    g_ctl->ring_size = kRingSize;
    g_ctl->query_offset = kControlBytes + kRingSize;
    g_ctl->snapshot_offset = kControlBytes + kRingSize + kQueryBytes;
    g_ctl->snapshot_stride = stride;
    g_ctl->max_bodies = max_bodies;
    g_ctl->game_pid = GetCurrentProcessId();
    g_ctl->backend = cfg.exp_physx == 2 ? ipc::kBackendCpu : ipc::kBackendGpu;
    g_ctl->step_hz = float(cfg.exp_physx_rate < 10 ? 10 : cfg.exp_physx_rate > 240 ? 240 : cfg.exp_physx_rate);
    g_ctl->snapshot_latest.store(1, std::memory_order_relaxed);
    g_ring = g_base + kControlBytes;

    const std::wstring exe = session::plugin_dir() + L"\\rfgphysx\\rfgphysx_host.exe";
    std::wstring cmd = quote(exe) + L" --map " + name + L" --log " + quote(session::run_dir() + L"\\physx_host.log");
    if (cfg.telemetry) cmd += L" --steps-csv " + quote(session::run_dir() + L"\\physx_steps.csv");  // ~10 MB an hour
    // The job ends the host when the game's last handle to it closes, that is when the game process ends however it
    // ends. Job objects nest, so this works when the game itself runs in one (launchers, the test queue).
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        nullptr, (session::plugin_dir() + L"\\rfgphysx").c_str(), &si, &pi)) {
        LOG("experiment PhysX: cannot start %ls (error %lu)", exe.c_str(), GetLastError());
        return true;
    }
    if (g_job && !AssignProcessToJobObject(g_job, pi.hProcess))
        LOG("experiment PhysX: the host is not in a job object (error %lu); it ends when it sees the game gone",
            GetLastError());
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_process = pi.hProcess;
    g_on = true;
    g_state = State::Starting;
    g_start_tick = GetTickCount();
    session::add_frame_callback(&on_frame);
    session::add_flush_handler(&on_exit);
    LOG("experiment PhysX: host started (pid %lu, %s backend requested, %.0f Hz, %u body slots, %u MB shared memory)",
        pi.dwProcessId, cfg.exp_physx == 2 ? "CPU" : "GPU", double(g_ctl->step_hz), max_bodies, total >> 20);
    return true;
}

bool active() { return g_state == State::Starting || g_state == State::Ready; }
bool ready() { return g_state == State::Ready; }
uint32_t max_bodies() { return g_ctl ? g_ctl->max_bodies : 0; }
float step_hz() { return g_ctl ? g_ctl->step_hz : 60.0f; }

uint64_t host_cpu_100ns() {
    FILETIME created, exited, kernel, user;
    if (!g_process || !GetProcessTimes(g_process, &created, &exited, &kernel, &user)) return 0;
    return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
           ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
}

bool reset() {
    ipc::CmdHeader h{uint16_t(ipc::Cmd::Reset), 0, sizeof(ipc::CmdHeader)};
    return write(&h, sizeof(h));
}

bool add_static_plane(const ipc::Vec3& normal, float distance) {
    ipc::StaticPlane c{};
    c.normal = normal;
    c.distance = distance;
    return send(ipc::Cmd::AddStaticPlane, c);
}

bool add_static_box(const ipc::Vec3& pos, const ipc::Quat& rot, const ipc::Vec3& half_extents) {
    ipc::StaticBox c{};
    c.pos = pos;
    c.rot = rot;
    c.half_extents = half_extents;
    return send(ipc::Cmd::AddStaticBox, c);
}

bool shape_box(uint32_t shape, const ipc::Vec3& half_extents) {
    ipc::ShapeBox c{};
    c.shape = shape;
    c.half_extents = half_extents;
    return send(ipc::Cmd::ShapeBox, c);
}

bool shape_convex(uint32_t shape, const ipc::Vec3* vertices, uint32_t count) {
    if (count < 4 || count > 255) return false;
    std::vector<uint8_t> buf(sizeof(ipc::ShapeConvex) + count * sizeof(ipc::Vec3));
    auto* c = reinterpret_cast<ipc::ShapeConvex*>(buf.data());
    c->h = {uint16_t(ipc::Cmd::ShapeConvex), 0, uint32_t(buf.size())};
    c->shape = shape;
    c->vertex_count = count;
    memcpy(c + 1, vertices, count * sizeof(ipc::Vec3));
    return write(buf.data(), uint32_t(buf.size()));
}

bool create_body(uint32_t slot, uint32_t shape, const ipc::Vec3& pos, const ipc::Quat& rot, const ipc::Vec3& lin_vel,
                 const ipc::Vec3& ang_vel, float density, uint32_t flags) {
    ipc::CreateBody c{};
    c.slot = slot;
    c.shape = shape;
    c.pos = pos;
    c.rot = rot;
    c.lin_vel = lin_vel;
    c.ang_vel = ang_vel;
    c.density = density;
    c.flags = flags;
    return send(ipc::Cmd::CreateBody, c);
}

bool destroy_body(uint32_t slot) {
    ipc::DestroyBody c{};
    c.slot = slot;
    return send(ipc::Cmd::DestroyBody, c);
}

bool explosion(const ipc::Vec3& center, float radius, float impulse, float up) {
    ipc::Explosion c{};
    c.center = center;
    c.radius = radius;
    c.impulse = impulse;
    c.up = up;
    return send(ipc::Cmd::Explosion, c);
}

bool test_crash() {
    ipc::CmdHeader h{uint16_t(ipc::Cmd::TestCrash), 0, sizeof(ipc::CmdHeader)};
    return write(&h, sizeof(h));
}

bool send_record(const void* record, uint32_t size) { return write(record, size); }

uint32_t ring_used() {
    if (!g_ctl || (g_state != State::Starting && g_state != State::Ready)) return 0;
    return g_head - g_ctl->ring_tail.load(std::memory_order_acquire);
}

uint32_t ring_free() {
    if (!g_ctl || (g_state != State::Starting && g_state != State::Ready)) return 0;
    const uint32_t used = g_head - g_ctl->ring_tail.load(std::memory_order_acquire);
    const uint32_t to_end = kRingSize - (g_head & (kRingSize - 1));
    const uint32_t free = kRingSize - used;
    // The largest record that fits: records never straddle the end of the ring (a pad record fills it first).
    const uint32_t here = free < to_end ? free : to_end;
    const uint32_t wrapped = free > to_end ? free - to_end : 0;
    return here > wrapped ? here : wrapped;
}

bool raycast_batch(uint32_t request, const ipc::Ray* rays, uint32_t count) {
    if (count > ipc::kMaxRays) count = ipc::kMaxRays;
    std::vector<uint8_t> buf(sizeof(ipc::RaycastBatch) + size_t(count) * sizeof(ipc::Ray));
    auto* c = reinterpret_cast<ipc::RaycastBatch*>(buf.data());
    c->h = {uint16_t(ipc::Cmd::RaycastBatch), 0, uint32_t(buf.size())};
    c->request = request;
    c->count = count;
    memcpy(c + 1, rays, size_t(count) * sizeof(ipc::Ray));
    return write(buf.data(), uint32_t(buf.size()));
}

uint32_t query_done() { return g_ctl ? g_ctl->query_done.load(std::memory_order_acquire) : 0; }
const float* query_results() { return g_ctl ? reinterpret_cast<const float*>(g_base + g_ctl->query_offset) : nullptr; }
const uint32_t* query_hit_kinds() {
    return g_ctl ? reinterpret_cast<const uint32_t*>(g_base + g_ctl->query_offset + ipc::kMaxRays * sizeof(float)) : nullptr;
}
uint32_t host_statics() { return g_ctl ? g_ctl->statics.load(std::memory_order_relaxed) : 0; }

const ipc::SnapshotHeader* snapshot(bool* fresh) {
    if (fresh) *fresh = g_fresh;
    return g_state == State::Ready ? g_snapshot : nullptr;
}

FrameStats take_frame() {
    FrameStats f = g_stats;
    f.state = g_state;
    g_stats.snapshots = 0;
    g_stats.take_us = 0;
    g_stats.cmd_bytes = 0;
    g_stats.cmd_dropped = 0;
    return f;
}

}  // namespace pxbridge
