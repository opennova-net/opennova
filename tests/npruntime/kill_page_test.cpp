// D-NET-284 — the host's join-window kill page (C2S 0x28 -> S2C 0x4E). Retail
// walks pools 0..2 from the request's start handle and lists every dead,
// non-player row with an item def whose last S2C 0x26 left inside the
// request's window (entity+560, the host's GetTickCount), at most 33 per
// page, the leading word the handle the walk stopped at; it stamps S2C 0x19 /
// 0x1A / 0x0F with the same GetTickCount the window is built from.
// [orig: NapiNPServerMsg_HandleWeaponLoadoutRequest @0x51A550;
//  Server_CollectValidWeaponSlots @0x516000; NetSync_IsEntityEligibleInWindow
//  @0x507AA0; WeaponLoadout_IteratorBegin @0x501680; ItemPoolIterator_Advance
//  @0x501740; Server_SendEntityStatePacket @0x509D70; NetPacket_WriteTimestampB
//  @0x5046F0]
#include <runtime/inmatch/server_kill_page.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/world/destruction.h> // emit_item_state
#include <runtime/world/entity.h>
#include <runtime/world/world.h>
#include <base/io/tick_rate.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

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

w::EntityHandle spawn_row(w::World &world, int pool, uint32_t flags, int32_t health,
		uint32_t stamp, int32_t type_index = 5) {
	w::Entity e;
	e.kind = w::EntityKind::Item;
	e.has_item_def = true;
	e.item_type_index = type_index;
	e.flags = flags;
	e.health = health;
	e.last_state_sent_ms = stamp;
	return world.registry.spawn(pool, e);
}

std::vector<uint16_t> words(const std::vector<uint8_t> &page) {
	std::vector<uint16_t> out;
	for (size_t i = 0; i + 1 < page.size(); i += 2)
		out.push_back(static_cast<uint16_t>(page[i] | (page[i + 1] << 8)));
	return out;
}

std::unique_ptr<w::World> make_world() {
	auto world = std::make_unique<w::World>();
	for (int pool = 0; pool < 3; ++pool) world->registry.configure_pool(pool, 64);
	return world;
}

void test_eligibility_and_walk() {
	auto heap = make_world();
	w::World &world = *heap;
	spawn_row(world, 0, 0x102u, 0, 500);                 // 0x0000 a dead player: never
	spawn_row(world, 1, 0x2u, 100, 500);                  // 0x1000 dead (Flags 2)
	spawn_row(world, 1, 0x0u, 100, 500);                  // 0x1001 alive
	spawn_row(world, 1, 0x0u, 0, 500);                    // 0x1002 health at zero
	spawn_row(world, 1, 0x2u, 100, 50);                   // 0x1003 sent before the window
	spawn_row(world, 2, 0x4u, 100, 500, 0);               // 0x2000 type index 0: never
	spawn_row(world, 2, 0x4u, 100, 1000);                 // 0x2001 dead (Flags 4), at max
	CHECK(words(inmatch::Server_CollectKillPage(world, false, 100, 1000, 0)) ==
			std::vector<uint16_t>({0xFFFF, 0x1000, 0x1002, 0x2001}));
	// A start mid-pool resumes there.
	CHECK(words(inmatch::Server_CollectKillPage(world, false, 100, 1000, 0x1001)) ==
			std::vector<uint16_t>({0xFFFF, 0x1002, 0x2001}));
	// Past pool 2, or while spawns are held: nothing is sent.
	CHECK(inmatch::Server_CollectKillPage(world, false, 100, 1000, 0x3000).empty());
	CHECK(inmatch::Server_CollectKillPage(world, true, 100, 1000, 0).empty());
	// An empty walk still answers with the bare leading word.
	CHECK(words(inmatch::Server_CollectKillPage(world, false, 2000, 3000, 0)) ==
			std::vector<uint16_t>({0xFFFF}));
}

