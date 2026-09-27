// A retail joiner owns only the 616-byte BMS header. Pools 1-3 therefore have
// to be materialized from the decoded initial-state stream, at the exact packed
// handles the host sent, before world-side deploy/mount/collision consumers run.

#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/replication/client_world_materializer.h>

#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;
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
	pool1.slot_id = 0x14B0; // pool 1, slot 1200: just beyond retail capacity
	pool1_batch.records.push_back(pool1);
	pool1.slot_id = 0x2001; // wrong pool nibble for a 0x0D record
	pool1_batch.records.push_back(pool1);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(pool1_batch));

	pool2.item_type_id = 0x0601;
	pool2_batch.start_index = 1200; // one past retail pool-2 capacity
	pool2_batch.records.clear();
	pool2_batch.records.push_back(pool2);
	pipeline.apply(0x10, nw::encode_static_entity_batch(pool2_batch));

	pool3.item_type_id = 6003;
	pool3_batch.start_index = 768; // one past retail pool-3 capacity
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
			world.registry.get(handle)->display_name == "Fresh wire generation" &&
			world.registry.get(handle)->registry_spawn_id != foreign_id,
			"a later wire generation can claim the now-empty exact slot");
}

bool wire_target_authors_ground_separately_from_parent() {
	ns::ClientReplicaPipeline pipeline;
	// The gun is an addeweap (no network callback) child.
	pipeline.set_item_class_resolver(
			[](uint16_t type) -> ns::ClientReplicaPipeline::ItemClassResolution {
				if (type == 5009) return nw::EntityClass::NoNetworkCallback;
				return std::nullopt;
			});
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
	// gun rides and the carrier the ewep class update follows. The materializer
	// must never let the parent author the structural carrier. [orig:
	// NapiNPClientMsg_0x00D @0x432C40 — parent → occupantEntity (+368) store
	// @0x433289; target → groundEntity resolve @0x4332bc, store @0x4332d7;
	// NetPacket_SerializeEntityPoolToPacket_0 +0x170 @0x503BC9, +0x28 @0x503C22]
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
	const w::Entity *live_hull = materializer.owned(world, hull_h);
	const w::Entity *live_gun = materializer.owned(world, gun_h);
	if (!expect(first.spawned.size() == 3 && live_hull != nullptr &&
			live_gun != nullptr && live_gun->emplacement_parent == hull_h &&
			live_gun->emplacement_parent != occupant_h &&
			live_gun->ground_target == hull_h &&
			live_gun->emplacement_parent_spawn_id ==
					live_hull->registry_spawn_id,
			"the 0x0D target authors retail groundEntity and the attachment carrier; "
			"the parent stays a back-ref"))
		return false;

	// Losing the materializer-owned TARGET lifetime must clear the structural
	// carrier; an unrelated replacement at the same packed handle cannot be
	// adopted by the next fold, and the occupant back-ref never stands in.
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
			!live_gun->emplacement_parent.valid(),
			"a foreign replacement lifetime cannot inherit the wire target relation");
}

// A retail host's 0x0D can name its own player (slot 0) as a vehicle's
// occupantEntity parent — the D-NET-195 dune buggy. The joiner's native body
// L sits at pool-0 slot 0 too; that wire identity must never resolve to it,
// or the vehicle becomes an attachment of the joiner's own body and the
// orphan sweep removes it when the joiner dies. Only materialized pool-1..3
// lifetimes resolve, and a vehicle takes no 0x0D carrier at all.
// [orig: NapiNPClientMsg_0x00D occupantEntity store @0x433289;
//  NetPacket_SerializeEntityPoolToPacket_0 +0x170 @0x503BC9]
bool pool0_parent_never_aliases_the_native_body() {
	ns::ClientReplicaPipeline pipeline;
	static constexpr uint16_t kVehicleType = 5011;
	pipeline.set_item_class_resolver(
			[](uint16_t type) -> ns::ClientReplicaPipeline::ItemClassResolution {
				if (type == kVehicleType) return nw::EntityClass::Vehicle;
				if (type == 5009) return nw::EntityClass::NoNetworkCallback;
				return std::nullopt;
			});
	nw::PoolSpawnRecord buggy;
	buggy.slot_id = 0x1006;
	buggy.item_type_id = kVehicleType;
	buggy.parent_handle = 0x0000; // Player #1 on the retail host
	nw::PoolSpawnRecord gun;
	gun.slot_id = 0x1007;
	gun.item_type_id = 5009;
	gun.parent_handle = 0x0000; // its gunner, the same host player
	nw::PoolSpawnBatch batch;
	batch.records = {buggy, gun};
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(batch));

	w::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);
	w::Entity body;
	body.kind = w::EntityKind::Organic;
	const w::EntityHandle native_l = world.registry.spawn(0, body);
	if (!expect(native_l.packed == 0x0000, "the native body occupies pool-0 slot 0"))
		return false;
	ns::ClientWorldMaterializer materializer;
	materializer.sync(pipeline.state(), world);
	const w::Entity *live_buggy = materializer.owned(world, w::EntityHandle{buggy.slot_id});
	const w::Entity *live_gun = materializer.owned(world, w::EntityHandle{gun.slot_id});
	return expect(live_buggy != nullptr && live_gun != nullptr &&
			!live_buggy->emplacement_parent.valid() &&
			!live_gun->emplacement_parent.valid(),
			"a pool-0 wire parent never resolves to the joiner's native body");
}

