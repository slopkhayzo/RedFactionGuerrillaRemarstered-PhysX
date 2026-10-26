#include "test/automation.h"

#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace automation {
namespace {

constexpr int GS_MAINMENU = 0, GS_GAMEPLAY = 1, GS_PLAYER_DEATH_OPTIONS = 16, GS_INGAME_HANDBOOK_HUD = 43,
              GS_SAVE_LOAD_SCREEN = 50;

Config g_cfg;
int g_last_state = -1000;
int64_t g_state_since = 0;
int64_t g_gameplay_since = 0;
bool g_pressed_load = false, g_selected_save = false, g_refreshed_list = false;

bool g_caught_input = false;

// keen::InputSystem: ControllerInfo[16] at +0x11AC, 0x18 bytes each, device class at +0 (1 keyboard, 2 mouse).
constexpr uint32_t kInputControllerInfos = 0x11AC, kControllerInfoSize = 0x18, kMaxControllers = 16;
constexpr int kClassKeyboard = 1, kClassMouse = 2;
constexpr uint32_t kHumanFlags = 0xB4, kHumanInvulnerable = 0x20;  // human: flags, the invulnerable bit

}  // namespace

// The main menu waits for "any button" to pick player 1's input device (controller auto-catch). Only then does
// main_menu_process add the local player and load profile, options and the save list. Assigning the keyboard and
// mouse to player 0 is what a caught key press does (keen::input::setControllerPlayerId).
void catch_keyboard_and_mouse() {
    auto* system = rfg::global<uint8_t*>(rfg::rva::Keen_input_system);
    if (!system) return;
    auto set_player = rfg::fn<void(__cdecl*)(void*, uint32_t, void*)>(rfg::rva::keen__input__setControllerPlayerId);
    for (uint32_t i = 0; i < kMaxControllers; ++i) {
        const int cls = *reinterpret_cast<int32_t*>(system + kInputControllerInfos + i * kControllerInfoSize);
        if (cls == kClassKeyboard || cls == kClassMouse) {
            set_player(system, i, nullptr /* player 0 */);
            LOG("automation: input device %u (%s) assigned to player 1", i, cls == kClassKeyboard ? "keyboard" : "mouse");
        }
    }
}

// Profile, options and save data are loaded after the input device was caught; the save directory cache is built
// from them (by main_menu_process or lazily by the load screen). Opening the load screen earlier lists nothing.
bool main_menu_ready() {
    return rfg::global<uint8_t>(rfg::rva::Main_menu_auto_catch_done) &&
           !rfg::global<uint8_t>(rfg::rva::Main_menu_loading_options) &&
           !rfg::global<uint8_t>(rfg::rva::Main_menu_waiting_for_options_load);
}

namespace {

// main_menu_process_delayed_selection's states while New Game shows its message boxes, and the answers.
constexpr int GS_VIDEO_CUTSCENE_PLAY = 49;
constexpr int kBoxAutosaveWarning = 4, kBoxDifficulty = 10, kBoxGoldenHammer = 11;
constexpr int kDifficultyNormal = 1;  // the box lists casual, normal, hard, insane
constexpr int kGoldenHammerNo = 1;    // yes, no
int g_answered_box = -1;

// The intro video: video_cutscene_play_process stops it (video_player_stop, the loading screen, streaming unpaused, a
// fade) when button 9 or 11 reads pressed: ui_get_button_info gives Button_info's binfo[b] value unless that entry is
// consumed. The skip presses button 9 for those calls, as a key press would. (Calling video_cutscene_stop_callback
// directly instead left the player running on freed video memory: the render thread crashed in the particle renderer.)
constexpr uint32_t kButtonInfoStride = 0x18, kButtonInfoValue = 0xC, kButtonInfoConsumed = 0x19;
constexpr int kSkipButton = 9;
bool g_skip_video = false, g_skip_asked = false;
void(__cdecl* o_video_process)() = nullptr;

void __cdecl d_video_process() {
    if (!g_skip_video) {
        o_video_process();
        return;
    }
    uint8_t* b = static_cast<uint8_t*>(rfg::addr(rfg::rva::Button_info)) + kSkipButton * kButtonInfoStride;
    float value;
    memcpy(&value, b + kButtonInfoValue, 4);
    const uint8_t consumed = b[kButtonInfoConsumed];
    const float pressed = 1.0f;
    memcpy(b + kButtonInfoValue, &pressed, 4);
    b[kButtonInfoConsumed] = 0;
    o_video_process();
    memcpy(b + kButtonInfoValue, &value, 4);
    b[kButtonInfoConsumed] = consumed;
    if (!rfg::global<uint32_t>(rfg::rva::Video_cutscene_bink_handle)) {
        g_skip_video = false;
        LOG("automation: new game: the intro video skipped");
    }
}

}  // namespace

