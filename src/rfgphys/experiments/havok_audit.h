#pragma once

struct Config;

// Test.HavokAudit=1 (diagnostic): checks the island bookkeeping Havok's solver sizes its buffer by.
//
// hkpConstraintSolverSetup::calcBufferOffsetsForSolve lays an island's solver buffer out from the island's own totals
// (hkpSimulationIsland::m_constraintInfo and m_numConstraints): the jacobian schemas get the island's schema bytes,
// and the solver's temporaries follow right after. Totals smaller than what the island's constraints really need let
// the schemas run into the temporaries, which the solver then overwrites: an unknown schema (the int3 at
// hkSolveConstraints+0x2332), the crash the multithreaded step showed after giant shatters.
//
// The audit compares those totals with the sums over the island's constraints (hkConstraintInternal) and checks every
// constraint's owner and internal pointer:
// - for every island right before it is solved (hkpConstraintSolverSetup::solve, on whichever thread solves it), and
//   checks that no two islands solved at the same time share solver memory;
// - for every active and inactive island right before the world steps (havok_update_status), between the
//   multithreaded jobs and the step's end, and right after (havok_sim_islands_process_post);
// - every collide task: what it hands its island (the totals' change it gathered, for the island's post-collide job
//   or straight into the island) against how much its agents' contact constraints really moved, and whether those
//   constraints and the agents' bodies are in the task's island;
// - island merges asked for inside the jobs (a constraint between bodies of two islands; Havok queues the merge);
// - with Test.HavokAuditJobs=1, also after each post-collide job and each island split inside the jobs, and around
//   each island's broadphase job (slow enough to change the jobs' timing).
// Mismatches are logged ("Havok audit: ..."; the first 20, then every 1000th) with a summary every 30 s and at exit.
// A crash inside the solver logs the island being solved and where in its buffer the solver was, and writes the
// island's solver block (havok_crash_solver.bin) and its bodies and constraints with their atoms
// (havok_crash_island.txt) into the run folder for offline decoding.
// Test.HavokAudit=2 also repairs: an island about to be solved with totals below its constraints' sums gets the sums,
// and a buffer laid out from the old totals is swapped for one of the right size for that solve.
namespace experiments {

bool install_havok_audit(const Config& cfg);

// The multithreaded step (experiments/havok_mt.h): every island after the jobs, before finishMtStep (the TOIs and the
// queued world operations), on the sim thread. Nothing without Test.HavokAudit.
void havok_audit_after_jobs();

}  // namespace experiments
