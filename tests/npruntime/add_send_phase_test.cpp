// npruntime_add_send_phase: a remote add re-phases its connection's S2C send boundary
// (net parity lane L3, D-NET-NEW-L3-21).
//
// Retail's spawn pump adds a joiner through Server_PlayerAdd, which binds the slot's net player
// and runs its send-state reset: NapiNPServer_UpdateHoldoffTicks dictates the period again and
// zeroes the connection's countdown, so the slot's send boundary opens on the add's own server
// tick and its phase counts from the add, whatever the C2S 0x02's phase was. The host's own arm
// jumps past it, and the round init's re-add runs no Server_PlayerAdd.
// [orig: Server_PlayerAdd @0x51CBC0 - slot+28 @0x51CD4E, CNetPlayer_ResetSendState @0x51CD6F;
//  CNetPlayer_ResetSendState @0x500B30 - NapiNPServer_UpdateHoldoffTicks @0x500B59;
//  NapiNPServer_UpdateHoldoffTicks @0x4C5F40 - the resets @0x4C5F8D / @0x4C5FD4;
//  Server_TickUpdate - the spawn pump @0x51DBFD ahead of the boundary test @0x51E3D6 and the
//  send pump @0x51E487]
//
// Pinned, core-only: a joiner whose 0x02 dictation ran five ticks before its add (a countdown of
// seven of twelve, the boundary closed) leaves the add with its countdown zero, its boundary open
// and the dictated period; a kept slot's round-init re-add leaves the countdown as it was.

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/udp_session_transport.h>

#include "host_test_setup.h"

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>

namespace {

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

int failures = 0;
#define CHECK(c)                                                                     \
	do {                                                                             \
		if (!(c)) {                                                                  \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
			++failures;                                                              \
		}                                                                            \
	} while (0)

// One remote joiner on a Serve Only session host, admitted and waiting for the spawn pump, its
// send boundary dictated at the 0x02 and `ticks_since` receive pumps past it. Heap-held: a World
// and a server context overflow the 1 MB test-main stack under MSVC.
struct AddFixture {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	opennova::bms::File mission;
	ns::UdpSessionTransport joiner_transport{ns::UdpSessionTransport::Role::Host};

	explicit AddFixture(uint32_t ticks_since) {
		world->rules.mp_session = true;
		world->registry.configure_pool(0, 16);
		world->registry.configure_pool(1, 8);
		world->registry.configure_pool(2, 8);
		world->registry.configure_pool(3, 16);
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6002;
		start.position = {10.0f, 20.0f, 1.0f};
		start.item_type_index = 1;
		start.has_item_def = true;
		world->registry.spawn(3, start);
		opennova::mission::make_default(mission);

		inmatch::GameConfig config;
		config.max_players = 4;
		inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly,
				inmatch::SocketMode::Socketless, /*host_key=*/0, nullptr, config);
		ctx->world = world.get();
		ctx->mission = &mission;
		inmatch::Server_InitNewRoundState(*ctx);

		inmatch::NapiNPConnection &joiner = ctx->np_protocol.connection_list.emplace_back();
		joiner.type = inmatch::NapiNPConnection::kTypeServerSide;
		joiner.peer = opennova::PeerAddr{0x0100007Fu, 30002};
		joiner.connection_id = inmatch::kFirstJoinerDcb;
		joiner.player_name = "Joiner";
		joiner.phase = inmatch::ConnectionPhase::Joined;
		joiner.admission_stage = inmatch::GameAdmissionStage::Complete;
		joiner.self_id_seen = true;
		joiner.link.transport = &joiner_transport;
		joiner.link.mode = ns::TransportMode::Client;
		// The 0x02's dictation and its first open boundary's reload, then `ticks_since` receive
		// pumps' decrements, the boundary closed between them.
		joiner.s2c_send_holdoff_ticks = 12;
		joiner.s2c_send_holdoff_dictated = true;
		joiner.s2c_send_holdoff_countdown = 12 - ticks_since;
		joiner.s2c_send_boundary_open = false;
	}

	inmatch::NapiNPConnection &joiner() { return ctx->np_protocol.connection_list.back(); }
};

// The spawn pump's add zeroes the countdown: the boundary opens on the add's tick.
void test_the_add_opens_the_boundary_on_its_tick() {
	auto f = std::make_unique<AddFixture>(/*ticks_since=*/5);
	CHECK(f->joiner().s2c_send_holdoff_countdown == 7);
	CHECK(inmatch::Server_ProcessPendingPlayerSpawns(*f->ctx, *f->world) == 1);
	const inmatch::NapiNPConnection &j = f->joiner();
	CHECK(j.phase >= inmatch::ConnectionPhase::PlayerAdded);
	CHECK(j.s2c_send_holdoff_countdown == 0);
	CHECK(j.s2c_send_boundary_open);
	CHECK(j.s2c_send_holdoff_dictated);
	// The period UpdateHoldoffTicks dictates again, the session's.
	CHECK(j.s2c_send_holdoff_ticks ==
			inmatch::clamp_send_holdoff_ticks(f->ctx->config.effective_send_holdoff_ticks()));
}

// A kept slot's round-init re-add runs no Server_PlayerAdd: its phase stays.
void test_the_round_init_re_add_keeps_the_phase() {
	auto f = std::make_unique<AddFixture>(/*ticks_since=*/5);
	CHECK(inmatch::Server_ProcessPendingPlayerSpawns(*f->ctx, *f->world) == 1);
	inmatch::NapiNPConnection &j = f->joiner();
	j.s2c_send_holdoff_countdown = 3;
	j.s2c_send_boundary_open = false;
	(void)inmatch::Server_BuildPlayerInfoAndAdd(*f->ctx, j, *f->world, /*round_init=*/true);
	CHECK(j.s2c_send_holdoff_countdown == 3);
	CHECK(!j.s2c_send_boundary_open);
}

} // namespace

int main() {
	test_the_add_opens_the_boundary_on_its_tick();
	test_the_round_init_re_add_keeps_the_phase();
	if (failures != 0) {
		std::printf("add_send_phase: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("add_send_phase: ok\n");
	return 0;
}
