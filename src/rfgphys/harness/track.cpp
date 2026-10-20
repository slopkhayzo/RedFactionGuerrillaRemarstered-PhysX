#include "harness/track.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace track {
namespace {

constexpr char kMagic[8] = {'R', 'F', 'G', 'T', 'R', 'K', '0', '1'};
// Game layouts (rfg.pdb).
constexpr uint32_t kCameraRealPos = 0x2C, kCameraRealOrient = 0x50;  // rfg_camera (camera_do_frame_post copies these
                                                                     // into render_pos / render_orient)
constexpr uint32_t kObjectPos = 0x4, kObjectOrient = 0x10, kObjectHandle = 0x6C;
constexpr uint32_t kItemOwnerHandle = 0x13C;  // item::owner_handle (a weapon's holder)

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t pose_size;
    uint32_t shot_size;
    uint32_t reserved[5];
};
struct Pose {
    uint32_t frame;
    uint32_t flags;
    float cam_pos[3];
    float cam_orient[9];
    float player_pos[3];
    float player_orient[9];
};
struct Shot {
    uint32_t frame;
    uint32_t seq;  // order within the frame
    float start[3];
    float target[3];
};
#pragma pack(pop)
enum : uint32_t { kPoseCamera = 1, kPosePlayer = 2 };
enum : int { kTagPose = 'P', kTagShot = 'S' };

// Writing (record and replay). Records collect in memory and reach the file when the session ends: file writes on
// the sim thread during a session would perturb its timing (and heavy-destruction physics is timing-sensitive).
std::wstring g_out_path;
std::vector<uint8_t> g_out;
bool g_writing = false;
int32_t g_frame = -1;
int32_t g_last_pose_frame = -1;
int32_t g_shot_frame = -1;
uint32_t g_shot_seq = 0;

// Locking (replay).
bool g_locked = false;
float g_tolerance_sq = 0.0625f;
std::vector<Pose> g_poses;  // indexed by frame; flags == 0 where the track has no pose
std::vector<Shot> g_shots;  // in file order (frame, then seq)
std::unordered_map<uint32_t, size_t> g_first_shot;  // frame -> index of its first shot
uint32_t g_last_track_frame = 0;
uint32_t g_camera_locked = 0, g_corrections = 0, g_shots_fired = 0, g_shots_locked = 0, g_shots_suppressed = 0,
         g_shots_forced = 0, g_shots_missed = 0;
float g_max_deviation = 0;

void(__cdecl* o_camera_post)();
void(__fastcall* o_fire_projectile)(uint8_t*, void*, float*, float*);

uint8_t* local_player() { return rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)(); }
uint8_t* camera() { return static_cast<uint8_t*>(rfg::addr(rfg::rva::Rfg_camera)); }

void write_record(int tag, const void* data, size_t size) {
    if (!g_writing) return;
    g_out.push_back(uint8_t(tag));
    const auto* p = static_cast<const uint8_t*>(data);
    g_out.insert(g_out.end(), p, p + size);
}

const Pose* recorded_pose(int32_t frame) {
    if (frame < 0 || size_t(frame) >= g_poses.size() || !g_poses[size_t(frame)].flags) return nullptr;
    return &g_poses[size_t(frame)];
}

const Shot* recorded_shot(int32_t frame, uint32_t seq) {
    auto it = g_first_shot.find(uint32_t(frame));
    if (it == g_first_shot.end()) return nullptr;
    const size_t i = it->second + seq;
    if (i >= g_shots.size() || g_shots[i].frame != uint32_t(frame) || g_shots[i].seq != seq) return nullptr;
    return &g_shots[i];
}

// Puts the player back on the recorded path: human::update_pos_and_orient moves the object and its character
// controller, and the movement velocity (whatever pushed the player) is cleared, as human_teleport_unsafe does.
void correct_player(uint8_t* player, const Pose& rec, float deviation) {
    if (g_corrections < 10)
        LOG("track: frame %d: player %.2f m off the recorded path, put back", g_frame, deviation);
    float pos[3], orient[9];
    memcpy(pos, rec.player_pos, sizeof(pos));
    memcpy(orient, rec.player_orient, sizeof(orient));
    rfg::fn<void(__fastcall*)(void*, void*, float*, float*, bool)>(rfg::rva::human__update_pos_and_orient)(
        player, nullptr, pos, orient, true);
    float zero[3] = {0, 0, 0};
    rfg::fn<void(__cdecl*)(void*, float*)>(rfg::rva::human_controller_set_vel)(player, zero);
    ++g_corrections;
}

