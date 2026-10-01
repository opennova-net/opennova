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
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/world/world.h>
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

} // namespace

int main() {
	test_host_reports_door_commands_and_completions();
	test_single_player_reports_nothing();
	std::printf("npruntime_door_relay: %d failures\n", failures);
	return failures ? 1 : 0;
}
