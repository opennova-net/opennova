// A decoded row's AnimMap primary channel: its state half (row_channel_prepare), its playhead
// half (row_channel_advance) and a seated body's channel at its update's head
// (ClientReplicaPipeline::advance_seated_channel). Split out of client_replica_motion.cpp by
// responsibility (the oversize-TU ratchet); the body passes there drive it.
#include <runtime/replication/client_replica_pipeline.h>

#include "client_replica_channel.h"

#include <runtime/anim/clip_timeline.h> // kWrapFadeStep (the wrap's eight-tick fade)
#include <runtime/world/infantry.h>     // IRootMotionSource, AnimVariantRings, the anim tables

#include <cstdint>

namespace opennova::replication {

namespace {

// One served ring entry for a decoded row's channel (world::AnimVariantRings,
// D-NET-196): entry 0 without the shared table.
int32_t row_serve_ring(world::AnimVariantRings *rings, const world::IRootMotionSource &src,
                       const ClientEntityState &es, int state) {
	return rings != nullptr ? rings->serve(&src, es.rm_adm_id, state) : 0;
}

} // namespace

// A decoded row's AnimMap primary channel, its state half: the gait insert, the
// re-init onto `target_state` (the insert's, when one replaces the request) and
// the deferred promotion. Returns whether the promotion released a loop parked on
// its boundary this tick (row_channel_advance treats that as the wrap).
// [orig: AnimMap_UpdateEntity @0x40B5F0]
bool row_channel_prepare(ClientEntityState &es, world::IRootMotionSource &src,
                         world::AnimVariantRings *rings, int &target_state) {
	const int blend_key_state = target_state;
	if (es.net_anim_pending == 0 && es.rm_state >= 0 &&
			es.net_anim_current >= 0 && target_state != es.rm_state) {
		const int trans =
				world::gait_stance_transition_clip(es.rm_state, target_state);
		if (trans >= 0 && src.clip_length_ticks(es.rm_adm_id, trans, 0) >= 0) {
			es.net_anim_pending = static_cast<int16_t>(target_state);
			es.net_anim_pending_boundary = -1;
			es.net_anim_current = static_cast<int16_t>(trans);
			target_state = trans;
		}
	}
	// A channel re-init plays the played state's ring head and advances it, from
	// the heads every body of the .adm shares (the client's own World body too)
	// [orig: AnimMap_UpdateEntity @0x40B737..0x40B778: the head read @0x40B740,
	// table[S] = node+0x24 @0x40B746..0x40B749, slot+0x44 = node @0x40B76F].
	const auto serve = [&](int state) { return row_serve_ring(rings, src, es, state); };
	if (es.rm_state < 0) {
		es.rm_state = static_cast<int16_t>(target_state);
		es.rm_prev_state = static_cast<int16_t>(target_state);
		es.rm_variant = serve(target_state);
		es.rm_prev_variant = es.rm_variant;
		es.rm_phase = (es.cls == EntityClass::Player && es.net_anim_ratio_live)
				? es.net_anim_ratio
				: 0;
		es.net_anim_ratio_live = false;
		es.rm_prev_phase = es.rm_phase;
		es.rm_blend_weight = 1.0f;
		es.rm_blend_step = 0.0f;
		es.rm_prev_bottom_live = false;
	} else if (target_state != es.rm_state) {
		if (es.rm_blend_weight >= 1.0f) {
			es.rm_prev_state = es.rm_state;
			es.rm_prev_phase = es.rm_phase;
			es.rm_prev_variant = es.rm_variant;
		}
		es.rm_state = static_cast<int16_t>(target_state);
		es.rm_variant = serve(target_state);
		es.rm_phase = (es.cls == EntityClass::Player && es.net_anim_ratio_live)
				? es.net_anim_ratio
				: 0;
		es.net_anim_ratio_live = false;
		// A deferred state armed against the previous clip re-arms on the new
		// clip: the park is only ever the playing clip's own wrap.
		es.net_anim_pending_boundary = -1;
		es.rm_blend_weight = 0.0f;
		es.rm_blend_step =
				(world::infantry_anim_flags(blend_key_state) & 0x400u) != 0
						? (1.0f / 15.0f)
						: 0.1f;
	}
	// The clip-end deferred promotion [orig: the deferral arms the channel's
	// end-notify each tick (@0x40b7db/@0x40b7ad), AnimChannel_AdvancePlayback
	// latches it at the next loop wrap / one-shot end (@0x40b1ae/@0x40b18f),
	// and AnimMap promotes on the latched flag (@0x40b795/@0x40b7c3) — the
	// promoted retarget lands on the NEXT tick, as here]. The boundary is
	// armed lazily in the growing-phase convention; a queue behind an
	// already-finished one-shot (or a track-less state) promotes immediately —
	// the shipped hold-wedge safety, recorded inside D-NET-209.
	// The promotion that releases a channel parked on its boundary leaves the
	// loop on its end, so the old channel wraps on that tick (as
	// world::advance_primary_channel's kReleased).
	bool released = false;
	if (es.net_anim_pending != 0) {
		if (es.net_anim_pending_boundary < 0) {
			const int32_t len =
					src.clip_length_ticks(es.rm_adm_id, es.rm_state, es.rm_variant);
			if (len <= 0) {
				es.net_anim_pending_boundary = es.rm_phase;
			} else if (src.clip_loops(es.rm_adm_id, es.rm_state, es.rm_variant)) {
				es.net_anim_pending_boundary = src.clip_boundary_after(
						es.rm_adm_id, es.rm_state, es.rm_phase, es.rm_variant);
			} else {
				es.net_anim_pending_boundary = len;
			}
		}
		if (es.rm_phase >= es.net_anim_pending_boundary) {
			released = es.rm_phase == es.net_anim_pending_boundary;
			es.net_anim_current = es.net_anim_pending;
			es.net_anim_pending = 0;
			es.net_anim_pending_boundary = -1;
			// The promoted request starts at frame zero on its retarget —
			// retail zeroes the +0x377 seed every tick [orig: @0x40b7e4].
			es.net_anim_ratio_live = false;
		}
	}
	return released;
}

// The channel's playhead half: the advance (parked while armed) and an unarmed
// loop wrap's ring serve, or the blend's two halves. False = no track.
// [orig: AnimMap_UpdateEntity @0x40B5F0 (the advance @0x40B7FE)]
bool row_channel_advance(ClientEntityState &es, world::IRootMotionSource &src,
                         world::AnimVariantRings *rings, bool released,
                         world::RootMotionFrame &frame) {
	bool have = false;
	const auto serve = [&](int state) { return row_serve_ring(rings, src, es, state); };
	if (es.rm_blend_weight >= 1.0f) {
		int32_t phase = es.rm_phase;
		const int state = es.rm_state;
		const int32_t variant = es.rm_variant;
		const bool armed = es.net_anim_pending != 0;
		// With a deferral armed, the boundary tick samples the parked clip end
		// (the promote above read the latch first) [orig: AnimChannel_AdvancePlayback
		// @0x40B193..0x40B1B1; AnimMap_UpdateEntity advance @0x40B7FE].
		have = src.advance_armed(es.rm_adm_id, state, variant, phase,
		                         armed ? es.net_anim_pending_boundary : -1, frame);
		es.rm_phase = phase;
		// An unarmed loop that wraps serves its playing state's ring: another entry
		// fades in from its first frame over eight ticks, the same entry plays on;
		// an armed channel parks instead and serves nothing. The wrap tick reads
		// both halves at weight 0: the wrapped entry's motion and extents, the
		// incoming entry's trigger word at t = 0.
		// [orig: AnimChannel_AdvancePlayback @0x40B140 (the park @0x40B19E, the
		//  callback @0x40B1C8) -> AnimMap_AdvanceToNextAnim @0x40BDF0: the serve
		//  @0x40BE02..0x40BE07, the same-entry return @0x40BE09..0x40BE0C, the
		//  eight-tick init @0x40BE13..0x40BE24; world::advance_primary_channel]
		const bool wrapped = released
				? src.clip_loops(es.rm_adm_id, state, variant)
				: !armed && src.clip_wraps_at(es.rm_adm_id, state, variant, es.rm_phase);
		if (have && wrapped) {
			const int32_t served = serve(state);
			if (served != variant) {
				es.rm_prev_state = es.rm_state;
				es.rm_prev_variant = variant;
				es.rm_prev_phase = es.rm_phase;
				es.rm_variant = served;
				es.rm_phase = 0;
				es.rm_blend_weight = 0.0f;
				es.rm_blend_step = anim::kWrapFadeStep;
				int32_t outgoing_phase = es.rm_prev_phase - 1;
				int32_t incoming_phase = -1;
				src.advance_blended(es.rm_adm_id, state, variant, outgoing_phase, state, served,
				                    incoming_phase, es.rm_blend_weight, frame);
			}
		}
	} else {
		es.rm_blend_weight += es.rm_blend_step;
		if (es.rm_blend_weight >= 1.0f) {
			es.rm_blend_weight = 1.0f;
			es.rm_blend_step = 0.0f;
		}
		int32_t pphase = es.rm_prev_phase, tphase = es.rm_phase;
		have = src.advance_blended(es.rm_adm_id, es.rm_prev_state, es.rm_prev_variant, pphase,
		                           es.rm_state, es.rm_variant, tphase, es.rm_blend_weight,
		                           frame);
		es.rm_prev_phase = pphase;
		es.rm_phase = tphase;
	}
	return have;
}

// A seated body's primary channel at its update's head (the dual update), ahead of its chase
// and that chase's starved exit, a player body's and an org1 body's alike: true with this
// tick's frame when the channel has a track. Its capsule is the mounted eye's extent and the
// mounted tail's resolve capsule; the capsule-bottom history takes the frame's bottom, as the
// AnimMap update rewrites it every update. The seated row presents this channel, as every
// armed row does (D-NET-330).
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0 - the dual update @0x4B41DF ahead of the
//  starved exit @0x4B4669..0x4B4670; Entity_UpdateInfantryAI @0x4B9910 - the dual update
//  @0x4B9A48 ahead of the starved exit @0x4B9C09..0x4B9C39; AnimMap_UpdateEntity
//  @0x40B88E..0x40B8A0]
bool ClientReplicaPipeline::advance_seated_channel(ClientEntityState &es,
		world::RootMotionFrame &frame) {
	if (root_motion_ == nullptr || es.rm_adm_id < 0) return false;
	int target_state = es.net_anim_current >= 0
			? static_cast<int>(es.net_anim_current)
			: static_cast<int>(es.anim_state_id);
	const bool bottom_reset_state =
			(target_state >= world::anim_state::kClimbIdle &&
			 target_state <= world::anim_state::kClimbIdle + 3) ||
			(target_state >= world::anim_state::kDeathGrenadeBase &&
			 target_state <= world::anim_state::kDeathGrenadeBase + 3);
	const bool released = row_channel_prepare(es, *root_motion_, anim_rings_, target_state);
	if (bottom_reset_state) es.rm_prev_bottom_live = false;
	const bool have = row_channel_advance(es, *root_motion_, anim_rings_, released, frame);
	if (have) {
		es.rm_prev_bottom = frame.capsule_bottom;
		es.rm_prev_bottom_live = true;
	}
	return have;
}

} // namespace opennova::replication
