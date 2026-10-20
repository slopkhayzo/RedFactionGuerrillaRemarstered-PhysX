#include "experiments/havok_mt.h"

#include <windows.h>
#include <intrin.h>
#include <malloc.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/contacts.h"
#include "core/game_lock.h"
#include "core/gameloop.h"
#include "core/log.h"
#include "core/session.h"
#include "core/thread_dump.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace experiments {
namespace {

namespace r = rfg::rva;

// hkpWorldCinfo fields (PDB layout).
constexpr uint32_t kCinfoMtPostponeBorderCallbacks = 0x2A;  // m_mtPostponeAndSortBroadPhaseBorderCallbacks
constexpr uint32_t kCinfoProcessToisMultithreaded = 0xB8;
constexpr uint32_t kCinfoSimulationType = 0xD9;
constexpr uint32_t kCinfoProcessActionsInSingleThread = 0xE0;
constexpr uint32_t kCinfoMinDesiredIslandSize = 0xDC;
constexpr uint8_t kSimulationContinuous = 2, kSimulationMultithreaded = 3;
// hkpWorld: m_broadPhase, m_modifyConstraintCriticalSection, m_islandDirtyListCriticalSection, m_propertyMasterLock
// (hkCriticalSection*, a CRITICAL_SECTION at offset 0; the three locks exist in multithreaded worlds only),
// m_simulationType (int). hkpSimulation::m_world. hkpBroadPhase's lock (hkCriticalSection*).
constexpr uint32_t kWorldBroadPhase = 0x58;
constexpr uint32_t kWorldModifyConstraintLock = 0xB4, kWorldIslandDirtyListLock = 0xBC, kWorldPropertyLock = 0xC0;
constexpr uint32_t kWorldSimulationType = 0xF4;
constexpr uint32_t kWorldMinDesiredIslandSize = 0xB0;
constexpr uint32_t kSimWorld = 0x0C;
constexpr uint32_t kBroadPhaseLock = 0x1C;
// hkpMultiThreadedSimulation's own locks (hkCriticalSection members), and
// m_multithreadConfig.m_maxNumConstraintsSolvedSingleThreaded (default 70).
constexpr uint32_t kSimAddPairLock = 0x78, kSimRemovePairLock = 0x9C, kSimToiQueueLock = 0x100, kSimPhantomLock = 0x140;
constexpr uint32_t kSimMaxConstraintsSolvedSingleThreaded = 0xB4;
constexpr uint32_t kWorldSimulation = 0x08;  // hkpWorld::m_simulation
// hkJobQueue (0x210 bytes, its lock at 0, DynamicData* at 0x18); hkJobQueueCinfo is an hkJobQueueHwSetup
// (m_numCpuThreads at +0xC) then m_maxNumJobTypes.
constexpr size_t kQueueSize = 0x210;
constexpr uint32_t kQueueData = 0x18;
constexpr uint32_t kQueueJobFuncs = 0x10C;  // m_jobFuncs[hkJobType]
constexpr size_t kQueueCinfoSize = 0x20;
constexpr uint32_t kCinfoNumCpuThreads = 0x0C;
constexpr uint32_t kCinfoMaxNumJobTypes = 0x1C;
constexpr int kJobTypeDynamics = 0, kJobTypeCollide = 1, kJobTypeMax = 15;  // hkJobType
constexpr int kNumJobSubTypes = 17;  // hkpDynamicsJob::DYNAMICS_JOB_END (collide jobs use the same subtypes)
constexpr int kMaxThreads = 12;      // hkJobQueue::m_cpuThreadIndexToSemaphoreIndex: Havok thread numbers 0..11
// hkReferencedObjectLock (hkReferencedObject::lockAll, taken by hkpWorld::lock): count, then its lock at +0x1C.
constexpr uint32_t kRefLockCount = 0x0C, kRefLockSection = 0x1C;
// hkFreeListMemorySystem::m_solverAllocator (+0x94), whose lock is at +0x224.
constexpr uint32_t kSolverAllocatorLock = 0x94 + 0x224;
constexpr SIZE_T kWorkerStack = 2 << 20;
constexpr double kHangMs = 5000;  // a step this long is a deadlock (heavy steps take a few ms)

struct JobHandlerFuncs {    // hkJobQueue::hkJobHandlerFuncs
    void* const* process;   // JobStatus (*)(hkJobQueue&, JobQueueEntry&), indexed by job subtype
    int num_process;
    void* pop;
    void* finish;
};

// __thiscall functions are called through __fastcall (ecx = this, edx unused).
using StepBegin = int(__fastcall*)(void* sim, void* edx, void* queue, float dt);
using StepFinish = int(__fastcall*)(void* sim, void* edx, void* queue, void* thread_pool);
using ProcessAllJobs = int(__fastcall*)(void* queue, void* edx, bool add_timers);
using QueueCtor = void*(__fastcall*)(void* queue, void* edx, const void* cinfo);
using HwSetupCtor = void*(__fastcall*)(void* setup, void* edx);
using WorldLockFn = void(__fastcall*)(void* world, void* edx);
using CreateRouter = void*(__cdecl*)();
using InitThread = int*(__cdecl*)(int* result, void* router);  // hkResult returned through a hidden pointer

int g_workers = 0;                 // worker threads running
int g_requested = 0;               // Experiment.HavokThreads
int g_min_island_size = 0;         // Experiment.HavokMinIslandSize (-1: Havok's default)
uint8_t* g_world = nullptr;        // the game's multithreaded world
void* g_queue = nullptr;           // hkJobQueue, built on the sim thread at the first multithreaded step
bool g_queue_failed = false;
void* g_process_funcs[kNumJobSubTypes];
HANDLE g_start[kMaxThreads];       // per worker (index = Havok thread number), auto-reset
DWORD g_worker_tid[kMaxThreads];
HANDLE g_done = nullptr;           // set by the last worker to finish a step's jobs
HANDLE g_ready = nullptr;          // worker start-up handshake
std::atomic<int> g_pending{0};
std::atomic<bool> g_in_jobs{false};
std::atomic<int64_t> g_step_start{0};  // qpc of the running multithreaded step, 0 between steps
uint8_t* g_step_sim = nullptr;
unsigned g_mxcsr = 0;              // the sim thread's SSE control word, copied into the workers at every step
double g_frame_jobs_ms = 0;        // sim thread
uint64_t g_steps = 0, g_steps_alone = 0;

ProcessAllJobs process_all_jobs() { return rfg::fn<ProcessAllJobs>(r::hkJobQueue__processAllJobs); }

// ---- hang report ----------------------------------------------------------------------------------------------

DWORD owner_of(const void* critical_section) {
    if (!critical_section) return 0;
    return DWORD(reinterpret_cast<uintptr_t>(static_cast<const RTL_CRITICAL_SECTION*>(critical_section)->OwningThread));
}

void log_lock(const char* name, const void* critical_section, std::vector<DWORD>& owners) {
    if (!critical_section) {
        LOG("hang:   lock %-32s none", name);
        return;
    }
    const auto* cs = static_cast<const RTL_CRITICAL_SECTION*>(critical_section);
    const DWORD owner = owner_of(cs);
    LOG("hang:   lock %-32s owner %5lu, recursion %ld, lock count %ld", name, owner, cs->RecursionCount, cs->LockCount);
    if (owner && std::find(owners.begin(), owners.end(), owner) == owners.end()) owners.push_back(owner);
}

const void* ptr_at(const uint8_t* base, uint32_t offset) {
    return base ? *reinterpret_cast<void* const*>(base + offset) : nullptr;
}

void report_hang(double ms) {
    LOG("hang: a multithreaded Havok step has run for %.0f ms: deadlock. Workers still in the step: %d of %d", ms,
        g_pending.load(), g_workers);
    std::vector<DWORD> owners;
    const uint8_t* world = g_world;
    const uint8_t* sim = g_step_sim;
    log_lock("game callbacks (world constraint)", ptr_at(world, kWorldModifyConstraintLock), owners);
    log_lock("world island dirty list", ptr_at(world, kWorldIslandDirtyListLock), owners);
    log_lock("world properties", ptr_at(world, kWorldPropertyLock), owners);
    const uint8_t* broadphase = static_cast<const uint8_t*>(ptr_at(world, kWorldBroadPhase));
    log_lock("broadphase", ptr_at(broadphase, kBroadPhaseLock), owners);
    if (sim) {
        log_lock("simulation add cross-island pair", sim + kSimAddPairLock, owners);
        log_lock("simulation remove cross-island pair", sim + kSimRemovePairLock, owners);
        log_lock("simulation TOI queue", sim + kSimToiQueueLock, owners);
        log_lock("simulation phantom", sim + kSimPhantomLock, owners);
    }
    const uint8_t* ref_lock = rfg::global<uint8_t*>(r::hkSingleton_hkReferencedObjectLock___s_instance);
    if (ref_lock) {
        LOG("hang:   reference lock count %d", *reinterpret_cast<const int*>(ref_lock + kRefLockCount));
        log_lock("reference lock (hkpWorld::lock)", ref_lock + kRefLockSection, owners);
    }
    if (const uint8_t* memory = rfg::global<uint8_t*>(r::Havok_memory))
        log_lock("solver allocator", memory + kSolverAllocatorLock, owners);
    if (g_queue) {
        const auto* q = static_cast<const uint8_t*>(g_queue);
        log_lock("job queue", q, owners);
        if (const auto* data = static_cast<const uint8_t*>(ptr_at(q, kQueueData))) {
            const auto* active = reinterpret_cast<const uint16_t*>(data);
            const auto* waiting = reinterpret_cast<const uint16_t*>(data + 0x2A);
            LOG("hang:   job queue: active jobs dynamics %u collide %u, finishing flags %08X, wait policy %d, waiting "
                "threads %u %u, queued jobs %d %d %d %d %d %d",
                active[kJobTypeDynamics], active[kJobTypeCollide], *reinterpret_cast<const uint32_t*>(data + 0x20),
                *reinterpret_cast<const int*>(data + 0x24), waiting[0], waiting[1],
                *reinterpret_cast<const int*>(data + 0x34 + 0x10), *reinterpret_cast<const int*>(data + 0x48 + 0x10),
                *reinterpret_cast<const int*>(data + 0x5C + 0x10), *reinterpret_cast<const int*>(data + 0x70 + 0x10),
                *reinterpret_cast<const int*>(data + 0x84 + 0x10), *reinterpret_cast<const int*>(data + 0x98 + 0x10));
        }
    }
    std::vector<DWORD> shown;
    const DWORD sim_tid = gameloop::sim_thread_id();
    thread_dump::log_thread(sim_tid, "sim, Havok thread 0");
    shown.push_back(sim_tid);
    for (int i = 1; i <= g_workers; ++i) {
        char label[32];
        snprintf(label, sizeof(label), "worker, Havok thread %d", i);
        thread_dump::log_thread(g_worker_tid[i], label);
        shown.push_back(g_worker_tid[i]);
    }
    for (DWORD owner : owners) {
        if (std::find(shown.begin(), shown.end(), owner) != shown.end()) continue;
        thread_dump::log_thread(owner, "lock owner");
        shown.push_back(owner);
    }
}

// Ends the process when a multithreaded step deadlocks, so unattended runs never hang (the log says where).
DWORD WINAPI watch_main(void*) {
    SetThreadDescription(GetCurrentThread(), L"rfgphys havok watch");
    for (;;) {
        Sleep(250);
        const int64_t start = g_step_start.load(std::memory_order_acquire);
        if (!start) continue;
        const double ms = session::ticks_to_ms(session::qpc() - start);
        if (ms < kHangMs) continue;
        report_hang(ms);
        session::exit_now("experiment HavokThreads: a multithreaded Havok step deadlocked (hang report in the log)");
    }
}

// ---- workers and the job queue --------------------------------------------------------------------------------

// Havok's worker loop (hkCpuThreadPool does the same): Havok thread number, thread memory, then processAllJobs once
// per step until every job of the step is finished.
DWORD WINAPI worker_main(void* param) {
    const int index = int(reinterpret_cast<intptr_t>(param));
    wchar_t name[48];
    swprintf_s(name, L"rfgphys havok worker %d", index);
    SetThreadDescription(GetCurrentThread(), name);
    TlsSetValue(rfg::global<DWORD>(r::hkThreadNumber), reinterpret_cast<void*>(intptr_t(index)));
    void* router = rfg::fn<CreateRouter>(r::havok_create_thread_memory_router)();
    int result = 0;
    rfg::fn<InitThread>(r::hkBaseSystem__initThread)(&result, router);
    SetEvent(g_ready);
    const ProcessAllJobs process = process_all_jobs();
    for (;;) {
        WaitForSingleObject(g_start[index], INFINITE);
        _mm_setcsr(g_mxcsr);
        process(g_queue, nullptr, false);
        if (g_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) SetEvent(g_done);
    }
}

// On the sim thread (the job queue's constructor makes the calling thread Havok thread 0, the master).
bool init_queue() {
    alignas(16) uint8_t cinfo[kQueueCinfoSize] = {};
    rfg::fn<HwSetupCtor>(r::hkJobQueueHwSetup__hkJobQueueHwSetup)(cinfo, nullptr);  // no SPUs, no shared caches
    *reinterpret_cast<int*>(cinfo + kCinfoNumCpuThreads) = g_requested + 1;
    *reinterpret_cast<int*>(cinfo + kCinfoMaxNumJobTypes) = kJobTypeMax;
    void* queue = _aligned_malloc(kQueueSize, 16);
    if (!queue) return false;
    memset(queue, 0, kQueueSize);
    rfg::fn<QueueCtor>(r::hkJobQueue__hkJobQueue)(queue, nullptr, cinfo);

    // hkpWorld::registerWithJobQueue: processNextJob dispatches every dynamics and collide job by subtype; the queue
    // asks hkpJobQueueUtils how to split and continue them.
    std::fill(std::begin(g_process_funcs), std::end(g_process_funcs),
              rfg::addr(r::hkpMultiThreadedSimulation__processNextJob));
    auto* funcs = reinterpret_cast<JobHandlerFuncs*>(static_cast<uint8_t*>(queue) + kQueueJobFuncs);
    funcs[kJobTypeDynamics] = {g_process_funcs, kNumJobSubTypes, rfg::addr(r::hkpJobQueueUtils__popIntegrateJob),
                               rfg::addr(r::hkpJobQueueUtils__finishIntegrateJob)};
    funcs[kJobTypeCollide] = {g_process_funcs, kNumJobSubTypes, rfg::addr(r::hkpJobQueueUtils__popCollideJob),
                              rfg::addr(r::hkpJobQueueUtils__finishCollideJob)};
    g_queue = queue;

    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const int priority = GetThreadPriority(GetCurrentThread());
    for (int i = 1; i <= g_requested; ++i) {
        g_start[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DWORD tid = 0;
        HANDLE thread = CreateThread(nullptr, kWorkerStack, &worker_main, reinterpret_cast<void*>(intptr_t(i)),
                                     STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, &tid);
        if (!thread) {
            LOG("experiment HavokThreads: could not start worker %d (error %lu)", i, GetLastError());
            break;
        }
        SetThreadPriority(thread, priority);
        ResumeThread(thread);
        // One at a time, with this thread waiting: each worker sets up its Havok thread memory (the game does the
        // same under a lock for its scheduler threads, havok_setup_thread_job).
        WaitForSingleObject(g_ready, INFINITE);
        CloseHandle(thread);
        g_worker_tid[i] = tid;
        g_workers = i;
    }
    CloseHandle(CreateThread(nullptr, 256 * 1024, &watch_main, nullptr, 0, nullptr));
    LOG("experiment HavokThreads: job queue %p for %d threads, %d worker threads started (priority %d)", queue,
        g_requested + 1, g_workers, priority);
    return true;
}

// hkReferencedObject::lockAll's lock (taken by hkpWorld::lock): held count.
int ref_lock_count() {
    auto* lock = rfg::global<uint8_t*>(r::hkSingleton_hkReferencedObjectLock___s_instance);
    return lock ? *reinterpret_cast<volatile int*>(lock + kRefLockCount) : 0;
}

// ---- hooks ----------------------------------------------------------------------------------------------------

// hkpSimulation::stepDeltaTime(dt), called by hkpWorld::stepDeltaTime with the world locked. For the multithreaded
// simulation it becomes hkpWorld::stepMultithreaded with our threads instead of an hkThreadPool.
int(__fastcall* o_sim_step)(uint8_t*, void*, float);
int __fastcall d_sim_step(uint8_t* sim, void* edx, float dt) {
    uint8_t* world = sim ? *reinterpret_cast<uint8_t**>(sim + kSimWorld) : nullptr;
    if (!world || *reinterpret_cast<int*>(world + kWorldSimulationType) != kSimulationMultithreaded)
        return o_sim_step(sim, edx, dt);
    if (!g_queue && !g_queue_failed && !init_queue()) {
        g_queue_failed = true;
        LOG("experiment HavokThreads: could not create the job queue; falling back to Havok's single-threaded step");
    }
    if (!g_queue) return o_sim_step(sim, edx, dt);

    g_step_sim = sim;
    g_step_start.store(session::qpc(), std::memory_order_release);
    // Havok's multithreaded step runs without the world lock (hkReferencedObject::lockAll): jobs that queue world
    // operations take it, so a worker would wait for the sim thread while the sim thread waits for the worker.
    rfg::fn<WorldLockFn>(r::hkpWorld__unlock)(world, nullptr);
    // Multiplayer keeps the step on the sim thread, like the other experiments.
    int workers = rfg::global<uint8_t>(r::Multiplayer) ? 0 : g_workers;
    if (workers > 0 && ref_lock_count() != 0) {
        if (g_steps_alone++ < 5) LOG("experiment HavokThreads: world lock still held, this step runs on the sim thread");
        workers = 0;
    }
    int result = rfg::fn<StepBegin>(r::hkpMultiThreadedSimulation__stepBeginSt)(sim, nullptr, g_queue, dt);
    if (result == 0) {
        const int64_t start = session::qpc();
        g_in_jobs.store(true, std::memory_order_relaxed);
        if (workers > 0) {
            g_mxcsr = _mm_getcsr();
            g_pending.store(workers, std::memory_order_release);
            for (int i = 1; i <= workers; ++i) SetEvent(g_start[i]);
        }
        process_all_jobs()(g_queue, nullptr, false);
        if (workers > 0) WaitForSingleObject(g_done, INFINITE);
        g_in_jobs.store(false, std::memory_order_relaxed);
        g_frame_jobs_ms += session::ticks_to_ms(session::qpc() - start);
        ++g_steps;
        // No thread pool: the TOIs and the rest of the step's end run on this thread.
        result = rfg::fn<StepFinish>(r::hkpMultiThreadedSimulation__finishMtStep)(sim, nullptr, g_queue, nullptr);
    }
    rfg::fn<WorldLockFn>(r::hkpWorld__lock)(world, nullptr);
    g_step_start.store(0, std::memory_order_release);
    return result;
}

// hkpWorld::hkpWorld(const hkpWorldCinfo&, sdk version): the game's world (from havok_system_init) is created
// multithreaded.
void*(__fastcall* o_world_ctor)(void*, void*, uint8_t*, uint32_t);
void* __fastcall d_world_ctor(void* world, void* edx, uint8_t* cinfo, uint32_t version) {
    const uint32_t caller = uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress()) - rfg::base());
    const bool game_world = caller - r::havok_system_init < rfg::size::havok_system_init;
    const bool patch = game_world && cinfo && cinfo[kCinfoSimulationType] == kSimulationContinuous;
    if (patch) {
        cinfo[kCinfoSimulationType] = kSimulationMultithreaded;
        cinfo[kCinfoProcessActionsInSingleThread] = 1;  // vehicles, player hold, flyers: game code, sim thread
        cinfo[kCinfoProcessToisMultithreaded] = 0;      // continuous physics as before, after the jobs
        cinfo[kCinfoMtPostponeBorderCallbacks] = 1;     // game_broad_phase_border: sorted, at the end of the step
        // Havok merges islands below this size into sparse islands that sleep only as a whole. The single-threaded
        // game's islands stay small with the same setting (a few bodies each); 0 keeps that behaviour.
        if (g_min_island_size >= 0)
            *reinterpret_cast<uint32_t*>(cinfo + kCinfoMinDesiredIslandSize) = uint32_t(g_min_island_size);
    }
    void* result = o_world_ctor(world, edx, cinfo, version);
    if (patch) {
        auto* w = static_cast<uint8_t*>(world);
        const int type = *reinterpret_cast<int*>(w + kWorldSimulationType);
        void* lock = *reinterpret_cast<void**>(w + kWorldModifyConstraintLock);
        if (type == kSimulationMultithreaded) {
            g_world = w;
            game_lock::set(lock);
            // Every island is solved inside its own integrate job, as the single-threaded step does. Above this
            // many constraints Havok splits an island's solver into batch and export jobs, and the island's
            // broadphase job then spins (cpuBroadPhaseJob) until the last export job is done; when every thread
            // was spinning there while export jobs were still queued, the step never finished (a hang on the
            // Watchtower benchmark once collapses built islands of more than 70 constraints).
            auto* sim = *reinterpret_cast<uint8_t**>(w + kWorldSimulation);
            *reinterpret_cast<uint32_t*>(sim + kSimMaxConstraintsSolvedSingleThreaded) = 0xFFFFFFFFu;
        }
        LOG("experiment HavokThreads: world %p created with simulation type %d (3 = multithreaded), game callbacks "
            "under its constraint lock %p, min island size %u",
            world, type, lock, *reinterpret_cast<uint32_t*>(w + kWorldMinDesiredIslandSize));
    }
    return result;
}

