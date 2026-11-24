// rfgphys's names table for an OpenRFG64 build (src/shared/openrfg64_column.h), a post-build step of a game that
// builds the plugin's code (OpenRFG64 VOM): the names, the struct layouts and the optional layout values looked up in
// the program's PDB once, so that the plugin needs neither the PDB nor DbgHelp at start-up (game/rfg.cpp reads the
// table, in the same order of lists).
//   rfgphys_names <OpenRFG.exe> <rfgphys.names, in the plugin's folder>
#include <iterator>

#include "game/layout.h"
#include "game/rfg_symbols_names.generated.h"
#include "openrfg64_column.h"

int wmain(int argc, wchar_t** argv) {
    const openrfg64::List lists[] = {{rfg::kOpenRFG64Names, std::size(rfg::kOpenRFG64Names)},
                                     {layout::names(), layout::name_count(), true},
                                     {layout::optional_names(), layout::optional_count()}};
    return openrfg64::write_table_main(argc, argv, lists, std::size(lists));
}
