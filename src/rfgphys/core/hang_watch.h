#pragma once
#include "core/config.h"

// The game can hang without crashing: a thread loops forever, or waits for one that does, and the window freezes.
// A watch thread notices when a gameplay frame has not ended for Config::hang_s seconds and, if a thread of the game
// is busy (spinning; an unattended run ends whatever the threads do), writes a hang report to the log (the stacks of
// the sim thread, the render thread and the busy threads; tools/hang_report.py) and ends the process the way
// session::exit_now does, so recordings and telemetry are written. A game that is only blocked (its window
// minimised, a debugger) uses no CPU and is left alone.
namespace hang_watch {

// Before the hooks are enabled (it may register a frame callback: Test.HangAfterSeconds).
void start(const Config& cfg);

}  // namespace hang_watch
