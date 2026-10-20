#pragma once
#include <cstdint>
#include <string>

// State checkpoints for determinism checks: hashes of exact bit patterns (no quantization), so the first frame
// where a replay differs from its recording in any of these is found.
namespace checkpoint {

struct State {
    uint32_t frame = 0;
    int32_t state = 0;
    uint32_t rng = 0;      // game generator state and table, sim-thread stream positions, real CRT state
    uint32_t player = 0;   // local player position and orientation
    uint32_t objects = 0;  // order-independent hash of every non-effect world object's type and position
    uint32_t havok = 0;    // order-independent hash of every active rigid body's id, position and velocity
    int32_t n_objects = 0;
    int32_t n_active = 0;
    int32_t movers_destroyed = 0;
    float px = 0, py = 0, pz = 0;
};

State capture(uint32_t frame, int game_state);  // sim thread, end of frame

// Diagnostic: every world object (handle, type, subtype, position bits) as CSV, sorted by handle.
void dump_objects(const std::wstring& path);
// Diagnostic: every active rigid body (Havok uid, layer, position and velocity with their bits), sorted by uid.
void dump_bodies(const std::wstring& path);

const char* csv_header();
std::string to_csv(const State& s);
bool from_csv(const char* line, State& s);
// Comma-separated names of the fields that differ (empty when the states match). The gameplay fields are state,
// player, objects, havok and movers_destroyed; rng also counts streams of cosmetic call sites (audio variations,
// effects), so it is informational.
std::string diff(const State& recorded, const State& now, bool include_rng);

}  // namespace checkpoint
