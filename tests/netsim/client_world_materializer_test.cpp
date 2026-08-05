// A retail joiner owns only the 616-byte BMS header. Pools 1-3 therefore have
// to be materialized from the decoded initial-state stream, at the exact packed
// handles the host sent, before world-side deploy/mount/collision consumers run.

#include <netsim/client_replica_pipeline.h>
#include <netsim/client_world_materializer.h>

#include <npwire/ingame_encode.h>
#include <world/angle.h>
#include <world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

namespace nw = opennova;
namespace ns = opennova::netsim;
namespace w = opennova::world;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool exact_registry_slot_contract() {
	w::EntityRegistry registry;
	registry.configure_pool(1, 1024);
	w::Entity seed;
	seed.item_id = 5008;

	const w::EntityHandle wanted = w::EntityHandle::make(1, 1023);
	const w::EntityHandle actual = registry.spawn_at(wanted, seed);
	if (!expect(actual == wanted && registry.get(wanted) != nullptr,
			"spawn_at preserves a high retail pool/slot identity"))
		return false;
	if (!expect(!registry.spawn_at(wanted, seed).valid(),
			"spawn_at rejects an occupied exact slot"))
		return false;
	if (!expect(!registry.spawn_at(w::EntityHandle::make(1, 1024), seed).valid(),
			"spawn_at rejects a slot outside the configured retail capacity"))
		return false;
	return true;
}

bool registry_lifetime_rejects_handle_reuse() {
	w::EntityRegistry registry;
	registry.configure_pool(1, 8);
	const w::EntityHandle handle = w::EntityHandle::make(1, 3);
	w::Entity first;
	first.item_id = 5008;
	if (!expect(registry.spawn_at(handle, first) == handle,
			"the first exact-slot lifetime is allocated"))
		return false;
	const w::Entity *first_live = registry.get(handle);
	const w::EntityLifetime first_lifetime{
			handle, first_live != nullptr ? first_live->registry_spawn_id : 0};

	registry.despawn(handle);
	w::Entity replacement;
	replacement.item_id = 5009;
	if (!expect(registry.spawn_at(handle, replacement) == handle,
			"the packed handle can be reused by a later lifetime"))
		return false;
	const w::Entity *replacement_live = registry.get(handle);
	const uint64_t replacement_id = replacement_live != nullptr
			? replacement_live->registry_spawn_id
			: 0;

	return expect(first_lifetime.registry_spawn_id != 0 &&
			registry.get(first_lifetime) == nullptr &&
			!registry.despawn(first_lifetime) &&
			registry.get(handle) != nullptr &&
			registry.get(handle)->registry_spawn_id == replacement_id,
			"a stale lifetime token cannot read or despawn a replacement at the same handle");
}

