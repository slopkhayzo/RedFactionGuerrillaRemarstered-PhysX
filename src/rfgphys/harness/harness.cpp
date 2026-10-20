#include "harness/harness.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/gameloop.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "harness/checkpoint.h"
#include "harness/rec_file.h"
#include "harness/rng_guard.h"
#include "harness/screenshot.h"
#include "harness/track.h"
#include "hooks/hooks.h"
#include "test/automation.h"

#pragma intrinsic(_ReturnAddress)

namespace harness {
namespace {

constexpr int GS_MAINMENU = 0, GS_GAMEPLAY = 1, GS_LOAD = 2, GS_WRECKING_CREW_MAIN_MENU = 19,
              GS_SAVE_LOAD_SCREEN = 50;
// A recording ends when the game reaches one of these states (left gameplay or started another load).
constexpr int kEndStates[] = {0 /* MAINMENU */,  2 /* LOAD */,      3 /* BOOT */,       4 /* MULTI_INIT */,
                              19 /* WC_MAIN_MENU */, 48 /* MAINMENU_IDLE */, 55 /* VERIFY_SAVEGAME */,
                              56 /* SHUTDOWN */, 59 /* WC_INIT */,   60 /* WC_SHUTDOWN */};

// Game layouts (rfg.pdb).
constexpr uint32_t kWcOptionsSize = 0x2C;             // wrecking_crew_options
constexpr uint32_t kWcOptionsMode = 0x0, kWcOptionsMap = 0x8;
constexpr uint32_t kWcPlayerInfoSize = 0xC14;         // wc_player_info
constexpr uint32_t kWcPlayerCharacterIndex = 0x58;
constexpr int kWcMenuLoaded = 1;                      // wc_shell_info::state MMS_WC_LOADED
constexpr uint32_t kSaveInfoSize = 0xBC;              // game_save_info
constexpr uint32_t kSaveInfoSlot = 0x34;              // game_save_info::save_slot_index
constexpr uint32_t kInputPollReturn = 0x13A;          // controls_poll+0x135 calls input_poll
constexpr uint32_t kRandomseedReturn = 0x38;          // math_randomseed+0x32 calls srand
// cached_input_state: controller (buttons_down +0x8, sticks +0x208, gyro +0x308), mouse (+0x328: pos, relative
// move x/y +0x330/+0x334, wheel +0x338, buttons[5] +0x33C), keyboard keys[256] +0x344.
constexpr uint32_t kInControllerButtons = 0x8, kInControllerAnalogEnd = 0x324;
constexpr uint32_t kInMouseMoveX = 0x330, kInMouseMoveY = 0x334, kInMouseButtons = 0x33C, kInKeys = 0x344;

enum class Mode { kOff, kRecord, kReplay };
enum class Phase { kIdle, kPending, kActive };

Config g_cfg;
Mode g_mode = Mode::kOff;
Phase g_phase = Phase::kIdle;
bool g_synthetic = false;  // input comes from the recording or a script; real devices and focus are ignored

rec::Header g_header{};
rec::Frame g_cur{};         // record: the frame being captured; replay: the frame being fed
rec::Frame g_transition{};  // replay: the load transition frame, fed during every frame while a start is pending
uint32_t g_frame = 0;       // frames since the transition frame
bool g_seen_load = false;
uint32_t g_pending_frames = 0;
uint32_t g_gov_index = 0;
int32_t g_last_gov = 0;
bool g_inject = false;      // this frame's input state and frame time come from g_cur
float g_saved_frametime_min = 0;
double g_game_seconds = 0;  // game time since frame 0 (sum of frame times)
int64_t g_active_qpc = 0;

// Virtual clock for timer_get on the sim thread: frozen at the anchor from the start event to frame 0, then
// advanced by the recorded frame times.
bool g_clock_on = false;
int32_t g_clock_anchor_ms = 0;
double g_clock_elapsed_ms = 0;

// Randomness bookkeeping.
uint32_t g_srand_calls = 0;
std::atomic<uint32_t> g_rand_other{0};
std::string g_rng_other_summary;  // game generator draws on other threads during the last session

// Recording output.
std::wstring g_bundle_dir;
rec::Writer g_writer;
rec::TextFile g_checkpoints;
std::vector<uint32_t> g_markers;
bool g_stop_requested = false;
bool g_f10_down = false, g_f11_down = false;
bool g_flushing = false;  // inside a session flush handler: never call session::exit_now from there

// Diagnostics (both modes).
std::set<uint32_t> g_shot_frames;
std::set<uint32_t> g_dump_frames;  // frames whose world objects are written to objects_<frame>.csv

std::set<uint32_t> parse_frames(const std::wstring& list) {
    std::set<uint32_t> frames;
    for (const wchar_t* p = list.c_str(); *p;) {
        wchar_t* end = nullptr;
        const unsigned long frame = wcstoul(p, &end, 10);
        if (end == p) {
            ++p;
            continue;
        }
        frames.insert(uint32_t(frame));
        p = end;
    }
    return frames;
}
rec::TextFile g_fingerprints;  // replay: per-frame randomness fingerprint, recorded vs now

// Replay input.
rec::Reader g_reader;
std::map<uint32_t, checkpoint::State> g_expected;
int64_t g_first_rng_divergence = -1;
bool g_replay_starting = false;  // the harness itself is calling the start function
uint32_t g_checkpoints_compared = 0, g_checkpoints_mismatched = 0, g_state_mismatches = 0;
uint32_t g_checkpoints_rng_differs = 0;  // informational: random streams include cosmetic call sites
int64_t g_first_divergence = -1;
std::string g_first_divergence_fields;
std::string g_load_error;

// Record with AutoStart=wc and AutoStartFrom: the Wrecking Crew setup of that recording.
rec::Header g_setup_from{};
bool g_has_setup_from = false;

// Start automation (replay, and record with AutoStart=wc).
int g_last_state = -1000;
int64_t g_state_since = 0;
int64_t g_ready_since = 0, g_wc_menu_since = 0;
bool g_caught_input = false, g_menu_pressed = false, g_start_called = false;

// Input script (record self-tests).
struct ScriptEvent {
    double t;
    enum Kind { kLook, kMouse, kKey, kStop } kind;
    float dx, dy, duration;
    int index;
    bool down;
};
std::vector<ScriptEvent> g_script;
bool g_has_script = false;
// InputFromRecording: another recording's input frames play the session (record mode, unattended).
rec::Reader g_input_from;
rec::Frame g_input_from_transition;
bool g_input_from_active = false;
size_t g_script_next = 0;
struct Look {
    double until;
    float dx, dy;
};
std::vector<Look> g_looks;
uint8_t g_script_state[rec::kInputSize];
float g_last_dt = 1.f / 60.f;

using VoidFn = void(__cdecl*)();
VoidFn o_input_poll, o_match_begin;
void(__cdecl* o_verify_save)(uint8_t*, bool);
bool(__fastcall* o_input_event)(void*, void*, uint8_t*);
bool(__cdecl* o_os_foreground)();
int(__cdecl* o_gov)();
int(__cdecl* o_timer_get)(int);
void(__cdecl* o_srand)(unsigned);
int(__cdecl* o_rand)();

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (uint8_t(c) < 0x20) continue;
        out += c;
    }
    return out;
}

void make_dirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); ++i)
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

std::string local_time(const char* fmt) {
    time_t t = time(nullptr);
    tm local{};
    localtime_s(&local, &t);
    char buf[64];
    strftime(buf, sizeof(buf), fmt, &local);
    return buf;
}

