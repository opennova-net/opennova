#include "npwire/replay_timeline.h"

#include "npwire/ingame_decode.h"
#include "npwire/ingame_message_id.h"

#include <algorithm>
#include <cmath>
#include <string>
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
bool is_dead_pose_parent(uint16_t parent) {
	return parent != 0xFFFF && (parent & 0xF000) < 0x5000;
}

// One S2C 0x0A compact record -> a ReplaySample. Unmounted records decode to WORLD
// (decompress + the 0x0A header anchor). Mounted records carry a vehicle-LOCAL
// offset (decompress, NO anchor) -> RecKind::Mounted, placed in world later by the
// parent transform (resolve_mounted). RecKind::Skip = class without a compact form.
enum class RecKind { Skip, World, Mounted };
RecKind frame_record_sample(const FrameUpdateRecord &r, int32_t ax, int32_t ay,
                            int32_t az, int frame, ReplaySample &s,
                            uint8_t &flags_out, uint16_t &parent_out) {
	uint16_t cx, cy, cz, parent;
	double yaw_deg;
	switch (r.cls) {
	case EntityClass::Player:
		cx = r.player.pos_x_compressed; cy = r.player.pos_y_compressed;
		cz = r.player.pos_z_compressed; parent = r.player.carrier_handle;
		yaw_deg = bam_byte_to_deg(r.player.yaw_byte);
		flags_out = r.player.state_flags;
		break;
	case EntityClass::Infantry:
		cx = r.infantry.pos_x_compressed; cy = r.infantry.pos_y_compressed;
		cz = r.infantry.pos_z_compressed; parent = r.infantry.vehicle_slot_handle;
		yaw_deg = bam_byte_to_deg(r.infantry.yaw_byte);
		flags_out = r.infantry.flags_byte;
		break;
	case EntityClass::Vehicle:
		cx = r.vehicle.pos_x_compressed; cy = r.vehicle.pos_y_compressed;
		cz = r.vehicle.pos_z_compressed; parent = r.vehicle.parent_slot_handle;
		yaw_deg = bam32_to_deg(uint32_t(int32_t(r.vehicle.euler_z) << 16));
		flags_out = r.vehicle.flags_byte;
		break;
	default:
		return RecKind::Skip;
	}
	parent_out = parent;
	s.frame_index = frame;
	s.heading_deg = yaw_deg;
	s.has_heading = true;
	s.source = ReplaySampleSource::FrameUpdate;
	const int32_t dx = network_decompress_fixedpoint(cx);
	const int32_t dy = network_decompress_fixedpoint(cy);
	const int32_t dz = network_decompress_fixedpoint(cz);
	if (is_dead_pose_parent(parent)) {
		s.x = dx; s.y = dy; s.z = dz; // vehicle-local; world via the parent transform
		return RecKind::Mounted;
	}
	s.x = dx + ax; s.y = dy + ay; s.z = dz + az;
	return RecKind::World;
}

// A deferred mounted record: a rider's vehicle-LOCAL offset + the parent handle, to
// be lifted to world by resolve_mounted once the parent's world track is known.
struct MountedRec {
	uint16_t handle = 0, type = 0;
	int frame = 0;
	int32_t lx = 0, ly = 0, lz = 0; // vehicle-local offset (decompressed)
	uint16_t parent = 0xFFFF;       // pool<<12|slot of the parent (vehicle / weapon mount)
	double yaw_deg = 0.0;           // rider's own (local) heading
	bool has_yaw = false;
	bool dead = false;
	ReplaySampleSource source = ReplaySampleSource::FrameUpdate; // 0x0A or own uplink
};

// The slot-id sentinel that ends a spawn batch (also guards stale/free slots).
bool is_sentinel_slot(uint16_t slot) {
	return slot == 0xFFFF || (slot & 0xF000) >= 0x5000;
}

