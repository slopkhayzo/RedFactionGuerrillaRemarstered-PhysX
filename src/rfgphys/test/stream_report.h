#pragma once

struct Config;

// Test.StreamReport=S: S seconds into gameplay, what the stream containers say (research/docs/07, B5a: a map's new
// building types never appeared). Logs every container whose load failed (its error flag, 0x40, set: the game then
// gives the objects waiting for it the placeholder missing_chunk.rfgchunkx) with the error text the game kept, every
// building type's container (.rfgchunkx) and ns_base with their status, flags, references and entries (the primitives
// of a failed one or of a type's still waiting), and each rfg_mover with its container and whether its chunk loaded.
namespace stream_report {

bool install(const Config& cfg);

}  // namespace stream_report