uint8_t* input_states() { return static_cast<uint8_t*>(rfg::addr(rfg::rva::Cached_input_states)); }
uint8_t* current_input() {
    return input_states() + rfg::global<uint8_t>(rfg::rva::Current_cached_input_state) * rec::kInputSize;
}
bool is_end_state(int s) {
    for (int e : kEndStates)
        if (s == e) return true;
    return false;
}
bool multiplayer() { return rfg::global<uint8_t>(rfg::rva::Multiplayer) != 0; }

// The rate whose reciprocal frametime_calculate turns into exactly `dt`: it computes (float)(1.0 / (double)rate).
float rate_for(float dt) {
    float r = float(1.0 / double(dt));
    for (int i = 0; i < 64; ++i) {
        const float got = float(1.0 / double(r));
        if (got == dt) break;
        r = got > dt ? std::nextafter(r, INFINITY) : std::nextafter(r, 0.f);
    }
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// Frame clock (frametime_calculate's counters, FPS average and the timestamp clock)

void capture_clock(rec::FrameClock& c) {
    namespace r = rfg::rva;
    c.framecount = rfg::global<int32_t>(r::Framecount);
    c.total_ms = rfg::global<uint32_t>(r::Frametime_total_milliseconds);
    c.total_ms_always = rfg::global<uint32_t>(r::Frametime_total_milliseconds_increment_always);
    c.sample_count = rfg::global<int32_t>(r::Sample_count);
    c.frametime_total = rfg::global<float>(r::Frametime_total);
    c.fps = rfg::global<float>(r::Frames_per_second);
    memcpy(c.frametimes, rfg::addr(r::Frametimes), sizeof(c.frametimes));
    c.ticker = rfg::global<int32_t>(r::Timestamp_ticker);
    c.realtime_ticker = rfg::global<int32_t>(r::Timestamp_realtime_ticker);
    c.amount_rounded = rfg::global<float>(r::amount_rounded);
}

void restore_clock(const rec::FrameClock& c) {
    namespace r = rfg::rva;
    rfg::global<int32_t>(r::Framecount) = c.framecount;
    rfg::global<uint32_t>(r::Frametime_total_milliseconds) = c.total_ms;
    rfg::global<uint32_t>(r::Frametime_total_milliseconds_increment_always) = c.total_ms_always;
    rfg::global<int32_t>(r::Sample_count) = c.sample_count;
    rfg::global<float>(r::Frametime_total) = c.frametime_total;
    rfg::global<float>(r::Frames_per_second) = c.fps;
    memcpy(rfg::addr(r::Frametimes), c.frametimes, sizeof(c.frametimes));
    rfg::global<int32_t>(r::Timestamp_ticker) = c.ticker;
    rfg::global<int32_t>(r::Timestamp_realtime_ticker) = c.realtime_ticker;
    rfg::global<float>(r::amount_rounded) = c.amount_rounded;
}

// ---------------------------------------------------------------------------------------------------------------
// Randomness

// session_start: the start event (all streams restart); otherwise a loading frame (the sim thread's restart).
void seed_rngs(bool session_start) {
    rng_guard::reseed(g_header.seed, session_start);  // rand_regenerate: MT, table and index
    if (o_srand) o_srand(g_header.seed);  // MSVCR100 keeps rand() state per thread: this is the sim thread's
    g_srand_calls = 0;
}

// The game seeds rand() itself: create_cells with each terrain cell's index (deterministic placement, passed
// through), Lua's math.randomseed with whatever a script passes (possibly os.time()). During a session the Lua
// seeds are replaced by seeds derived from the recording's seed.
void __cdecl d_srand(unsigned seed) {
    if (g_phase != Phase::kIdle && _ReturnAddress() == rfg::addr(rfg::rva::math_randomseed + kRandomseedReturn)) {
        const unsigned replaced = g_header.seed ^ (0x9E3779B9u * ++g_srand_calls);
        LOG("harness: math.randomseed(%u) replaced by %u", seed, replaced);
        seed = replaced;
    }
    o_srand(seed);
}

// rand() state is per thread; calls from threads other than the sim thread are not controlled. Their callers are
// logged so they can be checked.
std::atomic<uintptr_t> g_rand_other_callers[4];

int __cdecl d_rand() {
    if (g_phase != Phase::kIdle) {
        if (!gameloop::on_sim_thread()) {
            const uint32_t n = g_rand_other.fetch_add(1, std::memory_order_relaxed);
            if (n < 4) g_rand_other_callers[n] = reinterpret_cast<uintptr_t>(_ReturnAddress()) - rfg::base();
        } else {
            int value;
            if (rng_guard::crt_rand(reinterpret_cast<uintptr_t>(_ReturnAddress()), value)) return value;
        }
    }
    return o_rand();
}

// create_cells(zone, layer, x, y): foliage placement, seeded per terrain cell with srand(); keeps the real CRT.
void(__cdecl* o_create_cells)(void*, void*, int, int);
void __cdecl d_create_cells(void* zone, void* layer, int x, int y) {
    rng_guard::enter_cells();
    o_create_cells(zone, layer, x, y);
    rng_guard::leave_cells();
}

std::string rand_other_callers() {
    std::string out;
    for (uint32_t i = 0; i < 4 && i < g_rand_other.load(); ++i) {
        char b[16];
        snprintf(b, sizeof(b), "%s%08X", out.empty() ? "" : " ", uint32_t(g_rand_other_callers[i].load()));
        out += b;
    }
    return out.empty() ? "-" : out;
}


// ---------------------------------------------------------------------------------------------------------------
// Wall clock, load governor, focus, input

uint32_t virtual_ms() { return uint32_t(g_clock_anchor_ms) + uint32_t(g_clock_elapsed_ms); }

// True when `ret` (a return address) lies inside the function at `rva` (`size` bytes of code).
bool called_from(uintptr_t ret, uint32_t rva, uint32_t size) { return ret - rfg::base() - rva < size; }

int __cdecl d_timer_get(int units) {
    if (g_clock_on && gameloop::on_sim_thread()) {
        const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        // gamestate_gp_simulate_frame times the Havok step for the load governor: real time.
        if (!called_from(ret, rfg::rva::gamestate_gp_simulate_frame, rfg::size::gamestate_gp_simulate_frame)) {
            const uint32_t ms = virtual_ms();
            if (units == 1) return int(ms / 1000u);
            if (units == 1000) return int(ms);
            if (units == 1000000) return int(ms * 1000u);
        }
    }
    return o_timer_get(units);
}

// keen::SystemTimer::getElapsedTime (milliseconds): the input code reads it directly for button hold times, poll
// intervals, double clicks and menu repeat. On the sim thread during a session those reads get the virtual clock,
// so a held button lasts as long in a fast replay as it did while recording.
uint32_t(__fastcall* o_elapsed)(void*, void*);
uint32_t __fastcall d_elapsed(void* timer, void* edx) {
    if (g_clock_on && gameloop::on_sim_thread()) {
        namespace r = rfg::rva;
        namespace s = rfg::size;
        const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (called_from(ret, r::input_poll, s::input_poll) || called_from(ret, r::key_mark, s::key_mark) ||
            called_from(ret, r::mouse_mark_button, s::mouse_mark_button) ||
            called_from(ret, r::mouse_poll, s::mouse_poll) ||
            called_from(ret, r::scroll_info__eval, s::scroll_info__eval) ||
            called_from(ret, r::input_update_idle_time, s::input_update_idle_time))
            return virtual_ms();
    }
    return o_elapsed(timer, edx);
}

// gamestate_gp_last_havok_sim_time(): microseconds the last Havok step took, read by the load governor (rfgl).
int __cdecl d_gov() {
    if (g_phase == Phase::kIdle || !gameloop::on_sim_thread()) return o_gov();
    if (g_mode == Mode::kReplay) {
        if (g_gov_index < g_cur.n_gov) g_last_gov = g_cur.gov[g_gov_index];
        ++g_gov_index;
        return g_last_gov;
    }
    const int v = o_gov();
    if (g_cur.n_gov < rec::kMaxGov) g_cur.gov[g_cur.n_gov++] = v;
    return v;
}

bool __cdecl d_os_foreground() {
    if (!g_synthetic) return o_os_foreground();
    return g_phase == Phase::kIdle || (g_cur.flags & rec::kFrameAppActive) != 0;
}

void set_app_active() {
    if (g_synthetic)
        rfg::global<uint8_t>(rfg::rva::Os_app_active) =
            g_phase == Phase::kIdle || (g_cur.flags & rec::kFrameAppActive) ? 1 : 0;
}

// Application::processInputEvent(InputEvent*): event type at +3; 7..0x1D are keyboard, mouse, controller and
// focus events. Synthetic sessions drop them all, so nobody at the machine can disturb a replay.
bool __fastcall d_input_event(void* app, void* edx, uint8_t* ev) {
    if (g_synthetic && ev && ev[3] >= 7 && ev[3] <= 0x1D) return true;
    return o_input_event(app, edx, ev);
}

void __cdecl d_input_poll() {
    const bool from_controls = _ReturnAddress() == rfg::addr(rfg::rva::controls_poll + kInputPollReturn);
    if (!from_controls || g_phase == Phase::kIdle) {
        o_input_poll();
        return;
    }
    if (g_inject) {  // input_poll makes the "next" buffer current: that is what the frame then reads
        const uint8_t next = rfg::global<uint8_t>(rfg::rva::Next_cached_input_state);
        memcpy(input_states() + next * rec::kInputSize, g_cur.input, rec::kInputSize);
    }
    o_input_poll();
    if (g_mode == Mode::kRecord) memcpy(g_cur.input, current_input(), rec::kInputSize);
    set_app_active();
}

// ---------------------------------------------------------------------------------------------------------------
// Input script (record self-tests)

bool load_script(const std::wstring& path) {
    std::string text;
    if (!rec::read_text_file(path, text)) {
        LOG("harness: cannot read input script %ls", path.c_str());
        return false;
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        double t = 0;
        char cmd[16] = {}, arg[16] = {};
        float a = 0, b = 0, c = 0;
        int index = 0;
        ScriptEvent e{};
        if (sscanf_s(line.c_str(), "%lf %15s", &t, cmd, unsigned(sizeof(cmd))) != 2) continue;
        e.t = t;
        if (strcmp(cmd, "look") == 0 && sscanf_s(line.c_str(), "%lf %15s %f %f %f", &t, cmd, unsigned(sizeof(cmd)), &a, &b, &c) == 5) {
            e.kind = ScriptEvent::kLook;
            e.dx = a;
            e.dy = b;
            e.duration = c;
        } else if ((strcmp(cmd, "mouse") == 0 || strcmp(cmd, "key") == 0) &&
                   sscanf_s(line.c_str(), "%lf %15s %d %15s", &t, cmd, unsigned(sizeof(cmd)), &index, arg,
                            unsigned(sizeof(arg))) == 4) {
            e.kind = cmd[0] == 'm' ? ScriptEvent::kMouse : ScriptEvent::kKey;
            e.index = index;
            e.down = strcmp(arg, "down") == 0;
            if ((e.kind == ScriptEvent::kMouse && (index < 0 || index > 4)) || index < 0 || index > 255) continue;
        } else if (strcmp(cmd, "stop") == 0) {
            e.kind = ScriptEvent::kStop;
        } else {
            LOG("harness: input script line ignored: %s", line.c_str());
            continue;
        }
        g_script.push_back(e);
    }
    LOG("harness: input script %ls: %zu events", path.c_str(), g_script.size());
    return true;
}

// Neutral input: the current state with all buttons, keys, sticks and mouse movement released.
void neutral_input(uint8_t* out) {
    memcpy(out, current_input(), rec::kInputSize);
    memset(out + kInControllerButtons, 0, kInControllerAnalogEnd - kInControllerButtons);
    memset(out + kInMouseMoveX, 0, kInMouseButtons + 5 - kInMouseMoveX);
    memset(out + kInKeys, 0, 256);
}

// Returns false when the script asks to stop.
bool build_scripted_input() {
    bool keep_going = true;
    while (g_script_next < g_script.size() && g_script[g_script_next].t <= g_game_seconds) {
        const ScriptEvent& e = g_script[g_script_next++];
        switch (e.kind) {
            case ScriptEvent::kLook: g_looks.push_back({e.t + e.duration, e.dx, e.dy}); break;
            case ScriptEvent::kMouse: g_script_state[kInMouseButtons + e.index] = e.down ? 1 : 0; break;
            case ScriptEvent::kKey: g_script_state[kInKeys + e.index] = e.down ? 1 : 0; break;
            case ScriptEvent::kStop: keep_going = false; break;
        }
    }
    memcpy(g_cur.input, g_script_state, rec::kInputSize);
    float dx = 0, dy = 0;
    for (const Look& l : g_looks) {
        if (g_game_seconds < l.until) {
            dx += l.dx * g_last_dt;
            dy += l.dy * g_last_dt;
        }
    }
    memcpy(g_cur.input + kInMouseMoveX, &dx, 4);
    memcpy(g_cur.input + kInMouseMoveY, &dy, 4);
    return keep_going;
}

// ---------------------------------------------------------------------------------------------------------------
// Recording bundle

void fill_header_common(uint32_t start_type) {
    memset(&g_header, 0, sizeof(g_header));
    memcpy(g_header.magic, "RFGREC01", 8);
    g_header.header_size = sizeof(rec::Header);
    g_header.input_size = rec::kInputSize;
    g_header.seed = g_cfg.harness_seed;
    g_header.start_type = start_type;
    g_header.flags = g_cfg.harness_virtual_clock ? rec::kFlagVirtualClock : 0;
    g_header.clock_anchor_ms = o_timer_get(1000);
    g_header.checkpoint_interval = uint32_t(g_cfg.harness_checkpoint_interval);
    g_header.exe_timestamp = rfg::kExeTimeDateStamp;
    capture_clock(g_header.clock);
}

bool open_bundle(const std::string& label) {
    std::wstring dir = g_cfg.harness_recordings_dir;
    if (dir.empty()) dir = session::run_dir() + L"\\recordings";
    g_bundle_dir = dir + L"\\" + widen(local_time("%Y%m%d-%H%M%S") + "-" + label);
    make_dirs(g_bundle_dir);
    if (!g_writer.open(g_bundle_dir + L"\\input.rfgrec", g_header)) {
        LOG("harness: cannot create %ls", g_bundle_dir.c_str());
        return false;
    }
    g_checkpoints.open(g_bundle_dir + L"\\checkpoints.csv");
    g_checkpoints.write(checkpoint::csv_header());
    g_markers.clear();
    g_stop_requested = false;
    LOG("harness: recording into %ls", g_bundle_dir.c_str());
    return true;
}

std::string wc_map_name(int map) {
    if (map < 0 || map >= rfg::global<int32_t>(rfg::rva::Wrecking_crew_num_maps)) return "?";
    const char* name = rfg::fn<const char*(__cdecl*)(int)>(rfg::rva::wrecking_crew_get_map_file_name)(map);
    return name ? name : "?";
}

std::string wc_mode_name(int mode) {
    if (mode < 0 || mode > 4) return "?";
    const char* name = rfg::fn<const char*(__cdecl*)(int)>(rfg::rva::wrecking_crew_get_mode_internal_name)(mode);
    return name ? name : "?";
}

std::string wc_weapon_name(int weapon) {
    if (weapon < 0) return "-";
    if (weapon >= rfg::global<int32_t>(rfg::rva::Wrecking_crew_num_weapons)) return "?";
    auto* info = rfg::fn<uint32_t*(__cdecl*)(int)>(rfg::rva::wrecking_crew_get_weapon_info)(weapon);
    if (!info) return "?";
    auto* w = rfg::fn<const char**(__cdecl*)(uint32_t)>(rfg::rva::weapons_get_weapon_info)(info[0]);
    return w && w[0] ? w[0] : "?";
}

std::string describe_wc(const uint8_t* o) {
    int32_t v[11];
    memcpy(v, o, sizeof(v));
    char buf[512];
    snprintf(buf, sizeof(buf),
             "mode %d (%s), players %d, map %d (%s), weapon %d (%s), backpack %d, walker %d, stress %d, rounds %d, "
             "barrels %d, free play %d, challenge %d",
             v[0], wc_mode_name(v[0]).c_str(), v[1], v[2], wc_map_name(v[2]).c_str(), v[4],
             wc_weapon_name(v[4]).c_str(), v[5], v[6], v[7], v[8], o[0x24], o[0x25], v[10]);
    return buf;
}

std::string file_stem(const char* path) {
    std::string s = path;
    if (const size_t slash = s.find_last_of("\\/"); slash != std::string::npos) s = s.substr(slash + 1);
    if (const size_t dot = s.find_last_of('.'); dot != std::string::npos) s.resize(dot);
    for (char& c : s)
        if (!isalnum(uint8_t(c)) && c != '_' && c != '-') c = '_';
    return s;
}

bool start_recording_wc() {
    fill_header_common(rec::kStartWreckingCrew);
    const auto* options = static_cast<const uint8_t*>(rfg::addr(rfg::rva::Wrecking_crew_options));
    memcpy(g_header.wc_options, options, kWcOptionsSize);
    const auto* players = static_cast<const uint8_t*>(rfg::addr(rfg::rva::Wc_player_infos));
    for (int i = 0; i < 4; ++i)
        memcpy(&g_header.wc_character_index[i], players + i * kWcPlayerInfoSize + kWcPlayerCharacterIndex, 4);
    int32_t map = 0, mode = 0;
    memcpy(&map, options + kWcOptionsMap, 4);
    memcpy(&mode, options + kWcOptionsMode, 4);
    strncpy_s(g_header.wc_map_file, wc_map_name(map).c_str(), _TRUNCATE);
    LOG("harness: Wrecking Crew match begins: %s", describe_wc(options).c_str());
    return open_bundle("wc-" + file_stem(g_header.wc_map_file) + "-" + wc_mode_name(mode));
}

bool start_recording_save(const uint8_t* info, bool reset_destruction) {
    fill_header_common(rec::kStartSave);
    memcpy(&g_header.save_slot, info + kSaveInfoSlot, 4);
    g_header.save_reset_destruction = reset_destruction ? 1 : 0;
    if (!open_bundle("save-slot" + std::to_string(g_header.save_slot))) return false;
    // keen_savegame_<profile>_<slot>.sav (SaveDataProviderWin32Steam::generateFileName)
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = g_cfg.harness_save_dir + L"\\keen_savegame_*_" + std::to_wstring(g_header.save_slot) + L".sav";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        const std::wstring src = g_cfg.harness_save_dir + L"\\" + fd.cFileName;
        if (CopyFileW(src.c_str(), (g_bundle_dir + L"\\" + fd.cFileName).c_str(), FALSE)) {
            strncpy_s(g_header.save_file, narrow(fd.cFileName).c_str(), _TRUNCATE);
            LOG("harness: save slot %u (%ls) copied into the recording", g_header.save_slot, fd.cFileName);
        } else {
            LOG("harness: copying %ls failed (%lu)", src.c_str(), GetLastError());
        }
    } else {
        LOG("harness: no save file matches %ls", pattern.c_str());
    }
    return true;
}