bool malformed_load_indices_do_not_alias_low_slots() {
	ns::ClientReplicaPipeline pipeline;

	nw::PoolSpawnRecord pool1;
	pool1.slot_id = 0x1000;
	pool1.item_type_id = 5008;
	nw::PoolSpawnBatch pool1_batch;
	pool1_batch.records.push_back(pool1);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(pool1_batch));

	nw::StaticEntityRecord pool2;
	pool2.item_type_id = 0x0600;
	nw::StaticEntityBatch pool2_batch;
	pool2_batch.start_index = 0;
	pool2_batch.records.push_back(pool2);
	pipeline.apply(0x10, nw::encode_static_entity_batch(pool2_batch));

	nw::Pool3SyncRecord pool3;
	pool3.item_type_id = 6002;
	pool3.net_handle = 0x0444;
	nw::Pool3SyncBatch pool3_batch;
	pool3_batch.start_index = 0;
	pool3_batch.records.push_back(pool3);
	pipeline.apply(0x20, nw::encode_pool3_sync_batch(pool3_batch));

	const std::size_t count_before = pipeline.state().entities.size();
	const uint64_t revision_before = pipeline.state().revision;
	const uint64_t stream_revision_before =
			pipeline.state().world_stream_revision;
	const uint32_t p1_spawn_revision =
			pipeline.state().find(0x1000)->spawn_revision;
	const uint32_t p2_spawn_revision =
			pipeline.state().find(0x2000)->spawn_revision;
	const uint32_t p3_spawn_revision =
			pipeline.state().find(0x3000)->spawn_revision;

	pool1.item_type_id = 5009;
	pool1_batch.records.clear();
	pool1.slot_id = 0x1400; // pool 1, slot 1024: just beyond capacity
	pool1_batch.records.push_back(pool1);
	pool1.slot_id = 0x2001; // wrong pool nibble for a 0x0D record
	pool1_batch.records.push_back(pool1);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(pool1_batch));

	pool2.item_type_id = 0x0601;
	pool2_batch.start_index = 1024;
	pool2_batch.records.clear();
	pool2_batch.records.push_back(pool2);
	pipeline.apply(0x10, nw::encode_static_entity_batch(pool2_batch));

	pool3.item_type_id = 6003;
	pool3_batch.start_index = 4096;
	pool3_batch.records.clear();
	pool3_batch.records.push_back(pool3);
	pipeline.apply(0x20, nw::encode_pool3_sync_batch(pool3_batch));

	return expect(pipeline.state().entities.size() == count_before &&
			pipeline.state().revision == revision_before &&
			pipeline.state().world_stream_revision == stream_revision_before &&
			pipeline.state().find(0x1000)->type_id == 5008 &&
			pipeline.state().find(0x1000)->spawn_revision == p1_spawn_revision &&
			pipeline.state().find(0x2000)->type_id == 0x0600 &&
			pipeline.state().find(0x2000)->spawn_revision == p2_spawn_revision &&
			pipeline.state().find(0x3000)->type_id == 6002 &&
			pipeline.state().find(0x3000)->spawn_revision == p3_spawn_revision,
			"over-capacity/wrong-pool load indices cannot alias low slots");
}

bool external_same_type_reuse_is_never_mutated_or_retired() {
	ns::ClientReplicaPipeline pipeline;
	nw::PoolSpawnRecord row;
	row.slot_id = 0x1002;
	row.item_type_id = 5008;
	row.entity_name = "Wire lifetime";
	nw::PoolSpawnBatch batch;
	batch.records.push_back(row);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(batch));

	w::World world;
	world.registry.configure_pool(1, 8);
	ns::ClientWorldMaterializer materializer;
	const w::EntityHandle handle{row.slot_id};
	const ns::ClientWorldSyncResult initial =
			materializer.sync(pipeline.state(), world);
	if (!expect(initial.spawned.size() == 1 &&
			initial.spawned.front().handle == handle &&
			initial.spawned.front().registry_spawn_id != 0 &&
			materializer.owned(world, handle) != nullptr,
			"the wire row initially owns its exact slot"))
		return false;
	const w::EntityLifetime wire_lifetime = initial.spawned.front();

	world.registry.despawn(handle);
	w::Entity foreign;
	foreign.item_id = row.item_type_id;
	foreign.name = "Foreign replacement";
	foreign.position = {91.0f, 92.0f, 93.0f};
	foreign.emplacement_parent = w::EntityHandle{0x1003};
	foreign.emplacement_parent_spawn_id = 777;
	if (!expect(world.registry.spawn_at(handle, foreign) == handle,
			"an external system can reuse the exact slot"))
		return false;
	const uint64_t foreign_id = world.registry.get(handle)->registry_spawn_id;

	row.entity_name = "Repeated wire lifetime";
	row.pos_x = 7 * 65536;
	batch.records.clear();
	batch.records.push_back(row);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(batch));
	const ns::ClientWorldSyncResult repeated =
			materializer.sync(pipeline.state(), world);
	const w::Entity *survivor = world.registry.get(handle);
	if (!expect(repeated.updated.empty() && repeated.retired.size() == 1 &&
			repeated.retired.front().handle == wire_lifetime.handle &&
			repeated.retired.front().registry_spawn_id ==
					wire_lifetime.registry_spawn_id &&
			materializer.owned(world, handle) == nullptr && survivor != nullptr &&
			survivor->registry_spawn_id == foreign_id &&
			survivor->name == "Foreign replacement" &&
			std::fabs(survivor->position.x - 91.0f) < 0.0001f &&
			survivor->emplacement_parent == w::EntityHandle{0x1003} &&
			survivor->emplacement_parent_spawn_id == 777,
			"a repeated wire row cannot update, parent, or retain ownership of a foreign replacement lifetime"))
		return false;

	auto &rows = pipeline.state().entities;
	rows.erase(std::remove_if(rows.begin(), rows.end(),
			[&](const ns::ClientEntityState &candidate) {
				return candidate.handle == row.slot_id;
			}), rows.end());
	const ns::ClientWorldSyncResult removed =
			materializer.sync(pipeline.state(), world);
	survivor = world.registry.get(handle);
	return expect(removed.retired.empty() && survivor != nullptr &&
			survivor->registry_spawn_id == foreign_id &&
			survivor->name == "Foreign replacement",
			"a tombstoned wire row cannot retire or despawn a foreign replacement lifetime twice");
}

