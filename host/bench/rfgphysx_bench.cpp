// rfgphysx_bench: how fast PhysX steps piles of colliding building debris, on the GPU and on CPU threads.
//
// Each run builds piles the way Red Faction Guerrilla does: pieces break off in bursts (one per rocket) at a few
// building sites and fall into piles that keep growing, every piece colliding with every other. Then the piles
// settle, get hit by explosions (each wakes part of a pile), and finally a quake wakes every piece at once.
// Every phase reports the step time (simulate + fetchResults), the awake pieces, contact pairs, CPU use and the
// cost of reading back the moved pieces' poses (what the game would receive each step).
//
//   rfgphysx_bench --backend gpu|cpu [--threads 8] [--pieces 20000] [--sites 4] [--bursts 20] [--csv steps.csv]
//
// Output: one summary line per phase on stdout (also appended to --summary as CSV), per-step rows in --csv.
#include <PxPhysicsAPI.h>
#include <gpu/PxGpu.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace physx;

namespace {

struct Options {
    bool gpu = true;
    int threads = 8;          // CPU dispatcher threads
    int gpu_threads = 4;      // dispatcher threads for PhysX's CPU-side work in GPU runs
    int scenes = 1;           // independent scenes stepped concurrently (sites spread over them)
    int pieces = 20000;       // total pieces spawned over the build-up
    int sites = 4;            // pile sites (buildings), 60 m apart
    int bursts = 20;          // build-up bursts, spread over the sites
    int burst_interval = 30;  // steps between bursts (0.5 s at 60 Hz)
    int settle = 300;         // steps without spawning after the build-up
    int explosions = 6;       // explosions into the piles, one every explosion_interval steps
    int explosion_interval = 60;
    int quake = 180;          // steps after a velocity kick to every piece
    float dt = 1.0f / 60.0f;
    bool tgs = true;
    int pos_iters = 4;
    // Settling: what lets piles fall asleep (a pile is one island, so one jittering piece keeps all of it awake).
    bool stabilization = false;
    float linear_damping = 0.0f;
    float angular_damping = 0.05f;   // PhysX default
    float sleep_threshold = -1.0f;   // mass-normalized kinetic energy; < 0 keeps PhysX's default (5e-5 * speed^2)
    float max_depenetration = -1.0f; // m/s; < 0 keeps PhysX's default
    // Freezing: a piece that has been calm (below freeze_speed m/s and 3x that in rad/s) for freeze_after steps
    // becomes kinematic, so it no longer ties its pile into one island; explosions release pieces near them.
    bool freeze = false;
    float freeze_speed = 0.15f;
    int freeze_after = 60;
    int freeze_check = 10;           // steps between calm checks
    std::string csv;          // per-step rows
    std::string summary;      // per-phase rows (appended)
    std::string label;
};

struct ErrorCallback : PxErrorCallback {
    int reported = 0;
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        if (reported++ < 40) fprintf(stderr, "PhysX error %d: %s (%s:%d)\n", int(code), message, file, line);
    }
};

PxDefaultAllocator g_allocator;
ErrorCallback g_errors;

double now_ms() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

double process_cpu_ms() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](FILETIME f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
    return v(k) + v(u);
}

struct StepRow {
    int step;
    int phase;
    int actors;
    int awake;
    int pairs;
    int touching;
    float step_ms;
    float readback_ms;
    float cpu_ms;  // process CPU time during simulate + fetch (all threads)
    int frozen;
    float freeze_ms;  // calm checks and (un)freezing done before this step
};

const char* kPhaseNames[] = {"buildup", "settle", "explosions", "quake"};

