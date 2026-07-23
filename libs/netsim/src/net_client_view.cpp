#include "netsim/net_client_view.h"

#include "netsim/entity_wire_bridge.h" // class_for_type_id (default resolver)
#include "netsim/connection_fan.h"     // kTag0aFrameUpdate
#include <io/bam.h>                      // wrapped retail pitch chase

#include <algorithm>

namespace opennova::netsim {

// ---- ClientState lookup -----------------------------------------------------

ClientEntityState *ClientState::find(uint16_t handle) {
	// `entities` is intentionally public decoded state. Callers may clear,
	// reorder, append, or edit it directly, so a separate handle-to-index cache
	// cannot remain valid without changing that API. Keep lookup derived from
	// the authoritative vector.
	for (ClientEntityState &entity : entities) {
		if (entity.handle == handle) return &entity;
	}
	return nullptr;
}

ClientEntityState &ClientState::upsert(uint16_t handle) {
	if (ClientEntityState *e = find(handle)) return *e;
	ClientEntityState e;
	e.handle = handle;
	entities.push_back(e);
	return entities.back();
}

// ---- NetClientView ----------------------------------------------------------

NetClientView::NetClientView()
		: resolver_([](uint16_t tid) { return class_for_type_id(tid); }) {}

NetClientView::NetClientView(std::function<EntityClass(uint16_t)> resolver)
		: resolver_(std::move(resolver)) {}

void NetClientView::set_item_class_resolver(std::function<EntityClass(uint16_t)> resolver) {
	item_resolver_ = std::move(resolver);
}

EntityClass NetClientView::classify(uint16_t type_id) const {
	// items.def first — the retail client's own dispatch source [orig: itemDef+356
	// @0x50f2e2]. It must outrank the 0x0D pool blanket: pool-1 holds no-callback
	// types too (an `ewep` emplacement), and sizing their header-only records as a
	// vehicle compact desyncs the whole frame after them.
	if (item_resolver_) {
		const EntityClass cls = item_resolver_(type_id);
		if (cls != EntityClass::Unknown) return cls;
	}
	const auto it = learned_classes_.find(type_id);
	if (it != learned_classes_.end()) return it->second;
	return resolver_(type_id);
}

void NetClientView::apply(uint8_t tag, const std::vector<uint8_t> &body) {
	switch (tag) {
	case 0x08: { // fixed session config; field 3 = shared g_GameType
		SessionConfig config;
		if (decode_session_config(body.data(), body.size(), config))
			game_type_ = static_cast<uint32_t>(config.fields[3]);
		else
			++unknown_tags_;
		break;
	}
	case 0x7B: { // full player/session info; extra = shared g_GameType
		FullPlayerInfo info;
		if (decode_full_player_info(body.data(), body.size(), info))
			game_type_ = info.extra;
		else
			++unknown_tags_;
		break;
	}
	case kTag0aFrameUpdate:
		apply_frame_update(body);
		break;
	case 0x49: { // weapon reload echo (same four-byte body as C2S 0x25)
		WeaponReload reload;
		size_t consumed = 0;
		if (decode_weapon_reload(body.data(), body.size(), reload, consumed) &&
		    consumed == body.size())
			pending_weapon_reloads_.push_back(reload);
		else
			++unknown_tags_;
		break;
	}
	case 0x0C: // pool-0 organic spawn batch (§5.23)
		apply_organic_spawn(body);
		break;
	case 0x0D: // pool-1 entity spawn batch (§5.11)
		apply_pool_spawn(body);
		break;
	case 0x10: // pool-2 static entity batch (§5.9)
		apply_static_batch(body);
		break;
	case 0x20: // pool-3 marker/waypoint sync batch (§5.12)
		apply_pool3_batch(body);
		break;
	default:
		// Game-start scalars / world-state-load and other non-entity tags.
		++unknown_tags_;
		break;
	}
}

std::vector<ClientRoundEvent> NetClientView::drain_round_events() {
	std::vector<ClientRoundEvent> out;
	out.swap(pending_round_events_);
	return out;
}

std::vector<WeaponReload> NetClientView::drain_weapon_reloads() {
	std::vector<WeaponReload> out;
	out.swap(pending_weapon_reloads_);
	return out;
}

void NetClientView::pump(ISessionTransport &channel) {
	Datagram dg;
	while (channel.client_recv(dg)) apply(dg.tag, dg.body);
}

namespace {
// The compact coarse heading the present rebuilds: yaw_byte = top 8 bits of the 32-bit
// engine BAM (present does `bam = yaw_byte << 24`). [nova_simulation present.]
inline uint8_t yaw_byte_from_bam(int32_t bam) {
	return static_cast<uint8_t>(static_cast<uint32_t>(bam) >> 24);
}

inline int32_t chase_infantry_pitch(int32_t current, uint8_t target_byte) {
	const int32_t target = static_cast<int32_t>(
			static_cast<uint32_t>(target_byte) << 24);
	const int32_t delta = opennova::io::bam_sub(target, current);
	const int32_t step = opennova::io::bam_sar(
			opennova::io::bam_add(delta, 4), 3);
	return opennova::io::bam_add(current, step);
}
} // namespace

void NetClientView::apply_organic_spawn(const std::vector<uint8_t> &body) {
	OrganicSpawnBatch batch;
	decode_organic_spawn_batch(body.data(), body.size(), batch); // lenient: apply what decoded
	for (const OrganicSpawnRecord &rec : batch.records) {
		if (!rec.has_body) continue;
		ClientEntityState &es = state_.upsert(rec.slot_id);
		es.type_id = rec.item_type_id;
		es.cls = resolver_(rec.item_type_id);
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.orientation);
		es.pitch_bam = 0; // organic spawn carries no entity+20/+24 Euler fields
		es.roll_bam = 0;
	}
}