// At most 33 handles per page; the leading word is where the next page starts.
void test_page_cap() {
	auto heap = make_world();
	w::World &world = *heap;
	for (int i = 0; i < 40; ++i) spawn_row(world, 1, 0x2u, 100, 500);
	const std::vector<uint16_t> page =
			words(inmatch::Server_CollectKillPage(world, false, 0, 1000, 0));
	CHECK(page.size() == 34);
	if (page.size() == 34) {
		CHECK(page[0] == 0x1021);
		CHECK(page[1] == 0x1000 && page[33] == 0x1020);
	}
	const std::vector<uint16_t> next =
			words(inmatch::Server_CollectKillPage(world, false, 0, 1000, 0x1021));
	CHECK(next.size() == 8 && next[0] == 0xFFFF && next[1] == 0x1021 && next[7] == 0x1027);
}

// The host stamps entity+560 and its S2C 0x19 in the same GetTickCount, and
// answers C2S 0x28 with the page to an in-match sender only.
void test_host_request() {
	auto heap = make_world();
	w::World &world = *heap;
	world.rules.logic_authority = true;
	world.rules.mp_session = true;
	world.logic_tick = 620;
	const w::EntityHandle wreck = spawn_row(world, 1, 0x2u, 100, 0);
	w::emit_item_state(world, *world.registry.get(wreck), 0);
	const uint32_t stamp = io::host_milliseconds_for_logic_tick(620);
	CHECK(world.registry.get(wreck)->last_state_sent_ms == stamp);
	world.out.entity_events.clear();

	ns::LoopbackChannel wire;
	inmatch::NapiNPServerCtx ctx;
	inmatch::set_connection_mode(ctx, inmatch::ConnectionMode::HostOnly);
	ctx.is_in_session = 1;
	ctx.world = &world;
	ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(1, 1, &wire,
			ns::TransportMode::Client, w::EntityHandle{}, true));
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];
	inmatch::ServerDispatchInputs inputs;
	inputs.server_ctx = &ctx;
	const auto send = [&](uint8_t tag, std::vector<uint8_t> body) {
		return inmatch::dispatch_session_replies(ctx.config, conn,
				{make_protocol_message(tag, std::move(body))}, 7, ctx.np_protocol.connection_list,
				&world, inputs);
	};
	std::vector<ProtocolMessage> replies = send(c2s::SPAWN_MENU_REQUEST, {});
	CHECK(replies.size() == 1 && replies[0].tag == s2c::SPAWN_ACK_TIMESTAMP);
	if (replies.size() == 1)
		CHECK(replies[0].payload == std::vector<uint8_t>({static_cast<uint8_t>(stamp),
				static_cast<uint8_t>(stamp >> 8), static_cast<uint8_t>(stamp >> 16),
				static_cast<uint8_t>(stamp >> 24)}));
	const auto request = [&](uint32_t lo, uint32_t hi, uint16_t start) {
		std::vector<uint8_t> b;
		for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(lo >> (8 * i)));
		for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(hi >> (8 * i)));
		b.push_back(static_cast<uint8_t>(start));
		b.push_back(static_cast<uint8_t>(start >> 8));
		return send(c2s::LOADOUT_REQUEST, b);
	};
	replies = request(stamp, stamp, 0);
	CHECK(replies.size() == 1 && replies[0].tag == s2c::KILL_BY_SLOT);
	if (replies.size() == 1)
		CHECK(words(replies[0].payload) == std::vector<uint16_t>({0xFFFF, wreck.packed}));
	replies = request(stamp + 1, stamp + 100, 0); // sent before the window: a bare page
	CHECK(replies.size() == 1 && words(replies[0].payload) == std::vector<uint16_t>({0xFFFF}));
	conn.burst.spawned = false; // not in the match (mask 0x80)
	CHECK(request(stamp, stamp, 0).empty());
	conn.burst.spawned = true;
	conn.phase = inmatch::ConnectionPhase::Joined; // no player added
	CHECK(request(stamp, stamp, 0).empty());
	conn.phase = inmatch::ConnectionPhase::InMatch;
	ctx.is_authority = 0;
	CHECK(request(stamp, stamp, 0).empty());
}

} // namespace

int main() {
	test_eligibility_and_walk();
	test_page_cap();
	test_host_request();
	std::printf("npruntime_kill_page: %d failures\n", failures);
	return failures ? 1 : 0;
}
