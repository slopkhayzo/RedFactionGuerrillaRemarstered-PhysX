// rfgphysx_host: the 64-bit PhysX side of rfgphys (research/docs/06 §3, §13).
//
// rfgphys.asi (inside the 32-bit game) creates a shared-memory mapping and starts this process with its name. The
// host steps one PhysX scene at a fixed rate, applies the game's commands before each step and publishes a snapshot
// of every body after it (src/shared/rfgphysx_ipc.h). It never waits for the game, and it exits when the game asks,
// when the game process is gone, or when the game's job object closes.
//
//   rfgphysx_host.exe --map <mapping name> [--log <file>] [--steps-csv <file>]
#include <PxPhysicsAPI.h>
#include <gpu/PxGpu.h>

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

#include "rfgphysx_ipc.h"

using namespace physx;
namespace ipc = rfgphysx;

namespace {

// Contact filtering: simulation filter data word0 says what a shape is (ipc::ShapeFlags), word1 what it does not
// touch. Everything else collides (no triggers here).
PxFilterFlags filter_shader(PxFilterObjectAttributes, PxFilterData d0, PxFilterObjectAttributes, PxFilterData d1,
                            PxPairFlags& flags, const void*, PxU32) {
    if ((d0.word0 & d1.word1) || (d1.word0 & d0.word1)) return PxFilterFlag::eSUPPRESS;
    flags = PxPairFlag::eCONTACT_DEFAULT;
    return PxFilterFlag::eDEFAULT;
}

PxFilterData filter_for(uint32_t shape_flags) {
    if (shape_flags & ipc::kShapePlayer) return PxFilterData(ipc::kShapePlayer, ipc::kShapeHeavy, 0, 0);
    return PxFilterData(shape_flags & ipc::kShapeHeavy, 0, 0, 0);
}

// ---- log ------------------------------------------------------------------------------------------------------------

FILE* g_log = nullptr;
LARGE_INTEGER g_freq, g_t0;

double now_ms() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_t0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log) {
        fprintf(g_log, "[%10.3f] %s\n", now_ms() / 1000.0, buf);
        fflush(g_log);
    }
}

// ---- a stuck main loop ----------------------------------------------------------------------------------------------

// The heartbeat thread notices when the main loop has finished no step for a while and logs where it stands: the
// main thread is suspended for a moment, its stack walked, and the frames named from the host's PDB (module + offset
// for PhysX's GPU module, which has no symbols here).
std::atomic<int64_t> g_last_progress_qpc{0};  // the main loop's last finished step (or start)
HANDLE g_main_thread = nullptr;

void log_main_stack(double stuck_ms) {
    logf("main loop stuck: no step finished for %.0f s; its stack:", stuck_ms / 1000.0);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    DWORD64 frames[48];
    int n = 0;
    if (SuspendThread(g_main_thread) == DWORD(-1)) return;
    if (GetThreadContext(g_main_thread, &ctx)) {
        STACKFRAME64 f{};
        f.AddrPC.Offset = ctx.Rip, f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = ctx.Rbp, f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = ctx.Rsp, f.AddrStack.Mode = AddrModeFlat;
        while (n < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), g_main_thread, &f, &ctx, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
               f.AddrPC.Offset)
            frames[n++] = f.AddrPC.Offset;
    }
    ResumeThread(g_main_thread);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    for (int i = 0; i < n; ++i) {
        memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        char module[MAX_PATH] = "?";
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frames[i]), &mod))
            GetModuleFileNameA(mod, module, MAX_PATH);
        const char* slash = strrchr(module, 0x5C);  // the last backslash
        const char* base = slash ? slash + 1 : module;
        if (SymFromAddr(GetCurrentProcess(), frames[i], &disp, sym))
            logf("  #%-2d %s!%s+0x%llX", i, base, sym->Name, disp);
        else
            logf("  #%-2d %s+0x%llX", i, base, frames[i] - reinterpret_cast<DWORD64>(mod));
    }
}

struct ErrorCallback : PxErrorCallback {
    int reported = 0;
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        if (reported++ < 50) logf("PhysX error %d: %s (%s:%d)", int(code), message, file, line);
    }
};

PxDefaultAllocator g_allocator;
ErrorCallback g_errors;

// ---- shared memory ----------------------------------------------------------------------------------------------------

struct Shared {
    HANDLE mapping = nullptr;
    uint8_t* base = nullptr;
    ipc::Control* control = nullptr;
    uint8_t* ring = nullptr;
    uint32_t ring_mask = 0;
    uint32_t tail = 0;
    uint32_t back = 0;  // the snapshot buffer the host writes next (the game starts with 2, latest with 1)

    bool open(const wchar_t* name) {
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!mapping) return false;
        base = static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!base) return false;
        control = reinterpret_cast<ipc::Control*>(base);
        if (control->magic != ipc::kMagic || control->version != ipc::kVersion) return false;
        ring = base + control->ring_offset;
        ring_mask = control->ring_size - 1;
        tail = control->ring_tail.load(std::memory_order_acquire);
        return true;
    }

    ipc::SnapshotHeader* snapshot(uint32_t index) {
        return reinterpret_cast<ipc::SnapshotHeader*>(base + control->snapshot_offset +
                                                      size_t(index) * control->snapshot_stride);
    }

    void message(const char* text) {
        strncpy_s(control->host_message, text, _TRUNCATE);
    }
};

// ---- PhysX scene ------------------------------------------------------------------------------------------------------

struct Body {
    PxRigidDynamic* actor = nullptr;
};

struct Host {
    Shared sh;
    uint32_t max_bodies = 0;
    float dt = 1.0f / 60.0f;

    PxFoundation* foundation = nullptr;
    PxPhysics* physics = nullptr;
    PxCudaContextManager* cuda = nullptr;
    PxDefaultCpuDispatcher* dispatcher = nullptr;
    PxScene* scene = nullptr;
    PxMaterial* material = nullptr;
    bool gpu = false;

