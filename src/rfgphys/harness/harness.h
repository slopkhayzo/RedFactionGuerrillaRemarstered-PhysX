#pragma once
#include <cstdint>

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

// The replay's current frame (the recording's frame numbers, as its markers and checkpoints use them); -1 when not
// replaying. Any thread.
int32_t replay_frame();

// Harness.ShotsAfterBlow=N: a screenshot on each of the next N frames (shot_<frame>.png), asked for when a wide melee
// blow starts and when a frame shatters 500 pieces or more. Nothing without the setting. Any thread.
void shot_series();

}  // namespace harness
