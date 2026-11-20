#include "hooks/hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>

#include "core/log.h"
#include "game/rfg.h"

namespace hooks {

bool init() {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("MinHook init failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

bool create(uint32_t rva, void* detour, void** original, const char* name) {
    if (rva == 0) {  // OpenRFG64: a name its PDB does not have (the start-up report lists it)
        LOG("hook %s: no address in this build", name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(rfg::addr(rva), detour, original);
    if (s != MH_OK) {
        LOG("hook %s (RVA %08X) failed: %s", name, rva, MH_StatusToString(s));
        return false;
    }
    return true;
}

bool create_at(void* target, void* detour, void** original, const char* name) {
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        LOG("hook %s (%p) failed: %s", name, target, MH_StatusToString(s));
        return false;
    }
    return true;
}

bool enable_at(void* target) {
    MH_STATUS s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOG("enabling hook at %p failed: %s", target, MH_StatusToString(s));
        return false;
    }
    return true;
}

void* patch_import(const char* dll, const char* function, void* replacement) {
    auto base = reinterpret_cast<uint8_t*>(rfg::base());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            DWORD old;  // OpenRFG64: pointer-sized slots (8 bytes on x64)
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old);
            void* previous = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            return previous;
        }
    }
    LOG("import %s!%s not found", dll, function);
    return nullptr;
}

bool enable_all() {
    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) {
        LOG("enabling hooks failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

void shutdown() { MH_Uninitialize(); }

}  // namespace hooks
