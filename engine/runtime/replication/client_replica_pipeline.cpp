#include <runtime/replication/client_replica_pipeline.h>

#include "client_replica_body_arbitration.h"

#include <runtime/replication/entity_wire_bridge.h> // class_for_type_id (default resolver)
#include <runtime/world/entity.h>              // kEntityFlag* (the wire state_flags byte IS entity+36 low)
#include <runtime/world/infantry.h>            // the anim flag/state tables (infantry_anim_flags)
#include <runtime/world/world.h>               // exact mission PRNG seed
#include <base/gameprofile/game_type.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/wire_handle.h>
#include <base/io/bam.h>                      // wrapped retail pitch chase

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <base/io/fixed.h>

namespace opennova::replication {


// ---- ClientReplicaPipeline -----------------------------------------------

ClientReplicaPipeline::ClientReplicaPipeline()
		: resolver_([](uint16_t tid) { return class_for_type_id(tid); }),
		  prng16_(world::World::kMissionPrng16Seed) {}

ClientReplicaPipeline::ClientReplicaPipeline(std::function<EntityClass(uint16_t)> resolver)
		: resolver_(std::move(resolver)),
		  prng16_(world::World::kMissionPrng16Seed) {}

void ClientReplicaPipeline::set_item_class_resolver(ItemClassResolver resolver) {
	item_resolver_ = std::move(resolver);
}

void ClientReplicaPipeline::set_item_catalog(std::shared_ptr<const ItemReplicationCatalog> catalog) {
	item_catalog_ = std::move(catalog);
	if (!item_catalog_) {
		item_resolver_ = {};
		return;
	}
	item_resolver_ = [catalog = item_catalog_](uint16_t type_id) {
		return catalog->resolve_wire_entity_class(type_id);
	};
}

const ItemReplicationProfile *ClientReplicaPipeline::item_def(uint16_t type_id) const {
	return item_catalog_ ? item_catalog_->by_wire_type(type_id) : nullptr;
}

EntityClass ClientReplicaPipeline::classify(uint16_t type_id) const {
	// items.def first — the retail client's own dispatch source [orig: itemDef+356
	// @0x50f2e2]. It must outrank the 0x0D pool blanket: pool-1 holds no-callback
	// types too (an `ewep` emplacement), and sizing their header-only records as a
	// vehicle compact desyncs the whole frame after them.
	if (item_resolver_) {
		const ItemClassResolution resolution = item_resolver_(type_id);
		// A present Unknown is a catalogued unresolved/ambiguous definition. It
		// is terminal and intentionally reaches npwire's fail-closed width path.
		if (resolution.has_value()) return *resolution;
	}
	const auto it = learned_classes_.find(type_id);
	if (it != learned_classes_.end()) return it->second;
	return resolver_(type_id);
}


namespace {
// The compact coarse heading the present rebuilds: yaw_byte = top 8 bits of the 32-bit
// engine BAM (present does `bam = yaw_byte << 24`). [simulation present.]
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

// Disarm the row's root-motion channel: the next free-standing tick re-arms
// it fresh from the wire state (+ the live phase seed). Used by the mover
// freezes (dead/bit0/carried — retail applies the anim byte per record
// regardless of the mover skip [orig: @0x4c1153], so presentation must show
// the WIRE state while frozen), the respawn edge (the resume must not blend
// out of the pre-death primary), and type-change/handle-reuse edges.
void row_channel_disarm(ClientEntityState &es) {
	es.rm_state = -1;
	es.rm_prev_state = -1;
	es.rm_blend_weight = 1.0f;
	es.rm_blend_step = 0.0f;
	es.rm_prev_bottom_live = false;
	es.rm_leg_seeded = false;
}
} // namespace

void ClientReplicaPipeline::apply_organic_spawn(const std::vector<uint8_t> &body) {
	OrganicSpawnBatch batch;
	if (!decode_organic_spawn_batch(body.data(), body.size(), batch)) {
		// The retail handler applies records as it walks the page; a
		// malformed tail loses only the unread remainder [orig: NapiNPClientMsg_0x0E family].
		// Count the malformed page, drop the half-read record the decoder
		// staged at the failure point, and apply the complete prefix.
		++malformed_bodies_;
		if (batch.last_record_partial && !batch.records.empty())
			batch.records.pop_back();
		if (batch.records.empty()) return;
	}
	bool changed = false;
	const auto in_pool_tables = [](uint16_t packed) {
		const world::EntityHandle h{packed};
		return packed != wire_handle::kInvalid && h.pool() < world::kEntityPoolCount &&
				static_cast<std::size_t>(h.slot()) < world::retail_pool_capacity(h.pool());
	};
	for (const OrganicSpawnRecord &rec : batch.records) {
		// The page stops at the first handle outside the pool tables
		// [orig: NapiNPClientMsg_0x00C @0x42E7A5..0x42E7CE].
		if (!in_pool_tables(rec.slot_id)) break;
		// Every record clears the whole slot before its body byte is read: a
		// body-less record leaves the slot empty, a body rebuilds it from
		// scratch, the same type included [orig: the memsets @0x42E7E9 /
		// @0x42E7F6; the has-body byte @0x42E803 -> LABEL_107].
		ClientEntityState *existing = state_.find(rec.slot_id);
		const bool type_changed = existing != nullptr &&
				existing->type_id != rec.item_type_id;
		const uint32_t next_spawn_revision = begin_entity_lifetime(rec.slot_id);
		if (!rec.has_body()) {
			const std::size_t before = state_.entities.size();
			state_.entities.erase(
					std::remove_if(state_.entities.begin(), state_.entities.end(),
							[&rec](const ClientEntityState &row) {
								return row.handle == rec.slot_id;
							}),
					state_.entities.end());
			if (state_.entities.size() != before) {
				state_.mark_topology_changed();
				++state_.world_stream_revision;
				changed = true;
			}
			continue;
		}
		ClientEntityState &es = state_.upsert(rec.slot_id);
		if (type_changed) state_.mark_topology_changed();
		es = ClientEntityState{};
		es.handle = rec.slot_id;
		es.spawn_revision = next_spawn_revision;
		++state_.world_stream_revision;
		es.type_id = rec.item_type_id;
		es.cls = classify(rec.item_type_id);
		// The Name copy is bounded: at most 15 characters, then the NUL.
		// [orig: NapiNPClientMsg_0x00C @0x42E860..0x42E8EA]
		es.display_name = rec.entity_name.substr(0, 15);
		es.net_id = rec.net_id;
		es.spawn_tag = s2c::ENTITY_SPAWN_BATCH;
		es.spawn_owner_connection_id = rec.owner_connection_id;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.orientation);
		es.heading_bam = rec.orientation;
		// Retail rows are born with the spawn orientation in EVERY heading
		// slot; seed the stage + promote target so the first compact's
		// unconditional +0x1A8 promote publishes the spawn orientation, not a
		// zero-initialized stage [orig: the spawn writes behind the @0x4C0320
		// promote].
		es.net_smooth_heading = rec.orientation;
		es.net_target_heading_bam = rec.orientation;
		es.heading_known = true;
		es.pitch_bam = 0; // organic spawn carries no entity+20/+24 Euler fields
		es.roll_bam = 0;
		es.recoil_pitch = 0;
		es.team = rec.team;
		es.team_known = true;
		// Flags (entity+36) is the record's word, zero-extended: an
		// undeployed or spectating player arrives hidden (bit0) and a dead
		// one dead (0x02, the local latch the respawn edge reads)
		// [orig: Flags = minimapFlags @0x42E917].
		es.spawn_entity_flags = rec.minimap_flags;
		es.rm_entity_flags = rec.minimap_flags;
		es.state_flags = static_cast<uint8_t>(rec.minimap_flags & 0xFFu);
		es.state_flags_known = true;
		// The rest of the record's entity fields [orig: aiState @0x42E989,
		// animSlot @0x42E9A6, playerClass @0x42E9CD, entity+0x154 @0x42EA06,
		// refNum @0x42EA20, subType @0x42EA35, attachBoneId @0x42EA4A,
		// parentEntity (+0x16C) @0x42EAB5].
		es.spawn_ai_state = rec.ai_state;
		es.spawn_anim_slot = rec.anim_slot;
		es.spawn_player_class = rec.player_class;
		es.spawn_byte_154 = rec.player_slot_id;
		es.spawn_ref_num = rec.alert_level;
		es.spawn_sub_type = rec.sub_type;
		es.mount_bone = rec.weapon_type;
		if (in_pool_tables(rec.parent_handle)) es.carrier_handle = rec.parent_handle;
		changed = true;
	}
	if (changed) state_.mark_changed();
}