bool deployed_item_spawn_update_and_remove_materialize() {
	ns::ClientReplicaPipeline pipeline;
	constexpr uint16_t viewer_handle = 0x0001;
	constexpr uint16_t owner_handle = 0x0002;
	ns::ClientEntityState &viewer = pipeline.state().upsert(viewer_handle);
	viewer.team = 2;
	viewer.team_known = true;
	ns::ClientEntityState &owner = pipeline.state().upsert(owner_handle);
	owner.team = 2;
	owner.team_known = true;
	pipeline.set_viewer_handle(viewer_handle);
	pipeline.set_mp_attributes(0);

	nw::PoolSpawnRecord carrier;
	carrier.slot_id = 0x1002;
	carrier.item_type_id = 5008;
	nw::PoolSpawnBatch carriers;
	carriers.records.push_back(carrier);
	pipeline.apply(nw::s2c::POOL_SPAWN,
			nw::encode_pool_spawn_batch(carriers));

	nw::DeployedItemSpawn spawn;
	spawn.item_id = 0x0361;
	spawn.owner_handle = owner_handle;
	spawn.friendly_item_id = 0x0362;
	spawn.enemy_item_id = 0x0363;
	spawn.slot_handle = 0x1003;
	spawn.parent_handle = carrier.slot_id;
	spawn.pos_x = 10 * 65536;
	spawn.pos_y = -4 * 65536;
	spawn.pos_z = 3 * 65536;
	spawn.angle_x = 0x1000;
	spawn.angle_y = 0xE000;
	spawn.angle_z = 0x4000;
	pipeline.apply(nw::s2c::DEPLOYED_ITEM,
			nw::encode_deployed_item_spawn(spawn));

	const ns::ClientEntityState *row = pipeline.state().find(spawn.slot_handle);
	if (!expect(row != nullptr && row->type_id == spawn.friendly_item_id &&
			row->spawn_tag == nw::s2c::DEPLOYED_ITEM &&
			row->target_handle == carrier.slot_id &&
			row->parent_handle == 0xFFFFu && row->team_known && row->team == 2 &&
			row->x == spawn.pos_x && row->y == spawn.pos_y &&
			row->z == spawn.pos_z &&
			// angle words = entity+16/+20/+24 high halves = yaw/pitch/roll
			row->heading_bam == static_cast<int32_t>(
					static_cast<uint32_t>(spawn.angle_x) << 16) &&
			row->pitch_bam == static_cast<int32_t>(
					static_cast<uint32_t>(spawn.angle_y) << 16) &&
			row->roll_bam == static_cast<int32_t>(
					static_cast<uint32_t>(spawn.angle_z) << 16) &&
			row->spawn_revision == 1,
			"0x59 selects the friendly item and folds the placed-device pose/carrier"))
		return false;

	w::World world;
	world.registry.configure_pool(1, 16);
	ns::ClientWorldMaterializer materializer;
	const ns::ClientWorldSyncResult first =
			materializer.sync(pipeline.state(), world);
	w::Entity *placed = world.registry.get(w::EntityHandle{spawn.slot_handle});
	if (!expect(first.spawned.size() == 2 && placed != nullptr &&
			placed->item_id == spawn.friendly_item_id &&
			placed->position.x == 10.0f && placed->position.y == -4.0f &&
			placed->position.z == 3.0f &&
			placed->ground_target == w::EntityHandle{carrier.slot_id},
			"the folded 0x59 row materializes at its exact pool-1 handle"))
		return false;
	const uint64_t friendly_lifetime = placed->registry_spawn_id;
	const uint32_t friendly_revision = row->spawn_revision;

	spawn.pos_x = 12 * 65536;
	spawn.pos_y = -6 * 65536;
	spawn.angle_z = 0x6000;
	pipeline.apply(nw::s2c::DEPLOYED_ITEM,
			nw::encode_deployed_item_spawn(spawn));
	row = pipeline.state().find(spawn.slot_handle);
	const ns::ClientWorldSyncResult moved =
			materializer.sync(pipeline.state(), world);
	placed = world.registry.get(w::EntityHandle{spawn.slot_handle});
	if (!expect(row != nullptr && row->spawn_revision == friendly_revision &&
			moved.spawned.empty() && moved.retired.empty() &&
			moved.updated.size() == 1 && placed != nullptr &&
			placed->registry_spawn_id == friendly_lifetime &&
			placed->position.x == 12.0f && placed->position.y == -6.0f,
			"a same-item 0x59 update mutates pose without replacing the lifetime"))
		return false;

	// The team-variant pick runs only at FRESH SPAWN: retail's found/update
	// path never touches the item id, so a later team change (or a re-send
	// after one) leaves the materialized type and lifetime alone.
	// [orig: the found path @0x546828..0x54697a; pick @0x5469db fresh only]
	pipeline.apply_team_assign(owner_handle, 3);
	pipeline.apply(nw::s2c::DEPLOYED_ITEM,
			nw::encode_deployed_item_spawn(spawn));
	row = pipeline.state().find(spawn.slot_handle);
	const ns::ClientWorldSyncResult hostile =
			materializer.sync(pipeline.state(), world);
	placed = world.registry.get(w::EntityHandle{spawn.slot_handle});
	if (!expect(row != nullptr && row->type_id == spawn.friendly_item_id &&
			row->spawn_revision == friendly_revision &&
			hostile.retired.empty() && hostile.spawned.empty() &&
			placed != nullptr && placed->registry_spawn_id == friendly_lifetime,
			"a 0x59 update after a team change keeps the spawned type and lifetime"))
		return false;

	// A fresh spawn while the owner reads hostile picks the enemy variant.
	nw::DeployedItemSpawn enemy_spawn = spawn;
	enemy_spawn.slot_handle = 0x1004;
	pipeline.apply(nw::s2c::DEPLOYED_ITEM,
			nw::encode_deployed_item_spawn(enemy_spawn));
	row = pipeline.state().find(enemy_spawn.slot_handle);
	if (!expect(row != nullptr && row->type_id == spawn.enemy_item_id,
			"a fresh 0x59 spawn from a hostile owner picks the enemy variant"))
		return false;

	pipeline.apply_team_assign(owner_handle, 2);
	pipeline.set_mp_attributes(0x8000u);
	nw::DeployedItemSpawn forced_spawn = spawn;
	forced_spawn.slot_handle = 0x1005;
	pipeline.apply(nw::s2c::DEPLOYED_ITEM,
			nw::encode_deployed_item_spawn(forced_spawn));
	row = pipeline.state().find(forced_spawn.slot_handle);
	if (!expect(row != nullptr && row->type_id == spawn.enemy_item_id,
			"mp_attributes bit 0x8000 forces the enemy deployed-item variant"))
		return false;

	// The variant pair gate: retail uses friendly/enemy only when BOTH are
	// nonzero — a one-sided pair shows every client the base item id, even
	// on the side whose variant IS authored. [orig: @0x5469db..0x546a11]
	pipeline.set_mp_attributes(0);
	nw::DeployedItemSpawn one_sided = spawn;
	one_sided.slot_handle = 0x1006;
	one_sided.friendly_item_id = 0;
	pipeline.apply(nw::s2c::DEPLOYED_ITEM,
			nw::encode_deployed_item_spawn(one_sided));
	row = pipeline.state().find(one_sided.slot_handle);
	if (!expect(row != nullptr && row->type_id == spawn.item_id,
			"a one-sided variant pair falls back to the base item on a friendly viewer"))
		return false;
	pipeline.apply_team_assign(owner_handle, 3);
	nw::DeployedItemSpawn one_sided_foe = one_sided;
	one_sided_foe.slot_handle = 0x1007;
	pipeline.apply(nw::s2c::DEPLOYED_ITEM,
			nw::encode_deployed_item_spawn(one_sided_foe));
	row = pipeline.state().find(one_sided_foe.slot_handle);
	if (!expect(row != nullptr && row->type_id == spawn.item_id,
			"a one-sided variant pair falls back to the base item on a hostile viewer too"))
		return false;
	pipeline.apply_team_assign(owner_handle, 2);

	nw::DeployedItemSpawn orphan = spawn;
	orphan.slot_handle = 0x1008;
	orphan.owner_handle = 0x0BAD;
	pipeline.apply(nw::s2c::DEPLOYED_ITEM,
			nw::encode_deployed_item_spawn(orphan));
	row = pipeline.state().find(orphan.slot_handle);
	if (!expect(row != nullptr && row->type_id == spawn.item_id,
			"an unresolved owner falls back to the base item"))
		return false;

	nw::EntityRemove removal;
	removal.entity_handle = spawn.slot_handle;
	pipeline.apply(nw::s2c::ENTITY_REMOVE, nw::encode_entity_remove(removal));
	const ns::ClientWorldSyncResult removed =
			materializer.sync(pipeline.state(), world);
	return expect(pipeline.state().find(spawn.slot_handle) == nullptr &&
			removed.retired.size() == 1 &&
			world.registry.get(w::EntityHandle{spawn.slot_handle}) == nullptr &&
			world.registry.get(w::EntityHandle{carrier.slot_id}) != nullptr,
			"0x12 retires the placed device without removing its carrier");
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
	vehicle.has_zone_number_rank = true;
	vehicle.zone_number_rank = 0x22;
	vehicle.zone_radius = 70;
	vehicle.seat_mask = 0x8Fu; // fixed slots 0..3 and 7 exist
	vehicle.mount_handles[0] = 0x0001u;
	vehicle.mount_handles[1] = 0xFFFFu;
	vehicle.mount_handles[2] = 0x5000u; // invalid pool nibble
	vehicle.mount_handles[3] = 0x14B0u; // pool 1, one past retail capacity (1200)
	vehicle.mount_handles[7] = 0x0007u;
	// Retail copies tail slots 8/9 raw; unlike passenger slots 0..7, their
	// receive path does not structurally resolve these values.
	vehicle.mount_handle_8 = 0x5000u;
	vehicle.mount_handle_9 = 0x14B0u;
	nw::PoolSpawnBatch vehicles;
	vehicles.records.push_back(vehicle);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(vehicles));

	nw::Pool3SyncRecord marker;
	marker.item_type_id = 6006;
	// The 0x20 handler selects slot start_index+i. Its net_handle field is a
	// separate value stored on that exact row (retail @0x425C00/+124).
	marker.net_handle = 0x0777;
	marker.pos_x = -7 * 65536;
	marker.pos_y = 8 * 65536;
	marker.pos_z = 0;
	marker.movement_val = static_cast<uint32_t>(
			w::bam_heading_from_mission_yaw_deg(91.0));
	marker.orientation_val = 25u << 16;
	marker.team_byte = 1;
	nw::Pool3SyncBatch markers;
	markers.start_index = 11;
	markers.records.push_back(marker);
	pipeline.apply(0x20, nw::encode_pool3_sync_batch(markers));

	w::World world;
	for (int pool = 0; pool < w::kEntityPoolCount; ++pool)
		world.registry.configure_pool(pool, w::kRetailPoolCapacity[pool]);
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
			boat->item_id == 5008 && boat->display_name == "Wire Boat" &&
			boat->net_id == 0 &&
			boat->team == 1 &&
			// The streamed dword lands whole, each bit in the word that owns it:
			// 0x20 is a mover's runtime bit, so it rides `flags` alone.
			(boat->flags | boat->engine_flags) == 0x01004020u &&
			boat->flags == 0x01004020u && boat->engine_flags == 0x01004000u &&
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
				boat_state->spawn_mount_handles[3] == 0x14B0u &&
				boat_state->spawn_mount_handles[7] == 0x0007u &&
				boat_state->spawn_mount_handles[8] == 0x5000u &&
				boat_state->spawn_mount_handles[9] == 0x14B0u &&
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
				boat->seats[0].occupant == w::EntityHandle{0x14B0u} &&
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
			start->item_id == 6006 && start->team == 1 &&
			start->net_id == 0x0777 &&
			std::fabs(start->bound_radius - 25.0f) < 0.0001f,
			"pool-3 materialization preserves handle, net id, and entity+0 radius"))
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
			boat != nullptr && boat->display_name == "Repeated Boat" &&
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

