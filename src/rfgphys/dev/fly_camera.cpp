#include "dev/fly_camera.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "harness/screenshot.h"
#include "hooks/hooks.h"

namespace fly_camera {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// rfg_camera: real_fov; render_pos and render_orient, the view drawn (camera_do_frame_post copies real_pos and
// real_orient there each frame, game_render_setup_frame hands them and real_fov to the render camera); and whether
// the culling frustum of the game's own visibility tests (made from those three when first needed) is up to date.
constexpr uint32_t kCamRealFov = 0xBC, kCamRenderPos = 0x10C, kCamRenderOrient = 0x118, kCamCullingValid = 0x808;
// cached_input_state (0x444 bytes): controller buttons and sticks, mouse movement x/y, wheel, buttons[5], keys[256].
constexpr uint32_t kInputSize = 0x444;
constexpr uint32_t kInControllerButtons = 0x8, kInControllerAnalogEnd = 0x324;
constexpr uint32_t kInMouseMoveX = 0x330, kInMouseMoveY = 0x334, kInMouseWheel = 0x338, kInMouseButtons = 0x33C,
                   kInKeys = 0x344;
constexpr uint32_t kInputPollReturn = 0x13A;  // controls_poll+0x135 calls input_poll

constexpr float kDegPerMouseUnit = 0.12f;  // LookSpeed=100: about the game camera's own (750 units turn it 90 degrees)
constexpr float kKeyTurnDegPerS = 60.f, kRollDegPerS = 45.f;
constexpr float kFastFactor = 4.f, kSlowFactor = 0.25f;
constexpr float kWheelNotch = 120.f, kFovPerNotch = 0.9f, kMinFov = 5.f, kMaxFov = 120.f;  // degrees
constexpr float kMaxStepS = 0.1f;

struct Vec {
    float x, y, z;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
bool normalize(Vec& v) {
    const float len = std::sqrt(dot(v, v));
    if (!(len > 1e-6f)) return false;
    v = v * (1.f / len);
    return true;
}

// The flying view: position, orientation (the game's matrix rows: right, up, forward) and field of view in degrees.
struct View {
    Vec pos, rvec, uvec, fvec;
    float fov;
};

// The keys that fly it: -1 to 1 along each axis.
struct Controls {
    float forward, right, up, roll, yaw, pitch;  // yaw and pitch: the arrow keys
    float speed_factor;
    bool reset_zoom;
};

int g_key = VK_F10;
float g_speed = 10.f, g_deg_per_unit = kDegPerMouseUnit;
bool g_hide_hud = true;

// Sim thread only: input_poll (from controls_poll), game_render_setup_frame (in the sim/render handshake) and the end
// of the frame.
bool g_flying = false;
bool g_hud_hidden = false;
bool g_key_was_down = false;
bool g_block_until_release = false;  // after flying: the game gets no input until the key is let go
View g_view{};
float g_game_fov = 0;                           // when flying started
float g_mouse_x = 0, g_mouse_y = 0, g_wheel = 0;  // since the view last moved
int64_t g_last_qpc = 0;

// Test.FlyCamera: the flight is scripted, the keyboard and mouse are not read.
bool g_test = false;
Controls g_test_controls{0, 0, 0, 0, 0, 0, 1.f, false};

void(__cdecl* o_input_poll)();
void(__cdecl* o_render_setup)(void*);

uint8_t* camera() { return static_cast<uint8_t*>(rfg::addr(r::Rfg_camera)); }
template <typename T>
T& field(uint8_t* p, uint32_t offset) {
    return *reinterpret_cast<T*>(p + offset);
}

bool focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Perpendicular unit rows again (Gram-Schmidt from the forward vector, keeping the matrix's handedness).
bool orthonormalize(View& v) {
    if (!normalize(v.fvec)) return false;
    v.rvec = v.rvec - v.fvec * dot(v.rvec, v.fvec);
    if (!normalize(v.rvec)) return false;
    v.uvec = v.uvec - v.fvec * dot(v.uvec, v.fvec) - v.rvec * dot(v.uvec, v.rvec);
    return normalize(v.uvec);
}

// Turns `a` toward `b` by `deg` degrees, in their plane.
void turn(Vec& a, Vec& b, float deg) {
    if (deg == 0) return;
    const float rad = deg * 0.017453292f, c = std::cos(rad), s = std::sin(rad);
    const Vec a2 = a * c + b * s;
    b = b * c - a * s;
    a = a2;
}

void __cdecl d_input_poll() {
    if ((!g_flying && !g_block_until_release) || _ReturnAddress() != rfg::addr(r::controls_poll + kInputPollReturn)) {
        o_input_poll();
        return;
    }
    // input_poll makes the "next" state current and turns it into the game's controls: the fly camera takes its mouse
    // movement, and the game gets everything released (keys, buttons, sticks, mouse movement).
    uint8_t* next = static_cast<uint8_t*>(rfg::addr(r::Cached_input_states)) +
                    rfg::global<uint8_t>(r::Next_cached_input_state) * kInputSize;
    if (g_flying && !g_test) {
        g_mouse_x += field<float>(next, kInMouseMoveX);
        g_mouse_y += field<float>(next, kInMouseMoveY);
        g_wheel += field<float>(next, kInMouseWheel);
    }
    memset(next + kInControllerButtons, 0, kInControllerAnalogEnd - kInControllerButtons);
    memset(next + kInMouseMoveX, 0, kInMouseButtons + 5 - kInMouseMoveX);
    memset(next + kInKeys, 0, 256);
    o_input_poll();
}

Controls keyboard() {
    Controls c{0, 0, 0, 0, 0, 0, 1.f, false};
    if (!focused()) return c;
    auto axis = [](int plus, int minus) { return float(down(plus)) - float(down(minus)); };
    c.forward = axis('W', 'S');
    c.right = axis('D', 'A');
    c.up = axis(VK_SPACE, VK_CONTROL);
    c.roll = axis('E', 'Q');
    c.yaw = axis(VK_RIGHT, VK_LEFT);
    c.pitch = axis(VK_UP, VK_DOWN);
    c.speed_factor = down(VK_SHIFT) ? kFastFactor : down(VK_MENU) ? kSlowFactor : 1.f;
    c.reset_zoom = down(VK_MBUTTON);
    return c;
}

// Moves the view by the input since the last frame.
void fly() {
    const int64_t now = session::qpc();
    float dt = float(session::ticks_to_ms(now - g_last_qpc) / 1000.0);
    g_last_qpc = now;
    dt = dt > 0 ? std::min(dt, kMaxStepS) : 0.f;

    const Controls c = g_test ? g_test_controls : keyboard();
    View& v = g_view;
    // Turning, slower as the view zooms in: yaw about the view's up, pitch about its right, roll about its forward
    // vector. Mouse movement right and up (y down is positive) turn right and up.
    const float zoom = v.fov / g_game_fov;
    const float yaw = (g_mouse_x * g_deg_per_unit + c.yaw * kKeyTurnDegPerS * dt) * zoom;
    const float pitch = (-g_mouse_y * g_deg_per_unit + c.pitch * kKeyTurnDegPerS * dt) * zoom;
    g_mouse_x = g_mouse_y = 0;
    turn(v.fvec, v.rvec, yaw);
    turn(v.fvec, v.uvec, pitch);
    turn(v.uvec, v.rvec, c.roll * kRollDegPerS * dt);
    orthonormalize(v);
    // Moving along the same axes.
    const Vec dir = v.fvec * c.forward + v.rvec * c.right + v.uvec * c.up;
    v.pos = v.pos + dir * (g_speed * c.speed_factor * dt);
    // The wheel zooms (a notch is 120 units; forward narrows the view), the middle button gives back the game's view.
    if (g_wheel != 0) {
        v.fov = std::clamp(v.fov * std::pow(kFovPerNotch, g_wheel / kWheelNotch), std::min(kMinFov, g_game_fov),
                           std::max(kMaxFov, g_game_fov));
        g_wheel = 0;
    }
    if (c.reset_zoom) v.fov = g_game_fov;
}

// game_render_setup_frame hands the camera's view to the render camera (and runs the game's render tasks), in the
// sim/render handshake on the sim thread while the render thread waits.
void __cdecl d_render_setup(void* renderer) {
    if (!g_flying) {
        o_render_setup(renderer);
        return;
    }
    fly();
    // The view drawn is the fly camera's: render_pos and render_orient keep it until camera_do_frame_post copies the
    // game camera there in the next frame (as the remaster's unused free camera does it), the game's culling frustum
    // is made anew for it, and real_fov holds its field of view while the render camera is set up.
    uint8_t* cam = camera();
    field<Vec>(cam, kCamRenderPos) = g_view.pos;
    Vec* orient = &field<Vec>(cam, kCamRenderOrient);
    orient[0] = g_view.rvec;
    orient[1] = g_view.uvec;
    orient[2] = g_view.fvec;
    field<uint8_t>(cam, kCamCullingValid) = 0;
    float& fov = field<float>(cam, kCamRealFov);
    const float game_fov = fov;
    fov = g_view.fov;
    o_render_setup(renderer);
    fov = game_fov;
}

void start() {
    uint8_t* cam = camera();
    View v{};
    v.pos = field<Vec>(cam, kCamRenderPos);
    const Vec* orient = &field<Vec>(cam, kCamRenderOrient);
    v.rvec = orient[0];
    v.uvec = orient[1];
    v.fvec = orient[2];
    if (!orthonormalize(v)) {
        v.rvec = {1, 0, 0};
        v.uvec = {0, 1, 0};
        v.fvec = {0, 0, 1};
    }
    g_game_fov = field<float>(cam, kCamRealFov);
    if (!(g_game_fov > 1.f && g_game_fov < 179.f)) g_game_fov = 60.f;
    v.fov = g_game_fov;
    g_view = v;
    g_mouse_x = g_mouse_y = g_wheel = 0;
    g_last_qpc = session::qpc();
    if (g_hide_hud) {
        rfg::fn<void(__cdecl*)(bool)>(r::hud_hide)(true);  // reference-counted, as cutscenes use it
        g_hud_hidden = true;
    }
    g_flying = true;
    LOG("fly camera: on at (%.1f, %.1f, %.1f), field of view %.1f", v.pos.x, v.pos.y, v.pos.z, v.fov);
}

void stop(const char* why) {
    g_flying = false;
    if (g_hud_hidden) {
        rfg::fn<void(__cdecl*)(bool)>(r::hud_hide)(false);
        g_hud_hidden = false;
    }
    const View& v = g_view;
    LOG("fly camera: off (%s) at (%.1f, %.1f, %.1f), field of view %.1f; back to the game camera", why, v.pos.x,
        v.pos.y, v.pos.z, v.fov);
}

// Test.FlyCamera: one scripted flight, 15 s into gameplay, with screenshots in the session folder: the game camera
// (fly_0), the flight's start at the same place (fly_1), after 2 s forward and up while turning right and down (fly_2),
// after 1 s rolling right and zooming in (fly_3), and the game camera again (fly_4). Meanwhile the game is fed mouse
// movement that would spin its camera. The log checks that the drawn view followed the flight while the game camera
// stayed, that afterwards the drawn view is the game camera's again, with the game's field of view, the player
// unmoved, and (the control, fly_5) that the same mouse movement turns the game camera once the flight is over.
constexpr double kTestStartS = 15.0;
constexpr uint32_t kCamRealPos = 0x2C, kCamRealOrient = 0x50;  // rfg_camera
constexpr uint32_t kObjPos = 0x4;                                // object
constexpr float kTestGameMouse = 300.f;  // per frame: about 36 degrees of the game camera's turn
int g_test_step = 0;
int64_t g_test_gameplay_since = 0, g_test_last = 0;
float g_test_mouse_x = 0, g_test_mouse_y = 0, g_test_wheel = 0;  // per second
float g_test_game_fov = 0;
Vec g_test_camera{}, g_test_camera_fvec{}, g_test_player{};
bool g_test_ok = true;

Vec player_pos() {
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    return player ? field<Vec>(player, kObjPos) : Vec{};
}
float dist(Vec a, Vec b) { return std::sqrt(dot(a - b, a - b)); }

void test_shot(const char* name) {
    screenshot::request(session::run_dir() + L"\\" + std::wstring(name, name + strlen(name)) + L".png");
    uint8_t* cam = camera();
    const Vec real = field<Vec>(cam, kCamRealPos), drawn = field<Vec>(cam, kCamRenderPos), player = player_pos();
    const Vec forward = (&field<Vec>(cam, kCamRenderOrient))[2];
    LOG("fly camera test: %s: game camera (%.2f, %.2f, %.2f), field of view %.2f; drawn view (%.2f, %.2f, %.2f), "
        "forward (%.3f, %.3f, %.3f); fly camera field of view %.2f; player (%.2f, %.2f, %.2f)", name, real.x, real.y,
        real.z, field<float>(cam, kCamRealFov), drawn.x, drawn.y, drawn.z, forward.x, forward.y, forward.z, g_view.fov,
        player.x, player.y, player.z);
}

void test_check(bool ok, const char* what) {
    LOG("fly camera test: %s: %s", what, ok ? "yes" : "NO");
    g_test_ok = g_test_ok && ok;
}

void test_frame(bool gameplay) {
    if (!gameplay || g_test_step > 7) return;
    const int64_t now = session::qpc();
    if (!g_test_gameplay_since) g_test_gameplay_since = g_test_last = now;
    const double t = session::ticks_to_ms(now - g_test_gameplay_since) / 1000.0 - kTestStartS;
    const float dt = float(session::ticks_to_ms(now - g_test_last) / 1000.0);
    g_test_last = now;
    if (t < 0) return;
    if (g_flying) {
        g_mouse_x += g_test_mouse_x * dt;
        g_mouse_y += g_test_mouse_y * dt;
        g_wheel += g_test_wheel * dt;
    }
    // Mouse movement for the game: while flying until 0.5 s before the flight ends, and for the control afterwards.
    if ((g_flying && (g_test_step == 3 || g_test_step == 4)) || g_test_step == 7) {
        uint8_t* next = static_cast<uint8_t*>(rfg::addr(r::Cached_input_states)) +
                        rfg::global<uint8_t>(r::Next_cached_input_state) * kInputSize;
        field<float>(next, kInMouseMoveX) += kTestGameMouse;
    }
    uint8_t* cam = camera();
    switch (g_test_step) {
    case 0:
        g_test_game_fov = field<float>(cam, kCamRealFov);
        g_test_camera = field<Vec>(cam, kCamRealPos);
        g_test_camera_fvec = (&field<Vec>(cam, kCamRealOrient))[2];
        g_test_player = player_pos();
        test_shot("fly_0_game");
        LOG("fly camera test: game window active %d, foreground %d", rfg::global<uint8_t>(r::Os_app_active), focused());
        ++g_test_step;
        break;
    case 1:
        if (t < 0.5) break;
        start();
        ++g_test_step;
        break;
    case 2:
        if (t < 1.0) break;
        test_shot("fly_1_start");
        g_test_controls.forward = g_test_controls.up = 1;  // 20 m forward and 20 m up
        g_test_mouse_x = 200;                              // 48 degrees right
        g_test_mouse_y = 100;                              // 24 degrees down
        ++g_test_step;
        break;
    case 3:
        if (t < 3.0) break;
        g_test_controls = Controls{0, 0, 0, 1.f, 0, 0, 1.f, false};  // 45 degrees roll right
        g_test_mouse_x = g_test_mouse_y = 0;
        g_test_wheel = 360;  // 3 notches in: field of view x 0.73
        test_shot("fly_2_moved");
        test_check(dist(field<Vec>(cam, kCamRenderPos), g_view.pos) < 0.01f, "the drawn view is the fly camera's");
        ++g_test_step;
        break;
    case 4:
        if (t < 4.0) break;
        g_test_controls = Controls{0, 0, 0, 0, 0, 0, 1.f, false};
        g_test_wheel = 0;
        test_shot("fly_3_rolled_zoomed");
        test_check(dist(g_view.pos, g_test_camera) > 20.f, "the fly camera went more than 20 m");
        test_check(dist(field<Vec>(cam, kCamRealPos), g_test_camera) < 0.5f &&
                       dot((&field<Vec>(cam, kCamRealOrient))[2], g_test_camera_fvec) > 0.999f,
                   "the game camera stayed and did not turn (its input was held back)");
        ++g_test_step;
        break;
    case 5:
        if (t < 4.5) break;
        stop("test");
        ++g_test_step;
        break;
    case 6: {
        if (t < 5.0) break;
        test_shot("fly_4_back");
        // render_orient is a copy of real_orient from before game_render_setup_frame orthogonalized it: the last bits
        // differ.
        const Vec* drawn = &field<Vec>(cam, kCamRenderOrient);
        const Vec* game = &field<Vec>(cam, kCamRealOrient);
        bool same = dist(field<Vec>(cam, kCamRenderPos), field<Vec>(cam, kCamRealPos)) < 0.001f;
        for (int i = 0; i < 3; ++i) same = same && dot(drawn[i], game[i]) > 0.99999f;
        test_check(same, "the drawn view is the game camera's again");
        test_check(dist(field<Vec>(cam, kCamRealPos), g_test_camera) < 0.5f && dot(game[2], g_test_camera_fvec) > 0.999f,
                   "the game camera is where it was");
        test_check(field<float>(cam, kCamRealFov) == g_test_game_fov, "the game's field of view is unchanged");
        test_check(dist(player_pos(), g_test_player) < 0.5f, "the player stayed");
        ++g_test_step;
        break;
    }
    case 7: {  // the control: the same mouse movement, without the fly camera, turns the game camera
        if (t < 5.5) break;
        test_shot("fly_5_control");
        const float turned = dot((&field<Vec>(cam, kCamRealOrient))[2], g_test_camera_fvec);
        LOG("fly camera test: control: game camera direction against the start's: dot %.3f (game window active %d, "
            "foreground %d)", turned, rfg::global<uint8_t>(r::Os_app_active), focused());
        // A message waiting for a button press (a tutorial box) keeps the game camera still whatever the mouse does.
        const bool control = turned < 0.99f;
        if (control)
            LOG("fly camera test: without the fly camera the same mouse movement turns the game camera: yes");
        else
            LOG("fly camera test: without the fly camera the same mouse movement does not turn the game camera either "
                "(a message on screen?): the input check proves nothing here");
        LOG("fly camera test: %s", !g_test_ok ? "FAILED" : control ? "passed" : "passed, input blocking not verified");
        ++g_test_step;
        break;
    }
    }
}

void on_frame(int game_state) {
    const bool gameplay = game_state == kGameplay && !rfg::global<uint8_t>(r::Multiplayer);
    if (g_flying && !gameplay) stop("left gameplay");
    if (g_test) {
        test_frame(gameplay);
        return;
    }
    const bool key = focused() && down(g_key);
    const bool pressed = key && !g_key_was_down;
    g_key_was_down = key;
    if (!key) g_block_until_release = false;
    if (g_flying && pressed) {
        stop("key");
        g_block_until_release = true;
    } else if (!g_flying && pressed && gameplay) {
        start();
    }
}

}  // namespace

bool install(const Config& cfg) {
    if (!cfg.fly_camera && !cfg.test_fly_camera) return true;
    if (cfg.harness_mode != 0) {
        LOG("fly camera: not with the record/replay harness");
        return true;
    }
    g_key = cfg.fly_camera_key;
    g_speed = float(cfg.fly_camera_speed);
    g_deg_per_unit = kDegPerMouseUnit * float(cfg.fly_camera_look_speed) / 100.f;
    g_hide_hud = cfg.fly_camera_hide_hud;
    g_test = cfg.test_fly_camera;
    bool ok = hooks::create(r::input_poll, &d_input_poll, &o_input_poll, "input_poll");
    ok &= hooks::create(r::game_render_setup_frame, &d_render_setup, &o_render_setup, "game_render_setup_frame");
    if (g_test) ok &= screenshot::install();
    if (!ok) return false;
    session::add_frame_callback(&on_frame);
    if (g_test)
        LOG("fly camera test armed: a scripted flight 15 s into gameplay, screenshots fly_*.png");
    else
        LOG("fly camera: virtual key %d toggles it (single player); %d m/s, look speed %d%%, %s", g_key,
            cfg.fly_camera_speed, cfg.fly_camera_look_speed, g_hide_hud ? "HUD hidden while flying" : "HUD kept");
    return true;
}

}  // namespace fly_camera
