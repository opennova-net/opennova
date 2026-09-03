// npruntime host bring-up — the §5.0 connection-mode -> {is_authority, is_mp_session_peer}
// table and the socketless transport mode. The IDA-faithful in-process listen-server bring-up
// state writes [orig: SinglePlayer_StartMission @0x561af0].

#include <runtime/session/server_session.h>
#include <runtime/session/host_session.h>
#include <runtime/session/joiner_connection.h>
#include <runtime/session/server_message_dispatch.h>
#include <runtime/session/server_spawn.h>
#include <runtime/session/session_status.h>
#include <runtime/session/server_tick.h>

#include <runtime/replication/connection_fan.h>
#include <runtime/session/loopback_channel.h>
#include <net/npwire/idatagram_socket.h>
#include <runtime/session/udp_session_transport.h>

#include <net/npwire/nw_session_framing.h>
#include <net/npwire/entity_class.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>

#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <base/gameprofile/game_type.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <algorithm>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace {

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

bool check_scoreboard_message_is_transient() {
	opennova::np::GameConfig config;
	const opennova::ProtocolMessage message =
			opennova::np::build_player_list_message(config, {}, nullptr);
	return expect(message.tag == opennova::s2c::PLAYER_LIST && !message.reliable,
	              "scoreboard 0x16 uses retail's one-send transient delivery");
}

bool check_scoreboard_projects_every_retail_mode_shape() {
	opennova::world::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 16);

	auto spawn_player = [&](uint8_t team, bool alive) {
		opennova::world::Entity entity;
		entity.kind = opennova::world::EntityKind::Organic;
		entity.team = team;
		entity.alive = alive;
		if (!alive) entity.flags |= opennova::world::kEntityFlagDead;
		return world.registry.spawn(0, entity);
	};
	const opennova::world::EntityHandle blue = spawn_player(1, true);
	const opennova::world::EntityHandle red = spawn_player(2, false);

	std::vector<opennova::np::NapiNPConnection> roster(2);
	for (uint8_t slot = 0; slot < roster.size(); ++slot) {
		roster[slot].burst.spawned = true;
		roster[slot].link.owned_entity = slot == 0 ? blue : red;
		roster[slot].reply.player_slot = slot;
	}

	auto objective = [&](int32_t item_id, uint8_t team, uint32_t attrib = 0) {
		opennova::world::Entity entity;
		entity.kind = opennova::world::EntityKind::Item;
		entity.item_id = item_id;
		entity.team = team;
		entity.item_attrib = attrib;
		entity.alive = true;
		return world.registry.spawn(1, entity);
	};
	objective(4091, 1); // one authored blue flag
	objective(4093, 2);
	objective(4093, 2); // two authored red flags
	objective(7001, 1, opennova::world::kItemAttribObjectiveTarget);
	objective(7002, 1, opennova::world::kItemAttribObjectiveTarget);
	objective(7003, 2, opennova::world::kItemAttribObjectiveTarget);

	opennova::np::GameConfig config;
	config.num_teams = 4;
	auto board = [&](uint32_t game_type) {
		config.game_type = game_type;
		opennova::world::MatchRules rules;
		rules.game_type = game_type;
		rules.team_count = config.num_teams;
		world.match.configure(rules);
		world.match.upsert_player({blue, 0, "Blue", {}});
		world.match.upsert_player({red, 1, "Red", {}});
		const opennova::ProtocolMessage message =
				opennova::np::build_player_list_message(config, roster, &world);
		opennova::PlayerList decoded;
		if (!opennova::decode_player_list(
					message.payload.data(), message.payload.size(), decoded))
			decoded = {};
		return decoded;
	};

	for (const uint32_t solo : {
			opennova::game_type::kDeathmatch,
			opennova::game_type::kFlagMe}) {
		const opennova::PlayerList decoded = board(solo);
		if (!expect(decoded.flags == 0 && decoded.team_count == 0 &&
		                    decoded.teams.size() == 1,
		            "DM/Flag Me scoreboard has the one neutral row and no team bit"))
			return false;
	}
	{
		const opennova::PlayerList decoded =
				board(opennova::game_type::kKingOfTheHill);
		if (!expect(decoded.flags == 2 && decoded.team_count == 0 &&
		                    decoded.teams.size() == 1,
		            "solo KOTH alone sets the timed-score bit"))
			return false;
	}
	for (const uint32_t four_team : {
			opennova::game_type::kTeamDeathmatch,
			opennova::game_type::kFlagBall}) {
		const opennova::PlayerList decoded = board(four_team);
		if (!expect(decoded.flags == 1 && decoded.team_count == 4 &&
		                    decoded.teams.size() == 5,
		            "TDM/FlagBall honor the configured four-team scoreboard"))
			return false;
	}
	{
		const opennova::PlayerList decoded =
				board(opennova::game_type::kAdvanceAndSecure);
		if (!expect(decoded.flags == 1 && decoded.team_count == 2 &&
		                    decoded.teams.size() == 3,
		            "A&S remains two-team even when mp_numteams is four"))
			return false;
	}
	{
		const opennova::PlayerList decoded =
				board(opennova::game_type::kTeamKingOfTheHill);
		if (!expect(decoded.team_count == 4 && decoded.teams[1].koth_hold == 1 &&
		                    decoded.teams[2].koth_hold == 0,
		            "team KOTH rows carry each team's live-player count"))
			return false;
	}
	{
		const opennova::PlayerList decoded =
				board(opennova::game_type::kCaptureTheFlag);
		if (!expect(decoded.team_count == 2 && decoded.teams[1].ctf_flag == 1 &&
		                    decoded.teams[2].ctf_flag == 2,
		            "CTF rows carry each side's authored own-flag count"))
			return false;
	}
	for (const uint32_t demolition : {
			opennova::game_type::kSearchAndDestroy,
			opennova::game_type::kAttackDefend}) {
		const opennova::PlayerList decoded = board(demolition);
		if (!expect(decoded.team_count == 2 && decoded.teams[1].ctf_flag == 2 &&
		                    decoded.teams[2].ctf_flag == 1,
		            "S&D/A&D rows carry each defending side's authored target count"))
			return false;
	}
	return true;
}

void add_retail_game_environment(opennova::ClientAuth &auth) {
	for (const auto &field : {
			std::pair{"BT", "0"},
			std::pair{"VN", "2"},
			std::pair{"BN", "1"},
			std::pair{"DB", "0"},
			std::pair{"MBN", "20042002"},
			std::pair{"SOPD", "180"},
	}) {
		auth.cu.push_back(opennova::make_client_cu_chunk(
				2, field.first, field.second));
	}
}

using opennova::np::ConnectionMode;
using opennova::np::NapiNPServerCtx;
using opennova::np::SocketMode;

struct CaptureDatagramSocket final : opennova::netsim::IDatagramSocket {
	struct Incoming {
		opennova::PeerAddr peer;
		std::vector<uint8_t> bytes;
	};
	std::deque<Incoming> incoming;
	std::vector<std::vector<uint8_t>> sent;
	std::vector<opennova::PeerAddr> sent_to;

	int recv_from(uint8_t *data, std::size_t capacity, opennova::PeerAddr &from) override {
		if (incoming.empty()) return 0;
		Incoming item = std::move(incoming.front());
		incoming.pop_front();
		const std::size_t count = std::min(capacity, item.bytes.size());
		std::copy_n(item.bytes.begin(), count, data);
		from = item.peer;
		return static_cast<int>(count);
	}
	void send_to(const opennova::PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent_to.push_back(to);
		sent.emplace_back(data, data + len);
	}
};

// [orig: CGameSession_SetConnectionMode @0x4c49f0] decomposes the mode into the is_host /
// is_client booleans exactly per the §5.0 table.
bool check_connection_mode_table() {
	struct Row {
		ConnectionMode mode;
		uint32_t is_host;
		uint32_t is_client;
	};
	const Row rows[] = {
	    {ConnectionMode::None, 0, 0},
	    {ConnectionMode::HostOnly, 1, 0},
	    {ConnectionMode::ClientOnly, 0, 1},
	    {ConnectionMode::HostClient, 1, 1}, // single-player / co-op listen server
	};
	for (const Row &r : rows) {
		NapiNPServerCtx ctx;
		opennova::np::set_connection_mode(ctx, r.mode);
		if (!expect(ctx.connection_mode == r.mode, "connection_mode stored")) return false;
		if (!expect(ctx.is_authority == r.is_host, "is_authority = is_host bit")) return false;
		if (!expect(ctx.is_mp_session_peer == r.is_client, "is_mp_session_peer = is_client bit"))
			return false;
	}
	return true;
}

// SP host = mode 3 + socketless transport (§5.0 steps 1-2): is_authority && is_mp_session_peer,
// socket_state == Socketless (no UDP socket opened).
bool check_single_player_signature() {
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(ctx, SocketMode::Socketless);
	if (!expect(ctx.is_authority == 1 && ctx.is_mp_session_peer == 1, "SP is host + client"))
		return false;
	if (!expect(ctx.socket_state == SocketMode::Socketless, "SP is socketless")) return false;
	// Defaults that must hold before StartServer (P1).
	if (!expect(ctx.np_protocol.host_running == 0, "host not running before StartServer"))
		return false;
	if (!expect(ctx.is_in_session == 0, "no session before CreateSession")) return false;
	if (!expect(ctx.np_protocol.connection_list.empty(), "no connections before bring-up"))
		return false;
	return true;
}

// Retail's defaults are selected by session family and LAN mode, while an
// explicit per-session override still wins. [orig:
// NapiNPServer_GetSendHoldoffTicks @0x4c4ab0; round-start budget @0x51ca7c]
bool check_retail_rate_defaults() {
	opennova::np::GameConfig config;
	if (!expect(config.entity_send_budget == 600,
	            "retail 0x0A soft budget defaults to 600 bytes")) return false;
	if (!expect(config.effective_send_holdoff_ticks() == 1,
	            "automatic/socketless cadence defaults to one tick")) return false;

	config.session_channel = opennova::np::GameSessionChannel::NovaWorld;
	if (!expect(config.effective_send_holdoff_ticks() == 12,
	            "NovaWorld cadence defaults to twelve ticks")) return false;

	config.session_channel = opennova::np::GameSessionChannel::Lan;
	const uint32_t expected_by_lan_mode[] = {12, 6, 4, 3};
	for (uint32_t mode = 1; mode <= 4; ++mode) {
		config.lan_mode = mode;
		if (!expect(config.effective_send_holdoff_ticks() ==
		                    expected_by_lan_mode[mode - 1],
		            "LAN mode selects its witnessed retail cadence")) return false;
	}
	config.lan_mode = 99;
	if (!expect(config.effective_send_holdoff_ticks() == 6,
	            "invalid LAN mode uses retail mode-two fallback")) return false;

	config.send_holdoff_ticks = 7;
	if (!expect(config.effective_send_holdoff_ticks() == 7,
	            "explicit cadence override wins over session defaults")) return false;
	config.send_holdoff_ticks.reset();

	NapiNPServerCtx lan_ctx;
	opennova::np::set_connection_mode(lan_ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(lan_ctx, SocketMode::Lan);
	config.session_channel = opennova::np::GameSessionChannel::Automatic;
	config.lan_mode = 1;
	opennova::np::create_session(
			lan_ctx, config, opennova::np::SessionStartup{}, nullptr);
	if (!expect(lan_ctx.config.send_holdoff_ticks.has_value() &&
	                    *lan_ctx.config.send_holdoff_ticks == 12,
	            "session creation resolves automatic socketed cadence once")) return false;

	NapiNPServerCtx sp_ctx;
	opennova::np::set_connection_mode(sp_ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(sp_ctx, SocketMode::Socketless);
	opennova::np::create_session(
			sp_ctx, opennova::np::GameConfig{},
			opennova::np::SessionStartup{}, nullptr);
	if (!expect(sp_ctx.config.send_holdoff_ticks.has_value() &&
	                    *sp_ctx.config.send_holdoff_ticks == 1,
	            "session creation resolves automatic socketless cadence to one tick"))
		return false;

	// start_host_session must actively restore the default after a prior
	// BANDWIDTH override because the selector is a retail-style process global.
	opennova::netsim::set_entity_send_budget(1600);
	opennova::np::HostOwner default_owner;
	opennova::np::HostConfig default_host;
	default_host.socket_mode = SocketMode::Socketless;
	opennova::np::start_host_session(default_owner, default_host);
	if (!expect(opennova::netsim::entity_send_budget() == 600,
	            "new host session restores the 600-byte round default")) return false;

	opennova::np::HostOwner override_owner;
	opennova::np::HostConfig override_host;
	override_host.socket_mode = SocketMode::Socketless;
	override_host.config.entity_send_budget = 777;
	opennova::np::start_host_session(override_owner, override_host);
	const bool override_kept = expect(
			opennova::netsim::entity_send_budget() == 777,
			"explicit entity budget override remains effective");
	opennova::netsim::set_entity_send_budget(600);
	return override_kept;
}

// The remote send period exists in GameConfig before a peer connects, but
// retail does not arm connection+0x648 during the pre-session hello/auth legs.
// The first retained 0x83 settings packet therefore shares the ClientAuth pump
// with the immediate 0x82 even when the eventual LAN period is greater than 1.
// Only the later C2S 0x02 admission/dictation boundary arms that period.
bool check_pre_dictation_holdoff_keeps_initial_settings_open() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	config.send_holdoff_ticks = 4;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x0FE0E112u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);

	const opennova::PeerAddr peer{0x0100007Fu, 33111};
	opennova::np::JoinerConnection joiner("HoldoffHandshake");
	CaptureDatagramSocket socket;
	socket.incoming.push_back({peer, joiner.start()});
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1,
	            "pre-dictation fixture receives one immediate 0x81"))
		return false;

	auto opcode_is = [](const std::vector<uint8_t> &datagram, uint8_t expected) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		return opennova::nw_decode_inbound(
				datagram.data(), datagram.size(), opcode, body) &&
		       opcode == expected;
	};
	if (!expect(opcode_is(socket.sent[0], opennova::SESSION_OPCODE_SERVER_HELLO),
	            "pre-dictation fixture starts with ServerHello"))
		return false;
	const opennova::np::JoinerConnection::PollResult hello_result =
			joiner.handle_datagram(socket.sent[0].data(), socket.sent[0].size());
	if (!expect(hello_result.outbound.size() == 1 &&
	                    opcode_is(hello_result.outbound[0],
	                              opennova::SESSION_OPCODE_CLIENT_AUTH),
	            "ServerHello produces one ClientAuth"))
		return false;

	socket.sent.clear();
	socket.sent_to.clear();
	socket.incoming.push_back({peer, hello_result.outbound[0]});
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 2 &&
	                    opcode_is(socket.sent[0],
	                              opennova::SESSION_OPCODE_SERVER_AUTH) &&
	                    opcode_is(socket.sent[1],
	                              opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE),
	            "period-four pre-session pump sends immediate 0x82 then retained 0x83 settings"))
		return false;
	if (!expect(!owner.ctx.np_protocol.connection_list.front().
	                            s2c_send_holdoff_dictated &&
	                    owner.ctx.np_protocol.connection_list.front().
	                                    s2c_send_holdoff_countdown == 0 &&
	                    owner.ctx.np_protocol.connection_list.front().
	                            s2c_send_boundary_open,
	            "ClientAuth stores period four without arming it before dictation"))
		return false;

	// Drive the captured 0x00 -> 0x01 -> 0x02 admission turns. Each client
	// response is generated only after consuming the prior host boundary.
	for (int turn = 0; turn < 8 &&
	                   owner.ctx.np_protocol.connection_list.front().admission_stage !=
	                           opennova::np::GameAdmissionStage::Complete;
	     ++turn) {
		std::vector<std::vector<uint8_t>> client_outbound;
		for (const std::vector<uint8_t> &datagram : socket.sent) {
			const opennova::np::JoinerConnection::PollResult result =
					joiner.handle_datagram(datagram.data(), datagram.size());
			client_outbound.insert(client_outbound.end(), result.outbound.begin(),
			                       result.outbound.end());
		}
		socket.sent.clear();
		socket.sent_to.clear();
		for (std::vector<uint8_t> &datagram : client_outbound)
			socket.incoming.push_back({peer, std::move(datagram)});
		opennova::np::host_session_pump(owner, socket);
	}
	const opennova::np::NapiNPConnection &conn =
			owner.ctx.np_protocol.connection_list.front();
	return expect(conn.admission_stage ==
	                      opennova::np::GameAdmissionStage::Complete &&
	                      conn.s2c_send_holdoff_dictated &&
	                      conn.s2c_send_holdoff_ticks == 4 &&
	                      conn.s2c_send_holdoff_countdown == 4 &&
	                      !conn.s2c_send_boundary_open,
	              "admission dictation sends on the open edge then arms period four");
}

// The full SP bring-up [orig: SinglePlayer_StartMission @0x561af0]:
// SetConnectionMode(3) -> SetTransportMode(1) -> CreateSession -> StartServer, registering the
// host's own loopback client connection. After it: host_running == 1, in session, one connection.
bool check_create_session_brings_up_host() {
	opennova::netsim::LoopbackChannel local_client;
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(ctx, SocketMode::Socketless);

	opennova::np::GameConfig settings;
	settings.server_name = "SINGLEPLAYERGAME"; // §5.0 default
	settings.max_players = 1;

	opennova::np::SessionStartup startup;
	startup.host_key = 0xABCD1234;     // seed-injected (NapiNP_GenerateSessionKey result)
	startup.host_start_tick = 100000;  // seed-injected (GetTickCount)
	startup.session_seed_id = 654321;  // seed-injected

	// A stale board stream from a previous round is cleared with the other
	// round-end fields [orig: Server_ProcessRoundEnd's stru_C947D8 producer is
	// the only writer; the session clear resets it].
	ctx.round_end_board_stream = {1u, 2u, 3u};
	opennova::np::create_session(ctx, settings, startup, &local_client);

	if (!expect(ctx.is_in_session == 1, "in session after CreateSession")) return false;
	if (!expect(ctx.round_end_board_stream.empty(),
	            "create_session clears the frozen round-end board stream"))
		return false;
	if (!expect(ctx.np_protocol.host_running == 1, "host_running == 1 after StartServer"))
		return false;
	if (!expect(ctx.np_protocol.host_key == 0xABCD1234, "host_key stamped")) return false;
	if (!expect(ctx.np_protocol.host_start_tick == 100000, "host_start_tick stamped"))
		return false;
	if (!expect(ctx.np_protocol.session_seed_id == 654321, "session_seed_id stamped"))
		return false;
	if (!expect(ctx.np_protocol.session_name == "SINGLEPLAYERGAME", "session_name = server_name"))
		return false;
	if (!expect(ctx.np_protocol.max_players == 1, "MP TLV mirrors settings")) return false;
	if (!expect(ctx.np_protocol.connection_list.size() == 1, "one (loopback) connection"))
		return false;
	const opennova::np::NapiNPConnection &c = ctx.np_protocol.connection_list[0];
	if (!expect(c.type == 2, "host's own client is a type-2 connection")) return false;
	if (!expect(c.link.mode == opennova::netsim::TransportMode::Loopback, "mode 1 loopback"))
		return false;
	if (!expect(c.link.transport == &local_client, "transport bound (non-owning)")) return false;
	return true;
}

