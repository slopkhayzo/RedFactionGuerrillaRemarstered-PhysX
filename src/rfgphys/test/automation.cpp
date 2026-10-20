#include "test/automation.h"

#include <cstdint>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace automation {
namespace {

constexpr int GS_MAINMENU = 0, GS_GAMEPLAY = 1, GS_INGAME_HANDBOOK_HUD = 43, GS_SAVE_LOAD_SCREEN = 50;

Config g_cfg;
int g_last_state = -1000;
int64_t g_state_since = 0;
int64_t g_gameplay_since = 0;
bool g_pressed_load = false, g_selected_save = false, g_refreshed_list = false;

bool g_caught_input = false;

// keen::InputSystem: ControllerInfo[16] at +0x11AC, 0x18 bytes each, device class at +0 (1 keyboard, 2 mouse).
constexpr uint32_t kInputControllerInfos = 0x11AC, kControllerInfoSize = 0x18, kMaxControllers = 16;
constexpr int kClassKeyboard = 1, kClassMouse = 2;

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

    if (g_cfg.auto_load_save) {
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
                LOG("automation: main menu %s after %.1f s (cache loaded %d) -> Load Game",
                    settled ? "ready" : "NOT ready (timeout)", in_state_ms / 1000.0,
                    rfg::global<uint8_t>(rfg::rva::Directory_cache_loaded));
                rfg::fn<void(__cdecl*)(int)>(rfg::rva::main_menu_handle_selection)(kMainMenuLoadGame);
            }
        }
        if (state == GS_SAVE_LOAD_SCREEN && !g_selected_save && in_state_ms > 1500) {
            const int saves = rfg::global<int32_t>(rfg::rva::Num_saved_games);
            if (saves > 0) {
                g_selected_save = true;
                LOG("automation: load screen lists %d saves -> loading the selected (most recent) one", saves);
                rfg::fn<void(__cdecl*)(bool)>(rfg::rva::hud_ui_save_load_selection_made)(false);
            } else if (!g_refreshed_list && in_state_ms > 5000) {
                g_refreshed_list = true;
                LOG("automation: load screen still empty -> refreshing the save directory cache");
                rfg::fn<void(__cdecl*)()>(rfg::rva::game_save_clear_load_directory_cache)();
                rfg::global<uint8_t>(rfg::rva::Update_list_uisave) = 1;
            } else if (in_state_ms > 20000) {
                session::exit_now("automation: no saves listed on the load screen");
            }
        }
        // A save can open a handbook (tutorial) pane a little after loading, which waits for a key. Closed as its
        // back button does: hud_ui_handbook_process sees ExitHandbook, stops the video and pops the state.
        static bool closing = false;
        if (state != GS_INGAME_HANDBOOK_HUD) {
            closing = false;
        } else if (!closing && in_state_ms > 3000) {
            closing = true;
            rfg::global<uint8_t>(rfg::rva::ExitHandbook) = 1;
            LOG("automation: a handbook pane waits for a key -> closed");
        }
    }
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
    if (cfg.auto_load_save || cfg.exit_after_gameplay_s > 0) session::add_frame_callback(&on_frame);
    return ok;
}

}  // namespace automation
