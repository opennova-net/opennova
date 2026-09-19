// Internal declarations shared between infantry.cpp and its size-gate splits.
// These are implementation helpers of the infantry motor, not a public surface:
// nothing outside engine/runtime/world/infantry*.cpp should include this.

#pragma once

#include <runtime/world/ai.h>

namespace opennova::world {

// BAM bearing of (dx, dy) [orig: dbl_7C19D8 @0x7c19d8 -- atan2 * 2^31/pi]; the body-state
// commits with the gait->stance insert: the org1 form skips the arbitration on
// equality [orig: @0x4bd841], the org2 player form arbitrates unconditionally and
// applies the rotor-wash substitution [orig: @0x4b7356..0x4b73e5]. All defined in
// infantry.cpp, shared with infantry_combat.cpp and infantry_board.cpp.
int32_t bearing_to(int32_t dx, int32_t dy);
void commit_body_state(InfantryState &inf, int resolved, const IRootMotionSource *root_motion);
void commit_player_body_state(InfantryState &inf, int resolved,
                              const IRootMotionSource *root_motion, bool wash);
// Standing org1/org2 carrier delta, after the animation capsule sample.
// Returns whether carrier transport changed the position.
bool infantry_follow_carrier(AiEntity &, World &, int32_t capsule_bottom, bool player_body);
// Select/cache an obstacle detour and publish target_heading before gait selection.
// [orig: ai_find_cover_position @0x4AFAB0]
void infantry_detour(AiSystem &ai, AiEntity &e, World &world);
void infantry_escort_goal(AiEntity &, World &, const Entity &target,
                         int32_t goal[3], int32_t &radius, int32_t &distance);
bool infantry_is_dragger(const AiEntity &, const World &);
bool infantry_drag_corpse(AiEntity &, World &);
// Called after the authority's 16-tick selection; scan every 256 ticks, or every 32
// while the entity is the scripted voice speaker.
// [orig: Entity_UpdateInfantryAI @0x4BE0D0..0x4BE7FD]
void infantry_attention_think(AiSystem &ai, AiEntity &e, World &world, uint32_t key);

// Per-tick attachment sample, resolved before think and consumed before root motion.
// [orig: Entity_UpdateInfantryAI @0x4B9910, entity+0x184/+0x364]
struct InfantryAttachmentPose {
    EntityHandle parent;
    int32_t point[3] = {};
    int32_t distance = 0;
};
InfantryAttachmentPose infantry_attachment_pose(AiEntity &, World &);
void infantry_attachment_select(AiEntity &, World &, const InfantryAttachmentPose &);
bool infantry_attachment_move(AiEntity &, World &, const InfantryAttachmentPose &);

bool player_jump_world_state_blocked(const InfantryState &inf, const Entity *ent);
bool reset_capsule_bottom_state(int state);
bool advance_primary_channel(InfantryState &inf, IRootMotionSource &source,
                             RootMotionFrame &out);
void advance_primary_channel_fallback(InfantryState &inf);
void begin_body_transition_with_insert(InfantryState &inf, int resolved,
                                       const IRootMotionSource *root_motion);

} // namespace opennova::world