// Connection residency changes only at the true session boundary. Reusing a
// context must discard every role from the prior session, install exactly the
// new role set, and restart remote connection ids from the retail joiner base.
bool check_create_session_replaces_connection_role_set() {
	opennova::netsim::LoopbackChannel first_local;
	opennova::netsim::LoopbackChannel replacement_local;
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(ctx, SocketMode::Socketless);

	opennova::np::GameConfig settings;
	settings.max_players = 8;
	opennova::np::SessionStartup startup;
	opennova::np::create_session(ctx, settings, startup, &first_local);

	opennova::np::NapiNPConnection remote;
	remote.type = opennova::np::NapiNPConnection::kTypeServerSide;
	remote.connection_id = opennova::np::kFirstJoinerDcb + 9;
	ctx.np_protocol.connection_list.push_back(remote);
	ctx.np_protocol.next_connection_id = 77;

	opennova::np::create_session(ctx, settings, startup, &replacement_local);
	if (!expect(ctx.np_protocol.connection_list.size() == 1,
	            "replacement listen session owns exactly one connection"))
		return false;
	const opennova::np::NapiNPConnection &replacement =
			ctx.np_protocol.connection_list.front();
	if (!expect(
				replacement.type ==
						opennova::np::NapiNPConnection::kTypeClientSide &&
					replacement.connection_id == opennova::np::kHostPlayerDcb &&
					replacement.link.transport == &replacement_local &&
					ctx.np_protocol.next_connection_id ==
						opennova::np::kFirstJoinerDcb,
				"replacement session drops stale roles and installs its own loopback"))
		return false;

	opennova::np::set_connection_mode(ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(ctx, SocketMode::Lan);
	ctx.np_protocol.next_connection_id = 88;
	opennova::np::create_session(ctx, settings, startup, nullptr);
	return expect(
			ctx.np_protocol.connection_list.empty() &&
					ctx.np_protocol.next_connection_id ==
						opennova::np::kFirstJoinerDcb,
			"dedicated replacement clears the prior loopback and resets joiner ids");
}

// A listen host has no ClientAuth upload. Its resolved PLAYER_INFO fields must
// therefore reach the type-2 loopback before Server_ProcessPendingPlayerSpawns
// consumes them [orig: local profile path into Server_PlayerAdd @0x51CBC0].
bool check_listen_host_installs_local_character_profile() {
	opennova::netsim::LoopbackChannel local_client;
	opennova::np::HostOwner owner;
	owner.host_loopback = &local_client;
	opennova::np::HostConfig config;
	config.socket_mode = SocketMode::Socketless;
	config.serve_and_play = true;
	config.local_character_vars.char_id[0] = 0x0400;
	config.local_character_vars.char_id[1] = 0x8407;
	config.local_character_vars.char_class[0] = 6;
	config.local_character_vars.char_class[1] = 7;
	config.local_character_vars.avatar[0] = 3;
	config.local_character_vars.avatar[1] = 9;

	opennova::np::start_host_session(owner, config);
	if (!expect(owner.ctx.np_protocol.connection_list.size() == 1,
			"listen host owns one type-2 local connection"))
		return false;
	const opennova::np::NapiNPConnection &connection =
			owner.ctx.np_protocol.connection_list.front();
	return expect(connection.type == 2 &&
			connection.char_vars.char_id[0] == 0x0400 &&
			connection.char_vars.char_id[1] == 0x8407 &&
			connection.char_vars.char_class[0] == 6 &&
			connection.char_vars.char_class[1] == 7 &&
			connection.char_vars.avatar[0] == 3 &&
			connection.char_vars.avatar[1] == 9,
			"listen-host type-2 connection receives both selected characters");
}

// A dedicated host (mode 1) starts the server but registers no local client connection.
bool check_dedicated_host_has_no_local_client() {
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(ctx, SocketMode::Lan);
	opennova::np::GameConfig settings;
	settings.max_players = 16;
	opennova::np::create_session(ctx, settings, opennova::np::SessionStartup{}, nullptr);
	if (!expect(ctx.np_protocol.host_running == 1, "dedicated host running")) return false;
	if (!expect(ctx.np_protocol.connection_list.empty(), "no local client on a dedicated host"))
		return false;
	return true;
}

// Production serve mode is a true HostOnly session: supplying the adapter's loopback must not
// create a type-2 local client or lazily spawn/count a phantom player. Volatile startup terms
// are minted by the production helper when the deterministic test override is zero.
bool check_production_serve_mode_has_no_phantom_and_mints_startup() {
	opennova::netsim::LoopbackChannel unused_local_view;
	opennova::np::HostOwner owner;
	owner.host_loopback = &unused_local_view;
	opennova::np::HostConfig config;
	config.socket_mode = SocketMode::Lan;
	config.config.max_players = 16;
	config.serve_and_play = false;
	config.host_key = 0;

	opennova::np::start_host_session(owner, config);
	if (!expect(owner.ctx.connection_mode == ConnectionMode::HostOnly &&
	                    owner.ctx.is_authority == 1 &&
	                    owner.ctx.is_mp_session_peer == 0,
	            "production serve mode starts a HostOnly session")) return false;
	if (!expect(owner.ctx.np_protocol.connection_list.empty(),
	            "production serve mode registers no type-2 phantom player")) return false;
	if (!expect(owner.ctx.np_protocol.host_key != 0,
	            "production startup mints a nonzero host key")) return false;
	if (!expect(owner.ctx.np_protocol.host_start_tick != 0,
	            "production startup records a nonzero monotonic start tick")) return false;
	if (!expect(owner.ctx.np_protocol.session_seed_id >= 100000 &&
	                    owner.ctx.np_protocol.session_seed_id <= 999999,
	            "production startup mints the retail six-digit session seed")) return false;
	return true;
}

// Messages queued at one host send boundary share one sequenced packet up to the negotiated packet
// ceiling. The retail gameplay capture carries the S2C 0x57 RTT reply beside the per-frame 0x0A;
// framing each UdpSessionTransport record separately doubles sequence/UDP traffic.
bool check_host_pump_batches_one_send_boundary() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);

	const opennova::PeerAddr peer{0x0100007Fu, 33100};
	opennova::np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERBATCHSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	conn.client_scrk =
			"CLIENTBATCHSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789AB";
	conn.server_sk = 0x55667788u;
	conn.client_ck = 0x10203040u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.seq.outbound_message_limit = opennova::JO_SESSION_OUTBOUND_MESSAGE_MAX;

	// One real C2S 0x2C echo-request produces transient 0x57 through
	// handle_server_datagram; the same boundary also carries retained 0x39 and
	// transient 0x0A.
	opennova::ProtocolPacketHeader inbound_header;
	inbound_header.session_id = conn.server_sk;
	inbound_header.seq_num = 1;
	inbound_header.ack_count = 0;
	std::vector<uint8_t> inbound_body;
	if (!expect(opennova::encode_protocol_packet_plaintext(
	                    inbound_header,
	                    {opennova::make_protocol_message(0x2C, {1, 2, 3, 4, 1})},
	                    conn.client_scrk, inbound_body),
	            "batch fixture encodes the client RTT request")) return false;
	std::vector<uint8_t> inbound = opennova::nw_encode_outbound(
			opennova::SESSION_OPCODE_PROTOCOL_MESSAGE, std::move(inbound_body));
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	peer_link.transport->host_send(
			opennova::s2c::CHARATTR_CRC_CHALLENGE, {0x5D, 0x3D, 0x00, 0x00});
	peer_link.transport->host_send(
			opennova::s2c::PER_FRAME_UPDATE, {0, 0, 0, 0}, /*reliable=*/false);
	CaptureDatagramSocket socket;
	socket.incoming.push_back({peer, std::move(inbound)});
	opennova::np::host_session_pump(owner, socket);
	if (!expect(owner.ctx.np_protocol.connection_list.size() == 1 &&
	                    owner.ctx.np_protocol.connection_list.front().peer == peer,
	            "one host tick preserves the admitted remote peer"))
		return false;
	if (!expect(socket.sent.size() == 1,
	            "host batches mixed-delivery records in one send-boundary datagram"))
		return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> session_body;
	if (!expect(opennova::nw_decode_inbound(
	                    socket.sent[0].data(), socket.sent[0].size(), opcode, session_body) &&
	                    opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
	            "batched host datagram is one S2C session packet")) return false;
	opennova::ProtocolPacketHeader header;
	std::vector<opennova::ProtocolMessage> messages;
	if (!expect(opennova::decode_protocol_packet_plaintext(
	                    session_body.data(), session_body.size(),
	                    owner.ctx.np_protocol.connection_list.front().server_scrk,
	                    header, messages),
	            "batched host session packet decodes")) return false;
	if (!expect(messages.size() == 3 && messages[0].tag == 0x57 &&
	                      messages[1].tag ==
	                              opennova::s2c::CHARATTR_CRC_CHALLENGE &&
	                      messages[2].tag == opennova::s2c::PER_FRAME_UPDATE,
	              "decoded packet preserves transient/reliable/transient queue order"))
		return false;
	const auto &retained =
			owner.ctx.np_protocol.connection_list.front().seq.retained_outbound;
	return expect(retained.size() == 1 && retained.begin()->second.size() == 1 &&
	                      retained.begin()->second.front().tag ==
	                              opennova::s2c::CHARATTR_CRC_CHALLENGE &&
	                      owner.ctx.np_protocol.connection_list.front()
	                                      .seq.send_flush_counter == 1,
	              "NACK state retains only 0x39 and advances one logical host flush");
}

// Initial-state producers participate in the same semantic send queue as
// reactive replies. The 00TRg retail witness batches the first player-sync
// record behind the requested player repair: [0x46, 0x2C]. Pre-framing 0x2C
// inside tick_connections reverses that order and consumes a second UDP packet.
bool check_initial_stream_batches_with_reactive_reply() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	config.send_holdoff_ticks = 1;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);

	opennova::world::World world;
	world.registry.configure_pool(0, 4);
	opennova::world::Entity player;
	player.kind = opennova::world::EntityKind::Organic;
	player.item_id = 0x14B9;
	const opennova::world::EntityHandle owned = world.registry.spawn(0, player);
	if (!expect(owned.valid(), "initial batching fixture owns a player"))
		return false;
	owner.ctx.world = &world;
	owner.now_tick = 1;

	const opennova::PeerAddr peer{0x0100007Fu, 33101};
	opennova::np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport =
			std::make_unique<opennova::netsim::UdpSessionTransport>(
					opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::PlayerAdded;
	conn.admission_stage = opennova::np::GameAdmissionStage::Complete;
	conn.reply.admission_metadata_pushed = true;
	conn.reply.spawn_metadata_pushed = true;
	conn.reply.roster_pushed = true;
	conn.reply.roster_completed_tick = 0;
	conn.burst.sync_state = 2;
	conn.burst.player_sync_subphase = 8;
	conn.link.owned_entity = owned;
	conn.server_scrk =
			"SERVERINITIALBATCHSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
	conn.client_ck = 0x10203040u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	opennova::np::arm_s2c_send_holdoff(conn, 1);
	opennova::np::reset_s2c_send_holdoff_counter(conn);
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));

	owner.pending_session_messages[peer].push_back(
			opennova::make_protocol_message(0x46, {0x00}));
	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1,
	            "reactive 0x46 and initial 0x2C share one S2C packet"))
		return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> session_body;
	opennova::ProtocolPacketHeader header;
	std::vector<opennova::ProtocolMessage> messages;
	if (!expect(
			opennova::nw_decode_inbound(
					socket.sent[0].data(), socket.sent[0].size(), opcode,
					session_body) &&
					opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
			opennova::decode_protocol_packet_plaintext(
					session_body.data(), session_body.size(),
					owner.ctx.np_protocol.connection_list.front().server_scrk,
					header, messages),
			"batched reactive/initial packet decodes")) {
		return false;
	}
	return expect(
			messages.size() == 2 && messages[0].tag == 0x46 &&
			messages[1].tag == opennova::s2c::MISSION_MAP_NAMES,
			"batched boundary preserves retail 0x46 then 0x2C ordering");
}

bool check_host_frame_failure_preserves_owner_queue() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	opennova::np::create_session(
			owner.ctx, config, opennova::np::SessionStartup{}, nullptr);

	const opennova::PeerAddr peer{0x0100007Fu, 33102};
	auto &peer_link = owner.peers[peer];
	peer_link.transport =
			std::make_unique<opennova::netsim::UdpSessionTransport>(
					opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.client_ck = 0x10203040u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	owner.pending_session_messages[peer].push_back(
			opennova::make_protocol_message(0x49, {0x02, 0x00, 0x07, 0x10}));

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	auto pending = owner.pending_session_messages.find(peer);
	if (!expect(socket.sent.empty() &&
	                    pending != owner.pending_session_messages.end() &&
	                    pending->second.size() == 1 &&
	                    pending->second.front().tag == 0x49,
	            "host frame failure preserves the unframed semantic owner queue"))
		return false;

	owner.ctx.np_protocol.connection_list.front().server_scrk =
			"SERVERRETRYQUEUESCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
	opennova::np::host_session_pump(owner, socket);
	return expect(socket.sent.size() == 1 &&
	                      owner.pending_session_messages.find(peer) ==
	                              owner.pending_session_messages.end(),
	              "the next valid host boundary drains the preserved queue once");
}

// A FIRST/MID/FINAL run re-queued by a failed flush arrives as separate
// pending entries (headless once its leading pieces shipped). The capacity
// gate must treat the run as ONE unit: admitting a prefix — or dropping the
// closing FINAL under the one-record rejection rule — permanently strands the
// receiver's reassembly buffer on the ordered reliable channel.
bool check_requeued_fragment_run_stays_one_capacity_unit() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	opennova::np::create_session(
			owner.ctx, config, opennova::np::SessionStartup{}, nullptr);

	const opennova::PeerAddr peer{0x0100007Fu, 33115};
	auto &peer_link = owner.peers[peer];
	peer_link.transport =
			std::make_unique<opennova::netsim::UdpSessionTransport>(
					opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERFRAGRUNSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456";
	conn.client_ck = 0x10203040u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.seq = opennova::np::make_jo_game_session_sequencing(2, 0);
	conn.seq.retained_outbound[1] = std::vector<opennova::ProtocolMessage>(
			opennova::JO_SESSION_OUTBOUND_MESSAGE_MAX - 1,
			opennova::make_protocol_message(0x60, {}));
	conn.seq.retained_outbound_message_count =
			opennova::JO_SESSION_OUTBOUND_MESSAGE_MAX - 1;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	// The headless orphan run: a MID (FRAG_CONT|FRAG_END) and its closing
	// FINAL (FRAG_END), exactly what a failed mid-boundary flush re-queues
	// after the FIRST already shipped.
	owner.pending_session_messages[peer] = {
			opennova::make_protocol_message(
					0x63, {0x11, 0x22},
					opennova::PROTOCOL_MSG_FLAG_FRAG_CONT |
							opennova::PROTOCOL_MSG_FLAG_FRAG_END |
							opennova::PROTOCOL_MSG_FLAG_LEN8),
			opennova::make_protocol_message(
					0x63, {0x33, 0x44},
					opennova::PROTOCOL_MSG_FLAG_FRAG_END |
							opennova::PROTOCOL_MSG_FLAG_LEN8),
	};

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	auto pending = owner.pending_session_messages.find(peer);
	if (!expect(socket.sent.empty() &&
	                    pending != owner.pending_session_messages.end() &&
	                    pending->second.size() == 2 &&
	                    pending->second[0].flags.frag_cont &&
	                    pending->second[0].flags.frag_end &&
	                    pending->second[1].flags.frag_end &&
	                    !pending->second[1].flags.frag_cont,
	            "one free node cannot split the orphan MID/FINAL run — both "
	            "pieces stay queued in order"))
		return false;

	auto &remote = owner.ctx.np_protocol.connection_list.front();
	remote.seq.retained_outbound[1].clear();
	remote.seq.retained_outbound_message_count = 0;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1 &&
	                    owner.pending_session_messages.find(peer) ==
	                            owner.pending_session_messages.end(),
	            "the freed boundary drains the whole orphan run at once"))
		return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> session_body;
	opennova::ProtocolPacketHeader header;
	std::vector<opennova::ProtocolMessage> messages;
	return expect(
			opennova::nw_decode_inbound(
					socket.sent[0].data(), socket.sent[0].size(), opcode,
					session_body) &&
					opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
					opennova::decode_protocol_packet_plaintext(
							session_body.data(), session_body.size(),
							remote.server_scrk, header, messages) &&
					messages.size() == 2 && messages[0].tag == 0x63 &&
					messages[0].flags.frag_cont && messages[0].flags.frag_end &&
					messages[1].tag == 0x63 && messages[1].flags.frag_end &&
					!messages[1].flags.frag_cont,
			"the drained packet carries MID then FINAL so reassembly completes");
}

// D-NET-173, host side: with nothing queued and nothing retained, retail's
// send pump still mints a header-only sequence once the EMPTY interval
// (30000 ms) elapses since the connection last framed anything, so a stock
// client's 120 s connection reap never fires on a quiet host; while reliable
// records remain retained, the ACTIVE interval (10000 ms) mints the same
// header-only sequence so the peer's 0x44/0x84 machinery can request the
// loss. [orig: CNapiNPConnection_PumpSendIntervals @0x628FD0]
bool check_host_idle_send_interval_keepalive() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	opennova::np::create_session(
			owner.ctx, config, opennova::np::SessionStartup{}, nullptr);

	const opennova::PeerAddr peer{0x0100007Fu, 33116};
	auto &peer_link = owner.peers[peer];
	peer_link.transport =
			std::make_unique<opennova::netsim::UdpSessionTransport>(
					opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERIDLEKEEPALIVESCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0";
	conn.client_ck = 0x10203040u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));

	CaptureDatagramSocket socket;
	// 30000 ms at the 62 Hz host clock: elapsed_ms = ticks*1000/62, strictly
	// greater than the interval first 1862 ticks after the clock arms on the
	// first open boundary (tick 0 is the arm sentinel, so arming lands on
	// tick 1). Quiet pumps through the threshold send nothing.
	for (int i = 0; i < 1862; ++i)
		opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.empty(),
	            "a quiet connection sends nothing through 30 s"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1,
	            "the elapsed EMPTY interval mints exactly one keepalive"))
		return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> session_body;
	opennova::ProtocolPacketHeader header;
	std::vector<opennova::ProtocolMessage> messages;
	auto &remote = owner.ctx.np_protocol.connection_list.front();
	if (!expect(
			opennova::nw_decode_inbound(
					socket.sent[0].data(), socket.sent[0].size(), opcode,
					session_body) &&
					opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
					opennova::decode_protocol_packet_plaintext(
							session_body.data(), session_body.size(),
							remote.server_scrk, header, messages) &&
					messages.empty(),
			"the keepalive is the header-only minted sequence"))
		return false;

	// The mint reset the clock: the next quiet pump stays silent.
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1,
	            "the minted keepalive re-arms the interval"))
		return false;

	// The ACTIVE leg is tick_connections' preexisting append_active_probe:
	// with reliable records retained, its 16 ms-per-tick accumulator crosses
	// 10000 ms on the 626th retained pump and mints the header-only probe
	// that drives the peer's 0x44/0x84 NACK machinery. The EMPTY leg must
	// stay out of its way (retained connections are not "empty").
	remote.seq.retained_outbound[remote.seq.next_outbound_seq] = {
			opennova::make_protocol_message(0x49, {0x01, 0x00, 0x07, 0x10})};
	remote.seq.retained_outbound_message_count = 1;
	for (int i = 0; i < 625; ++i)
		opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1,
	            "a retained connection stays silent through 10 s"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	return expect(socket.sent.size() == 2,
	              "the elapsed ACTIVE interval mints the retained-records probe");
}