void drive_new_game(int state, double in_state_ms) {
    namespace r = rfg::rva;
    if (state == GS_MAINMENU) {
        const int handle = rfg::global<int32_t>(r::Message_box_handle_main);
        const int box = rfg::global<int32_t>(r::Main_menu_delayed_selection_state);
        if (rfg::global<int32_t>(r::Main_menu_delayed_selection) != kMainMenuNewGame || handle == -1 ||
            box == g_answered_box)
            return;
        uint32_t answer = 0;  // bool cb(box handle, selected item, msgbox_choice: 0 accepts)
        int item = 0;
        const char* what = nullptr;
        if (box == kBoxAutosaveWarning) answer = r::main_menu_autosave_cb, what = "the autosave warning: go on";
        else if (box == kBoxDifficulty) answer = r::main_menu_difficulty_cb, item = kDifficultyNormal, what = "normal";
        else if (box == kBoxGoldenHammer) answer = r::main_menu_golden_hammer_cb, item = kGoldenHammerNo,
                                          what = "no golden hammer";
        else return;
        g_answered_box = box;
        LOG("automation: new game: %s", what);
        rfg::fn<bool(__cdecl*)(int, int, int)>(answer)(handle, item, 0);
        rfg::fn<void(__cdecl*)(int, bool)>(r::ui_hide_msgbox)(handle, false);
    } else if (state == GS_VIDEO_CUTSCENE_PLAY && !g_skip_asked && in_state_ms > 2500 &&
               rfg::global<uint32_t>(r::Video_cutscene_bink_handle)) {
        g_skip_asked = g_skip_video = true;  // pressed in the video's own processing (d_video_process)
    }
}

void reset_new_game() {
    g_answered_box = -1;
    g_skip_asked = g_skip_video = false;
}

