// npruntime_side_password_hold: the host's side-password admission, the spawn pump's balance gate
// (net parity lane L1, D-NET-167's host half; D-NET-1074).
//
// A retail host held a side-password joiner through its own balance-join gate: in a team game a
// side password arms the BuildFlags nibble (0x20 side A, 0x10 side B), the JSP picks the side, and
// with balance_join on a joiner who would put its side more than max(1, ceil(min * percent))
// ahead stays on the pending list, re-sent its S2C 0x03 when more than 1000 ms passed since the
// last one, on the once-a-second pump. Pinned here:
//   - a joiner whose JSP is the host's side's password is held, its 0x03s carrying its place on
//     the pending list and the list's count ([1][1][1] for a lone joiner, as the retail host sent
//     our joiner), at once and then at every second pump (992 ms is not over 1000);
//   - the JSP is compared with both passwords whatever their lengths, so an empty JSP picks the
//     side whose password is unset: on the side the host is not on it is admitted, and with that
//     side ahead it is held (ours read the TR, -1, and admitted it unbalanced);
//   - two held joiners: the second's C2S 0x02 answers [1][2][2], the first's nag [1][1][2];
//   - a failed add's unlinked node gets the one byte [0].
// A JSP matching neither password is rejected with reason 17 at the form post
// (npruntime_join_form_post_gate, D-NET-819).
// [orig: CNapiServer_ProcessPendingPlayerSpawns @0x4C8DC0 - CNapiServerConfig_BuildFlags
//  @0x4C8E82 / & 0xF0 @0x4C8E87, TR @0x4C8E8F, side A's length twice @0x4C8EB0..0x4C8ECD,
//  _stricmp(JSP, A) @0x4C8ED1 / (JSP, B) @0x4C8EEC, the gate @0x4C8F0A, the counts @0x4C8F10 /
//  @0x4C8F17, ceil @0x4C8F41..0x4C8F53, the compare @0x4C8F74, the hold's 0x03 @0x4C8F7A..0x4C8FD1;
//  NetPacket_WriteSpawnQueueRecord @0x502AC0; NapiNode_CountToHead @0x4C5FE0;
//  NapiNPPlayer_MoveNodeToEntityList @0x4C7E60; NapiNPServerMsg_0x002 @0x512FD0 (the listing
//  @0x513193, the 0x03 @0x5131D5); the pump's periodic second Server_TickUpdate @0x51DB6D..0x51DB93
//  (62 ticks), the call @0x51DBFD]

#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_dispatch_join.h>
#include <runtime/inmatch/server_initial_state.h> // build_server_config_flags
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/udp_session_transport.h>

#include "host_test_setup.h"

#include <base/gameprofile/game_type.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../common/test_expect.h"
#include "../common/title_fixture.h"

namespace {

namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

using test_expect::expect;

using Bytes = std::vector<uint8_t>;

// A LAN session host of a team game (TDM) whose side A carries the password "blue1" (the
// jox01 SERVER game.cfg's case), balance_join 1 at 0.5, eight players. Its BuildFlags word
// carries 0x20 and the team-choose bits: 0x924, the word the server_hello's P2 advertises.
struct HoldFixture {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	bool world_jo = test_titles::jo_world(*world);
	std::unique_ptr<inmatch::NapiNPServerCtx> ctx = std::make_unique<inmatch::NapiNPServerCtx>();
	bool ctx_jo = test_titles::jo_config(ctx->config);
	opennova::bms::File mission;
	std::vector<std::unique_ptr<ns::UdpSessionTransport>> transports;
	uint32_t tick = 1;

	HoldFixture() {
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
		test_titles::jo_config(config);
		config.max_players = 8;
		config.game_type = opennova::game_type::kTeamDeathmatch;
		config.mp_attributes |= inmatch::GameConfig::kMpAttribTeamChoose;
		config.side_a_password = "blue1";
		config.balance_join = true;
		config.balance_join_percent = 0.5f;
		inmatch::test::bring_up_host(*ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
				/*host_key=*/0, nullptr, config);
		ctx->world = world.get();
		ctx->mission = &mission;
		inmatch::Server_InitNewRoundState(*ctx);
		ctx->np_protocol.connection_list.reserve(16); // the references below stay put
	}

	inmatch::NapiNPConnection &add(uint16_t port) {
		transports.push_back(
				std::make_unique<ns::UdpSessionTransport>(ns::UdpSessionTransport::Role::Host));
		inmatch::NapiNPConnection &c = ctx->np_protocol.connection_list.emplace_back();
		c.type = inmatch::NapiNPConnection::kTypeServerSide;
		c.peer = opennova::PeerAddr{0x0100007Fu, port};
		c.connection_id = inmatch::kFirstJoinerDcb + port;
		c.player_name = "P" + std::to_string(port);
		c.link.transport = transports.back().get();
		c.link.mode = ns::TransportMode::Client;
		return c;
	}