// Regression (2026-08-05, PR #417 round): a retail 01TR host streams 1157
// pool-2 entities; the final 0x10 pages walk slots 1024..1156. The former
// 1024-slot clamp (an unwitnessed guess) silently dropped that tail — the
// entire chain-link fence line of the east base never materialized on a
// joiner while the host showed it. Retail's client indexes pool 2 unchecked;
// its own pool capacity 1200 is the reachable bound.
// [orig: NapiNPClientMsg_0x010 @0x433400 — Pool_GetEntryUnchecked(2, idx)
//  @0x433487; EntityPool_Allocate @0x442168 — pool-2 capacity 1200 @0x4421a2]
bool pool2_tail_beyond_1024_materializes() {
	ns::ClientReplicaPipeline pipeline;

	nw::StaticEntityBatch tail;
	tail.start_index = 1150;
	for (int i = 0; i < 7; ++i) {
		nw::StaticEntityRecord rec;
		rec.item_type_id = 0x10EB; // the 01TR fence tail's own type
		rec.pos_x = (753 + i) * 65536;
		rec.pos_y = 762 * 65536;
		rec.pos_z = 42 * 65536;
		tail.records.push_back(rec);
	}
	tail.entity_count = static_cast<uint16_t>(tail.records.size());
	pipeline.apply(0x10, nw::encode_static_entity_batch(tail));

	for (int slot = 1150; slot <= 1156; ++slot) {
		const uint16_t handle = static_cast<uint16_t>(0x2000u | slot);
		if (!expect(pipeline.state().find(handle) != nullptr,
				"a pool-2 load record at slot >= 1024 must fold (retail capacity 1200)"))
			return false;
	}

	w::World world;
	for (int pool = 0; pool < w::kEntityPoolCount; ++pool)
		world.registry.configure_pool(pool, w::kRetailPoolCapacity[pool]);
	ns::ClientWorldMaterializer materializer;
	const ns::ClientWorldSyncResult sync =
			materializer.sync(pipeline.state(), world);
	if (!expect(sync.spawned.size() == 7,
			"every pool-2 tail row materializes into the native world"))
		return false;
	const w::Entity *last = world.registry.get(w::EntityHandle{0x2000u | 1156u});
	return expect(last != nullptr && last->item_id == 0x10EB &&
			last->kind == w::EntityKind::Building,
			"the final streamed pool-2 slot (1156) exists at its exact handle");
}

// S2C 0x12 destroys ONE row and detaches dependents; a child attached to the
// removed handle (outside its EWeap refNum group, the next test) survives with
// its parent link cleared until its own remove.
// [orig: Entity_Destroy @0x43e810 — occupant/mount detach @0x43e9e9/
//  @0x43ea38..0x43ea59, memset of the one row @0x43ea70]
bool entity_remove_detaches_children_in_place() {
	ns::ClientReplicaPipeline pipeline;
	nw::PoolSpawnRecord parent;
	parent.slot_id = 0x1002;
	parent.item_type_id = 5008;
	nw::PoolSpawnRecord child;
	child.slot_id = 0x1003;
	child.item_type_id = 5009;
	child.parent_handle = parent.slot_id;
	nw::PoolSpawnBatch batch;
	batch.records = {parent, child};
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(batch));
	const ns::ClientEntityState *decoded_child =
			pipeline.state().find(child.slot_id);
	if (!expect(decoded_child != nullptr &&
			decoded_child->parent_handle == parent.slot_id,
			"the 0x0D child row carries its parent handle"))
		return false;

	nw::EntityRemove removal;
	removal.entity_handle = parent.slot_id;
	pipeline.apply(nw::s2c::ENTITY_REMOVE, nw::encode_entity_remove(removal));
	decoded_child = pipeline.state().find(child.slot_id);
	return expect(pipeline.state().find(parent.slot_id) == nullptr &&
			decoded_child != nullptr &&
			decoded_child->parent_handle == 0xFFFFu &&
			!decoded_child->parent_pose_valid,
			"0x12 removes only the named row; the child survives detached");
}

