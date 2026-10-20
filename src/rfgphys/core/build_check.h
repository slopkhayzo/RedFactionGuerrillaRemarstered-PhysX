#pragma once
#include <string>

// True when the running rfg.exe is the build our symbols were generated from
// (PE timestamp, image size and the PDB GUID/age in its debug directory).
bool verify_build(std::string& detail);