struct Bench {
    Options o;
    PxFoundation* foundation = nullptr;
    PxPhysics* physics = nullptr;
    PxCudaContextManager* cuda = nullptr;
    PxDefaultCpuDispatcher* dispatcher = nullptr;
    std::vector<PxScene*> scenes;
    PxScene* scene_of_site(int site) { return scenes[size_t(site) % scenes.size()]; }
    PxMaterial* material = nullptr;
    std::vector<PxConvexMesh*> prototypes;
    std::vector<PxRigidDynamic*> bodies;
    std::vector<uint16_t> calm;     // per body: steps spent calm
    std::vector<uint8_t> frozen;    // per body: kinematic because it settled
    int nfrozen = 0;
    float pending_freeze_ms = 0;
    std::vector<PxVec3> site_centers;
    std::vector<StepRow> rows;
    std::vector<PxTransform> snapshot;  // poses read back after each step
    std::mt19937 rng{20261019};
    int step = 0;

    float uni(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

    bool init() {
        foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_allocator, g_errors);
        if (!foundation) return false;
        if (o.gpu) {
            PxCudaContextManagerDesc desc;
            cuda = PxCreateCudaContextManager(*foundation, desc, PxGetProfilerCallback());
            if (!cuda || !cuda->contextIsValid()) {
                fprintf(stderr, "no valid CUDA context: GPU PhysX unavailable\n");
                return false;
            }
            printf("GPU: %s, %d SMs, %.0f MB\n", cuda->getDeviceName(), cuda->getMultiprocessorCount(),
                   double(cuda->getDeviceTotalMemBytes()) / (1024.0 * 1024.0));
        }
        physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale(), false, nullptr);
        if (!physics) return false;
        material = physics->createMaterial(0.8f, 0.6f, 0.1f);  // concrete-ish

        PxSceneDesc sd(physics->getTolerancesScale());
        sd.gravity = PxVec3(0, -9.81f, 0);
        dispatcher = PxDefaultCpuDispatcherCreate(PxU32(o.gpu ? o.gpu_threads : o.threads));
        sd.cpuDispatcher = dispatcher;
        sd.filterShader = PxDefaultSimulationFilterShader;
        sd.flags |= PxSceneFlag::eENABLE_PCM | PxSceneFlag::eENABLE_ACTIVE_ACTORS;
        if (o.stabilization) sd.flags |= PxSceneFlag::eENABLE_STABILIZATION;
        sd.solverType = o.tgs ? PxSolverType::eTGS : PxSolverType::ePGS;
        if (o.gpu) {
            sd.cudaContextManager = cuda;
            sd.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS;
            sd.broadPhaseType = PxBroadPhaseType::eGPU;
            // Piles: about 3 touching pairs and 12 contacts per piece; generous so nothing overflows at 100k.
            const PxU32 share = PxU32(std::max(1, o.scenes));
            sd.gpuDynamicsConfig.maxRigidContactCount = (4u << 20) / share;
            sd.gpuDynamicsConfig.maxRigidPatchCount = (2u << 20) / share;
            sd.gpuDynamicsConfig.foundLostPairsCapacity = (2u << 20) / share;
            sd.gpuDynamicsConfig.tempBufferCapacity = 128ull << 20;
            sd.gpuDynamicsConfig.heapCapacity = 128u << 20;
            sd.gpuMaxNumPartitions = 32;
        } else {
            sd.broadPhaseType = PxBroadPhaseType::ePABP;
        }
        // Independent scenes (sites assigned round-robin), stepped together: pieces of different sites never touch.
        for (int k = 0; k < std::max(1, o.scenes); ++k) {
            PxScene* s = physics->createScene(sd);
            if (!s) return false;
            s->addActor(*PxCreatePlane(*physics, PxPlane(0, 1, 0, 0), *material));
            scenes.push_back(s);
        }