	// An in-match player whose slot team is `team` (the row the balance count reads).
	inmatch::NapiNPConnection &seat(uint16_t port, uint8_t team) {
		inmatch::NapiNPConnection &c = add(port);
		c.phase = inmatch::ConnectionPhase::InMatch;
		c.admission_stage = inmatch::GameAdmissionStage::Complete;
		c.slot_state = inmatch::PlayerSlotState::Joined;
		c.burst.spawned = true;
		c.burst.game_state = 6;
		c.reply.player_slot = static_cast<uint8_t>(port % 8);
		c.assigned_team_valid = true;
		c.assigned_team = team;
		return c;
	}

	// A joiner past its form post (game state 6, its probe out) whose 0x42 carried JSP
	// `password` and TR `tr`; not yet on the pending list.
	inmatch::NapiNPConnection &joiner(uint16_t port, const char *password, uint8_t tr) {
		inmatch::NapiNPConnection &c = add(port);
		c.phase = inmatch::ConnectionPhase::Joined;
		c.admission_stage = inmatch::GameAdmissionStage::Complete;
		c.burst.game_state = 6;
		c.join_password = password;
		c.char_vars.team_request = tr;
		return c;
	}

	// Its C2S 0x02: the replies' 0x03 body.
	Bytes echo(inmatch::NapiNPConnection &c) {
		inmatch::ServerDispatchInputs inputs;
		inputs.server_ctx = ctx.get();
		std::vector<opennova::ProtocolMessage> replies;
		inmatch::dispatch_join_padding_echo(ctx->config, c, tick, ctx->np_protocol.connection_list,
				inputs, replies);
		Bytes body;
		for (const opennova::ProtocolMessage &m : replies)
			if (m.tag == opennova::s2c::SYNC_TICK) body = m.payload;
		return body;
	}

	// The pump at host clock `ms`, then one producer pass; returns how many it added.
	int pump_at(uint32_t ms) {
		ctx->np_protocol.host_run_duration_ms = ms;
		const int added = inmatch::Server_ProcessPendingPlayerSpawns(*ctx, *world);
		(void)inmatch::tick_connections(*ctx, /*elapsed_ms=*/16, tick++);
		return added;
	}

