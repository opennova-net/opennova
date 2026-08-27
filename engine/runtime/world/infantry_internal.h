// Internal declarations shared between infantry.cpp and its size-gate splits.
// These are implementation helpers of the infantry motor, not a public surface:
// nothing outside engine/runtime/world/infantry*.cpp should include this.

#pragma once

#include <runtime/world/ai.h>

namespace opennova::world {

bool player_jump_world_state_blocked(const InfantryState &inf, const Entity *ent);
bool reset_capsule_bottom_state(int state);
bool advance_primary_channel(InfantryState &inf, IRootMotionSource &source,
                             RootMotionFrame &out);
void advance_primary_channel_fallback(InfantryState &inf);
void begin_body_transition_with_insert(InfantryState &inf, int resolved,
                                       const IRootMotionSource *root_motion);

} // namespace opennova::world
