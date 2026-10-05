// D-DOOR-1 — the door records on the wire, host side. The authority reports
// every selected section of a door command as S2C 0x37 numbered 0-based (so
// the record it carries is the one before that section's, and the number-0
// packet never lands on a client), and every record that completes with its
// own 1-based number, both to every in-match remote but never the listen host
// (mask 0x90).
// [orig: Entity_ProcessSectionDamageTransition @0x43F370 (the send @0x43f462);
//  FadeEffect_UpdateAll @0x44E920 (the send @0x44e982);
//  Server_SendWeaponSlotActionPacket @0x50F9A0]
#include <runtime/inmatch/server_tick.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/world/player_spawn.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_keys.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/world/world.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/mission/item_traits.h>
#include <formats/def/def.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <cstdio>
#include <memory>
#include <vector>
#include "conn_fixture.h"

using namespace opennova;
namespace w = opennova::world;
namespace ns = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

std::vector<DoorSlotAction> take_doors(ns::LoopbackChannel &channel) {
	std::vector<DoorSlotAction> rows;
	ns::Datagram dg;
	while (channel.client_recv(dg)) {
		if (dg.tag != s2c::DOOR_SLOT_ACTION) continue;
		DoorSlotAction row;
		size_t consumed = 0;
		decode_door_slot_action(dg.body.data(), dg.body.size(), row, consumed);
		rows.push_back(row);
	}
	return rows;
}

void test_host_reports_door_commands_and_completions() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(2, 8);
	world.rules.logic_authority = true;
	world.rules.mp_session = true;
	w::Entity person;
	person.kind = w::EntityKind::Organic;
	person.flags = person.engine_flags = w::kEntityFlagPlayer;
	const w::EntityHandle remote = world.registry.spawn(0, person);
	const w::EntityHandle local = world.registry.spawn(0, person);
	ns::LoopbackChannel remote_wire, local_wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = ctx.is_in_session = 1;
	auto &connections = ctx.np_protocol.connection_list;
	connections.push_back(conn_fixture::make_conn(1, 1, &remote_wire,
			ns::TransportMode::Client, remote, true));
	connections.push_back(conn_fixture::make_conn(2, 2, &local_wire,
			ns::TransportMode::Loopback, local, true));
	for (size_t i = 0; i < connections.size(); ++i) {
		connections[i].reply.player_slot = static_cast<uint8_t>(i);
		connections[i].admission_stage = inmatch::GameAdmissionStage::Complete;
		connections[i].link.last_deploy_tick_valid = true;
	}

	w::Entity e;
	e.kind = w::EntityKind::Building;
	e.item_type = 5;
	e.item_id = 1998;
	e.has_item_def = true;
	e.door_count = 2;
	e.door_first_bone = 1;
	e.door_event = e.door_motion = true;
	const w::EntityHandle h = world.registry.spawn(2, e);
	world.doors.initialize(*world.registry.get(h), 16384, 0); // four ticks to open
	world.doors.command(world, *world.registry.get(h), 7);

	std::vector<DoorSlotAction> rows;
	for (int tick = 0; tick < 12 && rows.size() < 4; ++tick) {
		inmatch::Server_TickUpdate(ctx);
		const std::vector<DoorSlotAction> got = take_doors(remote_wire);
		rows.insert(rows.end(), got.begin(), got.end());
	}
	CHECK(rows.size() == 4);
	if (rows.size() == 4) {
		// The command: section 0 as number 0 (the record before this door's
		// first, none here, reads 0), section 1 as number 1 carrying record 0,
		// already opening.
		CHECK(rows[0].entity_handle == h.packed && rows[0].number == 0 && rows[0].state == 0);
		CHECK(rows[1].entity_handle == h.packed && rows[1].number == 1 && rows[1].state == 1);
		// The completions: each record's own 1-based number, open.
		CHECK(rows[2].number == 1 && rows[2].state == 2);
		CHECK(rows[3].number == 2 && rows[3].state == 2);
	}
	CHECK(take_doors(local_wire).empty());
	CHECK(world.out.entity_events.empty());

	// A closing command reports the records' new states the same way.
	world.doors.command(world, *world.registry.get(h), 8);
	inmatch::Server_TickUpdate(ctx);
	rows = take_doors(remote_wire);
	CHECK(rows.size() >= 2);
	if (rows.size() >= 2) {
		CHECK(rows[0].number == 0 && rows[1].number == 1 && rows[1].state == 3);
	}
}