// The client's 0x12 runs the shared Entity_Destroy, refNum walk included: a
// non-person row with a def and a refNum takes every member of its refNum
// group whose def carries EWeap, and the members need no 0x12 of their own. A
// member without EWeap and a gun of another refNum stay, a person's destroy
// walks nothing, and without the embedder's items.def catalog no row has a def
// and nothing walks.
// [orig: NapiNPClientMsg_0x012 @0x425F8F -> Entity_Destroy @0x43E810 (the
//  gates @0x43E9B6..0x43E9CA, the call @0x43E9CD) -> EntityReference_DestroyEWeapGroup
//  @0x546F30 (member tests @0x546F8A..0x546FA0, Entity_Destroy @0x546FA3)]
bool entity_remove_takes_the_eweap_refnum_group() {
	const auto definition = [](uint16_t wire_type, int32_t item_type, uint32_t attrib) {
		ns::ItemReplicationDefinition d;
		d.definition_id = ns::ItemReplicationCatalog::kDefinitionIdOffset + wire_type;
		d.item_type = item_type;
		d.attrib = attrib;
		return d;
	};
	constexpr uint32_t kEweap = 0x20u;         // ItemDef+0x54 EWeap
	constexpr uint32_t kPlayerControl = 0x40u; // ItemDef+0x54 PlayerControl
	const auto catalog = std::make_shared<const ns::ItemReplicationCatalog>(
			ns::ItemReplicationCatalog::from_definitions({
					definition(5010, 1, kPlayerControl | kEweap), // the carrier
					definition(5011, 6, kEweap),                  // its guns
					definition(5012, 6, 0),                       // a peer without EWeap
					definition(5013, 3, 0)}));                    // a person
	const auto record = [](uint16_t slot, uint16_t type, uint8_t ref) {
		nw::PoolSpawnRecord r;
		r.slot_id = slot;
		r.item_type_id = type;
		r.alert_byte = ref; // entity+533, the refNum
		return r;
	};
	nw::PoolSpawnBatch batch;
	batch.records = {record(0x1002, 5010, 9), record(0x1003, 5011, 9),
			record(0x1004, 5011, 9), record(0x1005, 5012, 9), record(0x1006, 5011, 10),
			record(0x1007, 5013, 11), record(0x1008, 5011, 11)};
	nw::EntityRemove removal;

	ns::ClientReplicaPipeline bare;
	bare.apply(0x0D, nw::encode_pool_spawn_batch(batch));
	removal.entity_handle = 0x1002;
	bare.apply(nw::s2c::ENTITY_REMOVE, nw::encode_entity_remove(removal));
	if (!expect(bare.state().find(0x1002) == nullptr && bare.state().find(0x1003) != nullptr &&
			bare.state().find(0x1004) != nullptr,
			"without a catalog no row has a def, so the 0x12 walks no group"))
		return false;

	ns::ClientReplicaPipeline pipeline;
	pipeline.set_item_catalog(catalog);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(batch));
	const ns::ClientState &s = pipeline.state();
	if (!expect(s.find(0x1003) != nullptr && s.find(0x1003)->spawn_ref_num == 9,
			"the 0x0D rows carry their refNum"))
		return false;
	pipeline.apply(nw::s2c::ENTITY_REMOVE, nw::encode_entity_remove(removal));
	if (!expect(s.find(0x1002) == nullptr && s.find(0x1003) == nullptr &&
			s.find(0x1004) == nullptr,
			"the carrier's 0x12 takes its EWeap refNum members"))
		return false;
	if (!expect(s.find(0x1005) != nullptr && s.find(0x1005)->spawn_ref_num == 9 &&
			s.find(0x1006) != nullptr,
			"a member without EWeap and another refNum's gun stay"))
		return false;
	removal.entity_handle = 0x1007;
	pipeline.apply(nw::s2c::ENTITY_REMOVE, nw::encode_entity_remove(removal));
	return expect(s.find(0x1007) == nullptr && s.find(0x1008) != nullptr,
			"a person's destroy walks no refNum group");
}

// A joiner's twin joins its refNum's group list the way the client's 0x0D
// handler joins the row: once its def resolves, a row whose def is not a
// person's and whose refNum is nonzero. A person, a row without a refNum and a
// row whose def never resolved stay off the list, the join is taken once per
// lifetime, and a carrier's death then releases its gun on the joiner's twins.
// [orig: NapiNPClientMsg_0x00D @0x433381..0x4333AD (NapiNPClientMsg_0x010
//  @0x433684..0x4336B0 and NapiNPClientMsg_FullEntitySpawn @0x433E27..0x433E52
//  join the same way); Vehicle_ReleaseEWeapGroupOnDestruction @0x547040 (the
//  member tests @0x5470B9..0x5470D3, the gun words @0x5470F9..0x547100)]
bool materialized_rows_join_their_refnum_group() {
	const auto record = [](uint16_t slot, uint16_t type, uint8_t ref) {
		nw::PoolSpawnRecord r;
		r.slot_id = slot;
		r.item_type_id = type;
		r.alert_byte = ref; // entity+533, the refNum
		return r;
	};
	nw::PoolSpawnBatch batch;
	batch.records = {record(0x1002, 5010, 9), record(0x1003, 5011, 9),
			record(0x1004, 5013, 9), record(0x1005, 5011, 0), record(0x1006, 5014, 9)};
	ns::ClientReplicaPipeline pipeline;
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(batch));
	auto world_heap = std::make_unique<w::World>();
	w::World &world = *world_heap;
	world.registry.configure_pool(1, 16);
	ns::ClientWorldMaterializer materializer;
	materializer.sync(pipeline.state(), world);
	const auto twin = [&](uint16_t packed) {
		return world.registry.get(w::EntityHandle{packed});
	};
	if (!expect(twin(0x1002) != nullptr && twin(0x1003) != nullptr &&
			twin(0x1006) != nullptr && twin(0x1003)->ref_num == 9 &&
			!twin(0x1002)->ref_group_member && !twin(0x1003)->ref_group_member,
			"a twin whose def has not resolved is on no list"))
		return false;
	// The joiner's item-traits resweep resolves the defs, then the fold runs
	// again (JoinerRole::on_replica_world_changed).
	const auto stamp_def = [&](uint16_t packed, uint8_t item_type, uint32_t attrib) {
		w::Entity *e = twin(packed);
		e->has_item_def = true;
		e->item_type = item_type;
		e->item_attrib = attrib;
	};
	stamp_def(0x1002, 1, w::kItemAttribPlayerControl); // the carrier
	stamp_def(0x1003, 6, w::kItemAttribEweap);         // its gun
	stamp_def(0x1004, 3, 0);                           // a person
	stamp_def(0x1005, 6, w::kItemAttribEweap);         // a gun without a refNum
	materializer.sync(pipeline.state(), world);         // 0x1006 has no def row
	if (!expect(twin(0x1002)->ref_group_member && twin(0x1003)->ref_group_member &&
			!twin(0x1004)->ref_group_member && !twin(0x1005)->ref_group_member &&
			!twin(0x1006)->ref_group_member,
			"a resolved non-person twin with a refNum joins; a person, a zero refNum and an unresolved def do not"))
		return false;
	twin(0x1004)->item_type = 6;
	materializer.sync(pipeline.state(), world);
	if (!expect(!twin(0x1004)->ref_group_member,
			"the join is taken once per lifetime: a later fold does not take it again"))
		return false;
	twin(0x1003)->emplaced_gun_yaw_word = 0x1234;
	twin(0x1003)->emplaced_gun_pitch_word = -0x234;
	w::entity_update_death_transforms(world, *twin(0x1002), /*silent=*/true);
	return expect(twin(0x1003) != nullptr && twin(0x1003)->emplaced_gun_yaw_word == 0 &&
			twin(0x1003)->emplaced_gun_pitch_word == 0,
			"the carrier's death releases its gun in place on the joiner's twin");
}

