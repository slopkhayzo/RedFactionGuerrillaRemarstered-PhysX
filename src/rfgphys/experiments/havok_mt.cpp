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
#include "experiments/havok_audit.h"
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
bool g_jobs_on_sim_thread = false;  // Test.HavokJobsOnSimThread
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
std::atomic<uint32_t> g_removes_skipped{0}, g_adds_skipped{0};  // queued island constraint changes (below)

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

// hkpWorld::m_activeSimulationIslands and m_inactiveSimulationIslands (hkArray: data, size), hkpSimulationIsland's
// body count (m_entities).
constexpr uint32_t kWorldActiveIslands = 0x28, kWorldInactiveIslands = 0x34, kIslandNumEntities = 0x50;

// hkReferencedObject::lockAll's lock (taken by hkpWorld::lock): held count.
int ref_lock_count() {
    auto* lock = rfg::global<uint8_t*>(r::hkSingleton_hkReferencedObjectLock___s_instance);
    return lock ? *reinterpret_cast<volatile int*>(lock + kRefLockCount) : 0;
}

// ---- collide jobs for big agent tracks ------------------------------------------------------------------------

// hkpSingleThreadedJobsOnIsland::cpuBroadPhaseJob ends an island's broadphase by queuing its collide work: one
// hkpAgentSectorJob per agent track (narrowphase, then midphase), which the job queue cuts into tasks of
// m_maxSectorsPer...CollideTask sectors, and one hkpAgentSectorHeader counting all their tasks; the last task to finish
// queues the island's post-collide job, which replays what the tasks gathered (the constraints their contacts gained
// and lost, and the totals' changes) into the island. The header counts the tasks from the tracks' full sector counts,
// but the job keeps its sector count in 16 bits (hkpAgentBaseJob::m_numElements): from a track of 65,536 sectors on,
// the sectors past that are never processed, the header never sees its last task, and the island's post-collide job
// never comes. That step's contact changes of the whole island are lost: its constraint totals drift from its
// constraints, constraints that gained their first contact point stay out of any island (and are removed twice later),
// and the solver soon lays the island's buffer out too small (the int3 at hkSolveConstraints+0x2332). A pile of
// colliding debris from a tower shattered by a shockwave hammer reached 83,000 narrowphase sectors (about 330,000
// agents among 4,100 bodies); the single-threaded step walks the tracks directly and has no such limit.
// Right after the header is allocated, before the jobs are queued, a stub passes them to split_big_sector_jobs: a job
// past the limit keeps the first sectors that fit (whole tasks, all sectors full) and the rest go into more jobs for
// the same header, numbered as the header expects. The task numbers themselves are signed 16-bit (the header's size in
// hkpAgentSectorHeader::allocate, the task's slot in hkCpuAgentSectorJob), so a step whose islands would need more than
// that raises the sectors per task first (plan_step) or, past that too, runs single-threaded.
constexpr uint32_t kIslandMidphaseTrack = 0x5C, kIslandNarrowphaseTrack = 0x70;  // hkpAgentNnTrack
constexpr uint32_t kTrackLastSectorBytes = 0x00, kTrackNumSectors = 0x08;      // u16; hkInplaceArray size
constexpr uint32_t kWorldMidphaseSectorsPerTask = 0xDC, kWorldNarrowphaseSectorsPerTask = 0xE0;
// hkpAgentSectorJob (a job queue entry): its island, header, task number (s16), sector count and sectors per task
// (u16), track type (2 narrowphase), sectors (hkpAgentNnSector**), bytes used in its last sector (u16).
constexpr uint32_t kJobIsland = 0x14, kJobHeader = 0x20, kJobTaskIndex = 0x24, kJobNumSectors = 0x26,
                   kJobSectorsPerTask = 0x28, kJobTrackType = 0x2A, kJobSectors = 0x2C, kJobLastSectorBytes = 0x44;
