// rfgphys: Red Faction Guerrilla Re-Mars-tered physics/performance plugin, loaded by Ultimate ASI Loader.
// The start itself is core/startup.cpp, shared with games that build rfgphys in.
#include <windows.h>

#include <string>

#include "core/session.h"
#include "core/startup.h"

namespace {

HMODULE g_module = nullptr;

std::wstring module_dir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

}  // namespace

// Called by Ultimate ASI Loader after LoadLibrary, outside the loader lock, when the game's own code starts.
extern "C" __declspec(dllexport) void InitializeASI() {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) startup::run(module_dir());
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        session::on_process_exit();  // process is terminating: flush what is buffered
    }
    return TRUE;
}
