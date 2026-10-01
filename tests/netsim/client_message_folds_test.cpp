// The joiner's folds of the retail S2C messages the dispatch audit found
// without a faithful consumer (docs/net/retail-message-dispatch-audit.md):
//   * S2C 0x50 rebinds a player's identity pair (NetId + animSlot), the avatar
//     the presentation keys on [orig: NapiNPClientMsg_TeamAssign @0x431910];
//   * S2C 0x51 folds one team-change list entry and walks the list with C2S
//     0x29 {index + 1} [orig: NapiNPClientMsg_HandlePlayerSpawn @0x431BB0].
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_keys.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/replication/client_replica_pipeline.h>
#include <runtime/world/player_spawn.h>

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

// S2C 0x51: the entry's team lands off the authority only, a player row takes
// the identity pair, and a non-player keeps its own.
// [orig: NapiNPClientMsg_HandlePlayerSpawn @0x431BB0 — Team @0x431c6d behind
//  !is_authority @0x431c6b, the player gate @0x431ca5, NetId @0x431cad,
//  animSlot @0x431cff]
void test_team_change_confirm_folds_the_entry() {
	ClientReplicaPipeline view;
	view.apply(s2c::ENTITY_SPAWN_BATCH, player_spawn(0x0006, 1, 0x0401, 2));
	TeamAssign entry;
	entry.entity_handle = 0x0006;
	entry.team = 2;
	entry.net_id = 0x8403;
	entry.anim_slot = 5;
	view.apply(s2c::TEAM_CHANGE_CONFIRM, encode_team_change_confirm(0, entry));
	const ClientEntityState *row = view.state().find(0x0006);
	CHECK(row != nullptr && row->team == 2 && row->net_id == 0x8403 &&
			row->spawn_anim_slot == 5);
	CHECK(view.malformed_bodies() == 0);
	CHECK(view.unknown_tags() == 0);

	// The authority's own view keeps its team; the identity pair still lands.
	ClientReplicaPipeline authority;
	authority.set_authority_recipient(true);
	authority.apply(s2c::ENTITY_SPAWN_BATCH, player_spawn(0x0006, 1, 0x0401, 2));
	authority.apply(s2c::TEAM_CHANGE_CONFIRM, encode_team_change_confirm(0, entry));
	row = authority.state().find(0x0006);
	CHECK(row != nullptr && row->team == 1 && row->net_id == 0x8403);

	// A pool-3 entry (a zone a capture flipped) takes the team alone.
	TeamAssign zone;
	zone.entity_handle = 0x3002;
	zone.team = 1;
	view.apply(s2c::TEAM_CHANGE_CONFIRM, encode_team_change_confirm(1, zone));
	row = view.state().find(0x3002);
	CHECK(row != nullptr && row->team == 1 && row->team_known);
}

// The wire leg: a joiner answers each S2C 0x51 entry with one reliable C2S
// 0x29 {index + 1} at its next send boundary; a sentinel handle draws none.
// [orig: NapiNPClientMsg_HandlePlayerSpawn @0x431c1a, the queue
//  @0x431c88..0x431c99]
void test_team_change_confirm_walks_the_list() {
	const std::string client_scrk = "CLIENT-TEAM-WALK-SCRK";
	const std::string server_scrk = "SERVER-TEAM-WALK-SCRK";
	inmatch::ClientRuntime client("TeamWalkRuntime");
	client.seed_session(0x51290001u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, world::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	const auto deliver_and_collect = [&](const std::vector<ProtocolMessage> &messages,
			uint32_t frame) {
		std::vector<uint8_t> body;
		std::vector<std::vector<uint8_t>> acks;
		if (!frame_session_packet(server_tx, SessionCrypto{server_scrk, {}, 1u},
				messages, body))
			return acks;
		const std::vector<uint8_t> datagram =
				nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
		client.receive(datagram.data(), datagram.size());
		for (const std::vector<uint8_t> &out : client.Client_ProcessNetworkFrame(frame)) {
			uint8_t opcode = 0;
			std::vector<uint8_t> plain;
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> sent;
			if (!nw_decode_inbound(out.data(), out.size(), opcode, plain) ||
					opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
					!decode_protocol_packet_plaintext(
							plain.data(), plain.size(), client_scrk, header, sent))
				continue;
			for (const ProtocolMessage &m : sent)
				if (m.tag == c2s::TEAM_SPAWN_ACK) acks.push_back(m.payload);
		}
		return acks;
	};
	TeamAssign entry;
	entry.entity_handle = 0x0006;
	entry.team = 2;
	std::vector<std::vector<uint8_t>> acks = deliver_and_collect(
			{make_protocol_message(s2c::TEAM_CHANGE_CONFIRM,
					encode_team_change_confirm(4, entry))}, 1);
	CHECK(acks.size() == 1);
	if (acks.size() == 1) CHECK(acks[0] == std::vector<uint8_t>({5, 0}));
	entry.entity_handle = 0xFFFF;
	acks = deliver_and_collect({make_protocol_message(s2c::TEAM_CHANGE_CONFIRM,
			encode_team_change_confirm(5, entry))}, 2);
	CHECK(acks.empty());
}

} // namespace

int main() {
	test_team_assign_rebinds_player_identity();
	test_team_change_confirm_folds_the_entry();
	test_team_change_confirm_walks_the_list();
	std::printf("client_message_folds: %d failures\n", failures);
	return failures ? 1 : 0;
}
