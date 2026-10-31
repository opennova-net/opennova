// A person body's presentation inputs, from the frame's present rows (world/present_rows.h) to the
// verbs its body channel takes (anim::BodyChannelTarget, runtime/anim/body_channel.h): which clip
// a placed or authoritative row plays (the playing state at its tick, the outgoing state blended
// under it, or the coarse body-anim slot), the receive-side walk a remote-request row takes on a
// joiner, and the aim overlay's per-class deltas the row carries. A backend's present walk reads
// these and fans the verbs out over its model's parts; it derives none of them.
#pragma once

#include <runtime/anim/aim_overlay.h>
#include <runtime/anim/anim_sample.h>
#include <runtime/anim/body_channel.h>
#include <runtime/world/infantry.h>

#include <cstdint>

namespace opennova::world {

// One body's clip request: the state's clip at the simulation's tick (kClipAt), the outgoing
// state blended under it while its blend runs (kBlendAt), or the coarse slot (world::BodyAnim) at
// the tick when no state resolves (kSlotAt). Two equal requests pose the same channel; a walk
// re-dispatches only a changed one.
struct BodyClipRequest {
	enum class Kind : uint8_t { kNone, kClipAt, kBlendAt, kSlotAt };
	Kind kind = Kind::kNone;
	int32_t selector = -1; // the state (kClipAt, kBlendAt) or the slot (kSlotAt)
	int32_t phase = 0;
	int32_t variant = 0;
	bool parked = false; // the armed end-notify's park (an unblended target alone)
	int32_t source_state = -1; // kBlendAt
	int32_t source_phase = 0;
	int32_t source_variant = 0;
	float weight = 1.0f;

	bool operator==(const BodyClipRequest &o) const {
		return kind == o.kind && selector == o.selector && phase == o.phase && variant == o.variant &&
				parked == o.parked && source_state == o.source_state &&
				source_phase == o.source_phase && source_variant == o.source_variant &&
				weight == o.weight;
	}
	bool operator!=(const BodyClipRequest &o) const { return !(*this == o); }
};

// A placed (authoritative) row's request over its PF_ANIM_* fields: the state's clip, blended from
// its source while the target's weight is below 1; a row with no state key but a source plays the
// source at its own tick; with neither, the row's PF_BODY_ANIM_SLOT. The bone build samples the
// body's primary channel [orig: Entity_BuildBoneTransformMatrices @0x4B1290 - the primary channel
// @0x4B1355, AnimChannel_ComputeBoneMatrices called @0x4B1388; AnimChannel_BlendTwoChannels
// @0x410740 the cross-fade].
BodyClipRequest placed_body_clip_request(const float *row);

// The local player's avatar: its primary channel as the motor left it (InfantryBodyPose), the
// coarse slot when its state names no clip.
BodyClipRequest local_body_clip_request(const InfantryBodyPose &pose, int32_t body_anim_slot);

// Play a request on a target (the state keys resolved through infantry_anim_key_ref).
void play_body_clip_request(anim::BodyChannelTarget &target, const BodyClipRequest &request);

// The wire walk's latch for one body: the state and request kind last dispatched, and whether a
// remote transition still ticks (the presenter keeps it per handle across plan rebuilds).
struct WireBodyLatch {
	int32_t anim_state = -2;
	int32_t anim_request = -1;
	int32_t remote_tick = 0;
};

// One wire row's body presentation over `tick_delta` simulation ticks:
//  * a remote-request row (PF_ANIM_REMOTE_REQUEST, a disarmed replica's latest wire state) forwards
//    every raw request to the channel's receive-side arbitration, the pulse (a transition state
//    overwritten within one decode fold) first so the channel sees retail's per-record order, and
//    then ticks an accepted transition once per simulation tick while it runs, the steady state
//    dispatching nothing [orig: NetPacket_SerializePlayerState @0x4C09C0 - the per-record apply
//    @0x4C1153, the arbitration @0x4C11A6; NetPacket_SerializeInfantryEntityState @0x4C0859;
//    AnimMap_UpdateEntity @0x40B5F0 the per-tick blend];
//  * a host-loopback or armed row poses the authority's accepted current state at its tick
//    (blended from its source while the blend runs), its source alone when the state has no clip,
//    else the coarse slot; a row whose compact omits the phase plays its clip free.
// Returns whether the latch was written (the steady remote state writes nothing).
bool present_wire_body(anim::BodyChannelTarget &target, const float *row, int tick_delta,
		WireBodyLatch &latch);

// The upper-body weapon channel a row carries (PF_WPN_*): the hold state's clip at its own tick,
// cross-fading from its source; state -1 is the gate off, which clears the channel's splice
// [orig: the selection Entity_UpdateInfantryPlayerBody @0x4B5DAD, run for every player body
// retail draws; the park AnimChannel_AdvancePlayback @0x40B19E..0x40B1B1].
anim::WeaponChannel row_weapon_channel(const float *row);

// The aim overlay's per-class deltas a body composes: each class's orientation relative to the
// body frame (body^-1 * class), every orientation built from its mission euler degrees through the
// one placement convention (mission::bms_to_presentation_basis), so the body class is identity by
// construction [orig: Entity_BuildBoneTransformMatrices @0x4B1290 - every overlay blends toward
// bodyHeading/bodyPitch; world-wac-ai-re.md §14 (D-INF-11)]. `class_deg` holds nine
// (pitch, yaw, roll) triples in OverlayClass order.
void aim_overlay_deltas(const float body_deg[3], const float class_deg[anim::kOverlayClassCount][3],
		anim::Quat r_deltas[anim::kOverlayClassCount]);
// The same over a row's PF_AIM_* fields; false (nothing written) when the row carries no overlay
// (PF_AIM_OVERLAY_VALID 0).
bool row_aim_overlay_deltas(const float *row, anim::Quat r_deltas[anim::kOverlayClassCount]);

} // namespace opennova::world
