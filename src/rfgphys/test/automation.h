#pragma once

struct Config;

// Unattended-run support: block all save writes/erases, load the most recent save from the main menu without
// input, and quit after a given time in gameplay. Driven from the main-loop frame callback.
namespace automation {

bool install(const Config& cfg);

// Main-menu helpers shared with the record/replay harness (sim thread, end of frame).
// The main menu waits for "any button" to pick player 1's input device; this does what a caught key press does.
void catch_keyboard_and_mouse();
// Profile, options and the save list are loaded (only then do Load Game and Wrecking Crew work).
bool main_menu_ready();

// Main menu actions (main_menu_handle_selection argument; the jump table in main_menu_handle_selection).
constexpr int kMainMenuLoadGame = 1;
constexpr int kMainMenuWreckingCrew = 2;

}  // namespace automation
