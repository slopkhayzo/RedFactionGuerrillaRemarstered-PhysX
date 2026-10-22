#pragma once

struct Config;

// Test.VehiclePush: a vehicle driven through rubble in PhysX pushes it (the vehicle proxies of physx/debris_offload),
// unattended. In the campaign, 15 s into gameplay: the vehicle nearest the player and the building nearest that
// vehicle (15-60 m from it); the building is blown up, and 20 s later, the rubble settled and mostly frozen, the
// vehicle is moved through it at 8 m/s, up to 12 m past the building. The log compares the pieces near its path that
// moved with the ones farther off.
namespace vehicle_push {

bool install(const Config& cfg);

}  // namespace vehicle_push