bool check_host_admits_exact_retail_message_prefix() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	opennova::np::create_session(
			owner.ctx, config, opennova::np::SessionStartup{}, nullptr);

	const opennova::PeerAddr peer{0x0100007Fu, 33103};
	auto &peer_link = owner.peers[peer];
	peer_link.transport =
			std::make_unique<opennova::netsim::UdpSessionTransport>(
					opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERMSGPREFIXSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
	conn.client_ck = 0x10203040u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.seq = opennova::np::make_jo_game_session_sequencing(2, 0);
	conn.seq.retained_outbound[1] = std::vector<opennova::ProtocolMessage>(
			opennova::JO_SESSION_OUTBOUND_MESSAGE_MAX - 1,
			opennova::make_protocol_message(0x60, {}));
	conn.seq.retained_outbound_message_count =
			opennova::JO_SESSION_OUTBOUND_MESSAGE_MAX - 1;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	owner.pending_session_messages[peer] = {
			opennova::make_protocol_message(0x61, {0xA1}),
			opennova::make_protocol_message(0x62, {0xB2}),
	};

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 1 &&
	                    owner.pending_session_messages.find(peer) ==
	                            owner.pending_session_messages.end(),
			"host admits one queued node at 1,199 retained and drops the cap tail"))
		return false;

	uint8_t opcode = 0;
	std::vector<uint8_t> session_body;
	opennova::ProtocolPacketHeader header;
	std::vector<opennova::ProtocolMessage> messages;
	const auto &remote = owner.ctx.np_protocol.connection_list.front();
	if (!expect(
			opennova::nw_decode_inbound(
					socket.sent[0].data(), socket.sent[0].size(), opcode,
					session_body) &&
					opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
			opennova::decode_protocol_packet_plaintext(
					session_body.data(), session_body.size(), remote.server_scrk,
					header, messages) &&
					messages.size() == 1 && messages[0].tag == 0x61 &&
					remote.seq.retained_outbound_message_count ==
						opennova::JO_SESSION_OUTBOUND_MESSAGE_MAX,
			"host's exact admitted prefix reaches the retail 1,200-node bound"))
		return false;

	opennova::np::HostOwner transient_owner;
	opennova::np::set_connection_mode(
			transient_owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(transient_owner.ctx, SocketMode::Lan);
	opennova::np::create_session(
			transient_owner.ctx, config, opennova::np::SessionStartup{}, nullptr);
	const opennova::PeerAddr transient_peer{0x0100007Fu, 33104};
	auto &transient_link = transient_owner.peers[transient_peer];
	transient_link.transport =
			std::make_unique<opennova::netsim::UdpSessionTransport>(
					opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection transient_conn;
	transient_conn.peer = transient_peer;
	transient_conn.type = 1;
	transient_conn.phase = opennova::np::ConnectionPhase::InMatch;
	transient_conn.burst.spawned = true;
	transient_conn.reply.roster_seen_gen =
			transient_owner.ctx.np_protocol.roster_generation;
	transient_conn.server_scrk =
			"SERVERTRANSIENTCAPSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123";
	transient_conn.client_ck = 0x50607080u;
	transient_conn.link.transport = transient_link.transport.get();
	transient_conn.link.mode = opennova::netsim::TransportMode::Client;
	transient_conn.seq = opennova::np::make_jo_game_session_sequencing();
	transient_owner.ctx.np_protocol.connection_list.push_back(
			std::move(transient_conn));
	opennova::ProtocolMessage transient =
			opennova::make_protocol_message(0x63, {});
	transient.reliable = false;
	transient_owner.pending_session_messages[transient_peer] =
			std::vector<opennova::ProtocolMessage>(
					opennova::JO_SESSION_OUTBOUND_MESSAGE_MAX + 1, transient);

	CaptureDatagramSocket transient_socket;
	opennova::np::host_session_pump(transient_owner, transient_socket);
	std::size_t admitted = 0;
	for (const std::vector<uint8_t> &datagram : transient_socket.sent) {
		session_body.clear();
		messages.clear();
		if (!opennova::nw_decode_inbound(
					datagram.data(), datagram.size(), opcode, session_body) ||
				opcode != opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE ||
				!opennova::decode_protocol_packet_plaintext(
						session_body.data(), session_body.size(),
						transient_owner.ctx.np_protocol.connection_list.front().server_scrk,
						header, messages))
			return expect(false, "decode host MTU-split transient prefix");
		admitted += messages.size();
	}
	return expect(
			admitted == opennova::JO_SESSION_OUTBOUND_MESSAGE_MAX &&
					transient_owner.ctx.np_protocol.connection_list.front()
							.seq.retained_outbound_message_count == 0 &&
					transient_owner.pending_session_messages.find(transient_peer) ==
							transient_owner.pending_session_messages.end(),
			"host MTU splits count exactly 1,200 transient nodes in one boundary");
}

// The dictated CS field-3 period applies in both directions. The host keeps
// simulation/C2S full-rate, but remote 0x0A production and the S2C flush open
// only on the exact decrement-before-gate boundary. The high retail BANDWIDTH
// setting must still fit one 1300-byte session packet after its LEN16 envelope.
bool check_host_s2c_holdoff_and_frame_envelope() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	config.entity_send_budget = 1600;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);

	opennova::world::World world;
	world.registry.configure_pool(0, 2);
	world.registry.configure_pool(1, 80);
	opennova::world::Entity recipient;
	recipient.kind = opennova::world::EntityKind::Organic;
	recipient.item_type = 3;
	recipient.health = 150;
	const opennova::world::EntityHandle recipient_h =
			world.registry.spawn(0, recipient);
	if (!expect(recipient_h.valid(), "frame-envelope recipient spawned")) return false;
	for (int i = 0; i < 70; ++i) {
		opennova::world::Entity vehicle;
		vehicle.kind = opennova::world::EntityKind::Item;
		vehicle.item_id = 0x1004;
		vehicle.net_class_code = static_cast<uint8_t>(opennova::EntityClass::Vehicle);
		vehicle.health = 3000;
		vehicle.health_max = 3000;
		vehicle.position.x = static_cast<float>(i);
		if (!expect(world.registry.spawn(1, vehicle).valid(),
		            "frame-envelope fixture vehicle spawned")) return false;
	}
	owner.ctx.world = &world;
	opennova::netsim::set_entity_send_budget(1600);

	const opennova::PeerAddr peer{0x0100007Fu, 33120};
	opennova::np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERHOLDOFFSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	conn.client_scrk =
			"CLIENTHOLDOFFSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	conn.client_ck = 0x10203040u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.link.owned_entity = recipient_h;
	conn.link.owned_entity_spawn_id =
			world.registry.get(recipient_h)->registry_spawn_id;
	// The per-frame writer needs the deployed recipient allocation before it
	// can derive the compression reference used by this envelope test.
	// [orig: Server_SendEntityStateToPlayer @0x517BA0 state==6 gate and
	// recipient eye/reference reads @0x517BF5..0x517C13]
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	auto &remote = owner.ctx.np_protocol.connection_list.front();
	opennova::np::arm_s2c_send_holdoff(remote, 3);

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.empty() && remote.s2c_send_holdoff_countdown == 2,
	            "first host S2C holdoff tick stays closed") ||
	    !expect(remote.seq.send_flush_counter == 0,
	            "closed host boundary does not age finite retention")) return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.empty() && remote.s2c_send_holdoff_countdown == 1,
	            "second host S2C holdoff tick stays closed") ||
	    !expect(remote.seq.send_flush_counter == 0,
	            "second closed host boundary still does not advance the flush counter")) return false;
	opennova::np::host_session_pump(owner, socket);
	const std::size_t boundary_packet_count = socket.sent.size();
	if (!expect(boundary_packet_count >= 1 && boundary_packet_count <= 2 &&
	                    remote.s2c_send_holdoff_countdown == 3,
	            "third host tick opens one S2C boundary and reloads") ||
	    !expect(remote.seq.send_flush_counter == 1,
	            "one open host boundary increments the flush counter exactly once")) return false;
	if (!expect(world.logic_tick == 3,
	            "host simulation remains full-rate while S2C is held")) return false;

	std::vector<opennova::ProtocolMessage> messages;
	// BuildOutgoingPackets may place the small 0x79 beside 0x0A or start a
	// second physical packet when the large frame consumes the 1300-byte cap;
	// both packets still belong to this ONE logical PumpFlags boundary.
	// [orig: CNapiNPConnection_BuildOutgoingPackets @0x628430;
	// CNapiNPConnection_PumpFlags @0x629780]
	for (const std::vector<uint8_t> &raw : socket.sent) {
		uint8_t opcode = 0;
		std::vector<uint8_t> session_body;
		if (!expect(opennova::nw_decode_inbound(
		                    raw.data(), raw.size(), opcode, session_body) &&
		                    opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
		            "held host boundary decodes as S2C session packet(s)")) return false;
		if (!expect(session_body.size() <= opennova::np::kGameSessionMaxPacketBytes,
		            "every session body obeys the installed 1300-byte ceiling")) return false;
		opennova::ProtocolPacketHeader header;
		std::vector<opennova::ProtocolMessage> packet_messages;
		if (!expect(opennova::decode_protocol_packet_plaintext(
		                    session_body.data(), session_body.size(),
		                    owner.ctx.np_protocol.connection_list.front().server_scrk,
		                    header, packet_messages),
		            "held host session packet decrypts")) return false;
		messages.insert(messages.end(),
		                std::make_move_iterator(packet_messages.begin()),
		                std::make_move_iterator(packet_messages.end()));
	}
	if (!expect(messages.size() == 2 &&
	                    messages[0].tag == opennova::s2c::NETWORK_QUALITY &&
	                    messages[0].payload == std::vector<uint8_t>({0x01}) &&
	                    messages[1].tag == opennova::s2c::PER_FRAME_UPDATE &&
	                    messages[1].payload.size() > 1200 &&
	                    messages[1].payload.size() <=
	                            opennova::np::kMaxFrameUpdateBodyBytes,
	            "round-reset 0x79 and one fresh 0x0A fit the envelope-aware cap")) return false;

	opennova::np::host_session_pump(owner, socket);
	return expect(socket.sent.size() == boundary_packet_count &&
	                      remote.s2c_send_holdoff_countdown == 2 &&
	                      remote.seq.send_flush_counter == 1,
	              "next host S2C boundary remains closed for the full period");
}

// The retail send-block clock belongs to each NapiNPConnection. Peers admitted
// on different host ticks must retain their own phase; a session-global counter
// would incorrectly release both on the same boundary.
bool check_host_s2c_holdoff_is_per_connection() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);

	opennova::world::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	owner.ctx.world = &world;

	auto add_remote = [&](const opennova::PeerAddr &peer, const char *server_scrk,
	                      uint32_t client_ck) {
		opennova::np::PeerLink &peer_link = owner.peers[peer];
		peer_link.transport =
				std::make_unique<opennova::netsim::UdpSessionTransport>(
						opennova::netsim::UdpSessionTransport::Role::Host);
		opennova::np::NapiNPConnection conn;
		conn.peer = peer;
		conn.type = 1;
		conn.phase = opennova::np::ConnectionPhase::InMatch;
		conn.burst.spawned = true;
		conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
		conn.server_scrk = server_scrk;
		conn.client_ck = client_ck;
		conn.link.transport = peer_link.transport.get();
		conn.link.mode = opennova::netsim::TransportMode::Client;
		owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	};

	const opennova::PeerAddr peer_a{0x0100007Fu, 33130};
	const opennova::PeerAddr peer_b{0x0100007Fu, 33131};
	add_remote(peer_a,
	           "SERVERHOLDAPERSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
	           0x11112222u);
	add_remote(peer_b,
	           "SERVERHOLDBPERSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
	           0x33334444u);
	auto &remote_a = owner.ctx.np_protocol.connection_list[0];
	auto &remote_b = owner.ctx.np_protocol.connection_list[1];
	opennova::np::arm_s2c_send_holdoff(remote_a, 3);
	opennova::np::arm_s2c_send_holdoff(remote_b, 3);
	// Model B having been admitted two pump ticks earlier than A.
	remote_b.s2c_send_holdoff_countdown = 1;
	remote_b.s2c_send_boundary_open = false;

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent_to.size() == 1 && socket.sent_to[0] == peer_b &&
	                    remote_a.s2c_send_holdoff_countdown == 2 &&
	                    remote_b.s2c_send_holdoff_countdown == 3,
	            "earlier peer opens and reloads without releasing the later peer"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent_to.size() == 1 &&
	                    remote_a.s2c_send_holdoff_countdown == 1 &&
	                    remote_b.s2c_send_holdoff_countdown == 2,
	            "staggered peer clocks advance independently"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	return expect(socket.sent_to.size() == 2 && socket.sent_to[1] == peer_a &&
	                      remote_a.s2c_send_holdoff_countdown == 3 &&
	                      remote_b.s2c_send_holdoff_countdown == 1 &&
	                      world.logic_tick == 3,
	              "later peer reaches its own boundary while simulation stays full-rate");
}

// The same connection clock gates the initial-state stream. This protocol-only
// fixture deliberately omits a HostOwner PeerLink, exercising the preframed
// fallback while proving that it still obeys the remote send boundary.
bool check_initial_stream_obeys_connection_holdoff() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	config.send_holdoff_ticks = 3;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);

	opennova::world::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(1, 4);
	opennova::world::Entity player;
	player.kind = opennova::world::EntityKind::Organic;
	player.item_id = 0x14B9;
	const auto owned = world.registry.spawn(0, player);
	if (!expect(owned.valid(), "initial-stream fixture owns a pool-0 player"))
		return false;
	owner.ctx.world = &world;

	const opennova::PeerAddr peer{0x0100007Fu, 33135};
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::PlayerAdded;
	conn.admission_stage = opennova::np::GameAdmissionStage::AwaitPaddingEcho;
	conn.admission_padding_x = 0x11223344u;
	conn.admission_padding_y = 0x55667788u;
	conn.link.owned_entity = owned;
	conn.server_scrk =
			"SERVERINITIALHOLDSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	conn.client_ck = 0x778899AAu;
	opennova::np::arm_s2c_send_holdoff(
			conn, config.effective_send_holdoff_ticks());
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	auto &remote = owner.ctx.np_protocol.connection_list.front();
	std::vector<uint8_t> echo(256, 0);
	auto write_echo_u32 = [&](size_t offset, uint32_t value) {
		for (size_t i = 0; i < 4; ++i)
			echo[offset + i] = static_cast<uint8_t>(value >> (i * 8));
	};
	write_echo_u32(0, remote.admission_padding_x);
	write_echo_u32(4, remote.admission_padding_y);
	const std::vector<opennova::ProtocolMessage> post_handshake =
			opennova::np::dispatch_session_replies(
					config, remote,
					{opennova::make_protocol_message(0x02, std::move(echo))},
					0, owner.ctx.np_protocol.connection_list, &world);
	if (!expect(!post_handshake.empty() &&
	                    remote.admission_stage ==
	                            opennova::np::GameAdmissionStage::Complete &&
	                    remote.s2c_send_holdoff_ticks == 3 &&
	                    remote.s2c_send_holdoff_countdown == 0 &&
	                    remote.s2c_send_boundary_open,
	            "post-handshake holdoff dictation resets the first boundary open"))
		return false;
	const uint32_t initial_sequence = remote.seq.next_outbound_seq;

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.empty() &&
	                    remote.s2c_send_holdoff_countdown == 3 &&
	                    remote.seq.next_outbound_seq == initial_sequence &&
	                    owner.pending_session_datagrams.find(peer) ==
	                            owner.pending_session_datagrams.end(),
	            "the admission pump does not coalesce the later spawn metadata"))
		return false;
	const size_t first_send_count = socket.sent.size();
	const uint32_t first_boundary_sequence = remote.seq.next_outbound_seq;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == first_send_count &&
	                    remote.s2c_send_holdoff_countdown == 2 &&
	                    remote.seq.next_outbound_seq == first_boundary_sequence &&
	                    owner.pending_session_datagrams.find(peer) ==
	                            owner.pending_session_datagrams.end(),
	            "period three holds the first frame after admission"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == first_send_count &&
	                    remote.s2c_send_holdoff_countdown == 1 &&
	                    remote.seq.next_outbound_seq == first_boundary_sequence &&
	                    owner.pending_session_datagrams.find(peer) ==
	                            owner.pending_session_datagrams.end(),
	            "period three holds exactly N-1 frames after admission"))
		return false;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() > first_send_count &&
	                    remote.s2c_send_holdoff_countdown == 3 &&
	                    remote.seq.next_outbound_seq > first_boundary_sequence,
	            "the next host boundary emits spawn metadata after exactly N-1 held frames"))
		return false;
	return std::all_of(socket.sent_to.begin(), socket.sent_to.end(),
	                   [&](const opennova::PeerAddr &to) { return to == peer; });
}

// The 1300-byte cap belongs to UDP session framing, not the host's type-2
// in-process presentation channel. Preserve its existing unbounded high-rate
// frame so a dense listen-server world is not artificially subrated.
bool check_host_loopback_does_not_inherit_udp_envelope() {
	opennova::netsim::LoopbackChannel loopback;
	NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(ctx, ConnectionMode::HostClient);
	opennova::np::set_transport_mode(ctx, SocketMode::Socketless);
	opennova::np::GameConfig config;
	opennova::np::create_session(
			ctx, config, opennova::np::SessionStartup{}, &loopback);
	if (!expect(ctx.np_protocol.connection_list.size() == 1,
	            "loopback envelope fixture has one host client")) return false;
	ctx.np_protocol.connection_list.front().burst.spawned = true;

	opennova::world::World world;
	world.registry.configure_pool(0, 2);
	world.registry.configure_pool(1, 80);
	opennova::world::Entity recipient;
	recipient.kind = opennova::world::EntityKind::Organic;
	recipient.item_type = 3;
	recipient.health = 150;
	const opennova::world::EntityHandle recipient_h =
			world.registry.spawn(0, recipient);
	if (!expect(recipient_h.valid(), "loopback envelope recipient spawned")) return false;
	auto &loopback_conn = ctx.np_protocol.connection_list.front().link;
	loopback_conn.owned_entity = recipient_h;
	loopback_conn.owned_entity_spawn_id =
			world.registry.get(recipient_h)->registry_spawn_id;
	// Host loopback follows the same deployed-player writer gate as a peer.
	// [orig: Server_BuildPlayerInfoAndAdd @0x51D560 ->
	// Server_SendEntityStateToPlayer @0x517BA0]
	for (int i = 0; i < 70; ++i) {
		opennova::world::Entity vehicle;
		vehicle.kind = opennova::world::EntityKind::Item;
		vehicle.item_id = 0x1004;
		vehicle.net_class_code = static_cast<uint8_t>(opennova::EntityClass::Vehicle);
		vehicle.health = 3000;
		vehicle.position.x = static_cast<float>(i);
		if (!expect(world.registry.spawn(1, vehicle).valid(),
		            "loopback envelope fixture vehicle spawned")) return false;
	}
	ctx.world = &world;
	opennova::netsim::set_entity_send_budget(1600);
	opennova::np::Server_TickUpdate(ctx);
	opennova::netsim::Datagram quality;
	opennova::netsim::Datagram frame;
	if (!expect(loopback.client_recv(quality) &&
	                    quality.tag == opennova::s2c::NETWORK_QUALITY &&
	                    quality.body == std::vector<uint8_t>({0x01}) &&
	                    loopback.client_recv(frame) && frame.tag == 0x0A,
	            "host loopback receives its full-rate frame")) return false;
	return expect(frame.body.size() > opennova::np::kMaxFrameUpdateBodyBytes,
	              "host loopback remains outside the UDP frame envelope");
}