    std::vector<Body> bodies;            // by slot
    std::vector<ipc::BodyPose> poses;    // by slot: what the next snapshot publishes
    std::vector<uint32_t> awake_slots;   // slots PhysX moved in the last step
    std::vector<uint32_t> now_awake;     // scratch for step()
    std::vector<uint32_t> freeze_list;   // scratch for step()
    std::vector<uint8_t> was_awake;
    // Freezing (06 §12): a pile is one island and never falls asleep, so a piece that stays calm for kFreezeSteps
    // becomes kinematic until an explosion or the player comes near it. That cuts the pile's island apart, so the
    // rest can sleep, and the game stops moving its render instance.
    static constexpr uint16_t kFreezeSteps = 60;
    static constexpr float kCalmSpeed = 0.15f, kCalmSpin = 0.5f;
    static constexpr float kMaxDepenetration = 3.0f;  // m/s, PxRigidBody::setMaxDepenetrationVelocity of debris
    std::vector<uint16_t> calm;
    std::vector<uint8_t> frozen, user_kinematic, heavy;  // heavy: the player's capsule does not touch it
    std::vector<uint32_t> frozen_slots;  // may hold stale entries; frozen[] is the truth
    uint32_t frozen_count = 0, stat_frozen = 0, stat_thawed = 0;
    // A frozen piece does not notice when what it rests on goes, and would hang in the air. So removed statics wake
    // the frozen pieces in and above their bounds, and a woken piece that then moves (2 cm within 4 steps: it lost its
    // support or was pushed) wakes the frozen pieces touching it from above, which do the same, ring by ring. A woken
    // piece that stays put passes nothing on (otherwise the player's capsule, which wakes what it comes near ten
    // times a second, kept whole piles churning). Frozen pieces are found in a grid of 2 m cells; entries go stale
    // when they thaw, and the grid is rebuilt now and then.
    static constexpr float kCell = 2.0f, kTouch = 0.3f, kNeighbourReach = 3.0f, kMovedSq = 0.02f * 0.02f;
    static constexpr uint32_t kCascadePerStep = 400, kCascadeDelaySteps = 4;
    std::vector<ipc::Vec3> wake_pos;    // per slot: where it was when woken
    std::vector<uint32_t> wake_due;     // per slot: the step its movement is checked
    std::vector<float> body_radius;  // per slot: bounds of its shapes around its origin
    std::unordered_map<uint64_t, std::vector<uint32_t>> frozen_grid;
    std::vector<uint32_t> cascade_now, cascade_next;
    std::vector<uint8_t> cascade_mark;
    uint32_t stat_cascade = 0, stat_unsupported = 0;
    std::unordered_map<uint32_t, std::vector<PxShape*>> shapes;  // body shapes by id: one or more convex parts
    std::vector<PxRigidStatic*> statics;                                  // test planes and boxes
    // The mirrored world, one group per Havok body: its convex pieces as shapes of one static actor (found by key,
    // so a building can lose single pieces), and its meshes and height fields as actors of their own.
    struct Group {
        PxRigidStatic* pieces = nullptr;
        std::unordered_map<uint32_t, std::vector<PxShape*>> by_key;
        std::vector<PxRigidStatic*> actors;
        uint32_t shape_count = 0;
    };
    std::unordered_map<uint32_t, Group> groups;
    uint32_t static_count = 0;  // static shapes of the mirror
    double stat_cook_ms = 0;
    uint32_t stat_cooked = 0, stat_cook_failed = 0;
    std::vector<PxActor*> pending_add;   // bodies created by this batch of commands, added to the scene together
    uint32_t live = 0, slots_used = 0;

    uint32_t step_index = 0, sequence = 0, last_frame = 0, commands_since_snapshot = 0;
    double sim_time = 0;
    int64_t last_frame_qpc = 0;

    // Statistics for the periodic log line.
    uint32_t stat_steps = 0, stat_commands = 0;
    double stat_step_ms = 0, stat_step_max = 0, stat_work_ms = 0, stat_cmd_latency_ms = 0;
    uint32_t stat_cmd_latency_n = 0;
    FILE* steps_csv = nullptr;