constexpr size_t kJobEntrySize = 0x80;
constexpr uint16_t kSectorBytes = 0x200;
constexpr int kMaxJobSectors = 0xFFFF;
constexpr uint8_t kNarrowphaseTrack = 2;
// In cpuBroadPhaseJob, right after `call hkpAgentSectorHeader::allocate(tasks, 32)`: xor edi, edi; mov ebx, eax;
// add esp, 8. The jobs are at [esp+0x98] (pointers, [esp+0x14] of them), the job queue is the second argument.
const rfg::CodeSite kAfterHeaderAllocate{r::hkpSingleThreadedJobsOnIsland__cpuBroadPhaseJob, 0x931};
const uint8_t kAfterHeaderAllocateBytes[] = {0x33, 0xFF, 0x8B, 0xD8, 0x83, 0xC4, 0x08};

using AddJobFn = void(__fastcall*)(void* queue, void* edx, void* entry, int priority);  // hkJobQueue::addJob
bool g_split_installed = false;
std::atomic<uint32_t> g_jobs_split{0};

// On whichever Havok thread runs the island's broadphase job.
void __cdecl split_big_sector_jobs(void* queue, uint8_t* header, uint8_t* const* jobs, int num_jobs) {
    for (int j = 0; j < num_jobs; ++j) {
        uint8_t* job = jobs[j];
        const uint8_t* island = *reinterpret_cast<const uint8_t* const*>(job + kJobIsland);
        const uint8_t* track =
            island + (job[kJobTrackType] == kNarrowphaseTrack ? kIslandNarrowphaseTrack : kIslandMidphaseTrack);
        const int sectors = *reinterpret_cast<const int*>(track + kTrackNumSectors);
        const int per = *reinterpret_cast<const uint16_t*>(job + kJobSectorsPerTask);
        if (sectors <= kMaxJobSectors || per <= 0) continue;
        const int keep = kMaxJobSectors - kMaxJobSectors % per;
        auto* const* all = *reinterpret_cast<uint8_t* const* const*>(job + kJobSectors);
        const int first_task = *reinterpret_cast<const int16_t*>(job + kJobTaskIndex);
        *reinterpret_cast<uint16_t*>(job + kJobNumSectors) = uint16_t(keep);
        *reinterpret_cast<uint16_t*>(job + kJobLastSectorBytes) = kSectorBytes;
        int added = 0;
        for (int start = keep; start < sectors; start += keep, ++added) {
            alignas(16) uint8_t more[kJobEntrySize];
            memcpy(more, job, sizeof(more));
            const int count = std::min(keep, sectors - start);
            *reinterpret_cast<uint8_t**>(more + kJobHeader) = header;
            *reinterpret_cast<int16_t*>(more + kJobTaskIndex) = int16_t(first_task + start / per);
            *reinterpret_cast<uint16_t*>(more + kJobNumSectors) = uint16_t(count);
            *reinterpret_cast<uint8_t* const**>(more + kJobSectors) = all + start;
            *reinterpret_cast<uint16_t*>(more + kJobLastSectorBytes) =
                start + count == sectors ? *reinterpret_cast<const uint16_t*>(track + kTrackLastSectorBytes)
                                         : kSectorBytes;
            rfg::fn<AddJobFn>(r::hkJobQueue__addJobEntry)(queue, nullptr, more, 1);
        }
        const uint32_t n = g_jobs_split.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10 || n % 1000 == 0)
            LOG("experiment HavokThreads: an island of %d bodies has %d %s agent sectors, more than a Havok collide "
                "job counts (%d): its job split in %d (%u so far)",
                *reinterpret_cast<const int*>(island + kIslandNumEntities), sectors,
                job[kJobTrackType] == kNarrowphaseTrack ? "narrowphase" : "midphase", kMaxJobSectors, added + 1, n);
    }
}

