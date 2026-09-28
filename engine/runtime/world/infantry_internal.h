// Internal declarations shared between infantry.cpp and its size-gate splits.
// These are implementation helpers of the infantry motor, not a public surface:
// nothing outside engine/runtime/world/infantry*.cpp should include this.

#pragma once

#include <runtime/world/ai.h>

namespace opennova::world {

// BAM bearing of (dx, dy) [orig: dbl_7C19D8 @0x7c19d8 -- atan2 * 2^31/pi]; the body-state
// request arbitration: the org1 form skips the arbitration on
// equality [orig: @0x4bd841], the org2 player form arbitrates unconditionally and
// applies the rotor-wash substitution [orig: @0x4b7356..0x4b73e5]. All defined in
// infantry.cpp, shared with infantry_combat.cpp and infantry_board.cpp.
// The motor-head channel update in infantry_animation.cpp owns gait inserts.
int32_t bearing_to(int32_t dx, int32_t dy);
void commit_body_state(InfantryState &inf, int resolved);
void commit_player_body_state(InfantryState &inf, int resolved,
                              const IRootMotionSource *root_motion, bool wash);
// Standing org1/org2 carrier delta, after the animation capsule sample.
// Returns whether carrier transport changed the position.
bool infantry_follow_carrier(AiEntity &, World &, int32_t capsule_bottom, bool player_body);
// Select/cache an obstacle detour and publish target_heading before gait selection.
// [orig: AI_FindCoverPosition @0x4AFAB0]
void infantry_detour(AiSystem &ai, AiEntity &e, World &world);
// The think's entity LOS [orig: Entity_CheckLineOfSightTerrainAndEntities
// @0x53B130]: the shared collision world, or its terrain leg alone when the
// embedder wired no model world. Defined in infantry_combat.cpp.
bool infantry_entity_los(AiSystem &ai, World &world, EntityHandle a, EntityHandle b,
                         const int32_t start[3], const int32_t end[3], int32_t height_offset,
                         bool all_types);
void infantry_escort_goal(AiEntity &, World &, const Entity &target,
                         int32_t goal[3], int32_t &radius, int32_t &distance);
bool infantry_is_dragger(const AiEntity &, const World &);
bool infantry_drag_corpse(AiEntity &, World &);
// The think's post-commit tail, called after the authority's 16-tick selection:
// the ride link, the idle facing fan, the stop fix-up, facials and attention
// (scan every 256 ticks, or every 32 while the entity is the scripted voice
// speaker). [orig: Entity_UpdateInfantryAI @0x4BD87E..0x4BE7FD]
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
// The once-per-life death edge (infantry_death.cpp). `org1` selects the NPC
// legs: the drowning clip, the unstaged-hit alert, the death tick, the 0xC0 clear
// and, on the authority, the edge's own death transaction.
// [orig: Entity_UpdateInfantryAI @0x4B9C40..0x4B9D55]
void infantry_death_edge(AiSystem &ai, AiEntity &e, World &world, Entity *ent, bool org1,
                         uint32_t logic_tick);
bool reset_capsule_bottom_state(int state);
// The end-notify arm both channels share: the tick the armed channel parks on,
// where AnimChannel_AdvancePlayback latches the end flag: a loop's next wrap
// after `phase`, a one-shot's end, kEndNotifyNeverLatches for a one-shot that
// already stopped (its advance body is skipped, so the flag never latches);
// -1 without a clip. [orig: AnimMap_UpdateEntity @0x40B7B3 / @0x40B7E1;
//  AnimChannel_AdvancePlayback @0x40B14D (the stop gate), @0x40B188..0x40B18F,
//  @0x40B19E..0x40B1B1]
int32_t arm_end_notify(const IRootMotionSource &source, int adm_id, int state, int variant,
                       int32_t phase);
// The primary channel's motor-head update; a re-init serves its ring entry from
// `rings` (AnimVariantRings).
bool advance_primary_channel(InfantryState &inf, IRootMotionSource &source,
                             AnimVariantRings &rings, RootMotionFrame &out);
void advance_primary_channel_fallback(InfantryState &inf);
void begin_body_transition_with_insert(InfantryState &inf, int resolved,
                                       const IRootMotionSource *root_motion,
                                       AnimVariantRings *rings);

} // namespace opennova::world
