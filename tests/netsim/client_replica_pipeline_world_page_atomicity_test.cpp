// World-load pages fold like retail's handlers walk them: records apply as
// they decode, so a malformed page loses only its unread remainder. The
// half-read record the decoder stages at the failure point is never exposed,
// a complete prefix (or a complete page with trailing junk) applies, and
// preexisting replica state is untouched by the failure.

#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/inmatch/client_replica_present_projection.h>

#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/world/present_rows.h>

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;

struct StateStamp {
	std::uint64_t revision = 0;
	std::uint64_t topology_revision = 0;
	std::uint64_t world_stream_revision = 0;
	std::size_t entity_count = 0;
};

StateStamp stamp(const ns::ClientState &state) {
	return {state.revision, state.topology_revision,
	        state.world_stream_revision, state.entities.size()};
}

bool expect(bool ok, const char *what, const char *page) {
	if (!ok) std::fprintf(stderr, "FAIL [%s]: %s\n", page, what);
	return ok;
}

bool applies_complete_prefix(
		std::uint8_t tag, const std::vector<std::uint8_t> &body,
		std::initializer_list<std::uint16_t> applied_handles,
		std::initializer_list<std::uint16_t> rejected_handles,
		const char *page) {
	ns::ClientReplicaPipeline pipeline;
	ns::ClientEntityState &sentinel = pipeline.state().upsert(0x4FFEu);
	sentinel.type_id = 0x7EEFu;
	sentinel.name = "preexisting";
	sentinel.x = 0x12345678;

	pipeline.apply(tag, body);

	bool ok = true;
	for (std::uint16_t handle : applied_handles) {
		ok = expect(pipeline.state().find(handle) != nullptr,
		            "a complete record from the malformed page was dropped",
		            page) && ok;
	}
	for (std::uint16_t handle : rejected_handles) {
		ok = expect(pipeline.state().find(handle) == nullptr,
		            "the half-read failure record was exposed", page) && ok;
	}
	const ns::ClientEntityState *kept = pipeline.state().find(0x4FFEu);
	ok = expect(kept != nullptr && kept->type_id == 0x7EEFu &&
	                    kept->name == "preexisting" && kept->x == 0x12345678,
	            "preexisting state changed after the malformed page", page) && ok;
	return ok;
}

nw::OrganicSpawnRecord organic_record(
		std::uint16_t handle, std::uint16_t type_id, const char *name) {
	nw::OrganicSpawnRecord rec;
	rec.slot_id = handle;
	rec.has_body = true;
	rec.item_type_id = type_id;
	rec.owner_connection_id = 0x01020304u;
	rec.entity_name = name;
	rec.minimap_flags = 0x0100u;
	rec.pos_x = 0x00110000;
	rec.pos_y = 0x00220000;
	rec.pos_z = 0x00330000;
	rec.orientation = 0x40000000;
	rec.team = 2;
	rec.ai_state = 3;
	rec.anim_slot = 4;
	rec.net_id = static_cast<std::uint16_t>(handle + 0x20u);
	rec.player_class = 5;
	rec.ai_action = 6;
	rec.skip_byte = 7;
	rec.unused_byte = 8;
	rec.alert_level = 9;
	rec.sub_type = 10;
	rec.weapon_type = 11;
	rec.parent_slot = 12;
	rec.parent_handle = 0x1ABCu;
	return rec;
}

bool test_organic_page_applies_complete_prefix() {
	nw::OrganicSpawnBatch two_records;
	two_records.records.push_back(organic_record(0x0010u, 0x1410u, "alpha"));
	two_records.records.push_back(organic_record(0x0011u, 0x1411u, "bravo"));
	two_records.entity_count =
			static_cast<std::uint16_t>(two_records.records.size());
	std::vector<std::uint8_t> truncated =
			nw::encode_organic_spawn_batch(two_records);
	if (!expect(!truncated.empty(), "encoder returned an empty page", "0x0C truncated"))
		return false;
	truncated.pop_back(); // final byte of the second record's parent_handle

	nw::OrganicSpawnBatch one_record;
	one_record.records.push_back(organic_record(0x0012u, 0x1412u, "charlie"));
	one_record.entity_count = 1;
	std::vector<std::uint8_t> trailing =
			nw::encode_organic_spawn_batch(one_record);
	trailing.push_back(0xA5u);

	bool ok = true;
	ok = applies_complete_prefix(
			nw::s2c::ENTITY_SPAWN_BATCH, truncated, {0x0010u}, {0x0011u},
			"0x0C truncated") && ok;
	ok = applies_complete_prefix(
			nw::s2c::ENTITY_SPAWN_BATCH, trailing, {0x0012u}, {},
			"0x0C trailing") && ok;
	return ok;
}

