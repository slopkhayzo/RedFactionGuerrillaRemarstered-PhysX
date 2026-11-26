#include "menu/display_rows.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>

#include "core/config.h"
#include "core/log.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace display_rows {
namespace {

// The rows' captions and values: keys of the game's localization table (hashed as keen::getCrc32LwrValue does) and
// our texts, which the table points to (hence writable arrays that live as long as the game).
struct Label {
    const char* key;
    wchar_t text[32];
};
Label g_labels[] = {
    {"RFGPHYS_PHYSICS_NEXT_START", L"PHYSICS (NEXT START)"},
    {"RFGPHYS_PHYSICS_HAVOK", L"HAVOK"},
    {"RFGPHYS_PHYSICS_PHYSX", L"PHYSX"},
    {"RFGPHYS_KEEP_DEBRIS_NEXT_START", L"KEEP DEBRIS (NEXT START)"},
};
const char* kPhysicsNames[2] = {"RFGPHYS_PHYSICS_HAVOK", "RFGPHYS_PHYSICS_PHYSX"};

using ItemCallback = void(__cdecl*)(int, void*);
using PageFn = void(__cdecl*)(int);
using EnterFn = void(__cdecl*)(int);
using HintFn = void(__cdecl*)(int);
using ChangesMadeFn = bool(__cdecl*)();
using AddListFn = int(__cdecl*)(int, const char*, const char**, int, ItemCallback, int, bool);
using AddBoolFn = int(__cdecl*)(int, const char*, bool*, ItemCallback, bool);
using EnableFn = void(__cdecl*)(int, int, bool);
using CountFn = int(__cdecl*)(int);
using HashGetFn = bool(__thiscall*)(void*, uint32_t, wchar_t**);
using HashInsertFn = bool(__thiscall*)(void*, uint32_t, wchar_t* const*);

PageFn o_set_detail_page = nullptr;
EnterFn o_enter = nullptr;
HintFn o_hint = nullptr;
ChangesMadeFn o_changes_made = nullptr;

// The settings for the next start as they are, and as the rows have them since the display menu opened.
struct Values {
    int physics = 1;  // 1 PHYSX, 0 HAVOK
    bool keep_debris = false;
};
Values g_current, g_pending;
bool g_pending_keep = false;  // the KEEP DEBRIS row's variable: the menu writes it itself

std::wstring g_ini;
std::string g_test;          // Test.DisplayRows: "physics=0|1,keepdebris=0|1", set at the first accept from the main menu
int g_state_idle = -1;       // GS_MAINMENU_IDLE: with it (or no main menu document) the menu was opened in game
int g_hint_selection = 0;    // HT_SELECTION: the menu's accept
int g_page_general = 0;      // DETAIL_OPTIONS_PAGE_GENERAL
int g_row_physics = -1, g_row_keep = -1;
bool g_told = false;

int enum_value(const char* name, int fallback) {
    for (const auto& e : rfg::game_state_names)
        if (!strcmp(e.name, name)) return e.value;
    for (const auto& e : rfg::hint_type_names)
        if (!strcmp(e.name, name)) return e.value;
    for (const auto& e : rfg::detail_option_pages_names)
        if (!strcmp(e.name, name)) return e.value;
    return fallback;
}

// keen::getCrc32LwrValue: CRC-32 (reflected 0xEDB88320, initial value and final xor all ones), 'A'..'Z' folded to lower
// case, characters sign-extended.
uint32_t crc32_lower(const char* s) {
    static const struct Table {
        uint32_t v[256];
        Table() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int bit = 0; bit < 8; ++bit) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
                v[i] = c;
            }
        }
    } table;
    uint32_t crc = 0xFFFFFFFFu;
    for (; *s; ++s) {
        char ch = *s;
        if (uint8_t(ch - 'A') < 26) ch = char(ch + ' ');
        crc = (crc >> 8) ^ table.v[(uint32_t(int32_t(ch)) ^ crc) & 0xFF];
    }
    return ~crc;
}

