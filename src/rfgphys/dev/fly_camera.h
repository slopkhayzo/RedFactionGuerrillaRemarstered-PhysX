#pragma once

struct Config;

// FlyCamera (development, for screenshots): a key (F10) detaches the view from the game camera, and the keyboard and
// mouse fly it about its own axes: W/S forward and back, A/D sideways, Space/Ctrl along the view's up, Q/E roll, the
// mouse or the arrow keys turn (yaw about the view's up, pitch about its right), Shift moves faster and Alt slower,
// the wheel zooms and the middle button gives back the game's field of view. The game gets no input meanwhile, so the
// player stands where they were; the same key returns to the game camera, which never moved. Only the drawn view
// changes: the game camera's state, and world streaming with it, stays with the player. Leaving gameplay (a menu,
// loading, death) ends it. (The remaster's own free camera, s_freeCamera in render_frame, has no way to turn it on.)
namespace fly_camera {

bool install(const Config& cfg);

}  // namespace fly_camera