bool test_player_character_identity_reaches_present_row() {
	ns::ClientReplicaPipeline pipeline;
	nw::OrganicSpawnBatch batch;
	nw::OrganicSpawnRecord rec = organic_record(0x0014u, 0x14B9u, "custom");
	rec.anim_slot = 3;
	rec.net_id = 0x0400u;
	batch.records.push_back(rec);
	batch.entity_count = 1;
	pipeline.apply(nw::s2c::ENTITY_SPAWN_BATCH,
			nw::encode_organic_spawn_batch(batch));

	const ns::ClientEntityState *row = pipeline.state().find(rec.slot_id);
	if (!expect(row != nullptr && row->net_id == rec.net_id,
			"0x0C packed character id was dropped by the replica fold",
			"player identity"))
		return false;

	float present[nw::world::PF_STRIDE];
	nw::inmatch::initialize_client_replica_present_row(present);
	nw::inmatch::ClientReplicaPresentContext context;
	nw::inmatch::project_client_replica_present_row(
			present, *row, pipeline.state(), context);
	bool ok = expect(
			static_cast<int>(present[nw::world::PF_CHARACTER_ID]) == rec.net_id,
			"player character identity was dropped before presentation",
			"player identity");
	// The carrier link's one sentinel: a fresh spawn (no link, wire 0xFFFF)
	// publishes -1, a linked row publishes the packed handle.
	ok = expect(present[nw::world::PF_CARRIER_HANDLE] == -1.0f,
			"an unlinked row must publish the -1 carrier sentinel",
			"carrier sentinel") && ok;
	ns::ClientEntityState linked = *row;
	linked.carrier_handle = 0x1234u;
	nw::inmatch::initialize_client_replica_present_row(present);
	nw::inmatch::project_client_replica_present_row(
			present, linked, pipeline.state(), context);
	ok = expect(
			static_cast<int>(present[nw::world::PF_CARRIER_HANDLE]) == 0x1234,
			"the decoded carrier link was dropped before presentation",
			"carrier link") && ok;
	return ok;
}

nw::PoolSpawnRecord pool_spawn_record(
		std::uint16_t handle, std::uint16_t type_id, const char *name) {
	nw::PoolSpawnRecord rec;
	rec.slot_id = handle;
	rec.item_type_id = type_id;
	rec.entity_name = name;
	rec.pos_x = 0x00140000;
	rec.pos_y = 0x00250000;
	rec.pos_z = 0x00360000;
	rec.bone_byte = 0x5Au;
	return rec;
}

bool test_pool_spawn_page_applies_complete_prefix() {
	nw::PoolSpawnBatch two_records;
	two_records.records.push_back(pool_spawn_record(0x1010u, 0x2410u, "delta"));
	two_records.records.push_back(pool_spawn_record(0x1011u, 0x2411u, "echo"));
	std::vector<std::uint8_t> truncated =
			nw::encode_pool_spawn_batch(two_records);
	if (!expect(!truncated.empty(), "encoder returned an empty page", "0x0D truncated"))
		return false;
	truncated.pop_back(); // final byte of the second record's mandatory bone field

	nw::PoolSpawnBatch one_record;
	one_record.records.push_back(pool_spawn_record(0x1012u, 0x2412u, "foxtrot"));
	std::vector<std::uint8_t> trailing =
			nw::encode_pool_spawn_batch(one_record);
	trailing.push_back(0xA5u);

	bool ok = true;
	ok = applies_complete_prefix(
			nw::s2c::POOL_SPAWN, truncated, {0x1010u}, {0x1011u},
			"0x0D truncated") && ok;
	ok = applies_complete_prefix(
			nw::s2c::POOL_SPAWN, trailing, {0x1012u}, {},
			"0x0D trailing") && ok;
	return ok;
}

nw::StaticEntityRecord static_entity_record(std::uint16_t type_id) {
	nw::StaticEntityRecord rec;
	rec.item_type_id = type_id;
	rec.pos_x = 0x00170000;
	rec.pos_y = 0x00280000;
	rec.pos_z = 0x00390000;
	rec.ammo_count = 0x31u;
	// With no optional tail, weapon_byte is the record's final required byte.
	rec.weapon_byte = 0;
	return rec;
}

bool test_static_page_applies_complete_prefix() {
	nw::StaticEntityBatch two_records;
	two_records.start_index = 0x0010u;
	two_records.records.push_back(static_entity_record(0x3410u));
	two_records.records.push_back(static_entity_record(0x3411u));
	std::vector<std::uint8_t> truncated =
			nw::encode_static_entity_batch(two_records);
	if (!expect(!truncated.empty(), "encoder returned an empty page", "0x10 truncated"))
		return false;
	truncated.pop_back(); // final byte of the second record's mandatory weapon field

	nw::StaticEntityBatch one_record;
	one_record.start_index = 0x0012u;
	one_record.records.push_back(static_entity_record(0x3412u));
	std::vector<std::uint8_t> trailing =
			nw::encode_static_entity_batch(one_record);
	trailing.push_back(0xA5u);

	bool ok = true;
	ok = applies_complete_prefix(
			nw::s2c::STATIC_ENTITY_BATCH, truncated, {0x2010u}, {0x2011u},
			"0x10 truncated") && ok;
	ok = applies_complete_prefix(
			nw::s2c::STATIC_ENTITY_BATCH, trailing, {0x2012u}, {},
			"0x10 trailing") && ok;
	return ok;
}

