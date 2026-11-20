#pragma once
// OpenRFG64 as a build of rfg.exe for the plugins (the same file serves the path tracer, rfgrt).
//
// OpenRFG64 (gamerscruffy/OpenRFG64, a 64-bit OpenRFG) keeps rfg.pdb's names for its functions, globals, types and
// members, but every rebuild moves its addresses, and its layouts are the 64-bit ones. So the plugins keep no table
// of its values: at start-up the program's own PDB (OpenRFG.pdb, beside it) is read through a private DbgHelp session
// and each value is looked up by name.
#include <cstddef>
#include <cstdint>
#include <string>

namespace openrfg64 {

// One value to fill.
//   kind 'f' function: RVA into *value, its code size into *fn_size (if given); 'signature' (the original's
//          parameter list) picks between overloads.
//   kind 'v' global: RVA.  kind 'p' public symbol by decorated name (vftables: ??_7X@@6B@): RVA.
//   kind 'm' member: offset of 'member' in type 'name', plus 'addend'. 'member' is a path: a.b.c, with [i] for an
//          array element and (Base) for a base class step (inherited members are also found without it).
//   kind 's' type size: sizeof('name'), plus 'addend'.
struct Name {
    uint32_t* value;
    char kind;
    const char* name;
    const char* member;
    const char* signature;
    uint32_t* fn_size;
    int32_t addend;
};

// The running program is OpenRFG64: an x64 image whose CodeView record names OpenRFG.pdb.
bool detected(const char* pdb_path, uint16_t machine);

// Fills every entry from the PDB of the program loaded at 'base'. Returns how many could not be resolved (their
// values are set to 0: callers must treat a 0 RVA as "not in this build"); 'report' gets one line per problem and a
// summary line.
int load(Name* names, size_t count, uintptr_t base, std::string& report);

}  // namespace openrfg64