bool install_split_big_sector_jobs() {
    auto* site = static_cast<uint8_t*>(rfg::addr(kAfterHeaderAllocate.rva()));
    // The site follows the call to hkpAgentSectorHeader::allocate; the job loop after it reads [esp+0x14] and
    // [esp+edi*4+0x98], and the queue from [ebp+0xC].
    static const uint8_t kJobCount[] = {0x89, 0x7B, 0x0C, 0x89, 0x7B, 0x10, 0x39, 0x7C, 0x24, 0x14};
    static const uint8_t kJobPointer[] = {0x8B, 0x84, 0xBC, 0x98, 0x00, 0x00, 0x00};
    static const uint8_t kQueue[] = {0x8B, 0x4D, 0x0C, 0x6A, 0x01, 0x50, 0xE8};
    int32_t call_rel = 0;
    memcpy(&call_rel, site - 4, 4);
    if (site[-5] != 0xE8 || site + call_rel != rfg::addr(r::hkpAgentSectorHeader__allocate) ||
        memcmp(site, kAfterHeaderAllocateBytes, sizeof(kAfterHeaderAllocateBytes)) != 0 ||
        memcmp(site + 7, kJobCount, sizeof(kJobCount)) != 0 ||
        memcmp(site + 0x13, kJobPointer, sizeof(kJobPointer)) != 0 || memcmp(site + 0x21, kQueue, sizeof(kQueue)) != 0)
        return false;
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    uint8_t* p = stub;
    auto put = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *p++ = b;
    };
    auto put_u32 = [&](uint32_t v) {
        memcpy(p, &v, 4);
        p += 4;
    };
    put({0x33, 0xFF, 0x8B, 0xD8, 0x83, 0xC4, 0x08});  // the replaced code: xor edi, edi; mov ebx, eax; add esp, 8
    put({0x60});                                      // pushad
    put({0x8D, 0x54, 0x24, 0x20});                    // lea edx, [esp+0x20]: the function's esp
    put({0xFF, 0x72, 0x14});                          // push [edx+0x14]: the job count
    put({0x8D, 0x8A, 0x98, 0x00, 0x00, 0x00, 0x51});  // lea ecx, [edx+0x98]; push ecx: the jobs
    put({0x53});                                      // push ebx: the header
    put({0xFF, 0x75, 0x0C});                          // push [ebp+0xC]: the job queue
    put({0xB8});                                      // mov eax, split_big_sector_jobs
    put_u32(reinterpret_cast<uint32_t>(&split_big_sector_jobs));
    put({0xFF, 0xD0, 0x83, 0xC4, 0x10});              // call eax; add esp, 16
    put({0x61});                                      // popad
    put({0xE9});                                      // jmp back after the replaced code
    put_u32(uint32_t(reinterpret_cast<uintptr_t>(site + sizeof(kAfterHeaderAllocateBytes)) -
                     reinterpret_cast<uintptr_t>(p + 4)));
    FlushInstructionCache(GetCurrentProcess(), stub, size_t(p - stub));
    uint8_t jump[sizeof(kAfterHeaderAllocateBytes)] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
    const int32_t rel = int32_t(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(site + 5));
    memcpy(jump + 1, &rel, 4);
    DWORD old;
    VirtualProtect(site, sizeof(jump), PAGE_EXECUTE_READWRITE, &old);
    memcpy(site, jump, sizeof(jump));
    VirtualProtect(site, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(jump));
    return true;
}

// Before each step: the islands' agent tracks against the jobs' 16-bit task numbers. Havok's 4 narrowphase / 2
// midphase sectors per task (16 agents) give up to 32,767 tasks per island; 8 and 4 (32 agents, as many contact
// constraint changes as a task's command queue holds: hkpAgentSectorHeader::allocate(tasks, 32)) give twice that.
// Islands merge at the step's start (cleanupDirtyIslands) and gain agents in their broadphase job, hence the margins.
constexpr int kMaxTasksPlanned = 24000;
constexpr int kMaxIslandsOrBodies = 60000;  // u16 island indices (the jobs) and body counts (the solver's task header)
constexpr int kTrackMargin = 16384;  // without the split, a track this close to a job's count steps single-threaded
bool g_big_tasks = false;
int g_narrow_per_task = 4, g_mid_per_task = 2;  // the world's own (read when it is created)
uint32_t g_plan_logs = 0;
uint64_t g_steps_single = 0;

struct StepPlan {
    bool single_threaded;
    bool big_tasks;
    int sectors, tasks, bodies, islands;  // the most sectors in a track, tasks and bodies in an island; islands
};

int track_sectors(const uint8_t* island, uint32_t track) {
    return *reinterpret_cast<const int*>(island + track + kTrackNumSectors);
}

int tasks_for(int sectors, int per) { return (sectors + per - 1) / per; }

