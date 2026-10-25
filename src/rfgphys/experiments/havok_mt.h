#pragma once

struct Config;

// Experiment.HavokThreads=N: the Havok world steps with Havok's own multithreaded simulation, on the sim thread plus N
// worker threads (research/docs/01-findings.md §3.4 and §10.8).
//
// rfg.exe still contains hkpMultiThreadedSimulation, its job functions and hkJobQueue, but the game never uses them.
// The experiment:
// - creates the game's world (hkpWorld::hkpWorld from havok_system_init) with SIMULATION_TYPE_MULTITHREADED. Actions
//   and continuous physics (TOIs) stay on the sim thread; broadphase-border callbacks are postponed to the end of the
//   step; each island is solved by one thread (Havok's split solver can starve the job queue, see havok_mt.cpp);
// - builds an hkJobQueue and registers the dynamics and collide job handlers (hkpWorld::registerWithJobQueue itself
//   was dropped by the linker);
// - replaces the step body (hkpSimulation::stepDeltaTime, which hkpWorld::stepDeltaTime calls from havok_simulate)
//   with hkpMultiThreadedSimulation::stepBeginSt, the jobs on all threads, and finishMtStep. The post-collide callback
//   (the game's destruction work), activation callbacks, pending world operations and TOIs still run on the sim
//   thread after the jobs;
// - serializes the game callbacks that run inside the jobs (contact points added and removed, phantom overlaps,
//   trigger shapes) under Havok's own constraint-update lock (core/game_lock.h). Collision filters and the game's
//   custom shapes only read data and run unlocked;
// - works around the collide jobs' 16-bit counts, which a pile of thousands of colliding pieces outgrows (one island
//   with 65,536+ sectors of agents lost a whole step's contact changes, and the solver crashed soon after): such a
//   track's job is split in several, an island needing more tasks than the jobs can number gets bigger tasks, and a
//   step beyond even that runs single-threaded;
// - ends the process with a report of every Havok lock's owner and each Havok thread's stack when a step runs longer
//   than 5 s (tools/hang_report.py resolves it).
// Callbacks reach the game in a different order than in the single-threaded step, so replays diverge from
// single-threaded ones.
namespace experiments {

bool install_havok_mt(const Config& cfg);

// Sim thread: the time the multithreaded steps of the frame spent in their job phase (ms), then resets it.
float havok_mt_take_jobs_ms();

// True while a multithreaded step's jobs run (on whatever thread asks).
bool havok_mt_in_jobs();

}  // namespace experiments
