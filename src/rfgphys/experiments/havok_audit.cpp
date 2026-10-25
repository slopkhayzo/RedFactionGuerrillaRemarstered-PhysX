#include "experiments/havok_audit.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "experiments/havok_mt.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace experiments {
namespace {

namespace r = rfg::rva;

// hkpSimulationIsland: m_constraintInfo (hkpConstraintOwner +8: m_maxSizeOfSchema, m_sizeOfSchemas,
// m_numSolverResults, m_numSolverElemTemps), m_numConstraints, m_entities (hkInplaceArray: data, size).
constexpr uint32_t kIslandInfo = 0x08, kIslandNumConstraints = 0x1C, kIslandEntities = 0x4C, kIslandNumEntities = 0x50;
// hkpEntity: m_constraintsMaster (hkSmallArray<hkConstraintInternal>: data, u16 size), m_simulationIsland, the motion
// type (5: fixed).
constexpr uint32_t kEntityMasters = 0xA8, kEntityNumMasters = 0xAC, kEntityIsland = 0xC8, kEntityMotionType = 0xE8;
constexpr uint8_t kMotionFixed = 5;
// hkConstraintInternal (0x30 bytes): m_constraint, m_entities[2], ..., u16 m_numSolverResults, m_sizeOfSchemas,
// m_numSolverElemTemps, m_constraintType (1: a chain).
constexpr uint32_t kInternalSize = 0x30, kInternalEntities = 0x04, kInternalResults = 0x14, kInternalSchemas = 0x18,
                   kInternalTemps = 0x1C, kInternalType = 0x21;
// hkpConstraintInstance: m_owner, m_internal.
constexpr uint32_t kConstraintOwner = 0x08, kConstraintInternal = 0x30;
// hkpWorld: m_activeSimulationIslands, m_inactiveSimulationIslands (hkArray: data, size).
constexpr uint32_t kWorldActive = 0x28, kWorldInactive = 0x34;
constexpr int kMaxSolves = 16;  // islands solved at the same time (one per Havok thread)
constexpr double kSummaryMs = 30000;

int g_mode = 0;

struct Sums {
    uint32_t constraints = 0, schemas = 0, results = 0, temps = 0, max_schema = 0;
    uint32_t wrong_owner = 0, wrong_internal = 0, wrong_island = 0;
    uint32_t spanning = 0;  // constraints with a body that is not fixed in another island
    const uint8_t* first_bad = nullptr;
    const void* first_bad_owner = nullptr;
};

struct Totals {
    uint32_t info[4];  // the island's m_constraintInfo
    int num_constraints;
    int num_entities;
};

// SEH only (no C++ objects): the audit must not take down the game it watches.
bool read_island(const uint8_t* island, Sums& s, Totals& t) {
    __try {
        memcpy(t.info, island + kIslandInfo, sizeof(t.info));
        t.num_constraints = *reinterpret_cast<const int*>(island + kIslandNumConstraints);
        const auto* entities = *reinterpret_cast<const uint8_t* const* const*>(island + kIslandEntities);
        const int n = *reinterpret_cast<const int*>(island + kIslandNumEntities);
        t.num_entities = n;
        for (int i = 0; i < n; ++i) {
            const uint8_t* e = entities[i];
            if (*reinterpret_cast<const uint8_t* const*>(e + kEntityIsland) != island) ++s.wrong_island;
            const uint8_t* m = *reinterpret_cast<const uint8_t* const*>(e + kEntityMasters);
            const int count = *reinterpret_cast<const uint16_t*>(e + kEntityNumMasters);
            for (int j = 0; j < count; ++j, m += kInternalSize) {
                const uint8_t* c = *reinterpret_cast<const uint8_t* const*>(m);
                const uint32_t schemas = *reinterpret_cast<const uint32_t*>(m + kInternalSchemas);
                ++s.constraints;
                s.schemas += schemas;
                s.results += *reinterpret_cast<const uint16_t*>(m + kInternalResults);
                s.temps += *reinterpret_cast<const uint32_t*>(m + kInternalTemps);
                s.max_schema = std::max(s.max_schema, schemas);
                const void* owner = *reinterpret_cast<void* const*>(c + kConstraintOwner);
                const bool bad_internal = *reinterpret_cast<const uint8_t* const*>(c + kConstraintInternal) != m;
                const bool bad_owner = owner != island;
                s.wrong_internal += bad_internal;
                s.wrong_owner += bad_owner;
                for (uint32_t k = 0; k < 2; ++k) {
                    const uint8_t* body = *reinterpret_cast<const uint8_t* const*>(m + kInternalEntities + 4 * k);
                    if (body && body[kEntityMotionType] != kMotionFixed &&
                        *reinterpret_cast<const uint8_t* const*>(body + kEntityIsland) != island) {
                        ++s.spanning;
                        break;
                    }
                }
                if ((bad_internal || bad_owner) && !s.first_bad) {
                    s.first_bad = c;
                    s.first_bad_owner = owner;
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Totals below the sums are the dangerous direction: the solver lays the island's buffer out from the totals.
bool short_of(const Sums& s, const Totals& t) {
    return s.schemas > t.info[1] || s.temps > t.info[3] || s.constraints > uint32_t(t.num_constraints) ||
           s.max_schema > t.info[0];
}

bool differs(const Sums& s, const Totals& t) {
    return s.schemas != t.info[1] || s.results != t.info[2] || s.temps != t.info[3] ||
           s.constraints != uint32_t(t.num_constraints) || s.max_schema > t.info[0] || s.wrong_owner ||
           s.wrong_internal || s.wrong_island || s.spanning;
}

enum Phase { kBeforeStep, kAtSolve, kAfterSplit, kAfterPostCollide, kAfterJobs, kAfterStep, kPhases };
const char* const kPhaseNames[kPhases] = {"before the step", "at its solve", "after a split in its integrate job",
                                           "after its post-collide job", "after the jobs (before the step's end)",
                                           "after the step"};

std::atomic<uint64_t> g_checked[kPhases], g_mismatched[kPhases], g_short[kPhases];
std::atomic<uint64_t> g_unreadable{0}, g_solves{0}, g_size_changed{0}, g_overlaps{0}, g_repaired{0}, g_swapped{0};
std::atomic<uint64_t> g_splits{0};
std::atomic<uint64_t> g_tasks{0}, g_tasks_elsewhere{0}, g_tasks_split_agent{0};  // collide tasks (account_task)
std::atomic<uint64_t> g_task_mismatches{0}, g_task_too_big{0}, g_merges_in_jobs{0};
// A task starts with its header's m_openJobs counting it; one that starts with none left runs after its island's
// post-collide job has replayed and freed the header, and what it hands on is lost.
std::atomic<uint64_t> g_tasks_late{0};
std::atomic<uint32_t> g_reports{0};
int64_t g_last_summary = 0;

// An island stays wrong once it is: it is reported again only when its numbers change.
struct Reported {
    const uint8_t* island;
    uint32_t sum, total;
};
Reported g_reported[64];
uint32_t g_reported_next = 0;
SRWLOCK g_reported_lock = SRWLOCK_INIT;

bool seen_before(const uint8_t* island, const Sums& s, const Totals& t) {
    AcquireSRWLockExclusive(&g_reported_lock);
    bool seen = false;
    for (Reported& r : g_reported)
        if (r.island == island) {
            seen = r.sum == s.schemas && r.total == t.info[1];
            r = {island, s.schemas, t.info[1]};
            ReleaseSRWLockExclusive(&g_reported_lock);
            return seen;
        }
    g_reported[g_reported_next++ % 64] = {island, s.schemas, t.info[1]};
    ReleaseSRWLockExclusive(&g_reported_lock);
    return false;
}

void report(Phase phase, const uint8_t* island, const Sums& s, const Totals& t) {
    if (seen_before(island, s, t)) return;
    const uint32_t n = g_reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 20 && n % 1000 != 0) return;
    LOG("Havok audit: island %p (%d bodies) %s%s: constraints %d (their sum %u), schema bytes %u (sum %u), solver "
        "results %u (sum %u), solver temps %u (sum %u), largest schema %u (largest constraint %u); owner wrong %u, "
        "internal wrong %u, bodies in another island %u, constraints reaching into another island %u (first bad "
        "constraint %p, owner %p); report %u",
        static_cast<const void*>(island), t.num_entities, kPhaseNames[phase],
        short_of(s, t) ? " SHORT (the solver would overrun)" : "", t.num_constraints, s.constraints, t.info[1],
        s.schemas, t.info[2], s.results, t.info[3], s.temps, t.info[0], s.max_schema, s.wrong_owner,
        s.wrong_internal, s.wrong_island, s.spanning, static_cast<const void*>(s.first_bad), s.first_bad_owner, n);
}

// ---- island history (with Test.HavokAuditJobs) ------------------------------------------------------------------

// For islands of kHistoryBodies or more, a record at every job edge goes into a ring. The first time the check after
// the jobs finds such an island wrong, the records of the last steps go to havok_island_history.txt in the run folder.
constexpr int kHistoryBodies = 1000;
constexpr uint32_t kHistorySize = 1u << 16;
constexpr uint64_t kHistorySteps = 3;

enum Event : uint8_t {
    kEvBeforeStep, kEvSolve, kEvSplit, kEvBroadPhaseIn, kEvBroadPhaseOut, kEvTask, kEvPostCollideIn, kEvPlan,
    kEvPostCollideOut, kEvAfterJobs, kEvAfterStep, kEvents
};
const char* const kEventNames[kEvents] = {"before step", "solve", "split", "broadphase in", "broadphase out", "task",
                                          "post-collide in", "replay plan", "post-collide out", "after jobs",
                                          "after step"};

struct HistoryRecord {
    uint64_t step;
    const uint8_t* island;
    const uint8_t* header;
    DWORD thread;
    uint8_t event, subtype;
    int16_t task, open_jobs, total_tasks;
    int bodies, constraints, sum_constraints;  // sum_constraints -1: the sums were not read
    uint32_t schemas, sum_schemas, results, sum_results;
    int32_t a, b, c, d;  // task: handed and moved schema bytes, joined, left; plan: tasks, additions, removals, the
                         // deltas' schema bytes; split: new islands, their bodies
};

bool g_history = false;
HistoryRecord* g_ring = nullptr;
std::atomic<uint32_t> g_ring_next{0};
std::atomic<uint64_t> g_step{0};
std::atomic<bool> g_history_written{false};

HistoryRecord* history_slot() {
    if (!g_ring) return nullptr;
    HistoryRecord& r = g_ring[g_ring_next.fetch_add(1, std::memory_order_relaxed) % kHistorySize];
    r = {};
    r.step = g_step.load(std::memory_order_relaxed);
    r.thread = GetCurrentThreadId();
    return &r;
}

void history(Event event, const uint8_t* island, const Sums* s, const Totals& t) {
    if (!g_history || t.num_entities < kHistoryBodies) return;
    HistoryRecord* r = history_slot();
    if (!r) return;
    r->event = event;
    r->island = island;
    r->bodies = t.num_entities;
    r->constraints = t.num_constraints;
    r->schemas = t.info[1];
    r->results = t.info[2];
    r->sum_constraints = s ? int(s->constraints) : -1;
    if (s) {
        r->sum_schemas = s->schemas;
        r->sum_results = s->results;
    }
}

void write_history(const uint8_t* bad) {
    if (!g_ring || g_history_written.exchange(true)) return;
    const std::wstring path = session::run_dir() + L"\\havok_island_history.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const uint64_t now = g_step.load(std::memory_order_relaxed);
    const uint32_t end = g_ring_next.load(std::memory_order_relaxed);
    const uint32_t begin = end > kHistorySize ? end - kHistorySize : 0;
    char line[512];
    DWORD written = 0;
    int n = snprintf(line, sizeof(line), "island %p went wrong in step %llu; records of islands of %d+ bodies from the "
                     "last %llu steps (gap = totals - sums)\n", static_cast<const void*>(bad),
                     static_cast<unsigned long long>(now), kHistoryBodies, static_cast<unsigned long long>(kHistorySteps));
    WriteFile(f, line, DWORD(n), &written, nullptr);
    for (uint32_t i = begin; i < end; ++i) {
        const HistoryRecord& r = g_ring[i % kHistorySize];
        if (r.step + kHistorySteps <= now || r.event >= kEvents) continue;
        if (r.event == kEvPlan)
            n = snprintf(line, sizeof(line), "%llu %5lu %-16s %p%s %d bodies, header %p (%d of %d open): %d tasks, %d "
                         "additions, %d removals, deltas' schema bytes %d\n", static_cast<unsigned long long>(r.step),
                         r.thread, kEventNames[r.event], static_cast<const void*>(r.island), r.island == bad ? " *" : "",
                         r.bodies, static_cast<const void*>(r.header), r.open_jobs, r.total_tasks, r.a, r.b, r.c, r.d);
        else if (r.event == kEvTask)
            n = snprintf(line, sizeof(line), "%llu %5lu %-16s %p%s %d bodies, task %d (subtype %u, header %p, %d of %d "
                         "open): handed %d, moved %d, joined %d, left %d; totals now: constraints %d, schemas %u\n",
                         static_cast<unsigned long long>(r.step), r.thread, kEventNames[r.event],
                         static_cast<const void*>(r.island), r.island == bad ? " *" : "", r.bodies, r.task, r.subtype,
                         static_cast<const void*>(r.header), r.open_jobs, r.total_tasks, r.a, r.b, r.c, r.d,
                         r.constraints, r.schemas);
        else if (r.sum_constraints < 0)
            n = snprintf(line, sizeof(line), "%llu %5lu %-16s %p%s %d bodies: constraints %d, schemas %u, results %u; "
                         "%d %d\n", static_cast<unsigned long long>(r.step), r.thread, kEventNames[r.event],
                         static_cast<const void*>(r.island), r.island == bad ? " *" : "", r.bodies, r.constraints,
                         r.schemas, r.results, r.a, r.b);
        else
            n = snprintf(line, sizeof(line), "%llu %5lu %-16s %p%s %d bodies: constraints %d (sum %d), schemas %u (sum "
                         "%u, gap %lld), results %u (sum %u); %d %d %d %d\n", static_cast<unsigned long long>(r.step),
                         r.thread, kEventNames[r.event], static_cast<const void*>(r.island), r.island == bad ? " *" : "",
                         r.bodies, r.constraints, r.sum_constraints, r.schemas, r.sum_schemas,
                         int64_t(r.schemas) - int64_t(r.sum_schemas), r.results, r.sum_results, r.a, r.b, r.c, r.d);
        if (n > 0) WriteFile(f, line, DWORD(n < int(sizeof(line)) ? n : int(sizeof(line)) - 1), &written, nullptr);
    }
    CloseHandle(f);
    LOG("Havok audit: the last %llu steps of the islands of %d+ bodies are in havok_island_history.txt",
        static_cast<unsigned long long>(kHistorySteps), kHistoryBodies);
}

bool check(Phase phase, const uint8_t* island, Sums& s, Totals& t) {
    if (!read_island(island, s, t)) {
        g_unreadable.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_checked[phase].fetch_add(1, std::memory_order_relaxed);
    if (phase == kBeforeStep || phase == kAtSolve || phase == kAfterJobs || phase == kAfterStep)
        history(phase == kBeforeStep ? kEvBeforeStep : phase == kAtSolve ? kEvSolve : phase == kAfterJobs ? kEvAfterJobs
                : kEvAfterStep, island, &s, t);
    if (!differs(s, t)) return true;
    g_mismatched[phase].fetch_add(1, std::memory_order_relaxed);
    if (short_of(s, t)) g_short[phase].fetch_add(1, std::memory_order_relaxed);
    report(phase, island, s, t);
    if (phase == kAfterJobs && g_history && t.num_entities >= kHistoryBodies) write_history(island);
    return true;
}

void check_list(Phase phase, const uint8_t* world, uint32_t offset) {
    const auto* islands = *reinterpret_cast<const uint8_t* const* const*>(world + offset);
    const int n = *reinterpret_cast<const int*>(world + offset + 4);
    for (int i = 0; i < n; ++i) {
        Sums s;
        Totals t;
        check(phase, islands[i], s, t);
    }
}

void log_summary(const char* when) {
    char phases[512] = "";
    size_t used = 0;
    for (int p = 0; p < kPhases; ++p)
        used += size_t(snprintf(phases + used, sizeof(phases) - used, "%s%s %llu (differing %llu, short %llu)",
                                p ? "; " : "", kPhaseNames[p], g_checked[p].load(), g_mismatched[p].load(),
                                g_short[p].load()));
    LOG("Havok audit %s: islands checked %s; unreadable %llu; solves %llu, buffer size changed since it was allocated "
        "%llu, overlapping solver memory %llu; repaired %llu (buffer swapped %llu); splits in jobs %llu; collide tasks "
        "%llu (too big to check %llu): handing on other than their constraints moved %llu, with a constraint in another "
        "island %llu, with an agent between two islands %llu, after their post-collide job %llu; island merges asked "
        "for inside the jobs %llu", when, phases, g_unreadable.load(), g_solves.load(), g_size_changed.load(),
        g_overlaps.load(), g_repaired.load(), g_swapped.load(), g_splits.load(), g_tasks.load(), g_task_too_big.load(),
        g_task_mismatches.load(), g_tasks_elsewhere.load(), g_tasks_split_agent.load(), g_tasks_late.load(),
        g_merges_in_jobs.load());
}

void check_world(Phase phase) {
    const uint8_t* world = rfg::global<uint8_t*>(r::Havok_world);
    if (!world) return;
    if (phase == kBeforeStep) g_step.fetch_add(1, std::memory_order_relaxed);
    check_list(phase, world, kWorldActive);
    check_list(phase, world, kWorldInactive);
    const int64_t now = session::qpc();
    if (phase == kAfterStep && session::ticks_to_ms(now - g_last_summary) >= kSummaryMs) {
        if (g_last_summary) log_summary("so far");
        g_last_summary = now;
    }
}

// ---- solves ---------------------------------------------------------------------------------------------------

// The island a thread is solving, for a crash report from inside the solver.
struct SolveContext {
    const uint8_t* island;
    const uint8_t* buffer;
    int size;
    const uint8_t* schemas;
    const uint8_t* accumulators;
    const uint8_t* temps;
    Sums sums;
    Totals totals;
};
thread_local SolveContext t_solve{};

struct ActiveSolve {
    const uint8_t* island;
    uintptr_t begin, end;
    DWORD thread;
};
SRWLOCK g_active_lock = SRWLOCK_INIT;
ActiveSolve g_active[kMaxSolves];

void enter_solve(const uint8_t* island, const uint8_t* buffer, int size) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(buffer), end = begin + uintptr_t(size > 0 ? size : 0);
    const DWORD tid = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_active_lock);
    int free_slot = -1;
    for (int i = 0; i < kMaxSolves; ++i) {
        const ActiveSolve& a = g_active[i];
        if (!a.thread) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (begin < a.end && a.begin < end) {
            const uint64_t n = g_overlaps.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 20)
                LOG("Havok audit: island %p is solved in solver memory %p-%p while island %p (thread %lu) uses "
                    "%p-%p", static_cast<const void*>(island), reinterpret_cast<void*>(begin),
                    reinterpret_cast<void*>(end), static_cast<const void*>(a.island), a.thread,
                    reinterpret_cast<void*>(a.begin), reinterpret_cast<void*>(a.end));
        }
    }
    if (free_slot >= 0) g_active[free_slot] = {island, begin, end, tid};
    ReleaseSRWLockExclusive(&g_active_lock);
}

void leave_solve() {
    const DWORD tid = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_active_lock);
    for (ActiveSolve& a : g_active)
        if (a.thread == tid) a = {};
    ReleaseSRWLockExclusive(&g_active_lock);
}

// Test.HavokAudit=2: a buffer for a solve whose island needed more than the integrate job allocated (per thread,
// kept for the next one).
thread_local uint8_t* t_spare = nullptr;
thread_local int t_spare_size = 0;

uint8_t* spare_buffer(int size) {
    if (t_spare_size >= size) return t_spare;
    if (t_spare) VirtualFree(t_spare, 0, MEM_RELEASE);
    const int rounded = (size + 0xFFFF) & ~0xFFFF;
    t_spare = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(rounded), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    t_spare_size = t_spare ? rounded : 0;
    return t_spare;
}

int calc_buffer_size(const uint8_t* island) {
    return rfg::fn<int(__cdecl*)(const void*)>(r::hkpConstraintSolverSetup__calcBufferSize)(island);
}

// hkpConstraintSolverSetup::solve(stepInfo, solverInfo, queryIn, island, buffer, bufferSize, entities, numEntities):
// lays the buffer out from the island's totals, builds the jacobians, solves (hkSolveConstraints), integrates.
using SolveFn = int(__cdecl*)(void*, void*, void*, uint8_t*, uint8_t*, int, void*, int);
SolveFn o_solve = nullptr;
int __cdecl d_solve(void* step_info, void* solver_info, void* query_in, uint8_t* island, uint8_t* buffer, int size,
                    void* entities, int num_entities) {
    g_solves.fetch_add(1, std::memory_order_relaxed);
    SolveContext& ctx = t_solve;
    ctx = {};
    ctx.island = island;
    if (island && check(kAtSolve, island, ctx.sums, ctx.totals)) {
        const int now = calc_buffer_size(island);
        if (now != size) {
            const uint64_t n = g_size_changed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 20)
                LOG("Havok audit: island %p (%d bodies) comes to its solve with a %d-byte buffer; its totals now ask "
                    "for %d", static_cast<const void*>(island), ctx.totals.num_entities, size, now);
        }
        if (g_mode >= 2 && short_of(ctx.sums, ctx.totals)) {
            auto* info = reinterpret_cast<uint32_t*>(island + kIslandInfo);
            info[0] = std::max(info[0], ctx.sums.max_schema);
            info[1] = std::max(info[1], ctx.sums.schemas);
            info[3] = std::max(info[3], ctx.sums.temps);
            auto* num = reinterpret_cast<int*>(island + kIslandNumConstraints);
            *num = std::max(*num, int(ctx.sums.constraints));
            g_repaired.fetch_add(1, std::memory_order_relaxed);
            const int need = calc_buffer_size(island);
            if (need > size) {
                if (uint8_t* spare = spare_buffer(need)) {
                    buffer = spare;
                    size = need;
                    g_swapped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    ctx.buffer = buffer;
    ctx.size = size;
    enter_solve(island, buffer, size);
    const int result = o_solve(step_info, solver_info, query_in, island, buffer, size, entities, num_entities);
    leave_solve();
    ctx.island = nullptr;
    return result;
}

// hkSolveConstraints(solverInfo, schemas, accumulators, elemTemp): where the island's schemas and temporaries begin.
using SolveConstraintsFn = bool(__cdecl*)(void*, uint8_t*, uint8_t*, uint8_t*);
SolveConstraintsFn o_solve_constraints = nullptr;
bool __cdecl d_solve_constraints(void* solver_info, uint8_t* schemas, uint8_t* accumulators, uint8_t* temps) {
    SolveContext& ctx = t_solve;
    ctx.schemas = schemas;
    ctx.accumulators = accumulators;
    ctx.temps = temps;
    return o_solve_constraints(solver_info, schemas, accumulators, temps);
}

// ---- crash report ---------------------------------------------------------------------------------------------

struct CrashSite {
    const uint32_t* rva;
    uint32_t size;
    const char* name;
};

const char* crash_site(uint32_t rva, uint32_t& offset) {
    const CrashSite sites[] = {
        {&r::hkSolveConstraints, rfg::size::hkSolveConstraints, "hkSolveConstraints"},
        {&r::hkpConstraintSolverSetup__solve, rfg::size::hkpConstraintSolverSetup__solve,
         "hkpConstraintSolverSetup::solve"},
        {&r::hkpConstraintSolverSetup___buildJacobianElements, rfg::size::hkpConstraintSolverSetup___buildJacobianElements,
         "hkpConstraintSolverSetup::_buildJacobianElements"},
        {&r::hkSolverBuildJacobianFromAtomsNotContact, rfg::size::hkSolverBuildJacobianFromAtomsNotContact,
         "hkSolverBuildJacobianFromAtomsNotContact"},
        {&r::hkpWorldConstraintUtil__removeConstraint, rfg::size::hkpWorldConstraintUtil__removeConstraint,
         "hkpWorldConstraintUtil::removeConstraint"},
    };
    for (const CrashSite& site : sites)
        if (rva - *site.rva < site.size) {
            offset = rva - *site.rva;
            return site.name;
        }
    return nullptr;
}

std::atomic<int> g_crashes_reported{0};

bool copy_guarded(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void write_line(HANDLE f, const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    DWORD written = 0;
    if (n > 0) WriteFile(f, line, DWORD(n < int(sizeof(line)) ? n : int(sizeof(line)) - 1), &written, nullptr);
}

// For offline decoding of a solver crash: the island's solver block as the crash left it (havok_crash_solver.bin)
// and everything the island holds (havok_crash_island.txt: per body its motion and island, per constraint its sizes,
// owner, data class and atom bytes).
void dump_crash(const SolveContext& ctx, const CONTEXT& c) {
    const std::wstring dir = session::run_dir() + L"\\";
    if (ctx.buffer && ctx.size > 0) {
        HANDLE f = CreateFileW((dir + L"havok_crash_solver.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(f, ctx.buffer, DWORD(ctx.size), &written, nullptr);
            CloseHandle(f);
        }
    }
    if (!ctx.island) return;
    HANDLE f = CreateFileW((dir + L"havok_crash_island.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const auto base = reinterpret_cast<uintptr_t>(ctx.buffer);
    write_line(f, "island %p, buffer %p size %d, schemas +%d, temps +%d, accumulators +%d, eip rva %X, esi +%lld edi "
               "+%lld ebx +%lld\n", static_cast<const void*>(ctx.island), static_cast<const void*>(ctx.buffer),
               ctx.size, ctx.schemas ? int(ctx.schemas - ctx.buffer) : -1, ctx.temps ? int(ctx.temps - ctx.buffer) : -1,
               ctx.accumulators ? int(ctx.accumulators - ctx.buffer) : -1, uint32_t(c.Eip - rfg::base()),
               int64_t(c.Esi) - int64_t(base), int64_t(c.Edi) - int64_t(base), int64_t(c.Ebx) - int64_t(base));
    write_line(f, "totals at the solve: constraints %d, info %u %u %u %u, bodies %d; sums constraints %u schemas %u "
               "results %u temps %u largest %u\n", ctx.totals.num_constraints, ctx.totals.info[0],
               ctx.totals.info[1], ctx.totals.info[2], ctx.totals.info[3], ctx.totals.num_entities,
               ctx.sums.constraints, ctx.sums.schemas, ctx.sums.results, ctx.sums.temps, ctx.sums.max_schema);
    uint8_t head[0x58];
    if (!copy_guarded(head, ctx.island, sizeof(head))) {
        CloseHandle(f);
        return;
    }
    const auto* entities = *reinterpret_cast<const uint8_t* const* const*>(head + kIslandEntities);
    const int n = *reinterpret_cast<const int*>(head + kIslandNumEntities);
    for (int i = 0; i < n && i < 100000; ++i) {
        const uint8_t* e = nullptr;
        uint8_t ent[0xD0];
        if (!copy_guarded(&e, entities + i, sizeof(e)) || !copy_guarded(ent, e, sizeof(ent))) {
            write_line(f, "body %d unreadable\n", i);
            continue;
        }
        const auto* masters = *reinterpret_cast<const uint8_t* const*>(ent + kEntityMasters);
        const int count = *reinterpret_cast<const uint16_t*>(ent + kEntityNumMasters);
        uint8_t motion = 0;
        copy_guarded(&motion, e + 0xE8, 1);
        write_line(f, "body %d %p motion %u island %p masters %d\n", i, static_cast<const void*>(e), motion,
                   *reinterpret_cast<void* const*>(ent + kEntityIsland), count);
        for (int j = 0; j < count; ++j) {
            uint8_t in[kInternalSize], inst[0x38];
            const uint8_t* m = masters + j * kInternalSize;
            if (!copy_guarded(in, m, sizeof(in))) {
                write_line(f, "  constraint %d unreadable\n", j);
                continue;
            }
            const uint8_t* cst = *reinterpret_cast<const uint8_t* const*>(in);
            const uint8_t* atoms = *reinterpret_cast<const uint8_t* const*>(in + 0x0C);
            const uint16_t atoms_size = *reinterpret_cast<const uint16_t*>(in + 0x10);
            uint32_t vtable = 0;
            const bool inst_ok = copy_guarded(inst, cst, sizeof(inst));
            const uint8_t* data = inst_ok ? *reinterpret_cast<const uint8_t* const*>(inst + 0x0C) : nullptr;
            if (data) copy_guarded(&vtable, data, 4);
            write_line(f, "  c %p internal %p type %u master %u atoms %p size %u schemas %u results %u temps %u; "
                       "owner %p internal %p modifiers %p entities %p %p data %p vtable rva %X uid %u%s\n",
                       static_cast<const void*>(cst), static_cast<const void*>(m), in[kInternalType], in[0x20],
                       static_cast<const void*>(atoms), atoms_size, *reinterpret_cast<const uint32_t*>(in + kInternalSchemas),
                       *reinterpret_cast<const uint16_t*>(in + kInternalResults),
                       *reinterpret_cast<const uint32_t*>(in + kInternalTemps),
                       inst_ok ? *reinterpret_cast<void* const*>(inst + kConstraintOwner) : nullptr,
                       inst_ok ? *reinterpret_cast<void* const*>(inst + kConstraintInternal) : nullptr,
                       inst_ok ? *reinterpret_cast<void* const*>(inst + 0x10) : nullptr,
                       inst_ok ? *reinterpret_cast<void* const*>(inst + 0x14) : nullptr,
                       inst_ok ? *reinterpret_cast<void* const*>(inst + 0x18) : nullptr,
                       static_cast<const void*>(data), vtable ? uint32_t(vtable - rfg::base()) : 0u,
                       inst_ok ? *reinterpret_cast<const uint32_t*>(inst + 0x34) : 0u, inst_ok ? "" : " (unreadable)");
            uint8_t bytes[256];
            const size_t take = atoms_size < sizeof(bytes) ? atoms_size : sizeof(bytes);
            if (atoms && take && copy_guarded(bytes, atoms, take)) {
                char hex[sizeof(bytes) * 2 + 1];
                for (size_t k = 0; k < take; ++k) snprintf(hex + 2 * k, 3, "%02X", bytes[k]);
                write_line(f, "    atoms %s\n", hex);
            }
        }
    }
    CloseHandle(f);
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_BREAKPOINT && code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t rva = uint32_t(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) - rfg::base());
    uint32_t offset = 0;
    const char* site = crash_site(rva, offset);
    if (!site || g_crashes_reported.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
    const SolveContext& ctx = t_solve;
    const CONTEXT& c = *ep->ContextRecord;
    const auto rel = [&](DWORD reg) { return int64_t(reg) - int64_t(reinterpret_cast<uintptr_t>(ctx.buffer)); };
    LOG("Havok audit: exception %08lX in %s+0x%X on thread %lu; island %p, solver buffer %p (%d bytes), schemas at "
        "+%lld, temporaries at +%lld, accumulators at +%lld; esi +%lld, edi +%lld, ebx +%lld, ecx +%lld",
        code, site, offset, GetCurrentThreadId(), static_cast<const void*>(ctx.island),
        static_cast<const void*>(ctx.buffer), ctx.size,
        ctx.schemas ? int64_t(ctx.schemas - ctx.buffer) : -1, ctx.temps ? int64_t(ctx.temps - ctx.buffer) : -1,
        ctx.accumulators ? int64_t(ctx.accumulators - ctx.buffer) : -1, rel(c.Esi), rel(c.Edi), rel(c.Ebx),
        rel(c.Ecx));
    if (ctx.island) {
        report(kAtSolve, ctx.island, ctx.sums, ctx.totals);
        Sums now;
        Totals t;
        if (read_island(ctx.island, now, t)) {
            LOG("Havok audit: that island now: constraints %d (sum %u), schema bytes %u (sum %u), temps %u (sum %u), "
                "%d bodies", t.num_constraints, now.constraints, t.info[1], now.schemas, t.info[3], now.temps,
                t.num_entities);
        }
    }
    log_summary("at the crash");
    if (g_crashes_reported.load() == 1) {
        dump_crash(ctx, c);
        LOG("Havok audit: the island and its solver block are in havok_crash_island.txt and havok_crash_solver.bin");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- inside the jobs ------------------------------------------------------------------------------------------

// hkCpuPostCollideJob(mtThreadStructure, queue, job) replays an island's collide tasks into it: job +0x14 the island,
// +0x20 its hkpAgentSectorHeader (task count, then per task a slot: an hkpConstraintInfo delta, then a command queue,
// +0x10 its start relative to that field and +0x14 its bytes, 16 per command: type 0 add / 1 remove, constraint at +4).
// What a replay is about to do, read before it runs (it frees the header).
struct PostCollidePlan {
    int tasks;
    int64_t schemas, results, temps;
    uint32_t adds, removes, add_owned, add_elsewhere, remove_unowned, remove_elsewhere;
};

const uint8_t* master_island(const uint8_t* constraint) {
    const uint8_t* entities[2] = {};
    uint8_t motion = 0;
    if (!copy_guarded(entities, constraint + 0x14, sizeof(entities)) || !entities[0]) return nullptr;
    copy_guarded(&motion, entities[0] + 0xE8, 1);
    const uint8_t* master = motion == 5 /* fixed */ ? entities[1] : entities[0];
    const uint8_t* island = nullptr;
    if (master) copy_guarded(&island, master + kEntityIsland, sizeof(island));
    return island;
}

bool read_plan(const uint8_t* job, const uint8_t* island, PostCollidePlan& p) {
    const uint8_t* header = nullptr;
    if (!copy_guarded(&header, job + 0x20, sizeof(header)) || !header) return false;
    if (!copy_guarded(&p.tasks, header, sizeof(p.tasks)) || p.tasks < 0 || p.tasks > 100000) return false;
    for (int i = 0; i < p.tasks; ++i) {
        const uint8_t* slot = nullptr;
        int32_t info[4];
        uint32_t queue[2];
        if (!copy_guarded(&slot, header + 0x14 + 4 * i, sizeof(slot)) || !slot ||
            !copy_guarded(info, slot, sizeof(info)) || !copy_guarded(queue, slot + 0x10, sizeof(queue)))
            continue;
        p.schemas += info[1];
        p.results += info[2];
        p.temps += info[3];
        const uint8_t* commands = slot + 0x10 + queue[0];
        for (uint32_t off = 0; off + 16 <= queue[1] && off < 0x10000; off += 16) {
            uint8_t c[16];
            if (!copy_guarded(c, commands + off, sizeof(c))) break;
            const uint8_t* constraint = *reinterpret_cast<const uint8_t* const*>(c + 4);
            const void* owner = nullptr;
            copy_guarded(&owner, constraint + kConstraintOwner, sizeof(owner));
            if (c[0] == 0) {
                ++p.adds;
                if (owner) ++p.add_owned;
                else if (master_island(constraint) != island) ++p.add_elsewhere;
            } else if (c[0] == 1) {
                ++p.removes;
                if (!owner) ++p.remove_unowned;
                else if (owner != island) ++p.remove_elsewhere;
            }
        }
    }
    return true;
}

std::atomic<uint32_t> g_plans_logged{0};

using PostCollideFn = int(__cdecl*)(void*, void*, uint8_t*);
PostCollideFn o_post_collide = nullptr;
int __cdecl d_post_collide(void* mt, void* queue, uint8_t* job) {
    const uint8_t* island = nullptr;
    if (job) copy_guarded(&island, job + 0x14, sizeof(island));
    PostCollidePlan plan{};
    Totals before{};
    Sums before_sums;
    const bool planned = island && read_plan(job, island, plan);
    if (planned && read_island(island, before_sums, before)) {
        history(kEvPostCollideIn, island, &before_sums, before);
        if (g_history && before.num_entities >= kHistoryBodies) {
            // the plan: tasks, additions, removals, the deltas' schema bytes
            if (HistoryRecord* r = history_slot()) {
                r->event = kEvPlan;
                r->island = island;
                r->bodies = before.num_entities;
                copy_guarded(&r->header, job + 0x20, sizeof(r->header));
                if (r->header) {
                    copy_guarded(&r->total_tasks, r->header, sizeof(r->total_tasks));
                    copy_guarded(&r->open_jobs, r->header + 4, sizeof(r->open_jobs));
                }
                r->a = plan.tasks;
                r->b = int32_t(plan.adds);
                r->c = int32_t(plan.removes);
                r->d = int32_t(plan.schemas);
            }
        }
    }
    const int result = o_post_collide(mt, queue, job);  // the job entry now holds the next job
    if (!island) return result;
    Sums s;
    Totals t;
    const bool read = check(kAfterPostCollide, island, s, t);
    if (read) history(kEvPostCollideOut, island, &s, t);
    if (read && differs(s, t) && planned &&
        g_plans_logged.fetch_add(1, std::memory_order_relaxed) < 20)
        LOG("Havok audit:   that post-collide job (thread %lu): %d tasks with deltas schemas %lld results %lld temps %lld; "
            "%u additions (%u for a constraint already in an island, %u into another island), %u removals (%u of a "
            "constraint in none, %u from another island); before it the island had constraints %d (sum %u), schema "
            "bytes %u (sum %u)", GetCurrentThreadId(), plan.tasks, plan.schemas, plan.results, plan.temps, plan.adds,
            plan.add_owned, plan.add_elsewhere, plan.removes, plan.remove_unowned, plan.remove_elsewhere,
            before.num_constraints, before_sums.constraints, before.info[1], before_sums.schemas);
    return result;
}

// hkCpuSplitSimulationIslandJobImpl(island, newIslands): an island split at the end of its integrate job, in the job.
std::atomic<uint32_t> g_splits_logged{0};
using SplitJobFn = void(__cdecl*)(uint8_t*, uint8_t*);
SplitJobFn o_split_job = nullptr;
void __cdecl d_split_job(uint8_t* island, uint8_t* new_islands) {
    int bodies_before = 0, islands_before = 0;
    copy_guarded(&bodies_before, island + kIslandNumEntities, sizeof(bodies_before));
    if (new_islands) copy_guarded(&islands_before, new_islands + 4, sizeof(islands_before));
    o_split_job(island, new_islands);
    g_splits.fetch_add(1, std::memory_order_relaxed);
    Sums s;
    Totals t;
    check(kAfterSplit, island, s, t);
    const uint8_t* const* list = nullptr;
    int n = 0;
    if (new_islands) {
        copy_guarded(&list, new_islands, sizeof(list));
        copy_guarded(&n, new_islands + 4, sizeof(n));
    }
    int fresh_bodies = 0;
    for (int i = islands_before; list && i < n && i < 65536; ++i) {
        const uint8_t* fresh = nullptr;
        Sums fs;
        Totals ft;
        if (copy_guarded(&fresh, list + i, sizeof(fresh)) && fresh && check(kAfterSplit, fresh, fs, ft))
            fresh_bodies += ft.num_entities;
    }
    if (g_history && bodies_before >= kHistoryBodies)
        if (HistoryRecord* r = history_slot()) {
            r->event = kEvSplit;
            r->island = island;
            r->bodies = t.num_entities;
            r->constraints = t.num_constraints;
            r->sum_constraints = int(s.constraints);
            r->schemas = t.info[1];
            r->sum_schemas = s.schemas;
            r->results = t.info[2];
            r->sum_results = s.results;
            r->a = n - islands_before;
            r->b = fresh_bodies;
        }
    if (bodies_before >= 500 && g_splits_logged.fetch_add(1, std::memory_order_relaxed) < 20)
        LOG("Havok audit: an island of %d bodies split in its integrate job (thread %lu): %d bodies stay, %d new islands",
            bodies_before, GetCurrentThreadId(), t.num_entities, n - islands_before);
}

// hkCpuAgentSectorJob(mtThreadStructure, queue, job) runs one collide task of an island: a sector task (subtype 13)
// the agents in its sectors (+0x2C, +0x26 of them, the last one +0x44 bytes full), an entry task (subtype 15) its
// list of agents. The task's island (+0x14) is looked up when the task is popped, by the job's island index (+0x10)
// in hkpWorld::m_activeSimulationIslands. An agent (hkpAgentNnEntry: contact manager +8, collidables +0x10 and +0x14,
// each owned by the entity at collidable + its signed byte +0x10; 64 bytes times its track type, +0xE) belongs to the
// island of its bodies that are not fixed. What its contact constraint (hkpSimpleConstraintContactMgr +0x44) gains
// and loses goes into the task's hkpDeferredConstraintOwner: the totals' change of constraints already in an island,
// and the constraints that join or leave one (commands). The task hands that on: with an hkpAgentSectorHeader (+0x20)
// into its slot (+0x14 + 4 x task index (+0x24): the change, then the commands), replayed into the island by its
// post-collide job; without a header straight into the island, commands included.
// Per task: its constraints' own totals (hkConstraintInternal) must have moved by exactly what it handed on, and the
// constraints in an island must be in the task's. Exact with the jobs on one thread (Test.HavokJobsOnSimThread); with
// workers, the island's post-collide job can replay the task on another thread before the task's constraints are read
// back here, and a few hundred tasks a run show up as mismatched although the island ends up right.
constexpr int kMaxTaskAgents = 64;  // Havok's tasks have at most 16 (2 midphase sectors of 8, 4 narrowphase of 4)
constexpr uint32_t kMgrConstraint = 0x44, kMgrAtom = 0x3C, kAtomNumPoints = 0x04;
constexpr uint8_t kSubtypeSectors = 13, kSubtypeEntries = 15;

struct TaskAgent {
    const uint8_t* constraint;
    const uint8_t* bodies_island;  // the island of its bodies that are not fixed (null: none)
    const uint8_t* owner;          // the constraint's island
    bool split;                    // its two bodies are in two islands
    bool in_island;                // the constraint has an hkConstraintInternal
    uint32_t schemas, results, temps;
    uint16_t points;
};

struct Task {
    uint8_t subtype;
    const uint8_t* island;
    const uint8_t* header;
    int16_t index;
    int header_tasks, header_open;  // the header's m_numTotalTasks and m_openJobs when the task starts
    int agents;  // with a contact constraint; the first kMaxTaskAgents are kept
    uint32_t island_info[4];
    TaskAgent a[kMaxTaskAgents];
};


std::atomic<uint32_t> g_tasks_logged{0};

const uint8_t* entity_island(const uint8_t* collidable) {
    int8_t offset = 0;
    uint8_t motion = 0;
    const uint8_t* island = nullptr;
    if (!collidable || !copy_guarded(&offset, collidable + 0x10, 1)) return nullptr;
    const uint8_t* entity = collidable + offset;
    if (!copy_guarded(&motion, entity + kEntityMotionType, 1) || motion == kMotionFixed) return nullptr;
    copy_guarded(&island, entity + kEntityIsland, sizeof(island));
    return island;
}

void read_constraint_state(TaskAgent& t) {
    const uint8_t* internal = nullptr;
    const uint8_t* atom = nullptr;
    t.owner = nullptr;
    t.schemas = t.results = t.temps = 0;
    t.points = 0;
    copy_guarded(&t.owner, t.constraint + kConstraintOwner, sizeof(t.owner));
    copy_guarded(&internal, t.constraint + kConstraintInternal, sizeof(internal));
    t.in_island = internal != nullptr;
    if (internal) {
        uint16_t results = 0;
        copy_guarded(&results, internal + kInternalResults, sizeof(results));
        copy_guarded(&t.schemas, internal + kInternalSchemas, sizeof(t.schemas));
        copy_guarded(&t.temps, internal + kInternalTemps, sizeof(t.temps));
        t.results = results;
    }
    if (copy_guarded(&atom, t.constraint - kMgrConstraint + kMgrAtom, sizeof(atom)) && atom)
        copy_guarded(&t.points, atom + kAtomNumPoints, sizeof(t.points));
}

void add_agent(Task& t, const uint8_t* entry) {
    const uint8_t* mgr = nullptr;
    uintptr_t vtable = 0;
    if (!copy_guarded(&mgr, entry + 8, sizeof(mgr)) || !mgr || !copy_guarded(&vtable, mgr, sizeof(vtable)) ||
        vtable != reinterpret_cast<uintptr_t>(rfg::addr(r::hkpSimpleConstraintContactMgr_vftable)))
        return;
    if (t.agents++ >= kMaxTaskAgents) return;
    TaskAgent& a = t.a[t.agents - 1];
    a = {};
    a.constraint = mgr + kMgrConstraint;
    const uint8_t* coll[2] = {};
    copy_guarded(coll, entry + 0x10, sizeof(coll));
    const uint8_t* first = entity_island(coll[0]);
    const uint8_t* second = entity_island(coll[1]);
    a.bodies_island = first ? first : second;
    a.split = first && second && first != second;
    read_constraint_state(a);
}

void gather_task(const uint8_t* job, Task& t) {
    t.subtype = 0;
    t.island = t.header = nullptr;
    t.index = 0;
    t.header_tasks = t.header_open = 0;
    t.agents = 0;
    uint16_t count = 0, last_bytes = 0;
    const uint8_t* const* elements = nullptr;
    copy_guarded(&t.subtype, job, 1);
    copy_guarded(&t.island, job + 0x14, sizeof(t.island));
    copy_guarded(&t.header, job + 0x20, sizeof(t.header));
    copy_guarded(&t.index, job + 0x24, sizeof(t.index));
    copy_guarded(&count, job + 0x26, sizeof(count));
    copy_guarded(&elements, job + 0x2C, sizeof(elements));
    copy_guarded(&last_bytes, job + 0x44, sizeof(last_bytes));
    if (!t.header && t.island) copy_guarded(t.island_info, t.island + kIslandInfo, sizeof(t.island_info));
    if (t.header) {
        copy_guarded(&t.header_tasks, t.header, sizeof(t.header_tasks));
        copy_guarded(&t.header_open, t.header + 4, sizeof(t.header_open));
    }
    if (!elements) return;
    for (int i = 0; i < count; ++i) {
        const uint8_t* element = nullptr;
        if (!copy_guarded(&element, elements + i, sizeof(element)) || !element) break;
        if (t.subtype == kSubtypeEntries) {
            add_agent(t, element);
            continue;
        }
        if (t.subtype != kSubtypeSectors) return;
        uint8_t track = 0;  // the first entry's track type: 64 bytes per agent and type
        copy_guarded(&track, element + 0xE, 1);
        const int stride = track << 6, end = i == count - 1 ? last_bytes : 0x200;
        if (stride <= 0) break;
        for (int offset = 0; offset + stride <= end; offset += stride) add_agent(t, element + offset);
    }
}

void account_task(Task& t) {
    g_tasks.fetch_add(1, std::memory_order_relaxed);
    if (!t.island) return;
    if (t.agents > kMaxTaskAgents) {
        g_task_too_big.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    int64_t moved[4] = {}, handed[4] = {};  // [1] schema bytes, [2] solver results, [3] temps
    int elsewhere = 0, split = 0, misplaced = 0, joined = 0, left = 0;
    int first = -1;
    TaskAgent after[kMaxTaskAgents];
    for (int i = 0; i < t.agents; ++i) {
        const TaskAgent& before = t.a[i];
        after[i] = before;
        read_constraint_state(after[i]);
        const TaskAgent& now = after[i];
        if (before.in_island && now.in_island) {
            moved[1] += int64_t(now.schemas) - before.schemas;
            moved[2] += int64_t(now.results) - before.results;
            moved[3] += int64_t(now.temps) - before.temps;
        }
        joined += !before.in_island && now.in_island;
        left += before.in_island && !now.in_island;
        split += before.split;
        misplaced += before.bodies_island && before.bodies_island != t.island;
        if (before.owner && before.owner != t.island) {
            ++elsewhere;
            if (first < 0) first = i;
        }
    }
    uint32_t command_bytes = 0;
    if (t.header) {
        const uint8_t* slot = nullptr;
        int32_t info[4] = {};
        if (copy_guarded(&slot, t.header + 0x14 + 4 * t.index, sizeof(slot)) && slot &&
            copy_guarded(info, slot, sizeof(info)))
            for (int k = 1; k < 4; ++k) handed[k] = info[k];
        if (slot) copy_guarded(&command_bytes, slot + 0x14, sizeof(command_bytes));
    } else {
        // Straight into the island: the change, and the constraints that joined or left it.
        uint32_t now[4] = {};
        copy_guarded(now, t.island + kIslandInfo, sizeof(now));
        for (int k = 1; k < 4; ++k) handed[k] = int64_t(now[k]) - int64_t(t.island_info[k]);
        for (int i = 0; i < t.agents; ++i) {
            const TaskAgent& b = t.a[i];
            const TaskAgent& a = after[i];
            if (!b.in_island && a.in_island && a.owner == t.island) {
                handed[1] -= a.schemas;
                handed[2] -= a.results;
                handed[3] -= a.temps;
            } else if (b.in_island && !a.in_island && b.owner == t.island) {
                // It left with what it had at the end: its totals before plus the points it lost on the way.
                handed[1] += b.schemas;
                handed[2] += b.results;
                handed[3] += b.temps;
            }
        }
    }
    const bool mismatch = moved[1] != handed[1] || moved[2] != handed[2] || moved[3] != handed[3];
    const bool late = t.header && t.subtype == kSubtypeSectors && t.header_open <= 0;
    if (mismatch) g_task_mismatches.fetch_add(1, std::memory_order_relaxed);
    if (elsewhere) g_tasks_elsewhere.fetch_add(1, std::memory_order_relaxed);
    if (split) g_tasks_split_agent.fetch_add(1, std::memory_order_relaxed);
    if (late) g_tasks_late.fetch_add(1, std::memory_order_relaxed);
    int bodies = 0;
    uint32_t info[4] = {};
    copy_guarded(&bodies, t.island + kIslandNumEntities, sizeof(bodies));
    if (g_history && bodies >= kHistoryBodies && copy_guarded(info, t.island + kIslandInfo, sizeof(info)))
        if (HistoryRecord* r = history_slot()) {
            r->event = kEvTask;
            r->island = t.island;
            r->header = t.header;
            r->subtype = t.subtype;
            r->task = t.index;
            r->open_jobs = int16_t(t.header_open);
            r->total_tasks = int16_t(t.header_tasks);
            r->bodies = bodies;
            copy_guarded(&r->constraints, t.island + kIslandNumConstraints, sizeof(r->constraints));
            r->sum_constraints = -1;
            r->schemas = info[1];
            r->results = info[2];
            r->a = int32_t(handed[1]);
            r->b = int32_t(moved[1]);
            r->c = joined;
            r->d = left;
        }
    if (!mismatch && !elsewhere && !split && !misplaced && !late) return;
    if (g_tasks_logged.fetch_add(1, std::memory_order_relaxed) >= 30) return;
    LOG("Havok audit: a collide task (subtype %u, task %d, thread %lu%s) for island %p (%d bodies), %d agents: handed on "
        "schema bytes %lld, results %lld, temps %lld (%u command bytes); its constraints moved by %lld, %lld, %lld; %d "
        "joined an island, %d left one; constraints in another island %d, agents between two islands %d, agents whose "
        "bodies are in another island %d; its header %p had %d of %d tasks open when it started%s", t.subtype, t.index,
        GetCurrentThreadId(), t.header ? "" : ", no header", static_cast<const void*>(t.island), bodies, t.agents,
        handed[1], handed[2], handed[3], command_bytes, moved[1], moved[2], moved[3], joined, left, elsewhere, split,
        misplaced, static_cast<const void*>(t.header), t.header_open, t.header_tasks,
        late ? " (after its post-collide job: what it hands on is lost)" : "");
    for (int i = 0, shown = 0; i < t.agents && shown < 6; ++i) {
        const TaskAgent& b = t.a[i];
        const TaskAgent& a = after[i];
        const bool changed = b.schemas != a.schemas || b.in_island != a.in_island || b.points != a.points;
        if (!changed && !(b.owner && b.owner != t.island) && !b.split) continue;
        ++shown;
        LOG("Havok audit:   constraint %p: island %p -> %p, points %u -> %u, schema bytes %u -> %u, results %u -> %u; "
            "its bodies' island %p%s", static_cast<const void*>(b.constraint), static_cast<const void*>(b.owner),
            static_cast<const void*>(a.owner), b.points, a.points, b.schemas, a.schemas, b.results, a.results,
            static_cast<const void*>(b.bodies_island), b.split ? " (and another)" : "");
    }
}

using AgentSectorFn = int(__cdecl*)(void*, void*, uint8_t*);
AgentSectorFn o_agent_sector = nullptr;
thread_local Task t_task;  // ~3 KB: off the stack
int __cdecl d_agent_sector(void* mt, void* queue, uint8_t* job) {
    Task& task = t_task;
    if (job) gather_task(job, task);
    const int result = o_agent_sector(mt, queue, job);  // the job entry now holds the next job
    if (job) account_task(task);
    return result;
}

// hkpSingleThreadedJobsOnIsland::cpuBroadPhaseJob(mtThreadStructure, queue, job): an island's broadphase, after its
// integrate job and before its collide tasks. It removes the agents of pairs that ended (their constraints leave the
// island at once) and adds agents for new pairs; the island's totals and its constraints' must move together.
// Islands split in the integrate job join the job's island list here (hkpBroadPhaseJob::popJobTask).
using BroadPhaseFn = int(__cdecl*)(void*, void*, uint8_t*);
BroadPhaseFn o_broad_phase = nullptr;
std::atomic<uint32_t> g_broad_phase_logged{0};
int __cdecl d_broad_phase(void* mt, void* queue, uint8_t* job) {
    const uint8_t* island = nullptr;
    if (job) copy_guarded(&island, job + 0x14, sizeof(island));
    Sums before_sums, after_sums;
    Totals before{}, after{};
    const bool read = island && read_island(island, before_sums, before);
    if (read) history(kEvBroadPhaseIn, island, &before_sums, before);
    const int result = o_broad_phase(mt, queue, job);
    if (!read || !read_island(island, after_sums, after)) return result;
    history(kEvBroadPhaseOut, island, &after_sums, after);
    const int64_t gap_before = int64_t(before.info[1]) - before_sums.schemas;
    const int64_t gap_after = int64_t(after.info[1]) - after_sums.schemas;
    if ((gap_before || gap_after) && g_broad_phase_logged.fetch_add(1, std::memory_order_relaxed) < 20)
        LOG("Havok audit: island %p (%d bodies) at its broadphase job (thread %lu): schema bytes %u (sum %u) before, %u "
            "(sum %u) after; constraints %d (sum %u) before, %d (sum %u) after", static_cast<const void*>(island),
            after.num_entities, GetCurrentThreadId(), before.info[1], before_sums.schemas, after.info[1],
            after_sums.schemas, before.num_constraints, before_sums.constraints, after.num_constraints,
            after_sums.constraints);
    return result;
}

// hkpWorldOperationUtil::mergeIslands(world, entityA, entityB): what a constraint between bodies of two islands asks
// for (hkpWorldConstraintUtil::addConstraint). Inside the jobs critical operations are locked and the merge is queued
// to the step's end, while the constraint goes into one of the two islands at once.
using MergeIslandsFn = void(__cdecl*)(void*, uint8_t*, uint8_t*);
MergeIslandsFn o_merge_islands = nullptr;
void __cdecl d_merge_islands(void* world, uint8_t* a, uint8_t* b) {
    if (havok_mt_in_jobs()) {
        const uint64_t n = g_merges_in_jobs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 20) {
            const uint8_t* islands[2] = {};
            int bodies[2] = {};
            copy_guarded(&islands[0], a + kEntityIsland, sizeof(islands[0]));
            copy_guarded(&islands[1], b + kEntityIsland, sizeof(islands[1]));
            for (int i = 0; i < 2; ++i)
                if (islands[i]) copy_guarded(&bodies[i], islands[i] + kIslandNumEntities, sizeof(bodies[i]));
            LOG("Havok audit: islands %p (%d bodies) and %p (%d bodies) to merge inside the jobs (thread %lu), for "
                "bodies %p and %p; caller +%X", static_cast<const void*>(islands[0]), bodies[0],
                static_cast<const void*>(islands[1]), bodies[1], GetCurrentThreadId(), static_cast<const void*>(a),
                static_cast<const void*>(b), uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress()) - rfg::base()));
        }
    }
    o_merge_islands(world, a, b);
}

// ---- the step's edges -----------------------------------------------------------------------------------------

// havok_update_status runs right before hkpWorld::stepDeltaTime in havok_simulate, havok_sim_islands_process_post
// right after it (both on the sim thread, outside the step).
void(__cdecl* o_update_status)() = nullptr;
void __cdecl d_update_status() {
    o_update_status();
    check_world(kBeforeStep);
}

void(__cdecl* o_islands_post)() = nullptr;
void __cdecl d_islands_post() {
    check_world(kAfterStep);
    o_islands_post();
}

}  // namespace

void havok_audit_after_jobs() {
    if (g_mode > 0) check_world(kAfterJobs);
}

bool install_havok_audit(const Config& cfg) {
    g_mode = cfg.test_havok_audit;
    if (g_mode <= 0) return true;
    bool ok = hooks::create(r::hkpConstraintSolverSetup__solve, &d_solve, &o_solve, "hkpConstraintSolverSetup::solve");
    ok &= hooks::create(r::hkSolveConstraints, &d_solve_constraints, &o_solve_constraints, "hkSolveConstraints");
    ok &= hooks::create(r::havok_update_status, &d_update_status, &o_update_status, "havok_update_status");
    ok &= hooks::create(r::havok_sim_islands_process_post, &d_islands_post, &o_islands_post,
                        "havok_sim_islands_process_post");
    ok &= hooks::create(r::hkCpuAgentSectorJob, &d_agent_sector, &o_agent_sector, "hkCpuAgentSectorJob");
    ok &= hooks::create(r::hkpWorldOperationUtil__mergeIslands, &d_merge_islands, &o_merge_islands,
                        "hkpWorldOperationUtil::mergeIslands");
    // The checks after each post-collide job, broadphase job and split cost the jobs enough time to hide the
    // corruption they look for when the jobs run on every thread.
    if (cfg.test_havok_audit_jobs) {
        g_ring = static_cast<HistoryRecord*>(VirtualAlloc(nullptr, kHistorySize * sizeof(HistoryRecord),
                                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        g_history = g_ring != nullptr;
        ok &= hooks::create(r::hkCpuPostCollideJob, &d_post_collide, &o_post_collide, "hkCpuPostCollideJob");
        ok &= hooks::create(r::hkCpuSplitSimulationIslandJobImpl, &d_split_job, &o_split_job,
                            "hkCpuSplitSimulationIslandJobImpl");
        ok &= hooks::create(r::hkpSingleThreadedJobsOnIsland__cpuBroadPhaseJob, &d_broad_phase, &o_broad_phase,
                            "hkpSingleThreadedJobsOnIsland::cpuBroadPhaseJob");
    }
    AddVectoredExceptionHandler(1, &on_exception);
    LOG("Test.HavokAudit=%d: Havok island totals checked before each island's solve and around each step%s%s", g_mode,
        g_mode >= 2 ? "; islands short of their constraints' needs are repaired" : "",
        ok ? "" : " (some hooks failed)");
    return ok;
}

}  // namespace experiments