    bool init_physx(uint32_t backend) {
        foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_allocator, g_errors);
        if (!foundation) return fail("PxCreateFoundation failed");
        if (backend == ipc::kBackendGpu) {
            PxCudaContextManagerDesc desc;
            cuda = PxCreateCudaContextManager(*foundation, desc, PxGetProfilerCallback());
            if (cuda && cuda->contextIsValid()) {
                gpu = true;
                logf("GPU: %s, %d SMs, %.0f MB", cuda->getDeviceName(), cuda->getMultiprocessorCount(),
                     double(cuda->getDeviceTotalMemBytes()) / (1024.0 * 1024.0));
            } else {
                logf("no valid CUDA context: CPU PhysX instead");
                if (cuda) cuda->release();
                cuda = nullptr;
            }
        }
        physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale(), false, nullptr);
        if (!physics) return fail("PxCreatePhysics failed");
        material = physics->createMaterial(0.8f, 0.6f, 0.1f);

        PxSceneDesc sd(physics->getTolerancesScale());
        sd.gravity = PxVec3(0, -9.81f, 0);
        // GPU dynamics leave PhysX little CPU work to spread; the CPU version takes what the game leaves over.
        dispatcher = PxDefaultCpuDispatcherCreate(gpu ? 2 : 6);
        sd.cpuDispatcher = dispatcher;
        sd.filterShader = filter_shader;
        sd.flags |= PxSceneFlag::eENABLE_PCM | PxSceneFlag::eENABLE_ACTIVE_ACTORS;
        sd.solverType = PxSolverType::eTGS;
        // Scene queries see the static world only (body shapes are simulation shapes, so thousands of moving
        // pieces cost the query structure nothing); explosions go through the host's own pose table.
        sd.staticStructure = PxPruningStructureType::eDYNAMIC_AABB_TREE;
        if (gpu) {
            sd.cudaContextManager = cuda;
            sd.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS;
            sd.broadPhaseType = PxBroadPhaseType::eGPU;
            sd.gpuDynamicsConfig.maxRigidContactCount = 2u << 20;
            sd.gpuDynamicsConfig.maxRigidPatchCount = 1u << 20;
            sd.gpuDynamicsConfig.foundLostPairsCapacity = 1u << 20;
            sd.gpuDynamicsConfig.tempBufferCapacity = 64ull << 20;
            sd.gpuDynamicsConfig.heapCapacity = 64u << 20;
            sd.gpuMaxNumPartitions = 32;
        } else {
            sd.broadPhaseType = PxBroadPhaseType::ePABP;
        }
        scene = physics->createScene(sd);
        if (!scene) return fail("createScene failed");
        logf("PhysX %d.%d.%d scene ready: %s dynamics, TGS solver, %.0f Hz steps, %u body slots", PX_PHYSICS_VERSION_MAJOR,
             PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX, gpu ? "GPU" : "CPU", 1.0 / dt, max_bodies);
        return true;
    }

    bool fail(const char* what) {
        logf("failed: %s", what);
        sh.message(what);
        sh.control->host_state.store(uint32_t(ipc::HostState::Failed), std::memory_order_release);
        return false;
    }

    // ---- commands ----

    void reset() {
        flush_pending();
        for (uint32_t s = 0; s < slots_used; ++s) release_body(s);
        for (PxRigidStatic* st : statics) st->release();
        statics.clear();
        for (auto& [g, group] : groups) release_group(group);
        groups.clear();
        static_count = 0;
        sh.control->statics.store(0, std::memory_order_relaxed);
        for (auto& [id, parts] : shapes)
            for (PxShape* shape : parts) shape->release();
        shapes.clear();
        awake_slots.clear();
        std::fill(was_awake.begin(), was_awake.end(), uint8_t(0));
        frozen_slots.clear();
        frozen_count = 0;
        slots_used = 0;
        logf("reset: scene emptied");
    }

    void release_body(uint32_t slot) {
        Body& b = bodies[slot];
        if (!b.actor) return;
        if (b.actor->getScene()) {
            b.actor->release();
        } else {  // still waiting in pending_add
            pending_add.erase(std::remove(pending_add.begin(), pending_add.end(), static_cast<PxActor*>(b.actor)),
                              pending_add.end());
            b.actor->release();
        }
        b.actor = nullptr;
        poses[slot].flags = 0;
        if (frozen[slot]) {
            frozen[slot] = 0;
            --frozen_count;
        }
        calm[slot] = 0;
        user_kinematic[slot] = 0;
        --live;
    }

    void add_shape(uint32_t id, PxShape* shape, bool start_over = true) {
        if (!shape) return;
        std::vector<PxShape*>& parts = shapes[id];
        if (start_over) {
            for (PxShape* old : parts) old->release();
            parts.clear();
        }
        parts.push_back(shape);
    }

    // Cooks the convex hull of the points around their centre (returned; the shape's local pose puts it back).
    // PhysX's QuickHull looped forever on a building piece of the open world, sent in world coordinates thousands
    // of metres from the origin (Wrecking Crew maps sit near it): the points are centred and welded within 1 mm,
    // a flat set gets 2 mm of thickness along its thin axis, and what is left without a hull (a line, a point,
    // NaN) is skipped.
    uint32_t stat_hull_skipped = 0;
    PxConvexMesh* cook_hull(const ipc::Vec3* v, uint32_t count, PxVec3& centre) {
        PxBounds3 b = PxBounds3::empty();
        for (uint32_t i = 0; i < count; ++i) b.include(PxVec3(v[i].x, v[i].y, v[i].z));
        centre = b.getCenter();
        const PxVec3 extent = b.getDimensions();
        if (!(extent.maxElement() > 0.002f) || !(extent.maxElement() < 1000.0f)) {  // a point, or NaN / absurd
            ++stat_hull_skipped;
            return nullptr;
        }
        PxVec3 pts[256];
        uint32_t n = 0;
        for (uint32_t i = 0; i < count && n < 128; ++i) {
            const PxVec3 p = PxVec3(v[i].x, v[i].y, v[i].z) - centre;
            bool dup = false;
            for (uint32_t k = 0; k < n && !dup; ++k) dup = (pts[k] - p).magnitudeSquared() < 1e-6f;
            if (!dup) pts[n++] = p;
        }
        if (extent.minElement() < 0.002f) {  // flat: a copy of the points 2 mm along the thin axis
            const uint32_t axis = extent.x <= extent.y && extent.x <= extent.z ? 0 : extent.y <= extent.z ? 1 : 2;
            PxVec3 shift(0.0f);
            shift[axis] = 0.002f;
            const uint32_t flat = n;
            for (uint32_t i = 0; i < flat; ++i) pts[n++] = pts[i] + shift;
        }
        if (n < 4) {
            ++stat_hull_skipped;
            return nullptr;
        }
        PxCookingParams params(physics->getTolerancesScale());
        params.buildGPUData = gpu;
        PxConvexMeshDesc desc;
        desc.points.count = n;
        desc.points.stride = sizeof(PxVec3);
        desc.points.data = pts;
        desc.flags = PxConvexFlag::eCOMPUTE_CONVEX | PxConvexFlag::eSHIFT_VERTICES;
        desc.vertexLimit = 64;
        return PxCreateConvexMesh(params, desc, physics->getPhysicsInsertionCallback());
    }

    PxShape* make_convex(const ipc::Vec3* v, uint32_t count) {
        PxVec3 centre;
        PxConvexMesh* mesh = cook_hull(v, count, centre);
        if (!mesh) return nullptr;
        PxShape* shape = physics->createShape(PxConvexMeshGeometry(mesh), *material, false, PxShapeFlag::eSIMULATION_SHAPE);
        mesh->release();  // the shape keeps its reference
        if (shape) shape->setLocalPose(PxTransform(centre));
        return shape;
    }

    void create_body(const ipc::CreateBody& c) {
        if (c.slot >= max_bodies) return;
        auto it = shapes.find(c.shape);
        if (it == shapes.end() || it->second.empty()) return;
        release_body(c.slot);
        const PxTransform pose(PxVec3(c.pos.x, c.pos.y, c.pos.z), PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized());
        PxRigidDynamic* a = physics->createRigidDynamic(pose);
        for (PxShape* shape : it->second) a->attachShape(*shape);
        if ((c.flags & ipc::kBodyMass) && c.density > 0)
            PxRigidBodyExt::setMassAndUpdateInertia(*a, c.density);
        else
            PxRigidBodyExt::updateMassAndInertia(*a, c.density > 0 ? c.density : 2000.0f);
        a->setSolverIterationCounts(4, 1);
        // Overlaps (pieces adopted slightly inside each other, the player's capsule walked into a pile) are resolved
        // at this speed at most instead of shooting pieces away.
        a->setMaxDepenetrationVelocity(kMaxDepenetration);
        a->setLinearVelocity(PxVec3(c.lin_vel.x, c.lin_vel.y, c.lin_vel.z));
        a->setAngularVelocity(PxVec3(c.ang_vel.x, c.ang_vel.y, c.ang_vel.z));
        a->userData = reinterpret_cast<void*>(uintptr_t(c.slot));
        if (c.flags & ipc::kBodyKinematic) a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
        user_kinematic[c.slot] = (c.flags & ipc::kBodyKinematic) ? 1 : 0;
        heavy[c.slot] = (it->second[0]->getSimulationFilterData().word0 & ipc::kShapeHeavy) ? 1 : 0;
        float radius = 0;
        for (PxShape* shape : it->second) {
            const PxBounds3 b = PxShapeExt::getWorldBounds(*shape, *a, 1.0f);
            radius = std::max(radius, (b.getCenter() - pose.p).magnitude() + b.getExtents().magnitude());
        }
        body_radius[c.slot] = radius;
        calm[c.slot] = 0;
        bodies[c.slot].actor = a;
        pending_add.push_back(a);
        ipc::BodyPose& p = poses[c.slot];
        p.pos = c.pos;
        p.prev_pos = c.pos;
        p.rot = {pose.q.x, pose.q.y, pose.q.z, pose.q.w};
        p.prev_rot = p.rot;
        p.flags = ipc::kPoseAlive;
        ++live;
        slots_used = std::max(slots_used, c.slot + 1);
    }

    void flush_pending() {
        if (pending_add.empty()) return;
        scene->addActors(pending_add.data(), PxU32(pending_add.size()));
        pending_add.clear();
    }

    // ---- the static world mirror ----

    void count_statics(int delta) {
        static_count = uint32_t(int(static_count) + delta);
        sh.control->statics.store(static_count, std::memory_order_relaxed);
    }

    void release_group(Group& g) {
        if (g.pieces) g.pieces->release();
        for (PxRigidStatic* st : g.actors) st->release();
        count_statics(-int(g.shape_count));
        g = Group{};
    }

    void add_static_actor(uint32_t group, PxRigidStatic* st) {
        scene->addActor(*st);
        Group& g = groups[group];
        g.actors.push_back(st);
        ++g.shape_count;
        count_statics(1);
    }

    void static_mesh(const ipc::StaticMesh& c, uint32_t size) {
        const size_t need = sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) + size_t(c.triangle_count) * 12;
        if (c.vertex_count < 3 || c.triangle_count == 0 || need > size) return;
        const auto* v = reinterpret_cast<const ipc::Vec3*>(&c + 1);
        const auto* idx = reinterpret_cast<const uint32_t*>(v + c.vertex_count);
        const double t0 = now_ms();
        PxCookingParams params(physics->getTolerancesScale());
        params.buildGPUData = gpu;
        PxTriangleMeshDesc desc;
        desc.points.count = c.vertex_count;
        desc.points.stride = sizeof(ipc::Vec3);
        desc.points.data = v;
        desc.triangles.count = c.triangle_count;
        desc.triangles.stride = 12;
        desc.triangles.data = idx;
        PxTriangleMesh* mesh = PxCreateTriangleMesh(params, desc, physics->getPhysicsInsertionCallback());
        stat_cook_ms += now_ms() - t0;
        if (!mesh) {
            ++stat_cook_failed;
            return;
        }
        ++stat_cooked;
        PxRigidStatic* st = physics->createRigidStatic(PxTransform(PxIdentity));
        PxRigidActorExt::createExclusiveShape(*st, PxTriangleMeshGeometry(mesh), *material);
        mesh->release();
        add_static_actor(c.group, st);
    }

    void static_convex(const ipc::StaticConvex& c, uint32_t size) {
        if (c.vertex_count < 4 || sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) > size) return;
        const double t0 = now_ms();
        PxVec3 centre;
        PxConvexMesh* mesh = cook_hull(reinterpret_cast<const ipc::Vec3*>(&c + 1), c.vertex_count, centre);
        stat_cook_ms += now_ms() - t0;
        if (!mesh) {
            ++stat_cook_failed;
            return;
        }
        ++stat_cooked;
        Group& g = groups[c.group];
        if (!g.pieces) {
            g.pieces = physics->createRigidStatic(PxTransform(PxIdentity));
            scene->addActor(*g.pieces);
        }
        PxShape* shape = PxRigidActorExt::createExclusiveShape(*g.pieces, PxConvexMeshGeometry(mesh), *material);
        mesh->release();
        if (!shape) return;
        shape->setLocalPose(PxTransform(centre));
        g.by_key[c.key].push_back(shape);
        ++g.shape_count;
        count_statics(1);
    }

    void remove_static_keys(const ipc::RemoveStaticKeys& c, uint32_t size) {
        if (sizeof(c) + size_t(c.count) * 4 > size) return;
        auto it = groups.find(c.group);
        if (it == groups.end() || !it->second.pieces) return;
        Group& g = it->second;
        const auto* keys = reinterpret_cast<const uint32_t*>(&c + 1);
        uint32_t removed = 0;
        PxBounds3 gone = PxBounds3::empty();
        for (uint32_t i = 0; i < c.count; ++i) {
            auto k = g.by_key.find(keys[i]);
            if (k == g.by_key.end()) continue;
            for (PxShape* shape : k->second) {
                gone.include(PxShapeExt::getWorldBounds(*shape, *g.pieces, 1.0f));
                g.pieces->detachShape(*shape);
                ++removed;
            }
            g.by_key.erase(k);
        }
        g.shape_count -= removed;
        count_statics(-int(removed));
        wake_unsupported(gone);
    }

    // Heights arrive as int16 samples; PhysX's own cell split runs from (row, column) to (row + 1, column + 1) when a
    // sample's tessellation flag is set.
    void static_height_field(const ipc::StaticHeightField& c, uint32_t size) {
        const size_t samples = size_t(c.rows) * c.columns;
        if (c.rows < 2 || c.columns < 2 || sizeof(c) + samples * 2 > size) return;
        const auto* heights = reinterpret_cast<const int16_t*>(&c + 1);
        const double t0 = now_ms();
        std::vector<PxHeightFieldSample> s(samples);
        for (size_t i = 0; i < samples; ++i) {
            s[i].height = heights[i];
            s[i].materialIndex0 = 0;
            s[i].materialIndex1 = 0;
            if (!c.flip) s[i].setTessFlag(); else s[i].clearTessFlag();
        }
        PxHeightFieldDesc desc;
        desc.nbRows = c.rows;
        desc.nbColumns = c.columns;
        desc.samples.data = s.data();
        desc.samples.stride = sizeof(PxHeightFieldSample);
        PxHeightField* hf = PxCreateHeightField(desc, physics->getPhysicsInsertionCallback());
        stat_cook_ms += now_ms() - t0;
        if (!hf) {
            ++stat_cook_failed;
            return;
        }
        ++stat_cooked;
        const PxTransform pose(PxVec3(c.origin.x, c.origin.y, c.origin.z), PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized());
        PxRigidStatic* st = physics->createRigidStatic(pose);
        PxRigidActorExt::createExclusiveShape(*st, PxHeightFieldGeometry(hf, PxMeshGeometryFlags(), c.height_scale, c.row_scale,
                                                                         c.column_scale), *material);
        hf->release();
        add_static_actor(c.group, st);
        logf("height field %u x %u (cells %.2f x %.2f m, height step %.4f m) for group %u", c.rows, c.columns,
             double(c.row_scale), double(c.column_scale), double(c.height_scale), c.group);
    }

    // A whole body leaves the mirror: in the open world mostly a zone streaming out, whose ground comes back with
    // it. Frozen pieces there stay where they are (waking them would drop them through the world); sleeping ones
    // PhysX wakes as it loses their contacts. Buildings losing pieces go through remove_static_keys, which wakes.
    void remove_statics(uint32_t group) {
        auto it = groups.find(group);
        if (it == groups.end()) return;
        release_group(it->second);
        groups.erase(it);
    }

    void raycast_batch(const ipc::RaycastBatch& c, uint32_t size) {
        const uint32_t n = std::min<uint32_t>(c.count, ipc::kMaxRays);
        if (sizeof(c) + size_t(n) * sizeof(ipc::Ray) > size) return;
        flush_pending();
        // Queries see what the last step left in the scene query structure; statics added since are included by
        // forcing the update.
        scene->flushQueryUpdates();
        const auto* rays = reinterpret_cast<const ipc::Ray*>(&c + 1);
        float* out = reinterpret_cast<float*>(sh.base + sh.control->query_offset);
        uint32_t* kinds = reinterpret_cast<uint32_t*>(out + ipc::kMaxRays);
        const PxQueryFilterData statics_only(PxQueryFlag::eSTATIC);
        for (uint32_t i = 0; i < n; ++i) {
            const ipc::Ray& r = rays[i];
            PxRaycastBuffer hit;
            const bool any = scene->raycast(PxVec3(r.origin.x, r.origin.y, r.origin.z),
                                            PxVec3(r.direction.x, r.direction.y, r.direction.z), r.max_distance, hit,
                                            PxHitFlag::eDEFAULT, statics_only);
            out[i] = any && hit.hasBlock ? hit.block.distance : -1.0f;
            kinds[i] = any && hit.hasBlock && hit.block.shape ? uint32_t(hit.block.shape->getGeometry().getType()) + 1 : 0;
        }
        sh.control->query_done.store(c.request, std::memory_order_release);
        logf("raycast batch %u: %u rays answered", c.request, n);
    }

    void thaw(uint32_t s) {
        if (!frozen[s] || !bodies[s].actor) return;
        PxRigidDynamic* a = bodies[s].actor;
        a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, false);
        a->wakeUp();
        frozen[s] = 0;
        calm[s] = 0;
        poses[s].flags &= ~ipc::kPoseFrozen;
        --frozen_count;
        ++stat_thawed;
    }

    static int cell_of(float v) { return int(std::floor(v / kCell)); }
    static uint64_t cell_key(int x, int y, int z) {
        return (uint64_t(uint32_t(x) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(y) & 0x1FFFFF) << 21) |
               uint64_t(uint32_t(z) & 0x1FFFFF);
    }
    void grid_add(uint32_t s) {
        const ipc::Vec3& p = poses[s].pos;
        frozen_grid[cell_key(cell_of(p.x), cell_of(p.y), cell_of(p.z))].push_back(s);
    }
    void grid_rebuild() {
        frozen_grid.clear();
        for (uint32_t s : frozen_slots)
            if (frozen[s]) grid_add(s);
    }

    // Thaws a frozen piece; if it then moves it passes the news on to what rests on it.
    void wake(uint32_t s) {
        if (!frozen[s]) return;
        thaw(s);
        wake_pos[s] = poses[s].pos;
        wake_due[s] = step_index + kCascadeDelaySteps;
        if (!cascade_mark[s]) {
            cascade_mark[s] = 1;
            cascade_next.push_back(s);
        }
    }

    // Once per step: woken pieces whose check is due and that moved wake the frozen pieces touching them from above.
    void run_cascade() {
        if (cascade_next.empty()) return;
        cascade_now.clear();
        size_t keep = 0;
        for (size_t i = 0; i < cascade_next.size(); ++i) {
            const uint32_t t = cascade_next[i];
            if (step_index < wake_due[t] || cascade_now.size() >= kCascadePerStep) cascade_next[keep++] = t;  // later
            else cascade_now.push_back(t);
        }
        cascade_next.resize(keep);
        for (uint32_t t : cascade_now) {
            cascade_mark[t] = 0;
            if (!bodies[t].actor || frozen[t]) continue;
            const ipc::Vec3 c = poses[t].pos;
            const float mx = c.x - wake_pos[t].x, my = c.y - wake_pos[t].y, mz = c.z - wake_pos[t].z;
            if (mx * mx + my * my + mz * mz < kMovedSq) continue;  // still where it was: it holds what rests on it
            const float reach = body_radius[t] + kNeighbourReach + kTouch;
            const int x0 = cell_of(c.x - reach), x1 = cell_of(c.x + reach), y0 = cell_of(c.y - 0.1f),
                      y1 = cell_of(c.y + reach), z0 = cell_of(c.z - reach), z1 = cell_of(c.z + reach);
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y)
                    for (int z = z0; z <= z1; ++z) {
                        auto it = frozen_grid.find(cell_key(x, y, z));
                        if (it == frozen_grid.end()) continue;
                        for (uint32_t f : it->second) {
                            if (!frozen[f] || f == t) continue;
                            const ipc::Vec3& p = poses[f].pos;
                            if (p.y < c.y - 0.1f) continue;  // below: it does not rest on t
                            const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z, r = body_radius[t] + body_radius[f] + kTouch;
                            if (dx * dx + dy * dy + dz * dz < r * r) {
                                wake(f);
                                ++stat_cascade;
                            }
                        }
                    }
        }
    }

    // Statics went (a building lost pieces): frozen pieces in and above their bounds move again.
    void wake_unsupported(PxBounds3 b) {
        if (b.isEmpty() || !frozen_count) return;
        b.minimum -= PxVec3(0.5f, 0.5f, 0.5f);
        b.maximum += PxVec3(0.5f, 3.0f, 0.5f);
        for (size_t i = 0; i < frozen_slots.size(); ++i) {
            const uint32_t s = frozen_slots[i];
            if (!frozen[s]) continue;
            const ipc::Vec3& p = poses[s].pos;
            if (b.contains(PxVec3(p.x, p.y, p.z))) {
                wake(s);
                ++stat_unsupported;
            }
        }
    }

    // Frozen pieces within `radius` of `c` move again (with light_only, heavy pieces stay: nothing of the player's
    // touches them).
    void thaw_near(const PxVec3& c, float radius, bool light_only = false) {
        const float r2 = radius * radius;
        size_t keep = 0;
        for (size_t i = 0; i < frozen_slots.size(); ++i) {
            const uint32_t s = frozen_slots[i];
            if (!frozen[s]) continue;  // stale
            const ipc::Vec3& p = poses[s].pos;
            const PxVec3 d(p.x - c.x, p.y - c.y, p.z - c.z);
            if (d.magnitudeSquared() < r2 && !(light_only && heavy[s])) wake(s);
            else frozen_slots[keep++] = s;
        }
        frozen_slots.resize(keep);
    }

    void explosion(const ipc::Explosion& e) {
        flush_pending();
        const PxVec3 c(e.center.x, e.center.y, e.center.z);
        const float r2 = e.radius * e.radius;
        {
            const float wake_r2 = r2 * 2.25f;  // 1.5 x the radius
            for (size_t i = 0; i < frozen_slots.size(); ++i) {
                const uint32_t s = frozen_slots[i];
                if (!frozen[s]) continue;
                const ipc::Vec3& p = poses[s].pos;
                const PxVec3 d(p.x - c.x, p.y - c.y, p.z - c.z);
                if (d.magnitudeSquared() < wake_r2) wake(s);
            }
        }
        uint32_t pushed = 0;
        for (uint32_t s = 0; s < slots_used; ++s) {
            PxRigidDynamic* a = bodies[s].actor;
            if (!a) continue;
            const ipc::Vec3& p = poses[s].pos;
            PxVec3 d(p.x - c.x, p.y - c.y, p.z - c.z);
            const float d2 = d.magnitudeSquared();
            if (d2 >= r2) continue;
            const float dist = std::sqrt(d2);
            PxVec3 dir = dist > 1e-3f ? d / dist : PxVec3(0, 1, 0);
            dir = (dir + PxVec3(0, e.up, 0)).getNormalized();
            if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) continue;
            a->addForce(dir * (e.impulse * (1.0f - dist / e.radius)),
                        e.mode == ipc::kExplosionVelocityChange ? PxForceMode::eVELOCITY_CHANGE : PxForceMode::eIMPULSE);
            ++pushed;
        }
        logf("explosion at (%.1f, %.1f, %.1f) r %.1f m: %u bodies pushed", c.x, c.y, c.z, e.radius, pushed);
    }

    // Applies the complete commands in the ring, for at most kDrainBudgetMs per step: a whole static world (the open
    // world sends 2,000+ bodies, 11 MB of commands, height fields and meshes to cook) arrives over several steps,
    // while the host keeps stepping and its heartbeat keeps going (the game ends a host silent for 3 s). Returns
    // false if the ring is corrupt.
    static constexpr double kDrainBudgetMs = 8.0, kDrainBudgetIdleMs = 40.0;  // idle: no bodies to keep moving
    bool drain() {
        ipc::Control* ctl = sh.control;
        const uint32_t head = ctl->ring_head.load(std::memory_order_acquire);
        const uint32_t size = ctl->ring_size;
        const double t0 = now_ms(), budget = live > 1 ? kDrainBudgetMs : kDrainBudgetIdleMs;  // 1: the player
        uint32_t applied = 0;
        while (sh.tail != head) {
            const uint32_t off = sh.tail & sh.ring_mask;
            const auto* h = reinterpret_cast<const ipc::CmdHeader*>(sh.ring + off);
            if (h->size < sizeof(ipc::CmdHeader) || (h->size & 7) || h->size > size - off || h->size > head - sh.tail) {
                logf("command ring corrupt at %u (type %u, size %u)", sh.tail, h->type, h->size);
                return false;
            }
            const double tc = now_ms();
            apply(*h);
            const double took = now_ms() - tc;
            if (took >= 100.0) logf("slow command: type %u, %u bytes, %.0f ms", unsigned(h->type), h->size, took);
            sh.tail += h->size;
            ++applied;
            ctl->ring_tail.store(sh.tail, std::memory_order_release);
            if (now_ms() - t0 >= budget) break;  // the rest after the next step
        }
        ctl->ring_tail.store(sh.tail, std::memory_order_release);
        flush_pending();
        commands_since_snapshot += applied;
        stat_commands += applied;
        return true;
    }

    void apply(const ipc::CmdHeader& h) {
        switch (ipc::Cmd(h.type)) {
            case ipc::Cmd::Pad:
                break;
            case ipc::Cmd::Reset:
                reset();
                break;
            case ipc::Cmd::AddStaticPlane: {
                const auto& c = reinterpret_cast<const ipc::StaticPlane&>(h);
                PxRigidStatic* st = PxCreatePlane(*physics, PxPlane(c.normal.x, c.normal.y, c.normal.z, c.distance), *material);
                scene->addActor(*st);
                statics.push_back(st);
                break;
            }
            case ipc::Cmd::AddStaticBox: {
                const auto& c = reinterpret_cast<const ipc::StaticBox&>(h);
                PxRigidStatic* st = PxCreateStatic(*physics,
                                                   PxTransform(PxVec3(c.pos.x, c.pos.y, c.pos.z),
                                                               PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized()),
                                                   PxBoxGeometry(c.half_extents.x, c.half_extents.y, c.half_extents.z),
                                                   *material);
                scene->addActor(*st);
                statics.push_back(st);
                break;
            }
            case ipc::Cmd::ShapeBox: {
                const auto& c = reinterpret_cast<const ipc::ShapeBox&>(h);
                add_shape(c.shape, physics->createShape(PxBoxGeometry(c.half_extents.x, c.half_extents.y, c.half_extents.z),
                                                        *material, false, PxShapeFlag::eSIMULATION_SHAPE));
                break;
            }
            case ipc::Cmd::ShapeConvex: {
                const auto& c = reinterpret_cast<const ipc::ShapeConvex&>(h);
                if (c.vertex_count >= 4 && sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) <= h.size)
                    add_shape(c.shape, make_convex(reinterpret_cast<const ipc::Vec3*>(&c + 1), c.vertex_count));
                break;
            }
            case ipc::Cmd::CreateBody:
                create_body(reinterpret_cast<const ipc::CreateBody&>(h));
                break;
            case ipc::Cmd::DestroyBody: {
                const auto& c = reinterpret_cast<const ipc::DestroyBody&>(h);
                if (c.slot < max_bodies) release_body(c.slot);
                break;
            }
            case ipc::Cmd::Explosion:
                explosion(reinterpret_cast<const ipc::Explosion&>(h));
                break;
            case ipc::Cmd::Frame: {
                const auto& c = reinterpret_cast<const ipc::Frame&>(h);
                last_frame = c.frame;
                last_frame_qpc = c.qpc;
                const double latency = double(qpc() - c.qpc) * 1000.0 / double(g_freq.QuadPart);
                stat_cmd_latency_ms += latency;
                ++stat_cmd_latency_n;
                break;
            }
            case ipc::Cmd::TestCrash:
                logf("TestCrash: ending the host process abruptly");
                TerminateProcess(GetCurrentProcess(), 3);
                break;
            case ipc::Cmd::StaticMesh:
                static_mesh(reinterpret_cast<const ipc::StaticMesh&>(h), h.size);
                break;
            case ipc::Cmd::StaticConvex:
                static_convex(reinterpret_cast<const ipc::StaticConvex&>(h), h.size);
                break;
            case ipc::Cmd::RemoveStatics:
                remove_statics(reinterpret_cast<const ipc::RemoveStatics&>(h).group);
                break;
            case ipc::Cmd::RaycastBatch:
                raycast_batch(reinterpret_cast<const ipc::RaycastBatch&>(h), h.size);
                break;
            case ipc::Cmd::RemoveStaticKeys:
                remove_static_keys(reinterpret_cast<const ipc::RemoveStaticKeys&>(h), h.size);
                break;
            case ipc::Cmd::StaticHeightField:
                static_height_field(reinterpret_cast<const ipc::StaticHeightField&>(h), h.size);
                break;
            case ipc::Cmd::ShapePart: {
                const auto& c = reinterpret_cast<const ipc::ShapePart&>(h);
                if (c.vertex_count >= 4 && sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) <= h.size) {
                    PxShape* s = make_convex(reinterpret_cast<const ipc::Vec3*>(&c + 1), c.vertex_count);
                    if (s && c.flags) s->setSimulationFilterData(filter_for(c.flags));
                    add_shape(c.shape, s, c.part == 0);
                }
                break;
            }
            case ipc::Cmd::KinematicTarget: {
                const auto& c = reinterpret_cast<const ipc::KinematicTarget&>(h);
                if (c.slot < max_bodies && bodies[c.slot].actor &&
                    (bodies[c.slot].actor->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC)) {
                    flush_pending();
                    bodies[c.slot].actor->setKinematicTarget(
                        PxTransform(PxVec3(c.pos.x, c.pos.y, c.pos.z), PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized()));
                }
                break;
            }
        }
    }

    // ---- step and snapshot ----

    void step() {
        const int64_t t0 = qpc();
        scene->simulate(dt);
        scene->fetchResults(true);
        const int64_t t1 = qpc();
        const double step_ms = double(t1 - t0) * 1000.0 / double(g_freq.QuadPart);
        if (step_ms >= 250.0) logf("slow step: %.0f ms (%u statics, %u bodies)", step_ms, static_count, live);
        g_last_progress_qpc.store(t1);
        ++step_index;
        sim_time += dt;

        // Bodies that moved in the previous step but not in this one stop interpolating.
        for (uint32_t s : awake_slots) was_awake[s] = 1;
        PxU32 n = 0;
        PxActor** active = scene->getActiveActors(n);
        now_awake.clear();
        for (PxU32 i = 0; i < n; ++i) {
            const uint32_t s = uint32_t(reinterpret_cast<uintptr_t>(active[i]->userData));
            if (s >= max_bodies || !bodies[s].actor) continue;
            const PxTransform t = static_cast<PxRigidDynamic*>(active[i])->getGlobalPose();
            ipc::BodyPose& p = poses[s];
            p.prev_pos = p.pos;
            p.prev_rot = p.rot;
            p.pos = {t.p.x, t.p.y, t.p.z};
            p.rot = {t.q.x, t.q.y, t.q.z, t.q.w};
            p.flags = ipc::kPoseAlive | ipc::kPoseAwake;
            was_awake[s] = 0;
            now_awake.push_back(s);
            if (user_kinematic[s]) continue;
            const PxRigidDynamic* d = static_cast<PxRigidDynamic*>(active[i]);
            if (d->getLinearVelocity().magnitudeSquared() < kCalmSpeed * kCalmSpeed &&
                d->getAngularVelocity().magnitudeSquared() < kCalmSpin * kCalmSpin) {
                if (++calm[s] >= kFreezeSteps) freeze_list.push_back(s);
            } else {
                calm[s] = 0;
            }
        }
        // Made kinematic after the loop: the active-actor buffer belongs to the scene until the next simulate.
        for (uint32_t s : freeze_list) {
            bodies[s].actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
            frozen[s] = 1;
            poses[s].flags = ipc::kPoseAlive | ipc::kPoseFrozen;  // its last pose stands
            frozen_slots.push_back(s);
            grid_add(s);
            ++frozen_count;
            ++stat_frozen;
        }
        freeze_list.clear();
        run_cascade();
        if (step_index % 600 == 0) grid_rebuild();
        // The player and other user kinematic bodies wake what they come near, ten times a second.
        if (step_index % 6 == 0 && frozen_count) {
            for (uint32_t s = 0; s < slots_used; ++s)
                if (user_kinematic[s] && bodies[s].actor) {
                    const ipc::Vec3& p = poses[s].pos;
                    thaw_near(PxVec3(p.x, p.y, p.z), 2.5f, true);
                }
        }
        for (uint32_t s : awake_slots) {
            if (!was_awake[s]) continue;
            was_awake[s] = 0;
            ipc::BodyPose& p = poses[s];
            if (!(p.flags & ipc::kPoseAlive)) continue;
            p.prev_pos = p.pos;
            p.prev_rot = p.rot;
            p.flags = ipc::kPoseAlive | (frozen[s] ? ipc::kPoseFrozen : 0u);
        }
        awake_slots.swap(now_awake);

        publish(t0, float(double(t1 - t0) * 1000.0 / double(g_freq.QuadPart)), uint32_t(n));
    }

    void publish(int64_t step_qpc, float step_ms, uint32_t awake) {
        ipc::SnapshotHeader* snap = sh.snapshot(sh.back);
        auto* records = reinterpret_cast<ipc::BodyPose*>(snap + 1);
        memcpy(records, poses.data(), size_t(slots_used) * sizeof(ipc::BodyPose));
        snap->sequence = ++sequence;
        snap->step = step_index;
        snap->sim_time = sim_time;
        snap->step_qpc = step_qpc;
        snap->step_ms = step_ms;
        snap->bodies = live;
        snap->awake = awake;
        snap->slots = slots_used;
        snap->last_frame = last_frame;
        snap->commands = commands_since_snapshot;
        snap->frozen = frozen_count;
        commands_since_snapshot = 0;
        snap->publish_qpc = qpc();
        snap->work_ms = float(double(snap->publish_qpc - step_qpc) * 1000.0 / double(g_freq.QuadPart));
        const uint32_t prev = sh.control->snapshot_latest.exchange(sh.back | ipc::kFresh, std::memory_order_acq_rel);
        sh.back = prev & ipc::kIndexMask;

        ++stat_steps;
        stat_step_ms += step_ms;
        stat_step_max = std::max(stat_step_max, double(step_ms));
        stat_work_ms += snap->work_ms;
        if (steps_csv)
            fprintf(steps_csv, "%u,%.3f,%.3f,%.3f,%u,%u,%u,%u\n", step_index, now_ms(), step_ms, snap->work_ms, live,
                    awake, snap->commands, last_frame);
    }

    void log_stats() {
        if (stat_cooked || stat_cook_failed) {
            logf("static world: %u statics in %zu groups; cooked %u (%u failed, %u hulls without a volume) in %.1f ms",
                 static_count, groups.size(), stat_cooked, stat_cook_failed, stat_hull_skipped, stat_cook_ms);
            stat_cooked = stat_cook_failed = stat_hull_skipped = 0;
            stat_cook_ms = 0;
        }
        if (!stat_steps) return;
        logf("%u steps: step %.2f ms mean, %.2f max; work %.2f ms mean; %u bodies, %u awake, %u frozen (+%u, -%u; woken "
             "by removed statics %u, by the cascade %u); %u commands; frame command latency %.2f ms",
             stat_steps, stat_step_ms / stat_steps, stat_step_max, stat_work_ms / stat_steps, live,
             uint32_t(awake_slots.size()), frozen_count, stat_frozen, stat_thawed, stat_unsupported, stat_cascade,
             stat_commands, stat_cmd_latency_n ? stat_cmd_latency_ms / stat_cmd_latency_n : 0.0);
        stat_frozen = stat_thawed = stat_unsupported = stat_cascade = 0;
        stat_steps = stat_commands = stat_cmd_latency_n = 0;
        stat_step_ms = stat_step_max = stat_work_ms = stat_cmd_latency_ms = 0;
        if (steps_csv) fflush(steps_csv);
    }
};