nw::Pool3SyncRecord pool3_record(
		std::uint16_t type_id, std::uint16_t net_handle) {
	nw::Pool3SyncRecord rec;
	rec.item_type_id = type_id;
	rec.pos_x = 0x001A0000;
	rec.pos_y = 0x002B0000;
	rec.pos_z = 0x003C0000;
	// With all optional fields zero, net_handle is the record's final required u16.
	rec.net_handle = net_handle;
	return rec;
}

bool test_pool3_page_applies_complete_prefix() {
	nw::Pool3SyncBatch two_records;
	two_records.start_index = 0x0010u;
	two_records.records.push_back(pool3_record(0x4410u, 0x5010u));
	two_records.records.push_back(pool3_record(0x4411u, 0x5011u));
	std::vector<std::uint8_t> truncated =
			nw::encode_pool3_sync_batch(two_records);
	if (!expect(!truncated.empty(), "encoder returned an empty page", "0x20 truncated"))
		return false;
	truncated.pop_back(); // final byte of the second record's mandatory net_handle

	nw::Pool3SyncBatch one_record;
	one_record.start_index = 0x0012u;
	one_record.records.push_back(pool3_record(0x4412u, 0x5012u));
	std::vector<std::uint8_t> trailing =
			nw::encode_pool3_sync_batch(one_record);
	trailing.push_back(0xA5u);

	bool ok = true;
	ok = applies_complete_prefix(
			nw::s2c::POOL3_SYNC, truncated, {0x3010u}, {0x3011u},
			"0x20 truncated") && ok;
	ok = applies_complete_prefix(
			nw::s2c::POOL3_SYNC, trailing, {0x3012u}, {},
			"0x20 trailing") && ok;
	return ok;
}

bool test_empty_static_and_pool3_rows_are_tombstones() {
	ns::ClientReplicaPipeline pipeline;

	nw::StaticEntityBatch static_live;
	static_live.start_index = 7;
	static_live.records.push_back(static_entity_record(0x3607u));
	pipeline.apply(nw::s2c::STATIC_ENTITY_BATCH,
			nw::encode_static_entity_batch(static_live));

	nw::Pool3SyncBatch marker_live;
	marker_live.start_index = 9;
	marker_live.records.push_back(pool3_record(0x4609u, 0x7009u));
	pipeline.apply(nw::s2c::POOL3_SYNC,
			nw::encode_pool3_sync_batch(marker_live));
	if (!expect(pipeline.state().find(0x2007u) != nullptr &&
				pipeline.state().find(0x3009u) != nullptr,
			"live setup rows were decoded", "empty tombstones"))
		return false;

	const StateStamp before = stamp(pipeline.state());
	nw::StaticEntityRecord static_empty;
	static_empty.is_empty_slot = true;
	nw::StaticEntityBatch static_tombstone;
	static_tombstone.start_index = 7;
	static_tombstone.records.push_back(static_empty);
	pipeline.apply(nw::s2c::STATIC_ENTITY_BATCH,
			nw::encode_static_entity_batch(static_tombstone));
	if (!expect(pipeline.state().find(0x2007u) == nullptr &&
				pipeline.state().find(0x3009u) != nullptr,
			"a canonical empty 0x10 row clears its exact slot",
			"empty tombstones"))
		return false;

	nw::Pool3SyncRecord marker_empty;
	marker_empty.is_empty_slot = true;
	nw::Pool3SyncBatch marker_tombstone;
	marker_tombstone.start_index = 9;
	marker_tombstone.records.push_back(marker_empty);
	pipeline.apply(nw::s2c::POOL3_SYNC,
			nw::encode_pool3_sync_batch(marker_tombstone));
	const StateStamp after = stamp(pipeline.state());
	return expect(pipeline.state().find(0x3009u) == nullptr &&
				after.entity_count + 2 == before.entity_count &&
				after.topology_revision == before.topology_revision + 2 &&
				after.world_stream_revision ==
						before.world_stream_revision + 2 &&
				after.revision > before.revision,
			"empty 0x10/0x20 rows retire both slots and advance the world fold",
			"empty tombstones");
}