StepPlan plan_step(const uint8_t* world) {
    StepPlan plan{};
    int tasks_big = 0;  // with 8 and 4 sectors per task
    for (uint32_t list : {kWorldActiveIslands, kWorldInactiveIslands}) {
        const auto* islands = *reinterpret_cast<const uint8_t* const* const*>(world + list);
        const int n = *reinterpret_cast<const int*>(world + list + 4);
        plan.islands += n;
        for (int i = 0; i < n; ++i) {
            const int narrow = track_sectors(islands[i], kIslandNarrowphaseTrack);
            const int mid = track_sectors(islands[i], kIslandMidphaseTrack);
            plan.sectors = std::max({plan.sectors, narrow, mid});
            plan.tasks = std::max(plan.tasks, tasks_for(narrow, 4) + tasks_for(mid, 2));
            tasks_big = std::max(tasks_big, tasks_for(narrow, 8) + tasks_for(mid, 4));
            plan.bodies = std::max(plan.bodies, *reinterpret_cast<const int*>(islands[i] + kIslandNumEntities));
        }
    }
    plan.big_tasks = plan.tasks > kMaxTasksPlanned;
    plan.single_threaded = tasks_big > kMaxTasksPlanned || plan.islands > kMaxIslandsOrBodies ||
                           plan.bodies > kMaxIslandsOrBodies ||
                           (!g_split_installed && plan.sectors > kMaxJobSectors - kTrackMargin);
    return plan;
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

    // The islands against the collide jobs' 16-bit counts (split_big_sector_jobs above).
    const StepPlan plan = plan_step(world);
    if (plan.big_tasks != g_big_tasks) {
        g_big_tasks = plan.big_tasks;
        *reinterpret_cast<int*>(world + kWorldNarrowphaseSectorsPerTask) = plan.big_tasks ? 8 : g_narrow_per_task;
        *reinterpret_cast<int*>(world + kWorldMidphaseSectorsPerTask) = plan.big_tasks ? 4 : g_mid_per_task;
        if (g_plan_logs++ < 50)
            LOG("experiment HavokThreads: the biggest island needs %d collide tasks: %s sectors per task", plan.tasks,
                plan.big_tasks ? "8 narrowphase and 4 midphase" : "Havok's");
    }
    if (plan.single_threaded) {
        if (g_steps_single++ % 600 == 0)
            LOG("experiment HavokThreads: %d islands, up to %d bodies and %d agent sectors in one (%d collide tasks): "
                "beyond Havok's collide jobs, the step runs single-threaded (%llu so far)", plan.islands, plan.bodies,
                plan.sectors, plan.tasks, static_cast<unsigned long long>(g_steps_single));
        return o_sim_step(sim, edx, dt);
    }

    g_step_sim = sim;
    g_step_start.store(session::qpc(), std::memory_order_release);
    // Havok's multithreaded step runs without the world lock (hkReferencedObject::lockAll): jobs that queue world
    // operations take it, so a worker would wait for the sim thread while the sim thread waits for the worker.
    rfg::fn<WorldLockFn>(r::hkpWorld__unlock)(world, nullptr);
    // Multiplayer keeps the step on the sim thread, like the other experiments.
    int workers = rfg::global<uint8_t>(r::Multiplayer) || g_jobs_on_sim_thread ? 0 : g_workers;
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
        havok_audit_after_jobs();  // Test.HavokAudit: the islands between the jobs and the step's end
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
            g_narrow_per_task = *reinterpret_cast<const int*>(w + kWorldNarrowphaseSectorsPerTask);
            g_mid_per_task = *reinterpret_cast<const int*>(w + kWorldMidphaseSectorsPerTask);
            g_big_tasks = false;
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
        LOG("experiment HavokThreads: world %p destroyed after %llu multithreaded steps and %llu single-threaded ones "
            "for islands beyond Havok's collide jobs; collide jobs split for big agent tracks %u; queued constraint "
            "changes skipped: %u removals, %u additions", world, static_cast<unsigned long long>(g_steps),
            static_cast<unsigned long long>(g_steps_single), g_jobs_split.load(), g_removes_skipped.load(),
            g_adds_skipped.load());
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

// While the jobs run, the constraints a contact gains or loses go into Havok's command queue (hkpDeferredConstraintOwner)
// and the island's post-collide job replays them (hkPhysicsCommandMachineProcess) through these two. A shockwave mod's
// hammer blow that shattered a tower into thousands of pieces crashed there: a removal for a constraint no longer in
// any island (m_owner null, read by hkpWorldConstraintUtil::removeConstraint), left by a post-collide job that never
// came (the collide jobs' 16-bit counts, split_big_sector_jobs above). Such a removal, or an addition for a
// constraint already in an island, is still skipped and logged with the constraint's class (its data's vtable) and
// caller.
constexpr uint32_t kConstraintOwner = 0x08, kConstraintData = 0x0C, kConstraintEntities = 0x14, kConstraintUid = 0x34;
using IslandConstraintOp = void(__cdecl*)(void*, uint8_t*);
IslandConstraintOp o_add_locked = nullptr, o_remove_locked = nullptr;

struct ConstraintInfo {
    uint32_t data_vtable;  // RVA
    const void* entities[2];
    uint32_t uid;
    bool readable;
};

ConstraintInfo read_constraint(const uint8_t* c) {  // no C++ objects here: SEH
    ConstraintInfo out{};
    __try {
        const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(c + kConstraintData);
        out.data_vtable = data ? uint32_t(*reinterpret_cast<const uintptr_t*>(data) - rfg::base()) : 0;
        memcpy(out.entities, c + kConstraintEntities, sizeof(out.entities));
        out.uid = *reinterpret_cast<const uint32_t*>(c + kConstraintUid);
        out.readable = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return out;
}

void note_skipped(std::atomic<uint32_t>& count, const char* what, const uint8_t* c, const void* caller) {
    const uint32_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 10 && n % 1000 != 0) return;
    const ConstraintInfo info = read_constraint(c);
    LOG("experiment HavokThreads: %s (%u so far): constraint %p, data vtable +%X, entities %p %p, uid %u%s; caller +%X "
        "on thread %lu%s", what, n, static_cast<const void*>(c), info.data_vtable, info.entities[0], info.entities[1],
        info.uid, info.readable ? "" : " (unreadable)", uint32_t(reinterpret_cast<uintptr_t>(caller) - rfg::base()),
        GetCurrentThreadId(), g_in_jobs.load(std::memory_order_relaxed) ? " (in the jobs)" : " (outside the jobs)");
    if (n <= 10) thread_dump::log_own_stack("experiment HavokThreads:   stack", 32);
}

void __cdecl d_remove_locked(void* world, uint8_t* c) {
    if (c && !*reinterpret_cast<void* const*>(c + kConstraintOwner)) {
        note_skipped(g_removes_skipped, "a constraint removal from an island skipped: it is in none", c,
                     _ReturnAddress());
        return;
    }
    o_remove_locked(world, c);
}

void __cdecl d_add_locked(void* world, uint8_t* c) {
    if (c && *reinterpret_cast<void* const*>(c + kConstraintOwner)) {
        note_skipped(g_adds_skipped, "a constraint addition to an island skipped: it is in one", c, _ReturnAddress());
        return;
    }
    o_add_locked(world, c);
}

}  // namespace