bool preoccupied_exact_slot_requires_a_fresh_wire_generation() {
	ns::ClientReplicaPipeline pipeline;
	nw::PoolSpawnRecord row;
	row.slot_id = 0x1004;
	row.item_type_id = 5008;
	row.entity_name = "Wire claimant";
	nw::PoolSpawnBatch batch;
	batch.records.push_back(row);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(batch));

	w::World world;
	world.registry.configure_pool(1, 8);
	const w::EntityHandle handle{row.slot_id};
	w::Entity foreign;
	foreign.item_id = 5009;
	foreign.name = "Preexisting lifetime";
	if (!expect(world.registry.spawn_at(handle, foreign) == handle,
			"the exact slot begins under another owner"))
		return false;
	const uint64_t foreign_id = world.registry.get(handle)->registry_spawn_id;

	ns::ClientWorldMaterializer materializer;
	const ns::ClientWorldSyncResult conflicted =
			materializer.sync(pipeline.state(), world);
	if (!expect(conflicted.spawned.empty() &&
			materializer.owned(world, handle) == nullptr &&
			world.registry.get(handle) != nullptr &&
			world.registry.get(handle)->registry_spawn_id == foreign_id &&
			world.registry.get(handle)->name == "Preexisting lifetime",
			"a first-fold exact-slot conflict cannot evict the live owner"))
		return false;

	world.registry.despawn(handle);
	if (!expect(materializer.sync(pipeline.state(), world).spawned.empty() &&
			world.registry.get(handle) == nullptr,
			"freeing the conflict does not resurrect an already-consumed wire generation"))
		return false;

	row.entity_name = "Fresh wire generation";
	batch.records.clear();
	batch.records.push_back(row);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(batch));
	const ns::ClientWorldSyncResult fresh =
			materializer.sync(pipeline.state(), world);
	return expect(fresh.spawned.size() == 1 &&
			fresh.spawned.front().handle == handle &&
			fresh.spawned.front().registry_spawn_id != 0 &&
			materializer.owned(world, handle) == world.registry.get(handle) &&
			world.registry.get(handle) != nullptr &&
			world.registry.get(handle)->name == "Fresh wire generation" &&
			world.registry.get(handle)->registry_spawn_id != foreign_id,
			"a later wire generation can claim the now-empty exact slot");
}

