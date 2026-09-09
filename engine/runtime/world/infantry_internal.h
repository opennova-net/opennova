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
void commit_body_state(InfantryState &inf, int resolved, const IRootMotionSource *root_motion,
                       bool player_wash = false);
// Select/cache an obstacle detour and publish target_heading before gait selection.
// [orig: ai_find_cover_position @0x4AFAB0]
void infantry_detour(AiSystem &ai, AiEntity &e, World &world);
void infantry_escort_goal(AiEntity &, World &, const Entity &target,
                         int32_t goal[3], int32_t &radius, int32_t &distance);
bool infantry_is_dragger(const AiEntity &, const World &);
bool infantry_drag_corpse(AiEntity &, World &);
// Called after the authority's 16-tick selection; scan every 256 ticks or while speaking.
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
