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

// One list of values (a names table). required: a program without one of them cannot run the plugin (rfgphys's struct
// layouts), so the names table tool fails the build when one is not found (a renamed member, say).
struct List {
    Name* names;
    size_t count;
    bool required = false;
};

// The names table file: each list's values as a build's PDB gives them, looked up once at build time (write_table, a
// post-build step), so that the program needs neither its PDB nor DbgHelp at start-up. It holds the build's PDB
// signature (the GUID and age its debug directory names) and a hash of each list's names: a table of another build, or
// of other lists, is never used.
//
// Fills the lists from the table at 'path', for the program loaded at 'base'. -1 when there is no such table ('report'
// says why): load() them from the PDB then. Otherwise how many values the build does not have, each list's in
// missing[] (may be null); those are 0, as load() leaves them.
int load_table(const List* lists, size_t list_count, uintptr_t base, const wchar_t* path, int* missing, std::string& report);
// Build time: every list's values looked up in the PDB of 'exe' (beside it), as load() does in the running program, and
// written to 'path'. Returns how many it does not have (written as such), each list's in missing[] (may be null), or -1
// when no table was written.
int write_table(const List* lists, size_t list_count, const wchar_t* exe, const wchar_t* path, int* missing, std::string& report);
// A names table tool's main: "<tool> <OpenRFG.exe> <table file>", the report on stdout. 0 when the table is written
// with every required list's values.
int write_table_main(int argc, wchar_t** argv, const List* lists, size_t list_count);

}  // namespace openrfg64