// A single-player world reports nothing: the authority has no session.
void test_single_player_reports_nothing() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(2, 8);
	world.rules.logic_authority = true;
	world.rules.mp_session = false;
	w::Entity e;
	e.kind = w::EntityKind::Building;
	e.has_item_def = true;
	e.door_count = 1;
	e.door_event = e.door_motion = true;
	const w::EntityHandle h = world.registry.spawn(2, e);
	world.doors.initialize(*world.registry.get(h), 65536, 0);
	world.doors.command(world, *world.registry.get(h), 7);
	world.doors.tick(world);
	CHECK(world.out.entity_events.empty());
}

w::EntityHandle spawn_door(w::World &world, int count) {
	w::Entity e;
	e.kind = w::EntityKind::Building;
	e.item_type = 5;
	e.item_id = 1998;
	e.has_item_def = true;
	e.door_count = static_cast<int8_t>(count);
	e.door_first_bone = 1;
	e.door_event = e.door_motion = true;
	const w::EntityHandle h = world.registry.spawn(2, e);
	world.doors.initialize(*world.registry.get(h), 529, 0);
	return h;
}

// The host's C2S 0x1A leg: a closed record opens only for value 1, an opening
// or open record takes the value only for number 3, and the record's state
// goes back to the sender; number 0, a number past the count, an unknown row
// and a sender without a slot draw nothing.
// [orig: NapiNPServerMsg_HandleVoteUpdate @0x514B20]
void test_host_answers_door_requests() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(2, 8);
	world.rules.logic_authority = true;
	world.rules.mp_session = true;
	w::Entity person;
	person.kind = w::EntityKind::Organic;
	person.flags = person.engine_flags = w::kEntityFlagPlayer;
	const w::EntityHandle player = world.registry.spawn(0, person);
	ns::LoopbackChannel wire;
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = ctx.is_in_session = 1;
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(1, 1, &wire,
			ns::TransportMode::Client, player, true));
	inmatch::NapiNPConnection &sender = ctx.np_protocol.connection_list[0];
	const w::EntityHandle h = spawn_door(world, 3);
	const w::Entity &door = *world.registry.get(h);
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	const auto ask = [&](int16_t value, uint8_t number) {
		DoorSlotAction request;
		request.entity_handle = h.packed;
		request.state = value;
		request.number = number;
		std::vector<DoorSlotAction> replies;
		for (const ProtocolMessage &m : inmatch::dispatch_session_replies(ctx.config, sender,
				{make_protocol_message(c2s::DOOR_SLOT_REQUEST, encode_door_slot_action(request))},
				100, ctx.np_protocol.connection_list, &world, inputs)) {
			if (m.tag != s2c::DOOR_SLOT_ACTION) continue;
			DoorSlotAction row;
			size_t consumed = 0;
			decode_door_slot_action(m.payload.data(), m.payload.size(), row, consumed);
			replies.push_back(row);
		}
		return replies;
	};
	std::vector<DoorSlotAction> r = ask(1, 1);
	CHECK(r.size() == 1 && r[0].entity_handle == h.packed && r[0].state == 1 && r[0].number == 1);
	CHECK(world.doors.slot(door, 0)->state == 1);
	r = ask(2, 1); // opening, number != 3: unchanged, still answered
	CHECK(r.size() == 1 && r[0].state == 1 && world.doors.slot(door, 0)->state == 1);
	r = ask(2, 3); // record 2 is closed: only value 1 opens it
	CHECK(r.size() == 1 && r[0].state == 0 && world.doors.slot(door, 2)->state == 0);
	r = ask(1, 3);
	CHECK(r.size() == 1 && r[0].state == 1);
	r = ask(3, 3); // an opening record takes the value for number 3
	CHECK(r.size() == 1 && r[0].state == 3 && world.doors.slot(door, 2)->state == 3);
	CHECK(ask(1, 0).empty());
	CHECK(ask(1, 4).empty());
	sender.link.owned_entity = w::EntityHandle{};
	CHECK(ask(1, 2).empty());
	CHECK(world.doors.slot(door, 1)->state == 0);
}

