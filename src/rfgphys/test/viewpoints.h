#pragma once

struct Config;

// Test.Viewpoints=1 (replays; with Harness.LeaveLevelAtEnd the level is left afterwards): after the recording's last
// frame the view holds still at viewpoints placed around the level's debris, 4 s each, so the render cost of a
// levelled map is measured the same way on any map: the replay's last view, then from high above and from mid height,
// each from four sides, looking at the debris. Per view: a screenshot (viewpoint_N.png) and the frame times in the log
// ("viewpoints: view N ..."; its time window too, for the telemetry and the profiler), then a summary.
namespace viewpoints {

bool install(const Config& cfg);
// The harness, at the recording's end: whether the viewpoints run before the replay ends.
bool wanted();
// The harness: the replay is over, the view is the test's.
void begin();
bool finished();

}  // namespace viewpoints