// The per-body-tick lean integrator, run once per client frame for every remote
// organic: the decay first, then the ramp from the latest wire bits — the same
// locally integrated model retail runs at both ends (only the bits replicate).
// The order is load-bearing: decay-then-ramp settles at ~±0x30000000 ≈ 67.5°,
// ramp-then-decay one ramp step short of it (~±0x2D000000).
// [orig: both legs of the single Entity_UpdateInfantryPlayerBody @0x4b40e0 pass —
//  decay lean -= (lean+8)>>4 @0x4b5c97, then the on-foot ramp @0x4b7dbf (bit 6 left
//  −0x3000000/tick) / @0x4b7dd6 (bit 7 right +0x3000000/tick)]
// The producer's ramp gates (alive/prone, and the seated ±0x1400000 variant) are not
// applied here: the decoded row carries no honest stance/seat state for them (D-INF-17).
void ClientReplicaPipeline::land_compact_pose(ClientEntityState &es, int32_t wx,
		int32_t wy, int32_t wz, bool has_heading, int32_t heading_bam,
		bool force_live_snap) {
	es.net_has_compact = true;
	if (!remote_motion_mode_ || force_live_snap || es.net_world_mover) {
		// Live snap: the host/SP roles (full-rate loopback; the authority never
		// interpolates, D-NET-89), the respawn edge [orig: live snap +
		// Entity_ResetToSpawnState @0x4C113C], and the vehicle dead-pose form.
		es.x = wx;
		es.y = wy;
		es.z = wz;
		if (has_heading) es.heading_bam = heading_bam;
		// Snap-mode players keep the live pitch mirrored from the wire byte so
		// presentation reads one field in both modes (the joiner's org2 chase
		// maintains it below).
		if (has_heading && es.cls == EntityClass::Player) {
			es.pitch_bam = static_cast<int32_t>(
					static_cast<uint32_t>(es.pitch_byte) << 24);
		}
		if (force_live_snap && remote_motion_mode_) {
			// Retail stages the target BEFORE the respawn branch snaps the live
			// pose [orig: LABEL_123 staging precedes the +0x24&2 snap @0x4C1109+]
			// — the staged target stays at the wire pose, so the next mover tick
			// sees a zero delta (deadband) instead of gliding anywhere.
			es.net_smooth_target[0] = wx;
			es.net_smooth_target[1] = wy;
			es.net_smooth_target[2] = wz;
			es.net_smooth_heading = has_heading ? heading_bam : es.heading_bam;
			es.net_target_heading_bam = es.net_smooth_heading;
			es.net_interp_steps = 0;
			es.net_interp_progress = 0;
		}
		return;
	}
	// STAGE-ONLY (net-re §5.38e): the compact read never writes the live pose of
	// an alive remote entity — it stages the target cluster and resets the
	// progress counter; tick_remote_motion moves the live pose.
	// [orig: case-2 @0x4C0FE4/EA/F0 + @0x4C0FD7/@0x4C0FF6 + @0x4C0FFC;
	//  infantry mode-2 incl. the targetHeading promote; vehicle mode-2 @0x4607D5+]
	es.net_smooth_target[0] = wx;
	es.net_smooth_target[1] = wy;
	es.net_smooth_target[2] = wz;
	if (has_heading) {
		if (es.cls == EntityClass::Infantry) {
			// org1: the PREVIOUS staged heading becomes the chase target — one
			// record behind the wire, UNCONDITIONALLY [orig: the +0x1A8
			// promote in @0x4C0320]. The spawn appliers seed the stage with
			// the spawn orientation (retail rows are born with it in every
			// heading slot), so a row's first compact promotes that — no
			// first-record special case exists in the witness.
			es.net_target_heading_bam = es.net_smooth_heading;
		}
		es.net_smooth_heading = heading_bam;
		if (es.cls == EntityClass::Player) {
			es.net_smooth_pitch = static_cast<int32_t>(
					static_cast<uint32_t>(es.pitch_byte) << 24);
		}
	}
	es.net_interp_progress = 0; // [orig: @0x4C0FFC — bucket +0x27E is NOT reset]
}

void ClientReplicaPipeline::queue_carrier_repair(uint16_t handle) {
	for (uint16_t h : carrier_repair_requests_)
		if (h == handle) return;
	carrier_repair_requests_.push_back(handle);
}

std::vector<uint16_t> ClientReplicaPipeline::drain_carrier_repair_requests() {
	std::vector<uint16_t> out;
	out.swap(carrier_repair_requests_);
	return out;
}

std::vector<uint16_t> ClientReplicaPipeline::drain_death_edges() {
	std::vector<uint16_t> out;
	out.swap(death_edges_);
	return out;
}

std::vector<ClientReplicaPipeline::ReplicaCorpseDecay>
ClientReplicaPipeline::drain_corpse_decays() {
	std::vector<ReplicaCorpseDecay> out;
	out.swap(corpse_decays_);
	return out;
}

std::vector<ClientReplicaPipeline::ReplicaSlotSound>
ClientReplicaPipeline::drain_slot_sounds() {
	std::vector<ReplicaSlotSound> out;
	out.swap(slot_sounds_);
	return out;
}

void ClientReplicaPipeline::apply_pool_spawn(const std::vector<uint8_t> &body) {
	PoolSpawnBatch batch;
	if (!decode_pool_spawn_batch(body.data(), body.size(), batch)) {
		// The retail handler applies records as it walks the page; a
		// malformed tail loses only the unread remainder [orig: NapiNPClientMsg_0x00D @ 0x432C40].
		// Count the malformed page, drop the half-read record the decoder
		// staged at the failure point, and apply the complete prefix.
		++malformed_bodies_;
		if (batch.last_record_partial && !batch.records.empty())
			batch.records.pop_back();
		if (batch.records.empty()) return;
	}
	bool changed = false;
	for (const PoolSpawnRecord &rec : batch.records) {
		const world::EntityHandle spawn_handle{rec.slot_id};
		if (spawn_handle.pool() != 1 ||
				static_cast<std::size_t>(spawn_handle.slot()) >=
						world::retail_pool_capacity(1))
			continue;
		// A 0x0D spawn is pool-1 by construction — learn the type's 0x0A replication
		// class so the vehicle compact body decodes for it (see classify()).
		learned_classes_[rec.item_type_id] = EntityClass::Vehicle;
		ClientEntityState *existing = state_.find(rec.slot_id);
		const bool type_changed = existing != nullptr &&
				existing->type_id != rec.item_type_id;
		const uint32_t next_spawn_revision = begin_entity_lifetime(rec.slot_id);
		ClientEntityState &es = state_.upsert(rec.slot_id);
		if (type_changed) state_.mark_topology_changed();
		// Retail clears the complete 0x2B4-byte slot before applying every
		// 0x0D record. Replace the decoded row too: compact carrier/death/anim
		// and mover state belongs to the prior lifetime even when type matches.
		es = ClientEntityState{};
		es.handle = rec.slot_id;
		es.type_id = rec.item_type_id;
		es.cls = classify(rec.item_type_id);
		// entity+0xF4, which only an AIData record names (its serializer writes
		// the empty string for any other def). [orig: NapiNPClientMsg_0x00D
		//  @0x433320..0x43334A]
		es.display_name = rec.entity_name;
		// The 0x0D handler memsets the full slot and has no entity+124
		// net-id field. Preserve retail's resulting zero, not ClientState's
		// unknown/sentinel default.
		es.net_id = 0;
		es.spawn_tag = s2c::POOL_SPAWN;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.euler_z);
		es.heading_bam = rec.euler_z;
		// Born with the spawn orientation in every heading slot (see the
		// organic applier note).
		es.net_smooth_heading = rec.euler_z;
		es.net_target_heading_bam = rec.euler_z;
		es.heading_known = (rec.spawn_flags & 0x0001u) != 0;
		es.pitch_bam = rec.euler_x;
		es.roll_bam = rec.euler_y;
		// Flag-gated values are zero in the decoded record when omitted.
		// Assign unconditionally: retail's receive slot is zero-initialized, so
		// omission denotes zero rather than "preserve the previous value".
		es.team = rec.team_byte;
		es.team_known = (rec.spawn_flags & 0x0010u) != 0;
		es.zone_number_rank = rec.zone_number_rank;
		es.zone_radius = rec.zone_radius;
		es.spawn_entity_flags = rec.entity_flags;
		es.spawn_section_mask = static_cast<uint32_t>(rec.section_mask);
		es.spawn_ammo_count = rec.bone_byte;
		es.spawn_ref_num = rec.alert_byte;
		es.spawn_sub_type = rec.action_byte;
		es.spawn_mount_mask = rec.seat_mask;
		for (std::size_t slot = 0; slot < rec.mount_handles.size(); ++slot)
			es.spawn_mount_handles[slot] = rec.mount_handles[slot];
		es.spawn_mount_handles[8] = rec.mount_handle_8;
		es.spawn_mount_handles[9] = rec.mount_handle_9;
		es.spawn_revision = next_spawn_revision;
		++state_.world_stream_revision;
		es.parent_handle = rec.parent_handle;
		es.target_handle = rec.target_handle;
		es.parent_pose_valid = false;
		es.state_flags = static_cast<uint8_t>(rec.entity_flags & 0xFFu);
		es.health_known = false;
		changed = true;
	}
	// 0x0D positions are absolute and parent rows normally precede their BFS
	// children. Resolve after the complete batch anyway, so record ordering is
	// not a hidden requirement.
	refresh_carried_entities();
	if (changed) state_.mark_changed();
}

