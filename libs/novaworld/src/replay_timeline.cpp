#include "novaworld/replay_timeline.h"

#include "novaworld/ingame_decode.h"

#include <unordered_map>

namespace opennova {
namespace {

// 32-bit BAM (binary angular measure: full circle = 2^32) -> degrees [0,360).
double bam32_to_deg(uint32_t bam) { return double(bam) * 360.0 / 4294967296.0; }

// A high BAM byte (the compact records carry the top 8 bits of a 32-bit BAM).
double bam_byte_to_deg(uint8_t hi) { return bam32_to_deg(uint32_t(hi) << 24); }

// True when a compact record's parent/vehicle handle denotes a real mount (so
// the decompressed position is vehicle-LOCAL and we can't place it yet). 0xFFFF
// and the high-nibble sentinel both mean "unmounted" (the engine adds the anchor).
bool is_mounted_parent(uint16_t parent) {
	return parent != 0xFFFF && (parent & 0xF000) < 0x5000;
}

// Extract a world-frame sample from one S2C 0x0A compact record: decompress the
// three packed positions and add the message anchor (the 0x0A header refs).
// Returns false for mounted records (vehicle-local, deferred) or unknown class.
bool frame_record_world_sample(const FrameUpdateRecord &r, int32_t ax, int32_t ay,
                               int32_t az, int frame, ReplaySample &s) {
	uint16_t cx, cy, cz, parent;
	double yaw_deg;
	switch (r.cls) {
	case EntityClass::Player:
		cx = r.player.pos_x_compressed; cy = r.player.pos_y_compressed;
		cz = r.player.pos_z_compressed; parent = r.player.vehicle_handle;
		yaw_deg = bam_byte_to_deg(r.player.yaw_byte);
		break;
	case EntityClass::Infantry:
		cx = r.infantry.pos_x_compressed; cy = r.infantry.pos_y_compressed;
		cz = r.infantry.pos_z_compressed; parent = r.infantry.vehicle_slot_handle;
		yaw_deg = bam_byte_to_deg(r.infantry.yaw_byte);
		break;
	case EntityClass::Vehicle:
		cx = r.vehicle.pos_x_compressed; cy = r.vehicle.pos_y_compressed;
		cz = r.vehicle.pos_z_compressed; parent = r.vehicle.parent_slot_handle;
		yaw_deg = bam32_to_deg(uint32_t(int32_t(r.vehicle.euler_z) << 16));
		break;
	default:
		return false;
	}
	if (is_mounted_parent(parent)) return false;
	s.frame_index = frame;
	s.x = network_decompress_fixedpoint(cx) + ax;
	s.y = network_decompress_fixedpoint(cy) + ay;
	s.z = network_decompress_fixedpoint(cz) + az;
	s.heading_deg = yaw_deg;
	s.has_heading = true;
	s.source = ReplaySampleSource::FrameUpdate;
	return true;
}

// The slot-id sentinel that ends a spawn batch (also guards stale/free slots).
bool is_sentinel_slot(uint16_t slot) {
	return slot == 0xFFFF || (slot & 0xF000) >= 0x5000;
}

struct Builder {
	ReplayTimeline tl;
	std::unordered_map<uint16_t, size_t> index; // handle -> entities[] position
	bool any_frame = false;

	ReplayEntity &get(uint16_t handle, uint16_t type_id) {
		auto it = index.find(handle);
		if (it != index.end()) return tl.entities[it->second];
		ReplayEntity e;
		e.handle = handle;
		e.pool = uint8_t(handle >> 12);
		e.type_id = type_id;
		index.emplace(handle, tl.entities.size());
		tl.entities.push_back(std::move(e));
		return tl.entities.back();
	}

	void note_frame(int f) {
		if (!any_frame) {
			tl.first_frame = tl.last_frame = f;
			any_frame = true;
		} else {
			if (f < tl.first_frame) tl.first_frame = f;
			if (f > tl.last_frame) tl.last_frame = f;
		}
	}