// A sparse pool can make the high-water empty-slot sweep larger than one
// retail session packet. The owner must emit protocol FIRST/FINAL fragments,
// never silently discard the semantic message, and the client-side reassembler
// must recover the complete ordered destroy list.
bool check_sparse_empty_slot_sweep_fragments_without_loss() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);

	const opennova::PeerAddr peer{0x0100007Fu, 33140};
	opennova::np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERFRAGSWEEPSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	conn.client_ck = 0x55667788u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	auto &remote = owner.ctx.np_protocol.connection_list.front();
	opennova::np::arm_s2c_send_holdoff(remote, 0);

	opennova::world::World sparse_world;
	sparse_world.registry.configure_pool(0, 1024);
	opennova::world::Entity seed;
	const auto low = sparse_world.registry.spawn(0, seed);
	const auto high = sparse_world.registry.spawn_from(0, 1023, seed);
	if (!expect(low.valid() && low.slot() == 0 && high.valid() && high.slot() == 1023,
	            "sparse-sweep fixture establishes a 1024-slot high-water mark"))
		return false;

	std::vector<opennova::ProtocolMessage> replies =
			opennova::np::dispatch_session_replies(
					config, remote,
					{opennova::make_protocol_message(
							opennova::c2s::EMPTY_SLOT_SWEEP_REQUEST, {})},
					100, owner.ctx.np_protocol.connection_list, &sparse_world);
	if (!expect(replies.size() == 1 &&
	                    replies.front().tag == opennova::s2c::EMPTY_SLOT_SWEEP &&
	                    replies.front().payload.size() >
	                            opennova::np::kMaxFrameUpdateBodyBytes,
	            "sparse high-water sweep exceeds one framed-message payload"))
		return false;
	replies.front().retention_flushes = 310;
	owner.pending_session_messages[peer] = std::move(replies);

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 2 && socket.sent_to.size() == 2 &&
	                    socket.sent_to[0] == peer && socket.sent_to[1] == peer,
	            "oversized sweep emits two ordered S2C datagrams"))
		return false;
	if (!expect(remote.seq.send_flush_counter == 1 &&
	                    remote.seq.retained_outbound_message_count == 2,
	            "two fragment packets age as one open logical send boundary"))
		return false;
	for (const auto &[sequence, retained] : remote.seq.retained_outbound) {
		(void)sequence;
		if (!expect(retained.size() == 1 &&
		                    retained.front().retention_flushes == 310 &&
		                    retained.front().retention_deadline_flush == 309,
		            "every retained fragment preserves the semantic finite lifetime"))
			return false;
	}

	opennova::np::JoinerConnection joiner(
			"FragmentJoiner", [] { return uint64_t{0}; });
	joiner.seed_in_match(
			0x11223344u, remote.client_ck,
			"CLIENTFRAGSWEEPSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
			remote.server_scrk, 1, 0, 0, 0x14B9);
	const auto first_poll = joiner.handle_datagram(
			socket.sent[0].data(), socket.sent[0].size());
	if (!expect(first_poll.destroyed_pool0_slots.empty(),
	            "joiner does not dispatch an incomplete sweep fragment"))
		return false;
	const auto final_poll = joiner.handle_datagram(
			socket.sent[1].data(), socket.sent[1].size());
	if (!expect(final_poll.destroyed_pool0_slots.size() == 1022,
	            "joiner dispatches one complete sweep after the final fragment"))
		return false;
	for (std::size_t i = 0; i < final_poll.destroyed_pool0_slots.size(); ++i) {
		if (!expect(final_poll.destroyed_pool0_slots[i] == i + 1,
		            "joiner receives every fragmented pool slot in order"))
			return false;
	}

	opennova::ProtocolReassemblyState reassembly;
	std::vector<uint8_t> assembled;
	for (std::size_t i = 0; i < socket.sent.size(); ++i) {
		uint8_t opcode = 0;
		std::vector<uint8_t> session_body;
		if (!expect(opennova::nw_decode_inbound(
		                    socket.sent[i].data(), socket.sent[i].size(), opcode,
		                    session_body) &&
		                    opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
		                    session_body.size() <=
		                            opennova::np::kGameSessionMaxPacketBytes,
		            "each sweep fragment obeys the 1300-byte session ceiling"))
			return false;
		opennova::ProtocolPacketHeader header;
		std::vector<opennova::ProtocolMessage> messages;
		if (!expect(opennova::decode_protocol_packet_plaintext(
		                    session_body.data(), session_body.size(), remote.server_scrk,
		                    header, messages) &&
		                    messages.size() == 1 &&
		                    messages.front().tag == opennova::s2c::EMPTY_SLOT_SWEEP,
		            "sweep fragment decrypts as one 0x5D record"))
			return false;
		const opennova::ProtocolMessage &fragment = messages.front();
		if (i == 0) {
			if (!expect(fragment.flags.frag_cont && !fragment.flags.frag_end,
			            "first sweep piece carries the FIRST fragment flags"))
				return false;
		} else if (!expect(!fragment.flags.frag_cont && fragment.flags.frag_end,
		                   "last sweep piece carries the FINAL fragment flags")) {
			return false;
		}
		const bool complete =
				opennova::reassemble_protocol_payload(reassembly, fragment, assembled);
		if (!expect(complete == (i + 1 == socket.sent.size()),
		            "sweep dispatch occurs only after the final fragment"))
			return false;
	}

	opennova::DestroyEntityList decoded;
	if (!expect(opennova::decode_destroy_entity_list(
	                    assembled.data(), assembled.size(), decoded) &&
	                    decoded.pool0_indices.size() == 1022,
	            "reassembled sweep retains every sparse empty slot"))
		return false;
	for (std::size_t i = 0; i < decoded.pool0_indices.size(); ++i) {
		if (!expect(decoded.pool0_indices[i] == i + 1,
		            "reassembled sweep preserves pool-slot order"))
			return false;
	}
	return true;
}

// A semantic message that needs FIRST/FINAL records must be admitted as one
// group.  With only one queue node free, emitting FIRST and dropping FINAL
// poisons the receiver's reassembly buffer and corrupts the next message.
bool check_fragment_group_waits_for_full_node_capacity() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x01020304u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);

	const opennova::PeerAddr peer{0x0100007Fu, 33141};
	opennova::np::PeerLink &peer_link = owner.peers[peer];
	peer_link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.peer = peer;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.reply.roster_seen_gen = owner.ctx.np_protocol.roster_generation;
	conn.server_scrk =
			"SERVERFRAGCAPSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	conn.client_ck = 0x55667788u;
	conn.link.transport = peer_link.transport.get();
	conn.link.mode = opennova::netsim::TransportMode::Client;
	owner.ctx.np_protocol.connection_list.push_back(std::move(conn));
	auto &remote = owner.ctx.np_protocol.connection_list.front();
	opennova::np::arm_s2c_send_holdoff(remote, 0);

	opennova::ProtocolMessage oversized = opennova::make_protocol_message(
			opennova::s2c::EMPTY_SLOT_SWEEP,
			std::vector<uint8_t>(opennova::np::kGameSessionMaxPacketBytes, 0x5Au));
	oversized.reliable = true;
	owner.pending_session_messages[peer] = {oversized};
	remote.seq.outbound_message_limit = 102;
	remote.seq.retained_outbound_message_count = 101;

	CaptureDatagramSocket socket;
	opennova::np::host_session_pump(owner, socket);
	auto pending = owner.pending_session_messages.find(peer);
	if (!expect(socket.sent.empty(),
	            "one free node emits no partial semantic fragment group") ||
			!expect(pending != owner.pending_session_messages.end() &&
			            pending->second.size() == 1 &&
			            pending->second.front().payload == oversized.payload,
			        "capacity rejection retains the whole semantic message for retry"))
		return false;

	remote.seq.retained_outbound_message_count = 0;
	opennova::np::host_session_pump(owner, socket);
	if (!expect(socket.sent.size() == 2,
	            "full node capacity emits the complete FIRST/FINAL group") ||
			!expect(owner.pending_session_messages.find(peer) ==
			            owner.pending_session_messages.end(),
			        "successful fragment group retry drains the semantic queue"))
		return false;

	for (std::size_t i = 0; i < socket.sent.size(); ++i) {
		uint8_t opcode = 0;
		std::vector<uint8_t> session_body;
		opennova::ProtocolPacketHeader header;
		std::vector<opennova::ProtocolMessage> messages;
		if (!expect(opennova::nw_decode_inbound(socket.sent[i].data(),
		                    socket.sent[i].size(), opcode, session_body) &&
		                    opcode == opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE &&
		                    opennova::decode_protocol_packet_plaintext(
							session_body.data(), session_body.size(), remote.server_scrk,
							header, messages) &&
		                    messages.size() == 1,
		            "retried fragment packet decodes as one protocol record"))
			return false;
		const auto &fragment = messages.front();
		if (!expect(i == 0
					? fragment.flags.frag_cont && !fragment.flags.frag_end
					: !fragment.flags.frag_cont && fragment.flags.frag_end,
		            "retried fragment group preserves FIRST/FINAL ordering"))
			return false;
	}
	return true;
}

// Same-address replacement is delivered as owner cleanup after the protocol has already admitted the
// fresh node. The cleanup event must erase only the old PeerLink; dropping again by endpoint would
// delete the replacement connection created earlier in the same handle_server_datagram call.
bool check_host_pump_reconnect_keeps_fresh_connection() {
	opennova::np::HostOwner owner;
	opennova::np::set_connection_mode(owner.ctx, ConnectionMode::HostOnly);
	opennova::np::set_transport_mode(owner.ctx, SocketMode::Lan);
	opennova::np::GameConfig config;
	config.max_players = 8;
	// This case isolates same-pump replacement ordering; keep its historical
	// per-tick boundary explicit now that a LAN session defaults to period 12.
	config.send_holdoff_ticks = 1;
	opennova::np::SessionStartup startup;
	startup.host_key = 0x0FE0E112u;
	opennova::np::create_session(owner.ctx, config, startup, nullptr);

	opennova::world::World world;
	opennova::world::AiSystem ai;
	world.ai = &ai;
	world.registry.configure_pool(0, 16);
	opennova::world::Entity old_player;
	old_player.item_id = 0x2222u;
	old_player.name = "OldGhost";
	const opennova::world::EntityHandle old_entity =
			world.registry.spawn(0, old_player);
	owner.ctx.world = &world;

	const opennova::PeerAddr peer{0x0100007Fu, 33110};
	opennova::np::PeerLink &old_link = owner.peers[peer];
	old_link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
			opennova::netsim::UdpSessionTransport::Role::Host);
	old_link.transport->push_inbound({0xFE, 0xED});
	old_link.announced = true;
	opennova::np::NapiNPConnection old;
	old.peer = peer;
	old.type = 1;
	old.phase = opennova::np::ConnectionPhase::PlayerAdded;
	old.client_ci = 1;
	old.client_ck = 0x11112222u;
	old.client_scrk =
			"OLDPUMPCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	old.server_sk = 0x33334444u;
	old.server_scrk =
			"OLDPUMPSERVERSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	old.link.owned_entity = old_entity;
	old.link.transport = old_link.transport.get();
	old.reply.player_slot = 1;
	owner.ctx.np_protocol.connection_list.push_back(std::move(old));

	opennova::ClientAuth replacement = opennova::make_jointoperations_client_auth(
			2, 0x55556666u, startup.host_key, "ReplacementJoiner",
			"NEWPUMPCLIENTSCRK0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
	add_retail_game_environment(replacement);
	std::vector<uint8_t> auth = opennova::nw_encode_outbound(
			opennova::SESSION_OPCODE_CLIENT_AUTH,
			opennova::client_auth_to_bytes(replacement));
	CaptureDatagramSocket socket;
	socket.incoming.push_back({peer, std::move(auth)});
	opennova::np::host_session_pump(owner, socket);

	if (!expect(socket.sent.size() == 2 && socket.sent_to.size() == 2 &&
	                    socket.sent_to[0] == peer && socket.sent_to[1] == peer,
	            "replacement sends immediate 0x82 then fresh boundary 0x83"))
		return false;
	uint8_t auth_opcode = 0;
	uint8_t settings_opcode = 0;
	std::vector<uint8_t> auth_body;
	std::vector<uint8_t> settings_body;
	if (!expect(opennova::nw_decode_inbound(
	                    socket.sent[0].data(), socket.sent[0].size(),
	                    auth_opcode, auth_body) &&
	                    opennova::nw_decode_inbound(
	                            socket.sent[1].data(), socket.sent[1].size(),
	                            settings_opcode, settings_body) &&
	                    auth_opcode == opennova::SESSION_OPCODE_SERVER_AUTH &&
	                    settings_opcode ==
	                            opennova::SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE,
	            "replacement preserves auth-before-established packet order"))
		return false;
	const auto fresh_peer = owner.peers.find(peer);
	if (!expect(fresh_peer != owner.peers.end(),
	            "replacement cleanup installs a fresh PeerLink at handshake advance")) return false;
	if (!expect(owner.ctx.np_protocol.connection_list.size() == 1,
	            "replacement connection survives owner cleanup in the same pump")) return false;
	const opennova::np::NapiNPConnection &fresh =
			owner.ctx.np_protocol.connection_list.front();
	if (!expect(fresh.client_ci == replacement.ci &&
	                    fresh.client_ck == replacement.ck &&
	                    fresh.player_name == replacement.na,
	            "surviving connection carries the replacement identity")) return false;
	if (!expect(fresh.link.transport != nullptr &&
	                    fresh.link.transport == fresh_peer->second.transport.get() &&
	                    !fresh_peer->second.announced &&
	                    fresh_peer->second.transport->inbound_pending() == 0,
	            "replacement gets a new unannounced transport instead of inheriting the erased link"))
		return false;
	opennova::ProtocolPacketHeader settings_header;
	std::vector<opennova::ProtocolMessage> settings_messages;
	if (!expect(opennova::decode_protocol_packet_plaintext(
	                    settings_body.data(), settings_body.size(), fresh.server_scrk,
	                    settings_header, settings_messages) &&
	                    settings_header.seq_num == 1 &&
	                    !settings_messages.empty(),
	            "replacement sends its retained sequence-1 initial settings"))
		return false;
	bool old_ghost_survives = false;
	world.registry.for_each([&](const opennova::world::Entity &entity) {
		if (entity.item_id == 0x2222u && entity.name == "OldGhost")
			old_ghost_survives = true;
	});
	return expect(!old_ghost_survives,
	              "replacement pump despawns the old authoritative entity even if its slot is reused");
}