void NetClientView::apply_pool_spawn(const std::vector<uint8_t> &body) {
	PoolSpawnBatch batch;
	decode_pool_spawn_batch(body.data(), body.size(), batch);
	for (const PoolSpawnRecord &rec : batch.records) {
		// A 0x0D spawn is pool-1 by construction — learn the type's 0x0A replication
		// class so the vehicle compact body decodes for it (see classify()).
		learned_classes_[rec.item_type_id] = EntityClass::Vehicle;
		ClientEntityState &es = state_.upsert(rec.slot_id);
		es.type_id = rec.item_type_id;
		es.cls = EntityClass::Vehicle;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.euler_z);
		es.pitch_bam = rec.euler_x;
		es.roll_bam = rec.euler_y;
		es.parent_handle = rec.parent_handle;
		es.parent_pose_valid = false;
		es.state_flags = static_cast<uint8_t>(rec.entity_flags & 0xFFu);
		es.health_known = false;
	}
	// 0x0D positions are absolute and parent rows normally precede their BFS
	// children. Resolve after the complete batch anyway, so record ordering is
	// not a hidden requirement.
	refresh_parented_pool_entities();
}

void NetClientView::erase_entity_tree(uint16_t root_handle) {
	std::vector<uint16_t> retired{root_handle};
	// Promotion caps attachment lineage at eight. Discover descendants before
	// erasing so nested children cannot retain a dangling parent row.
	for (int depth = 0; depth < 8; ++depth) {
		const std::size_t before = retired.size();
		for (const ClientEntityState &entity : state_.entities) {
			if (entity.parent_handle == 0xFFFFu) continue;
			if (std::find(retired.begin(), retired.end(), entity.parent_handle) ==
					retired.end())
				continue;
			if (std::find(retired.begin(), retired.end(), entity.handle) ==
					retired.end())
				retired.push_back(entity.handle);
		}
		if (retired.size() == before) break;
	}
	state_.entities.erase(
			std::remove_if(state_.entities.begin(), state_.entities.end(),
					[&](const ClientEntityState &entity) {
						return std::find(retired.begin(), retired.end(), entity.handle) !=
								retired.end();
					}),
			state_.entities.end());
}

