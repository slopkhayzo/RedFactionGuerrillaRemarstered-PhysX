#pragma once
// rfgphys's start: the configuration, the game's build and names, and every module, in order. The ASI (main.cpp)
// runs it from its own folder; a game that builds rfgphys in (OpenRFG64 VOM's PhysX backend) runs it from its own.
#include <string>

namespace startup {

// `dir`: the folder of rfgphys.ini and of rfgphysx\ (the host or the PhysX core). Logs why it stops early. Call once.
// launch_physx false: everything installs but the PhysX host, which the game starts later (pxbridge::start), as when a
// game's PhysX backend is off at its start and the record/replay harness runs.
void run(const std::wstring& dir, bool launch_physx = true);

}  // namespace startup