	// Record a spawn pose: set the authoritative spawn and seed the track.
	void set_spawn(ReplayEntity &e, const ReplaySample &s, char tag) {
		e.spawn_tag = tag;
		e.has_spawn = true;
		e.spawn = s;
		if (e.track.empty()) e.track.push_back(s);
	}
};

} // namespace

ReplayTimeline build_replay_timeline(
    const std::vector<InGameMessage> &messages,
    const std::function<EntityClass(uint16_t)> &class_of) {
	Builder b;
	// Last (icon_color<<8 | flags) seen per capture-zone handle — emit a
	// CaptureZone event only when a zone's state changes (the 0x40 sync repeats).
	std::unordered_map<uint16_t, uint16_t> last_zone_state;
	for (const auto &m : messages) {
		b.note_frame(m.frame_index);

		if (m.dir == 'S' && m.tag == 0x0D) {
			// Pool-1/3 entity spawn (vehicles, items, objective markers). No
			// heading field on the wire here — left unset.
			PoolSpawnBatch batch;
			decode_pool_spawn_batch(m.payload.data(), m.payload.size(), batch);
			for (const auto &r : batch.records) {
				if (is_sentinel_slot(r.slot_id)) continue;
				ReplayEntity &e = b.get(r.slot_id, r.item_type_id);
				e.type_id = r.item_type_id;
				if (!r.entity_name.empty()) e.name = r.entity_name;
				if (r.spawn_flags & 0x0010) {
					e.team = r.team_byte;
					e.team_known = true;
				}
				ReplaySample s;
				s.frame_index = m.frame_index;
				s.x = r.pos_x;
				s.y = r.pos_y;
				s.z = r.pos_z;
				b.set_spawn(e, s, 0x0D);
			}
		} else if (m.dir == 'S' && m.tag == 0x20) {
			// Pool-3 bulk sync (markers/waypoints). Positioned records: the
			// handle is synthesized from the slot index; the marker's authored
			// BMS id rides net_handle.
			Pool3SyncBatch batch;
			decode_pool3_sync_batch(m.payload.data(), m.payload.size(), batch);
			for (size_t i = 0; i < batch.records.size(); ++i) {
				const auto &r = batch.records[i];
				if (r.is_empty_slot) continue;
				const uint16_t slot = uint16_t(batch.start_index + i);
				const uint16_t handle = uint16_t((3u << 12) | (slot & 0x0FFF));
				ReplayEntity &e = b.get(handle, r.item_type_id);
				e.type_id = r.item_type_id;
				e.net_id = r.net_handle;
				if (r.flags_byte & 0x08) {
					e.team = r.team_byte;
					e.team_known = true;
				}
				ReplaySample s;
				s.frame_index = m.frame_index;
				s.x = r.pos_x;
				s.y = r.pos_y;
				s.z = r.pos_z;
				if (r.flags_byte & 0x01) {
					s.heading_deg = bam32_to_deg(r.movement_val);
					s.has_heading = true;
				}
				b.set_spawn(e, s, 0x20);
			}
		} else if (m.dir == 'S' && m.tag == 0x0C) {
			// Pool-0 organic spawn (AI infantry + human players). Carries a
			// 32-bit BAM orientation and an always-present team.
			OrganicSpawnBatch batch;
			decode_organic_spawn_batch(m.payload.data(), m.payload.size(), batch);
			for (const auto &r : batch.records) {
				if (!r.has_body || is_sentinel_slot(r.slot_id)) continue;
				ReplayEntity &e = b.get(r.slot_id, r.item_type_id);
				e.type_id = r.item_type_id;
				if (!r.entity_name.empty()) e.name = r.entity_name;
				e.team = r.team;
				e.team_known = true;
				e.net_id = r.net_id;
				ReplaySample s;
				s.frame_index = m.frame_index;
				s.x = r.pos_x;
				s.y = r.pos_y;
				s.z = r.pos_z;
				s.heading_deg = bam32_to_deg(uint32_t(r.orientation));
				s.has_heading = true;
				b.set_spawn(e, s, 0x0C);
			}
		} else if (m.dir == 'C' && m.tag == 0x0C) {
			// Joiner's own-player uplink — the clean per-frame motion source.
			// Only the extended (type-10) sub_op carries world positions.
			EntityPacketSubHeader hdr;
			size_t consumed = 0;
			if (!decode_entity_packet_sub_header(m.payload.data(),
			                                     m.payload.size(), hdr, consumed))
				continue;
			if (hdr.sub_op != 0x0A) continue;
			const uint8_t *rest = m.payload.data() + consumed;
			const size_t rest_len = m.payload.size() - consumed;
			PlayerExtendedUplink up;
			size_t used = 0;
			if (!decode_player_extended_uplink(rest, rest_len, up, used)) continue;
			ReplayEntity &e = b.get(hdr.handle, hdr.item_type_id);
			if (e.type_id == 0) e.type_id = hdr.item_type_id;
			ReplaySample s;
			s.frame_index = m.frame_index;
			s.x = up.pos_x;
			s.y = up.pos_y;
			s.z = up.pos_z;
			// i16 heading sign-extends << 16 into a 32-bit BAM (§5.10).
			s.heading_deg = bam32_to_deg(uint32_t(int32_t(up.heading) << 16));
			s.has_heading = true;
			s.source = ReplaySampleSource::ClientUplink;
			e.track.push_back(s);
		} else if (m.dir == 'S' && m.tag == 0x0A) {
			// Per-frame motion for every nearby entity (the host's view). Each
			// compact record's position is compressed against the message's
			// header anchor; unmounted records decode to world here, mounted
			// ones (vehicle-local) are deferred.
			FrameUpdate fu;
			decode_frame_update(m.payload.data(), m.payload.size(), class_of, fu);
			for (const auto &r : fu.records) {
				if (is_sentinel_slot(r.handle)) continue;
				ReplaySample s;
				if (!frame_record_world_sample(r, fu.anchor_x, fu.anchor_y,
				                               fu.anchor_z, m.frame_index, s))
					continue;
				ReplayEntity &e = b.get(r.handle, r.type_id);
				if (e.type_id == 0) e.type_id = r.type_id;
				e.track.push_back(s);
			}
			// Environment snapshot (0x0A header sub-block case 2).
			if (fu.env.present) {
				ReplayEnvSample es;
				es.frame_index = m.frame_index;
				es.fog_dist = fu.env.fog_dist;
				es.fog_accel = fu.env.fog_accel;
				es.tod_fixed = fu.env.tod_fixed;
				es.quake_ticks = fu.env.quake_ticks;
				es.cloud_scroll = fu.env.cloud_scroll;
				es.overcast = fu.env.overcast;
				b.tl.environment.push_back(es);
			}
			// Weapon-hit events (0x0A tag==2): impact world pos = decompress + anchor.
			for (const auto &h : fu.hits) {
				ReplayEvent ev;
				ev.frame_index = m.frame_index;
				ev.kind = ReplayEventKind::Hit;
				ev.has_pos = true;
				ev.x = network_decompress_fixedpoint(h.pos_x_compressed) + fu.anchor_x;
				ev.y = network_decompress_fixedpoint(h.pos_y_compressed) + fu.anchor_y;
				ev.z = network_decompress_fixedpoint(h.pos_z_compressed) + fu.anchor_z;
				ev.target = h.target_handle;
				ev.aux = h.weapon_handle;
				ev.adm_index = h.adm_index;
				ev.sound = true;
				b.tl.events.push_back(std::move(ev));
			}
		} else if (m.dir == 'C' && m.tag == 0x06) {
			// Weapon-fire uplink — world origin + direction + shooter (§5.16).
			ClientFiredRound fr;
			size_t consumed = 0;
			if (decode_client_fired_round(m.payload.data(), m.payload.size(),
			                              fr, consumed)) {
				ReplayEvent ev;
				ev.frame_index = m.frame_index;
				ev.kind = ReplayEventKind::Fire;
				ev.has_pos = true; ev.x = fr.pos_x; ev.y = fr.pos_y; ev.z = fr.pos_z;
				ev.has_dir = true; ev.dir_x = fr.dir_x; ev.dir_y = fr.dir_y;
				ev.source = fr.shooter_handle;
				ev.target = fr.target_handle;
				ev.adm_index = fr.adm_index;
				ev.sound = true;
				b.tl.events.push_back(std::move(ev));
			}
		} else if (m.dir == 'S' && m.tag == 0x1E) {
			// Game event — kill feed + objectives. Pool-0 indices ARE pool-0
			// handles ((0<<12)|slot), matching organic spawn slot_ids.
			GameEventRecord ge;
			size_t consumed = 0;
			if (decode_game_event(m.payload.data(), m.payload.size(), ge, consumed)) {
				ReplayEvent ev;
				ev.frame_index = m.frame_index;
				ev.kind = game_event_kind(ge.event_type) == GameEventKind::Kill
				              ? ReplayEventKind::Kill : ReplayEventKind::GameEvent;
				ev.event_type = ge.event_type;
				if (ge.attacker_index != 0xFF) ev.source = ge.attacker_index;
				if (ge.victim_index != 0xFF) ev.target = ge.victim_index;
				if (ge.aux_index != 0xFF) ev.aux = ge.aux_index;
				if (ge.pos_x != 0 || ge.pos_y != 0) {
					ev.has_pos = true;
					ev.x = int32_t(ge.pos_x) << 16;
					ev.y = int32_t(ge.pos_y) << 16;
				}
				if (const char *key = game_event_strcnd_key(ge.event_type))
					ev.label = key;
				ev.sound = true;
				b.tl.events.push_back(std::move(ev));
			}
		} else if (m.dir == 'S' && m.tag == 0x26) {
			// Direct entity-death replication.
			KillRecord kr;
			size_t consumed = 0;
			if (decode_kill_record(m.payload.data(), m.payload.size(), kr, consumed)) {
				ReplayEvent ev;
				ev.frame_index = m.frame_index;
				ev.kind = ReplayEventKind::Kill;
				ev.source = kr.attacker;
				ev.target = kr.victim_slot;
				b.tl.events.push_back(std::move(ev));
			}
		} else if (m.dir == 'S' && m.tag == 0x40) {
			// Capture-zone state — emit only on change (the 0x40 sync repeats).
			CaptureZoneOverlayBatch cz;
			if (decode_capture_zone_overlay(m.payload.data(), m.payload.size(), cz)) {
				for (const auto &z : cz.entries) {
					const uint16_t state = uint16_t((z.icon_color << 8) | z.flags);
					auto it = last_zone_state.find(z.handle);
					if (it != last_zone_state.end() && it->second == state) continue;
					last_zone_state[z.handle] = state;
					ReplayEvent ev;
					ev.frame_index = m.frame_index;
					ev.kind = ReplayEventKind::CaptureZone;
					ev.source = z.handle;
					ev.event_type = z.icon_color;
					ev.aux = z.flags;
					b.tl.events.push_back(std::move(ev));
				}
			}
		}
	}
	return b.tl;
}

} // namespace opennova