// A client's door command asks the host for every selected section but
// section 0, each carrying the record before it, after this command's own
// transitions. [orig: Entity_ProcessSectionDamageTransition @0x43f482 ->
//  NetPacket_SendWeaponSwitch @0x42D0C0]
void test_client_command_raises_requests() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(2, 8);
	world.rules.logic_authority = false;
	world.rules.mp_session = true;
	const w::EntityHandle h = spawn_door(world, 3);
	world.doors.command(world, *world.registry.get(h), 7);
	const std::vector<w::DoorRowEvent> &asks = world.out.door_requests;
	CHECK(asks.size() == 2);
	if (asks.size() == 2) {
		CHECK(asks[0].handle == h.packed && asks[0].number == 1 && asks[0].state == 1);
		CHECK(asks[1].handle == h.packed && asks[1].number == 2 && asks[1].state == 1);
	}
	CHECK(world.out.entity_events.empty());
}

// The joiner queues each request as one reliable C2S 0x1A for its next send
// boundary. [orig: CNapiNetwork_QueueReliableMessage(0x1A, 1, 0, .., 5)
//  @0x42d169]
void test_joiner_sends_the_request() {
	const std::string client_scrk = "CLIENT-DOOR-REQ-SCRK";
	const std::string server_scrk = "SERVER-DOOR-REQ-SCRK";
	inmatch::ClientRuntime client("DoorRequest");
	client.seed_session(0x1A1A0001u, 1u, client_scrk, server_scrk,
			1, 0, 0x0002, w::kPlayerInfantryTypeId);
	CHECK(client.queue_door_request(0x2003, 1, 2));
	std::vector<std::vector<uint8_t>> sent;
	for (const std::vector<uint8_t> &datagram : client.Client_ProcessNetworkFrame(1)) {
		uint8_t opcode = 0;
		std::vector<uint8_t> plain;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!nw_decode_inbound(datagram.data(), datagram.size(), opcode, plain) ||
				opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
				!decode_protocol_packet_plaintext(plain.data(), plain.size(), client_scrk,
						header, messages))
			continue;
		for (const ProtocolMessage &m : messages)
			if (m.tag == c2s::DOOR_SLOT_REQUEST) sent.push_back(m.payload);
	}
	CHECK(sent.size() == 1);
	if (sent.size() == 1) CHECK(sent[0] == std::vector<uint8_t>({0x03, 0x20, 0x01, 0x00, 0x02}));
}