// S2C 0x2F updates the flag itself, its occupantEntity pointer, the carrier's
// mountedChild back-link, and groundEntity. A later detached record clears both
// sides without respawning the flag. [orig: NapiNPClientMsg_0x02F @0x430E10;
// Entity_AttachCarriedObject @0x43C130]
bool objective_state_attaches_and_detaches_flag() {
	ns::ClientReplicaPipeline pipeline;
	nw::PoolSpawnRecord flag;
	flag.slot_id = 0x1007;
	flag.item_type_id = 4093;
	nw::PoolSpawnBatch batch;
	batch.records.push_back(flag);
	pipeline.apply(nw::s2c::POOL_SPAWN, nw::encode_pool_spawn_batch(batch));

	w::World world;
	for (int pool = 0; pool < w::kEntityPoolCount; ++pool)
		world.registry.configure_pool(pool, w::kRetailPoolCapacity[pool]);
	// The client's own player: wire handle 0x0002, its native row at slot 4.
	// A remote carrier's wire handle 0x0004 names that native slot too, and
	// must never resolve to it: a pool-0 carrier resolves to the native row
	// only through the client's own wire handle.
	w::Entity carrier_seed;
	carrier_seed.kind = w::EntityKind::Organic;
	carrier_seed.item_id = 1;
	const w::EntityHandle carrier{0x0004};
	if (!expect(world.registry.spawn_at(carrier, carrier_seed) == carrier,
			"the client's own player sits at native slot 4"))
		return false;

	ns::ClientWorldMaterializer materializer;
	materializer.set_local_player(0x0002, carrier);
	materializer.sync(pipeline.state(), world);

	nw::ObjectiveEntityState carried;
	carried.entity_handle = flag.slot_id;
	carried.flags_byte = 0x01;
	carried.pos_x = 12 * 65536;
	carried.pos_y = -3 * 65536;
	carried.pos_z = 5 * 65536;
	carried.attach_handle = 0x0004;
	carried.ground_handle = 0xFFFF;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE,
			nw::encode_objective_entity_state(carried));
	materializer.sync(pipeline.state(), world);
	w::Entity *flag_entity = world.registry.get(w::EntityHandle{flag.slot_id});
	w::Entity *carrier_entity = world.registry.get(carrier);
	if (!expect(flag_entity != nullptr && carrier_entity != nullptr &&
			!flag_entity->primary_occupant.valid() &&
			!carrier_entity->mounted_child.valid() &&
			(flag_entity->flags & 0x01u) != 0,
			"a remote carrier's wire handle never names the native row at its slot"))
		return false;

	carried.attach_handle = 0x0002;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE,
			nw::encode_objective_entity_state(carried));
	materializer.sync(pipeline.state(), world);
	if (!expect(flag_entity->primary_occupant == carrier &&
			carrier_entity->mounted_child == w::EntityHandle{flag.slot_id} &&
			(flag_entity->flags & 0xFFu) == 0x01u &&
			std::fabs(flag_entity->position.x - 12.0f) < 0.001f,
			"0x2F attaches the flag to the client's own player and applies its low flags and fixed position"))
		return false;

	carried.flags_byte = 0;
	carried.attach_handle = 0xFFFF;
	carried.ground_handle = 0x1002;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE,
			nw::encode_objective_entity_state(carried));
	materializer.sync(pipeline.state(), world);
	return expect(flag_entity->primary_occupant == w::EntityHandle{} &&
			carrier_entity->mounted_child == w::EntityHandle{} &&
			flag_entity->ground_target == w::EntityHandle{},
			"a detached 0x2F clears both carry links and ignores an unresolved ground row");
}

// A client dropping the flag off its own player reads that player's own
// native pose (position, body heading and pitch), not the host's echo row of
// its wire handle.
// [orig: NapiNPClientMsg_0x02F @0x4310DC -> Entity_DropCarriedObject over the
//  client's own entity]
bool objective_drop_off_the_local_player_reads_its_own_pose() {
	ns::ClientReplicaPipeline pipeline;
	nw::PoolSpawnRecord flag;
	flag.slot_id = 0x1007;
	flag.item_type_id = 4091;
	nw::PoolSpawnBatch batch;
	batch.records.push_back(flag);
	pipeline.apply(nw::s2c::POOL_SPAWN, nw::encode_pool_spawn_batch(batch));
	// The host's echo of the client's own wire handle, somewhere else.
	ns::ClientEntityState &echo = pipeline.state().upsert(0x0002);
	echo.type_id = 1;
	echo.cls = nw::EntityClass::Player;
	echo.x = 20 * 65536;
	echo.y = 30 * 65536;
	echo.z = 4 * 65536;
	echo.heading_bam = 0x10000000;

	w::World world;
	for (int pool = 0; pool < w::kEntityPoolCount; ++pool)
		world.registry.configure_pool(pool, w::kRetailPoolCapacity[pool]);
	w::Entity local_seed;
	local_seed.kind = w::EntityKind::Organic;
	local_seed.item_id = 1;
	local_seed.position = {100.0f, 200.0f, 10.0f};
	const w::EntityHandle local{0x0005};
	world.registry.spawn_at(local, local_seed);
	w::AiEntity *body = world.ai.at(world.ai.attach(local));
	body->heading = 0x20000000;
	body->pitch = 0;
	ns::ClientWorldMaterializer materializer;
	materializer.set_local_player(0x0002, local);
	materializer.sync(pipeline.state(), world);

	nw::ObjectiveEntityState state;
	state.entity_handle = flag.slot_id;
	state.flags_byte = 0x01;
	state.pos_x = 20 * 65536;
	state.pos_y = 30 * 65536;
	state.pos_z = 4 * 65536;
	state.attach_handle = 0x0002;
	state.ground_handle = 0xFFFF;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE, nw::encode_objective_entity_state(state));
	materializer.sync(pipeline.state(), world);
	state.flags_byte = 0x00;
	state.attach_handle = 0xFFFF;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE, nw::encode_objective_entity_state(state));
	materializer.sync(pipeline.state(), world);
	const w::Entity *flag_entity = world.registry.get(w::EntityHandle{flag.slot_id});
	return expect(flag_entity != nullptr && flag_entity->position.x == 100.0f &&
			flag_entity->position.y == 200.0f &&
			w::to_fixed(flag_entity->position.z) == 10 * 65536 + 0x4000 &&
			flag_entity->veh.yaw_bam == 0x60000000 &&
			flag_entity->drop_motion == w::DropMotion::Fall &&
			!world.registry.get(local)->mounted_child.valid(),
			"the drop off the client's own player reads its native pose and body heading");
}