	// The S2C 0x03 bodies the connection on `port` took since the last call.
	std::vector<Bytes> sync_ticks(uint16_t port) {
		std::vector<Bytes> out;
		for (std::size_t i = 0; i < ctx->np_protocol.connection_list.size(); ++i) {
			const inmatch::NapiNPConnection &c = ctx->np_protocol.connection_list[i];
			if (c.peer.port != port) continue;
			auto *t = static_cast<ns::UdpSessionTransport *>(c.link.transport);
			ns::Datagram d;
			while (t->pop_outbound(d))
				if (d.tag == opennova::s2c::SYNC_TICK) out.push_back(d.body);
		}
		return out;
	}
};

const Bytes kLone = {0x01, 0x01, 0x00, 0x01, 0x00};

// The host's side A is ahead by one (an in-match player there, none on B): a joiner entering
// side A's password picks team 0 [orig: @0x4C8ED1 -> 0 @0x4C8EDD]; 2 against 0 is past
// max(1, ceil(0 * 0.5)) = 1 [orig: @0x4C8F74], so the pump holds it on the pending list. Its
// 0x02 answered [1][1][1] (listed ahead of the 0x03 @0x513193); the first hold nags at once (the
// stamp the entry into game state 7 zeroed), then each nag waits until more than 1000 ms passed
// (`jbe` @0x4C8F8D): on the periodic second (62 ticks of 16 ms, 992 ms apart) every second pump.
bool run_a_joiner_on_the_hosts_side_is_held_and_nagged() {
	HoldFixture f;
	if (!expect(inmatch::build_server_config_flags(*f.ctx) == 0x924u,
	            "a side-A password in a LAN team game: BuildFlags 0x924 (the P2 retail advertised)"))
		return false;
	(void)f.seat(30001, /*team=*/1); // side A
	inmatch::NapiNPConnection &j = f.joiner(30002, "blue1", 0xFF);
	if (!expect(f.echo(j) == kLone && j.self_id_seen && !j.spawn_queue_unlinked &&
	                    j.burst.game_state == 7,
	            "its 0x02 lists it and answers [1][1][1]")) return false;
	if (!expect(f.pump_at(5000) == 0 && j.phase < inmatch::ConnectionPhase::PlayerAdded,
	            "the balance gate holds it (2 vs 0 > 1)")) return false;
	std::vector<Bytes> nags = f.sync_ticks(30002);
	if (!expect(nags.size() == 1 && nags[0] == kLone,
	            "the first hold nags at once with its place and the list's count")) return false;
	if (!expect(f.pump_at(5992) == 0 && f.sync_ticks(30002).empty(),
	            "the next periodic second (992 ms) sends nothing")) return false;
	if (!expect(f.pump_at(6984) == 0, "still held")) return false;
	nags = f.sync_ticks(30002);
	if (!expect(nags.size() == 1 && nags[0] == kLone, "the second one past 1000 ms nags again"))
		return false;
	if (!expect(f.pump_at(7984) == 0 && f.sync_ticks(30002).empty(),
	            "exactly 1000 ms after the last nag is no nag")) return false;
	return expect(f.pump_at(7985) == 0 && f.sync_ticks(30002).size() == 1,
	              "1001 ms after it is");
}

// An empty JSP: compared with side A's password ("blue1": no), then side B's (unset: equal),
// so it picks team 1 whatever its TR [orig: @0x4C8EEC -> 1 @0x4C8EF8]. With the host's player on
// side A it is admitted (1 vs 1); with a player on side B it is held (0 vs 2), where a TR of -1
// alone would admit it unbalanced [orig: the gate's -1 test @0x4C8F07..0x4C8F0A].
bool run_an_empty_jsp_picks_the_side_without_a_password() {
	{
		HoldFixture f;
		(void)f.seat(30001, /*team=*/1); // side A
		inmatch::NapiNPConnection &j = f.joiner(30002, "", 0xFF);
		(void)f.echo(j);
		if (!expect(f.pump_at(5000) == 1 && j.phase >= inmatch::ConnectionPhase::PlayerAdded,
		            "on the side the host's player is not on, it is admitted (1 vs 1)"))
			return false;
		const std::vector<Bytes> sent = f.sync_ticks(30002);
		if (!expect(!sent.empty() && sent[0] == Bytes{0x00},
		            "the add's 0x03 is the unlinked node's [0]")) return false;
	}
	HoldFixture f;
	(void)f.seat(30001, /*team=*/2); // side B
	inmatch::NapiNPConnection &j = f.joiner(30002, "", 0xFF);
	(void)f.echo(j);
	if (!expect(f.pump_at(5000) == 0 && j.phase < inmatch::ConnectionPhase::PlayerAdded,
	            "with side B ahead the empty JSP is held on side B (2 vs 0)")) return false;
	const std::vector<Bytes> nags = f.sync_ticks(30002);
	return expect(nags.size() == 1 && nags[0] == kLone, "and nagged");
}

// Two joiners held on side A: the pending list is in listing order. The second's 0x02 lists it
// at the tail and answers [1][2][2]; the pump's nags carry [1][1][2] and [1][2][2]
// [orig: NapiNode_CountToHead @0x4C5FE0 (1 plus the nodes ahead), the count word
//  @0x502B13; NapiNPPlayer_MoveNodeToEntityList's tail append @0x4C7EB6..0x4C7EE2].
bool run_held_joiners_carry_their_places() {
	HoldFixture f;
	(void)f.seat(30001, /*team=*/1);
	inmatch::NapiNPConnection &first = f.joiner(30002, "blue1", 0xFF);
	inmatch::NapiNPConnection &second = f.joiner(30003, "BLUE1", 0xFF); // case-insensitive
	if (!expect(f.echo(first) == kLone, "the first 0x02: [1][1][1]")) return false;
	if (!expect(f.echo(second) == Bytes({0x01, 0x02, 0x00, 0x02, 0x00}),
	            "the second 0x02: place 2 of 2")) return false;
	if (!expect(f.echo(first) == Bytes({0x01, 0x01, 0x00, 0x02, 0x00}),
	            "a repeated 0x02 of a listed node keeps its place")) return false;
	if (!expect(f.pump_at(5000) == 0, "both held")) return false;
	const std::vector<Bytes> a = f.sync_ticks(30002);
	const std::vector<Bytes> b = f.sync_ticks(30003);
	return expect(a.size() == 1 && a[0] == Bytes({0x01, 0x01, 0x00, 0x02, 0x00}) &&
	                      b.size() == 1 && b[0] == Bytes({0x01, 0x02, 0x00, 0x02, 0x00}),
	              "the nags carry places 1 and 2 of 2");
}

} // namespace

int main() {
	int failures = 0;
	if (!run_a_joiner_on_the_hosts_side_is_held_and_nagged()) ++failures;
	if (!run_an_empty_jsp_picks_the_side_without_a_password()) ++failures;
	if (!run_held_joiners_carry_their_places()) ++failures;
	if (failures != 0) return 1;
	std::printf("OK\n");
	return 0;
}
