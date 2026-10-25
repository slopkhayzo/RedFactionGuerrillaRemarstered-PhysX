#pragma once

struct Config;

// Test.ViewShots=1: what the screen shows over 40 s of gameplay, for comparing settings by eye and by script (player
// reports: the minimap shaking and showing black squares, clouds jumping, the light dimming as if the sun were blocked
// depending on the view; they put the same symptoms down to running over 120 FPS without the mod). From 10 s into
// gameplay: a screenshot every second (view_NN.png in the session folder), the game camera turned 36 degrees every
// 4 s (ten directions, four shots each), and the game's frame times over those 40 s in a histogram. Test.ViewShots=N
// (N > 1) runs N seconds instead: every 10 s a burst of four shots half a second apart (view_BBB_K.png), then the turn,
// for a long stay (the reports say the minimap and the sky go wrong after a while in the open world).
namespace view_shots {

bool install(const Config& cfg);

}  // namespace view_shots