        // Per site, the stumps of the building: a broken wall ring the debris piles against.
        const int grid = int(std::ceil(std::sqrt(double(o.sites))));
        for (int s = 0; s < o.sites; ++s) {
            const PxVec3 c(float(s % grid) * 60.0f, 0, float(s / grid) * 60.0f);
            site_centers.push_back(c);
            for (int w = 0; w < 4; ++w) {
                const float a = float(w) * PxHalfPi;
                const PxVec3 n(std::cos(a), 0, std::sin(a));
                PxRigidStatic* wall = PxCreateStatic(*physics, PxTransform(c + n * 9.0f + PxVec3(0, 1.5f, 0), PxQuat(a, PxVec3(0, 1, 0))),
                                                     PxBoxGeometry(0.4f, 1.5f, 5.0f), *material);
                scene_of_site(s)->addActor(*wall);
            }
        }
        cook_prototypes();
        return true;
    }

    // Random convex chunks: rubble (irregular, roughly equidimensional), slabs (flat wall and floor pieces) and
    // beams, each at most 32 hull vertices so the GPU collision code takes them.
    void cook_prototypes() {
        PxCookingParams params(physics->getTolerancesScale());
        params.buildGPUData = o.gpu;
        for (int i = 0; i < 48; ++i) {
            PxVec3 ext(1, 1, 1);
            if (i % 3 == 1) ext = PxVec3(1.0f, uni(0.15f, 0.35f), uni(0.6f, 1.0f));  // slab
            if (i % 6 == 2) ext = PxVec3(1.0f, uni(0.12f, 0.25f), uni(0.12f, 0.25f)); // beam
            std::vector<PxVec3> pts;
            const int n = 10 + int(uni(0, 14));
            for (int k = 0; k < n; ++k)
                pts.push_back(PxVec3(uni(-0.5f, 0.5f) * ext.x, uni(-0.5f, 0.5f) * ext.y, uni(-0.5f, 0.5f) * ext.z));
            PxConvexMeshDesc desc;
            desc.points.count = PxU32(pts.size());
            desc.points.stride = sizeof(PxVec3);
            desc.points.data = pts.data();
            desc.flags = PxConvexFlag::eCOMPUTE_CONVEX;
            desc.vertexLimit = 32;
            PxConvexMeshCookingResult::Enum result;
            PxConvexMesh* mesh = PxCreateConvexMesh(params, desc, physics->getPhysicsInsertionCallback(), &result);
            if (mesh && (!o.gpu || mesh->isGpuCompatible())) prototypes.push_back(mesh);
        }
        printf("cooked %zu convex prototypes\n", prototypes.size());
    }

    // One burst: a building section breaks into `count` pieces above a site, flying apart a little.
    void spawn_burst(int count) {
        const int site_index = std::uniform_int_distribution<int>(0, o.sites - 1)(rng);
        const PxVec3 site = site_centers[size_t(site_index)];
        const PxVec3 origin = site + PxVec3(uni(-4, 4), uni(8, 14), uni(-4, 4));
        std::vector<PxActor*> batch;
        batch.reserve(count);
        for (int i = 0; i < count; ++i) {
            // Piece size 0.15-2 m, most of them small (log-uniform).
            const float size = std::exp(uni(std::log(0.15f), std::log(2.0f)));
            PxConvexMesh* mesh = prototypes[std::uniform_int_distribution<size_t>(0, prototypes.size() - 1)(rng)];
            const PxVec3 offset(uni(-5, 5), uni(-4, 4), uni(-5, 5));
            PxQuat q(uni(0, PxTwoPi), PxVec3(uni(-1, 1), uni(-1, 1), uni(-1, 1)).getNormalized());
            PxRigidDynamic* body = physics->createRigidDynamic(PxTransform(origin + offset, q));
            PxRigidActorExt::createExclusiveShape(*body, PxConvexMeshGeometry(mesh, PxMeshScale(size)), *material);
            PxRigidBodyExt::updateMassAndInertia(*body, 2000.0f);
            body->setSolverIterationCounts(PxU32(o.pos_iters), 1);
            body->setLinearDamping(o.linear_damping);
            body->setAngularDamping(o.angular_damping);
            if (o.sleep_threshold >= 0) body->setSleepThreshold(o.sleep_threshold);
            if (o.max_depenetration >= 0) body->setMaxDepenetrationVelocity(o.max_depenetration);
            body->setLinearVelocity(offset.getNormalized() * uni(0, 4) + PxVec3(0, uni(-1, 3), 0));
            body->setAngularVelocity(PxVec3(uni(-3, 3), uni(-3, 3), uni(-3, 3)));
            bodies.push_back(body);
            calm.push_back(0);
            frozen.push_back(0);
            batch.push_back(body);
        }
        scene_of_site(site_index)->addActors(batch.data(), PxU32(batch.size()));
    }

    // An explosion in a pile: pieces within 7 m are pushed out and up (as a rocket would); they and whatever
    // they touch wake up.
    void set_frozen(size_t i, bool f) {
        if (frozen[i] == uint8_t(f)) return;
        frozen[i] = uint8_t(f);
        nfrozen += f ? 1 : -1;
        calm[i] = 0;
        bodies[i]->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, f);
        if (!f) bodies[i]->wakeUp();
    }

    // Pieces that have been calm long enough become kinematic.
    void freeze_calm() {
        const double t0 = now_ms();
        const float v2 = o.freeze_speed * o.freeze_speed, w2 = 9.0f * v2;
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (frozen[i]) continue;
            PxRigidDynamic* b = bodies[i];
            if (b->isSleeping()) {  // asleep already costs nothing
                calm[i] = 0;
                continue;
            }
            if (b->getLinearVelocity().magnitudeSquared() < v2 && b->getAngularVelocity().magnitudeSquared() < w2) {
                calm[i] = uint16_t(std::min<int>(calm[i] + o.freeze_check, 60000));
                if (calm[i] >= o.freeze_after) set_frozen(i, true);
            } else {
                calm[i] = 0;
            }
        }
        pending_freeze_ms += float(now_ms() - t0);
    }

    void explode() {
        const PxVec3 site = site_centers[std::uniform_int_distribution<int>(0, o.sites - 1)(rng)];
        const PxVec3 center = site + PxVec3(uni(-4, 4), uni(0.5f, 3), uni(-4, 4));
        int hit = 0;
        const double t0 = now_ms();
        if (o.freeze) {  // release frozen pieces around the blast (a margin beyond its reach)
            for (size_t i = 0; i < bodies.size(); ++i)
                if (frozen[i] && (bodies[i]->getGlobalPose().p - center).magnitude() < 10.0f) set_frozen(i, false);
        }
        pending_freeze_ms += float(now_ms() - t0);
        for (PxRigidDynamic* b : bodies) {
            const PxVec3 d = b->getGlobalPose().p - center;
            const float dist = d.magnitude();
            if (dist > 7.0f) continue;
            const float push = 12.0f * (1.0f - dist / 7.0f);
            b->setLinearVelocity(b->getLinearVelocity() + d.getNormalized() * push + PxVec3(0, push * 0.5f, 0));
            ++hit;
        }
        (void)hit;
    }

    // Every piece gets a random kick: the worst case, a whole level of piles awake at once.
    void quake() {
        for (size_t i = 0; i < bodies.size(); ++i) set_frozen(i, false);
        for (PxRigidDynamic* b : bodies)
            b->setLinearVelocity(b->getLinearVelocity() + PxVec3(uni(-2, 2), uni(0, 3), uni(-2, 2)));
    }

    void simulate(int phase) {
        const double cpu0 = process_cpu_ms();
        const double t0 = now_ms();
        for (PxScene* s : scenes) s->simulate(o.dt);  // all scenes run concurrently until fetched
        for (PxScene* s : scenes) s->fetchResults(true);
        const double t1 = now_ms();
        const double cpu1 = process_cpu_ms();
        // What the game would receive: the poses of every piece that moved this step.
        snapshot.clear();
        for (PxScene* s : scenes) {
            PxU32 nactive = 0;
            PxActor** active = s->getActiveActors(nactive);
            for (PxU32 i = 0; i < nactive; ++i) snapshot.push_back(static_cast<PxRigidActor*>(active[i])->getGlobalPose());
        }
        const double t2 = now_ms();
        int awake = 0, pairs = 0, touching = 0;
        for (PxScene* s : scenes) {
            PxSimulationStatistics stats;
            s->getSimulationStatistics(stats);
            awake += int(stats.nbActiveDynamicBodies);
            pairs += int(stats.nbDiscreteContactPairsTotal);
            touching += int(stats.nbDiscreteContactPairsWithContacts);
        }
        rows.push_back({step, phase, int(bodies.size()), awake, pairs, touching, float(t1 - t0), float(t2 - t1),
                        float(cpu1 - cpu0), nfrozen, pending_freeze_ms});
        pending_freeze_ms = 0;
        ++step;
        if (o.freeze && step % o.freeze_check == 0) freeze_calm();
    }

    void run() {
        const int per_burst = std::max(1, o.pieces / o.bursts);
        for (int b = 0; b < o.bursts; ++b) {
            spawn_burst(b == o.bursts - 1 ? o.pieces - per_burst * (o.bursts - 1) : per_burst);
            for (int s = 0; s < o.burst_interval; ++s) simulate(0);
        }
        for (int s = 0; s < o.settle; ++s) simulate(1);
        for (int e = 0; e < o.explosions; ++e) {
            explode();
            for (int s = 0; s < o.explosion_interval; ++s) simulate(2);
        }
        quake();
        for (int s = 0; s < o.quake; ++s) simulate(3);
    }

    void report() {
        FILE* sum = o.summary.empty() ? nullptr : fopen(o.summary.c_str(), "a");
        if (sum) {
            fseek(sum, 0, SEEK_END);
            if (ftell(sum) == 0)
                fprintf(sum, "label,backend,threads,pieces,phase,steps,step_mean_ms,step_p50_ms,step_p95_ms,step_max_ms,"
                             "awake_mean,awake_max,pairs_mean,touching_mean,readback_mean_ms,cpu_cores_mean,"
                             "ms_per_1000_awake,frozen_mean,freeze_ms_mean\n");
        }
        for (int phase = 0; phase < 4; ++phase) {
            std::vector<StepRow> r;
            for (const StepRow& x : rows)
                if (x.phase == phase) r.push_back(x);
            if (r.empty()) continue;
            std::vector<float> ms;
            double sum_ms = 0, awake = 0, pairs = 0, touching = 0, readback = 0, cpu = 0, fz = 0, fz_ms = 0;
            int awake_max = 0;
            for (const StepRow& x : r) {
                ms.push_back(x.step_ms);
                sum_ms += x.step_ms;
                awake += x.awake;
                awake_max = std::max(awake_max, x.awake);
                pairs += x.pairs;
                touching += x.touching;
                readback += x.readback_ms;
                cpu += x.cpu_ms;
                fz += x.frozen;
                fz_ms += x.freeze_ms;
            }
            std::sort(ms.begin(), ms.end());
            const double n = double(r.size());
            const auto pct = [&](double p) { return ms[std::min(ms.size() - 1, size_t(p * double(ms.size())))]; };
            const double mean = sum_ms / n;
            const double per_k = awake > 0 ? mean / (awake / n / 1000.0) : 0.0;
            printf("%-10s %s x%-2d %6d pieces | %-10s step mean %7.2f ms p50 %7.2f p95 %7.2f max %7.2f | awake %6.0f "
                   "(max %6d) pairs %7.0f frozen %6.0f (%.2f ms) | readback %5.2f ms | CPU %4.1f cores | %5.2f ms per 1000 awake\n",
                   o.label.c_str(), o.gpu ? "GPU" : "CPU", o.gpu ? o.gpu_threads : o.threads, o.pieces, kPhaseNames[phase], mean,
                   pct(0.5), pct(0.95), double(ms.back()), awake / n, awake_max, pairs / n, fz / n, fz_ms / n,
                   readback / n, cpu / sum_ms, per_k);
            if (sum)
                fprintf(sum, "%s,%s,%d,%d,%s,%d,%.3f,%.3f,%.3f,%.3f,%.0f,%d,%.0f,%.0f,%.3f,%.2f,%.3f,%.0f,%.3f\n", o.label.c_str(),
                        o.gpu ? "gpu" : "cpu", o.gpu ? o.gpu_threads : o.threads, o.pieces, kPhaseNames[phase], int(r.size()), mean,
                        pct(0.5), pct(0.95), double(ms.back()), awake / n, awake_max, pairs / n, touching / n,
                        readback / n, cpu / sum_ms, per_k, fz / n, fz_ms / n);
        }
        if (sum) fclose(sum);
        if (o.gpu) {
            for (size_t k = 0; k < scenes.size(); ++k) {
                PxSimulationStatistics stats;
                scenes[k]->getSimulationStatistics(stats);
                const PxGpuDynamicsMemoryConfigStatistics& g = stats.gpuDynamicsMemoryConfigStatistics;
                printf("scene %zu GPU buffers needed: contacts %u, patches %u, found/lost pairs %u, temp %.0f MB; GPU heap "
                       "%.0f MB\n", k, g.rigidContactCount, g.rigidPatchCount, g.foundLostPairs,
                       double(g.tempBufferCapacity) / 1048576.0, double(stats.gpuMemHeap) / 1048576.0);
            }
        }
        if (!o.csv.empty()) {
            if (FILE* f = fopen(o.csv.c_str(), "w")) {
                fprintf(f, "step,phase,actors,awake,pairs,touching,step_ms,readback_ms,cpu_ms,frozen,freeze_ms\n");
                for (const StepRow& x : rows)
                    fprintf(f, "%d,%s,%d,%d,%d,%d,%.3f,%.3f,%.3f,%d,%.3f\n", x.step, kPhaseNames[x.phase], x.actors,
                            x.awake, x.pairs, x.touching, x.step_ms, x.readback_ms, x.cpu_ms, x.frozen, x.freeze_ms);
                fclose(f);
            }
        }
        if (g_errors.reported) printf("PhysX reported %d errors or warnings (see stderr)\n", g_errors.reported);
    }
};

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--backend") o.gpu = std::string(next()) == "gpu";
        else if (a == "--threads") o.threads = atoi(next());
        else if (a == "--gpu-threads") o.gpu_threads = std::max(1, atoi(next()));
        else if (a == "--scenes") o.scenes = std::max(1, atoi(next()));
        else if (a == "--pieces") o.pieces = atoi(next());
        else if (a == "--sites") o.sites = std::max(1, atoi(next()));
        else if (a == "--bursts") o.bursts = std::max(1, atoi(next()));
        else if (a == "--burst-interval") o.burst_interval = atoi(next());
        else if (a == "--settle") o.settle = atoi(next());
        else if (a == "--explosions") o.explosions = atoi(next());
        else if (a == "--quake") o.quake = atoi(next());
        else if (a == "--pgs") o.tgs = false;
        else if (a == "--iters") o.pos_iters = atoi(next());
        else if (a == "--stabilization") o.stabilization = true;
        else if (a == "--freeze") o.freeze = true;
        else if (a == "--freeze-speed") o.freeze_speed = float(atof(next()));
        else if (a == "--freeze-after") o.freeze_after = atoi(next());
        else if (a == "--linear-damping") o.linear_damping = float(atof(next()));
        else if (a == "--angular-damping") o.angular_damping = float(atof(next()));
        else if (a == "--sleep-threshold") o.sleep_threshold = float(atof(next()));
        else if (a == "--max-depenetration") o.max_depenetration = float(atof(next()));
        else if (a == "--settled") {  // a settling preset: what a game would ship with
            o.stabilization = true;
            o.linear_damping = 0.1f;
            o.angular_damping = 0.3f;
            o.sleep_threshold = 0.05f;
            o.max_depenetration = 3.0f;
        }
        else if (a == "--csv") o.csv = next();
        else if (a == "--summary") o.summary = next();
        else if (a == "--label") o.label = next();
        else {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        }
    }
    if (o.label.empty()) o.label = "bench";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return 2;
    Bench b;
    b.o = o;
    if (!b.init()) {
        fprintf(stderr, "initialization failed\n");
        return 3;
    }
    const double t0 = now_ms();
    b.run();
    printf("%s: %d steps in %.1f s\n", o.label.c_str(), b.step, (now_ms() - t0) / 1000.0);
    b.report();
    // Leave the scene to process exit: releasing 100k actors one by one would only add time.
    return 0;
}
