#include "telemetry/object_census.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"
#include "telemetry/telemetry.h"

namespace census {
namespace {

constexpr int32_t OT_MOVER = 2;

struct Entry {
    uint32_t caller;    // RVA of the return address in the calling function
    uint32_t outer;     // calls through a forwarding function: that function's caller (else 0)
    int32_t type;       // object::obj_type
    uint32_t total;
    uint32_t gameplay;  // during GS_GAMEPLAY
};

class Table {
public:
    void add(uint32_t caller, uint32_t outer, int32_t type) {
        const bool gameplay = telemetry::current_state() == 1;
        AcquireSRWLockExclusive(&lock_);
        Entry* e = nullptr;
        for (int i = 0; i < count_ && !e; ++i)
            if (entries_[i].caller == caller && entries_[i].outer == outer && entries_[i].type == type) e = &entries_[i];
        if (!e && count_ < kMax) {
            e = &entries_[count_++];
            *e = Entry{caller, outer, type, 0, 0};
        }
        if (e) {
            ++e->total;
            if (gameplay) ++e->gameplay;
        } else {
            ++dropped_;
        }
        ReleaseSRWLockExclusive(&lock_);
    }

    void write(const wchar_t* name, bool at_exit) {
        if (at_exit) {
            if (!TryAcquireSRWLockExclusive(&lock_)) return;
        } else {
            AcquireSRWLockExclusive(&lock_);
        }
        const std::wstring path = session::run_dir() + L"\\" + name;
        if (FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR)) {
            fputs("caller_rva,outer_rva,obj_type,count,gameplay_count\n", f);
            for (int i = 0; i < count_; ++i) {
                const Entry& e = entries_[i];
                // as the GOG build's RVAs, which its PDB names (the same in the GOG build)
                fprintf(f, "%08X,%08X,%d,%u,%u\n", rfg::gog_rva(e.caller), e.outer ? rfg::gog_rva(e.outer) : 0u,
                        e.type, e.total, e.gameplay);
            }
            if (dropped_) fprintf(f, "FFFFFFFF,00000000,-1,%u,0\n", dropped_);  // table full
            fclose(f);
        }
        ReleaseSRWLockExclusive(&lock_);
    }

private:
    static constexpr int kMax = 512;
    Entry entries_[kMax];
    int count_ = 0;
    uint32_t dropped_ = 0;
    SRWLOCK lock_ = SRWLOCK_INIT;
};

Table g_created, g_destroyed, g_faded, g_fade_refused, g_create_failed;
bool (*g_fade_filter)(uint8_t*, uint32_t) = nullptr;
std::atomic<uint32_t> g_failed_shards{0};
bool g_installed = false;
std::atomic<uint32_t> g_frame_movers_created{0}, g_frame_destroyed{0}, g_frame_island{0};
thread_local uint32_t t_outer_caller = 0;

inline uint32_t rva_of(uintptr_t p) { return uint32_t(p - rfg::base()); }
inline bool within(uint32_t rva, uint32_t fn, uint32_t size) { return rva - fn < size; }

// world::create_object<T> forwards to create_object_internal with a few instructions around the call, so its own
// return address sits a few slots above the call's arguments. Returns it when one of those slots holds the return
// address of a call to a function that starts just before `caller`.
uint32_t forwarder_caller(void* const* frame, uintptr_t caller) {
    const uintptr_t lo = rfg::base() + 0x1000, hi = rfg::base() + rfg::image_size();
    for (int i = 5; i <= 8; ++i) {
        const uintptr_t ret = reinterpret_cast<uintptr_t>(frame[i]);
        if (ret < lo + 5 || ret >= hi) continue;
        const auto* call = reinterpret_cast<const uint8_t*>(ret - 5);
        if (call[0] != 0xE8) continue;
        const uintptr_t target = ret + uintptr_t(*reinterpret_cast<const int32_t*>(call + 1));
        if (caller > target && caller - target < 48) return rva_of(ret);
    }
    return 0;
}

// world::create_object_internal(class hash, obj_prop_block*, world_zone*, flags)
uint8_t*(__fastcall* o_create)(void*, void*, uint32_t, void*, void*, uint32_t);
uint8_t* __fastcall d_create(void* world, void* edx, uint32_t cls, void* props, void* zone, uint32_t flags) {
    void* const* frame = static_cast<void* const*>(_AddressOfReturnAddress());
    const uintptr_t caller = reinterpret_cast<uintptr_t>(frame[0]);
    const uint32_t outer = forwarder_caller(frame, caller);
    uint8_t* obj = o_create(world, edx, cls, props, zone, flags);
    if (obj) {
        const int32_t type = int8_t(obj[0x7E]);
        g_created.add(rva_of(caller), outer, type);
        if (type == OT_MOVER) g_frame_movers_created.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_create_failed.add(rva_of(caller), outer, -1);
        if (within(rva_of(caller), rfg::rva::rfg_dyn_create_shard, rfg::size::rfg_dyn_create_shard))
            g_failed_shards.fetch_add(1, std::memory_order_relaxed);
    }
    return obj;
}

// object_mover_mark_for_fade(object_mover*, int): the piece fades out and object_mover::handle_fading destroys it.
void(__cdecl* o_mark_for_fade)(uint8_t*, int);
void __cdecl d_mark_for_fade(uint8_t* mover, int arg) {
    const uint32_t caller = rva_of(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    if (mover && g_fade_filter && !g_fade_filter(mover, caller)) {
        g_fade_refused.add(caller, 0, int8_t(mover[0x7E]));
        return;
    }
    if (mover) g_faded.add(caller, 0, int8_t(mover[0x7E]));
    o_mark_for_fade(mover, arg);
}

// world::destroy_object(object*, bool): flags the object for deletion (the world frees flagged objects later).
bool(__fastcall* o_destroy)(void*, void*, uint8_t*, bool);
bool __fastcall d_destroy(void* world, void* edx, uint8_t* obj, bool flag) {
    // The same checks the function makes before it destroys anything: a valid handle (+0x6C) and type (+0x7E).
    const bool valid = obj && *reinterpret_cast<uint32_t*>(obj + 0x6C) != 0xFFFFFFFFu && int8_t(obj[0x7E]) != -1;
    const int32_t type = valid ? int8_t(obj[0x7E]) : -1;
    uint32_t caller = rva_of(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    uint32_t outer = 0;
    if (within(caller, rfg::rva::world__destroy_object_by_handle, rfg::size::world__destroy_object_by_handle)) {
        outer = t_outer_caller;
    }
    const bool done = o_destroy(world, edx, obj, flag);
    if (done && valid) {
        g_destroyed.add(caller, outer, type);
        g_frame_destroyed.fetch_add(1, std::memory_order_relaxed);
        if (within(caller, rfg::rva::havok_memory_watch_dog_free_memory, rfg::size::havok_memory_watch_dog_free_memory) ||
            within(caller, rfg::rva::watch_dog_remove_objects_from_island,
                   rfg::size::watch_dog_remove_objects_from_island))
            g_frame_island.fetch_add(1, std::memory_order_relaxed);
    }
    return done;
}

// world::destroy_object(handle, bool) looks the object up and calls the overload above.
bool(__fastcall* o_destroy_by_handle)(void*, void*, uint32_t, bool);
bool __fastcall d_destroy_by_handle(void* world, void* edx, uint32_t handle, bool flag) {
    const uint32_t saved = t_outer_caller;
    t_outer_caller = rva_of(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    const bool done = o_destroy_by_handle(world, edx, handle, flag);
    t_outer_caller = saved;
    return done;
}

void write(bool at_exit) {
    if (!session::config().telemetry) return;  // KeepDebris needs the hooks, not the files
    g_created.write(L"creations.csv", at_exit);
    g_destroyed.write(L"deletions.csv", at_exit);
    g_faded.write(L"fades.csv", at_exit);
    if (g_fade_filter) g_fade_refused.write(L"fades_refused.csv", at_exit);
    g_create_failed.write(L"creations_failed.csv", at_exit);
}

}  // namespace

bool install() {
    if (g_installed) return true;
    g_installed = true;
    namespace r = rfg::rva;
    bool ok = hooks::create(r::world__create_object_internal, &d_create, &o_create, "world::create_object_internal");
    ok &= hooks::create(r::object_mover_mark_for_fade, &d_mark_for_fade, &o_mark_for_fade, "object_mover_mark_for_fade");
    ok &= hooks::create(r::world__destroy_object, &d_destroy, &o_destroy, "world::destroy_object");
    ok &= hooks::create(r::world__destroy_object_by_handle, &d_destroy_by_handle, &o_destroy_by_handle,
                        "world::destroy_object(handle)");
    session::add_flush_handler(&write);
    return ok;
}

void set_fade_filter(bool (*allow)(uint8_t*, uint32_t)) { g_fade_filter = allow; }

uint32_t take_failed_shards() { return g_failed_shards.exchange(0, std::memory_order_relaxed); }

FrameCounts take_frame_counts() {
    return {g_frame_movers_created.exchange(0, std::memory_order_relaxed),
            g_frame_destroyed.exchange(0, std::memory_order_relaxed),
            g_frame_island.exchange(0, std::memory_order_relaxed)};
}

}  // namespace census