bool install_havok_mt(const Config& cfg) {
    if (cfg.exp_havok_threads <= 0) return true;
    g_requested = std::min(cfg.exp_havok_threads, kMaxThreads - 1);
    g_min_island_size = cfg.exp_havok_min_island_size;
    g_jobs_on_sim_thread = cfg.test_havok_jobs_on_sim_thread;
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
    ok &= hooks::create(r::hkpWorldOperationUtil__addConstraintToCriticalLockedIsland, &d_add_locked, &o_add_locked,
                        "hkpWorldOperationUtil::addConstraintToCriticalLockedIsland");
    ok &= hooks::create(r::hkpWorldOperationUtil__removeConstraintFromCriticalLockedIsland, &d_remove_locked,
                        &o_remove_locked, "hkpWorldOperationUtil::removeConstraintFromCriticalLockedIsland");
    ok &= hooks::create(r::hkpSimulation__stepDeltaTime, &d_sim_step, &o_sim_step, "hkpSimulation::stepDeltaTime");
    ok &= hooks::create(r::hkpWorld___hkpWorld, &d_world_dtor, &o_world_dtor, "hkpWorld::~hkpWorld");
    g_split_installed = install_split_big_sector_jobs();
    if (!g_split_installed)
        LOG("experiment HavokThreads: unexpected code in hkpSingleThreadedJobsOnIsland::cpuBroadPhaseJob; islands with "
            "agent tracks near Havok's collide job limit step single-threaded");
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