std::wstring arg_value(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], name) == 0) return argv[i + 1];
    return {};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    const std::wstring map_name = arg_value(argc, argv, L"--map");
    const std::wstring log_path = arg_value(argc, argv, L"--log");
    const std::wstring csv_path = arg_value(argc, argv, L"--steps-csv");
    // Shared reads: the logs can be followed while the host runs.
    if (!log_path.empty()) g_log = _wfsopen(log_path.c_str(), L"w", _SH_DENYNO);
    logf("rfgphysx_host starting, mapping '%ls'", map_name.c_str());

    Host host;
    if (map_name.empty() || !host.sh.open(map_name.c_str())) {
        logf("cannot open the shared memory (error %lu)", GetLastError());
        return 2;
    }
    ipc::Control* ctl = host.sh.control;
    ctl->host_pid.store(GetCurrentProcessId(), std::memory_order_relaxed);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, ctl->game_pid);
    if (!game) {
        host.fail("cannot open the game process");
        return 2;
    }
    host.max_bodies = ctl->max_bodies;
    host.dt = ctl->step_hz > 0 ? 1.0f / ctl->step_hz : 1.0f / 60.0f;
    host.bodies.resize(host.max_bodies);
    host.poses.assign(host.max_bodies, ipc::BodyPose{});
    host.was_awake.assign(host.max_bodies, 0);
    host.calm.assign(host.max_bodies, 0);
    host.frozen.assign(host.max_bodies, 0);
    host.user_kinematic.assign(host.max_bodies, 0);
    host.heavy.assign(host.max_bodies, 0);
    host.body_radius.assign(host.max_bodies, 0.0f);
    host.cascade_mark.assign(host.max_bodies, 0);
    host.wake_pos.assign(host.max_bodies, ipc::Vec3{0, 0, 0});
    host.wake_due.assign(host.max_bodies, 0);
    host.sh.back = 0;
    if (!csv_path.empty() && (host.steps_csv = _wfsopen(csv_path.c_str(), L"w", _SH_DENYNO)) != nullptr)
        fprintf(host.steps_csv, "step,t_ms,step_ms,work_ms,bodies,awake,commands,last_frame\n");

    // Normal priority: at above-normal the host's threads took cores from the game's own (the multithreaded Havok
    // step slowed by 1.0-1.4 ms in heavy frames with 20,000 awake test bodies, 06 §13), and a step that starts a
    // little late costs nothing in an asynchronous world.
    if (!host.init_physx(ctl->backend)) return 2;
    ctl->host_gpu.store(host.gpu ? 1u : 0u, std::memory_order_relaxed);
    host.sh.message(host.gpu ? "GPU PhysX ready" : "CPU PhysX ready");
    ctl->host_state.store(uint32_t(ipc::HostState::Ready), std::memory_order_release);
    logf("ready");

    // The heartbeat says the process lives; it comes from a thread of its own, since cooking one large static (the
    // open world's meshes and height fields) can keep the main loop busy for seconds. The game judges progress by
    // the steps (snapshots) separately.
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    g_last_progress_qpc.store(qpc());
    std::thread([ctl] {
        bool told = false;
        for (;;) {
            ctl->host_heartbeat.fetch_add(1, std::memory_order_relaxed);
            Sleep(100);
            const double stuck = double(qpc() - g_last_progress_qpc.load()) * 1000.0 / double(g_freq.QuadPart);
            if (stuck > 15000.0 && !told) {
                told = true;
                log_main_stack(stuck);
            } else if (stuck < 1000.0) {
                told = false;
            }
        }
    }).detach();

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const double period_ms = 1000.0 / (ctl->step_hz > 0 ? ctl->step_hz : 60.0);
    double next_step = now_ms();
    double next_stats = now_ms() + 5000.0;
    const char* reason = "quit requested";
    for (;;) {
        ctl->host_heartbeat.fetch_add(1, std::memory_order_relaxed);
        if (ctl->quit.load(std::memory_order_acquire)) break;
        if (WaitForSingleObject(game, 0) == WAIT_OBJECT_0) {
            reason = "the game process is gone";
            break;
        }
        const double t = now_ms();
        if (t >= next_step) {
            if (!host.drain()) {
                host.fail("command ring corrupt");
                return 2;
            }
            host.step();
            next_step += period_ms;
            if (now_ms() - next_step > 3 * period_ms) next_step = now_ms() + period_ms;  // fell behind: no catch-up burst
        } else {
            // Sleep until about half a millisecond before the step, then poll; commands are applied at step time.
            const double wait = next_step - t;
            if (wait > 1.0 && timer) {
                LARGE_INTEGER due;
                due.QuadPart = -LONGLONG((wait - 0.5) * 10000.0);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 20);
            } else {
                Sleep(0);
            }
        }
        if (now_ms() >= next_stats) {
            host.log_stats();
            next_stats += 5000.0;
        }
    }
    host.log_stats();
    logf("exiting: %s", reason);
    ctl->host_state.store(uint32_t(ipc::HostState::Exited), std::memory_order_release);
    if (host.steps_csv) fclose(host.steps_csv);
    if (g_log) fclose(g_log);
    // The process exits without tearing PhysX down: the OS frees everything at once, and a CUDA context release can
    // take long enough to matter when the game is closing.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