void NetClientView::refresh_parented_pool_entities() {
	std::vector<uint16_t> dead_children;
	// Repeating the parent-before-child composition makes nested attachment
	// chains order-independent while preserving the promotion depth cap.
	for (int depth = 0; depth < 8; ++depth) {
		for (ClientEntityState &child : state_.entities) {
			if (child.parent_handle == 0xFFFFu) continue;
			ClientEntityState *parent = state_.find(child.parent_handle);
			if (parent == nullptr) continue; // a later batch may still provide it
			// 0x0D entity_flags bit 1 is a spawn/movement gate, not a death
			// verdict. Only interpret flags/health after a real live compact has
			// supplied the vehicle health word. Scripted removals without a final
			// compact still need their witnessed destroy-list message mapped.
			if (parent->health_known && parent->health_word == 0) {
				if (std::find(dead_children.begin(), dead_children.end(), child.handle) ==
						dead_children.end())
					dead_children.push_back(child.handle);
				continue;
			}

			const int32_t parent_yaw_bam = static_cast<int32_t>(
					static_cast<uint32_t>(parent->yaw_byte) << 24);
			if (!child.parent_pose_valid) {
				const WorldPose local = network_transform_world_to_local(
						child.x, child.y, child.z, parent->x, parent->y, parent->z,
						static_cast<uint32_t>(parent_yaw_bam),
						static_cast<uint32_t>(parent->pitch_bam),
						static_cast<uint32_t>(parent->roll_bam));
				child.parent_local_x = local.x;
				child.parent_local_y = local.y;
				child.parent_local_z = local.z;
				child.parent_local_yaw_byte =
						static_cast<uint8_t>(child.yaw_byte - parent->yaw_byte);
				child.parent_local_pitch_bam = static_cast<int32_t>(
						static_cast<uint32_t>(child.pitch_bam) -
						static_cast<uint32_t>(parent->pitch_bam));
				child.parent_local_roll_bam = static_cast<int32_t>(
						static_cast<uint32_t>(child.roll_bam) -
						static_cast<uint32_t>(parent->roll_bam));
				child.parent_pose_valid = true;
			}
			const WorldPose posed = network_transform_local_to_world(
					child.parent_local_x, child.parent_local_y, child.parent_local_z,
					parent->x, parent->y, parent->z,
					static_cast<uint32_t>(parent_yaw_bam),
					static_cast<uint32_t>(parent->pitch_bam),
					static_cast<uint32_t>(parent->roll_bam));
			child.x = posed.x;
			child.y = posed.y;
			child.z = posed.z;
			child.yaw_byte = static_cast<uint8_t>(
					parent->yaw_byte + child.parent_local_yaw_byte);
			child.pitch_bam = static_cast<int32_t>(
					static_cast<uint32_t>(parent->pitch_bam) +
					static_cast<uint32_t>(child.parent_local_pitch_bam));
			child.roll_bam = static_cast<int32_t>(
					static_cast<uint32_t>(parent->roll_bam) +
					static_cast<uint32_t>(child.parent_local_roll_bam));
		}
	}
	for (uint16_t handle : dead_children) erase_entity_tree(handle);
}

void NetClientView::apply_static_batch(const std::vector<uint8_t> &body) {
	StaticEntityBatch batch;
	decode_static_entity_batch(body.data(), body.size(), batch);
	// The 0x10 record carries no slot id — the entity's slot is start_index + iteration index.
	for (size_t i = 0; i < batch.records.size(); ++i) {
		const StaticEntityRecord &rec = batch.records[i];
		if (rec.is_empty_slot) continue;
		const uint16_t handle = static_cast<uint16_t>(0x2000u | ((batch.start_index + i) & 0x0FFFu));
		ClientEntityState &es = state_.upsert(handle);
		es.type_id = rec.item_type_id;
		es.cls = EntityClass::Unknown; // a static has no 0x0A motion class; it never moves
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.euler_z);
		es.pitch_bam = rec.euler_x;
		es.roll_bam = rec.euler_y;
	}
}

void NetClientView::apply_pool3_batch(const std::vector<uint8_t> &body) {
	Pool3SyncBatch batch;
	decode_pool3_sync_batch(body.data(), body.size(), batch);
	for (const Pool3SyncRecord &rec : batch.records) {
		if (rec.is_empty_slot) continue;
		ClientEntityState &es = state_.upsert(rec.net_handle);
		es.type_id = rec.item_type_id;
		es.cls = EntityClass::Unknown;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(static_cast<int32_t>(rec.movement_val));
		es.pitch_bam = 0; // pool-3 sync carries no entity+20/+24 Euler fields
		es.roll_bam = 0;
	}
}

