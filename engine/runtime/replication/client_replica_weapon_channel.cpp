// The decoded player rows' secondary (upper-body weapon) AnimMap channel: the
// per-tick advance and the 16-tick selection retail's client runs for every
// player body, and the S2C 0x2D emote stamp. The authority's twin is
// world/infantry_weapon_channel.cpp (AiSystem::infantry_weapon_channel*); a
// replica row has no motor entity, so the channel lives on ClientEntityState
// and plays the replica's variant-0 track (D-NET-196), with the replica
// primary's deferred arm (client_replica_pipeline.cpp row_root_motion_tick).
// Witness: world-wac-ai-re.md §14.8.

#include <runtime/replication/client_replica_pipeline.h>

#include <runtime/world/infantry.h>
#include <runtime/world/weapon_table.h>

namespace opennova::replication {

namespace {

// The primary state the hold ladder mirrors: the arbitrated current the row's
// primary channel chases, else the raw wire byte.
int row_primary_state(const ClientEntityState &es) {
	return es.net_anim_current >= 0 ? static_cast<int>(es.net_anim_current)
	                                 : static_cast<int>(es.anim_state_id);
}

// The hold ladder off the row's wire inputs: the held record's special_hold by
// the row's own ADM index, the scope and binocular Flags bits; a pure client
// never derives the remote reload window. [orig: Entity_UpdateInfantryPlayerBody
// @0x4b5dad..0x4b5e6f, the kind read @0x4b5dba]
int row_desired_state(const ClientEntityState &es, const world::WeaponTable *weapons) {
	int hold_kind = 0;
	if (weapons != nullptr)
		if (const world::WeaponTableEntry *held = weapons->by_index(es.equipped_adm_index))
			hold_kind = held->special_hold;
	return world::infantry_weapon_hold_state(hold_kind, row_primary_state(es),
			(es.state_flags & world::kEntityFlagScopeRaised) != 0,
			(es.state_flags & world::kEntityFlagBinoculars) != 0, /*reloading=*/false);
}

void row_weapon_channel_tick(ClientEntityState &es, world::IRootMotionSource &src,
		const world::WeaponTable *weapons, uint32_t tick) {
	const int adm = es.rm_adm_id;
	if (es.wpn_playing < 0) {
		// First armed tick: the channel starts settled on its target (the
		// ladder's state, or a stamp that landed first).
		if (es.wpn_state < 0) es.wpn_state = static_cast<int16_t>(row_desired_state(es, weapons));
		es.wpn_playing = es.wpn_prev = es.wpn_state;
		es.wpn_phase = es.wpn_prev_phase = 0;
		es.wpn_deferred_boundary = -1;
		es.wpn_blend_weight = 1.0f;
		es.wpn_blend_step = 0.0f;
	}
	// The advance [orig: AnimMap_UpdateDualChannels @0x40b8c0 -> the shared
	// AnimMap_UpdateEntity @0x40b5f0]: a target change re-inits the channel
	// (blend 10, 15 on flag 0x400), a forward gait's stance change first plays
	// its transition clip and defers the real target [orig: @0x40b662..0x40b737].
	if (es.wpn_state != es.wpn_playing) {
		int played = es.wpn_state;
		if (es.wpn_deferred == 0) {
			const int inserted = world::gait_stance_transition_clip(es.wpn_playing, es.wpn_state);
			if (inserted >= 0 && src.has_clip(adm, inserted)) {
				es.wpn_deferred = es.wpn_state;
				played = inserted;
			}
		}
		if (es.wpn_blend_weight >= 1.0f) {
			es.wpn_prev = es.wpn_playing;
			es.wpn_prev_phase = es.wpn_phase;
		}
		es.wpn_blend_step = (world::infantry_anim_flags(es.wpn_state) & 0x400u) != 0
				? 1.0f / 15.0f : 0.1f;
		es.wpn_state = static_cast<int16_t>(played);
		es.wpn_playing = static_cast<int16_t>(played);
		es.wpn_phase = 0;
		es.wpn_deferred_boundary = -1;
		es.wpn_blend_weight = 0.0f;
	}
	// The clip-end deferred promotion, the replica primary's arm: a loop
	// promotes at its next wrap, a one-shot at its end, a trackless state at
	// once [orig: the end-notify arm @0x40B7B3 / @0x40B7E1, the promotion on
	// 0x20000 @0x40B793 / @0x40B7C1].
	if (es.wpn_deferred != 0) {
		if (es.wpn_deferred_boundary < 0) {
			const int32_t len = src.clip_length_ticks(adm, es.wpn_playing, 0);
			es.wpn_deferred_boundary = len <= 0 ? es.wpn_phase
					: src.clip_loops(adm, es.wpn_playing, 0)
							? src.clip_boundary_after(adm, es.wpn_playing, es.wpn_phase)
							: len;
		}
		if (es.wpn_phase >= es.wpn_deferred_boundary) {
			es.wpn_state = es.wpn_deferred;
			es.wpn_deferred = 0;
			es.wpn_deferred_boundary = -1;
		}
	}
	// The playhead; the weapon layer's root output is discarded [orig:
	// parentEntity = 0 @0x40b8f3].
	world::RootMotionFrame discard;
	if (es.wpn_blend_weight >= 1.0f) {
		int32_t phase = es.wpn_phase;
		src.advance_armed(adm, es.wpn_playing, 0, phase,
				es.wpn_deferred != 0 ? es.wpn_deferred_boundary : -1, discard);
		es.wpn_phase = phase;
	} else {
		es.wpn_blend_weight += es.wpn_blend_step;
		if (es.wpn_blend_weight >= 1.0f) {
			es.wpn_blend_weight = 1.0f;
			es.wpn_blend_step = 0.0f;
		}
		int32_t phase = es.wpn_phase;
		int32_t prev_phase = es.wpn_prev_phase;
		src.advance_variant(adm, es.wpn_playing, 0, phase, discard);
		src.advance_variant(adm, es.wpn_prev, 0, prev_phase, discard);
		es.wpn_phase = phase;
		es.wpn_prev_phase = prev_phase;
	}
	// The selection + commit, on the 16-tick slow pass keyed on the raw tick:
	// the same state skips; a locked (flag 4) or emote (0x20) current defers
	// the change to its clip end; else the target changes now.
	// [orig: gate @0x4b5d71; commit @0x4b5e72..0x4b5ea3]
	if ((tick & 0xFu) == 0u) {
		const int desired = row_desired_state(es, weapons);
		if (desired != es.wpn_state) {
			if ((world::infantry_anim_flags(es.wpn_state) & 0x24u) != 0) {
				es.wpn_deferred = static_cast<int16_t>(desired);
			} else {
				es.wpn_state = static_cast<int16_t>(desired);
				es.wpn_deferred = 0;
			}
		}
	}
}

} // namespace

void ClientReplicaPipeline::tick_row_weapon_channels(uint16_t self_handle, uint32_t tick) {
	if (root_motion_ == nullptr) return;
	for (ClientEntityState &es : state_.entities) {
		// The own row presents through the local player's world body.
		if (es.cls != EntityClass::Player || es.handle == self_handle) continue;
		if (es.rm_adm_id < 0) {
			es.wpn_state = es.wpn_playing = -1;
			es.wpn_deferred = 0;
			continue;
		}
		row_weapon_channel_tick(es, *root_motion_, weapon_table_, tick);
	}
}

bool ClientReplicaPipeline::stamp_row_emote(uint16_t handle, uint8_t emote) {
	// [orig: NapiNPClientMsg_HandleEmote @0x427efb..0x427f18 — the authored
	//  slot test on the speaker's map, then +0x2C8 = 114 + emote, +0x2C4 = 0]
	ClientEntityState *es = state_.find(handle);
	if (es == nullptr || es->cls != EntityClass::Player || es->rm_adm_id < 0 ||
			root_motion_ == nullptr)
		return false;
	const int state = world::kEmoteAnimStateBase + emote;
	if (!root_motion_->has_clip(es->rm_adm_id, state)) return false;
	es->wpn_state = static_cast<int16_t>(state);
	es->wpn_deferred = 0;
	return true;
}

} // namespace opennova::replication