void ClientReplicaPipeline::refresh_carried_entities(bool tick_sweep) {
	std::vector<uint16_t> sweep_destroyed;
	// ClientState intentionally keeps its decoded rows public, so its general
	// find() contract must remain a derived linear lookup. This refresh owns a
	// stable vector for its whole eight-depth pass, though: build a disposable
	// first-row index here instead of rescanning a retail-sized world stream for
	// every carrier/parent edge. emplace preserves find()'s first-match behavior
	// if a caller has directly introduced duplicate handles.
	std::unordered_map<uint16_t, std::size_t> row_by_handle;
	row_by_handle.reserve(state_.entities.size());
	for (std::size_t i = 0; i < state_.entities.size(); ++i)
		row_by_handle.emplace(state_.entities[i].handle, i);
	auto find_row = [&](uint16_t handle) -> ClientEntityState * {
		const auto found = row_by_handle.find(handle);
		return found != row_by_handle.end()
				? &state_.entities[found->second]
				: nullptr;
	};

	// The item catalog and learned class table cannot change during this method.
	// Resolve each no-callback row's persistent STRUCTURAL CARRIER once, outside
	// the repeated pose-composition depths. Retail's 'ewep' class MOVE function
	// recomposes the child from groundEntity (entity+40) every tick — the slot
	// the 0x0D TARGET seeds — so a compact-less mounted gun rides its DRIVING
	// hull with no wire records of its own: the carrier-def seat bone
	// (carrierDef+532+subType) selects a model bone and the child adopts the
	// carrier-matrix-composed position + Euler set (or, with no resolvable
	// bone, the carrier pose verbatim).
	// [orig: move-fn table 'ewep' row @0x82abe0 -> Entity_UpdateTransformAndTurret
	//  @0x440ca0 — groundEntity read @0x440cbf, bone re-resolve @0x440cfd,
	//  carrier-matrix bone compose @0x44109d with position+Euler adoption
	//  @0x4410dd, no-bone verbatim carrier-pose adoption @0x4410ea..0x4411bc]
	// Without model bone tables at this layer, the recompose below carries the
	// rigid child-in-carrier pose captured from the 0x0D absolutes — the same
	// client-subset simplification the parent-follow path already pins.
	std::vector<uint16_t> persistent_carrier(state_.entities.size(), wire_handle::kInvalid);
	for (std::size_t i = 0; i < state_.entities.size(); ++i) {
		const ClientEntityState &child = state_.entities[i];
		if (child.target_handle == wire_handle::kInvalid && child.parent_handle == wire_handle::kInvalid)
			continue; // no carrier candidate — skip the catalog lookup
		if (classify(child.type_id) != EntityClass::NoNetworkCallback)
			continue;
		// The 0x0D TARGET is the structural carrier (groundEntity/+40) and
		// outranks any parent: retail's transform never reads +368. A POOL-0
		// parent on a no-callback child is the occupant/driver back-reference,
		// never a transform parent (live retail 0x0D witness, 00TRg 2026-08-04:
		// an OCCUPIED "50cal on 180 tripod" spawns with parent=<its gunner's
		// pool-0 handle>, while the gunner's own record carries parent=<the
		// gun> — composing both closes a mutual seat/parent loop that ratchets
		// the pair through the depth passes (the reported climbing/spinning
		// emplacements). persistent_carrier_handle (client_state.h) is that rule.
		// [orig: 0x0D store @0x433289 — entity+368 occupantEntity back-ref;
		//  target → groundEntity resolve @0x4332bc, store @0x4332d7]
		persistent_carrier[i] = persistent_carrier_handle(child);
	}
	// Repeating the composition makes mixed seat/persistent-parent chains
	// independent of pool/vector ordering while preserving the promotion depth
	// cap. This method runs only after movers, so every lookup observes the
	// carrier's final live pose for this tick.
	for (int depth = 0; depth < 8; ++depth) {
		for (std::size_t child_index = 0;
		     child_index < state_.entities.size(); ++child_index) {
			ClientEntityState &child = state_.entities[child_index];
			// Compact-carried player/infantry/vehicle rows retain the latest
			// successfully resolved local sample. A missing carrier invalidates
			// the sample rather than leaving an offset that could attach to a
			// later handle reuse.
			// A world-mover VEHICLE is predicted by the embedding sim from the
			// composed record sample whatever its carrier (a pool-1 deck, or
			// the static a bridge/roof/ramp resolves to), its mover riding the
			// carrier as its ground link; its row publishes the mirrored
			// predicted pose, so the per-tick recompose must not drag it back
			// to the record's sample between records. [orig:
			// Entity_SerializeVehicleState stages the composed sample and lands
			// groundEntity @0x4607cd..0x460802; no rigid re-attach]. A
			// dead-pose wreck on a pool-1 deck keeps the seat-follow: the
			// mover is frozen for it (JoinerRole) and the dead-pose form's
			// carrier handling is not re-witnessed here.
			const bool dead_pose = child.state_flags_known &&
					(child.state_flags & kVehicleFlagDeadPose) != 0u;
			const bool world_predicted = child.cls == EntityClass::Vehicle &&
					child.net_world_mover &&
					child.carrier_handle != wire_handle::kInvalid &&
					(!dead_pose || world::EntityHandle{child.carrier_handle}.pool() != 1);
			if (child.net_seat_valid && child.carrier_handle != wire_handle::kInvalid &&
					!world_predicted) {
				const ClientEntityState *carrier = find_row(child.carrier_handle);
				if (carrier == nullptr) {
					child.net_seat_valid = false;
				} else {
					const WorldPose posed = network_transform_local_to_world(
							child.net_seat_local[0], child.net_seat_local[1],
							child.net_seat_local[2], carrier->x, carrier->y,
							carrier->z, static_cast<uint32_t>(carrier->heading_bam),
							static_cast<uint32_t>(carrier->pitch_bam),
							static_cast<uint32_t>(carrier->roll_bam));
					child.x = posed.x;
					child.y = posed.y;
					child.z = posed.z;
					child.heading_bam = io::bam_add(
							carrier->heading_bam, child.net_seat_local_heading_bam);
					child.yaw_byte = yaw_byte_from_bam(child.heading_bam);
				}
			}

			if (persistent_carrier[child_index] == wire_handle::kInvalid) continue;
			// Only the addeweap/no-callback family rides this persistent
			// recompose: those children never receive compact motion samples,
			// so the load-time 0x0D target/parent relation is their only pose
			// source. A compact-sampled class (player/vehicle/infantry) moves
			// by its OWN records — its carrier composition happens per record
			// on the record's own carrier field — and its 0x0D parentHandle is
			// the occupantEntity/+368 DRIVER back-reference, never a transform
			// parent [orig: 0x0D store @0x433289; vehicle-compact carrier
			// compose @0x4608ce]. Recomposing such a row here glued the
			// vehicle to its spawn-time occupant — on non-COOP retail hosts,
			// "a vehicle follows the player around" (one per map, whichever
			// spawn record carried flag 0x0100).
			ClientEntityState *parent = find_row(persistent_carrier[child_index]);
			if (parent == nullptr) {
				// The pure-client stale-carrier sweep [orig: @0x440d41..
				// 0x440e2f]: after a 128-tick unresolvable run, request BOTH
				// rows over C2S 0x0F and locally destroy the child — the
				// authority's re-spawn re-materializes the pair. A later batch
				// may still provide the carrier inside the window, which
				// resets the run below.
				if (tick_sweep && depth == 0 &&
				    ++child.carrier_missing_ticks >= 128) {
					queue_carrier_repair(persistent_carrier[child_index]);
					queue_carrier_repair(child.handle);
					if (std::find(sweep_destroyed.begin(), sweep_destroyed.end(),
							child.handle) == sweep_destroyed.end())
						sweep_destroyed.push_back(child.handle);
				}
				continue;
			}
			child.carrier_missing_ticks = 0;
			// The dead-carrier leg. Retail's client HIDES the child in place —
			// carrier Flags & 2 -> child Flags |= 1, return, no pose follow — and
			// the live carrier's vehicle block clears the child's low Flags bits
			// again (Flags &= ~7) once it respawns; the row persists throughout,
			// as it does on the authority. The row's state_flags byte is the
			// entity Flags low byte, which no compact record writes for this
			// family. The death signal stays the known-zero health word only: the
			// wire flags bit 1 is an overloaded spawn/movement gate on 0x0D-fed
			// rows, not a death verdict (the loopback-identity pin).
			// [orig: Entity_UpdateTransformAndTurret @0x440cdb..0x440ce1 (the
			//  hide), `and [child+24h],0FFFFFFF8h` @0x440EB3 (the clear)]
			if (parent->health_known && parent->health_word == 0) {
				child.state_flags |= static_cast<uint8_t>(world::kEntityFlagCarried);
				child.state_flags_known = true;
				continue;
			}
			child.state_flags &= static_cast<uint8_t>(~(world::kEntityFlagCarried |
					world::kEntityFlagDead | world::kEntityFlagHusk));

			const int32_t parent_heading_bam = parent->heading_bam;
			if (!child.parent_pose_valid) {
				const WorldPose local = network_transform_world_to_local(
						child.x, child.y, child.z, parent->x, parent->y, parent->z,
						static_cast<uint32_t>(parent_heading_bam),
						static_cast<uint32_t>(parent->pitch_bam),
						static_cast<uint32_t>(parent->roll_bam));
				child.parent_local_x = local.x;
				child.parent_local_y = local.y;
				child.parent_local_z = local.z;
				child.parent_local_heading_bam =
						io::bam_sub(child.heading_bam, parent_heading_bam);
				child.parent_local_pitch_bam =
						io::bam_sub(child.pitch_bam, parent->pitch_bam);
				child.parent_local_roll_bam =
						io::bam_sub(child.roll_bam, parent->roll_bam);
				child.parent_pose_valid = true;
			}
			const WorldPose posed = network_transform_local_to_world(
					child.parent_local_x, child.parent_local_y, child.parent_local_z,
					parent->x, parent->y, parent->z,
					static_cast<uint32_t>(parent_heading_bam),
					static_cast<uint32_t>(parent->pitch_bam),
					static_cast<uint32_t>(parent->roll_bam));
			child.x = posed.x;
			child.y = posed.y;
			child.z = posed.z;
			child.heading_bam = io::bam_add(
					parent_heading_bam, child.parent_local_heading_bam);
			child.yaw_byte = yaw_byte_from_bam(child.heading_bam);
			child.pitch_bam = io::bam_add(
					parent->pitch_bam, child.parent_local_pitch_bam);
			child.roll_bam = io::bam_add(
					parent->roll_bam, child.parent_local_roll_bam);
		}
	}
	for (uint16_t handle : sweep_destroyed) erase_entity_tree(handle);
}