// Recorded shots of this frame that the replay did not fire (the player was knocked down, the weapon was not ready):
// fired now from the equipped weapon with the recorded start and target, so a variant gets every recorded shot.
// Called at the end of the frame's simulation, after the player's weapons ran.
void fire_missing_shots(uint8_t* player) {
    auto it = g_first_shot.find(uint32_t(g_frame));
    if (it == g_first_shot.end()) return;
    const uint32_t fired = g_shot_frame == g_frame ? g_shot_seq : 0;
    uint8_t* weapon = nullptr;
    if (player) weapon = rfg::fn<uint8_t*(__fastcall*)(void*, void*)>(rfg::rva::human__get_equipped_weapon)(player, nullptr);
    for (size_t i = it->second; i < g_shots.size() && g_shots[i].frame == uint32_t(g_frame); ++i) {
        Shot s = g_shots[i];
        if (s.seq < fired) continue;
        if (!weapon) {
            ++g_shots_missed;
            continue;
        }
        if (g_shots_forced < 10) LOG("track: frame %d: recorded shot %u not fired by the replay, fired now", g_frame, s.seq);
        o_fire_projectile(weapon, nullptr, s.start, s.target);  // the trampoline: not seen by d_fire_projectile
        ++g_shots_forced;
        write_record(kTagShot, &s, sizeof(s));
    }
}

// camera_do_frame_post: the end of the gameplay simulation frame, right before the camera's real pose becomes the
// render pose. Only the call from gamestate_gp_simulate_frame counts (the UI and level init call it too).
void __cdecl d_camera_post() {
    const uint32_t caller = uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress()) - rfg::base());
    const bool gameplay_call = caller - rfg::rva::gamestate_gp_simulate_frame < rfg::size::gamestate_gp_simulate_frame;
    if (gameplay_call && g_frame >= 0 && g_frame != g_last_pose_frame) {
        g_last_pose_frame = g_frame;
        uint8_t* cam = camera();
        uint8_t* player = local_player();
        if (g_locked) {
            if (const Pose* rec = recorded_pose(g_frame)) {
                if (rec->flags & kPoseCamera) {
                    memcpy(cam + kCameraRealPos, rec->cam_pos, sizeof(rec->cam_pos));
                    memcpy(cam + kCameraRealOrient, rec->cam_orient, sizeof(rec->cam_orient));
                    ++g_camera_locked;
                }
                if ((rec->flags & kPosePlayer) && player) {
                    const float* pos = reinterpret_cast<const float*>(player + kObjectPos);
                    const float dx = pos[0] - rec->player_pos[0], dy = pos[1] - rec->player_pos[1],
                                dz = pos[2] - rec->player_pos[2];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    g_max_deviation = std::max(g_max_deviation, std::sqrt(d2));
                    if (d2 > g_tolerance_sq) correct_player(player, *rec, std::sqrt(d2));
                }
            }
            fire_missing_shots(player);
        }
        Pose p{};
        p.frame = uint32_t(g_frame);
        p.flags = kPoseCamera;
        memcpy(p.cam_pos, cam + kCameraRealPos, sizeof(p.cam_pos));
        memcpy(p.cam_orient, cam + kCameraRealOrient, sizeof(p.cam_orient));
        if (player) {
            p.flags |= kPosePlayer;
            memcpy(p.player_pos, player + kObjectPos, sizeof(p.player_pos));
            memcpy(p.player_orient, player + kObjectOrient, sizeof(p.player_orient));
        }
        write_record(kTagPose, &p, sizeof(p));
    }
    o_camera_post();
}

// weapon::fire_projectile(start, target): the projectile flies from start towards target (after spread).
void __fastcall d_fire_projectile(uint8_t* weapon, void* edx, float* start, float* target) {
    uint8_t* player = g_frame >= 0 ? local_player() : nullptr;
    if (!player || !weapon || !start || !target ||
        *reinterpret_cast<uint32_t*>(weapon + kItemOwnerHandle) != *reinterpret_cast<uint32_t*>(player + kObjectHandle)) {
        o_fire_projectile(weapon, edx, start, target);
        return;
    }
    if (g_shot_frame != g_frame) {
        g_shot_frame = g_frame;
        g_shot_seq = 0;
    }
    Shot shot{uint32_t(g_frame), g_shot_seq++, {start[0], start[1], start[2]}, {target[0], target[1], target[2]}};
    ++g_shots_fired;
    if (g_locked && uint32_t(g_frame) <= g_last_track_frame) {
        const Shot* rec = recorded_shot(g_frame, shot.seq);
        if (!rec) {
            // A shot the recording does not have (the player could fire here only because physics differ): dropped,
            // so a variant's destruction comes from the recorded shots alone.
            if (g_shots_suppressed++ < 10) LOG("track: frame %d: shot %u is not in the recording, dropped", g_frame, shot.seq);
            return;
        }
        memcpy(shot.start, rec->start, sizeof(shot.start));
        memcpy(shot.target, rec->target, sizeof(shot.target));
        ++g_shots_locked;
    }
    write_record(kTagShot, &shot, sizeof(shot));
    o_fire_projectile(weapon, edx, shot.start, shot.target);
}

}  // namespace