namespace {

// __thiscall SaveDataProviderWin32Steam::writeFileToSteamStorage(const char* name, void* data, uint size).
// Every save write and erase goes through it; returning OperationResult_Ok (0) without writing keeps the game happy.
int(__fastcall* o_write_file)(void*, void*, const char*, void*, uint32_t);
int __fastcall d_write_file(void*, void*, const char* name, void*, uint32_t size) {
    LOG("save write BLOCKED (test mode): %s, %u bytes", name ? name : "?", size);
    return 0;
}

void on_frame(int state) {
    if (state != g_last_state) {
        g_last_state = state;
        g_state_since = session::qpc();
    }
    const double in_state_ms = session::ticks_to_ms(session::qpc() - g_state_since);

    const bool new_game = g_cfg.test_auto_new_game && !g_cfg.auto_load_save;
    if (g_cfg.auto_load_save || new_game) {
        if (state == GS_MAINMENU && !g_caught_input && in_state_ms > 2000 &&
            !rfg::global<uint8_t>(rfg::rva::Main_menu_auto_catch_done)) {
            g_caught_input = true;
            catch_keyboard_and_mouse();
        }
        static int64_t ready_since = 0;
        if (state == GS_MAINMENU && !g_pressed_load && in_state_ms > 2000) {
            if (main_menu_ready()) {
                if (!ready_since) ready_since = session::qpc();
            } else {
                ready_since = 0;
            }
            const bool settled = ready_since && session::ticks_to_ms(session::qpc() - ready_since) > 1500;
            if (settled || in_state_ms > 45000) {
                g_pressed_load = true;
                LOG("automation: main menu %s after %.1f s (cache loaded %d) -> %s", settled ? "ready" : "NOT ready (timeout)",
                    in_state_ms / 1000.0, rfg::global<uint8_t>(rfg::rva::Directory_cache_loaded),
                    new_game ? "New Game" : "Load Game");
                rfg::fn<void(__cdecl*)(int)>(rfg::rva::main_menu_handle_selection)(new_game ? kMainMenuNewGame
                                                                                            : kMainMenuLoadGame);
            }
        }
        if (new_game && g_pressed_load) drive_new_game(state, in_state_ms);
        if (!new_game && state == GS_SAVE_LOAD_SCREEN && !g_selected_save && in_state_ms > 1500) {
            const int saves = rfg::global<int32_t>(rfg::rva::Num_saved_games);
            if (saves > 0) {
                g_selected_save = true;
                LOG("automation: load screen lists %d saves -> loading the selected (most recent) one%s", saves,
                    g_cfg.test_reset_destruction ? ", destruction reset" : "");
                rfg::fn<void(__cdecl*)(bool)>(rfg::rva::hud_ui_save_load_selection_made)(g_cfg.test_reset_destruction);
            } else if (!g_refreshed_list && in_state_ms > 5000) {
                g_refreshed_list = true;
                LOG("automation: load screen still empty -> refreshing the save directory cache");
                rfg::fn<void(__cdecl*)()>(rfg::rva::game_save_clear_load_directory_cache)();
                rfg::global<uint8_t>(rfg::rva::Update_list_uisave) = 1;
            } else if (in_state_ms > 20000) {
                session::exit_now("automation: no saves listed on the load screen");
            }
        }
        // A save can open a handbook (tutorial) pane a little after loading, which waits for a key (the first mission
        // opens several). Closed as its back button does: hud_ui_handbook_process sees ExitHandbook, stops the video
        // and pops the state.
        static bool closing = false;
        if (state != GS_INGAME_HANDBOOK_HUD) {
            closing = false;
        } else if (!closing && in_state_ms > 3000) {
            closing = true;
            rfg::global<uint8_t>(rfg::rva::ExitHandbook) = 1;
            LOG("automation: a handbook pane waits for a key -> closed");
        }
    }
    // Test.GodMode: the flag human::is_invulnerable reads first (player_set_player_invulnerable sets it too, and starts a
    // glow), again every frame: a test's teleports and a replay that drifts from its recording got the player killed.
    if (g_cfg.test_god_mode && state == GS_GAMEPLAY) {
        if (uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)()) {
            uint32_t& flags = *reinterpret_cast<uint32_t*>(player + kHumanFlags);
            if (!(flags & kHumanInvulnerable)) {
                flags |= kHumanInvulnerable;
                static bool logged = false;
                if (!logged) LOG("automation: god mode, the player takes no damage (Test.GodMode)");
                logged = true;
            }
        }
    }
    // The death menu waits for a choice and the gameplay clock stops: a timed run would sit there until its watchdog
    // (a test's teleport put the player under the ground; the user ended the game from the menu).
    if (g_cfg.exit_after_gameplay_s > 0 && state == GS_PLAYER_DEATH_OPTIONS && in_state_ms > 5000)
        session::exit_now("automation: the player died (unattended run)");
    if (g_cfg.exit_after_gameplay_s > 0 && state == GS_GAMEPLAY) {
        if (!g_gameplay_since) {
            g_gameplay_since = session::qpc();
            LOG("automation: gameplay started");
        }
        if (session::ticks_to_ms(session::qpc() - g_gameplay_since) >= g_cfg.exit_after_gameplay_s * 1000.0)
            session::exit_now("ExitAfterGameplaySeconds elapsed");
    }
}

}  // namespace

bool install(const Config& cfg) {
    g_cfg = cfg;
    bool ok = true;
    if (cfg.block_save_writes) {
        ok &= hooks::create(rfg::rva::keen__SaveData__SaveDataProviderWin32Steam__writeFileToSteamStorage,
                            &d_write_file, &o_write_file, "SaveDataProviderWin32Steam::writeFileToSteamStorage");
        LOG("automation: save writes and erases are blocked for this session");
    }
    if (cfg.auto_load_save || cfg.test_auto_new_game || cfg.exit_after_gameplay_s > 0 || cfg.test_god_mode)
        session::add_frame_callback(&on_frame);
    // The intro skip, for an automated new game and for replays (a new game's replay drives the same menus).
    if (cfg.test_auto_new_game || cfg.harness_mode == 2)
        ok &= hooks::create(rfg::rva::video_cutscene_play_process, &d_video_process, &o_video_process,
                            "video_cutscene_play_process");
    return ok;
}

}  // namespace automation