void write_manifest(const char* end_reason) {
    rec::TextFile m;
    if (!m.open(g_bundle_dir + L"\\manifest.json")) return;
    const double wall = g_active_qpc ? session::ticks_to_ms(session::qpc() - g_active_qpc) / 1000.0 : 0.0;
    m.printf("{\n  \"format\": \"rfgrec1\",\n  \"created\": \"%s\",\n", local_time("%Y-%m-%d %H:%M:%S").c_str());
    m.printf("  \"title\": \"\",\n  \"notes\": \"\",\n");
    m.printf("  \"game_exe_timestamp\": \"%08X\",\n  \"plugin_version\": \"%s\",\n", g_header.exe_timestamp,
             RFGPHYS_VERSION);
    if (g_header.start_type == rec::kStartWreckingCrew) {
        std::string hex;
        char b[3];
        for (uint32_t i = 0; i < kWcOptionsSize; ++i) {
            snprintf(b, sizeof(b), "%02X", g_header.wc_options[i]);
            hex += b;
        }
        m.printf("  \"start\": {\"type\": \"wrecking_crew\", \"map_file\": \"%s\", \"setup\": \"%s\", "
                 "\"options_hex\": \"%s\", \"character_index\": [%d, %d, %d, %d]},\n",
                 json_escape(g_header.wc_map_file).c_str(), json_escape(describe_wc(g_header.wc_options)).c_str(),
                 hex.c_str(), g_header.wc_character_index[0], g_header.wc_character_index[1],
                 g_header.wc_character_index[2], g_header.wc_character_index[3]);
    } else {
        m.printf("  \"start\": {\"type\": \"save\", \"slot\": %u, \"file\": \"%s\", \"reset_destruction\": %s},\n",
                 g_header.save_slot, json_escape(g_header.save_file).c_str(),
                 g_header.save_reset_destruction ? "true" : "false");
    }
    m.printf("  \"seed\": %u,\n  \"virtual_clock\": %s,\n  \"checkpoint_interval\": %u,\n", g_header.seed,
             (g_header.flags & rec::kFlagVirtualClock) ? "true" : "false", g_header.checkpoint_interval);
    m.printf("  \"frames\": %u,\n  \"game_seconds\": %.3f,\n  \"wall_seconds\": %.3f,\n", g_frame, g_game_seconds, wall);
    m.printf("  \"synthetic_input\": %s,\n", g_has_script ? "true" : "false");
    std::string markers;
    for (uint32_t f : g_markers) markers += (markers.empty() ? "" : ", ") + std::to_string(f);
    m.printf("  \"markers\": [%s],\n", markers.c_str());
    m.printf("  \"crt_rand_calls_other_threads\": %u,\n  \"rng_draws_other_threads\": \"%s\",\n", g_rand_other.load(),
             json_escape(g_rng_other_summary).c_str());
    m.printf("  \"session_run_folder\": \"%s\",\n", json_escape(narrow(session::run_dir())).c_str());
    m.printf("  \"end_reason\": \"%s\"\n}\n", json_escape(end_reason).c_str());
    m.close();
}