void ClientReplicaPipeline::apply_static_batch(const std::vector<uint8_t> &body) {
	StaticEntityBatch batch;
	if (!decode_static_entity_batch(body.data(), body.size(), batch)) {
		// The retail handler applies records as it walks the page; a
		// malformed tail loses only the unread remainder [orig: the 0x10 static handler].
		// Count the malformed page, drop the half-read record the decoder
		// staged at the failure point, and apply the complete prefix.
		++malformed_bodies_;
		if (batch.last_record_partial && !batch.records.empty())
			batch.records.pop_back();
		if (batch.records.empty()) return;
	}
	bool changed = false;
	// The 0x10 record carries no slot id — the entity's slot is start_index + iteration index.
	for (size_t i = 0; i < batch.records.size(); ++i) {
		const StaticEntityRecord &rec = batch.records[i];
		const std::size_t slot = static_cast<std::size_t>(batch.start_index) + i;
		// Retail indexes pool 2 unchecked; its own capacity (1200) is the bound a
		// stock server can reach — a 1157-entity 01TR load page walked past a
		// 1024 clamp here and silently dropped the pool tail.
		// [orig: NapiNPClientMsg_0x010 @0x433400 — Pool_GetEntryUnchecked(2, idx)
		//  @0x433487; capacity EntityPool_Allocate @0x4421a2]
		if (slot >= world::retail_pool_capacity(2)) continue;
		const uint16_t handle = static_cast<uint16_t>(0x2000u | slot);
		if (rec.is_empty_slot) {
			const std::size_t before = state_.entities.size();
			state_.entities.erase(
					std::remove_if(state_.entities.begin(), state_.entities.end(),
							[handle](const ClientEntityState &row) {
								return row.handle == handle;
							}),
					state_.entities.end());
			if (state_.entities.size() != before) {
				state_.mark_topology_changed();
				++state_.world_stream_revision;
				changed = true;
			}
			continue;
		}
		ClientEntityState *existing = state_.find(handle);
		const bool type_changed = existing != nullptr &&
				existing->type_id != rec.item_type_id;
		const uint32_t next_spawn_revision = begin_entity_lifetime(handle);
		ClientEntityState &es = state_.upsert(handle);
		if (type_changed) state_.mark_topology_changed();
		// The retail 0x10 handler memsets the selected slot before itemType.
		es = ClientEntityState{};
		es.handle = handle;
		es.type_id = rec.item_type_id;
		es.cls = classify(rec.item_type_id);
		// Like 0x0D, the 0x10 handler clears entity+124 and never rewrites it.
		es.net_id = 0;
		es.spawn_tag = s2c::STATIC_ENTITY_BATCH;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(rec.euler_z);
		es.heading_bam = rec.euler_z;
		es.heading_known = (rec.field_flags & 0x0001u) != 0;
		es.pitch_bam = rec.euler_x;
		es.roll_bam = rec.euler_y;
		es.team = rec.team_byte;
		es.team_known = (rec.field_flags & 0x0010u) != 0;
		es.spawn_entity_flags = rec.entity_flags;
		es.spawn_section_mask = static_cast<uint32_t>(rec.section_mask);
		es.spawn_ammo_count = rec.ammo_count;
		es.spawn_ref_num = rec.bone_a;
		es.spawn_sub_type = rec.bone_b;
		es.spawn_revision = next_spawn_revision;
		++state_.world_stream_revision;
		es.zone_number_rank = rec.weapon_byte;
		es.zone_radius = rec.attach_ref;
		changed = true;
	}
	if (changed) state_.mark_changed();
}

void ClientReplicaPipeline::apply_pool3_batch(const std::vector<uint8_t> &body) {
	Pool3SyncBatch batch;
	if (!decode_pool3_sync_batch(body.data(), body.size(), batch)) {
		// The retail handler applies records as it walks the page; a
		// malformed tail loses only the unread remainder [orig: the 0x20 pool-3 handler].
		// Count the malformed page, drop the half-read record the decoder
		// staged at the failure point, and apply the complete prefix.
		++malformed_bodies_;
		if (batch.last_record_partial && !batch.records.empty())
			batch.records.pop_back();
		if (batch.records.empty()) return;
	}
	bool changed = false;
	for (std::size_t i = 0; i < batch.records.size(); ++i) {
		const Pool3SyncRecord &rec = batch.records[i];
		// Retail indexes pool 3 directly from the 0x20 start index and record
		// ordinal, then stores netHandle into the already selected entity. The two
		// fields are deliberately not aliases. [orig: 0x20 handler @0x425C00,
		// Pool_GetEntryUnchecked(3, startIndex+i), netHandle store at entity+124]
		const std::size_t slot =
				static_cast<std::size_t>(batch.start_index) + i;
		if (slot >= world::retail_pool_capacity(3)) continue;
		const uint16_t handle = static_cast<uint16_t>(0x3000u | slot);
		if (rec.is_empty_slot) {
			const std::size_t before = state_.entities.size();
			state_.entities.erase(
					std::remove_if(state_.entities.begin(), state_.entities.end(),
							[handle](const ClientEntityState &row) {
								return row.handle == handle;
							}),
					state_.entities.end());
			if (state_.entities.size() != before) {
				state_.mark_topology_changed();
				++state_.world_stream_revision;
				changed = true;
			}
			continue;
		}
		ClientEntityState *existing = state_.find(handle);
		const bool type_changed = existing != nullptr &&
				existing->type_id != rec.item_type_id;
		const uint32_t next_spawn_revision = begin_entity_lifetime(handle);
		ClientEntityState &es = state_.upsert(handle);
		if (type_changed) state_.mark_topology_changed();
		// The retail 0x20 handler likewise memsets its complete selected slot.
		es = ClientEntityState{};
		es.handle = handle;
		es.type_id = rec.item_type_id;
		es.cls = classify(rec.item_type_id);
		es.net_id = rec.net_handle;
		es.spawn_tag = s2c::POOL3_SYNC;
		es.x = rec.pos_x;
		es.y = rec.pos_y;
		es.z = rec.pos_z;
		es.yaw_byte = yaw_byte_from_bam(static_cast<int32_t>(rec.movement_val));
		es.heading_bam = static_cast<int32_t>(rec.movement_val);
		es.heading_known = (rec.flags_byte & 0x01u) != 0;
		es.pitch_bam = 0; // pool-3 sync carries no entity+20/+24 Euler fields
		es.roll_bam = 0;
		es.team = rec.team_byte;
		es.team_known = (rec.flags_byte & 0x08u) != 0;
		es.spawn_entity_flags = 0;
		es.spawn_section_mask = 0;
		es.spawn_ammo_count = rec.ammo_count;
		es.spawn_bound_radius_q16 =
				static_cast<int32_t>(rec.orientation_val);
		es.spawn_ref_num = 0;
		es.spawn_sub_type = 0;
		es.spawn_revision = next_spawn_revision;
		++state_.world_stream_revision;
		es.zone_number_rank = 0;
		es.zone_radius = 0;
		changed = true;
	}
	if (changed) state_.mark_changed();
}