// Destroying a person carrier's row drops the flag it carries, off its last
// pose, whether S2C 0x12 or the 0x5D sweep destroys it; a carrier of another
// class only detaches.
// [orig: Entity_Destroy @0x43E8AA..0x43E8B8 (def type 3 -> Entity_DropCarriedObject),
//  reached from NapiNPClientMsg_0x012 @0x425EE0 and NapiNPClientMsg_DestroyEntityList
//  @0x429730]
bool carrier_destroy_drops_the_flag() {
	for (int path = 0; path < 3; ++path) {
		ns::ClientReplicaPipeline pipeline;
		nw::PoolSpawnRecord flag;
		flag.slot_id = 0x1007;
		flag.item_type_id = 4093;
		nw::PoolSpawnBatch batch;
		batch.records.push_back(flag);
		pipeline.apply(nw::s2c::POOL_SPAWN, nw::encode_pool_spawn_batch(batch));
		ns::ClientEntityState &carrier = pipeline.state().upsert(0x0006);
		carrier.type_id = 1;
		carrier.cls = path == 2 ? nw::EntityClass::Vehicle : nw::EntityClass::Infantry;
		carrier.x = 20 * 65536;
		carrier.y = 30 * 65536;
		carrier.z = 4 * 65536;
		carrier.heading_bam = 0x10000000;
		carrier.pitch_bam = 0x10000000;

		w::World world;
		for (int pool = 0; pool < w::kEntityPoolCount; ++pool)
			world.registry.configure_pool(pool, w::kRetailPoolCapacity[pool]);
		ns::ClientWorldMaterializer materializer;
		materializer.sync(pipeline.state(), world);
		nw::ObjectiveEntityState state;
		state.entity_handle = flag.slot_id;
		state.flags_byte = 0x01;
		state.pos_x = 20 * 65536;
		state.pos_y = 30 * 65536;
		state.pos_z = 4 * 65536;
		state.attach_handle = 0x0006;
		state.ground_handle = 0xFFFF;
		pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE, nw::encode_objective_entity_state(state));
		materializer.sync(pipeline.state(), world);

		if (path == 1) {
			nw::DestroyEntityList sweep;
			sweep.pool0_indices.push_back(0x0006);
			pipeline.apply(nw::s2c::EMPTY_SLOT_SWEEP, nw::encode_destroy_entity_list(sweep));
		} else {
			nw::EntityRemove removal;
			removal.entity_handle = 0x0006;
			pipeline.apply(nw::s2c::ENTITY_REMOVE, nw::encode_entity_remove(removal));
		}
		materializer.sync(pipeline.state(), world);
		const w::Entity *flag_entity = world.registry.get(w::EntityHandle{flag.slot_id});
		if (!expect(flag_entity != nullptr && pipeline.state().find(0x0006) == nullptr,
				"the carrier's row is destroyed"))
			return false;
		if (path == 2) {
			if (!expect(flag_entity->drop_motion == w::DropMotion::None &&
					flag_entity->position.z == 4.0f,
					"a destroyed carrier of another class only detaches the flag"))
				return false;
			continue;
		}
		if (!expect((flag_entity->flags & 0x01u) == 0 && flag_entity->position.x == 20.0f &&
				flag_entity->position.y == 30.0f &&
				w::to_fixed(flag_entity->position.z) == 4 * 65536 + 0x4000 &&
				flag_entity->veh.yaw_bam == 0x50000000 && flag_entity->veh.slide_z == 391 &&
				flag_entity->drop_motion == w::DropMotion::Fall,
				path == 0 ? "0x12 destroying the person carrier drops the flag off its last pose"
				          : "the 0x5D sweep destroying the person carrier drops the flag off its last pose"))
			return false;
	}
	return true;
}

