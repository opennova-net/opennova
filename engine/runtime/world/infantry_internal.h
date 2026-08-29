// Internal declarations shared between infantry.cpp and its size-gate splits.
// These are implementation helpers of the infantry motor, not a public surface:
// nothing outside engine/runtime/world/infantry*.cpp should include this.

#pragma once

#include <runtime/world/ai.h>

namespace opennova::world {

// BAM bearing of (dx, dy) [orig: dbl_7C19D8 @0x7c19d8 -- atan2 * 2^31/pi]; the body-state
// commit with the gait->stance insert. Both defined in infantry.cpp, shared with
// infantry_combat.cpp.
int32_t bearing_to(int32_t dx, int32_t dy);
void commit_body_state(InfantryState &inf, int resolved, const IRootMotionSource *root_motion);

bool player_jump_world_state_blocked(const InfantryState &inf, const Entity *ent);
bool reset_capsule_bottom_state(int state);
bool advance_primary_channel(InfantryState &inf, IRootMotionSource &source,
                             RootMotionFrame &out);
void advance_primary_channel_fallback(InfantryState &inf);
void begin_body_transition_with_insert(InfantryState &inf, int resolved,
                                       const IRootMotionSource *root_motion);

} // namespace opennova::world