void ClientReplicaPipeline::apply_frame_update(const std::vector<uint8_t> &body) {
	FrameUpdate fu;
	// decode_frame_update leaves everything it walked in `fu` even on a short read,
	// so we apply whatever decoded cleanly (out.complete reflects a clean terminator).
	decode_frame_update(body.data(), body.size(),
	                    [this](uint16_t tid) { return classify(tid); }, fu,
	                    game_type::is_objective(game_type_), authority_recipient_);

	state_.anchor_x = fu.anchor_x;
	state_.anchor_y = fu.anchor_y;
	state_.anchor_z = fu.anchor_z;
	// flags1 bit 2 is the hit-feedback pulse: a set bit reloads the countdown to
	// 10, a clear bit drains one per received frame. It runs ahead of the
	// authority early-out, so the listen host folds its own.
	// [orig: NapiNPClientMsg_0x00A @0x42FF5C..0x42FF74 — dword_A8235C]
	if (fu.flags1 & 4u)
		state_.hud_hit_feedback_frames = 10;
	else if (state_.hud_hit_feedback_frames)
		--state_.hud_hit_feedback_frames;
	// The deploy-map overlay follows the host every frame — set AND cleared
	// by assignment, not edges [orig: NapiNPClientMsg_0x00A @0x42ff82 —
	// g_DeployScreenActive = (flags1 >> 1) & 1].
	state_.deploy_overlay_active = (fu.flags1 & 0x02u) != 0;
	// The death-screen edges on flags1 bit 0 [orig: @0x42ff88..0x43002b].
	{
		const bool bit = (fu.flags1 & 0x01u) != 0;
		if (bit && !state_.death_screen_active) {
			state_.death_screen_active = true;
			state_.death_screen_submode = 0;  // [orig: @0x42ffa6]
			state_.spectate_target = 0xFFFF;  // [orig: @0x42ffac]
			state_.enemy_tags_visible = true;
			++state_.death_screen_opens;        // [orig: @0x42ffb8..0x42ffc9]
			// The spectator tip, unless the round is over [orig:
			// `cmp g_SpawnSuccessGate` @0x42ffd0 -> CTipSystem_HandleEvent(22)
			// @0x42ffdf; the spectate-mode toast stamp dword_24C18F0 = 186
			// @0x42ffe4 rides the unported toast, docs/interface/hud-re.md].
			if (!state_.spawn_success_gate)
				pending_effect_commands_.push_back(TipEventCommand{22});
		} else if (!bit && state_.death_screen_active) {
			state_.death_screen_active = false;
			state_.enemy_tags_visible = false;
		}
	}
	if (fu.local_tail_present) {
		state_.local_health = fu.health;
		++state_.health_updates_applied;
		state_.local_stance_bits = static_cast<uint8_t>(fu.state_flag_byte & 0x03u); // [orig: @0x4303e5]
	}
	if (fu.weapon.present) {
		// Phase 0 is the sole retail mirror of g_PreRoundDelayTimer.
		// Retain it between phase cycles, exactly like the client global.
		// [orig: NapiNPClientMsg_0x00A @0x430064]
		state_.preround_delay_seconds = fu.weapon.preround_timer;
		state_.vehicle_reload_seconds = fu.weapon.reload_seconds;
		state_.owned_zone_mask = uint32_t(fu.weapon.uniform_team_mask);
		// The DEATH screen's three slot timers ride the same sub-block
		// [orig: @0x430084 / @0x43009f / @0x4300c3].
		state_.respawn_penalty_seconds = fu.weapon.slot_state360;
		state_.local_revive_seconds = fu.weapon.slot_state368;
		state_.spawn_hold_seconds = fu.weapon.slot_state364;
		// The underwater breath samples, the breath bar's counter
		// [orig: NapiNPClientMsg_0x00A @0x430104 -> word_A85B7C].
		state_.breath_samples = fu.weapon.slot_state460;
	}
	if (authority_recipient_) {
		// The listen host's own frame carries nothing past the phase-0 block
		// [orig: NapiNPClientMsg_0x00A @0x430174]; the timer, environment,
		// objective, tail, and every entity/round come from the host's own
		// World on this role.
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
		if (auto *shooter = state_.find(ev.shooter_handle)) shooter->fire_target_handle = ev.target_handle;
		if (round_receiver_) round_receiver_(ev);
        else pending_round_events_.push_back(ev);
	}

	++state_.frames_applied;
		state_.mark_changed();
		return;
	}
	if (fu.timer.present) {
		// The breath seconds and the fall-damage tolerance, zero-extended.
		// [orig: NapiNPClientMsg_0x00A @0x430199..0x4301A1, @0x4301B9..0x4301BC]
		state_.breathtime = fu.timer.state0;
		state_.fallmps = fu.timer.state1;
		// The round clock: 62 x the wire's whole seconds, negative = untimed
		// -1. [orig: NapiNPClientMsg_0x00A @0x430219..0x430235 —
		//  g_RoundTimeRemaining]
		state_.round_time_remaining_ticks = fu.timer.timer_seconds < 0
				? -1
				: 62 * static_cast<int32_t>(fu.timer.timer_seconds);
	}
	if (fu.objective.present) {
		state_.objective_won = static_cast<uint32_t>(fu.objective.state[0]);
		state_.objective_lost = static_cast<uint32_t>(fu.objective.state[1]);
		state_.objective_show_win = static_cast<uint32_t>(fu.objective.state[2]);
		state_.objective_show_lose = static_cast<uint32_t>(fu.objective.state[3]);
		++state_.objective_updates_applied;
	}
	if (fu.env.present) {
		state_.environment.present = true;
		state_.environment.fog_dist = fu.env.fog_dist;
		state_.environment.fog_accel = fu.env.fog_accel;
		state_.environment.tod_fixed = fu.env.tod_fixed;
		state_.environment.quake_ticks = fu.env.quake_ticks;
		state_.environment.cloud_scroll = fu.env.cloud_scroll;
		state_.environment.overcast = fu.env.overcast;
		state_.environment.rain_pct = fu.env.rain_pct;
		state_.environment.env_param = fu.env.env_param;
		++state_.environment.revision;
	}
	if (fu.passenger.present) {
		state_.mounted_ammo.present = true;
		state_.mounted_ammo.mount_handle = fu.passenger.mount_handle;
		state_.mounted_ammo.has_mount = fu.passenger.has_mount;
		state_.mounted_ammo.clip = fu.passenger.clip;
		state_.mounted_ammo.reserve = fu.passenger.reserve;
		++state_.mounted_ammo.revision;
	}

	state_.compact_records_applied += static_cast<std::uint32_t>(fu.records.size());
	for (ClientEntityState &e : state_.entities) e.seen_this_frame = false;
	struct PendingCarrierPose {
		uint16_t child_handle;
		uint16_t carrier_handle;
		uint16_t cx;
		uint16_t cy;
		uint16_t cz;
		// The carrier-RELATIVE heading every parented record carries (BAM32):
		// the player/infantry yaw byte widened, the vehicle compact's euler_z
		// high half — the writer's Entity_TransformWorldToLocal out[3] = own -
		// carrier [orig: @0x460c2a / @0x43bb87]; the reads compose it back
		// [orig: player @0x4c10d4; vehicle Entity_TransformLocalToWorld
		// @0x4608ce -> the entity+576 store @0x4607f5].
		int32_t local_heading_bam;
		// An organic record whose carrier is its groundEntity, not a seat:
		// the seat bone is 0, so Entity_TryAttachOrDetach attaches nothing and
		// the lifted world sample STAGES like a free-standing record's — the
		// row keeps its own chase [orig: player LABEL_123 stores +0x234..0x23C
		// / +0x240 @0x4c10d4, then Entity_TryAttachOrDetach(bone) @0x4c1329;
		// infantry @0x4C0320 stages +0x234 after the same attach call;
		// Entity_TryAttachOrDetach @0x436610 detaches on bone 0].
		bool ground = false;
		// The free-standing landing's respawn live snap, which a ground form
		// shares.
		bool force_live_snap = false;
		// The child's compact revision after this record: a later record for
		// the same child in this fold supersedes the sample.
		uint32_t compact_revision = 0;
	};
	std::vector<PendingCarrierPose> pending_carrier_poses;
	pending_carrier_poses.reserve(fu.records.size());

	// Fold in TWO sweeps so the witnessed child-before-carrier production
	// order still resolves: retail resolves a record's carrier against the
	// POOL, where the carrier exists from its spawn regardless of this
	// frame's record order — our row analog is created by the carrier's own
	// record, so a sweep-1 child whose carrier only appears later in the
	// same frame retries in sweep 2. A carrier absent after BOTH sweeps is
	// the retail bail: the WHOLE record drops and a C2S 0x0F entity request
	// is queued — nothing from the record lands [orig: vehicle resolve
	// @0x46085d, repair bail @0x4608ae..0x4608c1; the player op2 carrier
	// path @0x4c10d4]. Skipping only the position would half-apply
	// anim/flags/health from a sample retail never applied.
	std::vector<uint8_t> record_folded(fu.records.size(), 0u);
	for (int sweep = 0; sweep < 2; ++sweep)
	for (size_t rec_i = 0; rec_i < fu.records.size(); ++rec_i) {
		const FrameUpdateRecord &rec = fu.records[rec_i];
		if (record_folded[rec_i] != 0u) continue;
		// The retail pre-apply consistency check, run for EVERY tag-1 record
		// ahead of its class callback (no-callback types included): the
		// addressed pool slot must hold a live entity whose itemDef id and
		// defIndex match the wire type — else a C2S 0x0F entity request is
		// queued and the callback runs against a NULL target, consuming the
		// record's bytes and landing nothing. A handle outside pools 0..4 or
		// past its pool's capacity resolves to no slot at all: consumed
		// silently, no request. A compact record therefore NEVER creates or
		// re-types a row — only the spawn stream (0x0C/0x0D/0x10/0x20/0x18)
		// does, and a row the team-assign leg created ahead of its spawn
		// (type 0) is exactly retail's itemDef-less slot. [orig:
		// NapiNPClientMsg_0x00A @0x42FEC0 — the pool resolve `(handle &
		// 0xF000) < 0x5000` + `slot < capacity`, the check @0x4307B1..0x4307C4
		// (itemDef null / itemDef->id != type / entity->defIndex != idx),
		// QueueReliableMessage(0x0F, [u16 handle]) @0x4307E9, the target
		// nulled @0x4307F2..0x4307FA]
		{
			const int pool = rec.handle >> 12;
			const bool slot_exists = rec.handle != wire_handle::kInvalid &&
					pool < world::kEntityPoolCount &&
					static_cast<std::size_t>(rec.handle & 0x0FFFu) <
							world::retail_pool_capacity(pool);
			const ClientEntityState *slot_row = state_.find(rec.handle);
			if (!slot_exists || slot_row == nullptr || slot_row->type_id != rec.type_id) {
				if (slot_exists) queue_carrier_repair(rec.handle);
				record_folded[rec_i] = 1u;
				continue;
			}
		}
		if (rec.cls == EntityClass::NoNetworkCallback) {
			record_folded[rec_i] = 1u;
			continue;
		}
		uint16_t record_carrier = wire_handle::kInvalid;
		if (rec.cls == EntityClass::Player)
			record_carrier = rec.player.carrier_handle;
		else if (rec.cls == EntityClass::Vehicle)
			record_carrier = rec.vehicle.parent_slot_handle;
		else if (rec.cls == EntityClass::Infantry)
			record_carrier = rec.infantry.vehicle_slot_handle;
		if (record_carrier != wire_handle::kInvalid &&
				state_.find(record_carrier) == nullptr) {
			if (sweep == 0) continue; // the carrier may appear this frame
			queue_carrier_repair(record_carrier);
			record_folded[rec_i] = 1u;
			continue;
		}
		record_folded[rec_i] = 1u;
		// The row exists with this exact type (the pre-apply check above): a
		// compact never upserts, and a handle reused for a different type is
		// re-armed by the spawn stream that re-typed it, never here.
		ClientEntityState &es = *state_.find(rec.handle);
		// Capture the previous body-anim sample before the per-record clear: if
		// this record REPLACES it with a different state within one decode fold,
		// the old value becomes the transition PULSE presentation still has to
		// dispatch — retail applies each record's anim byte through the receive
		// arbitration as it decodes [orig: @0x4c1153], and a tapped prone roll
		// rides the wire for only 1-2 ticks (the byte is `pending ?: current`).
		// A row's first-ever organic sample never pulses (its default 0 would
		// read as the anim_reset clip).
		const bool prev_anim_sampled = es.cls == EntityClass::Player ||
		                               es.cls == EntityClass::Infantry;
		const uint8_t prev_anim_state = es.anim_state_id;
		const uint8_t prev_anim_ratio = es.anim_channel_ratio;
		es.cls = rec.cls;
		es.seen_this_frame = true;
		// Every compact record is a complete sample of these organic fields. Clear the
		// normalized row before class-specific assignment so dismounts and class changes
		// cannot retain a stale carrier/bone selector from an earlier frame.
		es.carrier_handle = wire_handle::kInvalid;
		es.mount_bone = 0;
		es.seat_type = 0;
		// Carrier identity and its resolved local pose form one atomic sample.
		// The second pass re-arms this only if the final carrier resolves.
		es.net_seat_valid = false;
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
		} else if (rec.cls == EntityClass::Vehicle) {
			// Vehicles carry the wire flags too; their dead marker is the
			// dead-pose/wreck bit (flags & 4), not the organic bit 1
			// [orig: the short-form select on flags_byte, §5.13].
			has_state_flags = true;
			state_flags = rec.vehicle.flags_byte;
		}
		const uint8_t dead_bit = rec.cls == EntityClass::Vehicle
				? kVehicleFlagDeadPose
				: static_cast<uint8_t>(world::kEntityFlagDead);
		bool respawned_this_record = false;
		bool row_was_dead = false;
		if (has_state_flags) {
			const bool was_known = es.state_flags_known;
			const bool was_dead = (es.state_flags & dead_bit) != 0u;
			const bool is_alive = (state_flags & dead_bit) == 0u;
			row_was_dead = was_known && was_dead;
			es.state_flags = state_flags;
			es.state_flags_known = true;
			if (was_known && was_dead && is_alive) {
				++es.respawn_revision;
				respawned_this_record = true;
			}
		}
		// The organic wire-dead position skip (LABEL_151). Vehicle wrecks are the
		// opposite: the dead-pose short form force-live-snaps the frozen pose.
		const bool wire_dead = has_state_flags &&
				rec.cls != EntityClass::Vehicle &&
				(state_flags & dead_bit) != 0u;

		// Reconstruct world position: decompress the compact (per-axis) and add the
		// frame anchor — or, for a CARRIER-LOCAL player record (vehicle/ground handle !=
		// 0xFFFF, D-NET-151), lift the local offset through the carrier's pose from this
		// view's own state [orig: op2 resolves the carrier from g_PoolList and runs
		// Entity_TransformLocalToWorld @0x4c10d4; a carrier with no itemDef DROPS the
		// record and queues a C2S 0x0F entity request — request plumbing an in-process
		// view does not need, so an unknown carrier just skips the position sample].
		// Carrier-local samples are queued for a second pass after every record has
		// updated the view. Production order is pool-0 child before pool-1 carrier.
		uint16_t cx = 0, cy = 0, cz = 0;
		bool skip_pos = false;
		// The record's decoded orientation target (BAM32). Players/infantry carry
		// the 8-bit yaw high byte; vehicles the 16-bit euler_z high half — the
		// wire precision each class actually has (§5.38e).
		int32_t heading_target = 0;
		bool has_heading_target = false;
		// An organic record's carrier with seat bone 0 is its groundEntity (a
		// roof, a bridge, a deck it stands on), not a seat: the row stays
		// free-standing and only its sample is carrier-local (PendingCarrierPose
		// ::ground). carrier_handle keeps the seat relation alone.
		uint16_t ground_carrier = wire_handle::kInvalid;
		int32_t ground_local_heading = 0;
		switch (rec.cls) {
		case EntityClass::Player:
			cx = rec.player.pos_x_compressed;
			cy = rec.player.pos_y_compressed;
			cz = rec.player.pos_z_compressed;
			es.carrier_handle = rec.player.vehicle_bone != 0
					? rec.player.carrier_handle : wire_handle::kInvalid;
			es.mount_bone = rec.player.vehicle_bone;
			es.seat_type = rec.player.seat_type;
			es.pitch_byte = rec.player.pitch_byte;
			es.anim_state_id = rec.player.anim_state_id;
			es.anim_channel_ratio = rec.player.anim_channel_ratio;
			es.equipped_adm_index = rec.player.anim_def_index;
			es.state_flags = rec.player.state_flags;
			es.move_input = rec.player.move_input_byte;
			es.health_class_byte = rec.player.health_class_byte;
			if (es.carrier_handle != wire_handle::kInvalid) {
				es.pitch_bam = static_cast<int32_t>(static_cast<uint32_t>(es.pitch_byte) << 24);
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.player.carrier_handle, cx, cy, cz,
						static_cast<int32_t>(
								static_cast<uint32_t>(rec.player.yaw_byte) << 24)});
				skip_pos = true;
			} else if (rec.player.carrier_handle != wire_handle::kInvalid) {
				ground_carrier = rec.player.carrier_handle;
				ground_local_heading = static_cast<int32_t>(
						static_cast<uint32_t>(rec.player.yaw_byte) << 24);
				skip_pos = true;
			} else {
				es.yaw_byte = rec.player.yaw_byte;
				heading_target = static_cast<int32_t>(
						static_cast<uint32_t>(rec.player.yaw_byte) << 24);
				has_heading_target = true;
			}
			break;
		case EntityClass::Vehicle:
			cx = rec.vehicle.pos_x_compressed;
			cy = rec.vehicle.pos_y_compressed;
			cz = rec.vehicle.pos_z_compressed;
			// The §5.13 compact's own parent field is the CARRIER (deck/ground
			// entity), consumed per record: a resolving parent composes THIS
			// record's vehicle-local position against the carrier's live pose,
			// an absent one takes the anchor-relative leg, and the stored
			// carrier ref is re-landed (nulled included) from every record
			// [orig: Entity_SerializeVehicleState read side — resolve
			// @0x46085d, local->world @0x4608ce, entity+40 (re)store
			// @0x460802]. The 0x0D spawn's parentHandle is a DIFFERENT slot —
			// occupantEntity/+368, a driver back-reference with no transform
			// semantics [orig: store @0x433289] — see
			// refresh_parented_pool_entities for the class gate that keeps it
			// out of this row's pose.
			// The carrier ref is consumed per record like the organic classes
			// (re-landed nulled included, D-NET-195) so the per-tick seat-follow
			// can ride a resolving deck carrier between records.
			es.carrier_handle = rec.vehicle.parent_slot_handle;
			if (rec.vehicle.parent_slot_handle != wire_handle::kInvalid) {
				// The parented form's euler_z is the CARRIER-LOCAL heading (the
				// writer's Entity_TransformWorldToLocal out[3] = own - carrier
				// @0x460c2a); the reader composes position AND heading through
				// Entity_TransformLocalToWorld into the same pose buffer before
				// the entity+576 store [orig: @0x4608ce, out[3] = carrier[3] +
				// local[3] @0x43bd00, then @0x4607f1..0x4607f5], so the world
				// heading lands with the position in the second pass.
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.vehicle.parent_slot_handle, cx, cy, cz,
						static_cast<int32_t>(rec.vehicle.euler_z) * 65536});
				skip_pos = true;
			} else {
				// Free-standing: the wire euler_z is the world heading (entity+16
				// @0x460cec) at its full 16-bit precision.
				es.yaw_byte = static_cast<uint8_t>(
						static_cast<uint16_t>(rec.vehicle.euler_z) >> 8);
				heading_target = static_cast<int32_t>(rec.vehicle.euler_z) * 65536;
				has_heading_target = true;
			}
			es.health_word = rec.vehicle.health_word;
			es.health_known = true;
			// The raw speed register mirror ([177] source field) — gates the
			// fast-vehicle snap threshold and decays on starvation (§5.38e §4).
			if (!rec.vehicle.is_dead_pose) {
				es.vehicle_speed_reg =
						network_decompress_fixedpoint(rec.vehicle.weapon_aim_y);
				es.vehicle_steer_bam = static_cast<int32_t>(
						rec.vehicle.weapon_heading_bam) * 65536;
				es.vehicle_lat_reg =
						network_decompress_fixedpoint(rec.vehicle.weapon_aim_z);
			}
			// entity+0xA0 slideDecay (the vertical velocity the family prediction
			// integrates) lands from every record whose WIRE flags clear bit 0x02;
			// the dead-pose form carries 0 there [orig: `and esi,2; jnz` on the
			// wire byte @0x460911..0x460918, the store @0x46091e; the short
			// form's weaponX = 0 @0x460684].
			if ((rec.vehicle.flags_byte & 0x02u) == 0u) {
				es.vehicle_vertical_velocity = rec.vehicle.is_dead_pose
						? 0
						: network_decompress_fixedpoint(rec.vehicle.vertical_velocity);
				es.vehicle_vertical_velocity_pending = true;
			}
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
			es.carrier_handle = rec.infantry.seat_bone_idx != 0
					? rec.infantry.vehicle_slot_handle : wire_handle::kInvalid;
			es.mount_bone = rec.infantry.seat_bone_idx;
			es.pitch_byte = rec.infantry.pitch_byte;
			es.aim_yaw_byte = rec.infantry.aim_yaw_byte;
			es.anim_state_id = rec.infantry.anim_byte;
			es.state_flags = rec.infantry.flags_byte;
			// The compact carries desired aim pitch (entity+0x2D0), not live
			// entity+0x14. Retail's remote gunner rebuilds the latter locally
			// with the same wrapped one-eighth chase as authoritative AI.
			// [orig: chase @0x4bef7b..0x4bef97]
			if (es.carrier_handle != wire_handle::kInvalid) {
				es.pitch_bam = chase_infantry_pitch(
						es.pitch_bam, rec.infantry.aim_yaw_byte);
			}
			if (es.carrier_handle != wire_handle::kInvalid) {
				pending_carrier_poses.push_back(PendingCarrierPose{
						rec.handle, rec.infantry.vehicle_slot_handle, cx, cy, cz,
						static_cast<int32_t>(
								static_cast<uint32_t>(rec.infantry.yaw_byte) << 24)});
				skip_pos = true;
			} else if (rec.infantry.vehicle_slot_handle != wire_handle::kInvalid) {
				ground_carrier = rec.infantry.vehicle_slot_handle;
				ground_local_heading = static_cast<int32_t>(
						static_cast<uint32_t>(rec.infantry.yaw_byte) << 24);
				skip_pos = true;
			} else {
				es.yaw_byte = rec.infantry.yaw_byte;
				heading_target = static_cast<int32_t>(
						static_cast<uint32_t>(rec.infantry.yaw_byte) << 24);
				has_heading_target = true;
			}
			break;
		default:
			break; // unresolved/guided records are not compact motion samples
		}
		// Latch the overwritten body-anim state as this row's transition pulse.
		// LAST transition wins: in a fold of [41, 48] the 41 is the state being
		// buried (the 48 latched by the first transition was already presented
		// last frame, and re-dispatching a presented state is a same-state
		// no-op at the model). The presenter drains the pulse once per frame.
		// (The pulse survives as the DISARMED-row fallback + the legacy
		// publish rollback seam; armed rows now run the per-record
		// arbitration below — D-NET-209.)
		if (prev_anim_sampled &&
				(rec.cls == EntityClass::Player ||
						rec.cls == EntityClass::Infantry) &&
				es.anim_state_id != prev_anim_state) {
			es.anim_state_pulse = static_cast<int16_t>(prev_anim_state);
			es.anim_pulse_ratio = prev_anim_ratio;
		}
		// The per-record receive arbitration (the D-NET-209 closure): every
		// player/infantry record applies its anim byte through the retail FSM
		// pair as it decodes — several records folded between presents each
		// arbitrate in arrival order, exactly the @0x4c1153 shape.
		if (rec.cls == EntityClass::Player || rec.cls == EntityClass::Infantry) {
			apply_record_body_arbitration(es, es.anim_state_id,
					es.anim_channel_ratio,
					rec.cls == EntityClass::Player, wire_dead, row_was_dead,
					respawned_this_record);
			// The death edge's inputs (client_state.h net_death_anim). A dead
			// record zeroes Health and, on a live row, parks its byte; the
			// respawn edge clears the latch and raises Health; an alive player
			// record re-derives Health from its class byte (never zero)
			// [orig: @0x4c10f5/@0x4c10fb, @0x4c1027, infantry @0x4c04e1/
			// @0x4c0509; Entity_ResetToSpawnState Flags &= ~2 @0x4b97b0 and
			// Entity_RaiseHealthToMax @0x4b97b4; Entity_SetHealthFromDifficultyByte
			// @0x4c11ba, the local floor at 1 @0x4c11ce].
			if (wire_dead) {
				es.net_health_zero = true;
				if (!row_was_dead) es.net_death_anim = es.anim_state_id;
			} else if (respawned_this_record) {
				es.rm_entity_flags &= ~world::kEntityFlagDead;
				es.net_health_zero = false;
			} else if (rec.cls == EntityClass::Player) {
				es.net_health_zero = false;
			}
            // [orig: NetPacket_SerializePlayerState @ 0x4C09C0, stance stores @ 0x4C11D7..0x4C1242]
            // The committed FSM state determines MoveOrder, including when
            // the just-received animation was deferred into the pending slot.
            if (rec.cls == EntityClass::Player) {
                const uint32_t flags = world::infantry_anim_flags(es.net_anim_current);
                es.net_stance_bits = static_cast<uint8_t>(
                        ((flags & 0x100u) ? 2 : 0) | ((flags & 0x200u) ? 1 : 0));
            }
		}
		if (rec.cls == EntityClass::Player || rec.cls == EntityClass::Infantry ||
				rec.cls == EntityClass::Vehicle) {
			es.heading_known = true;
			++es.compact_revision;
		}
		// Root-channel lifecycle at the organic freeze/respawn edges: a frozen
		// row (bit0/carried) never root-ticks, so its channel is DISARMED —
		// presentation falls back to the per-record wire anim byte exactly as
		// retail applies it [orig: @0x4c1153] (the seat clips dispatch). A
		// dead row is not frozen: its mover runs and the death edge commits
		// the death clip to the channel. The respawn edge keeps the channel
		// (the next state blends out of the death clip) and resets what
		// Entity_ResetToSpawnState resets on the row: the leg and body yaws
		// re-seed to the new heading, and the velocities zero
		// [orig: @0x4b962c velocityX/Y + slideDecay = 0; @0x4b968c..0x4b96b0
		// bodyHeading and the four leg yaws = Yaw]. (The one-shot phase seed
		// rides the arbitration's direct-commit leg — net_anim_ratio.)
		if (rec.cls == EntityClass::Player || rec.cls == EntityClass::Infantry) {
			const bool row_frozen =
					(has_state_flags && (state_flags & 0x01u) != 0u) ||
					es.carrier_handle != wire_handle::kInvalid;
			if (row_frozen) {
				row_channel_disarm(es);
			} else if (respawned_this_record) {
				es.rm_leg_seeded = false;
				es.rm_vel_xy[0] = 0;
				es.rm_vel_xy[1] = 0;
				es.rm_vel_z = 0;
			}
		}
		// A free-standing record clears any retained seat-local pose — the
		// relation is cleared before every record (D-NET-195); carrier-local
		// records atomically refresh it in the second pass below.
		// A vehicle wreck's short form re-lands the full frozen orientation;
		// treat it as the live-snap branch of the read [orig: the conditional
		// live stores @0x460930..0x460A50].
		const bool force_live_snap = respawned_this_record ||
				(rec.cls == EntityClass::Vehicle && rec.vehicle.is_dead_pose);
		// The compact apply's ground-link store, every organic record, nulled
		// included: an attached row takes its seat's ground link, any other
		// the record's carrier [orig: player @0x4c1353 — mount(+0x16C) ?
		// mount->groundEntity : carrier; infantry the same pair at the tail of
		// @0x4C0320]. The mover's ground probe re-stores it within the tick.
		if (rec.cls == EntityClass::Player || rec.cls == EntityClass::Infantry) {
			if (es.carrier_handle != wire_handle::kInvalid) {
				const ClientEntityState *mount = state_.find(es.carrier_handle);
				es.resolved_ground = mount != nullptr ? mount->resolved_ground
				                                      : wire_handle::kInvalid;
			} else {
				es.resolved_ground = ground_carrier;
			}
		}
		if (ground_carrier != wire_handle::kInvalid) {
			pending_carrier_poses.push_back(PendingCarrierPose{
					rec.handle, ground_carrier, cx, cy, cz, ground_local_heading,
					true, force_live_snap, es.compact_revision});
		}
		if (!skip_pos) {
			// A wire-dead record stages like any other: the player read stores
			// the target cluster BEFORE it tests the dead bit, and the infantry
			// read stages after its dead/respawn legs, so a corpse's records
			// keep steering its mover [orig: player stores @0x4c0fe4..0x4c0ffc,
			// the dead test's jz @0x4c1005; infantry @0x4c0689..0x4c06aa].
			const int32_t wx = fu.anchor_x + network_decompress_fixedpoint(cx);
			const int32_t wy = fu.anchor_y + network_decompress_fixedpoint(cy);
			const int32_t wz = fu.anchor_z + network_decompress_fixedpoint(cz);
			land_compact_pose(es, wx, wy, wz, has_heading_target, heading_target,
			                  force_live_snap);
		}
	}

	// Resolve carrier-local children only after the complete frame has upserted and
	// updated every carrier. If the carrier is genuinely absent, leave the child's
	// prior world pose/yaw untouched, matching the retail record-drop path.
	for (const PendingCarrierPose &pending : pending_carrier_poses) {
		ClientEntityState *child = state_.find(pending.child_handle);
		const ClientEntityState *carrier = state_.find(pending.carrier_handle);
		if (pending.ground) {
			// A ground-linked record lands like a free-standing one: the lifted
			// world sample (heading = carrier + local, the transform's out[3])
			// is staged and the row's own mover chases it; the deck ride
			// follows the ground link between records.
			if (child == nullptr || carrier == nullptr ||
					child->compact_revision != pending.compact_revision)
				continue;
			const WorldPose w = network_transform_local_to_world(
					network_decompress_fixedpoint(pending.cx),
					network_decompress_fixedpoint(pending.cy),
					network_decompress_fixedpoint(pending.cz), carrier->x, carrier->y,
					carrier->z, uint32_t(carrier->heading_bam),
					uint32_t(carrier->pitch_bam), uint32_t(carrier->roll_bam));
			const int32_t heading =
					io::bam_add(carrier->heading_bam, pending.local_heading_bam);
			child->yaw_byte = yaw_byte_from_bam(heading);
			land_compact_pose(*child, w.x, w.y, w.z, true, heading,
			                  pending.force_live_snap);
			continue;
		}
		// A fold may contain several records for one child. An earlier resolved
		// carrier must not overwrite a later unresolved switch/dismount.
		if (child == nullptr || carrier == nullptr ||
				child->carrier_handle != pending.carrier_handle)
			continue;
		// Compose against the carrier's LIVE (chased) pose — retail lifts through
		// the carrier entity's current +4..+0x18 block [orig: @0x4c10d4/@0x4608ce].
		const WorldPose w = network_transform_local_to_world(
				network_decompress_fixedpoint(pending.cx),
				network_decompress_fixedpoint(pending.cy),
				network_decompress_fixedpoint(pending.cz), carrier->x, carrier->y,
				carrier->z, uint32_t(carrier->heading_bam),
				uint32_t(carrier->pitch_bam), uint32_t(carrier->roll_bam));
		// Retain the seat-local offset for the per-tick carrier-follow: the
		// rider's rendered pose rides the carrier attach every frame in retail
		// [orig: the seat attach @0x4946D0/@0x494752]. The row is
		// live-snapped here (the recompose owns it from the next tick).
		child->net_seat_local[0] = network_decompress_fixedpoint(pending.cx);
		child->net_seat_local[1] = network_decompress_fixedpoint(pending.cy);
		child->net_seat_local[2] = network_decompress_fixedpoint(pending.cz);
		child->net_seat_local_heading_bam = pending.local_heading_bam;
		child->net_seat_valid = true;
		child->net_has_compact = true;
		child->x = w.x;
		child->y = w.y;
		child->z = w.z;
		// World heading = carrier + local for every carried class: the player
		// read composes its yaw byte [orig: @0x4c10d4] and the vehicle read
		// lifts its eulerZ through Entity_TransformLocalToWorld's out[3] =
		// carrier[3] + local[3] [orig: @0x4608ce / @0x43bd00 -> the entity+576
		// store @0x4607f5]. BAM addition holds in the yaw byte's 8-bit ring.
		child->heading_bam = io::bam_add(carrier->heading_bam, pending.local_heading_bam);
		child->yaw_byte = yaw_byte_from_bam(child->heading_bam);
		// A deck-carried vehicle is a carried OBJECT (the bit0-flagged attach
		// class @0x43C14A) whose mover is bit0-skipped — the per-tick
		// seat-follow owns its motion between records, so nothing chases a
		// staged heading; keep the staged slot coherent with the landed pose for
		// a later carrier-clear record [orig: the +0x240 stage @0x4607f5].
		if (child->cls == EntityClass::Vehicle)
			child->net_smooth_heading = child->heading_bam;
	}

	refresh_carried_entities();

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
		if (auto *shooter = state_.find(ev.shooter_handle)) shooter->fire_target_handle = ev.target_handle;
		if (round_receiver_) round_receiver_(ev);
        else pending_round_events_.push_back(ev);
	}

	++state_.frames_applied;
	state_.mark_changed();
}

} // namespace opennova::replication