void NetClientView::apply_frame_update(const std::vector<uint8_t> &body) {
	FrameUpdate fu;
	// decode_frame_update leaves everything it walked in `fu` even on a short read,
	// so we apply whatever decoded cleanly (out.complete reflects a clean terminator).
	decode_frame_update(body.data(), body.size(),
	                    [this](uint16_t tid) { return classify(tid); }, fu,
	                    (game_type_ & 0x20000u) != 0u);

	state_.anchor_x = fu.anchor_x;
	state_.anchor_y = fu.anchor_y;
	state_.anchor_z = fu.anchor_z;
	if (fu.local_tail_present) {
		state_.local_health = fu.health;
		++state_.health_updates_applied;
	}
	if (fu.objective.present) {
		state_.objective_won = static_cast<uint32_t>(fu.objective.state[0]);
		state_.objective_lost = static_cast<uint32_t>(fu.objective.state[1]);
		state_.objective_show_win = static_cast<uint32_t>(fu.objective.state[2]);
		state_.objective_show_lose = static_cast<uint32_t>(fu.objective.state[3]);
		++state_.objective_updates_applied;
	}

	// Tag 2 carries a fire origin and direction. Lift the compressed origin by
	// this frame's anchor now, while those transient coordinates are together.
	for (const RoundEventRecord &rec : fu.round_events) {
		ClientRoundEvent ev;
		ev.flags = rec.flags;
		ev.adm_index = rec.adm_index;
		ev.subtype = rec.subtype;
		ev.slot_byte = rec.slot_byte;
		ev.shooter_handle = rec.shooter_handle;
		ev.target_handle = rec.target_handle;
		ev.shot_seq = rec.shot_seq;
		ev.origin_x = fu.anchor_x + network_decompress_fixedpoint(rec.pos_x_compressed);
		ev.origin_y = fu.anchor_y + network_decompress_fixedpoint(rec.pos_y_compressed);
		ev.origin_z = fu.anchor_z + network_decompress_fixedpoint(rec.pos_z_compressed);
		ev.dir_yaw_bam = static_cast<int32_t>(
				static_cast<uint32_t>(rec.yaw_bam_high) << 16);
		ev.dir_pitch_bam = static_cast<int32_t>(
				static_cast<uint32_t>(rec.pitch_bam_high) << 16);
		pending_round_events_.push_back(ev);
	}

	for (ClientEntityState &e : state_.entities) e.seen_this_frame = false;
	struct PendingCarrierPose {
		uint16_t child_handle;
		uint16_t carrier_handle;
		uint16_t cx;
		uint16_t cy;
		uint16_t cz;
		uint8_t local_yaw_byte;
	};
	std::vector<PendingCarrierPose> pending_carrier_poses;
	pending_carrier_poses.reserve(fu.records.size());

	for (const FrameUpdateRecord &rec : fu.records) {
		if (rec.cls == EntityClass::NoNetworkCallback) {
			continue;
		}
		ClientEntityState &es = state_.upsert(rec.handle);
		es.type_id = rec.type_id;
		es.cls = rec.cls;
		es.seen_this_frame = true;
		// Every compact record is a complete sample of these organic fields. Clear the
		// normalized row before class-specific assignment so dismounts and class changes
		// cannot retain a stale carrier/bone selector from an earlier frame.
		es.carrier_handle = 0xFFFFu;
		es.mount_bone = 0;
		es.seat_type = 0;
		es.pitch_byte = 0;
		es.aim_yaw_byte = 0;
		es.anim_state_id = 0;
		es.anim_channel_ratio = 0;

		// Compact player and infantry records carry the authoritative organic
		// lifecycle bits. Retain the complete byte, while counting only known
		// dead -> alive edges so an initial alive spawn is not mistaken for a
		// respawn. This happens per decoded record rather than per render frame:
		// pump() may fold several queued 0x0A datagrams before Godot presents.
		bool has_state_flags = false;
		uint8_t state_flags = 0;
		if (rec.cls == EntityClass::Player) {
			has_state_flags = true;
			state_flags = rec.player.state_flags;
		} else if (rec.cls == EntityClass::Infantry) {
			has_state_flags = true;
			state_flags = rec.infantry.flags_byte;
		}
		if (has_state_flags) {
			const bool was_known = es.state_flags_known;
			const bool was_dead = (es.state_flags & 0x02u) != 0u;
			const bool is_alive = (state_flags & 0x02u) == 0u;
			es.state_flags = state_flags;
			es.state_flags_known = true;
			if (was_known && was_dead && is_alive) {
				++es.respawn_revision;
			}
		}

		// Reconstruct world position: decompress the compact (per-axis) and add the
		// frame anchor — or, for a CARRIER-LOCAL player record (vehicle/ground handle !=
		// 0xFFFF, D-NET-151), lift the local offset through the carrier's pose from this
		// view's own state [orig: op2 resolves the carrier from g_pool_list and runs
		// Entity_TransformLocalToWorld @0x4c10d4; a carrier with no itemDef DROPS the
		// record and queues a C2S 0x0F entity request — request plumbing an in-process
		// view does not need, so an unknown carrier just skips the position sample].
		// Carrier-local samples are queued for a second pass after every record has
		// updated the view. Production order is pool-0 child before pool-1 carrier.
		uint16_t cx = 0, cy = 0, cz = 0;
		bool skip_pos = false;
		switch (rec.cls) {
		case EntityClass::Player:
			cx = rec.player.pos_x_compressed;
			cy = rec.player.pos_y_compressed;
			cz = rec.player.pos_z_compressed;
			es.carrier_handle = rec.player.carrier_handle;
			es.mount_bone = rec.player.vehicle_bone;
			es.seat_type = rec.player.seat_type;
			es.pitch_byte = rec.player.pitch_byte;
			es.anim_state_id = rec.player.anim_state_id;
			es.anim_channel_ratio = rec.player.anim_channel_ratio;
			es.state_flags = rec.player.state_flags;
			if (rec.player.carrier_handle != 0xFFFFu) {
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.player.carrier_handle, cx, cy, cz,
						rec.player.yaw_byte});
				skip_pos = true;
			} else {
				es.yaw_byte = rec.player.yaw_byte;
			}
			break;
		case EntityClass::Vehicle:
			cx = rec.vehicle.pos_x_compressed;
			cy = rec.vehicle.pos_y_compressed;
			cz = rec.vehicle.pos_z_compressed;
			es.yaw_byte = static_cast<uint8_t>(
					static_cast<uint16_t>(rec.vehicle.euler_z) >> 8);
			es.state_flags = rec.vehicle.flags_byte;
			es.health_word = rec.vehicle.health_word;
			es.health_known = true;
			// Live vehicle compacts omit entity+20/+24. Preserve the last full
			// spawn/dead-pose values until the short dead-pose form carries new
			// signed high words [orig: @0x460d4c/@0x460d52].
			if (rec.vehicle.is_dead_pose) {
				es.pitch_bam = static_cast<int32_t>(rec.vehicle.euler_x) * 65536;
				es.roll_bam = static_cast<int32_t>(rec.vehicle.euler_y) * 65536;
			}
			break;
		case EntityClass::Infantry:
			cx = rec.infantry.pos_x_compressed;
			cy = rec.infantry.pos_y_compressed;
			cz = rec.infantry.pos_z_compressed;
			es.carrier_handle = rec.infantry.vehicle_slot_handle;
			es.mount_bone = rec.infantry.seat_bone_idx;
			es.pitch_byte = rec.infantry.pitch_byte;
			es.aim_yaw_byte = rec.infantry.aim_yaw_byte;
			es.anim_state_id = rec.infantry.anim_byte;
			es.state_flags = rec.infantry.flags_byte;
			// The compact carries desired aim pitch (entity+0x2D0), not live
			// entity+0x14. Retail's remote gunner rebuilds the latter locally
			// with the same wrapped one-eighth chase as authoritative AI.
			// [orig: chase @0x4bef7b..0x4bef97]
			if (rec.infantry.vehicle_slot_handle != 0xFFFFu &&
					rec.infantry.seat_bone_idx != 0) {
				es.pitch_bam = chase_infantry_pitch(
						es.pitch_bam, rec.infantry.aim_yaw_byte);
			}
			if (rec.infantry.vehicle_slot_handle != 0xFFFFu) {
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.infantry.vehicle_slot_handle, cx, cy, cz,
						rec.infantry.yaw_byte});
				skip_pos = true;
			} else {
				es.yaw_byte = rec.infantry.yaw_byte;
			}
			break;
		default:
			break; // unresolved/guided records are not compact motion samples
		}
		if (!skip_pos) {
			es.x = fu.anchor_x + network_decompress_fixedpoint(cx);
			es.y = fu.anchor_y + network_decompress_fixedpoint(cy);
			es.z = fu.anchor_z + network_decompress_fixedpoint(cz);
		}
	}

	// Resolve carrier-local children only after the complete frame has upserted and
	// updated every carrier. If the carrier is genuinely absent, leave the child's
	// prior world pose/yaw untouched, matching the retail record-drop path.
	for (const PendingCarrierPose &pending : pending_carrier_poses) {
		ClientEntityState *child = state_.find(pending.child_handle);
		const ClientEntityState *carrier = state_.find(pending.carrier_handle);
		if (child == nullptr || carrier == nullptr) continue;
		const int32_t carrier_yaw_bam = static_cast<int32_t>(
				uint32_t(carrier->yaw_byte) << 24);
		const WorldPose w = network_transform_local_to_world(
				network_decompress_fixedpoint(pending.cx),
				network_decompress_fixedpoint(pending.cy),
				network_decompress_fixedpoint(pending.cz), carrier->x, carrier->y,
				carrier->z, uint32_t(carrier_yaw_bam), uint32_t(carrier->pitch_bam),
				uint32_t(carrier->roll_bam));
		child->x = w.x;
		child->y = w.y;
		child->z = w.z;
		// World yaw byte = carrier yaw + local yaw; BAM addition holds in the
		// 8-bit ring used by the compact view.
		child->yaw_byte = uint8_t(carrier->yaw_byte + pending.local_yaw_byte);
	}

	refresh_parented_pool_entities();

	++state_.frames_applied;
}

} // namespace opennova::netsim