// Our captions and values into the localization table, those it lacks (its data has none; a language reload clears
// the table, hence at every general page).
void add_labels() {
    void* table = rfg::addr(rfg::rva::Localization_hash);
    auto get = rfg::fn<HashGetFn>(rfg::rva::LocalizationHash_get);
    auto insert = rfg::fn<HashInsertFn>(rfg::rva::LocalizationHash_insert);
    int added = 0, failed = 0;
    for (Label& label : g_labels) {
        const uint32_t key = crc32_lower(label.key);
        wchar_t* existing = nullptr;
        get(table, key, &existing);
        if (existing) continue;
        wchar_t* text = label.text;
        if (insert(table, key, &text))
            ++added;
        else
            ++failed;
    }
    if (failed) LOG("display rows: %d of our captions could not go into the localization table (their rows show no text)", failed);
    if (added && !g_told) LOG("display rows: %d captions put into the localization table", added);
}

// The display menu opened from the main menu (no level loaded), as the game's own screen rows decide.
bool at_main_menu() {
    return rfg::gameseq_get_state() != g_state_idle && rfg::global<int>(rfg::rva::Doc_handle_main) != -1;
}

void update_rows() {
    const int menu = rfg::global<int>(rfg::rva::Menu_handle_doptions);
    auto enable = rfg::fn<EnableFn>(rfg::rva::ui_menu_enable_menu_item);
    if (g_row_physics >= 0) enable(menu, g_row_physics, at_main_menu());
    if (g_row_keep >= 0) enable(menu, g_row_keep, at_main_menu());
}

void __cdecl on_physics(int, void* value) { g_pending.physics = int(intptr_t(value)) == 1 ? 1 : 0; }

void __cdecl on_keep_debris(int, void*) { g_pending.keep_debris = g_pending_keep; }

bool changed() { return g_pending.physics != g_current.physics || g_pending.keep_debris != g_current.keep_debris; }

// rfgphys.ini's key, for the next start. The environment's RFGPHYS_<SECTION>_<KEY> wins over it then (the log says so).
void save(const wchar_t* section, const wchar_t* key, int value) {
    wchar_t text[16], env[96], set[8];
    swprintf(text, 16, L"%d", value);
    swprintf(env, 96, L"RFGPHYS_%ls_%ls", section, key);
    for (wchar_t* c = env; *c; ++c) *c = wchar_t(towupper(*c));
    if (!WritePrivateProfileStringW(section, key, text, g_ini.c_str()))
        LOG("display rows: rfgphys.ini could not be written (%ls.%ls=%ls, error %lu)", section, key, text, GetLastError());
    else if (GetEnvironmentVariableW(env, set, 8) > 0)
        LOG("display rows: %ls.%ls=%ls written to rfgphys.ini (the environment's %ls wins over it at the next start)", section, key, text, env);
}

// Test.DisplayRows: the rows set as a player would, before the first accept from the main menu.
void test_choose() {
    if (g_test.empty() || !at_main_menu()) return;
    for (size_t at = 0; at < g_test.size();) {
        size_t end = g_test.find_first_of(",;", at);
        if (end == std::string::npos) end = g_test.size();
        const std::string item = g_test.substr(at, end - at);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) {
            const std::string key = item.substr(0, eq);
            const int value = atoi(item.c_str() + eq + 1);
            if (key == "physics" && g_row_physics >= 0)
                on_physics(g_row_physics, reinterpret_cast<void*>(intptr_t(value)));
            else if (key == "keepdebris" && g_row_keep >= 0) {
                g_pending_keep = value != 0;
                on_keep_debris(g_row_keep, nullptr);
            }
        }
        at = end + 1;
    }
    LOG("display rows (Test.DisplayRows %s): the rows now PHYSICS %d, KEEP DEBRIS %d", g_test.c_str(), g_pending.physics,
        int(g_pending.keep_debris));
    g_test.clear();
}

// Accept: the rows' changes written to rfgphys.ini, for the next start.
void apply() {
    test_choose();
    if (!changed()) return;
    if (!at_main_menu()) {
        LOG("display rows: PHYSICS and KEEP DEBRIS change from the main menu only: kept");
        g_pending = g_current;
        return;
    }
    if (g_pending.physics != g_current.physics) {
        LOG("display rows: PHYSICS %s from the next start (Experiment.PhysX=%d in rfgphys.ini)", g_pending.physics ? "PHYSX" : "HAVOK",
            g_pending.physics);
        save(L"Experiment", L"PhysX", g_pending.physics);
    }
    if (g_pending.keep_debris != g_current.keep_debris) {
        LOG("display rows: KEEP DEBRIS %s from the next start (Experiment.KeepDebris=%d in rfgphys.ini)", g_pending.keep_debris ? "on" : "off",
            int(g_pending.keep_debris));
        save(L"Experiment", L"KeepDebris", g_pending.keep_debris ? 1 : 0);
    }
    g_current = g_pending;
}