bool wire_target_authors_ground_separately_from_parent() {
	ns::ClientReplicaPipeline pipeline;
	nw::PoolSpawnRecord hull;
	hull.slot_id = 0x1002;
	hull.item_type_id = 5008;
	nw::PoolSpawnRecord occupant_ref;
	occupant_ref.slot_id = 0x1004;
	occupant_ref.item_type_id = 5010;
	nw::PoolSpawnRecord gun;
	gun.slot_id = 0x1003;
	gun.item_type_id = 5009;
	// The 0x0D parent and target are two relationships: parent is the
	// occupant/driver BACK-REF for an occupied mount, the separate flag-0x0200
	// target field authors retail groundEntity (+40) — the DRIVING hull a boat
	// gun rides. The materializer must never let the parent author the
	// structural carrier. [orig: NapiNPClientMsg_0x00D @0x432C40 — parent →
	// occupantEntity (+368) store @0x433289; target → groundEntity
	// resolve @0x4332bc, store @0x4332d7]
	gun.parent_handle = occupant_ref.slot_id;
	gun.target_handle = hull.slot_id;
	nw::PoolSpawnBatch batch;
	batch.records = {hull, occupant_ref, gun};
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(batch));

	w::World world;
	world.registry.configure_pool(1, 8);
	ns::ClientWorldMaterializer materializer;
	const ns::ClientWorldSyncResult first =
			materializer.sync(pipeline.state(), world);
	const w::EntityHandle hull_h{hull.slot_id};
	const w::EntityHandle occupant_h{occupant_ref.slot_id};
	const w::EntityHandle gun_h{gun.slot_id};
	const w::Entity *live_occupant = materializer.owned(world, occupant_h);
	const w::Entity *live_gun = materializer.owned(world, gun_h);
	if (!expect(first.spawned.size() == 3 && live_occupant != nullptr &&
			live_gun != nullptr && live_gun->emplacement_parent == occupant_h &&
			live_gun->ground_target == hull_h &&
			live_gun->emplacement_parent_spawn_id ==
					live_occupant->registry_spawn_id,
			"the 0x0D target authors retail groundEntity; the parent stays a back-ref"))
		return false;

	// Losing the materializer-owned TARGET lifetime must clear the structural
	// carrier; an unrelated replacement at the same packed handle cannot be
	// adopted by the next fold. The parent relation is independent and
	// survives.
	world.registry.despawn(hull_h);
	if (!expect(world.registry.get(hull_h) == nullptr,
			"the materialized target lifetime can be retired"))
		return false;
	w::Entity foreign;
	foreign.item_id = hull.item_type_id;
	if (!expect(world.registry.spawn_at(hull_h, foreign) == hull_h,
			"a foreign target replacement can reuse the packed handle"))
		return false;
	materializer.sync(pipeline.state(), world);
	live_gun = materializer.owned(world, gun_h);
	return expect(live_gun != nullptr &&
			!live_gun->ground_target.valid() &&
			live_gun->emplacement_parent == occupant_h,
			"a foreign replacement lifetime cannot inherit the wire target relation");
}

