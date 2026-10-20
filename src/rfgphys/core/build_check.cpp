#include "core/build_check.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "game/rfg.h"

namespace {

struct CodeViewRsds {
    DWORD signature;  // 'RSDS'
    GUID guid;
    DWORD age;
};

}  // namespace

bool verify_build(std::string& detail) {
    auto base = reinterpret_cast<const uint8_t*>(rfg::base());
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        detail = "no MZ header";
        return false;
    }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const DWORD ts = nt->FileHeader.TimeDateStamp;
    const DWORD size = nt->OptionalHeader.SizeOfImage;

    const CodeViewRsds* rsds = nullptr;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    auto dbg = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    for (DWORD i = 0; dir.VirtualAddress && i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        if (dbg[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW && dbg[i].AddressOfRawData) {
            auto cv = reinterpret_cast<const CodeViewRsds*>(base + dbg[i].AddressOfRawData);
            if (cv->signature == 0x53445352) rsds = cv;  // "RSDS"
        }
    }

    const bool ok_ts = ts == rfg::kExeTimeDateStamp;
    const bool ok_size = size == rfg::kExeSizeOfImage;
    const bool ok_pdb = rsds && memcmp(&rsds->guid, rfg::kPdbGuid, 16) == 0 && rsds->age == rfg::kPdbAge;
    char buf[256];
    snprintf(buf, sizeof(buf), "timestamp %08lX (%s), image size %08lX (%s), pdb guid/age %s", ts,
             ok_ts ? "ok" : "MISMATCH", size, ok_size ? "ok" : "MISMATCH",
             rsds ? (ok_pdb ? "ok" : "MISMATCH") : "missing");
    detail = buf;
    return ok_ts && ok_size && ok_pdb;
}
