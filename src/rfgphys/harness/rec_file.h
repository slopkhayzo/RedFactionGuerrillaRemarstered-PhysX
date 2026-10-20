#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// input.rfgrec: a fixed header, then one record per main-loop frame (the load transition frame first, then frame 0,
// 1, ...). Layout documented in research/docs/04-test-harness.md section 3; tools/rec_tool.py reads it.
namespace rec {

constexpr uint32_t kInputSize = 0x444;  // cached_input_state: one controller, mouse, 256 keys
constexpr uint32_t kMaxGov = 8;         // load-governor readings kept per frame

enum StartType : uint32_t { kStartNone = 0, kStartSave = 1, kStartWreckingCrew = 2 };
constexpr uint32_t kFlagVirtualClock = 1;

#pragma pack(push, 1)
// frametime_calculate's state at the start event: frame counters (AI staggers work by Framecount), the 16-frame
// FPS average (read by the simulation) and the game timestamp clock. Replays restore it at the start event and
// both modes freeze it while loading, so frame 0 begins with identical clocks.
struct FrameClock {
    int32_t framecount;
    uint32_t total_ms;
    uint32_t total_ms_always;
    int32_t sample_count;
    float frametime_total;
    float fps;
    float frametimes[16];
    int32_t ticker;
    int32_t realtime_ticker;
    float amount_rounded;
};

struct Header {
    char magic[8];                  // "RFGREC01"
    uint32_t header_size;           // sizeof(Header)
    uint32_t input_size;            // kInputSize
    uint32_t seed;
    uint32_t start_type;            // StartType
    uint32_t frame_count;           // frames after the transition frame (0 until the recording is finished)
    uint32_t flags;                 // kFlag*
    int32_t clock_anchor_ms;        // virtual clock at the start event (timer_get milliseconds)
    uint32_t checkpoint_interval;
    uint32_t exe_timestamp;         // rfg.exe build of the recording
    uint8_t wc_options[0x2C];       // Wrecking_crew_options as the match began
    int32_t wc_character_index[4];  // Wc_player_infos[i].character_index
    char wc_map_file[64];
    uint32_t save_slot;             // game_save_info::save_slot_index
    uint8_t save_reset_destruction;
    uint8_t pad[3];
    char save_file[64];             // keen_savegame_P_S.sav, copied next to input.rfgrec
    FrameClock clock;
    uint8_t reserved[64];
};
#pragma pack(pop)

// Per-frame flags.
constexpr uint8_t kFrameAppActive = 1;   // the game window had focus (Os_app_active)
constexpr uint8_t kFrameMarker = 2;      // marker hotkey pressed
constexpr uint8_t kFrameTransition = 4;  // the frame whose gameseq_process switched from loading to gameplay

struct Frame {
    uint8_t flags = 0;
    uint8_t end_state = 0;  // game state after the frame (checked during replay)
    uint8_t n_gov = 0;
    float dt = 0;           // Frametime_uncompressed after frametime_calculate
    float dt_real = 0;      // Frametime_real
    uint16_t rng_index = 0; // after the frame: Vlib table index and the sim thread's CRT rand state (checked
    uint32_t crt_rand = 0;  // during replay: the first frame that drew different random numbers)
    int32_t gov[kMaxGov]{}; // gamestate_gp_last_havok_sim_time() results, in call order
    uint8_t input[kInputSize]{};
};

// Sim-thread writer. Records are encoded into a memory buffer and written out in blocks; `committed_` marks the end
// of the last complete record so a flush at process exit never writes half a frame.
class Writer {
public:
    bool open(const std::wstring& path, const Header& header);
    void append(const Frame& f);
    void finish(uint32_t frame_count);  // rewrites the header with the frame count and closes
    void flush_at_exit(uint32_t frame_count);
    bool is_open() const { return file_ != INVALID_HANDLE_VALUE; }

private:
    void write_out();
    HANDLE file_ = INVALID_HANDLE_VALUE;
    Header header_{};
    std::vector<uint8_t> buf_;
    size_t committed_ = 0;
    uint8_t prev_[kInputSize]{};
};

class Reader {
public:
    bool open(const std::wstring& path, std::string& error);
    const Header& header() const { return header_; }
    bool next(Frame& f);  // false at the end (or on a malformed record)

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
    Header header_{};
    uint8_t prev_[kInputSize]{};
};

// Small text file written with WriteFile (usable from DllMain at process exit, unlike CRT streams).
class TextFile {
public:
    bool open(const std::wstring& path);
    void write(const std::string& s);
    void printf(const char* fmt, ...);
    void flush();
    void close();
    bool is_open() const { return file_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::string buf_;
};

bool read_text_file(const std::wstring& path, std::string& out);

}  // namespace rec