bool check_global_scoreboard_integrity_phase() {
	opennova::np::NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(
			ctx, opennova::np::ConnectionMode::HostOnly);
	opennova::np::GameConfig config;
	config.max_players = 4;
	config.player_name = "PhaseHost";
	opennova::np::create_session(
			ctx, config, opennova::np::SessionStartup{}, nullptr);

	opennova::world::World world;
	world.mp_session = true;
	world.registry.configure_pool(0, 8);
	opennova::world::AiSystem ai;
	world.ai = &ai;
	opennova::world::PlayerSpawn spawn;
	const opennova::world::EntityHandle player =
			opennova::world::spawn_remote_player(world, spawn);
	if (!expect(player.valid(), "integrity phase player spawned")) return false;
	opennova::world::Entity *entity = world.registry.get(player);
	entity->equipped_adm_index = 0x20;
	world.weapons.entries.resize(0x21);
	world.weapons.entries[0x20].valid = true;
	world.weapons.entries[0x20].ammo_index = 0x1A;
	ctx.world = &world;

	opennova::netsim::UdpSessionTransport transport(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.link.transport = &transport;
	conn.link.owned_entity = player;
	conn.reply.player_name = "RetailPhase";
	ctx.np_protocol.connection_list.push_back(conn);

	struct Emitted {
		uint8_t tag = 0;
		std::vector<uint8_t> body;
		bool reliable = true;
	};
	auto drain = [&]() {
		std::vector<Emitted> emitted;
		opennova::netsim::Datagram datagram;
		while (transport.pop_outbound(datagram))
			emitted.push_back(
					{datagram.tag, std::move(datagram.body), datagram.reliable});
		return emitted;
	};
	auto index_of = [](const std::vector<Emitted> &messages, uint8_t tag) {
		for (std::size_t i = 0; i < messages.size(); ++i)
			if (messages[i].tag == tag) return i;
		return messages.size();
	};
	auto has_integrity = [&](const std::vector<Emitted> &messages) {
		return index_of(messages, opennova::s2c::ENTITY_CHECKSUM_REQ) < messages.size() ||
				index_of(messages, opennova::s2c::LOADOUT_CRC_REQ) < messages.size();
	};

	// Mission start reset 0 uses `++timer > 0x136`: exactly 310 calls are
	// quiet even with an unrelated world-clock phase, and call 311 emits the
	// transient scoreboard BEFORE the initial (toggle-zero) 0x31 family.
	world.logic_tick = 9000;
	for (uint32_t i = 0; i < 0x136u; ++i) {
		opennova::np::Server_TickUpdate(ctx);
		if (!expect(!has_integrity(drain()),
		            "fresh scoreboard counter stays quiet through call 310"))
			return false;
	}
	opennova::np::Server_TickUpdate(ctx);
	std::vector<Emitted> boundary = drain();
	std::size_t scoreboard_i = index_of(boundary, opennova::s2c::PLAYER_LIST);
	std::size_t integrity_i = index_of(boundary, opennova::s2c::LOADOUT_CRC_REQ);
	std::size_t frame_i = index_of(boundary, opennova::s2c::PER_FRAME_UPDATE);
	if (!expect(scoreboard_i < integrity_i && integrity_i < frame_i &&
	                    boundary[scoreboard_i].reliable == false &&
	                    boundary[integrity_i].reliable == false &&
	                    boundary[integrity_i].body ==
	                            std::vector<uint8_t>({0x1A, 0x00, 0x00}),
	            "first 311 boundary orders transient 0x16, then initial 0x31, then 0x0A"))
		return false;

	for (uint32_t i = 0; i < 0x136u; ++i) {
		opennova::np::Server_TickUpdate(ctx);
		if (!expect(!has_integrity(drain()),
		            "reloaded scoreboard counter stays quiet through 310 calls"))
			return false;
	}
	opennova::np::Server_TickUpdate(ctx);
	boundary = drain();
	scoreboard_i = index_of(boundary, opennova::s2c::PLAYER_LIST);
	integrity_i = index_of(boundary, opennova::s2c::ENTITY_CHECKSUM_REQ);
	frame_i = index_of(boundary, opennova::s2c::PER_FRAME_UPDATE);
	if (!expect(scoreboard_i < integrity_i && integrity_i < frame_i &&
	                    boundary[integrity_i].body ==
	                            std::vector<uint8_t>({0xFF, 0x00, 0x00}) &&
	                    !boundary[integrity_i].reliable,
	            "second 311 boundary alternates to transient 0x30"))
		return false;

	// The global clocks advance even after the session gate closes, but every
	// maintenance send remains session-only. Keep the peer fully spawned/live so
	// this exercises the gate itself rather than making the recipient ineligible.
	// Then a reused mission owner resets the counter and connection table; after
	// the peer is re-admitted, the process-global toggle survives. Stale
	// health/alive do not suppress 0x30 while Flags bit
	// 0x02 is clear.
	ctx.is_in_session = 0;
	ctx.scoreboard_broadcast_timer = 0x136u;
	ctx.network_quality_broadcast_countdown = 0;
	opennova::np::Server_TickUpdate(ctx);
	boundary = drain();
	if (!expect(index_of(boundary, opennova::s2c::PLAYER_LIST) == boundary.size() &&
	                    !has_integrity(boundary) &&
	                    index_of(boundary, opennova::s2c::NETWORK_QUALITY) ==
	                            boundary.size() &&
	                    ctx.integrity_entity_family_next &&
	                    ctx.network_quality_broadcast_countdown ==
	                            opennova::np::NETWORK_QUALITY_BROADCAST_PERIOD_TICKS,
	            "closed session advances global clocks without maintenance sends"))
		return false;
	ctx.scoreboard_broadcast_timer = 77;
	opennova::np::create_session(
			ctx, config, opennova::np::SessionStartup{}, nullptr);
	ctx.np_protocol.connection_list.push_back(conn);
	if (!expect(ctx.scoreboard_broadcast_timer == 0 &&
	                    ctx.integrity_entity_family_next,
	            "mission reuse resets the scoreboard counter but preserves its family toggle"))
		return false;
	ctx.np_protocol.connection_list.front().burst.spawned = true;
	entity->alive = false;
	entity->health = 0;
	ctx.scoreboard_broadcast_timer = 0x136u;
	opennova::np::Server_TickUpdate(ctx);
	boundary = drain();
	if (!expect(index_of(boundary, opennova::s2c::PLAYER_LIST) <
	                    index_of(boundary, opennova::s2c::ENTITY_CHECKSUM_REQ),
	            "stale health/alive with dead flag clear still receives the due 0x30"))
		return false;

	// The scoreboard is unconditional for the in-match peer even when 0x31's
	// authoritative equipped-ADM lookup cannot resolve a row.
	entity->equipped_adm_index = 0xFF;
	ctx.scoreboard_broadcast_timer = 0x136u;
	opennova::np::Server_TickUpdate(ctx);
	boundary = drain();
	return expect(index_of(boundary, opennova::s2c::PLAYER_LIST) < boundary.size() &&
	                      !has_integrity(boundary),
	              "periodic 0x16 survives an unresolved 0x31 row");
}

bool check_scoreboard_active_slot_filter_is_distinct() {
	opennova::np::NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(
			ctx, opennova::np::ConnectionMode::HostOnly);
	opennova::np::GameConfig config;
	config.max_players = 4;
	opennova::np::create_session(
			ctx, config, opennova::np::SessionStartup{}, nullptr);

	opennova::world::World world;
	world.mp_session = true;
	world.registry.configure_pool(0, 8);
	opennova::world::AiSystem ai;
	world.ai = &ai;
	opennova::world::PlayerSpawn spawn;
	const opennova::world::EntityHandle player =
			opennova::world::spawn_remote_player(world, spawn);
	if (!expect(player.valid(), "scoreboard filter player spawned")) return false;
	ctx.world = &world;

	opennova::netsim::UdpSessionTransport transport(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::PlayerAdded;
	conn.burst.spawned = false;
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.link.transport = &transport;
	conn.link.owned_entity = player;
	conn.reply.player_name = "BoundBeforeSpawn";
	ctx.np_protocol.connection_list.push_back(std::move(conn));

	ctx.scoreboard_broadcast_timer = 0x136u;
	opennova::np::Server_TickUpdate(ctx);
	bool saw_scoreboard = false;
	bool saw_integrity = false;
	bool saw_quality = false;
	opennova::netsim::Datagram datagram;
	while (transport.pop_outbound(datagram)) {
		saw_scoreboard = saw_scoreboard ||
				datagram.tag == opennova::s2c::PLAYER_LIST;
		saw_integrity = saw_integrity ||
				datagram.tag == opennova::s2c::ENTITY_CHECKSUM_REQ ||
				datagram.tag == opennova::s2c::LOADOUT_CRC_REQ;
		saw_quality = saw_quality ||
				datagram.tag == opennova::s2c::NETWORK_QUALITY;
		if (datagram.tag == opennova::s2c::PLAYER_LIST &&
				datagram.reliable) {
			return expect(false, "periodic active-slot scoreboard is transient");
		}
	}
	if (!expect(saw_scoreboard && !saw_integrity && !saw_quality,
	            "active pre-spawn slot receives 0x16 but not state-gated integrity or 0x79"))
		return false;

	// Staging the terminal connection-description closes every ordinary
	// recipient predicate immediately, even though the connection node remains
	// resident long enough for the owner to flush that reliable record.
	if (!expect(opennova::np::Server_StageHostPunt(
	                    ctx.np_protocol.connection_list.front(), 16),
	            "scoreboard filter stages the terminal host description"))
		return false;
	bool saw_description = false;
	while (transport.pop_outbound(datagram)) {
		saw_description = saw_description ||
				datagram.protocol_flags_raw == 0xA0u && datagram.tag == 0x03u;
	}
	if (!expect(saw_description,
	            "scoreboard filter drains the staged connection description"))
		return false;
	ctx.scoreboard_broadcast_timer = 0x136u;
	opennova::np::Server_TickUpdate(ctx);
	while (transport.pop_outbound(datagram)) {
		if (datagram.tag == opennova::s2c::PLAYER_LIST)
			return expect(false,
			              "terminal connection is excluded from later scoreboards");
	}
	return true;
}

// The host compares each active player's accumulated Points field with the
// per-slot cache every authority pass and sends a reliable requester-only 0x81
// on change. The primary-score projection is deliberately not involved.
// [orig: Server_UpdateCaptureZoneProximity @0x5086A0;
//        CRenderState_GetFieldByIndex(player+18, 0x1C) @0x5086E5]
bool check_requester_score_delta_refresh() {
	opennova::np::NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(
			ctx, opennova::np::ConnectionMode::HostOnly);
	opennova::np::GameConfig config;
	config.game_type = opennova::game_type::kDeathmatch;
	opennova::np::create_session(
			ctx, config, opennova::np::SessionStartup{}, nullptr);

	opennova::world::World world;
	world.mp_session = true;
	world.registry.configure_pool(0, 4);
	opennova::world::Entity player_entity;
	player_entity.kind = opennova::world::EntityKind::Organic;
	player_entity.alive = true;
	player_entity.health = 100;
	const opennova::world::EntityHandle player =
			world.registry.spawn(0, player_entity);
	opennova::world::MatchRules rules;
	rules.game_type = config.game_type;
	world.match.configure(rules);
	world.match.upsert_player({player, 0, "Points", {}});
	world.match.player(player)->stats[opennova::world::MatchStats::kPoints] = -5;
	ctx.world = &world;

	opennova::netsim::UdpSessionTransport transport(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.link.transport = &transport;
	conn.link.owned_entity = player;
	ctx.np_protocol.connection_list.push_back(std::move(conn));

	auto drain_scores = [&]() {
		std::vector<opennova::netsim::Datagram> scores;
		opennova::netsim::Datagram datagram;
		while (transport.pop_outbound(datagram)) {
			if (datagram.tag == opennova::s2c::SCORE_DELTA_SOUND)
				scores.push_back(std::move(datagram));
		}
		return scores;
	};

	opennova::np::Server_TickUpdate(ctx);
	std::vector<opennova::netsim::Datagram> scores = drain_scores();
	if (!expect(scores.size() == 1 && scores[0].reliable &&
	                    scores[0].body ==
	                            std::vector<uint8_t>({0xFB, 0xFF, 0xFF, 0xFF}),
	            "changed signed Points emits one reliable requester 0x81"))
		return false;

	opennova::np::Server_TickUpdate(ctx);
	if (!expect(drain_scores().empty(),
	            "unchanged Points is suppressed by the per-player cache"))
		return false;

	world.match.player(player)->stats[opennova::world::MatchStats::kPoints] = 17;
	// The refresh rides the one-second proximity pass, not every frame.
	for (int i = 0; i < 60; ++i) opennova::np::Server_TickUpdate(ctx);
	if (!expect(drain_scores().empty(),
	            "a changed Points value waits for the next 1 Hz proximity pass"))
		return false;
	opennova::np::Server_TickUpdate(ctx);
	scores = drain_scores();
	return expect(scores.size() == 1 && scores[0].reliable &&
	                      scores[0].body ==
	                              std::vector<uint8_t>({0x11, 0x00, 0x00, 0x00}),
	              "the next one-second pass refreshes the requester");
}

bool check_listen_host_receives_targeted_maintenance() {
	opennova::netsim::LoopbackChannel loopback;
	opennova::np::NapiNPServerCtx ctx;
	opennova::np::set_connection_mode(
			ctx, opennova::np::ConnectionMode::HostClient);
	opennova::np::set_transport_mode(
			ctx, opennova::np::SocketMode::Socketless);
	opennova::np::GameConfig config;
	config.max_players = 4;
	opennova::np::create_session(
			ctx, config, opennova::np::SessionStartup{}, &loopback);
	if (!expect(ctx.np_protocol.connection_list.size() == 1 &&
	                    ctx.np_protocol.connection_list.front().type == 2,
	            "listen maintenance fixture owns one type-2 loopback slot"))
		return false;

	opennova::world::World world;
	world.mp_session = true;
	world.registry.configure_pool(0, 8);
	opennova::world::AiSystem ai;
	world.ai = &ai;
	opennova::world::PlayerSpawn spawn;
	const opennova::world::EntityHandle player =
			opennova::world::spawn_remote_player(world, spawn);
	if (!expect(player.valid(), "listen maintenance player spawned")) return false;
	opennova::world::Entity *entity = world.registry.get(player);
	entity->equipped_adm_index = 0x20;
	world.weapons.entries.resize(0x21);
	world.weapons.entries[0x20].valid = true;
	world.weapons.entries[0x20].ammo_index = 0x1A;
	ctx.world = &world;
	ctx.loaded_model_viewport_height = 100;
	ctx.scoreboard_broadcast_timer = 0x136u;
	auto &self = ctx.np_protocol.connection_list.front();
	self.phase = opennova::np::ConnectionPhase::InMatch;
	self.burst.spawned = true;
	self.link.owned_entity = player;
	self.reply.player_name = "ListenHost";
	self.reply.control_live_ticks =
			opennova::np::CONTROL_REQUEST_LIVE_GATE_TICKS;
	self.reply.control_request_countdown = 0;

	opennova::np::Server_TickUpdate(ctx);
	std::vector<uint8_t> tags;
	opennova::netsim::Datagram datagram;
	while (loopback.client_recv(datagram)) tags.push_back(datagram.tag);
	auto index_of = [&](uint8_t tag) {
		const auto it = std::find(tags.begin(), tags.end(), tag);
		return it == tags.end()
				? tags.size()
				: static_cast<std::size_t>(std::distance(tags.begin(), it));
	};
	const std::size_t scoreboard_i = index_of(opennova::s2c::PLAYER_LIST);
	const std::size_t integrity_i = index_of(opennova::s2c::LOADOUT_CRC_REQ);
	const std::size_t quality_i = index_of(opennova::s2c::NETWORK_QUALITY);
	const std::size_t charattr_i =
			index_of(opennova::s2c::CHARATTR_CRC_CHALLENGE);
	const std::size_t input_i = index_of(opennova::s2c::INPUT_STATE_FLAGS);
	const std::size_t time_i = index_of(opennova::s2c::TIME_SYNC_PING);
	const std::size_t model_i =
			index_of(opennova::s2c::LOADED_MODEL_PAGE_REQUEST);
	const std::size_t frame_i = index_of(opennova::s2c::PER_FRAME_UPDATE);
	return expect(scoreboard_i < integrity_i && integrity_i < quality_i &&
	                      quality_i < charattr_i && charattr_i < input_i &&
	                      input_i < time_i && time_i < model_i &&
	                      model_i < frame_i,
	              "listen-host slot receives ordered 0x16/0x31/0x79/quartet/0x0A maintenance");
}

// Retail maintenance follows authoritative player-slot state, not free-running
// per-connection world-clock epochs. Integrity shares the 0x136 scoreboard
// countdown and its 0x31 row comes from the equipped ADM's AmmoDef. The control
// quartet first fires on call 1,861 after 1,860 completed eligible age
// increments, then repeats every 744 mature gate calls. [orig:
// Server_TickUpdate @0x51D7E0 -> @0x508540;
// Server_UpdateAllActivePlayerSlots @0x518820]
bool check_spawned_peer_gets_periodic_retail_maintenance() {
	opennova::np::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.connection_mode = opennova::np::ConnectionMode::HostClient;
	ctx.loaded_model_viewport_height = 100;
	opennova::world::World world;
	world.prng16_state = 1;
	world.mp_session = true;
	world.registry.configure_pool(0, 64);
	opennova::world::AiSystem ai;
	world.ai = &ai;
	opennova::world::PlayerSpawn player_spawn;
	player_spawn.net_id = 0xFFF1;
	const opennova::world::EntityHandle player =
			opennova::world::spawn_remote_player(world, player_spawn);
	if (!expect(player.valid(), "maintenance peer's player entity spawned"))
		return false;
	opennova::world::Entity *player_entity = world.registry.get(player);
	player_entity->equipped_adm_index = 0x20;
	world.weapons.entries.resize(0x21);
	world.weapons.entries[0x20].valid = true;
	world.weapons.entries[0x20].ammo_index = 0x1A;
	ctx.world = &world;

	opennova::netsim::UdpSessionTransport transport(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.link.transport = &transport;
	conn.link.owned_entity = player;
	ctx.np_protocol.connection_list.push_back(std::move(conn));
	ctx.network_quality_broadcast_countdown = 17;
	opennova::np::Server_InitNewRoundState(ctx);

	struct Emitted {
		uint8_t tag = 0;
		std::vector<uint8_t> body;
		bool reliable = true;
	};
	auto drain = [&]() {
		std::vector<Emitted> emitted;
		opennova::netsim::Datagram datagram;
		while (transport.pop_outbound(datagram)) {
			emitted.push_back(
					{datagram.tag, std::move(datagram.body), datagram.reliable});
		}
		return emitted;
	};
	auto find = [](const std::vector<Emitted> &messages, uint8_t tag)
			-> const Emitted * {
		for (const Emitted &message : messages)
			if (message.tag == tag) return &message;
		return nullptr;
	};
	auto u32le = [](const std::vector<uint8_t> &body) {
		return body.size() < 4 ? 0u
				: static_cast<uint32_t>(body[0]) |
				  (static_cast<uint32_t>(body[1]) << 8) |
				  (static_cast<uint32_t>(body[2]) << 16) |
				  (static_cast<uint32_t>(body[3]) << 24);
	};
	auto acknowledge_time_sync = [&](const Emitted &ping, uint32_t client_ms) {
		std::vector<uint8_t> payload = ping.body;
		for (int shift = 0; shift < 32; shift += 8)
			payload.push_back(static_cast<uint8_t>(client_ms >> shift));
		(void)opennova::np::dispatch_session_replies(
				ctx.config, ctx.np_protocol.connection_list.front(),
				{opennova::make_protocol_message(
						opennova::c2s::TIME_SYNC_REPLY, std::move(payload))},
				world.logic_tick, ctx.np_protocol.connection_list, &world);
	};

	// Server_InitNewRoundState clears the one global retail countdown. The next
	// server boundary therefore broadcasts 0x79 immediately and reloads 0x136.
	opennova::np::Server_TickUpdate(ctx);
	const std::vector<Emitted> initial = drain();
	if (!expect(find(initial, opennova::s2c::ENTITY_CHECKSUM_REQ) == nullptr &&
	                    find(initial, opennova::s2c::LOADOUT_CRC_REQ) == nullptr &&
	                    find(initial, opennova::s2c::NETWORK_QUALITY) != nullptr &&
	                    find(initial, opennova::s2c::NETWORK_QUALITY)->body ==
	                            std::vector<uint8_t>({0x01}) &&
	                    find(initial, opennova::s2c::NETWORK_QUALITY)->reliable &&
	                    find(initial, opennova::s2c::CHARATTR_CRC_CHALLENGE) == nullptr,
	            "round-reset quality countdown emits immediately and reliably"))
		return false;

	// Reload is exactly 0x136: 309 later boundaries remain quiet and the 310th
	// emits even when the world clock is deliberately moved off that modulus.
	world.logic_tick = 1000;
	for (uint32_t elapsed = 1;
	     elapsed < opennova::np::NETWORK_QUALITY_BROADCAST_PERIOD_TICKS;
	     ++elapsed) {
		opennova::np::Server_TickUpdate(ctx);
		if (!expect(find(drain(), opennova::s2c::NETWORK_QUALITY) == nullptr,
		            "reloaded quality countdown stays silent before 0x136"))
			return false;
	}
	opennova::np::Server_TickUpdate(ctx);
	const std::vector<Emitted> quality_310 = drain();
	const Emitted *network_quality = find(
			quality_310, opennova::s2c::NETWORK_QUALITY);
	if (!expect(network_quality != nullptr &&
	                    network_quality->body == std::vector<uint8_t>({0x01}) &&
	                    network_quality->reliable &&
	                    find(quality_310,
	                            opennova::s2c::ENTITY_CHECKSUM_REQ) == nullptr,
	            "quality countdown reload emits exactly 0x136 boundaries later"))
		return false;

	// Deploy pending pauses the control live-age increment; it never erases age
	// already accrued. Filter 0x80 remains independent and still admits the slot.
	const uint32_t age_before_pending =
			ctx.np_protocol.connection_list.front().reply.control_live_ticks;
	ctx.np_protocol.connection_list.front().link.respawn_pending = true;
	opennova::np::Server_TickUpdate(ctx);
	const std::vector<Emitted> pending_before_live = drain();
	if (!expect(ctx.np_protocol.connection_list.front().reply.control_live_ticks ==
	                    age_before_pending &&
	                    find(pending_before_live,
	                            opennova::s2c::CHARATTR_CRC_CHALLENGE) == nullptr,
	            "deploy pending pauses without resetting the control live age"))
		return false;
	ctx.np_protocol.connection_list.front().link.respawn_pending = false;

	// Filter 0x80 accepts retail player-slot states 6 and 7. Death and the
	// deploy/respawn-pending presentation state therefore do not suppress 0x79.
	player_entity->alive = false;
	player_entity->health = 0;
	ctx.np_protocol.connection_list.front().link.respawn_pending = true;
	for (uint32_t elapsed = 1;
	     elapsed < opennova::np::NETWORK_QUALITY_BROADCAST_PERIOD_TICKS - 1u;
	     ++elapsed) {
		opennova::np::Server_TickUpdate(ctx);
		const std::vector<Emitted> messages = drain();
		if (!expect(find(messages, opennova::s2c::NETWORK_QUALITY) == nullptr,
		            "quality reload stays quiet before a dead slot's boundary"))
			return false;
	}
	opennova::np::Server_TickUpdate(ctx);
	const std::vector<Emitted> quality_dead = drain();
	if (!expect(find(quality_dead, opennova::s2c::NETWORK_QUALITY) != nullptr,
	            "filter 0x80 includes a dead respawn-pending player slot"))
		return false;

	// Recipient eligibility is sampled only when that global countdown expires.
	// A slot outside the in-match state at the boundary gets no per-peer catch-up.
	player_entity->alive = true;
	player_entity->health = 100;
	ctx.np_protocol.connection_list.front().link.respawn_pending = false;
	ctx.np_protocol.connection_list.front().burst.spawned = false;
	for (uint32_t elapsed = 1;
	     elapsed <= opennova::np::NETWORK_QUALITY_BROADCAST_PERIOD_TICKS;
	     ++elapsed) {
		opennova::np::Server_TickUpdate(ctx);
		const std::vector<Emitted> messages = drain();
		if (!expect(find(messages, opennova::s2c::NETWORK_QUALITY) == nullptr,
		            "quality boundary filters a slot outside the in-match state"))
			return false;
	}
	ctx.np_protocol.connection_list.front().burst.spawned = true;
	opennova::np::Server_TickUpdate(ctx); // no catch-up outside the global boundary
	const std::vector<Emitted> quality_after_rejoin = drain();
	if (!expect(find(quality_after_rejoin,
	                    opennova::s2c::NETWORK_QUALITY) == nullptr,
	            "network quality does not catch up after a missed global boundary"))
		return false;

	world.logic_tick = 743;
	opennova::np::Server_TickUpdate(ctx); // world clock cannot bypass the slot age
	const std::vector<Emitted> premature_control = drain();
	if (!expect(find(premature_control,
	                    opennova::s2c::CHARATTR_CRC_CHALLENGE) == nullptr &&
	                    find(premature_control, opennova::s2c::INPUT_STATE_FLAGS) == nullptr &&
	                    find(premature_control, opennova::s2c::TIME_SYNC_PING) == nullptr &&
	                    find(premature_control,
	                            opennova::s2c::LOADED_MODEL_PAGE_REQUEST) == nullptr,
	            "world tick 744 cannot bypass retail's 1,860-live-tick gate"))
		return false;

	uint32_t live_calls =
			ctx.np_protocol.connection_list.front().reply.control_live_ticks;
	bool early_quartet = false;
	for (; live_calls < opennova::np::CONTROL_REQUEST_LIVE_GATE_TICKS;
			++live_calls) {
		opennova::np::Server_TickUpdate(ctx);
		const std::vector<Emitted> messages = drain();
		early_quartet = early_quartet ||
				find(messages, opennova::s2c::CHARATTR_CRC_CHALLENGE) != nullptr ||
				find(messages, opennova::s2c::INPUT_STATE_FLAGS) != nullptr ||
				find(messages, opennova::s2c::TIME_SYNC_PING) != nullptr ||
				find(messages, opennova::s2c::LOADED_MODEL_PAGE_REQUEST) != nullptr;
	}
	if (!expect(!early_quartet,
	            "control quartet stays silent through 1,860 completed live-age increments"))
		return false;

	opennova::np::Server_TickUpdate(ctx); // next call observes age 1,860
	++live_calls;
	const std::vector<Emitted> first_control = drain();
	const Emitted *charattr = find(
			first_control, opennova::s2c::CHARATTR_CRC_CHALLENGE);
	const Emitted *input_state = find(
			first_control, opennova::s2c::INPUT_STATE_FLAGS);
	const Emitted *time_sync = find(
			first_control, opennova::s2c::TIME_SYNC_PING);
	const Emitted *model_page = find(
			first_control, opennova::s2c::LOADED_MODEL_PAGE_REQUEST);
	// Retail lazily initializes one retained per-player seed with PRNG_Next16;
	// 0x3D5D was one captured session's value, not a protocol constant.
	if (!expect(charattr != nullptr && charattr->body.size() == 4 &&
	                    u32le(charattr->body) == 0x8011u &&
	                    world.prng16_state == 0x8011u &&
	                    charattr->reliable &&
	                    input_state != nullptr &&
	                    input_state->body == std::vector<uint8_t>({0x00, 0x00}) &&
	                    !input_state->reliable &&
	                    time_sync != nullptr && time_sync->body.size() == 4 &&
	                    time_sync->reliable &&
	                    u32le(time_sync->body) == 1 &&
	                    model_page != nullptr && model_page->body.size() == 4 &&
	                    !model_page->reliable &&
	                    u32le(model_page->body) == 50,
	            "call 1,861 emits reliable 0x39/0x43 and transient 0x42/0x68"))
		return false;
	const uint32_t retained_seed = u32le(charattr->body);
	if (!expect(world.next_prng16() == 0x8111u,
	            "the next throwable-bounce draw continues the control 0x39 stream"))
		return false;

	// The two reply handlers clear their independent silence counters at
	// dispatch, not after validating body contents. In particular, retail's
	// validate_time_sync performs this store before the shape/sequence/timing
	// gates, so malformed or stale 0x08 traffic cannot advance the 1,1,2,2 phase
	// but still proves the peer is answering. The 0x1C body is ignored outright.
	auto &reply_state = ctx.np_protocol.connection_list.front().reply;
	(void)opennova::np::dispatch_session_replies(
			ctx.config, ctx.np_protocol.connection_list.front(),
			{opennova::make_protocol_message(
					opennova::c2s::CHARATTR_CRC_REPLY, {0xDE})},
			world.logic_tick, ctx.np_protocol.connection_list, &world);
	if (!expect(reply_state.charattr_unanswered_count == 0,
	            "any C2S 0x1C body clears the charattr silence counter"))
		return false;
	const uint32_t pending_sequence = reply_state.time_sync_sequence;
	const uint32_t pending_host_baseline =
			reply_state.time_sync_host_baseline_ms;
	const uint32_t pending_round_host = reply_state.time_sync_round_host_ms;
	if (!expect(pending_sequence == 1 && pending_host_baseline != 0 &&
	                    pending_round_host != 0 &&
	                    reply_state.time_sync_client_baseline_ms == 0 &&
	                    reply_state.time_sync_previous_client_ms == 0,
	            "first 0x43 opens retail host baselines for sequence one"))
		return false;
	reply_state.time_sync_unanswered_count = 7;
	(void)opennova::np::dispatch_session_replies(
			ctx.config, ctx.np_protocol.connection_list.front(),
			{opennova::make_protocol_message(
					opennova::c2s::TIME_SYNC_REPLY, {0xAA})},
			world.logic_tick, ctx.np_protocol.connection_list, &world);
	if (!expect(reply_state.time_sync_unanswered_count == 0 &&
	                    reply_state.time_sync_sequence == pending_sequence &&
	                    reply_state.time_sync_host_baseline_ms ==
	                            pending_host_baseline &&
	                    reply_state.time_sync_round_host_ms == pending_round_host &&
	                    reply_state.time_sync_client_baseline_ms == 0 &&
	                    reply_state.time_sync_previous_client_ms == 0,
	            "malformed C2S 0x08 clears silence without advancing its phase"))
		return false;
	std::vector<uint8_t> stale_time_sync;
	for (int shift = 0; shift < 32; shift += 8)
		stale_time_sync.push_back(
				static_cast<uint8_t>((pending_sequence + 1u) >> shift));
	for (int shift = 0; shift < 32; shift += 8)
		stale_time_sync.push_back(static_cast<uint8_t>(999u >> shift));
	reply_state.time_sync_unanswered_count = 8;
	(void)opennova::np::dispatch_session_replies(
			ctx.config, ctx.np_protocol.connection_list.front(),
			{opennova::make_protocol_message(
					opennova::c2s::TIME_SYNC_REPLY, std::move(stale_time_sync))},
			world.logic_tick, ctx.np_protocol.connection_list, &world);
	if (!expect(reply_state.time_sync_unanswered_count == 0 &&
	                    reply_state.time_sync_sequence == pending_sequence &&
	                    reply_state.time_sync_host_baseline_ms ==
	                            pending_host_baseline &&
	                    reply_state.time_sync_round_host_ms == pending_round_host &&
	                    reply_state.time_sync_client_baseline_ms == 0 &&
	                    reply_state.time_sync_previous_client_ms == 0,
	            "sequence-wrong C2S 0x08 clears silence without advancing its phase"))
		return false;
	acknowledge_time_sync(*time_sync, 1000u);
	if (!expect(reply_state.time_sync_client_baseline_ms == 1000u &&
	                    reply_state.time_sync_previous_client_ms == 1000u &&
	                    reply_state.time_sync_round_host_ms == pending_round_host,
	            "first matching C2S 0x08 installs the client baseline and previous sample"))
		return false;
	// A second sequence-correct sample in the same host millisecond moves client
	// time too quickly. It still clears silence, but both 3%-slack checks retain
	// the open round and therefore the next producer must repeat sequence one.
	reply_state.time_sync_unanswered_count = 9;
	acknowledge_time_sync(*time_sync, 2000u);
	if (!expect(reply_state.time_sync_unanswered_count == 0 &&
	                    reply_state.time_sync_sequence == pending_sequence &&
	                    reply_state.time_sync_round_host_ms == pending_round_host &&
	                    reply_state.time_sync_previous_client_ms == 1000u &&
	                    reply_state.time_sync_latest_client_ms == 2000u,
	            "too-fast matching C2S 0x08 clears silence but retains sequence one"))
		return false;

	bool repeat_was_early = false;
	for (uint32_t i = 1; i < opennova::np::CONTROL_REQUEST_PERIOD_TICKS; ++i) {
		opennova::np::Server_TickUpdate(ctx);
		const std::vector<Emitted> messages = drain();
		repeat_was_early = repeat_was_early ||
				find(messages, opennova::s2c::CHARATTR_CRC_CHALLENGE) != nullptr;
	}
	if (!expect(!repeat_was_early,
	            "per-player control countdown waits all 744 eligible ticks"))
		return false;
	opennova::np::Server_TickUpdate(ctx); // eligible tick 744 after reload
	const std::vector<Emitted> second_control = drain();
	charattr = find(second_control, opennova::s2c::CHARATTR_CRC_CHALLENGE);
	model_page = find(
			second_control, opennova::s2c::LOADED_MODEL_PAGE_REQUEST);
	time_sync = find(second_control, opennova::s2c::TIME_SYNC_PING);
	if (!expect(charattr != nullptr && u32le(charattr->body) == retained_seed &&
	                    model_page != nullptr && u32le(model_page->body) == 0 &&
	                    time_sync != nullptr && u32le(time_sync->body) == 1,
	            "second control round retains seed, repeats rejected-fast time 1, and wraps cursor"))
		return false;
	acknowledge_time_sync(*time_sync, 2000u);
	if (!expect(reply_state.time_sync_round_host_ms == 0 &&
	                    reply_state.time_sync_current_host_ms == 0 &&
	                    reply_state.time_sync_previous_client_ms == 0 &&
	                    reply_state.time_sync_latest_client_ms == 0 &&
	                    reply_state.time_sync_host_baseline_ms ==
	                            pending_host_baseline &&
	                    reply_state.time_sync_client_baseline_ms == 1000u,
	            "timely second sample closes only the current time-sync round"))
		return false;

	for (uint32_t i = 0; i < opennova::np::CONTROL_REQUEST_PERIOD_TICKS; ++i) {
		opennova::np::Server_TickUpdate(ctx);
		if (i + 1u < opennova::np::CONTROL_REQUEST_PERIOD_TICKS) (void)drain();
	}
	const std::vector<Emitted> third_control = drain();
	charattr = find(third_control, opennova::s2c::CHARATTR_CRC_CHALLENGE);
	model_page = find(
			third_control, opennova::s2c::LOADED_MODEL_PAGE_REQUEST);
	time_sync = find(third_control, opennova::s2c::TIME_SYNC_PING);
	if (!expect(charattr != nullptr && u32le(charattr->body) == retained_seed &&
	                      model_page != nullptr && u32le(model_page->body) == 50 &&
	                      time_sync != nullptr && u32le(time_sync->body) == 2,
	              "two valid replies advance time sync to 2 while the cursor cycles"))
		return false;
	acknowledge_time_sync(*time_sync, 3000u);

	for (uint32_t i = 0; i < opennova::np::CONTROL_REQUEST_PERIOD_TICKS; ++i) {
		opennova::np::Server_TickUpdate(ctx);
		if (i + 1u < opennova::np::CONTROL_REQUEST_PERIOD_TICKS) (void)drain();
	}
	const std::vector<Emitted> fourth_control = drain();
	time_sync = find(fourth_control, opennova::s2c::TIME_SYNC_PING);
	model_page = find(
			fourth_control, opennova::s2c::LOADED_MODEL_PAGE_REQUEST);
	if (!expect(time_sync != nullptr && u32le(time_sync->body) == 2 &&
	                    model_page != nullptr && u32le(model_page->body) == 0,
	            "time-sync requests preserve retail's 1,1,2,2 sequence"))
		return false;

	ctx.connection_mode = opennova::np::ConnectionMode::HostOnly;
	for (uint32_t i = 0; i < opennova::np::CONTROL_REQUEST_PERIOD_TICKS; ++i) {
		opennova::np::Server_TickUpdate(ctx);
		if (i + 1u < opennova::np::CONTROL_REQUEST_PERIOD_TICKS) (void)drain();
	}
	const std::vector<Emitted> dedicated_control = drain();
	if (!expect(find(dedicated_control,
	                    opennova::s2c::CHARATTR_CRC_CHALLENGE) != nullptr &&
	                    find(dedicated_control,
	                            opennova::s2c::LOADED_MODEL_PAGE_REQUEST) == nullptr,
	            "dedicated hosts omit only the renderer-backed loaded-model request"))
		return false;
	ctx.connection_mode = opennova::np::ConnectionMode::HostClient;

	// After maturity, deploy/death presentation does not reset or pause the
	// quartet countdown: UpdateAll checks its slot blockers and the saved age,
	// not entity health or the respawn-pending bit. Age itself pauses while hidden.
	const uint32_t mature_age =
			ctx.np_protocol.connection_list.front().reply.control_live_ticks;
	ctx.np_protocol.connection_list.front().reply.control_request_countdown = 1;
	ctx.np_protocol.connection_list.front().link.respawn_pending = true;
	player_entity->alive = false;
	player_entity->health = 0;
	player_entity->engine_flags |= opennova::world::kEntityFlagDead;
	ctx.scoreboard_broadcast_timer = 0x136u;
	opennova::np::Server_TickUpdate(ctx);
	const std::vector<Emitted> deploy_pending = drain();
	const bool pending_entity_crc = find(
			deploy_pending, opennova::s2c::ENTITY_CHECKSUM_REQ) != nullptr;
	const bool pending_loadout_crc = find(
			deploy_pending, opennova::s2c::LOADOUT_CRC_REQ) != nullptr;
	if (!expect(!pending_entity_crc && !pending_loadout_crc &&
	                    find(deploy_pending, opennova::s2c::PLAYER_LIST) != nullptr &&
	                    find(deploy_pending,
	                            opennova::s2c::CHARATTR_CRC_CHALLENGE) != nullptr &&
	                    find(deploy_pending, opennova::s2c::INPUT_STATE_FLAGS) != nullptr &&
	                    find(deploy_pending, opennova::s2c::TIME_SYNC_PING) != nullptr &&
	                    find(deploy_pending,
	                            opennova::s2c::LOADED_MODEL_PAGE_REQUEST) != nullptr &&
	                    ctx.np_protocol.connection_list.front().reply.control_live_ticks ==
	                            mature_age,
	            "mature countdown continues through dead/deploy pending while age pauses"))
		return false;
	ctx.np_protocol.connection_list.front().link.respawn_pending = false;
	player_entity->alive = true;
	player_entity->health = 100;
	player_entity->engine_flags &= ~opennova::world::kEntityFlagDead;

	// The retail 00TRg oracle's first S2C 0x6E occurs in the exact datagram that
	// kills the joiner's own entity (0x0035), then repeats once per second while
	// the deploy/death screen is held. AI deaths before it do not trigger 0x6E.
	// Pin the same recipient predicate and the no-wave `[u8 0]` body.
	opennova::world::Entity *dead_player = world.registry.get(player);
	dead_player->health = 0;
	// Advance to the next one-second service boundary (the shared countdown,
	// not a frame-count phase); the intervening frames carry no 0x6E.
	do {
		opennova::np::Server_TickUpdate(ctx);
	} while (!world.match.periodic_second());
	const std::vector<Emitted> death_screen = drain();
	const Emitted *wave_status = find(death_screen, opennova::s2c::SPAWN_WAVE_STATUS);
	return expect(wave_status != nullptr &&
	                      wave_status->body == std::vector<uint8_t>({0x00}),
	              "dead joiner receives exact empty spawn-wave status at 1 Hz");
}

// Numbered spawn points are wave-backed by default. Queue joins receive an
// exact requester-specific transient 0x6E immediately; the 1 Hz owner tick
// releases the head through the shared deploy transaction and publishes the
// remaining member's positional countdown.
// [orig: SpawnWaveList_TryQueuePlayer @0x52A490;
// SpawnWaveList_TickEntry @0x52A330; NetPacket_WriteSpawnWaveStatus @0x507490]
bool check_spawn_wave_queue_and_release_wire() {
	opennova::world::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(2, 8);
	auto make_player = [&]() {
		opennova::world::Entity entity;
		entity.kind = opennova::world::EntityKind::Organic;
		entity.team = 1;
		entity.alive = false;
		entity.health = 0;
		entity.health_max = 100;
		return world.registry.spawn(0, entity);
	};
	const opennova::world::EntityHandle first = make_player();
	const opennova::world::EntityHandle second = make_player();
	opennova::world::Entity zone;
	zone.kind = opennova::world::EntityKind::Item;
	zone.team = 1;
	zone.alive = true;
	zone.is_spawn_point = true;
	zone.zone_number = 1;
	zone.zone_control = 0x10000;
	const opennova::world::EntityHandle zone_handle =
			world.registry.spawn(2, zone);
	world.spawn_waves.build_from_mission(world, 0, 2);

	opennova::np::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.world = &world;
	ctx.config.game_type = opennova::game_type::kTeamDeathmatch;
	opennova::world::MatchRules rules;
	rules.game_type = ctx.config.game_type;
	world.match.configure(rules);
	opennova::netsim::UdpSessionTransport first_transport(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::netsim::UdpSessionTransport second_transport(
			opennova::netsim::UdpSessionTransport::Role::Host);
	ctx.np_protocol.connection_list.resize(2);
	for (size_t i = 0; i < 2; ++i) {
		auto &conn = ctx.np_protocol.connection_list[i];
		conn.type = 1;
		conn.phase = opennova::np::ConnectionPhase::InMatch;
		conn.burst.spawned = true;
		conn.link.mode = opennova::netsim::TransportMode::Client;
		conn.link.transport = i == 0 ? &first_transport : &second_transport;
		conn.link.owned_entity = i == 0 ? first : second;
		conn.link.respawn_pending = true;
	}
	const std::vector<uint8_t> pick{
			static_cast<uint8_t>(zone_handle.packed),
			static_cast<uint8_t>(zone_handle.packed >> 8)};
	auto queue = [&](size_t index) {
		return opennova::np::dispatch_session_replies(
				ctx.config, ctx.np_protocol.connection_list[index],
				{opennova::make_protocol_message(
						opennova::c2s::RESPAWN_REQUEST, pick)},
				0, ctx.np_protocol.connection_list, &world);
	};
	const std::vector<opennova::ProtocolMessage> first_join = queue(0);
	const std::vector<opennova::ProtocolMessage> second_join = queue(1);
	if (!expect(first_join.size() == 1 && second_join.size() == 1 &&
	                    first_join[0].tag == opennova::s2c::SPAWN_WAVE_STATUS &&
	                    second_join[0].tag == opennova::s2c::SPAWN_WAVE_STATUS &&
	                    !first_join[0].reliable && !second_join[0].reliable,
	            "wave queue joins emit one transient requester-local 0x6E"))
		return false;
	opennova::SpawnWaveStatus first_status;
	opennova::SpawnWaveStatus second_status;
	if (!expect(opennova::decode_spawn_wave_status(
	                    first_join[0].payload.data(), first_join[0].payload.size(),
	                    first_status) &&
	                    opennova::decode_spawn_wave_status(
	                            second_join[0].payload.data(),
	                            second_join[0].payload.size(), second_status) &&
	                    first_status.groups.size() == 1 &&
	                    first_status.groups[0].zone_handle == zone_handle.packed &&
	                    first_status.groups[0].zone_index == 0 &&
	                    first_status.groups[0].queued_count == 1 &&
	                    first_status.groups[0].wave_countdown == 0 &&
	                    second_status.groups[0].queued_count == 2 &&
	                    second_status.groups[0].wave_countdown == 2,
	            "queue-join 0x6E carries zone index, roster and positional ETA"))
		return false;
	if (!expect(queue(0).empty() &&
	                    world.spawn_waves.entries()[0].queued.size() == 2,
	            "duplicate wave pick waits without duplicate row or deployment"))
		return false;

	do {
		opennova::np::Server_TickUpdate(ctx);
	} while (!world.match.periodic_second());
	if (!expect(world.registry.get(first)->health == 100 &&
	                    !ctx.np_protocol.connection_list[0].link.respawn_pending &&
	                    world.registry.get(second)->health == 0 &&
	                    ctx.np_protocol.connection_list[1].link.respawn_pending,
	            "first 1 Hz wave boundary releases only the queue head"))
		return false;
	std::vector<opennova::netsim::Datagram> first_out;
	std::vector<opennova::netsim::Datagram> second_out;
	opennova::netsim::Datagram datagram;
	while (first_transport.pop_outbound(datagram))
		first_out.push_back(std::move(datagram));
	while (second_transport.pop_outbound(datagram))
		second_out.push_back(std::move(datagram));
	auto find = [](const auto &messages, uint8_t tag)
			-> const opennova::netsim::Datagram * {
		for (const auto &message : messages)
			if (message.tag == tag) return &message;
		return nullptr;
	};
	const auto *loadout = find(first_out, opennova::s2c::WEAPON_LOADOUT);
	const auto *seed = find(first_out, opennova::s2c::TICK_SEED);
	const auto *remaining = find(second_out, opennova::s2c::SPAWN_WAVE_STATUS);
	if (!expect(loadout != nullptr && seed != nullptr && remaining != nullptr &&
	                    !remaining->reliable,
	            "wave release stages the shared deploy bundle and periodic 0x6E"))
		return false;
	opennova::SpawnWaveStatus remaining_status;
	return expect(opennova::decode_spawn_wave_status(
	                      remaining->body.data(), remaining->body.size(),
	                      remaining_status) &&
	                      remaining_status.groups.size() == 1 &&
	                      remaining_status.groups[0].queued_count == 1 &&
	                      remaining_status.groups[0].wave_countdown == 2 &&
	                      remaining_status.groups[0].members ==
	                              std::vector<uint16_t>{second.packed},
	              "post-release 0x6E carries the remaining member at interval ETA");
}

// Retail's cfg key `nodefaultspawnpoints` is backed by a misleadingly named
// global/helper pair. It does not wait for a whole team to die: a target-less
// 0x0E is denied while the requester's team has an unnumbered zone or a fully
// controlled numbered zone. A resolved target bypasses this one gate.
// [orig: Server_ProcessClientRequestRespawn @0x519AF0;
//  Entity_HasAliveEntityOfTeam @0x4FC7B0]
bool check_default_spawn_requires_no_team_zone() {
	opennova::world::World world;
	world.registry.configure_pool(0, 4);
	world.registry.configure_pool(2, 4);
	world.registry.configure_pool(3, 4);

	opennova::world::Entity player_seed;
	player_seed.kind = opennova::world::EntityKind::Organic;
	player_seed.flags = opennova::world::kEntityFlagPlayer;
	player_seed.engine_flags = opennova::world::kEntityFlagDead;
	player_seed.team = 1;
	player_seed.alive = false;
	player_seed.health = 0;
	player_seed.health_max = 100;
	const opennova::world::EntityHandle player =
			world.registry.spawn(0, player_seed);

	opennova::world::Entity marker;
	marker.kind = opennova::world::EntityKind::Marker;
	marker.item_id = 6003;
	marker.position = {20.0f, 0.0f, 0.0f};
	world.registry.spawn(3, marker);

	opennova::world::Entity zone_seed;
	zone_seed.kind = opennova::world::EntityKind::Item;
	zone_seed.team = 1;
	zone_seed.alive = true;
	zone_seed.is_spawn_point = true;
	zone_seed.zone_number = 0;
	zone_seed.zone_control = 0;
	const opennova::world::EntityHandle zone =
			world.registry.spawn(2, zone_seed);

	opennova::np::GameConfig config;
	config.game_type = opennova::game_type::kTeamDeathmatch;
	config.default_spawn_requires_no_team_zone = 1;
	std::vector<opennova::np::NapiNPConnection> roster(1);
	auto &conn = roster.front();
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.owned_entity = player;
	conn.reply.player_slot = 0;

	const std::vector<opennova::ProtocolMessage> default_pick{
			opennova::make_protocol_message(
					opennova::c2s::RESPAWN_REQUEST, {0xFF, 0xFF})};
	auto reset_player = [&]() {
		opennova::world::Entity *entity = world.registry.get(player);
		entity->alive = false;
		entity->health = 0;
		entity->engine_flags |= opennova::world::kEntityFlagDead;
		conn.link.respawn_pending = false;
		conn.link.respawn_delay_seconds = 0;
		conn.link.spawn_target_hold_seconds = 0;
	};
	auto dispatch = [&](const std::vector<opennova::ProtocolMessage> &messages) {
		return opennova::np::dispatch_session_replies(
				config, conn, messages, 100, roster, &world);
	};

	reset_player();
	if (!expect(dispatch(default_pick).empty() &&
	                    world.registry.get(player)->health == 0,
	            "an unnumbered team zone denies the restricted Default Spawn"))
		return false;

	reset_player();
	const std::vector<opennova::ProtocolMessage> explicit_pick{
			opennova::make_protocol_message(
					opennova::c2s::RESPAWN_REQUEST,
					{static_cast<uint8_t>(zone.packed),
					 static_cast<uint8_t>(zone.packed >> 8)})};
	if (!expect(!dispatch(explicit_pick).empty() &&
	                    world.registry.get(player)->health == 100,
	            "a resolved spawn target bypasses the target-less restriction"))
		return false;

	opennova::world::Entity *zone_entity = world.registry.get(zone);
	zone_entity->zone_number = 1;
	zone_entity->zone_control = 0xFFFF;
	reset_player();
	if (!expect(!dispatch(default_pick).empty() &&
	                    world.registry.get(player)->health == 100,
	            "a partially controlled numbered zone does not deny Default Spawn"))
		return false;

	zone_entity->zone_control = 0x10000;
	reset_player();
	if (!expect(dispatch(default_pick).empty() &&
	                    world.registry.get(player)->health == 0,
	            "a fully controlled numbered team zone denies Default Spawn"))
		return false;

	config.default_spawn_requires_no_team_zone = 0;
	reset_player();
	return expect(!dispatch(default_pick).empty() &&
	                      world.registry.get(player)->health == 100,
	              "the retail-default disabled rule permits Default Spawn");
}

// A spawnable type-1 item is a mobile deploy target. The request handler first
// requires a live target and a free best seat; Server_ProcessPlayerDeath repeats
// the search, requires the target's +0x170 primary-occupant latch, resets the
// player, then requests the authoritative attach.
// [orig: Server_ProcessClientRequestRespawn @0x519AF0;
//  Server_ProcessPlayerDeath @0x517740;
//  Entity_FindBestSeatSlot @0x4351F0]
bool check_vehicle_spawn_target_deploys_into_best_seat() {
	opennova::world::World world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 4);

	opennova::world::Entity vehicle_seed;
	vehicle_seed.kind = opennova::world::EntityKind::Item;
	vehicle_seed.has_item_def = true;
	vehicle_seed.item_type = 1;
	vehicle_seed.is_spawn_point = true;
	vehicle_seed.team = 1;
	vehicle_seed.alive = true;
	vehicle_seed.health = 500;
	vehicle_seed.health_max = 500;
	vehicle_seed.position = {30.0f, 40.0f, 5.0f};
	opennova::world::Seat controller;
	controller.type = opennova::world::SeatType::Controller;
	controller.bone_index = 4;
	vehicle_seed.seats.push_back(controller);
	opennova::world::Seat passenger;
	passenger.type = opennova::world::SeatType::Passenger;
	passenger.bone_index = 7;
	passenger.seat_local = {2.0f, 0.0f, 1.0f};
	vehicle_seed.seats.push_back(passenger);
	const opennova::world::EntityHandle vehicle =
			world.registry.spawn(1, vehicle_seed);

	opennova::world::Entity driver_seed;
	driver_seed.kind = opennova::world::EntityKind::Organic;
	driver_seed.team = 1;
	driver_seed.alive = true;
	driver_seed.health = 100;
	const opennova::world::EntityHandle driver =
			world.registry.spawn(0, driver_seed);
	if (!expect(opennova::world::entity_process_vehicle_attach(
				world, driver, vehicle, 4),
			"fixture driver claims the mobile spawn vehicle"))
		return false;

	opennova::world::Entity player_seed;
	player_seed.kind = opennova::world::EntityKind::Organic;
	player_seed.flags = opennova::world::kEntityFlagPlayer |
			opennova::world::kEntityFlagDead;
	player_seed.engine_flags = player_seed.flags;
	player_seed.team = 1;
	player_seed.alive = false;
	player_seed.health = 0;
	player_seed.health_max = 125;
	player_seed.position = {-10.0f, -20.0f, 0.0f};
	const opennova::world::EntityHandle player =
			world.registry.spawn(0, player_seed);

	opennova::np::GameConfig config;
	config.game_type = opennova::game_type::kTeamDeathmatch;
	std::vector<opennova::np::NapiNPConnection> roster(1);
	auto &conn = roster.front();
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.owned_entity = player;
	conn.link.respawn_pending = true;
	conn.reply.player_slot = 1;
	const std::vector<opennova::ProtocolMessage> pick{
		opennova::make_protocol_message(
			opennova::c2s::RESPAWN_REQUEST,
			{static_cast<uint8_t>(vehicle.packed),
			 static_cast<uint8_t>(vehicle.packed >> 8)})};
	auto dispatch = [&]() {
		return opennova::np::dispatch_session_replies(
				config, conn, pick, 100, roster, &world);
	};
	auto reset_player = [&]() {
		opennova::world::Entity *entity = world.registry.get(player);
		if (entity->mounted)
			opennova::world::entity_detach_from_vehicle(world, player);
		entity->alive = false;
		entity->health = 0;
		entity->flags |= opennova::world::kEntityFlagDead;
		entity->engine_flags |= opennova::world::kEntityFlagDead;
		entity->position = {-10.0f, -20.0f, 0.0f};
		conn.link.respawn_pending = true;
		conn.link.respawn_delay_seconds = 0;
		conn.link.spawn_target_hold_seconds = 0;
	};

	// The first-stage request accepts the free seat, but the release leg stops
	// before any player mutation when no primary occupant powers the vehicle.
	world.registry.get(vehicle)->primary_occupant = {};
	if (!expect(dispatch().empty() &&
				world.registry.get(player)->health == 0 &&
				world.registry.get(player)->position.x == -10.0f,
			"mobile spawn release requires the primary-occupant latch"))
		return false;
	world.registry.get(vehicle)->primary_occupant = driver;

	// The request-time best-seat gate is independent of the release latch.
	world.registry.get(vehicle)->seats[1].occupant = driver;
	reset_player();
	if (!expect(dispatch().empty() && world.registry.get(player)->health == 0,
			"mobile spawn request rejects a vehicle with no free seat"))
		return false;
	world.registry.get(vehicle)->seats[1].occupant = {};

	world.registry.get(vehicle)->flags |= opennova::world::kEntityFlagDead;
	reset_player();
	if (!expect(dispatch().empty() && world.registry.get(player)->health == 0,
			"mobile spawn request rejects a dead vehicle"))
		return false;
	world.registry.get(vehicle)->flags &= ~opennova::world::kEntityFlagDead;

	reset_player();
	const std::vector<opennova::ProtocolMessage> deployed = dispatch();
	const opennova::world::Entity *live = world.registry.get(player);
	return expect(deployed.size() >= 2 && live->alive && live->health == 125 &&
				!conn.link.respawn_pending && live->mounted &&
				live->mount_target == vehicle && live->mount_seat == 1 &&
				world.registry.get(vehicle)->seats[1].occupant == player,
			"mobile spawn resets then boards the best free vehicle seat");
}

// Retail answers the world-state-load burst's empty C2S 0x2D member with a
// requester-only S2C 0x58. Its body is built from the live session report, not
// a captured blob: game/mission names, game type, player cap, session uptime,
// score.ini's 39 score values, and the mode-specific option pairs.
// [orig: NapiNPServerMsg_0x02D @0x502430 -> Server_BuildStatusReport @0x530A60
// -> SessionStatus_SerializeToBuffer @0x5310C0]
bool check_session_status_reply_matches_retail_writer() {
	opennova::np::GameConfig config;
	config.server_name = "Untitled ";
	config.mission_name = "Training: Grenade Launcher";
	config.game_type = 0x10020u;
	config.max_players = 4;
	config.respawn_time = 30;

	std::vector<opennova::np::NapiNPConnection> roster(2);
	roster[0].type = 2;
	roster[0].phase = opennova::np::ConnectionPhase::InMatch;
	roster[0].burst.spawned = true;
	roster[1].type = 1;
	roster[1].phase = opennova::np::ConnectionPhase::InMatch;
	roster[1].burst.spawned = true;

	opennova::np::ServerDispatchInputs inputs;
	inputs.session_uptime_ms = 111844u;
	std::vector<opennova::ProtocolMessage> replies =
			opennova::np::dispatch_session_replies(
					config, roster[1],
					{opennova::make_protocol_message(
							opennova::c2s::BURST_MEMBER_2D, {})},
					17u, roster, nullptr, inputs);
	if (!expect(replies.size() == 1 &&
	                    replies.front().tag == opennova::s2c::SESSION_STATUS,
	            "C2S 0x2D receives one requester-only S2C 0x58"))
		return false;
	if (!expect(replies.front().payload.size() == 216,
	            "00TRg session-status body has retail's exact 216-byte width"))
		return false;

	opennova::SessionStatusBlock decoded;
	if (!expect(opennova::decode_session_status(
	                    replies.front().payload.data(),
	                    replies.front().payload.size(), decoded),
	            "generated session-status body decodes"))
		return false;
	if (!expect(decoded.server_name == "Untitled " &&
	                    decoded.mission_name == "Training: Grenade Launcher" &&
	                    decoded.byte0 == 0x20 && decoded.byte1 == 0 &&
	                    decoded.byte2 == 4 && decoded.uptime_ms == 111844u,
	            "session-status identity/type/cap/uptime match the retail report"))
		return false;
	if (!expect(decoded.stat_values[3] == 5 &&
	                    decoded.stat_values[6] == 1 &&
	                    decoded.stat_values[7] == 2 &&
	                    decoded.stat_values[15] == 12 &&
	                    decoded.stat_values[16] == 10 &&
	                    decoded.stat_values[17] == 5 &&
	                    decoded.stat_values[18] == 1 &&
	                    decoded.stat_values[37] == 5,
	            "session-status score values come from the COOP score table"))
		return false;
	if (!expect(decoded.kv_count == 2 && decoded.kv.size() == 2 &&
	                    decoded.kv[0].key == 9 && decoded.kv[0].value == 2 &&
	                    decoded.kv[1].key == 8 && decoded.kv[1].value == 30 &&
	                    decoded.trailing_bytes == 5,
	            "session-status carries player-count/respawn pairs and writer sentinel"))
		return false;
	return expect(std::all_of(replies.front().payload.end() - 5,
	                          replies.front().payload.end(),
	                          [](uint8_t b) { return b == 0; }),
	              "retail writer's extra zero key/value sentinel is preserved");
}

bool check_objective_mode_session_status_options() {
	opennova::world::World world;
	world.registry.configure_pool(1, 16);
	auto objective = [&](int item_id, uint8_t team, uint32_t attrib = 0) {
		opennova::world::Entity entity;
		entity.kind = opennova::world::EntityKind::Item;
		entity.item_id = item_id;
		entity.team = team;
		entity.item_attrib = attrib;
		return world.registry.spawn(1, entity);
	};
	objective(4093, 2);
	objective(4093, 2);
	objective(4091, 1);

	opennova::np::GameConfig config;
	config.game_type = opennova::game_type::kCaptureTheFlag;
	opennova::world::MatchRules rules;
	rules.game_type = config.game_type;
	world.match.configure(rules);
	opennova::SessionStatusBlock decoded;
	std::vector<uint8_t> body = opennova::np::serialize_session_status(
			config, 0, 0, &world);
	if (!expect(opennova::decode_session_status(
				body.data(), body.size(), decoded) && decoded.kv.size() == 2 &&
				decoded.kv[0].key == 7 && decoded.kv[0].value == 2 &&
				decoded.kv[1].key == 6 && decoded.kv[1].value == 1,
			"CTF status publishes red/blue authored flag targets as keys 7/6"))
		return false;

	objective(7001, 1, opennova::world::kItemAttribObjectiveTarget);
	objective(7002, 1, opennova::world::kItemAttribObjectiveTarget);
	objective(7003, 2, opennova::world::kItemAttribObjectiveTarget);
	for (const uint32_t game_type : {
			opennova::game_type::kSearchAndDestroy,
			opennova::game_type::kAttackDefend}) {
		config.game_type = game_type;
		rules.game_type = game_type;
		world.match.configure(rules);
		body = opennova::np::serialize_session_status(config, 0, 0, &world);
		decoded = {};
		if (!expect(opennova::decode_session_status(
					body.data(), body.size(), decoded) && decoded.kv.size() == 2 &&
					decoded.kv[0].key == 3 && decoded.kv[0].value == 2 &&
					decoded.kv[1].key == 4 && decoded.kv[1].value == 1,
				"S&D/A&D status publishes the defending-side target census as keys 3/4"))
			return false;
	}

	for (const uint32_t game_type : {
			opennova::game_type::kFlagBall,
			opennova::game_type::kFlagMe}) {
		config.game_type = game_type;
		config.max_score = 0;
		rules.game_type = game_type;
		world.match.configure(rules);
		body = opennova::np::serialize_session_status(config, 0, 0, &world);
		decoded = {};
		if (!expect(opennova::decode_session_status(
					body.data(), body.size(), decoded) && decoded.kv.size() == 1 &&
					decoded.kv[0].key == 5 && decoded.kv[0].value == 0,
				"FlagBall/Flag Me status always publishes MaxScore as key 5"))
			return false;
	}
	return true;
}

bool check_score_ini_drives_session_status_values() {
	opennova::np::GameConfig config;
	config.game_type = 0x10020u;
	const std::string score_ini =
			"VERSION 40\n"
			"GAMETYPE \"COOP\"\n"
			"FIELD \"NUMENEMYKILLS\" 1\n"
			"FIELD \"NUMFRIENDLYKILLS\" 0\n"
			"FIELD \"NUMLFPTAKEOVERS\" 1\n"
			"VAR \"FIRE\" 7\n"
			"VAR \"ENEMYKILL\" 5\n"
			"VAR \"VATTACHKILL\" -3\n";
	if (!expect(opennova::np::load_session_score_config(config, score_ini),
	            "score.ini VERSION 40 loads for the current game type"))
		return false;
	if (!expect(config.session_status_stat_values.has_value() &&
	                    (*config.session_status_stat_values)[0] == 7 &&
	                    (*config.session_status_stat_values)[3] == 5 &&
	                    (*config.session_status_stat_values)[6] == 1 &&
	                    (*config.session_status_stat_values)[37] == -3 &&
	                    (*config.session_status_stat_values)[38] == 0,
	            "score.ini overlays the witnessed mode-default 39-value table"))
		return false;
	if (!expect(config.scoreboard_fields.size() == 3 &&
	                    config.scoreboard_fields[0].first == 3 &&
	                    config.scoreboard_fields[0].second == 1 &&
	                    config.scoreboard_fields[1].first == 2 &&
	                    config.scoreboard_fields[1].second == 0 &&
	                    config.scoreboard_fields[2].first == 32 &&
	                    config.scoreboard_fields[2].second == 1,
	            "score.ini FIELD rows replace the default board schema in file order"))
		return false;
	const auto before = config.session_status_stat_values;
	const auto fields_before = config.scoreboard_fields;
	if (!expect(!opennova::np::load_session_score_config(
	                      config, "VERSION 39\nGAMETYPE \"COOP\"\nVAR \"FIRE\" 99\n") &&
	                      config.session_status_stat_values == before &&
	                      config.scoreboard_fields == fields_before,
	              "wrong score.ini version fails closed without mutating live rules"))
		return false;

	opennova::np::GameConfig flag_me;
	flag_me.game_type = opennova::game_type::kFlagMe;
	return expect(!opennova::np::load_session_score_config(flag_me, score_ini) &&
	                      !flag_me.session_status_stat_values.has_value() &&
	                      flag_me.scoreboard_fields.empty(),
	              "Flag Me's out-of-range retail score row cannot inherit COOP score.ini");
}

// The host carries the persistent unnumbered capture transaction all the way to
// retail bodies: 0x50 ownership changes, 0x53 start/progress/completion, 0x6C
// unique presence changes, and 0x1E 41/43 start/completion events. Numbered
// instant flips deliberately have no synthetic 0x53. [orig: the CaptureCtx_* /
// Server_UpdateCaptureZones run from CaptureCtx_RemoveQueueEntries @0x53B340 to 0x53B8F0; Server_ChangeEntityTeam
// @0x518D70; NetPacket writers @0x506AD0/@0x506D00/@0x506DE0]
bool check_timed_capture_host_wire_transaction() {
	opennova::np::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.config.game_type = opennova::game_type::kAdvanceAndSecure;
	opennova::world::World world;
	opennova::world::CollisionWorld collision;
	opennova::world::AiSystem ai;
	world.mp_session = true;
	world.collision = &collision;
	world.ai = &ai;
	ai.collision = &collision;
	world.add_system(&ai);
	ctx.world = &world;
	world.registry.configure_pool(0, 8);
	world.registry.configure_pool(1, 8);

	opennova::world::MatchRules rules;
	rules.game_type = opennova::game_type::kAdvanceAndSecure;
	rules.capture_duration_seconds = 3;
	world.match.configure(rules);

	opennova::world::Entity zone;
	zone.kind = opennova::world::EntityKind::Item;
	zone.is_capture_trigger = true;
	zone.is_spawn_point = true;
	zone.zone_radius = 70;
	zone.team = 2;
	zone.health = 1;
	zone.alive = true;
	zone.position = {20.0f, 30.0f, 4.0f};
	zone.yaw = 90; // mission yaw 90 is identity collision placement
	const auto zone_handle = world.registry.spawn(1, zone);

	// The transaction starts only when the authority player-body resolver
	// intersects an authored type-10 Change Team Box. This is intentionally not
	// derived from zone_radius: the latter belongs to the separate 1 Hz
	// secure/proximity scorers. [orig: Entity_ComputeBoneCollisionForce
	// @0x4AE150 type dispatch @0x4AEB7B; player-body resolver callback
	// @0x4B31DD..0x4B3238]
	auto capture_box = [] {
		opennova::world::CollisionModel model;
		auto plane = [&](int nx, int ny, int nz, float distance) {
			opennova::world::CollisionPlane value;
			value.nx = static_cast<int16_t>(nx);
			value.ny = static_cast<int16_t>(ny);
			value.nz = static_cast<int16_t>(nz);
			value.dist = static_cast<int32_t>(distance * 65536.0f);
			model.planes.push_back(value);
		};
		plane(16384, 0, 0, -70.0f);
		plane(-16384, 0, 0, -70.0f);
		plane(0, 16384, 0, -70.0f);
		plane(0, -16384, 0, -70.0f);
		plane(0, 0, 16384, -12.0f);
		plane(0, 0, -16384, 0.0f);

		opennova::world::CollisionVolume volume;
		volume.type = opennova::world::bvol_type::kChangeTeamCT;
		volume.min_x = volume.min_y = -70 * 65536;
		volume.max_x = volume.max_y = 70 * 65536;
		volume.min_z = 0;
		volume.max_z = 12 * 65536;
		volume.plane_count = 6;
		model.volumes.push_back(volume);

		opennova::world::CollisionSection section;
		section.volume_count = 1;
		model.sections.push_back(section);
		return model;
	};
	const int capture_model = collision.add_model(capture_box());
	collision.assign_entity(zone_handle, capture_model);

	auto soldier = [&](uint8_t team) {
		opennova::world::Entity entity;
		entity.kind = opennova::world::EntityKind::Organic;
		entity.player_class = 8;
		entity.team = team;
		entity.health = 150;
		entity.alive = true;
		entity.position = zone.position;
		const auto handle = world.registry.spawn(0, entity);
		opennova::world::AiEntity *body = ai.at(ai.attach(handle));
		body->inf.active = true;
		body->net_is_remote_peer = true;
		body->health = 150;
		body->team = team;
		body->pos[0] = 20 * 65536;
		body->pos[1] = 30 * 65536;
		body->pos[2] = 4 * 65536;
		return handle;
	};
	const auto first = soldier(1);
	world.match.upsert_player({first, 0, "Blue", {}});

	opennova::netsim::UdpSessionTransport transport(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.link.transport = &transport;
	conn.link.owned_entity = first;
	ctx.np_protocol.connection_list.push_back(std::move(conn));

	using Record = std::pair<uint8_t, std::vector<uint8_t>>;
	auto tick_second = [&]() {
		for (int i = 0; i < 62; ++i)
			opennova::np::Server_TickUpdate(ctx);
		std::vector<Record> records;
		std::vector<uint8_t> raw;
		while (transport.pop_outbound(raw)) {
			if (raw.empty()) continue;
			records.emplace_back(
					raw.front(), std::vector<uint8_t>(raw.begin() + 1, raw.end()));
		}
		return records;
	};
	auto bodies = [](const std::vector<Record> &records, uint8_t tag) {
		std::vector<std::vector<uint8_t>> out;
		for (const auto &record : records)
			if (record.first == tag) out.push_back(record.second);
		return out;
	};
	auto record_index = [](const std::vector<Record> &records, uint8_t tag,
	                       const std::vector<uint8_t> &body) {
		for (size_t i = 0; i < records.size(); ++i)
			if (records[i].first == tag && records[i].second == body) return i;
		return records.size();
	};

	// Consume the first-frame service so every 62-tick window below ends on
	// its one-second boundary, after the contact stream has been drained.
	opennova::np::Server_TickUpdate(ctx);
	const auto started = tick_second();
	const auto start_50 = bodies(started, opennova::s2c::TEAM_ASSIGN);
	const auto start_53 = bodies(started, opennova::s2c::ZONE_TIMER_WINDOW);
	const auto start_events = bodies(started, 0x1E);
	const std::vector<uint8_t> expected_start_50 = {
			static_cast<uint8_t>(zone_handle.packed),
			static_cast<uint8_t>(zone_handle.packed >> 8),
			0, 0, 0, 0};
	const std::vector<uint8_t> expected_start = {
			static_cast<uint8_t>(zone_handle.packed),
			static_cast<uint8_t>(zone_handle.packed >> 8),
			0, 1, 0, 0, 3, 0, 1};
	const std::vector<uint8_t> expected_start_event = {
			41, 0, 0xFF, 0xFF, 0, 0, 0, 0};
	if (!expect(start_50.size() == 1 && start_50[0] == expected_start_50 &&
	                    start_53.size() == 1 && start_53[0] == expected_start &&
	                    std::find(start_events.begin(), start_events.end(),
	                              expected_start_event) !=
	                            start_events.end() &&
	                    record_index(started, opennova::s2c::TEAM_ASSIGN,
	                                 expected_start_50) <
	                            record_index(started,
	                                         opennova::s2c::ZONE_TIMER_WINDOW,
	                                         expected_start) &&
	                    record_index(started,
	                                 opennova::s2c::ZONE_TIMER_WINDOW,
	                                 expected_start) <
	                            record_index(started, 0x1E,
	                                         expected_start_event) &&
	                    world.registry.get(zone_handle)->team == 0,
	            "timed capture starts with ordered 0x50/0x53/event-41 and neutral owner"))
		return false;

	const auto second = soldier(1);
	const auto advanced = tick_second();
	const auto advance_53 = bodies(advanced, opennova::s2c::ZONE_TIMER_WINDOW);
	const auto advance_6c = bodies(advanced, opennova::s2c::ZONE_PRESENCE_COUNT);
	const std::vector<uint8_t> expected_6c = {
			static_cast<uint8_t>(zone_handle.packed),
			static_cast<uint8_t>(zone_handle.packed >> 8), 2};
	if (!expect(advance_53.size() == 1 && advance_53[0].size() == 9 &&
	                    advance_53[0][4] == 2 && advance_53[0][8] == 2 &&
	                    advance_6c.size() == 1 && advance_6c[0] == expected_6c,
	            "two unique contacts emit exact 0x6C and advance 0x53 by rate two"))
		return false;

	world.registry.get(second)->position = {500.0f, 500.0f, 0.0f};
	opennova::world::AiEntity *second_body = ai.for_handle(second);
	second_body->pos[0] = 500 * 65536;
	second_body->pos[1] = 500 * 65536;
	second_body->pos[2] = 0;
	const auto completed = tick_second();
	const auto complete_50 = bodies(completed, opennova::s2c::TEAM_ASSIGN);
	const auto complete_53 = bodies(completed, opennova::s2c::ZONE_TIMER_WINDOW);
	const auto complete_6c = bodies(completed, opennova::s2c::ZONE_PRESENCE_COUNT);
	const auto complete_events = bodies(completed, 0x1E);
	const std::vector<uint8_t> expected_complete_50 = {
			static_cast<uint8_t>(zone_handle.packed),
			static_cast<uint8_t>(zone_handle.packed >> 8),
			1, 0, 0, 0};
	const std::vector<uint8_t> expected_complete_event = {
			43, 0, 0xFF, 0xFF, 0, 0, 0, 0};
	const auto *scorer = world.match.player(first);
	return expect(complete_50.size() == 1 &&
	                      complete_50[0] == expected_complete_50 &&
	                      complete_53.size() == 1 && complete_53[0][4] == 3 &&
	                      complete_6c.size() == 1 && complete_6c[0][2] == 1 &&
	                      std::find(complete_events.begin(), complete_events.end(),
	                                expected_complete_event) !=
	                              complete_events.end() &&
	                      record_index(completed,
	                                   opennova::s2c::ZONE_TIMER_WINDOW,
	                                   complete_53[0]) <
	                              record_index(completed,
	                                           opennova::s2c::TEAM_ASSIGN,
	                                           expected_complete_50) &&
	                      record_index(completed,
	                                   opennova::s2c::TEAM_ASSIGN,
	                                   expected_complete_50) <
	                              record_index(completed, 0x1E,
	                                           expected_complete_event) &&
	                      world.registry.get(zone_handle)->team == 1 &&
	                      scorer != nullptr &&
	                      scorer->stats[opennova::world::MatchStats::kZoneTakeovers] == 1 &&
	                      scorer->stats[opennova::world::MatchStats::kPoints] == 15,
	              "timed completion emits 0x53/event-43, owns zone, and scores once");
}

// Retail's S2C 0x40 producer is the general minimap-overlay stream, not an AS
// capture-zone-only packet. 00TRg has no zone chain, yet its retail host sends
// two initial persistent batches for pool-2 buildings/armories (16 + 8), then
// visits one of 128 pool-1 residue classes every 14 simulation ticks. The five
// placed EWEAPs at slots 8..12 therefore arrive as one-entry packets and the
// sweep repeats after 128 phases (~28.9 s at 62 Hz).
bool check_retail_minimap_overlay_stream_without_zone_chain() {
	opennova::np::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	opennova::world::World world;
	world.mp_session = true;
	ctx.world = &world;
	world.registry.configure_pool(1, 256);
	world.registry.configure_pool(2, 27);

	auto building = [](uint8_t team = 0, uint8_t source = 0) {
		opennova::world::Entity e;
		e.has_item_def = true;
		e.item_type = 5; // ItemDef Building
		e.kind = opennova::world::EntityKind::Building;
		e.has_minimap_model_marker = true; // graphicModel+0xE0 portal/occlusion pointer
		e.team = team;
		e.zone_number = source;
		e.health = 1;
		e.alive = true;
		return e;
	};
	for (int slot = 2; slot <= 25; ++slot) {
		opennova::world::Entity e = building();
		if (slot >= 3 && slot <= 5) {
			e.item_attrib = 0x00080000u; // Armory
			e.has_minimap_model_marker = false; // armory classifies before Building
		}
		if (slot == 22) e = building(2, 4); // Red bunker, BMS lfp_group 4
		const opennova::world::EntityHandle h =
				world.registry.spawn_from(2, static_cast<size_t>(slot), e);
		if (!expect(h.valid() && h.slot() == slot,
		            "oracle minimap fixture keeps the retail pool-2 slot layout"))
			return false;
	}
	// A plain Building without graphicModel+0xE0 is not a minimap entry.
	opennova::world::Entity excluded = building();
	excluded.has_minimap_model_marker = false;
	if (!expect(world.registry.spawn_from(2, 26, excluded).slot() == 26,
	            "oracle minimap fixture includes a non-overlay building"))
		return false;

	for (int slot = 8; slot <= 12; ++slot) {
		opennova::world::Entity e;
		e.has_item_def = true;
		e.item_type = 6; // Object
		e.item_id = 1902;
		e.item_attrib = opennova::world::kItemAttribEweap;
		e.health = 100;
		e.alive = true;
		const opennova::world::EntityHandle h =
				world.registry.spawn_from(1, static_cast<size_t>(slot), e);
		if (!expect(h.valid() && h.slot() == slot,
		            "oracle minimap fixture keeps the retail pool-1 EWEAP layout"))
			return false;
	}

	opennova::netsim::UdpSessionTransport transport(
			opennova::netsim::UdpSessionTransport::Role::Host);
	opennova::np::NapiNPConnection conn;
	conn.type = 1;
	conn.phase = opennova::np::ConnectionPhase::InMatch;
	conn.burst.spawned = true;
	conn.link.mode = opennova::netsim::TransportMode::Client;
	conn.link.transport = &transport;
	ctx.np_protocol.connection_list.push_back(std::move(conn));

	auto drain_overlay = [&]() {
		std::vector<opennova::CaptureZoneOverlayBatch> batches;
		std::vector<uint8_t> raw;
		while (transport.pop_outbound(raw)) {
			if (raw.empty() || raw.front() != opennova::s2c::CAPTURE_ZONE_STATE)
				continue;
			opennova::CaptureZoneOverlayBatch batch;
			if (!opennova::decode_capture_zone_overlay(
						raw.data() + 1, raw.size() - 1, batch)) {
				batches.clear();
				return batches;
			}
			batches.push_back(std::move(batch));
		}
		return batches;
	};

	if (!expect(world.zone_chain.empty(),
	            "00TRg minimap oracle deliberately has no capture-zone chain"))
		return false;
	opennova::np::Server_TickUpdate(ctx);
	std::vector<opennova::CaptureZoneOverlayBatch> batches = drain_overlay();
	if (!expect(batches.size() == 2 && batches[0].count == 16 &&
	                    batches[1].count == 8,
	            "initial general-overlay scan emits retail's 16 + 8 batches without zones"))
		return false;
	std::vector<opennova::CaptureZoneOverlay> initial;
	for (const auto &batch : batches)
		initial.insert(initial.end(), batch.entries.begin(), batch.entries.end());
	if (!expect(initial.size() == 24 && initial.front().handle == 0x2002 &&
	                    initial.back().handle == 0x2019,
	            "initial overlay preserves the retail pool-2 handle run"))
		return false;
	if (!expect(initial[0].param == 0 && initial[0].icon_color == 0 &&
	                    initial[0].flags == 0x10 &&
	                    initial[1].param == 13 && initial[1].icon_color == 12 &&
	                    initial[2].param == 13 && initial[3].param == 13,
	            "building and Armory classifiers match the oracle bytes"))
		return false;
	const opennova::CaptureZoneOverlay &bunker = initial[20]; // handle 0x2016
	if (!expect(bunker.handle == 0x2016 && bunker.param == 0 &&
	                    bunker.icon_color == 9 && bunker.flags == 0x10 &&
	                    bunker.source == 4,
	            "Red bunker carries its team color and lfp_group source byte"))
		return false;

	// Phase 0 ran with the initial scan. Cooldown 13 means phase 8 runs on
	// logic tick 113: 8 * 14 ticks after the first invocation.
	for (int i = 0; i < 111; ++i) {
		opennova::np::Server_TickUpdate(ctx);
		if (!expect(drain_overlay().empty(),
		            "pool-1 residue sweep stays quiet before phase 8"))
			return false;
	}
	opennova::np::Server_TickUpdate(ctx);
	batches = drain_overlay();
	if (!expect(batches.size() == 1 && batches[0].entries.size() == 1,
	            "phase 8 emits one dynamic emplacement overlay"))
		return false;
	const opennova::CaptureZoneOverlay first_eweap = batches[0].entries[0];
	if (!expect(first_eweap.handle == 0x1008 && first_eweap.param == 4 &&
	                    first_eweap.icon_color == 8 && first_eweap.flags == 0 &&
	                    first_eweap.source == 0,
	            "generic EWEAP classifier matches retail's dynamic overlay bytes"))
		return false;

	for (int expected_slot = 9; expected_slot <= 12; ++expected_slot) {
		for (int i = 0; i < 14; ++i) opennova::np::Server_TickUpdate(ctx);
		batches = drain_overlay();
		if (!expect(batches.size() == 1 && batches[0].entries.size() == 1 &&
		                    batches[0].entries[0].handle ==
		                            static_cast<uint16_t>(0x1000 | expected_slot),
		            "successive 14-tick phases visit the next EWEAP slot"))
			return false;
	}
	return true;
}

// StartDelay is a host-side seconds phase, not a second gameplay clock. The
// ordinary 62 Hz clock and network maintenance continue, while the World
// systems stay frozen through the boundary that changes 1 -> 0. Gameplay
// resumes on the following frame.
// [orig: reset_round_counters @0x516C8D; Server_TickUpdate
// @0x51D8BD and @0x51DC20..0x51DC33]
bool check_preround_delay_phase_boundary() {
	struct CountingSystem final : opennova::world::ISystem {
		int ticks = 0;
		const char *name() const override { return "preround-counter"; }
		void tick(opennova::world::World &,
		          const opennova::world::TickContext &) override {
			++ticks;
		}
	};

	opennova::world::World world;
	CountingSystem counter;
	world.add_system(&counter);
	opennova::world::MatchRules rules;
	rules.game_type = opennova::game_type::kDeathmatch;
	rules.game_time_minutes = 1;
	world.match.configure(rules);
	const int32_t initial_round_ticks = world.match.remaining_ticks();
	opennova::np::NapiNPServerCtx ctx;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	ctx.world = &world;
	ctx.config.start_delay = 2;
	opennova::np::Server_InitNewRoundState(ctx);
	if (!expect(world.preround_delay_seconds == 2,
	            "round init seeds StartDelay as whole seconds"))
		return false;

	opennova::np::Server_TickUpdate(ctx);
	if (!expect(world.logic_tick == 1 && world.preround_delay_seconds == 1 &&
	                    counter.ticks == 0,
	            "the zero-armed one-second service fires on the first frame and "
	            "decrements StartDelay once"))
		return false;
	for (int i = 0; i < 61; ++i) opennova::np::Server_TickUpdate(ctx);
	if (!expect(world.logic_tick == 62 && world.preround_delay_seconds == 1 &&
	                    counter.ticks == 0,
	            "pre-round advances the frame clock without running gameplay"))
		return false;
	opennova::np::Server_TickUpdate(ctx);
	if (!expect(world.logic_tick == 63 && world.preround_delay_seconds == 0 &&
	                    counter.ticks == 0 &&
	                    world.match.remaining_ticks() == initial_round_ticks,
	            "transition frame freezes systems and the round clock"))
		return false;
	opennova::np::Server_TickUpdate(ctx);
	return expect(world.logic_tick == 64 && counter.ticks == 1 &&
	                      world.match.remaining_ticks() == initial_round_ticks - 1,
	              "gameplay resumes on the frame after countdown expiry");
}

} // namespace

int main() {
	bool ok = true;
	ok = check_scoreboard_message_is_transient() && ok;
	ok = check_scoreboard_projects_every_retail_mode_shape() && ok;
	ok = check_connection_mode_table() && ok;
	ok = check_single_player_signature() && ok;
	ok = check_retail_rate_defaults() && ok;
	ok = check_pre_dictation_holdoff_keeps_initial_settings_open() && ok;
	ok = check_create_session_brings_up_host() && ok;
	ok = check_create_session_replaces_connection_role_set() && ok;
	ok = check_listen_host_installs_local_character_profile() && ok;
	ok = check_dedicated_host_has_no_local_client() && ok;
	ok = check_production_serve_mode_has_no_phantom_and_mints_startup() && ok;
	ok = check_host_pump_batches_one_send_boundary() && ok;
	ok = check_initial_stream_batches_with_reactive_reply() && ok;
	ok = check_host_frame_failure_preserves_owner_queue() && ok;
	ok = check_requeued_fragment_run_stays_one_capacity_unit() && ok;
	ok = check_host_idle_send_interval_keepalive() && ok;
	ok = check_host_admits_exact_retail_message_prefix() && ok;
	ok = check_host_s2c_holdoff_and_frame_envelope() && ok;
	ok = check_host_s2c_holdoff_is_per_connection() && ok;
	ok = check_initial_stream_obeys_connection_holdoff() && ok;
	ok = check_host_loopback_does_not_inherit_udp_envelope() && ok;
	ok = check_sparse_empty_slot_sweep_fragments_without_loss() && ok;
	ok = check_fragment_group_waits_for_full_node_capacity() && ok;
	ok = check_host_pump_reconnect_keeps_fresh_connection() && ok;
	ok = check_global_scoreboard_integrity_phase() && ok;
	ok = check_scoreboard_active_slot_filter_is_distinct() && ok;
	ok = check_requester_score_delta_refresh() && ok;
	ok = check_listen_host_receives_targeted_maintenance() && ok;
	ok = check_spawned_peer_gets_periodic_retail_maintenance() && ok;
	ok = check_spawn_wave_queue_and_release_wire() && ok;
	ok = check_default_spawn_requires_no_team_zone() && ok;
	ok = check_vehicle_spawn_target_deploys_into_best_seat() && ok;
	ok = check_session_status_reply_matches_retail_writer() && ok;
	ok = check_objective_mode_session_status_options() && ok;
	ok = check_score_ini_drives_session_status_values() && ok;
	ok = check_timed_capture_host_wire_transaction() && ok;
	ok = check_retail_minimap_overlay_stream_without_zone_chain() && ok;
	ok = check_preround_delay_phase_boundary() && ok;
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
