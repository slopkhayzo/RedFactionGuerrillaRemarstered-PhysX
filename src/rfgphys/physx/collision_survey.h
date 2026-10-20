#pragma once

struct Config;

// X1 groundwork (research/docs/06 §5, §13). Experiment.CollisionSurvey=1: 5 s into each level's gameplay, walks
// every body of Havok's fixed island (the static world: terrain, rocks, intact buildings) through havok_shapes
// and logs what a PhysX mirror would have to hold: shape types, triangles, convex pieces, height fields, extents,
// and how long the walk took. One row per body in collision_survey.csv.
namespace collision_survey {

bool install(const Config& cfg);

}  // namespace collision_survey