// D-DOOR-1, the late join: the authority's 0x10 record carries a door's
// section word with bits 1..count re-read from its rows (opening/open set,
// closed/closing clear, the other bits as the entity holds them), and carries
// it even when it is zero.
// [orig: NetPacket_SerializePool2StaticToBuffer @0x504432..0x5044B3]
void test_static_batch_carries_door_sections() {
	auto heap = std::make_unique<w::World>();
	w::World &world = *heap;
	world.registry.configure_pool(2, 8);
	w::Entity e;
	e.kind = w::EntityKind::Building;
	e.item_id = 700;
	e.has_item_def = true;
	e.item_attrib = 0x80u;   // Door
	e.deathtime_ticks = 3;    // def+0x890's low byte: three doors
	e.door_count = 3;
	e.door_first_bone = 1;
	e.section_mask = 0x29u;   // bits 0 and 5 outside the doors, bit 3 stale
	const w::EntityHandle open_door = world.registry.spawn(2, e);
	e.section_mask = 0;
	const w::EntityHandle shut_door = world.registry.spawn(2, e);
	w::Entity &a = *world.registry.get(open_door);
	world.doors.initialize(a, 529, 0x40000000);
	world.doors.initialize(*world.registry.get(shut_door), 529, 0x40000000);
	world.doors.apply_wire_row(a, 1, 2); // open
	world.doors.apply_wire_row(a, 2, 1); // opening
	world.doors.apply_wire_row(a, 3, 3); // closing
	const StaticEntityBatch batch = ns::build_pool2_static_batch(world);
	CHECK(batch.records.size() == 2);
	if (batch.records.size() != 2) return;
	CHECK(batch.records[0].section_mask == 0x27);
	CHECK(batch.records[1].section_mask == 0 && batch.records[1].has_section_mask);
	StaticEntityBatch decoded;
	const std::vector<uint8_t> wire = encode_static_entity_batch(batch);
	CHECK(decode_static_entity_batch(wire.data(), wire.size(), decoded));
	CHECK(decoded.records.size() == 2);
	if (decoded.records.size() == 2) {
		CHECK(decoded.records[0].section_mask == 0x27);
		CHECK((decoded.records[1].field_flags & 0x0008u) != 0);
		CHECK(decoded.records[1].section_mask == 0);
	}
}

// A joiner's door rows are the 0x10 record's: a section whose bit is set
// starts open, its phase the def's max_angle word; the authority's own spawn
// allocates them closed. [orig: NapiNPClientMsg_0x010 @0x4336B5..0x433745;
//  Entity_SpawnFromBMSRecord @0x40F25D..0x40F2DA]
void test_joiner_doors_open_from_the_record() {
	static const char kItems[] =
			"begin \"Gate\"\r\n"
			"  id 100700\r\n"
			"  type building\r\n"
			"  attrib: Door\r\n"
			"  num_doors 2\r\n"
			"end\r\n";
	def::DefItemsFile items = {};
	CHECK(def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItems),
			sizeof(kItems) - 1, &items) == 0);
	if (items.count != 1) return;
	items.entries[0].door_open_rate_q16 = 529;
	items.entries[0].door_max_angle_bam = 0x40000000;
	const auto wire_class = [](int) -> uint8_t { return 0; };
	for (const bool authority : {false, true}) {
		auto heap = std::make_unique<w::World>();
		w::World &world = *heap;
		world.registry.configure_pool(2, 8);
		world.rules.logic_authority = authority;
		w::Entity e;
		e.kind = w::EntityKind::Building;
		e.item_id = 700;
		e.section_mask = 0x4u; // section 2 open on the wire
		const w::EntityHandle h = world.registry.spawn(2, e);
		mission::resolve_item_traits(world, items, wire_class, h);
		const w::Entity &door = *world.registry.get(h);
		const w::DoorSystem::Slot *first = world.doors.slot(door, 0);
		const w::DoorSystem::Slot *second = world.doors.slot(door, 1);
		CHECK(first != nullptr && second != nullptr);
		if (first == nullptr || second == nullptr) continue;
		CHECK(first->state == 0 && first->phase == 0);
		if (authority) {
			CHECK(second->state == 0 && second->phase == 0);
		} else {
			CHECK(second->state == 2 && second->phase == 0x40000000);
		}
	}
}
} // namespace

int main() {
	test_host_reports_door_commands_and_completions();
	test_single_player_reports_nothing();
	test_host_answers_door_requests();
	test_client_command_raises_requests();
	test_joiner_sends_the_request();
	test_static_batch_carries_door_sections();
	test_joiner_doors_open_from_the_record();
	std::printf("npruntime_door_relay: %d failures\n", failures);
	return failures ? 1 : 0;
}
