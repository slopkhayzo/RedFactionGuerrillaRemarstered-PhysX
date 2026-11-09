#pragma once
#include <cstdint>

#include "core/config.h"

// The game is a 32-bit program (large-address-aware: 4 GB of address space), and the game, its raised pools, the
// graphics driver and the plugin take most of it. When it runs out, allocations fail: the game stops drawing, shows
// "CreateTexture2D failed with error code 0x8007000e", or crashes (research/docs/08 §19). Measured on a thread of its
// own every few seconds; the log gets the main menu's, each level's first, every new low 32 MB below the last one
// logged, a warning once a level when little is left, and the level's lowest at its end.
namespace address_space {

struct Measure {
    uint32_t free_mb = 0;     // free below 4 GB
    uint32_t largest_mb = 0;  // the largest free block
};
Measure measure();  // a walk with VirtualQuery

void install(const Config& cfg);

}  // namespace address_space