struct Builder {
	ReplayTimeline tl;
	std::unordered_map<uint16_t, size_t> index; // handle -> entities[] position
	std::vector<MountedRec> mounted;            // deferred riders (vehicle-local)
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

// Flag respawn (teleport) boundaries on every entity's track. A dead->alive
// transition is a SNAP, not motion: the engine relocates the entity and calls
// Entity_ResetToSpawnState @ 0x4B9610 (it never interpolates from the death
// location). The dead state is taken from BOTH wire signals: the per-sample dead
// bit (S2C 0x0A compact flags & 0x02 — brackets the on-screen ragdoll) AND the
// kill stream (S2C 0x26/0x4E -> Entity_KillBySlotId @ 0x42BCE0 — the only signal
// when a dying entity drops out of the 0x0A set entirely). interp/trail rendering
// must not bridge a sample flagged respawn. Idempotent; safe to re-run on a view.
void mark_lifecycle(ReplayTimeline &tl) {
	std::unordered_map<uint16_t, std::vector<int>> kill_frames;
	for (const ReplayEvent &ev : tl.events)
		if (ev.kind == ReplayEventKind::Kill && ev.target != 0xFFFF)
			kill_frames[ev.target].push_back(ev.frame_index);
	for (auto &kv : kill_frames) std::sort(kv.second.begin(), kv.second.end());
	for (ReplayEntity &e : tl.entities) {
		auto it = kill_frames.find(e.handle);
		const std::vector<int> *kf = (it != kill_frames.end()) ? &it->second : nullptr;
		bool dead = false;
		for (size_t i = 0; i < e.track.size(); ++i) {
			ReplaySample &s = e.track[i];
			s.respawn = false;
			// A kill landing after the previous sample but at/before this one took
			// the entity dead in the interim (covers victims whose 0x0A records stop).
			if (kf) {
				const int prev = (i > 0) ? e.track[i - 1].frame_index
				                         : s.frame_index - 1;
				for (int f : *kf)
					if (f > prev && f <= s.frame_index) { dead = true; break; }
			}
			if (s.dead) {
				dead = true;
			} else {
				if (dead && i > 0) s.respawn = true; // dead -> alive = teleport snap
				dead = false;
			}
		}
	}
}

// World pose (i32 16.16 position + heading) of an entity at a capture frame, by
// interpolating its frame-sorted track. Used to resolve a parent's pose for the
// mount transform; the parent must already be world-resolved + sorted.
bool entity_pose_at(const ReplayEntity &e, int frame, int32_t &x, int32_t &y,
                    int32_t &z, double &yaw, bool &has_yaw) {
	const auto &t = e.track;
	if (t.empty()) return false;
	const ReplaySample *r = nullptr;
	if (frame <= t.front().frame_index) {
		r = &t.front();
	} else if (frame >= t.back().frame_index) {
		r = &t.back();
	} else {
		for (size_t i = 1; i < t.size(); ++i) {
			if (t[i].frame_index >= frame) {
				const ReplaySample &a = t[i - 1], &c = t[i];
				const int span = c.frame_index - a.frame_index;
				const double u = span > 0 ? double(frame - a.frame_index) / double(span) : 0.0;
				x = a.x + int32_t((c.x - a.x) * u);
				y = a.y + int32_t((c.y - a.y) * u);
				z = a.z + int32_t((c.z - a.z) * u);
				yaw = (u < 0.5) ? a.heading_deg : c.heading_deg;
				has_yaw = a.has_heading;
				return true;
			}
		}
		return false;
	}
	x = r->x; y = r->y; z = r->z; yaw = r->heading_deg; has_yaw = r->has_heading;
	return true;
}

// degrees -> 32-bit BAM (full circle = 2^32), the angle unit the transform expects.
uint32_t deg_to_bam(double deg) {
	double t = std::fmod(deg, 360.0);
	if (t < 0.0) t += 360.0;
	return uint32_t(int64_t(t / 360.0 * 4294967296.0) & 0xFFFFFFFFll);
}

// Lift every deferred mounted record (b.mounted) into world space. A rider's world
// pose = network_transform_local_to_world(local_offset, parent_world_pose) [orig:
// Entity_TransformLocalToWorld @ 0x43BD00, applied per mounted record by the read
// path]. Mounts NEST (a rider on a weapon mount on a vehicle, a passenger/driver on
// a vehicle), so resolve bottom-up: an entity is world-resolved only once every
// parent it rides is resolved. The parent's unmounted 0x0A record carries only yaw
// on the wire, so pitch/roll feed in as 0 (a wire limitation, not a divergence).
void resolve_mounted(Builder &b) {
	if (b.mounted.empty()) return;
	std::unordered_map<uint16_t, std::vector<size_t>> by_handle;
	for (size_t i = 0; i < b.mounted.size(); ++i)
		by_handle[b.mounted[i].handle].push_back(i);
	// Unmounted entities (no deferred records) start resolved; their tracks are
	// already frame-sorted by the fold.
	std::unordered_map<uint16_t, bool> resolved;
	for (const ReplayEntity &e : b.tl.entities)
		resolved[e.handle] = (by_handle.find(e.handle) == by_handle.end());
	bool progress = true;
	size_t guard = 0;
	while (progress && guard++ <= b.tl.entities.size() + 4) {
		progress = false;
		for (auto &kv : by_handle) {
			const uint16_t h = kv.first;
			if (resolved[h]) continue;
			bool ready = true; // every parent this rider mounts must be resolved first
			for (size_t idx : kv.second) {
				auto pit = b.index.find(b.mounted[idx].parent);
				if (pit == b.index.end() || !resolved[b.mounted[idx].parent]) {
					ready = false;
					break;
				}
			}
			if (!ready) continue;
			ReplayEntity &re = b.tl.entities[b.index[h]];
			for (size_t idx : kv.second) {
				const MountedRec &mr = b.mounted[idx];
				const ReplayEntity &pe = b.tl.entities[b.index[mr.parent]];
				int32_t px, py, pz;
				double pyaw;
				bool phy;
				if (!entity_pose_at(pe, mr.frame, px, py, pz, pyaw, phy)) continue;
				const uint32_t yaw_bam = phy ? deg_to_bam(pyaw) : 0;
				const WorldPose w = network_transform_local_to_world(
				    mr.lx, mr.ly, mr.lz, px, py, pz, yaw_bam, 0, 0);
				ReplaySample s;
				s.frame_index = mr.frame;
				s.x = w.x; s.y = w.y; s.z = w.z;
				if (mr.has_yaw || phy) { // world heading = parent yaw + local yaw
					double hd = std::fmod((phy ? pyaw : 0.0) + mr.yaw_deg, 360.0);
					if (hd < 0.0) hd += 360.0;
					s.heading_deg = hd;
					s.has_heading = true;
				}
				s.source = mr.source;
				s.dead = mr.dead;
				s.mounted = true;
				re.track.push_back(s);
			}
			std::stable_sort(re.track.begin(), re.track.end(),
			                 [](const ReplaySample &a, const ReplaySample &c) {
				                 return a.frame_index < c.frame_index;
			                 });
			resolved[h] = true;
			progress = true;
		}
	}
}

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