bool install() {
    bool ok = hooks::create(rfg::rva::camera_do_frame_post, &d_camera_post, &o_camera_post, "camera_do_frame_post");
    ok &= hooks::create(rfg::rva::weapon__fire_projectile, &d_fire_projectile, &o_fire_projectile,
                        "weapon::fire_projectile");
    return ok;
}

bool begin_writing(const std::wstring& path) {
    end_writing();
    g_out_path = path;
    g_out.clear();
    g_out.reserve(size_t(32000) * (sizeof(Pose) + 1));  // ~2 minutes of frames without reallocating
    FileHeader h{};
    memcpy(h.magic, kMagic, sizeof(kMagic));
    h.version = 1;
    h.pose_size = sizeof(Pose);
    h.shot_size = sizeof(Shot);
    const auto* p = reinterpret_cast<const uint8_t*>(&h);
    g_out.insert(g_out.end(), p, p + sizeof(h));
    g_writing = true;
    g_last_pose_frame = -1;
    g_shot_frame = -1;
    return true;
}

void end_writing() {
    if (!g_writing) return;
    g_writing = false;
    if (FILE* f = _wfsopen(g_out_path.c_str(), L"wb", _SH_DENYWR)) {
        fwrite(g_out.data(), 1, g_out.size(), f);
        fclose(f);
    } else {
        LOG("track: cannot write %ls", g_out_path.c_str());
    }
    g_out.clear();
    g_out.shrink_to_fit();
}

bool load(const std::wstring& path, float tolerance_m) {
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYWR);
    if (!f) return false;
    FileHeader h{};
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, kMagic, sizeof(kMagic)) != 0 || h.pose_size != sizeof(Pose) ||
        h.shot_size != sizeof(Shot)) {
        fclose(f);
        LOG("track: %ls is not a track this plugin can read", path.c_str());
        return false;
    }
    g_poses.clear();
    g_shots.clear();
    g_first_shot.clear();
    for (int tag; (tag = fgetc(f)) != EOF;) {
        if (tag == kTagPose) {
            Pose p;
            if (fread(&p, sizeof(p), 1, f) != 1) break;
            if (p.frame >= g_poses.size()) g_poses.resize(size_t(p.frame) + 1, Pose{});
            g_poses[p.frame] = p;
        } else if (tag == kTagShot) {
            Shot s;
            if (fread(&s, sizeof(s), 1, f) != 1) break;
            g_first_shot.emplace(s.frame, g_shots.size());  // keeps the first shot of each frame
            g_shots.push_back(s);
        } else {
            break;
        }
    }
    fclose(f);
    g_locked = true;
    g_tolerance_sq = tolerance_m * tolerance_m;
    g_last_track_frame = g_poses.empty() ? 0 : uint32_t(g_poses.size() - 1);
    LOG("track: replay locked to %ls: %zu frames, %zu shots, player tolerance %.2f m", path.c_str(), g_poses.size(),
        g_shots.size(), tolerance_m);
    return true;
}

void set_frame(int32_t frame) { g_frame = frame; }

std::string stats_json() {
    char b[512];
    snprintf(b, sizeof(b),
             "\"track_locked\": %s, \"track_camera_frames_locked\": %u, \"track_player_corrections\": %u, "
             "\"track_player_max_deviation_m\": %.3f, \"track_shots_recorded\": %zu, \"track_shots_fired\": %u, "
             "\"track_shots_locked\": %u, \"track_shots_forced\": %u, \"track_shots_suppressed\": %u, "
             "\"track_shots_missed\": %u",
             g_locked ? "true" : "false", g_camera_locked, g_corrections, g_max_deviation, g_shots.size(),
             g_shots_fired, g_shots_locked, g_shots_forced, g_shots_suppressed, g_shots_missed);
    return b;
}

}  // namespace track
