// The joiner's folds of the retail S2C messages the dispatch audit found
// without a faithful consumer (docs/net/retail-message-dispatch-audit.md):
//   * S2C 0x50 rebinds a player's identity pair (NetId + animSlot), the avatar
//     the presentation keys on [orig: NapiNPClientMsg_TeamAssign @0x431910];
//   * S2C 0x51 folds one team-change list entry and walks the list with C2S
//     0x29 {index + 1} [orig: NapiNPClientMsg_HandlePlayerSpawn @0x431BB0];
//   * S2C 0x4E kills a join-window kill-list page silently and walks the list
//     with C2S 0x28 [orig: NapiNPClientMsg_HandleBatchKill @0x431870];
//   * S2C 0x24 splits with the retail quote-aware tokenizer and SETFLASH1 arms
//     the lightning timer [orig: NapiNPClientMsg_HandleTextCommand @0x429E70];
//   * S2C 0x37 lands the host's door row state [orig:
//     NapiNPClientMsg_HandleWeaponSlotAction @0x431250].
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
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
#include <runtime/world/world.h>

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
	rec.def_type = 3; // ItemDefType person
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

// Collect the joiner's C2S messages of one tag after one framed S2C delivery.
struct FramedJoiner {
	const std::string client_scrk = "CLIENT-LANE-D-SCRK";
	const std::string server_scrk = "SERVER-LANE-D-SCRK";
	inmatch::ClientRuntime client{"LaneDRuntime"};
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	uint32_t frame = 1;
	FramedJoiner() {
		client.seed_session(0x4E280001u, 1u, client_scrk, server_scrk,
				1, 0, 0x0002, world::kPlayerInfantryTypeId);
	}
	std::vector<std::vector<uint8_t>> deliver(const std::vector<ProtocolMessage> &messages,
			uint8_t tag) {
		std::vector<uint8_t> body;
		std::vector<std::vector<uint8_t>> out;
		if (!frame_session_packet(server_tx, SessionCrypto{server_scrk, {}, 1u}, messages, body))
			return out;
		const std::vector<uint8_t> datagram =
				nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
		client.receive(datagram.data(), datagram.size());
		for (const std::vector<uint8_t> &sent : client.Client_ProcessNetworkFrame(frame++)) {
			uint8_t opcode = 0;
			std::vector<uint8_t> plain;
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> c2s_messages;
			if (!nw_decode_inbound(sent.data(), sent.size(), opcode, plain) ||
					opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
					!decode_protocol_packet_plaintext(
							plain.data(), plain.size(), client_scrk, header, c2s_messages))
				continue;
			for (const ProtocolMessage &m : c2s_messages)
				if (m.tag == tag) out.push_back(m.payload);
		}
		return out;
	}
};

std::vector<uint8_t> le32(uint32_t v) {
	return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
}

// S2C 0x4E: every complete word after the leading resume word dies through the
// 0x26 route's kill with flags 1 (the silent death); a trailing odd byte is
// never read and a bare page kills nothing.
// [orig: NapiNPClientMsg_HandleBatchKill @0x431870 — @0x43188a, @0x431893,
//  Entity_KillBySlotId(slot, 0, 1) @0x4318b5]
void test_batch_kill_page_kills_silently() {
	ClientReplicaPipeline view;
	BatchKillBatch page;
	page.count = 0x1007;
	page.slots = {0x1003, 0x2005};
	std::vector<uint8_t> wire = encode_batch_kill(page);
	wire.push_back(0x09); // a stray odd byte
	view.apply(s2c::KILL_BY_SLOT, wire);
	std::vector<EntityDeathEvent> deaths;
	for (const ClientEffectCommand &c : view.drain_effect_commands())
		if (const auto *d = std::get_if<EntityDeathEvent>(&c)) deaths.push_back(*d);
	CHECK(deaths.size() == 2);
	if (deaths.size() == 2) {
		CHECK(deaths[0].entity_handle == 0x1003 && deaths[1].entity_handle == 0x2005);
		CHECK(deaths[0].item_state && deaths[0].kill_flags == 1 && deaths[0].hit_section == 0);
		CHECK(deaths[1].kill_flags == 1);
	}
	CHECK(view.unknown_tags() == 0);
	view.apply(s2c::KILL_BY_SLOT, {0xFF, 0xFF});
	view.apply(s2c::KILL_BY_SLOT, {0xFF, 0xFF, 0x03});
	CHECK(view.drain_effect_commands().empty());
}

// The wire leg: a page with at least one slot queues ONE C2S 0x28 {the S2C
// 0x19 value, the S2C 0x1A value, the page's resume word}; a bare page ends
// the walk. [orig: NapiNPClientMsg_HandleBatchKill @0x4318db..0x4318ff;
//  NapiNPClientMsg_0x01A @0x425ecb]
void test_batch_kill_walks_the_list() {
	FramedJoiner joiner;
	CHECK(joiner.deliver({make_protocol_message(s2c::SPAWN_ACK_TIMESTAMP, le32(0x11223344u)),
			make_protocol_message(s2c::WAIT_FOR_GAME_START_ACK, le32(0x55667788u))},
			c2s::LOADOUT_REQUEST).empty());
	BatchKillBatch page;
	page.count = 0x0021;
	page.slots = {0x1003};
	std::vector<std::vector<uint8_t>> sent = joiner.deliver(
			{make_protocol_message(s2c::KILL_BY_SLOT, encode_batch_kill(page))},
			c2s::LOADOUT_REQUEST);
	CHECK(sent.size() == 1);
	if (sent.size() == 1)
		CHECK(sent[0] == std::vector<uint8_t>({0x44, 0x33, 0x22, 0x11,
				0x88, 0x77, 0x66, 0x55, 0x21, 0x00}));
	sent = joiner.deliver({make_protocol_message(s2c::KILL_BY_SLOT, {0xFF, 0xFF})},
			c2s::LOADOUT_REQUEST);
	CHECK(sent.empty());
}