// The general page: our rows after the ones already there.
void add_rows() {
    const int menu = rfg::global<int>(rfg::rva::Menu_handle_doptions);
    if (menu < 0) return;
    add_labels();
    auto count = rfg::fn<CountFn>(rfg::rva::ui_menu_get_total_item_count);
    auto add_list = rfg::fn<AddListFn>(rfg::rva::ui_menu_add_char_list_item);
    auto add_bool = rfg::fn<AddBoolFn>(rfg::rva::ui_menu_add_bool_item);
    g_row_physics = count(menu);
    if (add_list(menu, "RFGPHYS_PHYSICS_NEXT_START", kPhysicsNames, 2, &on_physics, g_pending.physics, true) < 0) g_row_physics = -1;
    // OpenRFG64: keep_debris is OpenRFG64's own (its source raises the game's limits, the plugin's stays off): no row
    if (!rfg::is_openrfg64()) {
        g_pending_keep = g_pending.keep_debris;
        g_row_keep = count(menu);
        if (add_bool(menu, "RFGPHYS_KEEP_DEBRIS_NEXT_START", &g_pending_keep, &on_keep_debris, true) < 0) g_row_keep = -1;
    }
    update_rows();
    if (!g_told) {
        g_told = true;
        LOG("display rows: the general page has PHYSICS (row %d) and KEEP DEBRIS (row %d)%s%s", g_row_physics, g_row_keep,
            rfg::is_openrfg64() ? " (OpenRFG64: no KEEP DEBRIS row, the game's own)" : "", at_main_menu() ? "" : " (in game: greyed out)");
    }
}

void __cdecl d_set_detail_page(int page) {
    o_set_detail_page(page);
    if (page == g_page_general) add_rows();
}

void __cdecl d_enter(int previous_state) {
    g_pending = g_current;
    g_row_physics = g_row_keep = -1;
    o_enter(previous_state);
}

void __cdecl d_hint(int hint) {
    if (hint == g_hint_selection) apply();
    o_hint(hint);
}

bool __cdecl d_changes_made() { return o_changes_made() || changed(); }

}  // namespace

bool install(const Config& cfg, const std::wstring& ini) {
    namespace rva = rfg::rva;
    if (!rva::set_detail_page || !rva::display_menu_enter || !rva::display_hint_callback || !rva::ui_menu_add_char_list_item ||
        !rva::ui_menu_add_bool_item || !rva::ui_menu_enable_menu_item || !rva::ui_menu_get_total_item_count ||
        !rva::Menu_handle_doptions || !rva::Doc_handle_main || !rva::Localization_hash || !rva::LocalizationHash_get ||
        !rva::LocalizationHash_insert) {
        LOG("display rows: this build lacks the display menu's names: no rows");
        return false;
    }
    g_ini = ini;
    g_test = cfg.test_display_rows;
    g_state_idle = enum_value("GS_MAINMENU_IDLE", 48);
    g_hint_selection = enum_value("HT_SELECTION", 0);
    g_page_general = enum_value("DETAIL_OPTIONS_PAGE_GENERAL", 0);
    g_current.physics = cfg.exp_physx != 0 ? 1 : 0;
    g_current.keep_debris = cfg.exp_keep_debris != 0;
    g_pending = g_current;
    if (!hooks::create(rva::set_detail_page, &d_set_detail_page, &o_set_detail_page, "set_detail_page") ||
        !hooks::create(rva::display_menu_enter, &d_enter, &o_enter, "display_menu_enter") ||
        !hooks::create(rva::display_hint_callback, &d_hint, &o_hint, "display_hint_callback")) {
        LOG("display rows: the display menu's hooks failed: no rows");
        return false;
    }
    // back's question (lose the changes?) for ours too; without it back drops them unasked
    const bool asks = rva::changes_made && hooks::create(rva::changes_made, &d_changes_made, &o_changes_made, "changes_made");
    LOG("display rows: PHYSICS and KEEP DEBRIS (from the next start) on the display menu's general page%s%s",
        asks ? "" : "; back drops their changes unasked", g_test.empty() ? "" : " (Test.DisplayRows chooses them at the first accept)");
    return true;
}

}  // namespace display_rows