// A client runs the drop itself: the 0x2F that takes the flag off its
// occupant drops it off that occupant's words (here a remote person, known
// only as its decoded row) with the drop's own legs, Z + 0x4000, a quarter
// turn and the lift from its pitch, and installs the fall. The fall owns the
// flag's pose until the next 0x2F state lands its own.
// [orig: NapiNPClientMsg_0x02F @0x4310DC -> Entity_DropCarriedObject
//  @0x439DF0; the pose stores @0x430F68..0x430F74]
bool objective_state_drop_runs_on_the_client() {
	ns::ClientReplicaPipeline pipeline;
	nw::PoolSpawnRecord flag;
	flag.slot_id = 0x1007;
	flag.item_type_id = 4091;
	nw::PoolSpawnBatch batch;
	batch.records.push_back(flag);
	pipeline.apply(nw::s2c::POOL_SPAWN, nw::encode_pool_spawn_batch(batch));
	ns::ClientEntityState &carrier_row = pipeline.state().upsert(0x0004);
	carrier_row.type_id = 1;
	carrier_row.x = 20 * 65536;
	carrier_row.y = 30 * 65536;
	carrier_row.z = 4 * 65536;
	carrier_row.heading_bam = 0x10000000;
	carrier_row.pitch_bam = 0x10000000;

	w::World world;
	for (int pool = 0; pool < w::kEntityPoolCount; ++pool)
		world.registry.configure_pool(pool, w::kRetailPoolCapacity[pool]);
	ns::ClientWorldMaterializer materializer;
	materializer.sync(pipeline.state(), world);

	nw::ObjectiveEntityState state;
	state.entity_handle = flag.slot_id;
	state.flags_byte = 0x01;
	state.pos_x = 20 * 65536;
	state.pos_y = 30 * 65536;
	state.pos_z = 4 * 65536;
	state.attach_handle = 0x0004;
	state.ground_handle = 0xFFFF;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE, nw::encode_objective_entity_state(state));
	materializer.sync(pipeline.state(), world);
	w::Entity *flag_entity = world.registry.get(w::EntityHandle{flag.slot_id});
	if (!expect(flag_entity != nullptr && (flag_entity->flags & 0x01u) != 0 &&
			flag_entity->drop_motion == w::DropMotion::None,
			"a carried 0x2F state hides the flag on its remote carrier"))
		return false;

	state.flags_byte = 0;
	state.pos_z = 4 * 65536 + 0x4000;
	state.attach_handle = 0xFFFF;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE, nw::encode_objective_entity_state(state));
	materializer.sync(pipeline.state(), world);
	if (!expect((flag_entity->flags & 0x01u) == 0 &&
			flag_entity->position.x == 20.0f && flag_entity->position.y == 30.0f &&
			w::to_fixed(flag_entity->position.z) == 4 * 65536 + 0x4000 &&
			flag_entity->veh.yaw_bam == 0x50000000 && flag_entity->veh.slide_z == 391 &&
			flag_entity->drop_motion == w::DropMotion::Fall,
			"the detached 0x2F drops the flag off its carrier's words and installs the fall"))
		return false;

	// The fall moves the flag; a sync without a new state leaves it there.
	flag_entity->position.z = 1.0f;
	pipeline.state().mark_topology_changed();
	materializer.sync(pipeline.state(), world);
	if (!expect(flag_entity->position.z == 1.0f,
			"between 0x2F states the client's own fall owns the flag's pose"))
		return false;

	// The next state (the host's round-robin refresh) lands its pose; the
	// installed callback stays.
	state.pos_z = 2 * 65536;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE, nw::encode_objective_entity_state(state));
	materializer.sync(pipeline.state(), world);
	if (!expect(flag_entity->position.z == 2.0f &&
			flag_entity->drop_motion == w::DropMotion::Fall,
			"a new 0x2F state lands its pose and leaves the installed fall"))
		return false;

	// A pickup, then a carrier swap in one state: the flag drops off the first
	// carrier and the second attaches it, hidden again with its fall gone.
	// [orig: @0x43105C then Entity_AttachCarriedObject @0x43C14A / @0x43C191]
	ns::ClientEntityState &second = pipeline.state().upsert(0x0005);
	second.type_id = 1;
	second.x = 50 * 65536;
	state.flags_byte = 0x01;
	state.attach_handle = 0x0004;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE, nw::encode_objective_entity_state(state));
	materializer.sync(pipeline.state(), world);
	if (!expect(flag_entity->drop_motion == w::DropMotion::None,
			"a pickup state puts back the def's callback"))
		return false;
	state.flags_byte = 0x00;
	state.attach_handle = 0x0005;
	pipeline.apply(nw::s2c::OBJECTIVE_ENTITY_STATE, nw::encode_objective_entity_state(state));
	materializer.sync(pipeline.state(), world);
	return expect((flag_entity->flags & 0x01u) != 0 &&
			flag_entity->drop_motion == w::DropMotion::None &&
			flag_entity->position.x == 20.0f &&
			w::to_fixed(flag_entity->position.z) == 4 * 65536 + 0x4000,
			"a carrier swap drops the flag off the first carrier, then the attach hides it");
}

