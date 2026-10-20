#pragma once
#include <string>

// Which known build the running rfg.exe is (index into rfg::kBuilds), -1 for none. A build is recognized by the PDB
// GUID/age in its debug directory, which names the compiled code and so every address we use. The PE timestamp and
// image size are reported but not required: a DRM wrapper or an exe patcher can change them (adding a section)
// without changing that code.
int identify_build(std::string& detail);