void poison_compact_state(ns::ClientEntityState &row) {
	row.carrier_handle = 0x1001u;
	row.mount_bone = 9;
	row.anim_state_id = 88;
	row.move_input = 0xC0u;
	row.recoil_pitch = 0x12345678;
	row.parent_pose_valid = true;
	row.state_flags = 3;
	row.state_flags_known = true;
	row.health_word = 1;
	row.health_known = true;
	row.respawn_revision = 9;
	row.net_has_compact = true;
	row.vehicle_speed_reg = 0x23456789;
	row.compact_revision = 12;
	row.rm_adm_id = 17;
	row.rm_state = 44;
	row.net_seat_valid = true;
}

bool compact_state_was_reset(const ns::ClientEntityState &row) {
	return row.carrier_handle == 0xFFFFu && row.mount_bone == 0 &&
			row.anim_state_id == 0 && row.move_input == 0 &&
			row.recoil_pitch == 0 && !row.parent_pose_valid &&
			row.state_flags == 0 && !row.state_flags_known &&
			row.health_word == 0 && !row.health_known &&
			row.respawn_revision == 0 && !row.net_has_compact &&
			row.vehicle_speed_reg == 0 && row.compact_revision == 0 &&
			row.rm_adm_id == -2 && row.rm_state == -1 &&
			!row.net_seat_valid;
}

bool test_repeated_load_records_reset_complete_replica_rows() {
	ns::ClientReplicaPipeline pipeline;

	nw::PoolSpawnBatch pool1;
	pool1.records.push_back(pool_spawn_record(0x1004u, 0x2504u, "pool1"));
	pipeline.apply(nw::s2c::POOL_SPAWN, nw::encode_pool_spawn_batch(pool1));
	ns::ClientEntityState *pool1_row = pipeline.state().find(0x1004u);
	const std::uint32_t pool1_generation = pool1_row->spawn_revision;
	poison_compact_state(*pool1_row);
	pipeline.apply(nw::s2c::POOL_SPAWN, nw::encode_pool_spawn_batch(pool1));
	pool1_row = pipeline.state().find(0x1004u);
	if (!expect(pool1_row != nullptr &&
				pool1_row->spawn_revision == pool1_generation + 1 &&
				compact_state_was_reset(*pool1_row),
			"a repeated 0x0D memsets compact/presentation/mover state",
			"full row reset"))
		return false;

	nw::StaticEntityBatch pool2;
	pool2.start_index = 5;
	pool2.records.push_back(static_entity_record(0x3505u));
	pipeline.apply(nw::s2c::STATIC_ENTITY_BATCH,
			nw::encode_static_entity_batch(pool2));
	ns::ClientEntityState *pool2_row = pipeline.state().find(0x2005u);
	const std::uint32_t pool2_generation = pool2_row->spawn_revision;
	poison_compact_state(*pool2_row);
	pipeline.apply(nw::s2c::STATIC_ENTITY_BATCH,
			nw::encode_static_entity_batch(pool2));
	pool2_row = pipeline.state().find(0x2005u);
	if (!expect(pool2_row != nullptr &&
				pool2_row->spawn_revision == pool2_generation + 1 &&
				compact_state_was_reset(*pool2_row),
			"a repeated 0x10 memsets compact/presentation/mover state",
			"full row reset"))
		return false;

	nw::Pool3SyncBatch pool3;
	pool3.start_index = 6;
	pool3.records.push_back(pool3_record(0x4506u, 0x7006u));
	pipeline.apply(nw::s2c::POOL3_SYNC, nw::encode_pool3_sync_batch(pool3));
	ns::ClientEntityState *pool3_row = pipeline.state().find(0x3006u);
	const std::uint32_t pool3_generation = pool3_row->spawn_revision;
	poison_compact_state(*pool3_row);
	pipeline.apply(nw::s2c::POOL3_SYNC, nw::encode_pool3_sync_batch(pool3));
	pool3_row = pipeline.state().find(0x3006u);
	return expect(pool3_row != nullptr &&
				pool3_row->spawn_revision == pool3_generation + 1 &&
				compact_state_was_reset(*pool3_row),
			"a repeated 0x20 memsets compact/presentation/mover state",
			"full row reset");
}

} // namespace

int main() {
	bool ok = true;
	ok = test_organic_page_applies_complete_prefix() && ok;
	ok = test_player_character_identity_reaches_present_row() && ok;
	ok = test_pool_spawn_page_applies_complete_prefix() && ok;
	ok = test_static_page_applies_complete_prefix() && ok;
	ok = test_pool3_page_applies_complete_prefix() && ok;
	ok = test_empty_static_and_pool3_rows_are_tombstones() && ok;
	ok = test_repeated_load_records_reset_complete_replica_rows() && ok;
	if (ok) std::printf("client_replica_pipeline_world_page_atomicity: OK\n");
	return ok ? 0 : 1;
}