// The world-stream fence hands every materialized static a placed identity so
// the shell presents it through the same batched placer path as the host's
// own statics (retail draws the client-built pools through the one sector
// renderer [orig: Terrain_CollectVisibleEntitiesForTerrain @0x5c8c60]). Kind
// follows the row's item def as the host's BMS list does (a building def is a
// Building, an object or a vehicle an Item), never the streamed Flags bit
// 0x20000, which a live retail vehicle carries from its mover [orig:
// Entity_UpdateVehiclePhysics @0x48D451]; pool 3 is markers, the index is a
// per-kind ordinal in the witnessed pool order, and the BMS-attribute bits map
// back off the Flags dword exactly [orig: Entity_SpawnFromBMSRecord @0x40ed14].
bool streamed_rows_take_a_placed_identity_at_the_fence() {
	ns::ClientReplicaPipeline pipeline;

	nw::StaticEntityRecord static_row;
	static_row.item_type_id = 0x0600;
	static_row.pos_x = 11 * 65536;
	static_row.pos_y = -3 * 65536;
	static_row.pos_z = 2 * 65536;
	static_row.euler_z = w::bam_heading_from_mission_yaw_deg(37.0);
	static_row.team_byte = 2;
	static_row.entity_flags = 0x04020400u; // Indestructible | Building | Reflective
	nw::StaticEntityBatch statics;
	statics.start_index = 37;
	statics.records.push_back(static_row);
	nw::StaticEntityRecord crate = static_row;
	crate.item_type_id = 0x0601;
	crate.entity_flags = 0x01020000u; // NoShadow + the matrix bit, but an object def: an Item
	statics.records.push_back(crate);
	pipeline.apply(0x10, nw::encode_static_entity_batch(statics));

	// A live retail vehicle: REFLECTABLE plus its mover's per-tick 0x20000, as
	// the retail load stream carries it (fixtures/novaworld/run_20260426_120859).
	nw::PoolSpawnRecord vehicle;
	vehicle.slot_id = 0x1042;
	vehicle.item_type_id = 5008;
	vehicle.pos_x = 2 * 65536;
	vehicle.team_byte = 1;
	vehicle.entity_flags = 0x00020400u;
	nw::PoolSpawnBatch vehicles;
	vehicles.records.push_back(vehicle);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(vehicles));

	nw::Pool3SyncRecord marker;
	marker.item_type_id = 6006;
	marker.net_handle = 0x0777;
	nw::Pool3SyncBatch markers;
	markers.start_index = 11;
	markers.records.push_back(marker);
	pipeline.apply(0x20, nw::encode_pool3_sync_batch(markers));

	w::World world;
	for (int pool = 0; pool < w::kEntityPoolCount; ++pool)
		world.registry.configure_pool(pool, w::kRetailPoolCapacity[pool]);
	ns::ClientWorldMaterializer materializer;
	materializer.sync(pipeline.state(), world);
	if (!expect(materializer.placed_rows(world).empty() &&
			world.registry.get(w::EntityHandle{0x2025})->spawn_origin ==
					w::kSpawnOriginNone &&
			world.registry.get(w::EntityHandle{0x2025})->bms_id == 0,
			"materialization alone stamps no placed identity"))
		return false;
	// The joiner's item-traits resweep resolves every materialized row's def
	// before the fence (JoinerRole::on_replica_world_changed).
	const auto stamp_def = [&](uint16_t packed, uint8_t item_type) {
		w::Entity *e = world.registry.get(w::EntityHandle{packed});
		e->has_item_def = true;
		e->item_type = item_type;
	};
	stamp_def(0x2025, 5); // a building def
	stamp_def(0x2026, 6); // an object def
	stamp_def(0x1042, 1); // a vehicle def

	if (!expect(materializer.assign_placement_origins(world) == 4,
			"the fence stamps every materialized pool-1..3 row once"))
		return false;
	const w::Entity *building = world.registry.get(w::EntityHandle{0x2025});
	const w::Entity *item = world.registry.get(w::EntityHandle{0x2026});
	const w::Entity *boat = world.registry.get(w::EntityHandle{0x1042});
	const w::Entity *mark = world.registry.get(w::EntityHandle{0x300B});
	if (!expect(building->spawn_origin == w::spawn_origin_pack(2, 0) &&
			building->bms_id == 0x2026 &&
			item->spawn_origin == w::spawn_origin_pack(1, 0) &&
			item->bms_id == 0x2027 &&
			boat->spawn_origin == w::spawn_origin_pack(1, 1) &&
			boat->bms_id == 0x1043 &&
			mark->spawn_origin == w::spawn_origin_pack(0, 0) &&
			mark->bms_id == 0x300C,
			"kind follows the item def (a live vehicle's 0x20000 keeps it an Item), indices are per-kind ordinals in pool 2/1/3 slot order, bms ids are the packed handle + 1"))
		return false;
	if (!expect(materializer.assign_placement_origins(world) == 0,
			"a second fence pass stamps nothing"))
		return false;

	// The placed rows ARE the registry rows (no record twin): (kind, index)-
	// ordered, carrying the identity and the record-space attributes.
	const std::vector<const w::Entity *> rows = materializer.placed_rows(world);
	const auto kind_of = [](const w::Entity *e) { return w::spawn_origin_kind(e->spawn_origin); };
	const auto index_of = [](const w::Entity *e) { return w::spawn_origin_index(e->spawn_origin); };
	const auto attribs_of = [](const w::Entity *e) {
		return w::bms_attributes_from_entity_flags(e->engine_flags);
	};
	if (!expect(rows.size() == 4 && kind_of(rows[0]) == 0 &&
			kind_of(rows[1]) == 1 && index_of(rows[1]) == 0 &&
			rows[1]->item_id == 0x0601 && rows[1]->bms_id == 0x2027 &&
			attribs_of(rows[1]) == 0x01000000u &&
			kind_of(rows[2]) == 1 && index_of(rows[2]) == 1 &&
			rows[2]->item_id == 5008 && rows[2]->team == 1 &&
			attribs_of(rows[2]) == 0x00800000u &&
			kind_of(rows[3]) == 2 && rows[3]->bms_id == 0x2026 &&
			rows[3]->item_id == 0x0600 && rows[3]->team == 2 &&
			rows[3]->yaw == 37 &&
			std::fabs(rows[3]->position.x - 11.0f) < 0.0001f &&
			attribs_of(rows[3]) == 0x00A00000u,
			"placed rows are (kind, index)-ordered and carry the record-space attributes"))
		return false;

	// A row streamed after the fence stays wire-direct until the next stamp,
	// which appends behind the existing ordinals.
	nw::PoolSpawnRecord late = vehicle;
	late.slot_id = 0x1043;
	nw::PoolSpawnBatch late_batch;
	late_batch.records.push_back(late);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(late_batch));
	materializer.sync(pipeline.state(), world);
	const w::Entity *late_row = world.registry.get(w::EntityHandle{0x1043});
	if (!expect(late_row != nullptr && late_row->spawn_origin == w::kSpawnOriginNone,
			"a later spawn carries no placed identity"))
		return false;
	if (!expect(materializer.assign_placement_origins(world) == 1 &&
			late_row->spawn_origin == w::spawn_origin_pack(1, 2),
			"a later stamp appends behind the existing per-kind ordinal"))
		return false;

	// A same-type re-spawn of a stamped slot (a new wire generation at the
	// same handle) keeps its placed identity on the fresh lifetime; the shell
	// hears nothing. A re-typed slot retires the identity for the shell to
	// hide, and its new occupant is wire-direct.
	nw::PoolSpawnRecord respawn = vehicle; // slot 0x1042, same type
	respawn.pos_x = 9 * 65536;
	nw::PoolSpawnBatch respawn_batch;
	respawn_batch.records.push_back(respawn);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(respawn_batch));
	const ns::ClientWorldSyncResult respawned = materializer.sync(pipeline.state(), world);
	const w::Entity *boat_again = world.registry.get(w::EntityHandle{0x1042});
	if (!expect(respawned.retired.size() == 1 && respawned.spawned.size() == 1 &&
			boat_again != nullptr && boat_again != nullptr &&
			boat_again->spawn_origin == w::spawn_origin_pack(1, 1) &&
			boat_again->bms_id == 0x1043 &&
			materializer.take_retired_placement_ids().empty(),
			"a same-type re-spawn carries the placed identity and retires nothing"))
		return false;
	nw::PoolSpawnRecord retyped = vehicle;
	retyped.item_type_id = 5009;
	nw::PoolSpawnBatch retyped_batch;
	retyped_batch.records.push_back(retyped);
	pipeline.apply(0x0D, nw::encode_pool_spawn_batch(retyped_batch));
	materializer.sync(pipeline.state(), world);
	const std::vector<int32_t> retired_ids = materializer.take_retired_placement_ids();
	const w::Entity *retyped_row = world.registry.get(w::EntityHandle{0x1042});
	if (!expect(retired_ids.size() == 1 && retired_ids[0] == 0x1043 &&
			retyped_row != nullptr && retyped_row->item_id == 5009 &&
			retyped_row->spawn_origin == w::kSpawnOriginNone &&
			retyped_row->bms_id == 0 &&
			materializer.take_retired_placement_ids().empty(),
			"a re-typed slot retires its placed identity once and its occupant is wire-direct"))
		return false;
	return true;
}

int main() {
	if (!exact_registry_slot_contract()) return 1;
	if (!registry_lifetime_rejects_handle_reuse()) return 1;
	if (!malformed_load_indices_do_not_alias_low_slots()) return 1;
	if (!external_same_type_reuse_is_never_mutated_or_retired()) return 1;
	if (!preoccupied_exact_slot_requires_a_fresh_wire_generation()) return 1;
	if (!wire_target_authors_ground_separately_from_parent()) return 1;
	if (!pool0_parent_never_aliases_the_native_body()) return 1;
	if (!deployed_item_spawn_update_and_remove_materialize()) return 1;
	if (!entity_remove_detaches_children_in_place()) return 1;
	if (!entity_remove_takes_the_eweap_refnum_group()) return 1;
	if (!materialized_rows_join_their_refnum_group()) return 1;
	if (!objective_state_attaches_and_detaches_flag()) return 1;
	if (!objective_state_drop_runs_on_the_client()) return 1;
	if (!objective_drop_off_the_local_player_reads_its_own_pose()) return 1;
	if (!carrier_destroy_drops_the_flag()) return 1;
	if (!decoded_world_stream_materializes_exact_rows()) return 1;
	if (!pool2_tail_beyond_1024_materializes()) return 1;
	if (!streamed_rows_take_a_placed_identity_at_the_fence()) return 1;
	std::puts("client_world_materializer_test: PASS");
	return 0;
}