		if (m.dir == 'S' && m.tag == s2c::POOL_SPAWN) {
			// Pool-1/3 entity spawn (vehicles, items, objective markers). The
			// 0x01-gated euler_z (entity+16) is the engine yaw heading (32-bit BAM),
			// not velocity (D-NET-86); surface it as the spawn pose facing.
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
				if (r.spawn_flags & 0x0001) {
					s.heading_deg = bam32_to_deg(uint32_t(r.euler_z));
					s.has_heading = true;
				}
				b.set_spawn(e, s, s2c::POOL_SPAWN);
			}
		} else if (m.dir == 'S' && m.tag == s2c::POOL3_SYNC) {
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
				b.set_spawn(e, s, s2c::POOL3_SYNC);
			}
		} else if (m.dir == 'S' && m.tag == s2c::ENTITY_SPAWN_BATCH) {
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
				b.set_spawn(e, s, s2c::ENTITY_SPAWN_BATCH);
			}
		} else if (m.dir == 'S' && m.tag == s2c::STATIC_ENTITY_BATCH) {
			// Pool-2 static-entity batch (armory, oil pump, static decorations).
			// Like 0x20 the handle is synthesized from the slot index; pure
			// statics carry no motion, only the spawn pose.
			StaticEntityBatch batch;
			decode_static_entity_batch(m.payload.data(), m.payload.size(), batch);
			for (size_t i = 0; i < batch.records.size(); ++i) {
				const auto &r = batch.records[i];
				if (r.is_empty_slot) continue;
				const uint16_t slot = uint16_t(batch.start_index + i);
				const uint16_t handle = uint16_t((2u << 12) | (slot & 0x0FFF));
				ReplayEntity &e = b.get(handle, r.item_type_id);
				e.type_id = r.item_type_id;
				if (r.field_flags & 0x0010) {
					e.team = r.team_byte;
					e.team_known = true;
				}
				ReplaySample s;
				s.frame_index = m.frame_index;
				s.x = r.pos_x;
				s.y = r.pos_y;
				s.z = r.pos_z;
				// 0x01-gated euler_z (entity+16) is the static's yaw heading (32-bit
				// BAM), not velocity (D-NET-86) — the spawn pose facing a spectator
				// renders. Without it every static stood at heading 0 (faced east).
				if (r.field_flags & 0x0001) {
					s.heading_deg = bam32_to_deg(uint32_t(r.euler_z));
					s.has_heading = true;
				}
				b.set_spawn(e, s, s2c::STATIC_ENTITY_BATCH);
			}
		} else if (m.dir == 'C' && m.tag == c2s::ENTITY_UPLINK) {
			// Joiner's own-player uplink — the clean per-frame motion source.
			// Only the extended (type-10) sub_op carries world positions.
			EntityPacketSubHeader hdr;
			size_t consumed = 0;
			if (!decode_entity_packet_sub_header(m.payload.data(),
			                                     m.payload.size(), hdr, consumed))
				continue;
			if (hdr.sub_op != ENTITY_SUB_OP_EXTENDED) continue;
			const uint8_t *rest = m.payload.data() + consumed;
			const size_t rest_len = m.payload.size() - consumed;
			PlayerExtendedUplink up;
			size_t used = 0;
			if (!decode_player_extended_uplink(rest, rest_len, up, used)) continue;
			ReplayEntity &e = b.get(hdr.handle, hdr.item_type_id);
			if (e.type_id == 0) e.type_id = hdr.item_type_id;
			e.owner_session = m.session; // this entity is the uplink-sender's own player
			// i16 heading sign-extends << 16 into a 32-bit BAM (§5.10).
			const double up_yaw = bam32_to_deg(uint32_t(int32_t(up.heading) << 16));
			if (is_dead_pose_parent(up.carrier_handle)) {
				// The own-player uplink is vehicle-LOCAL when mounted too (§5.10) —
				// defer to the same parent transform, tagged ClientUplink so the
				// owner's projected view keeps it.
				MountedRec mr;
				mr.handle = hdr.handle; mr.type = hdr.item_type_id;
				mr.frame = m.frame_index;
				mr.lx = up.pos_x; mr.ly = up.pos_y; mr.lz = up.pos_z;
				mr.parent = up.carrier_handle;
				mr.yaw_deg = up_yaw; mr.has_yaw = true;
				mr.source = ReplaySampleSource::ClientUplink;
				b.mounted.push_back(mr);
			} else {
				ReplaySample s;
				s.frame_index = m.frame_index;
				s.x = up.pos_x;
				s.y = up.pos_y;
				s.z = up.pos_z;
				s.heading_deg = up_yaw;
				s.has_heading = true;
				s.source = ReplaySampleSource::ClientUplink;
				e.track.push_back(s);
			}
		} else if (m.dir == 'S' && m.tag == s2c::PER_FRAME_UPDATE) {
			// Per-frame motion for every nearby entity (the host's view). Each
			// compact record's position is compressed against the message's
			// header anchor; unmounted records decode to world here, mounted
			// ones (vehicle-local) are deferred.
			FrameUpdate fu;
			decode_frame_update(m.payload.data(), m.payload.size(), class_of, fu);
			for (const auto &r : fu.records) {
				if (is_sentinel_slot(r.handle)) continue;
				ReplaySample s;
				uint8_t rec_flags = 0;
				uint16_t parent = 0xFFFF;
				const RecKind k = frame_record_sample(r, fu.anchor_x, fu.anchor_y,
				                                      fu.anchor_z, m.frame_index, s,
				                                      rec_flags, parent);
				if (k == RecKind::Skip) continue;
				// flags & 0x02 = the dead/spectator bit the read path gates on
				// [orig: NetPacket_SerializeInfantryEntityState @ 0x4C0320 (flagsByte
				// & 2 branch) / NetPacket_SerializePlayerState @ 0x4C09C0]. mark_lifecycle
				// turns the dead->alive transition into a respawn (no-interp) boundary.
				s.dead = (rec_flags & 0x02) != 0;
				ReplayEntity &e = b.get(r.handle, r.type_id);
				if (e.type_id == 0) e.type_id = r.type_id;
				if (k == RecKind::World) {
					e.track.push_back(s);
				} else { // Mounted — vehicle-local; lifted to world by resolve_mounted
					b.mounted.push_back({r.handle, r.type_id, m.frame_index,
					                     s.x, s.y, s.z, parent, s.heading_deg,
					                     s.has_heading, s.dead});
				}
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
			// Round events (0x0A tag==2): fire ORIGIN world pos = decompress + anchor
			// (the record is a round-fired event, not an impact — §5.9.1/D-NET-152).
			for (const auto &re : fu.round_events) {
				ReplayEvent ev;
				ev.frame_index = m.frame_index;
				ev.kind = ReplayEventKind::Hit;
				ev.has_pos = true;
				ev.x = network_decompress_fixedpoint(re.pos_x_compressed) + fu.anchor_x;
				ev.y = network_decompress_fixedpoint(re.pos_y_compressed) + fu.anchor_y;
				ev.z = network_decompress_fixedpoint(re.pos_z_compressed) + fu.anchor_z;
				ev.target = re.shooter_handle; // the round's owner (ex "target" — misdecoded)
				ev.aux = re.target_handle;     // the shooter's claimed target, iff flags&0x40
				ev.adm_index = re.adm_index;
				ev.sound = true;
				b.tl.events.push_back(std::move(ev));
			}
		} else if (m.dir == 'C' && m.tag == c2s::FIRED_ROUND) {
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
		} else if (m.dir == 'S' && m.tag == s2c::GAME_EVENT) {
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
		} else if (m.dir == 'S' && m.tag == s2c::KILL_SYNC) {
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
		} else if (m.dir == 'S' && m.tag == s2c::KILL_BY_SLOT) {
			// Batch despawn/kill — every listed slot is killed via Entity_KillBySlotId
			// [orig: NapiNPClientMsg_HandleBatchKill @ 0x431870]. Emit one Kill per
			// slot so mark_lifecycle ends each victim's life segment.
			BatchKillBatch bk;
			if (decode_batch_kill(m.payload.data(), m.payload.size(), bk)) {
				for (uint16_t slot : bk.slots) {
					if (is_sentinel_slot(slot)) continue;
					ReplayEvent ev;
					ev.frame_index = m.frame_index;
					ev.kind = ReplayEventKind::Kill;
					ev.target = slot;
					b.tl.events.push_back(std::move(ev));
				}
			}
		} else if (m.dir == 'S' && m.tag == s2c::CAPTURE_ZONE_STATE) {
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
	resolve_mounted(b);   // lift vehicle-local riders into world (parent transform)
	mark_lifecycle(b.tl); // flag death->respawn teleport boundaries (no-interp)
	return b.tl;
}

namespace {

// Project the full timeline to one participant's vantage. Spawn samples are always
// kept; an entity the participant OWNS (its own player) keeps its clean
// ClientUplink track, every other entity keeps the FrameUpdate (received-on-wire)
// track. The host keeps FrameUpdate for everything (the broadcast view). The owned
// uplink track is reconciled to the spawn's WORLD frame (the §5.10 world+origin
// offset the viewer's anchorfix removes) so it is comparable to the others.
ParticipantView project_view(const ReplayTimeline &full, const Participant &who) {
	ParticipantView v;
	v.who = who;
	v.timeline.first_frame = full.first_frame;
	v.timeline.last_frame = full.last_frame;
	v.timeline.events = full.events;            // one wire event/env stream, shared
	v.timeline.environment = full.environment;
	for (const ReplayEntity &src : full.entities) {
		const bool own = !who.is_host && src.owner_session != 0 &&
		                 src.owner_session == who.session;
		int32_t ox = 0, oy = 0, oz = 0;
		if (own && src.has_spawn) {
			for (const ReplaySample &s : src.track)
				if (s.source == ReplaySampleSource::ClientUplink) {
					ox = src.spawn.x - s.x;
					oy = src.spawn.y - s.y;
					oz = src.spawn.z - s.z;
					break;
				}
		}
		ReplayEntity e = src;
		e.track.clear();
		for (const ReplaySample &s : src.track) {
			bool keep;
			if (s.source == ReplaySampleSource::Spawn) keep = true;
			else if (own) keep = (s.source == ReplaySampleSource::ClientUplink);
			else keep = (s.source == ReplaySampleSource::FrameUpdate);
			if (!keep) continue;
			ReplaySample o = s;
			if (own && s.source == ReplaySampleSource::ClientUplink) {
				o.x += ox; o.y += oy; o.z += oz;
			}
			e.track.push_back(o);
		}
		v.timeline.entities.push_back(std::move(e));
	}
	mark_lifecycle(v.timeline); // recompute teleport boundaries on the filtered tracks
	return v;
}

// Linear-interpolate (x,y) in meters at capture frame `f` over `track`; false if
// the track is empty or `f` is outside its span (no extrapolation).
bool interp_pos(const std::vector<ReplaySample> &track, int f, double &x, double &y) {
	if (track.empty()) return false;
	if (f < track.front().frame_index || f > track.back().frame_index) return false;
	for (size_t i = 1; i < track.size(); ++i) {
		if (track[i].frame_index >= f) {
			const ReplaySample &a = track[i - 1];
			const ReplaySample &c = track[i];
			if (c.respawn) {
				// Teleport boundary (death -> respawn): never interpolate across it.
				// Hold the pre-death pose until the snap frame, then jump to `c`.
				const ReplaySample &h = (f < c.frame_index) ? a : c;
				x = h.x / 65536.0;
				y = h.y / 65536.0;
				return true;
			}
			const int span = c.frame_index - a.frame_index;
			const double u = span > 0 ? double(f - a.frame_index) / double(span) : 0.0;
			x = (a.x + (c.x - a.x) * u) / 65536.0;
			y = (a.y + (c.y - a.y) * u) / 65536.0;
			return true;
		}
	}
	x = track.front().x / 65536.0;
	y = track.front().y / 65536.0;
	return true;
}

} // namespace

std::vector<ParticipantView> build_per_participant_world(
    const std::vector<CaptureDatagram> &datagrams,
    const std::function<EntityClass(uint16_t)> &class_of) {
	const std::vector<InGameMessage> msgs = decode_capture_to_messages(datagrams);
	const ReplayTimeline full = build_replay_timeline(msgs, class_of);

	// Roster: the host (authority) + one client per session that OWNS an entity —
	// i.e. sent a C2S 0x0C uplink, an in-game player reporting its own position.
	// Sessions without one (the host's own local client on a listen server, the
	// lobby/gate sessions) are not distinct in-game viewpoints, so they get no view.
	std::vector<int> client_sessions;
	for (const ReplayEntity &e : full.entities) {
		if (e.owner_session == 0) continue;
		if (std::find(client_sessions.begin(), client_sessions.end(),
		              e.owner_session) == client_sessions.end())
			client_sessions.push_back(e.owner_session);
	}

	std::vector<ParticipantView> views;
	Participant host;
	host.is_host = true;
	host.name = "host";
	views.push_back(project_view(full, host));
	for (size_t i = 0; i < client_sessions.size(); ++i) {
		Participant c;
		c.id = int(i + 1);
		c.session = client_sessions[i];
		c.name = "client " + std::to_string(client_sessions[i]);
		views.push_back(project_view(full, c));
	}
	return views;
}

ViewDiff diff_participant_views(const ParticipantView &a, const ParticipantView &b) {
	ViewDiff out;
	std::unordered_map<uint16_t, const ReplayEntity *> bmap;
	for (const auto &e : b.timeline.entities) bmap[e.handle] = &e;
	for (const ReplayEntity &ea : a.timeline.entities) {
		auto it = bmap.find(ea.handle);
		if (it == bmap.end()) continue;
		const ReplayEntity &eb = *it->second;
		EntityDivergence d;
		d.handle = ea.handle;
		double sum = 0.0;
		for (const ReplaySample &s : ea.track) {
			double bx, by;
			if (!interp_pos(eb.track, s.frame_index, bx, by)) continue;
			const double dist = std::hypot(s.x / 65536.0 - bx, s.y / 65536.0 - by);
			++d.compared;
			sum += dist;
			if (dist > d.max_dist) d.max_dist = dist;
		}
		if (d.compared > 0) {
			d.mean_dist = sum / d.compared;
			if (d.max_dist > 1e-6) out.entities.push_back(d); // skip identical-source matches
		}
	}
	return out;
}

} // namespace opennova