void finish_recording(const char* reason, bool at_exit = false) {
    if (g_mode != Mode::kRecord || g_phase == Phase::kIdle) return;
    rng_guard::set_isolation(false);
    g_rng_other_summary = rng_guard::other_threads_summary();
    if (at_exit)
        g_writer.flush_at_exit(g_frame);
    else
        g_writer.finish(g_frame);
    g_checkpoints.close();
    write_manifest(reason);
    if (g_cfg.harness_trace_rng && !at_exit) rng_guard::write_trace(g_bundle_dir + L"\\rng_trace.csv");
    track::set_frame(-1);
    track::end_writing();
    LOG("harness: recording finished (%s): %u frames, %.1f s of game time, CRT rand calls on other threads %u "
        "(callers %s), game generator draws on other threads: %s; %ls",
        reason, g_frame, g_game_seconds, g_rand_other.load(), rand_other_callers().c_str(),
        g_rng_other_summary.c_str(), g_bundle_dir.c_str());
    g_phase = Phase::kIdle;
    g_clock_on = false;
    g_inject = false;
    if (!at_exit) MessageBeep(MB_OK);
    // Unattended self-tests (scripted input or AutoStart) end the game with their recording.
    if ((g_has_script || !g_cfg.harness_auto_start.empty()) && !g_flushing) session::exit_now("unattended recording finished");
}

