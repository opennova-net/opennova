// The joiner's folds of the retail S2C messages the dispatch audit found
// without a faithful consumer (docs/net/retail-message-dispatch-audit.md):
//   * S2C 0x50 rebinds a player's identity pair (NetId + animSlot), the avatar
//     the presentation keys on [orig: NapiNPClientMsg_TeamAssign @0x431910].
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/replication/client_replica_pipeline.h>

using namespace opennova;
using namespace opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// One pool-0 player (the person type) on `team` with an identity pair.
std::vector<uint8_t> player_spawn(uint16_t handle, uint8_t team, uint16_t net_id,
		uint8_t anim_slot) {
	OrganicSpawnRecord rec;
	rec.slot_id = handle;
	rec.has_body = true;
	rec.item_type_id = kPlayerPersonTypeId;
	rec.entity_name = "player";
	rec.minimap_flags = 0x0100u;
	rec.team = team;
	rec.anim_slot = anim_slot;
	rec.net_id = net_id;
	rec.player_class = 5;
	OrganicSpawnBatch batch;
	batch.records.push_back(rec);
	batch.entity_count = 1;
	return encode_organic_spawn_batch(batch);
}

// S2C 0x50 for a player writes the team AND rebinds the identity pair: the
// third field is the NetId (the packed character id the avatar is looked up
// by), the fourth the animSlot. A non-player row takes the team alone.
// [orig: NapiNPClientMsg_TeamAssign @0x431910 — team @0x4319ee, the player
//  gate @0x431a15, animSlot @0x431b3a, NetId @0x431b46, CharacterEntity
//  @0x431b91]
void test_team_assign_rebinds_player_identity() {
	ClientReplicaPipeline view;
	view.apply(s2c::ENTITY_SPAWN_BATCH, player_spawn(0x0005, 1, 0x0401, 2));
	const ClientEntityState *row = view.state().find(0x0005);
	CHECK(row != nullptr && row->cls == EntityClass::Player && row->net_id == 0x0401);
	const uint64_t before = view.state().revision;
	TeamAssign assign;
	assign.entity_handle = 0x0005;
	assign.team = 2;
	assign.net_id = 0x8402; // the other side's character
	assign.anim_slot = 7;
	view.apply(s2c::TEAM_ASSIGN, encode_team_assign(assign));
	row = view.state().find(0x0005);
	CHECK(row != nullptr);
	if (row != nullptr) {
		CHECK(row->team == 2);
		CHECK(row->net_id == 0x8402);
		CHECK(row->spawn_anim_slot == 7);
	}
	CHECK(view.state().revision != before);

	// A pool-1 item keeps its own identity; only the team moves.
	PoolSpawnRecord item;
	item.slot_id = 0x1003;
	item.item_type_id = 5;
	item.entity_name = "veh";
	PoolSpawnBatch items;
	items.records.push_back(item);
	view.apply(s2c::POOL_SPAWN, encode_pool_spawn_batch(items));
	const ClientEntityState *vehicle = view.state().find(0x1003);
	CHECK(vehicle != nullptr);
	const uint16_t vehicle_net_id = vehicle != nullptr ? vehicle->net_id : 0;
	assign.entity_handle = 0x1003;
	assign.team = 1;
	assign.net_id = 0x0123;
	assign.anim_slot = 9;
	view.apply(s2c::TEAM_ASSIGN, encode_team_assign(assign));
	vehicle = view.state().find(0x1003);
	CHECK(vehicle != nullptr && vehicle->team == 1 && vehicle->net_id == vehicle_net_id &&
			vehicle->spawn_anim_slot == 0);
}

} // namespace

int main() {
	test_team_assign_rebinds_player_identity();
	std::printf("client_message_folds: %d failures\n", failures);
	return failures ? 1 : 0;
}
