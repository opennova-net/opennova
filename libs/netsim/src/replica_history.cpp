#include "netsim/replica_history.h"

#include "netsim/client_replica_pipeline.h"

#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_id.h>

#include <cmath>
#include <utility>

// Samples the client replica rows the joiner stages per net-re 5.38e — the
// compact reads land on the smooth-target cluster [orig: case-2 staging
// @ 0x4C0FD7..0x4C0FFC] — into a bounded per-handle history; wire handles are
// retail's packed pool<<12|slot ids.

namespace opennova::netsim {
namespace {

double bam32_to_degrees(int32_t bam) {
	return static_cast<double>(static_cast<uint32_t>(bam)) *
	       (360.0 / 4294967296.0);
}

bool is_valid_handle(uint16_t handle) {
	return handle != 0xFFFFu && (handle & 0xF000u) < 0x5000u;
}

ReplaySample sample_from(const ClientEntityState &replica, int frame_index,
		ReplaySampleSource source) {
	ReplaySample sample;
	sample.frame_index = frame_index;
	sample.x = replica.x;
	sample.y = replica.y;
	sample.z = replica.z;
	sample.heading_deg = bam32_to_degrees(replica.heading_bam);
	sample.has_heading = replica.heading_known;
	sample.source = source;
	const uint8_t dead_bit = replica.cls == EntityClass::Vehicle
			? kVehicleFlagDeadPose
			: uint8_t{0x02};
	sample.dead = replica.state_flags_known &&
			(replica.state_flags & dead_bit) != 0;
	sample.mounted = replica.net_seat_valid &&
			is_valid_handle(replica.carrier_handle);
	return sample;
}

} // namespace

void ReplicaHistory::reset() {
	timeline_ = ReplayTimeline{};
	entity_index_.clear();
	compact_revisions_.clear();
	respawn_revisions_.clear();
	spawn_recorded_.clear();
	killed_.clear();
	zone_states_.clear();
	environment_revision_ = 0;
	any_frame_ = false;
	revision_ = 0;
	topology_revision_ = 0;
}

ReplayEntity &ReplicaHistory::entity(uint16_t handle, uint16_t type_id) {
	const auto found = entity_index_.find(handle);
	if (found != entity_index_.end()) return timeline_.entities[found->second];
	ReplayEntity replica;
	replica.handle = handle;
	replica.pool = static_cast<uint8_t>(handle >> 12);
	replica.type_id = type_id;
	entity_index_.emplace(handle, timeline_.entities.size());
	timeline_.entities.push_back(std::move(replica));
	return timeline_.entities.back();
}

void ReplicaHistory::mark_killed(uint16_t handle) {
	if (is_valid_handle(handle)) killed_.insert(handle);
}

void ReplicaHistory::retire_current_generation(uint16_t handle) {
	// Keep the completed ReplayEntity in the timeline, but release every lookup
	// and edge detector keyed by its reusable pool slot. A later spawn at the
	// same handle must append a new generation with its own spawn and compact
	// revision sequence instead of mutating the departed participant's history.
	entity_index_.erase(handle);
	compact_revisions_.erase(handle);
	respawn_revisions_.erase(handle);
	spawn_recorded_.erase(handle);
	killed_.erase(handle);
}

bool ReplicaHistory::capture_environment(
		int frame_index, const ClientReplicaPipeline &pipeline) {
	const ClientEnvironmentState &environment = pipeline.state().environment;
	if (!environment.present || environment.revision == environment_revision_)
		return false;
	environment_revision_ = environment.revision;
	ReplayEnvSample sample;
	sample.frame_index = frame_index;
	sample.fog_dist = environment.fog_dist;
	sample.fog_accel = environment.fog_accel;
	sample.tod_fixed = environment.tod_fixed;
	sample.quake_ticks = environment.quake_ticks;
	sample.cloud_scroll = environment.cloud_scroll;
	sample.overcast = environment.overcast;
	sample.rain_pct = environment.rain_pct;
	sample.env_param = environment.env_param;
	timeline_.environment.push_back(sample);
	return true;
}

bool ReplicaHistory::capture_replica_edges(
		int frame_index, ClientReplicaPipeline &pipeline) {
	bool changed = false;
	for (const ClientEntityState &replica : pipeline.state().entities) {
		ReplayEntity &record = entity(replica.handle, replica.type_id);
		if (record.type_id != replica.type_id || record.name != replica.name ||
				record.team != replica.team ||
				record.team_known != replica.team_known ||
				record.net_id != replica.net_id) {
			record.type_id = replica.type_id;
			record.name = replica.name;
			record.team = replica.team;
			record.team_known = replica.team_known;
			record.net_id = replica.net_id;
			changed = true;
		}
		if (replica.spawn_tag != 0 &&
				spawn_recorded_.insert(replica.handle).second) {
			ReplaySample spawn = sample_from(
					replica, frame_index, ReplaySampleSource::Spawn);
			spawn.dead = false;
			spawn.respawn = false;
			record.spawn_tag = static_cast<char>(replica.spawn_tag);
			record.has_spawn = true;
			record.spawn = spawn;
			record.track.push_back(spawn);
			changed = true;
		}

		uint32_t &seen_compact = compact_revisions_[replica.handle];
		if (replica.compact_revision == seen_compact) continue;
		seen_compact = replica.compact_revision;
		ReplaySample sample = sample_from(
				replica, frame_index, ReplaySampleSource::FrameUpdate);
		uint32_t &seen_respawn = respawn_revisions_[replica.handle];
		if (replica.respawn_revision != seen_respawn) {
			sample.respawn = true;
			seen_respawn = replica.respawn_revision;
		}
		if (!sample.dead && killed_.erase(replica.handle) != 0)
			sample.respawn = !record.track.empty();
		if (sample.dead) killed_.insert(replica.handle);
		record.track.push_back(sample);
		changed = true;
	}
	return changed;
}

bool ReplicaHistory::apply(
		const InGameMessage &message, ClientReplicaPipeline &pipeline) {
	bool changed = false;
	bool topology_changed = false;
	if (!any_frame_) {
		timeline_.first_frame = timeline_.last_frame = message.frame_index;
		any_frame_ = true;
		changed = true;
	} else {
		if (message.frame_index < timeline_.first_frame) {
			timeline_.first_frame = message.frame_index;
			changed = true;
		}
		if (message.frame_index > timeline_.last_frame) {
			timeline_.last_frame = message.frame_index;
			changed = true;
		}
	}

	const uint8_t tag = static_cast<uint8_t>(message.tag & 0xFFu);
	if (message.dir == 'S' && !message.settings_update) {
		std::vector<uint16_t> live_handles_before;
		if (tag == s2c::EMPTY_SLOT_SWEEP) {
			live_handles_before.reserve(pipeline.state().entities.size());
			for (const ClientEntityState &replica : pipeline.state().entities)
				live_handles_before.push_back(replica.handle);
		}
		const std::uint64_t pipeline_topology_before =
				pipeline.topology_revision();
		const std::size_t history_entities_before = timeline_.entities.size();
		pipeline.apply(tag, message.payload);
		for (uint16_t handle : live_handles_before) {
			if (pipeline.state().find(handle) == nullptr)
				retire_current_generation(handle);
		}
		// A replay/spectator view has no local participant, so every addressed
		// Person follows retail's remote 0x49 branch. Consume the one-shot here,
		// where history owns the pipeline pump, and retain its observable dip on
		// the canonical replica instead of silently discarding the notification.
		for (const WeaponReload &reload : pipeline.drain_weapon_reloads()) {
			ClientEntityState *peer = pipeline.state().find(reload.entity_handle);
			if (peer == nullptr ||
					(peer->cls != EntityClass::Player &&
							peer->cls != EntityClass::Infantry))
				continue;
			peer->arms_dip_ticks = 80;
			pipeline.state().mark_changed();
		}
		topology_changed |= pipeline.topology_revision() !=
				pipeline_topology_before;
		changed |= capture_replica_edges(message.frame_index, pipeline);
		topology_changed |= timeline_.entities.size() !=
				history_entities_before;
		changed |= capture_environment(message.frame_index, pipeline);

		if (tag == s2c::PER_FRAME_UPDATE) {
			for (const ClientRoundEvent &round : pipeline.drain_round_events()) {
				ReplayEvent event;
				event.frame_index = message.frame_index;
				event.kind = ReplayEventKind::Hit;
				event.has_pos = true;
				event.x = round.origin_x;
				event.y = round.origin_y;
				event.z = round.origin_z;
				event.target = round.shooter_handle;
				event.aux = round.target_handle;
				event.adm_index = round.adm_index;
				event.sound = true;
				timeline_.events.push_back(std::move(event));
				changed = true;
			}
		} else if (tag == s2c::GAME_EVENT) {
			GameEventRecord decoded;
			size_t consumed = 0;
			if (decode_game_event(message.payload.data(), message.payload.size(),
					decoded, consumed)) {
				ReplayEvent event;
				event.frame_index = message.frame_index;
				event.kind = game_event_kind(decoded.event_type) == GameEventKind::Kill
						? ReplayEventKind::Kill : ReplayEventKind::GameEvent;
				event.event_type = decoded.event_type;
				if (decoded.attacker_index != 0xFF)
					event.source = decoded.attacker_index;
				if (decoded.victim_index != 0xFF) {
					event.target = decoded.victim_index;
					if (event.kind == ReplayEventKind::Kill) mark_killed(event.target);
				}
				if (decoded.aux_index != 0xFF) event.aux = decoded.aux_index;
				if (decoded.pos_x != 0 || decoded.pos_y != 0) {
					event.has_pos = true;
					event.x = static_cast<int32_t>(decoded.pos_x) << 16;
					event.y = static_cast<int32_t>(decoded.pos_y) << 16;
				}
				if (const char *key = game_event_strcnd_key(decoded.event_type))
					event.label = key;
				event.sound = true;
				timeline_.events.push_back(std::move(event));
				changed = true;
			}
		} else if (tag == s2c::KILL_SYNC) {
			KillRecord decoded;
			size_t consumed = 0;
			if (decode_kill_record(message.payload.data(), message.payload.size(),
					decoded, consumed)) {
				ReplayEvent event;
				event.frame_index = message.frame_index;
				event.kind = ReplayEventKind::Kill;
				event.source = decoded.attacker;
				event.target = decoded.victim_slot;
				mark_killed(event.target);
				timeline_.events.push_back(std::move(event));
				changed = true;
			}
		} else if (tag == s2c::KILL_BY_SLOT) {
			BatchKillBatch decoded;
			if (decode_batch_kill(message.payload.data(), message.payload.size(), decoded)) {
				for (uint16_t slot : decoded.slots) {
					if (!is_valid_handle(slot)) continue;
					ReplayEvent event;
					event.frame_index = message.frame_index;
					event.kind = ReplayEventKind::Kill;
					event.target = slot;
					mark_killed(slot);
					timeline_.events.push_back(std::move(event));
					changed = true;
				}
			}
		} else if (tag == s2c::CAPTURE_ZONE_STATE) {
			CaptureZoneOverlayBatch decoded;
			if (decode_capture_zone_overlay(
					message.payload.data(), message.payload.size(), decoded)) {
				for (const CaptureZoneOverlay &zone : decoded.entries) {
					const uint16_t state = static_cast<uint16_t>(
							(static_cast<uint16_t>(zone.icon_color) << 8) |
							zone.flags);
					const auto previous = zone_states_.find(zone.handle);
					if (previous != zone_states_.end() && previous->second == state)
						continue;
					zone_states_[zone.handle] = state;
					ReplayEvent event;
					event.frame_index = message.frame_index;
					event.kind = ReplayEventKind::CaptureZone;
					event.source = zone.handle;
					event.event_type = zone.icon_color;
					event.aux = zone.flags;
					timeline_.events.push_back(std::move(event));
					changed = true;
				}
			}
		}
	} else if (message.dir == 'C' && !message.settings_update &&
			tag == c2s::ENTITY_UPLINK) {
		EntityPacketSubHeader header;
		size_t consumed = 0;
		if (decode_entity_packet_sub_header(message.payload.data(),
				message.payload.size(), header, consumed) &&
				header.sub_op == ENTITY_SUB_OP_EXTENDED) {
			PlayerExtendedUplink uplink;
			size_t used = 0;
			if (decode_player_extended_uplink(message.payload.data() + consumed,
					message.payload.size() - consumed, uplink, used)) {
				const std::size_t history_entities_before = timeline_.entities.size();
				ReplayEntity &record = entity(header.handle, header.item_type_id);
				topology_changed |= timeline_.entities.size() !=
						history_entities_before;
				if (record.type_id == 0) record.type_id = header.item_type_id;
				record.owner_session = message.session;
				ReplaySample sample;
				sample.frame_index = message.frame_index;
				sample.x = uplink.pos_x;
				sample.y = uplink.pos_y;
				sample.z = uplink.pos_z;
				sample.heading_deg = bam32_to_degrees(
						static_cast<int32_t>(
								static_cast<uint32_t>(
										static_cast<uint16_t>(uplink.heading)) << 16));
				sample.has_heading = true;
				sample.source = ReplaySampleSource::ClientUplink;
				if (is_valid_handle(uplink.carrier_handle)) {
					const auto parent = entity_index_.find(uplink.carrier_handle);
					if (parent != entity_index_.end()) {
						const ReplayEntity &carrier = timeline_.entities[parent->second];
						if (!carrier.track.empty()) {
							const ReplaySample &pose = carrier.track.back();
							const WorldPose world = network_transform_local_to_world(
								uplink.pos_x, uplink.pos_y, uplink.pos_z,
								pose.x, pose.y, pose.z,
								static_cast<uint32_t>(
										std::llround(pose.heading_deg /
												360.0 * 4294967296.0)), 0, 0);
							sample.x = world.x;
							sample.y = world.y;
							sample.z = world.z;
							sample.heading_deg = std::fmod(
									pose.heading_deg + sample.heading_deg, 360.0);
							sample.mounted = true;
						}
					}
				}
				record.track.push_back(sample);
				changed = true;
			}
		}
	} else if (message.dir == 'C' && !message.settings_update &&
			tag == c2s::FIRED_ROUND) {
		ClientFiredRound decoded;
		size_t consumed = 0;
		if (decode_client_fired_round(message.payload.data(),
				message.payload.size(), decoded, consumed)) {
			ReplayEvent event;
			event.frame_index = message.frame_index;
			event.kind = ReplayEventKind::Fire;
			event.has_pos = true;
			event.x = decoded.pos_x;
			event.y = decoded.pos_y;
			event.z = decoded.pos_z;
			event.has_dir = true;
			event.dir_x = decoded.dir_x;
			event.dir_y = decoded.dir_y;
			event.source = decoded.shooter_handle;
			event.target = decoded.target_handle;
			event.adm_index = decoded.adm_index;
			event.sound = true;
			timeline_.events.push_back(std::move(event));
			changed = true;
		}
	}
	if (topology_changed) {
		++topology_revision_;
		changed = true;
	}
	if (changed) ++revision_;
	return changed;
}

} // namespace opennova::netsim