// ---------------------------------------------------------------------------------------------------------------
// Replay

void write_replay_result(const char* reason, bool completed) {
    rec::TextFile r;
    if (!r.open(session::run_dir() + L"\\replay.json")) return;
    const double wall = g_active_qpc ? session::ticks_to_ms(session::qpc() - g_active_qpc) / 1000.0 : 0.0;
    r.printf("{\n  \"recording\": \"%s\",\n", json_escape(narrow(g_cfg.harness_recording)).c_str());
    r.printf("  \"completed\": %s,\n  \"reason\": \"%s\",\n", completed ? "true" : "false", json_escape(reason).c_str());
    r.printf("  \"frames_recorded\": %u,\n  \"frames_replayed\": %u,\n", g_header.frame_count, g_frame);
    r.printf("  \"game_seconds\": %.3f,\n  \"wall_seconds\": %.3f,\n", g_game_seconds, wall);
    r.printf("  \"first_divergence_frame\": %lld,\n  \"first_divergence_fields\": \"%s\",\n", g_first_divergence,
             json_escape(g_first_divergence_fields).c_str());
    r.printf("  \"first_rng_divergence_frame\": %lld,\n", g_first_rng_divergence);
    r.printf("  \"state_mismatches\": %u,\n  \"checkpoints_compared\": %u,\n  \"checkpoints_mismatched\": %u,\n",
             g_state_mismatches, g_checkpoints_compared, g_checkpoints_mismatched);
    r.printf("  \"checkpoints_rng_differs\": %u,\n", g_checkpoints_rng_differs);
    r.printf("  %s,\n", track::stats_json().c_str());
    r.printf("  \"virtual_clock\": %s,\n",
             g_cfg.harness_virtual_clock && (g_header.flags & rec::kFlagVirtualClock) ? "true" : "false");
    r.printf("  \"crt_rand_calls_other_threads\": %u,\n  \"rng_draws_other_threads\": \"%s\"\n}\n", g_rand_other.load(),
             json_escape(g_rng_other_summary).c_str());
    r.close();
}

[[noreturn]] void finish_replay(const char* reason, bool completed) {
    g_phase = Phase::kIdle;  // the exit flush must not overwrite the result
    g_inject = false;
    g_clock_on = false;
    track::set_frame(-1);
    track::end_writing();
    g_checkpoints.close();
    g_fingerprints.close();
    rng_guard::set_isolation(false);
    g_rng_other_summary = rng_guard::other_threads_summary();
    if (g_cfg.harness_trace_rng) rng_guard::write_trace(session::run_dir() + L"\\rng_trace.csv");
    write_replay_result(reason, completed);
    LOG("harness: replay %s (%s): %u/%u frames, first gameplay divergence %lld (%s), first random-draw divergence "
        "%lld, checkpoints %u compared, %u mismatched (gameplay), %u with different random streams, state mismatches "
        "%u, CRT rand calls on other threads %u (callers %s), game generator draws on other threads: %s",
        completed ? "complete" : "FAILED", reason, g_frame, g_header.frame_count, g_first_divergence,
        g_first_divergence_fields.empty() ? "-" : g_first_divergence_fields.c_str(), g_first_rng_divergence,
        g_checkpoints_compared, g_checkpoints_mismatched, g_checkpoints_rng_differs, g_state_mismatches,
        g_rand_other.load(), rand_other_callers().c_str(), g_rng_other_summary.c_str());
    session::exit_now(completed ? "replay finished" : "replay failed");
}

// Harness.LeaveLevelAtEnd=1: after the last frame the replay leaves the level the way the recording did
// (GS_WC_SHUTDOWN) instead of ending the process at once, so the game's level end runs (and whatever the plugin does
// there); the process ends 3 s after the level is gone, or after 40 s whatever happens.
constexpr int kStateWcShutdown = 60;
bool g_leaving = false;
double g_leave_t0 = 0, g_left_at = -1;

void begin_leaving() {
    g_phase = Phase::kIdle;
    g_inject = false;
    g_clock_on = false;
    track::set_frame(-1);
    track::end_writing();
    g_checkpoints.close();
    g_fingerprints.close();
    rng_guard::set_isolation(false);
    write_replay_result("end of recording", true);
    LOG("harness: replay complete (end of recording): %u/%u frames; leaving the level (Harness.LeaveLevelAtEnd)", g_frame,
        g_header.frame_count);
    g_leaving = true;
    g_leave_t0 = session::since_start_ms();
    rfg::fn<void(__cdecl*)(int, bool)>(rfg::rva::gameseq_set_state)(kStateWcShutdown, false);
}

void leave_tick(int state) {
    const double now = session::since_start_ms();
    if (g_left_at < 0 && !rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)()) {
        g_left_at = now;
        LOG("harness: the level is gone (game state %d) %.1f s after leaving; ending in 3 s", state,
            (now - g_leave_t0) / 1000.0);
    }
    if (g_left_at >= 0 && now - g_left_at > 3000) session::exit_now("replay finished; the level was left");
    if (now - g_leave_t0 > 40000) session::exit_now("replay finished; leaving the level timed out");
}

