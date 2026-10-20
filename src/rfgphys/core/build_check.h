#pragma once
#include <string>

// True when the running rfg.exe is the compile our symbols were generated from: the PDB GUID/age in its debug
// directory, which names the compiled code and so every address we use. The PE timestamp and image size are reported
// but not required: a DRM wrapper or an exe patcher can change them (adding a section) without changing that code.
bool verify_build(std::string& detail);