// hkpWorld::~hkpWorld deletes its locks before it removes the entities (whose contact-removed callbacks are guarded).
void(__fastcall* o_world_dtor)(void*, void*);
void __fastcall d_world_dtor(void* world, void* edx) {
    if (world == g_world) {
        game_lock::set(nullptr);
        g_world = nullptr;
        LOG("experiment HavokThreads: world %p destroyed after %llu multithreaded steps", world,
            static_cast<unsigned long long>(g_steps));
    }
    o_world_dtor(world, edx);
}

// Game callbacks that run inside Havok's jobs, serialized (the contact-added callback is in core/contacts).
template <int Id, typename... A>
struct Guarded {
    static inline void(__fastcall* original)(void*, void*, A...) = nullptr;
    static void __fastcall detour(void* self, void* edx, A... args) {
        game_lock::Guard guard;
        original(self, edx, args...);
    }
    static bool install(uint32_t rva, const char* name) { return hooks::create(rva, &detour, &original, name); }
};

}  // namespace

bool install_havok_mt(const Config& cfg) {
    if (cfg.exp_havok_threads <= 0) return true;
    g_requested = std::min(cfg.exp_havok_threads, kMaxThreads - 1);
    g_min_island_size = cfg.exp_havok_min_island_size;
    bool ok = contacts::install();
    ok &= Guarded<0, void*>::install(r::GeneralCollisionListener__contactPointRemovedCallback,
                                     "GeneralCollisionListener::contactPointRemovedCallback");
    ok &= Guarded<1, void*>::install(r::phantom_overlap_listener__collidableAddedCallback,
                                     "phantom_overlap_listener::collidableAddedCallback");
    ok &= Guarded<2, void*>::install(r::game_aabb_phantom__addOverlappingCollidable,
                                     "game_aabb_phantom::addOverlappingCollidable");
    ok &= Guarded<3, void*, void*, void*>::install(r::havok_region_callback_shape__phantomEnterEvent,
                                                   "havok_region_callback_shape::phantomEnterEvent");
    ok &= Guarded<4, void*, void*>::install(r::havok_region_callback_shape__phantomLeaveEvent,
                                            "havok_region_callback_shape::phantomLeaveEvent");
    ok &= hooks::create(r::hkpSimulation__stepDeltaTime, &d_sim_step, &o_sim_step, "hkpSimulation::stepDeltaTime");
    ok &= hooks::create(r::hkpWorld___hkpWorld, &d_world_dtor, &o_world_dtor, "hkpWorld::~hkpWorld");
    // Last: without every guard in place the world stays single-threaded.
    if (!ok || !hooks::create(r::hkpWorld__hkpWorld, &d_world_ctor, &o_world_ctor, "hkpWorld::hkpWorld")) {
        LOG("experiment HavokThreads: hooks failed, the world stays single-threaded");
        return false;
    }
    LOG("experiment HavokThreads=%d%s enabled: multithreaded Havok world, sim thread + %d worker threads",
        cfg.exp_havok_threads, cfg.exp_havok_threads_auto ? " (auto, by the processor)" : "", g_requested);
    return true;
}

float havok_mt_take_jobs_ms() {
    const double ms = g_frame_jobs_ms;
    g_frame_jobs_ms = 0;
    return float(ms);
}

bool havok_mt_in_jobs() { return g_in_jobs.load(std::memory_order_relaxed); }

}  // namespace experiments
