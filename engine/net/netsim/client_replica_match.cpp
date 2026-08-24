#include "netsim/client_replica_pipeline.h"

#include <npwire/wire_handle.h>
#include <world/entity.h>

#include <utility>

namespace opennova::netsim {

// Shared 0x13/0x26 fold: the retail handler gates (not the 0xFFFF sentinel,
// pool nibble < 5, slot < that pool's capacity), zeroes row health, and
// surfaces one record for the embedding sim's class death callback.
// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 / Entity_KillBySlotId @0x42BCE0]
void ClientReplicaPipeline::apply_entity_death(uint16_t handle_packed,
		int16_t killer_source) {
	const world::EntityHandle handle{handle_packed};
	if (handle_packed == wire_handle::kInvalid ||
			handle.pool() >= world::kEntityPoolCount ||
			static_cast<std::size_t>(handle.slot()) >=
					world::retail_pool_capacity(handle.pool()))
		return;
	if (ClientEntityState *row = state_.find(handle_packed)) {
		row->health_word = 0;
		row->health_known = true;
		state_.mark_changed();
	}
	EntityDeathRecord death;
	death.entity_handle = handle_packed;
	death.killer_source = killer_source;
	pending_entity_deaths_.push_back(death);
}

void ClientReplicaPipeline::apply_death_camera_target(
		const std::vector<uint8_t> &body) {
	DeathCameraTarget target;
	size_t consumed = 0;
	if (!decode_death_camera_target(
			body.data(), body.size(), target, consumed) ||
			consumed != body.size()) {
		++malformed_bodies_;
		return;
	}
	state_.death_camera.known = true;
	state_.death_camera.x = target.x;
	state_.death_camera.y = target.y;
	state_.death_camera.z = target.z;
	++state_.death_camera.updates;
	state_.mark_changed();
}

void ClientReplicaPipeline::apply_player_downed_state(
		const std::vector<uint8_t> &body) {
	PlayerDownedState downed;
	size_t consumed = 0;
	if (!decode_player_downed_state(
			body.data(), body.size(), downed, consumed) ||
			consumed != body.size()) {
		++malformed_bodies_;
		return;
	}
	const world::EntityHandle handle{downed.entity_handle};
	if (!handle.valid() || handle.pool() != 0) return;
	for (ClientRosterSlot &slot : state_.roster) {
		if (!slot.bound || slot.entity_slot != handle.slot()) continue;
		const bool changed =
				slot.downed_revive_seconds != downed.revive_seconds ||
				slot.medic_request_active != downed.medic_request_active;
		slot.downed_revive_seconds = downed.revive_seconds;
		slot.medic_request_active = downed.medic_request_active;
		if (changed) state_.mark_changed();
		return;
	}
}

void ClientReplicaPipeline::apply_spawn_wave_status(
		const std::vector<uint8_t> &body) {
	SpawnWaveStatus status;
	if (!decode_spawn_wave_status(body.data(), body.size(), status)) {
		++malformed_bodies_;
		return;
	}
	state_.spawn_waves.known = true;
	// word_A85BC0: -1 at every fold, then the zone handle of the group whose
	// member list names the local player [orig: @0x4298f6 reset;
	// @0x429a04..0x429a0b the match]. The viewer handle is the local one.
	state_.spawn_waves.self_zone_handle = 0xFFFF;
	for (const SpawnWaveGroup &group : status.groups) {
		for (uint16_t member : group.members) {
			if (member == viewer_handle_)
				state_.spawn_waves.self_zone_handle = group.zone_handle;
		}
	}
	state_.spawn_waves.value = std::move(status);
	++state_.spawn_waves.updates;
	state_.mark_changed();
}

void ClientReplicaPipeline::apply_score_delta_sound(
		const std::vector<uint8_t> &body) {
	ScoreDeltaSound sample;
	if (!decode_score_delta_sound(body.data(), body.size(), sample)) {
		++malformed_bodies_;
		return;
	}
	ClientScoreFeedback &feedback = state_.score_feedback;
	if (sample.score == feedback.score) return;
	feedback.delta = static_cast<int32_t>(
			static_cast<uint32_t>(sample.score) -
			static_cast<uint32_t>(feedback.score));
	feedback.score = sample.score;
	++feedback.updates;
	state_.mark_changed();
}

} // namespace opennova::netsim
