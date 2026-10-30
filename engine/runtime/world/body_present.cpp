#include "body_present.h"

#include <runtime/mission/placement_traits.h>
#include <runtime/world/present_rows.h>

namespace opennova::world {

namespace {

inline int32_t row_i(const float *row, int field) {
	return static_cast<int32_t>(row[field]);
}

// Column c of a placement basis, as three floats.
inline void basis_column(const mission::PlacementBasis &b, int c, float r_out[3]) {
	const mission::PlacementVec3 &v = c == 0 ? b.x : (c == 1 ? b.y : b.z);
	r_out[0] = v.x;
	r_out[1] = v.y;
	r_out[2] = v.z;
}

} // namespace

BodyClipRequest placed_body_clip_request(const float *row) {
	BodyClipRequest request;
	const int32_t state = row_i(row, PF_ANIM_STATE);
	const int32_t source_state = row_i(row, PF_ANIM_SOURCE_STATE);
	const std::string &key = infantry_anim_key_ref(state);
	const bool source_keyed = source_state >= 0 && !infantry_anim_key_ref(source_state).empty();
	if (!key.empty()) {
		request.selector = state;
		request.phase = row_i(row, PF_ANIM_PHASE_TICKS);
		request.variant = row_i(row, PF_ANIM_VARIANT);
		const float weight = row[PF_ANIM_BLEND_WEIGHT];
		if (source_keyed && weight < 1.0f) {
			request.kind = BodyClipRequest::Kind::kBlendAt;
			request.source_state = source_state;
			request.source_phase = row_i(row, PF_ANIM_SOURCE_PHASE_TICKS);
			request.source_variant = row_i(row, PF_ANIM_SOURCE_VARIANT);
			request.weight = weight;
		} else {
			request.kind = BodyClipRequest::Kind::kClipAt;
			request.parked = row_i(row, PF_ANIM_PHASE_PARKED) != 0;
		}
		return request;
	}
	if (source_keyed) {
		request.kind = BodyClipRequest::Kind::kClipAt;
		request.selector = source_state;
		request.phase = row_i(row, PF_ANIM_SOURCE_PHASE_TICKS);
		request.variant = row_i(row, PF_ANIM_SOURCE_VARIANT);
		return request;
	}
	const int32_t slot = row_i(row, PF_BODY_ANIM_SLOT);
	if (slot >= 0) {
		request.kind = BodyClipRequest::Kind::kSlotAt;
		request.selector = slot;
		request.phase = row_i(row, PF_ANIM_PHASE_TICKS);
		request.parked = row_i(row, PF_ANIM_PHASE_PARKED) != 0;
	}
	return request;
}

BodyClipRequest local_body_clip_request(const InfantryBodyPose &pose, int32_t body_anim_slot) {
	BodyClipRequest request;
	request.phase = pose.phase;
	if (!infantry_anim_key_ref(pose.state).empty()) {
		request.selector = pose.state;
		request.variant = pose.variant;
		if (pose.blending && pose.weight < 1.0f && !infantry_anim_key_ref(pose.source_state).empty()) {
			request.kind = BodyClipRequest::Kind::kBlendAt;
			request.source_state = pose.source_state;
			request.source_phase = pose.source_phase;
			request.source_variant = pose.source_variant;
			request.weight = pose.weight;
		} else {
			request.kind = BodyClipRequest::Kind::kClipAt;
			request.parked = pose.parked;
		}
		return request;
	}
	request.kind = BodyClipRequest::Kind::kSlotAt;
	request.selector = body_anim_slot;
	request.parked = pose.parked;
	return request;
}

void play_body_clip_request(anim::BodyChannelTarget &target, const BodyClipRequest &request) {
	switch (request.kind) {
		case BodyClipRequest::Kind::kClipAt:
			target.play_at(infantry_anim_key_ref(request.selector), request.phase, request.variant,
					request.parked);
			return;
		case BodyClipRequest::Kind::kBlendAt:
			target.play_blend_at(infantry_anim_key_ref(request.source_state), request.source_phase,
					infantry_anim_key_ref(request.selector), request.phase, request.weight,
					request.source_variant, request.variant);
			return;
		case BodyClipRequest::Kind::kSlotAt:
			if (request.selector >= 0) target.play_slot_at(request.selector, request.phase, request.parked);
			return;
		case BodyClipRequest::Kind::kNone:
			return;
	}
}

bool present_wire_body(anim::BodyChannelTarget &target, const float *row, int tick_delta,
		WireBodyLatch &latch) {
	const int32_t anim_state = row_i(row, PF_ANIM_STATE);
	const int32_t request_kind = row_i(row, PF_ANIM_REMOTE_REQUEST);
	const int32_t pulse = row_i(row, PF_ANIM_STATE_PULSE);
	const bool unchanged_remote = request_kind != 0 && pulse < 0 && anim_state == latch.anim_state &&
			request_kind == latch.anim_request;
	// An accepted or queued remote transition advances exactly once per simulation tick; the
	// latch is set only while the channel reports one in flight, so a steady row dispatches
	// nothing.
	if (latch.remote_tick != 0 && unchanged_remote) {
		int ticks_left = tick_delta;
		while (ticks_left > 0 && latch.remote_tick != 0) {
			latch.remote_tick = target.remote_tick(anim_state) ? 1 : 0;
			--ticks_left;
		}
		latch.anim_state = anim_state;
		latch.anim_request = request_kind;
		return true;
	}
	if (unchanged_remote) return false;
	latch.anim_state = anim_state;
	latch.anim_request = request_kind;
	const int32_t phase = row_i(row, PF_ANIM_PHASE_TICKS);
	const bool remote = request_kind != 0;
	// A transition state that arrived and was overwritten within one decode fold dispatches
	// first, so the channel's arbitration sees the records in their order [orig:
	// NetPacket_SerializePlayerState @0x4C09C0 - the per-record remote anim apply @0x4C1153].
	if (remote && pulse >= 0) {
		const std::string &pulse_key = infantry_anim_key_ref(pulse);
		if (!pulse_key.empty())
			target.remote_request(pulse, pulse_key, infantry_anim_flags(pulse),
					row_i(row, PF_ANIM_PULSE_TICKS));
	}
	const std::string &key = infantry_anim_key_ref(anim_state);
	if (!key.empty()) {
		if (remote) {
			latch.remote_tick = target.remote_request(anim_state, key, infantry_anim_flags(anim_state),
					phase) ? 1 : 0;
			return true;
		}
		// A host-loopback row carries the authority's already-accepted current state and its
		// playhead: pose it directly, each channel on its served ring entry.
		if (phase >= 0) {
			const int32_t source_state = row_i(row, PF_ANIM_SOURCE_STATE);
			const std::string &source_key = infantry_anim_key_ref(source_state);
			const float weight = row[PF_ANIM_BLEND_WEIGHT];
			const int32_t variant = row_i(row, PF_ANIM_VARIANT);
			if (!source_key.empty() && weight < 1.0f) {
				target.play_blend_at(source_key, row_i(row, PF_ANIM_SOURCE_PHASE_TICKS), key, phase,
						weight, row_i(row, PF_ANIM_SOURCE_VARIANT), variant);
			} else {
				target.play_at(key, phase, variant, row_i(row, PF_ANIM_PHASE_PARKED) != 0);
			}
			return true;
		}
		target.play_free(key);
		return true;
	}
	if (!remote) {
		const int32_t source_state = row_i(row, PF_ANIM_SOURCE_STATE);
		const std::string &source_key = infantry_anim_key_ref(source_state);
		if (!source_key.empty()) {
			target.play_at(source_key, row_i(row, PF_ANIM_SOURCE_PHASE_TICKS),
					row_i(row, PF_ANIM_SOURCE_VARIANT), false);
			return true;
		}
	}
	const int32_t slot = row_i(row, PF_BODY_ANIM_SLOT);
	if (slot < 0) return true;
	if (phase >= 0) {
		target.play_slot_at(slot, phase, row_i(row, PF_ANIM_PHASE_PARKED) != 0);
	} else {
		target.play_slot(slot);
	}
	return true;
}

anim::WeaponChannel row_weapon_channel(const float *row) {
	anim::WeaponChannel channel;
	channel.key = infantry_anim_key_ref(row_i(row, PF_WPN_ANIM_STATE));
	channel.phase_ticks = row_i(row, PF_WPN_PHASE_TICKS);
	channel.prev_key = infantry_anim_key_ref(row_i(row, PF_WPN_SOURCE_STATE));
	channel.prev_phase_ticks = row_i(row, PF_WPN_SOURCE_PHASE_TICKS);
	channel.blend_weight = row[PF_WPN_BLEND_WEIGHT];
	channel.variant = row_i(row, PF_WPN_VARIANT);
	channel.prev_variant = row_i(row, PF_WPN_SOURCE_VARIANT);
	channel.parked = row_i(row, PF_WPN_PHASE_PARKED) != 0;
	return channel;
}

void aim_overlay_deltas(const float body_deg[3], const float class_deg[anim::kOverlayClassCount][3],
		anim::Quat r_deltas[anim::kOverlayClassCount]) {
	const mission::PlacementBasis body =
			mission::bms_to_presentation_basis(body_deg[0], body_deg[1], body_deg[2]);
	float body_columns[3][3];
	for (int c = 0; c < 3; ++c) basis_column(body, c, body_columns[c]);
	for (size_t k = 0; k < anim::kOverlayClassCount; ++k) {
		const mission::PlacementBasis cls =
				mission::bms_to_presentation_basis(class_deg[k][0], class_deg[k][1], class_deg[k][2]);
		float class_columns[3][3];
		for (int c = 0; c < 3; ++c) basis_column(cls, c, class_columns[c]);
		// body^T * class (the inverse of a rotation), row-major: element (r, c) is the dot of the
		// body's column r with the class's column c.
		float rows[9];
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				rows[r * 3 + c] = body_columns[r][0] * class_columns[c][0] +
						body_columns[r][1] * class_columns[c][1] +
						body_columns[r][2] * class_columns[c][2];
		r_deltas[k] = anim::mat3_to_quat(rows);
	}
}

bool row_aim_overlay_deltas(const float *row, anim::Quat r_deltas[anim::kOverlayClassCount]) {
	if (row_i(row, PF_AIM_OVERLAY_VALID) == 0) return false;
	const float body[3] = {row[PF_AIM_BODY_PITCH_DEG], row[PF_AIM_BODY_YAW_DEG],
			row[PF_AIM_BODY_ROLL_DEG]};
	float classes[anim::kOverlayClassCount][3];
	for (size_t k = 0; k < anim::kOverlayClassCount; ++k) {
		const int offset = PF_AIM_ANGLES + static_cast<int>(k) * PF_AIM_CLASS_STRIDE;
		classes[k][0] = row[offset];
		classes[k][1] = row[offset + 1];
		classes[k][2] = row[offset + 2];
	}
	aim_overlay_deltas(body, classes, r_deltas);
	return true;
}

} // namespace opennova::world
