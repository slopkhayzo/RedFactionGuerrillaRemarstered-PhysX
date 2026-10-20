#pragma once

struct Config;

// Deterministic record/replay harness (research/docs/04-test-harness.md).
//
// Harness.Mode=record: every gameplay session that starts with a Wrecking Crew match or a save load becomes a
// recording bundle (input.rfgrec, checkpoints.csv, manifest.json, the loaded save): per main-loop frame the polled
// input state, the frame time, window focus and the load governor's physics-time readings. The random generators are
// seeded when the match or load starts, and wall-clock reads on the sim thread follow the recorded frame times.
// Harness.Mode=replay: starts the recorded match or save without input, feeds the frames back with the same seeds,
// compares checkpoints with the recording, writes replay.json into the run folder and quits.
namespace harness {

bool install(const Config& cfg);

}  // namespace harness