bool load_recording() {
    const std::wstring dir = g_cfg.harness_recording;
    std::string error;
    if (dir.empty()) {
        g_load_error = "Harness.Recording is not set";
        return false;
    }
    if (!g_reader.open(dir + L"\\input.rfgrec", error)) {
        g_load_error = error;
        return false;
    }
    g_header = g_reader.header();
    if (g_header.exe_timestamp != rfg::kExeTimeDateStamp) {
        g_load_error = "recorded with a different rfg.exe build";
        return false;
    }
    if (!g_header.frame_count) {
        g_load_error = "the recording has no frames (not finished?)";
        return false;
    }
    if (!g_reader.next(g_transition) || !(g_transition.flags & rec::kFrameTransition)) {
        g_load_error = "the recording does not start with a load transition frame";
        return false;
    }
    std::string text;
    if (rec::read_text_file(dir + L"\\checkpoints.csv", text)) {
        size_t pos = text.find('\n');
        while (pos != std::string::npos && pos + 1 < text.size()) {
            const size_t end = text.find('\n', pos + 1);
            checkpoint::State s;
            if (checkpoint::from_csv(text.substr(pos + 1, end - pos - 1).c_str(), s)) g_expected[s.frame] = s;
            pos = end;
        }
    }
    g_checkpoints.open(session::run_dir() + L"\\checkpoints.csv");
    g_checkpoints.write(checkpoint::csv_header());
    g_fingerprints.open(session::run_dir() + L"\\fingerprints.csv");
    g_fingerprints.write("frame,state,rng_index_recorded,rng_index,crt_rand_recorded,crt_rand\n");
    if (g_cfg.harness_lock_track && !track::load(dir + L"\\track.bin", g_cfg.harness_track_tolerance_cm / 100.f))
        LOG("harness: the recording has no track.bin: camera, player and shots follow the replay's own simulation");
    if (g_header.start_type == rec::kStartWreckingCrew)
        LOG("harness: replaying %ls: Wrecking Crew %s, %u frames, %zu checkpoints, seed %u", dir.c_str(),
            g_header.wc_map_file, g_header.frame_count, g_expected.size(), g_header.seed);
    else
        LOG("harness: replaying %ls: save slot %u (%s), %u frames, %zu checkpoints, seed %u", dir.c_str(),
            g_header.save_slot, g_header.save_file, g_header.frame_count, g_expected.size(), g_header.seed);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Session start (both modes): the match or save load begins

void enter_pending(int32_t clock_anchor_ms) {
    g_phase = Phase::kPending;
    g_seen_load = false;
    g_pending_frames = 0;
    g_frame = 0;
    g_game_seconds = 0;
    g_clock_on = g_cfg.harness_virtual_clock && (g_header.flags & rec::kFlagVirtualClock);
    g_clock_anchor_ms = clock_anchor_ms;
    g_clock_elapsed_ms = 0;
    g_rand_other = 0;
    g_last_gov = 0;
    if (g_mode == Mode::kReplay) restore_clock(g_header.clock);
    rng_guard::other_threads_summary();  // resets the per-session counts
    rng_guard::set_isolation(true);
    rng_guard::set_trace(g_cfg.harness_trace_rng);
    rng_guard::set_frame(-1);
    seed_rngs(true);
    if (g_has_script) {
        if (g_input_from_active)
            memcpy(g_script_state, g_input_from_transition.input, rec::kInputSize);
        else
            neutral_input(g_script_state);
        memcpy(g_cur.input, g_script_state, rec::kInputSize);
        g_script_next = 0;
        g_looks.clear();
    }
    LOG("harness: start event: random generators seeded (%u), virtual clock %s", g_header.seed,
        g_clock_on ? "on" : "off");
}

void __cdecl d_match_begin() {
    if (g_mode == Mode::kOff || multiplayer() || (g_mode == Mode::kReplay && !g_replay_starting)) {
        o_match_begin();
        return;
    }
    if (g_mode == Mode::kRecord) {
        if (g_phase != Phase::kIdle) finish_recording("a new Wrecking Crew match began");
        if (!start_recording_wc()) {
            o_match_begin();
            return;
        }
        enter_pending(g_header.clock_anchor_ms);
    } else {
        enter_pending(g_header.clock_anchor_ms);
    }
    o_match_begin();
}

void __cdecl d_verify_save(uint8_t* info, bool reset_destruction) {
    if (g_mode == Mode::kOff || !info || (g_mode == Mode::kReplay && !g_replay_starting)) {
        o_verify_save(info, reset_destruction);
        return;
    }
    if (g_mode == Mode::kRecord) {
        if (g_phase != Phase::kIdle) finish_recording("a save was loaded");
        if (!start_recording_save(info, reset_destruction)) {
            o_verify_save(info, reset_destruction);
            return;
        }
    }
    enter_pending(g_header.clock_anchor_ms);
    o_verify_save(info, reset_destruction);
}

// Main menu -> Wrecking Crew (restore the recorded setup) or Load Game (select the recorded slot). Also used by
// record mode's AutoStart=wc self-test, which keeps the Wrecking Crew menu's current setup.
void drive_start(int state) {
    if (state != g_last_state) {
        g_last_state = state;
        g_state_since = session::qpc();
    }
    const double in_state_ms = session::ticks_to_ms(session::qpc() - g_state_since);
    const bool wc = g_mode == Mode::kRecord || g_header.start_type == rec::kStartWreckingCrew;

    if (state == GS_MAINMENU && !g_menu_pressed) {
        if (!g_caught_input && in_state_ms > 2000 && !rfg::global<uint8_t>(rfg::rva::Main_menu_auto_catch_done)) {
            g_caught_input = true;
            automation::catch_keyboard_and_mouse();
        }
        if (automation::main_menu_ready()) {
            if (!g_ready_since) g_ready_since = session::qpc();
        } else {
            g_ready_since = 0;
        }
        if (g_ready_since && session::ticks_to_ms(session::qpc() - g_ready_since) > 1500) {
            g_menu_pressed = true;
            LOG("harness: main menu ready -> %s", wc ? "Wrecking Crew" : "Load Game");
            rfg::fn<void(__cdecl*)(int)>(rfg::rva::main_menu_handle_selection)(
                wc ? automation::kMainMenuWreckingCrew : automation::kMainMenuLoadGame);
        } else if (in_state_ms > 60000) {
            if (g_mode == Mode::kReplay) finish_replay("the main menu never became ready", false);
            session::exit_now("harness: the main menu never became ready");
        }
        return;
    }
    if (g_start_called) return;

    if (wc && state == GS_WRECKING_CREW_MAIN_MENU) {
        const int menu_state = rfg::global<int32_t>(rfg::rva::Menu_wrecking_crew_info);
        if (menu_state != kWcMenuLoaded) {
            g_wc_menu_since = 0;
        } else if (!g_wc_menu_since) {
            g_wc_menu_since = session::qpc();
        } else if (session::ticks_to_ms(session::qpc() - g_wc_menu_since) > 1000) {
            g_start_called = true;
            auto* options = static_cast<uint8_t*>(rfg::addr(rfg::rva::Wrecking_crew_options));
            // Replays restore the recorded setup; AutoStartFrom gives a scripted recording another recording's setup.
            const rec::Header* setup = g_mode == Mode::kReplay ? &g_header : g_has_setup_from ? &g_setup_from : nullptr;
            if (setup) {
                memcpy(options, setup->wc_options, kWcOptionsSize);
                auto* players = static_cast<uint8_t*>(rfg::addr(rfg::rva::Wc_player_infos));
                for (int i = 0; i < 4; ++i)
                    memcpy(players + i * kWcPlayerInfoSize + kWcPlayerCharacterIndex, &setup->wc_character_index[i], 4);
            }
            LOG("harness: starting the Wrecking Crew match: %s", describe_wc(options).c_str());
            g_replay_starting = true;
            rfg::fn<void(__cdecl*)()>(rfg::rva::wrecking_crew_match_begin)();
            g_replay_starting = false;
        }
        if (!g_start_called && in_state_ms > 60000) {
            if (g_mode == Mode::kReplay) finish_replay("the Wrecking Crew menu never loaded", false);
            session::exit_now("harness: the Wrecking Crew menu never loaded");
        }
        return;
    }

    if (!wc && state == GS_SAVE_LOAD_SCREEN && in_state_ms > 1500) {
        const int n = rfg::global<int32_t>(rfg::rva::Num_saved_games);
        auto* list = rfg::global<uint8_t*>(rfg::rva::Game_save_list);
        for (int i = 0; list && i < n; ++i) {
            uint32_t slot = 0;
            memcpy(&slot, list + i * kSaveInfoSize + kSaveInfoSlot, 4);
            if (slot != g_header.save_slot) continue;
            g_start_called = true;
            LOG("harness: load screen lists %d saves; loading entry %d (slot %u)", n, i, slot);
            rfg::global<int32_t>(rfg::rva::Display_start_index_uisave) = i;
            rfg::global<int32_t>(rfg::rva::Selected_row_uisave) = 0;
            g_replay_starting = true;
            rfg::fn<void(__cdecl*)(bool)>(rfg::rva::hud_ui_save_load_selection_made)(g_header.save_reset_destruction != 0);
            g_replay_starting = false;
            return;
        }
        if (in_state_ms > 20000) finish_replay("the recorded save slot is not on the load screen", false);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Per-frame flow

void poll_hotkeys() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool focused = pid == GetCurrentProcessId();
    const bool f10 = focused && (GetAsyncKeyState(VK_F10) & 0x8000);
    const bool f11 = focused && (GetAsyncKeyState(VK_F11) & 0x8000);
    if (f10 && !g_f10_down && g_phase == Phase::kActive) {
        g_stop_requested = true;
        LOG("harness: stop hotkey");
    }
    if (f11 && !g_f11_down && g_phase == Phase::kActive) {
        g_cur.flags |= rec::kFrameMarker;
        LOG("harness: marker at frame %u", g_frame);
    }
    g_f10_down = f10;
    g_f11_down = f11;
}

void frame_begin() {
    if (g_phase == Phase::kIdle) {
        g_inject = false;
        track::set_frame(-1);
        set_app_active();
        return;
    }
    g_gov_index = 0;
    if (g_mode == Mode::kReplay) {
        if (g_phase == Phase::kPending) {
            g_cur = g_transition;
        } else if (!g_reader.next(g_cur)) {
            finish_replay("the recording ended early (truncated input.rfgrec)", false);
        }
        g_inject = true;
    } else {
        g_cur.flags = (g_synthetic || rfg::global<uint8_t>(rfg::rva::Os_app_active)) ? rec::kFrameAppActive : 0;
        g_cur.n_gov = 0;
        g_cur.end_state = 0;
        if (!g_synthetic) poll_hotkeys();
        g_inject = g_has_script;
        if (g_has_script && g_phase == Phase::kActive) {
            if (g_input_from_active) {
                rec::Frame source;
                if (g_input_from.next(source))
                    memcpy(g_cur.input, source.input, rec::kInputSize);
                else
                    g_stop_requested = true;  // the source recording ended
            } else if (!build_scripted_input()) {
                g_stop_requested = true;
            }
        }
        if (g_has_script && g_phase == Phase::kPending) memcpy(g_cur.input, g_script_state, rec::kInputSize);
    }
    if (g_phase == Phase::kPending) seed_rngs(false);
    rng_guard::set_frame(g_phase == Phase::kActive ? int32_t(g_frame) : -1);
    track::set_frame(g_phase == Phase::kActive ? int32_t(g_frame) : -1);
    set_app_active();
}

float frametime_rate() {
    if (g_phase == Phase::kIdle || g_mode != Mode::kReplay) return 0.f;
    // A recorded frame can be shorter than the frame-cap minimum (another build, or a cap patch); with a fixed
    // rate the limiter would wait forever, so it is off during the call.
    float& min = rfg::global<float>(rfg::rva::Frametime_min);
    g_saved_frametime_min = min;
    min = 0.f;
    return rate_for(g_cur.dt);
}

void after_frametime() {
    if (g_phase == Phase::kIdle) return;
    if (g_phase == Phase::kPending) restore_clock(g_header.clock);  // no game time passes while loading
    if (g_mode == Mode::kReplay) {
        rfg::global<float>(rfg::rva::Frametime_min) = g_saved_frametime_min;
        rfg::global<float>(rfg::rva::Frametime_real) = g_cur.dt_real;
        static bool warned = false;
        if (!warned && rfg::global<float>(rfg::rva::Frametime_uncompressed) != g_cur.dt) {
            warned = true;
            LOG("harness: frame %u: Frametime_uncompressed %.9g differs from the recorded %.9g", g_frame,
                rfg::global<float>(rfg::rva::Frametime_uncompressed), g_cur.dt);
        }
    } else {
        g_cur.dt = rfg::global<float>(rfg::rva::Frametime_uncompressed);
        g_cur.dt_real = rfg::global<float>(rfg::rva::Frametime_real);
    }
    if (g_phase == Phase::kActive) {
        g_clock_elapsed_ms += double(g_cur.dt_real) * 1000.0;
        g_game_seconds += g_cur.dt;
    }
    g_last_dt = g_cur.dt;
}

void request_screenshot(uint32_t frame) {
    if (g_shot_frames.count(frame) || (g_cfg.harness_screenshot_every > 0 && frame % g_cfg.harness_screenshot_every == 0)) {
        wchar_t name[32];
        swprintf_s(name, L"\\shot_%06u.png", frame);
        screenshot::request(session::run_dir() + name);
    }
    if (g_dump_frames.count(frame)) {
        wchar_t name[32];
        swprintf_s(name, L"\\objects_%06u.csv", frame);
        checkpoint::dump_objects(session::run_dir() + name);
        swprintf_s(name, L"\\bodies_%06u.csv", frame);
        checkpoint::dump_bodies(session::run_dir() + name);
    }
}

// Randomness fingerprint after a frame: hashes of every sim-thread stream position (game generator, CRT).
void fingerprint(rec::Frame& f) {
    f.rng_index = uint16_t(rng_guard::vlib_hash());
    f.crt_rand = rng_guard::crt_hash();
}

void record_frame_end(int state) {
    g_cur.end_state = uint8_t(state);
    fingerprint(g_cur);
    g_writer.append(g_cur);
    request_screenshot(g_frame);
    if (g_cur.flags & rec::kFrameMarker) g_markers.push_back(g_frame);
    if (g_frame % g_header.checkpoint_interval == 0)
        g_checkpoints.write(checkpoint::to_csv(checkpoint::capture(g_frame, state)));
    ++g_frame;
    if (is_end_state(state)) {
        char reason[64];
        snprintf(reason, sizeof(reason), "game state %d (%s)", state, rfg::game_state_name(state));
        finish_recording(reason);
    } else if (g_stop_requested) {
        finish_recording(g_has_script ? "input script stop" : "stop hotkey");
    } else if (g_cfg.harness_max_frames > 0 && g_frame >= uint32_t(g_cfg.harness_max_frames)) {
        finish_recording("MaxFrames reached");
    }
}

void replay_frame_end(int state) {
    rec::Frame now;
    fingerprint(now);
    g_fingerprints.printf("%u,%d,%04X,%04X,%08X,%08X\n", g_frame, state, g_cur.rng_index, now.rng_index,
                          g_cur.crt_rand, now.crt_rand);
    if ((now.rng_index != g_cur.rng_index || now.crt_rand != g_cur.crt_rand) && g_first_rng_divergence < 0) {
        g_first_rng_divergence = g_frame;
        LOG("harness: frame %u drew different random numbers: game generator streams %04X (recorded %04X), CRT "
            "streams %08X (recorded %08X)", g_frame, now.rng_index, g_cur.rng_index, now.crt_rand, g_cur.crt_rand);
    }
    request_screenshot(g_frame);
    if (uint8_t(state) != g_cur.end_state) {
        if (g_state_mismatches++ == 0)
            LOG("harness: frame %u ended in state %d, recording says %d", g_frame, state, g_cur.end_state);
        if (g_first_divergence < 0) {
            g_first_divergence = g_frame;
            g_first_divergence_fields = "game_state";
        }
    }
    if (g_frame % g_header.checkpoint_interval == 0) {
        const checkpoint::State cp = checkpoint::capture(g_frame, state);
        g_checkpoints.write(checkpoint::to_csv(cp));
        if (auto it = g_expected.find(g_frame); it != g_expected.end()) {
            ++g_checkpoints_compared;
            if (it->second.rng != cp.rng) ++g_checkpoints_rng_differs;
            const std::string d = checkpoint::diff(it->second, cp, false);
            if (!d.empty()) {
                ++g_checkpoints_mismatched;
                if (g_first_divergence < 0 || g_first_divergence_fields == "game_state") {
                    if (g_first_divergence < 0) g_first_divergence = g_frame;
                    g_first_divergence_fields = d;
                    LOG("harness: first checkpoint mismatch at frame %u: %s (player %.3f %.3f %.3f, recorded %.3f %.3f "
                        "%.3f; objects %d/%d, active bodies %d/%d)",
                        g_frame, d.c_str(), cp.px, cp.py, cp.pz, it->second.px, it->second.py, it->second.pz,
                        cp.n_objects, it->second.n_objects, cp.n_active, it->second.n_active);
                }
            }
        }
    }
    ++g_frame;
    if (g_frame % 2000 == 0)
        LOG("harness: replay frame %u/%u, %.1f s game time, first divergence %lld", g_frame, g_header.frame_count,
            g_game_seconds, g_first_divergence);
    if (g_frame >= g_header.frame_count) {
        if (g_cfg.harness_leave_level_at_end) begin_leaving();
        else finish_replay("end of recording", true);
    }
}

// Diagnostic (TraceRng): where the player is after loading frames and the first gameplay frames.
void log_player(const char* what, uint32_t n, int state) {
    if (!g_cfg.harness_trace_rng) return;
    if (auto* player = rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)()) {
        const float* pos = reinterpret_cast<const float*>(player + 4);
        LOG("harness: %s %u (state %d, dt %.6f): player %.5f %.5f %.5f", what, n, state, g_cur.dt, pos[0], pos[1],
            pos[2]);
    }
}

void frame_end(int state) {
    if (g_leaving) {
        leave_tick(state);
        return;
    }
    if (g_phase == Phase::kPending) log_player("loading frame", g_pending_frames, state);
    if (g_phase == Phase::kActive && g_frame < 3) log_player("frame", g_frame, state);
    if (g_phase == Phase::kIdle) {
        if (g_mode == Mode::kReplay || (g_mode == Mode::kRecord && !g_cfg.harness_auto_start.empty()))
            drive_start(state);
        return;
    }
    if (g_phase == Phase::kPending) {
        ++g_pending_frames;
        if (state == GS_LOAD) g_seen_load = true;
        if (g_seen_load && state == GS_GAMEPLAY) {
            // This frame's gameseq_process switched to gameplay and already simulated: it is the transition frame.
            if (g_mode == Mode::kRecord) {
                g_cur.flags |= rec::kFrameTransition;
                g_cur.end_state = uint8_t(state);
                fingerprint(g_cur);
                g_writer.append(g_cur);
                MessageBeep(MB_ICONASTERISK);
            } else {
                rec::Frame now;
                fingerprint(now);
                if (now.rng_index != g_cur.rng_index || now.crt_rand != g_cur.crt_rand)
                    LOG("harness: the load transition frame drew different random numbers: game generator streams "
                        "%04X (recorded %04X), CRT streams %08X (recorded %08X)", now.rng_index, g_cur.rng_index,
                        now.crt_rand, g_cur.crt_rand);
            }
            g_phase = Phase::kActive;
            g_frame = 0;
            g_active_qpc = session::qpc();
            if (g_cfg.harness_track)
                track::begin_writing((g_mode == Mode::kRecord ? g_bundle_dir : session::run_dir()) + L"\\track.bin");
            LOG("harness: gameplay reached after %u loading frames: %s from the next frame", g_pending_frames,
                g_mode == Mode::kRecord ? "recording" : "replaying");
        } else if (g_pending_frames > 30000) {
            if (g_mode == Mode::kReplay) finish_replay("gameplay never started after the start event", false);
            finish_recording("gameplay never started after the start event");
        }
        return;
    }
    if (g_mode == Mode::kRecord)
        record_frame_end(state);
    else
        replay_frame_end(state);
}

void flush(bool at_exit) {
    g_flushing = true;
    if (g_mode == Mode::kRecord && g_phase != Phase::kIdle) finish_recording("game exited", at_exit);
    if (g_mode == Mode::kReplay && g_phase != Phase::kIdle) {
        g_checkpoints.close();
        write_replay_result("game exited before the end of the recording", false);
    }
    track::end_writing();
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.harness_mode == 0) return true;
    g_cfg = cfg;
    g_mode = cfg.harness_mode == 1 ? Mode::kRecord : Mode::kReplay;
    if (g_mode == Mode::kRecord && !cfg.harness_input_script.empty()) g_has_script = load_script(cfg.harness_input_script);
    if (g_mode == Mode::kRecord && !cfg.harness_input_from.empty()) {
        std::string error;
        if (g_input_from.open(cfg.harness_input_from + L"\\input.rfgrec", error) && g_input_from.next(g_input_from_transition)) {
            g_input_from_active = true;
            g_has_script = true;
            LOG("harness: input comes from the recording %ls (%u frames)", cfg.harness_input_from.c_str(),
                g_input_from.header().frame_count);
        } else {
            LOG("harness: InputFromRecording %ls unusable (%s)", cfg.harness_input_from.c_str(), error.c_str());
        }
    }
    if (g_mode == Mode::kRecord && !cfg.harness_auto_start_from.empty()) {
        rec::Reader setup;
        std::string error;
        if (setup.open(cfg.harness_auto_start_from + L"\\input.rfgrec", error) &&
            setup.header().start_type == rec::kStartWreckingCrew) {
            g_setup_from = setup.header();
            g_has_setup_from = true;
            LOG("harness: auto start uses the Wrecking Crew setup of %ls (%s)", cfg.harness_auto_start_from.c_str(),
                g_setup_from.wc_map_file);
        } else {
            LOG("harness: AutoStartFrom %ls is not a Wrecking Crew recording (%s)", cfg.harness_auto_start_from.c_str(),
                error.c_str());
        }
    }
    g_synthetic = g_mode == Mode::kReplay || g_has_script;
    if (g_mode == Mode::kReplay && !load_recording()) {
        LOG("harness: cannot replay: %s", g_load_error.c_str());
        rec::TextFile r;
        if (r.open(session::run_dir() + L"\\replay.json")) {
            r.printf("{\n  \"recording\": \"%s\",\n  \"completed\": false,\n  \"reason\": \"%s\"\n}\n",
                     json_escape(narrow(cfg.harness_recording)).c_str(), json_escape(g_load_error).c_str());
            r.close();
        }
        session::exit_now("replay: recording not usable");
    }

    namespace r = rfg::rva;
    bool ok = true;
    ok &= hooks::create(r::input_poll, &d_input_poll, &o_input_poll, "input_poll");
    ok &= hooks::create(r::keen__rfg__Application__processInputEvent, &d_input_event, &o_input_event,
                        "Application::processInputEvent");
    ok &= hooks::create(r::os_foreground, &d_os_foreground, &o_os_foreground, "os_foreground");
    ok &= hooks::create(r::gamestate_gp_last_havok_sim_time, &d_gov, &o_gov, "gamestate_gp_last_havok_sim_time");
    ok &= hooks::create(r::timer_get, &d_timer_get, &o_timer_get, "timer_get");
    ok &= hooks::create(r::keen__SystemTimer__getElapsedTime, &d_elapsed, &o_elapsed, "SystemTimer::getElapsedTime");
    ok &= rng_guard::install();
    if (cfg.harness_track) ok &= track::install();
    ok &= hooks::create(r::create_cells, &d_create_cells, &o_create_cells, "create_cells");
    g_shot_frames = parse_frames(cfg.harness_screenshot_frames);
    g_dump_frames = parse_frames(cfg.harness_dump_objects_frames);
    if ((!g_shot_frames.empty() || cfg.harness_screenshot_every > 0) && !screenshot::install())
        LOG("harness: screenshots unavailable");
    ok &= hooks::create(r::wrecking_crew_match_begin, &d_match_begin, &o_match_begin, "wrecking_crew_match_begin");
    ok &= hooks::create(r::verify_gamesave_start, &d_verify_save, &o_verify_save, "verify_gamesave_start");
    o_srand = reinterpret_cast<void(__cdecl*)(unsigned)>(
        hooks::patch_import("MSVCR100.dll", "srand", reinterpret_cast<void*>(&d_srand)));
    o_rand = reinterpret_cast<int(__cdecl*)()>(hooks::patch_import("MSVCR100.dll", "rand", reinterpret_cast<void*>(&d_rand)));
    ok &= o_srand && o_rand;
    gameloop::add_begin(&frame_begin);
    gameloop::add_end(&frame_end);
    gameloop::set_frametime_control(&frametime_rate, &after_frametime);
    session::add_flush_handler(&flush);
    LOG("harness: %s mode%s%s", g_mode == Mode::kRecord ? "record" : "replay", g_has_script ? ", scripted input" : "",
        ok ? "" : ", SOME HOOKS FAILED");
    return ok;
}

}  // namespace harness
