#pragma once
#include <cstdint>
#include <string>

// Player track (track.bin): per session frame the camera's view and the local player's pose, and every projectile the
// local player fires (where it starts and the point it flies at). Recordings store the track of the live session.
// Replays lock to the recording's track, so a variant that changes physics still looks and shoots at the same spots:
// each shot gets the recorded start and target, the camera shows the recorded view, and the player is put back on
// the recorded path when something pushed them further off it than a tolerance. Every replay also writes its own
// track into the run folder (a vanilla replay's track can become the reference of a recording made before tracks).
namespace track {

bool install();

// Starts writing the session's track to `path` (the harness calls it when gameplay starts); end_writing closes it.
bool begin_writing(const std::wstring& path);
void end_writing();

// Replay: loads `path` and locks to it. False when the file is missing or unreadable.
bool load(const std::wstring& path, float tolerance_m);

// The session frame being simulated (-1 outside a session).
void set_frame(int32_t frame);

// Replay statistics as JSON members (no braces), for replay.json.
std::string stats_json();

}  // namespace track