bool decoded_world_stream_materializes_exact_rows() {
	ns::ClientReplicaPipeline pipeline;

	nw::StaticEntityRecord static_row;
	static_row.item_type_id = 0x0600;
	static_row.pos_x = 11 * 65536;
	static_row.pos_y = -3 * 65536;
	static_row.pos_z = 2 * 65536;
	static_row.euler_z = w::bam_heading_from_mission_yaw_deg(37.0);
	static_row.euler_x = w::bam_from_degrees_wrapped(-8.0);
	static_row.euler_y = w::bam_from_degrees_wrapped(5.0);
	static_row.team_byte = 2;
	static_row.entity_flags = 0x04020400u;
	static_row.section_mask = 0x00000120;
	static_row.ammo_count = 0xA5;
	static_row.bone_a = 0x3C;
	static_row.bone_b = 0xFF;
	static_row.weapon_byte = 0x22;
	static_row.attach_ref = 70;
	nw::StaticEntityBatch statics;
	statics.start_index = 37;
	statics.records.push_back(static_row);
	pipeline.apply(0x10, nw::encode_static_entity_batch(statics));

	nw::PoolSpawnRecord vehicle;
	vehicle.slot_id = 0x1042;
	vehicle.item_type_id = 5008;
	vehicle.entity_name = "Wire Boat";
	vehicle.pos_x = 2 * 65536;
	vehicle.pos_y = 4 * 65536;
	vehicle.pos_z = 1 * 65536;
	vehicle.euler_z = w::bam_heading_from_mission_yaw_deg(-22.0);
	vehicle.euler_x = w::bam_from_degrees_wrapped(4.0);
	vehicle.euler_y = w::bam_from_degrees_wrapped(-6.0);
	vehicle.team_byte = 1;
	vehicle.entity_flags = 0x01004020u;
	vehicle.section_mask = 0x44;
	vehicle.bone_byte = 0x7A;
	vehicle.alert_byte = 0x17;
	vehicle.action_byte = 0x33;
	vehicle.zone_number_rank = 0x22;
	vehicle.zone_radius = 70;
	vehicle.seat_mask = 0x8Fu; // fixed slots 0..3 and 7 exist
	vehicle.mount_handles[0] = 0x0001u;
	vehicle.mount_handles[1] = 0xFFFFu;
	vehicle.mount_handles[2] = 0x5000u; // invalid pool nibble
	vehicle.mount_handles[3] = 0x1400u; // pool 1, one past retail capacity
	vehicle.mount_handles[7] = 0x0007u;
	// Retail copies tail slots 8/9 raw; unlike passenger slots 0..7, their
	// receive path does not structurally resolve these values.
	vehicle.mount_handle_8 = 0x5000u;
	vehicle.mount_handle_9 = 0x1400u;
	nw::PoolSpawnBatch vehicles;
	vehicles.records.push_back(vehicle);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(vehicles));

	nw::Pool3SyncRecord marker;
	marker.item_type_id = 6002;
	// The 0x20 handler selects slot start_index+i. Its net_handle field is a
	// separate value stored on that exact row (retail @0x425C00/+124).
	marker.net_handle = 0x0777;
	marker.pos_x = -7 * 65536;
	marker.pos_y = 8 * 65536;
	marker.pos_z = 0;
	marker.movement_val = static_cast<uint32_t>(
			w::bam_heading_from_mission_yaw_deg(91.0));
	marker.team_byte = 1;
	nw::Pool3SyncBatch markers;
	markers.start_index = 11;
	markers.records.push_back(marker);
	pipeline.apply(0x20, nw::encode_pool3_sync_batch(markers));

	w::World world;
	world.registry.configure_pool(0, w::kRetailActorPoolCapacity);
	world.registry.configure_pool(1, w::kRetailActorPoolCapacity);
	world.registry.configure_pool(2, w::kRetailActorPoolCapacity);
	world.registry.configure_pool(3, w::kRetailMarkerPoolCapacity);
	world.registry.configure_pool(4, w::kRetailActorPoolCapacity);
	ns::ClientWorldMaterializer materializer;
	const ns::ClientWorldSyncResult first =
			materializer.sync(pipeline.state(), world);
	if (!expect(first.spawned.size() == 3 && first.retired.empty(),
			"the first fold materializes every decoded mission-pool row"))
		return false;

	const w::Entity *building = world.registry.get(w::EntityHandle{0x2025});
	if (!expect(building != nullptr &&
			building->kind == w::EntityKind::Building &&
			building->item_id == 0x0600 &&
			building->net_id == 0 &&
			building->yaw == 37 && building->pitch == -8 &&
			building->roll == 5 &&
			building->engine_flags == 0x04020400u &&
			building->section_mask == 0x120u &&
			building->ammo_count == 0xA5 &&
			building->ref_num == 0x3C && building->sub_type == 0xFF &&
			building->zone_number == 2 && building->zone_radius == 70,
			"pool-2 materialization retains exact type/flags/section/zone metadata"))
		return false;

	w::Entity *boat = world.registry.get(w::EntityHandle{0x1042});
	if (!expect(boat != nullptr && boat->kind == w::EntityKind::Item &&
			boat->item_id == 5008 && boat->name == "Wire Boat" &&
			boat->net_id == 0 &&
			boat->team == 1 && boat->engine_flags == 0x01004020u &&
			boat->section_mask == 0x44u && boat->ammo_count == 0x7A &&
			boat->ref_num == 0x17 && boat->sub_type == 0x33 &&
			boat->zone_number == 2 && boat->zone_radius == 70 &&
			std::fabs(boat->position.x - 2.0f) < 0.0001f &&
			boat->yaw == 338 && boat->pitch == 4 && boat->roll == -6 &&
			boat->spawn_origin == w::kSpawnOriginNone,
			"pool-1 materialization retains exact handle, identity, pose, and traits"))
		return false;
	if (!expect(pipeline.state().find(0x2025)->net_id == 0 &&
				pipeline.state().find(0x1042)->net_id == 0,
			"pool-1/pool-2 retail memset identity is normalized before materialization"))
		return false;
	const ns::ClientEntityState *boat_state = pipeline.state().find(0x1042);
	if (!expect(boat_state->spawn_mount_mask == 0x8Fu &&
				boat_state->spawn_mount_handles[0] == 0x0001u &&
				boat_state->spawn_mount_handles[1] == 0xFFFFu &&
				boat_state->spawn_mount_handles[2] == 0x5000u &&
				boat_state->spawn_mount_handles[3] == 0x1400u &&
				boat_state->spawn_mount_handles[7] == 0x0007u &&
				boat_state->spawn_mount_handles[8] == 0x5000u &&
				boat_state->spawn_mount_handles[9] == 0x1400u &&
				boat->seats.empty(),
			"the decoded 0x0D row retains fixed mountHandles without inventing seat definitions"))
		return false;

	// Model/ItemDef resolution installs the actual seat definitions after the
	// wire row exists. A later materializer fold must map occupants by each
	// definition's retail_slot, never by dense vector index.
	for (const uint8_t retail_slot : {uint8_t{9}, uint8_t{3}, uint8_t{0},
			uint8_t{1}, uint8_t{2}, uint8_t{7}}) {
		w::Seat seat;
		seat.type = retail_slot == 9
				? w::SeatType::Gunner
				: w::SeatType::Passenger;
		seat.retail_slot = retail_slot;
		boat->seats.push_back(seat);
	}
	const ns::ClientWorldSyncResult seat_fold =
			materializer.sync(pipeline.state(), world);
	if (!expect(!seat_fold.changed() && boat->seats.size() == 6 &&
				boat->seats[0].retail_slot == 9 &&
				boat->seats[0].occupant == w::EntityHandle{0x1400u} &&
				boat->seats[1].retail_slot == 3 &&
				!boat->seats[1].occupant.valid() &&
				boat->seats[2].retail_slot == 0 &&
				boat->seats[2].occupant == w::EntityHandle{0x0001u} &&
				boat->seats[3].retail_slot == 1 &&
				!boat->seats[3].occupant.valid() &&
				boat->seats[4].retail_slot == 2 &&
				!boat->seats[4].occupant.valid() &&
				boat->seats[5].retail_slot == 7 &&
				boat->seats[5].occupant == w::EntityHandle{0x0007u},
			"materialization structurally resolves passenger handles while preserving raw tail slots"))
		return false;

	// The 0x0D mountHandles image seeds each model-defined slot once. Live
	// attach/detach updates own that slot afterward; an ordinary materializer
	// fold must not resurrect the stale spawn occupant.
	boat->seats[2].occupant = w::EntityHandle{};
	const ns::ClientWorldSyncResult detached_fold =
			materializer.sync(pipeline.state(), world);
	if (!expect(!detached_fold.changed() &&
				!boat->seats[2].occupant.valid(),
			"a later fold does not resurrect a detached spawn occupant"))
		return false;

	// A model table can arrive incrementally after the wire row. A newly
	// defined retail slot still consumes its retained initial occupant without
	// touching slots whose initial value was already projected.
	w::Seat late_driver;
	late_driver.type = w::SeatType::Driver;
	late_driver.retail_slot = 8;
	boat->seats.push_back(late_driver);
	const ns::ClientWorldSyncResult late_seat_fold =
			materializer.sync(pipeline.state(), world);
	if (!expect(!late_seat_fold.changed() &&
				!boat->seats[2].occupant.valid() &&
				boat->seats[6].retail_slot == 8 &&
				boat->seats[6].occupant == w::EntityHandle{0x5000u},
			"a late seat consumes only its own retained spawn occupant"))
		return false;

	const w::Entity *start = world.registry.get(w::EntityHandle{0x300B});
	if (!expect(start != nullptr && start->kind == w::EntityKind::Marker &&
			start->item_id == 6002 && start->team == 1 &&
			start->net_id == 0x0777,
			"pool-3 materialization separates start-index handle from net id"))
		return false;

	const uint64_t repeated_spawn_id = boat->registry_spawn_id;
	// Retail memsets the complete slot before every load record, even when the
	// incoming item type is unchanged. Seed state that must not survive that
	// new wire generation.
	boat->health = 1;
	boat->alive = false;
	boat->spawned_piece_mask = 0x12u;
	boat->death_motion = w::DeathMotionMode::Falling;
	w::Seat leaked_seat;
	leaked_seat.type = w::SeatType::Passenger;
	leaked_seat.retail_slot = 2;
	leaked_seat.occupant = w::EntityHandle{0x0003};
	boat->seats.push_back(leaked_seat);
	boat->veh.speed = 42.0f;
	vehicle.entity_name = "Repeated Boat";
	vehicle.entity_flags = 0x00001001u;
	vehicles.records.clear();
	vehicles.records.push_back(vehicle);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(vehicles));
	const ns::ClientWorldSyncResult repeated =
			materializer.sync(pipeline.state(), world);
	boat = world.registry.get(w::EntityHandle{0x1042});
	if (!expect(repeated.updated.empty() && repeated.spawned.size() == 1 &&
			repeated.retired.size() == 1 &&
			repeated.retired.front().handle == w::EntityHandle{0x1042} &&
			repeated.retired.front().registry_spawn_id == repeated_spawn_id &&
			repeated.spawned.front().handle == w::EntityHandle{0x1042} &&
			repeated.spawned.front().registry_spawn_id != repeated_spawn_id &&
			boat != nullptr && boat->name == "Repeated Boat" &&
			boat->engine_flags == 0x00001001u &&
			boat->registry_spawn_id != repeated_spawn_id && boat->health == 100 &&
			boat->alive && boat->spawned_piece_mask == 0 &&
			boat->death_motion == w::DeathMotionMode::None && boat->seats.empty() &&
			std::fabs(boat->veh.speed) < 0.0001f,
			"an identical-type load repeat creates a clean retail slot lifetime"))
		return false;

	const uint64_t old_spawn_id = boat->registry_spawn_id;
	ns::ClientEntityState *boat_row = pipeline.state().find(0x1042);
	boat_row->type_id = 5009;
	const ns::ClientWorldSyncResult changed =
			materializer.sync(pipeline.state(), world);
	boat = world.registry.get(w::EntityHandle{0x1042});
	if (!expect(changed.spawned.size() == 1 && changed.retired.size() == 1 &&
			changed.retired.front().handle == w::EntityHandle{0x1042} &&
			changed.retired.front().registry_spawn_id == old_spawn_id &&
			changed.spawned.front().handle == w::EntityHandle{0x1042} &&
			changed.spawned.front().registry_spawn_id != old_spawn_id &&
			boat != nullptr && boat->item_id == 5009 &&
			boat->registry_spawn_id != old_spawn_id,
			"a wire type change retires and recreates the same packed handle"))
		return false;

	auto &rows = pipeline.state().entities;
	rows.erase(std::remove_if(rows.begin(), rows.end(),
			[](const ns::ClientEntityState &row) {
				return row.handle == 0x1042;
			}), rows.end());
	const ns::ClientWorldSyncResult removed =
			materializer.sync(pipeline.state(), world);
	if (!expect(removed.retired.size() == 1 &&
			removed.retired.front().handle == w::EntityHandle{0x1042} &&
			removed.retired.front().registry_spawn_id ==
					changed.spawned.front().registry_spawn_id &&
			world.registry.get(w::EntityHandle{0x1042}) == nullptr,
			"row retirement removes the exact materialized world entity"))
		return false;
	return true;
}

} // namespace

int main() {
	if (!exact_registry_slot_contract()) return 1;
	if (!registry_lifetime_rejects_handle_reuse()) return 1;
	if (!malformed_load_indices_do_not_alias_low_slots()) return 1;
	if (!external_same_type_reuse_is_never_mutated_or_retired()) return 1;
	if (!preoccupied_exact_slot_requires_a_fresh_wire_generation()) return 1;
	if (!wire_target_authors_ground_separately_from_parent()) return 1;
	if (!decoded_world_stream_materializes_exact_rows()) return 1;
	std::puts("client_world_materializer_test: PASS");
	return 0;
}
