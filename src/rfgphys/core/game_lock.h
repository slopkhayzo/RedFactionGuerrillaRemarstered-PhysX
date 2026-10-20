#pragma once

// Serializes the game's own code where Havok may call it from several threads at once. With a multithreaded Havok
// step (experiments/havok_mt.h) the contact, phantom-overlap and trigger-shape callbacks the game registers run inside
// Havok's jobs on worker threads, and none of that game code is thread-safe. Hooks around those callbacks hold a Guard;
// it locks nothing until set() gives it a lock, so the single-threaded game pays no cost.
namespace game_lock {

// The lock to use from now on: a Win32 CRITICAL_SECTION (recursive), or null for none. havok_mt passes the
// multithreaded world's constraint-update lock (hkpWorld::m_modifyConstraintCriticalSection): Havok's broadphase job
// already holds it while it hands new phantom pairs to the game, so a lock of our own taken inside those callbacks
// would be acquired in both orders and deadlock. With Havok's lock every guarded callback has one lock order.
void set(void* critical_section);
bool enabled();
unsigned long owner_thread();  // diagnostics: the thread holding the lock, 0 if none

class Guard {
public:
    Guard();
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    void* cs_;
};

}  // namespace game_lock
