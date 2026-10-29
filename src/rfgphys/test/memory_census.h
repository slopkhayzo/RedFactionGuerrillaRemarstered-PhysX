#pragma once

#include <cstdint>

struct Config;

// Where the game's address space goes, for Test.StressPieces (about 0.9 KB of private memory committed per piece, its
// owner unknown): private memory committed by allocation (one VirtualAlloc reservation each), the heaps
// (HeapSummary) and, for a big or growing heap, its busy blocks by size; the D3D11 buffers and textures alive, by kind.
// Each log() compares with the one before.
namespace memory_census {

bool install(const Config& cfg);  // the D3D11 counters: before the game creates its device
// `havok_lo`/`havok_hi`: Havok's heap, to name its allocation (0: unknown).
void log(const char* when, uintptr_t havok_lo, uintptr_t havok_hi);

}  // namespace memory_census
