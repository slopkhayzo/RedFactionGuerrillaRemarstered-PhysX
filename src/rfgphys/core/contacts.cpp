#include "core/contacts.h"

#include <vector>

#include "core/game_lock.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace contacts {
namespace {

bool g_installed = false;
bool g_skip_debris_pairs = false;
std::vector<void (*)(uint32_t, uint32_t)> g_observers;

// Collision layer of the rigid body a contact belongs to: hkpCdBody::m_parent (+0xC) leads to the root
// hkpCollidable, whose broadphase handle's filter info is at +0x1C (layer in the low 16 bits).
uint32_t body_layer(const uint8_t* body) {
    for (int depth = 0; body && depth < 16; ++depth) {
        const uint8_t* parent = *reinterpret_cast<const uint8_t* const*>(body + 0xC);
        if (!parent) return *reinterpret_cast<const uint32_t*>(body + 0x1C) & 0xFFFF;
        body = parent;
    }
    return 0;
}

// __thiscall GeneralCollisionListener::contactPointAddedCallback(hkpContactPointAddedEvent*), hooked through
// __fastcall. The event starts with m_bodyA and m_bodyB (hkpCdBody*). With a multithreaded Havok step it runs on
// Havok's worker threads too: the game lock serializes it, observers included.
void(__fastcall* o_contact_added)(void*, void*, void*);
void __fastcall d_contact_added(void* self, void* edx, void* event) {
    game_lock::Guard guard;
    if (event) {
        const auto* const* bodies = static_cast<const uint8_t* const*>(event);
        const uint32_t a = body_layer(bodies[0]), b = body_layer(bodies[1]);
        for (auto fn : g_observers) fn(a, b);
        if (g_skip_debris_pairs && is_debris_layer(a) && is_debris_layer(b)) return;
    }
    o_contact_added(self, edx, event);
}

}  // namespace

bool install() {
    if (g_installed) return true;
    g_installed = hooks::create(rfg::rva::GeneralCollisionListener__contactPointAddedCallback, &d_contact_added,
                                &o_contact_added, "GeneralCollisionListener::contactPointAddedCallback");
    return g_installed;
}

void add_observer(void (*fn)(uint32_t, uint32_t)) { g_observers.push_back(fn); }
void set_skip_debris_pairs(bool on) { g_skip_debris_pairs = on; }

}  // namespace contacts