std::vector<uint8_t> text_body(const std::string &text) {
	std::vector<uint8_t> body(text.begin(), text.end());
	body.push_back(0);
	return body;
}

std::vector<int32_t> flash_timers(ClientReplicaPipeline &view) {
	std::vector<int32_t> out;
	for (const ClientEffectCommand &c : view.drain_effect_commands())
		if (const auto *f = std::get_if<LightningTimerCommand>(&c)) out.push_back(f->timer_a);
	return out;
}

// S2C 0x24: the first token selects the command case-insensitively, quotes are
// dropped by the tokenizer, SETFLASH1 alone arms timer A at 16 and with an
// argument at atol(n); the argument commands ignore a bare token.
// [orig: NapiNPClientMsg_HandleTextCommand @0x429E70 — String_TokenizeQuotedToArray
//  @0x429eb2, SETFLASH1 @0x429ecf..0x429ef5, SETCEASEFIRE's argument gate
//  @0x429f2e]
void test_text_command_setflash_and_tokenizer() {
	ClientReplicaPipeline view;
	view.apply(s2c::TEXT_COMMAND, text_body("SETFLASH1 16")); // the host's Lightning verb
	view.apply(s2c::TEXT_COMMAND, text_body("setflash1"));
	view.apply(s2c::TEXT_COMMAND, text_body("\"SETFLASH1\" \"5\""));
	view.apply(s2c::TEXT_COMMAND, text_body("SETFLASH1 7 extra"));
	CHECK(flash_timers(view) == std::vector<int32_t>({16, 16, 5, 7}));

	CHECK(!view.state().cease_fire);
	view.apply(s2c::TEXT_COMMAND, text_body("SETCEASEFIRE"));
	CHECK(!view.state().cease_fire);
	view.apply(s2c::TEXT_COMMAND, text_body("  setceasefire   \"1\"  "));
	CHECK(view.state().cease_fire);
	view.apply(s2c::TEXT_COMMAND, text_body("SETCEASEFIRE 0 trailing"));
	CHECK(!view.state().cease_fire);
	view.apply(s2c::TEXT_COMMAND, text_body(""));
	CHECK(flash_timers(view).empty());

	// The effect pass stores the timer on the world's weather.
	auto world = std::make_unique<world::World>();
	inmatch::ClientRuntime runtime("LaneDFlash");
	runtime.view().apply(s2c::TEXT_COMMAND, text_body("SETFLASH1 9"));
	runtime.apply_received_effects(*world);
	CHECK(world->weather.core.lightning.timer_a == 9);
}

// A two-door building in pool 2 with its door records allocated.
world::EntityHandle spawn_door(world::World &w) {
	world::Entity e;
	e.kind = world::EntityKind::Building;
	e.item_type = 5;
	e.item_id = 1998;
	e.has_item_def = true;
	e.door_count = 2;
	e.door_first_bone = 1;
	e.door_event = e.door_motion = true;
	const world::EntityHandle h = w.registry.spawn(2, e);
	w.doors.initialize(*w.registry.get(h), 529, 0);
	return h;
}

// S2C 0x37: row (first + number - 1) takes the state; closed snaps phase 0,
// open 65536, opening keeps its phase; number 0 and number past the count land
// nowhere. [orig: NapiNPClientMsg_HandleWeaponSlotAction @0x431250 — the gates
//  @0x4312d4..0x4312f8, the store @0x431307, phases @0x431316 / @0x43131f]
void test_door_row_update_lands_on_the_record() {
	auto w = std::make_unique<world::World>();
	w->registry.configure_pool(2, 8);
	const world::EntityHandle h = spawn_door(*w);
	const world::Entity &door = *w->registry.get(h);
	inmatch::ClientRuntime runtime("LaneDDoors");
	const auto deliver = [&](int16_t state, uint8_t number) {
		DoorSlotAction action;
		action.entity_handle = h.packed;
		action.state = state;
		action.number = number;
		runtime.view().apply(s2c::DOOR_SLOT_ACTION, encode_door_slot_action(action));
		runtime.apply_received_effects(*w);
	};
	deliver(2, 2); // the second row completes open
	CHECK(w->doors.slot(door, 1) != nullptr && w->doors.slot(door, 1)->state == 2 &&
			w->doors.slot(door, 1)->phase == 65536);
	CHECK(w->doors.slot(door, 0)->state == 0);
	deliver(1, 1); // the first row starts opening; its phase stays
	CHECK(w->doors.slot(door, 0)->state == 1 && w->doors.slot(door, 0)->phase == 0);
	deliver(2, 0); // a zero number (the command path's section 0) is dropped
	deliver(2, 3); // past the door count
	CHECK(w->doors.slot(door, 0)->state == 1 && w->doors.slot(door, 1)->state == 2);
	deliver(0, 2); // closed snaps the phase back
	CHECK(w->doors.slot(door, 1)->state == 0 && w->doors.slot(door, 1)->phase == 0);
	deliver(-1, 1); // the state word is signed and stored raw
	CHECK(w->doors.slot(door, 0)->state == -1);
}

} // namespace

int main() {
	test_team_assign_rebinds_player_identity();
	test_team_change_confirm_folds_the_entry();
	test_team_change_confirm_walks_the_list();
	test_batch_kill_page_kills_silently();
	test_batch_kill_walks_the_list();
	test_text_command_setflash_and_tokenizer();
	test_door_row_update_lands_on_the_record();
	std::printf("client_message_folds: %d failures\n", failures);
	return failures ? 1 : 0;
}
