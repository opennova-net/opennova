// The authority's METPROTOCOL gate reports (D-NET-995): the CServerTick clocks' writers on
// the host context and the slot rows (the session create, the mission start and its load's end,
// the remote add, the net player's game states 9 / 10 / 11), and the four senders through the
// embedder's sink: SERVER at the session's end (then both process clocks reset, so a later PLAYER
// prints GCC ""), PLAYER at a non-local leave, SERVERMISSION and the joined slots' PLAYERMISSION
// at the linger's expiry; each under retail's gates (in session, the NovaWorld network type, a
// sink), and the joiner's ClientAuth strings the player reports print.
// [orig: CNapiGameSession_CreateSession @0x4C9CE8 / @0x4C9CF2; Game_StartMission @0x5244A4 /
//  @0x525E42; Server_PlayerAdd @0x51CD1B / @0x51CD26, @0x51D2EE / @0x51D2F8;
//  CNetPlayer_SetGameState @0x4C4060;
//  Server_ProcessPeriodicTimerUpdate @0x4FBF50; Server_HandlePlayerDisconnect @0x51B690 /
//  @0x51B696; Server_TickUpdate @0x51DA5A / @0x51DB1F; NapiNetConfig_LoadFromConnTags @0x4C7260]

#include <net/npwire/session_hello.h>
#include <runtime/inmatch/gate_metrics_reports.h>
#include <runtime/inmatch/napi_np_protocol.h> // stop_unstopped_server (the held server's reset)
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_files.h>     // ServerLogs

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace opennova;
using namespace opennova::inmatch;

int failures = 0;

void expect(bool ok, const char *why) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", why);
		++failures;
	}
}

bool has_row(const std::string &text, const std::string &row) {
	return text.find(row + "\r\n") != std::string::npos;
}

struct Rig {
	std::unique_ptr<NapiNPServerCtx> ctx = std::make_unique<NapiNPServerCtx>();
	std::vector<std::string> sent;

	Rig() {
		NapiNPServerCtx &c = *ctx;
		c.is_authority = 1;
		c.is_in_session = 1;
		c.is_mp_session_peer = 1; // a listen host: its own slot counts in the marks
		c.transport_mode = NetworkType::NovaWorld;
		c.config.max_players = 16;
		c.config.game_type = 0x10000u; // a team game (TDM): the team marks count
		c.config.mission_file = "ASH_I5A.BMS";
		c.gate_metrics.host = [] {
			GateMetricsHost host;
			host.label = "LBL";
			host.lobby = "Lobby";
			host.version = "V1.7.5.7";
			return host;
		};
		c.gate_metrics.game_text = [](const char *section, const char *key, const char *fallback) {
			return std::string(section) == "GateTypeAbbrev" ? std::string(key) : std::string(fallback);
		};
		c.gate_metrics.send = [this](const std::string &text) { sent.push_back(text); };
		// The own client (local), and two remotes on teams 1 and 2.
		NapiNPConnection own;
		own.type = NapiNPConnection::kTypeClientSide;
		own.phase = ConnectionPhase::PlayerAdded;
		own.reply.player_slot = 0;
		own.assigned_team = 1;
		own.assigned_team_valid = true;
		c.np_protocol.connection_list.push_back(own);
		for (uint8_t slot = 1; slot <= 2; ++slot) {
			NapiNPConnection remote;
			remote.type = NapiNPConnection::kTypeServerSide;
			remote.link.mode = replication::TransportMode::Client;
			remote.phase = ConnectionPhase::PlayerAdded;
			remote.reply.player_slot = slot;
			remote.assigned_team = slot;
			remote.assigned_team_valid = true;
			remote.account.pcid = slot == 1 ? "PCID1" : "PCID2";
			remote.gate_player.version = "V1.7.5.7";
			remote.gate_player.country_code = "us";
			c.np_protocol.connection_list.push_back(remote);
		}
	}
	NapiNPConnection &slot(int i) { return ctx->np_protocol.connection_list[static_cast<size_t>(i)]; }
};

void check_client_auth_strings() {
	ClientAuth auth;
	auth.cu.push_back(make_client_cu_chunk(2, "VERSIONSTRING",
			"V1.7.5.7-0123456789012345678901234567")); // 37 characters: 31 kept
	auth.cu.push_back(make_client_cu_chunk(2, "COUNTRYCODE", "us"));
	auth.cu.push_back(make_client_cu_chunk(2, "countrycode", "abcdefghij")); // later wins, 7 kept
	auth.cu.push_back(make_client_cu_chunk(2, "CNT", "United States"));
	auth.cu.push_back(make_client_cu_chunk(2, "LNG", "English"));
	auth.cu.push_back(make_client_cu_chunk(2, "tzb", "300"));
	auth.cu.push_back(make_client_cu_chunk(1, "TZB", "999")); // not a type-2 tag: skipped
	const GateMetricsPlayer player = parse_client_gate_metrics_player(auth);
	expect(player.version == "V1.7.5.7-0123456789012345678901", "VERSIONSTRING keeps 31 characters");
	expect(player.country_code == "abcdefg", "COUNTRYCODE: the later tag, 7 characters");
	expect(player.country_name == "United States", "CNT");
	expect(player.language == "English", "LNG");
	expect(player.tz_bias == 300, "TZB atol, case-insensitive tag");
}

void check_writers() {
	Rig rig;
	NapiNPServerCtx &ctx = *rig.ctx;
	// The create: both process clocks constructed over CC.BIN and the config's count, the marks
	// over the live slots (the own slot counts on a listen host).
	ctx.tick_count_ms = 1000;
	gate_clocks_session_create(ctx, "us");
	expect(ctx.gate_clocks.process.start_ms == 1000 && ctx.gate_clocks.process.gcc == "us",
			"the create stamps the process clock and copies CC.BIN");
	expect(ctx.gate_clocks.process.max_players == 16 && ctx.gate_clocks.mission.max_players == 16,
			"MAXPLAYERS is the config's player count");
	expect(ctx.gate_clocks.process.most_players == 3 && ctx.gate_clocks.process.most_team1 == 2 &&
			ctx.gate_clocks.process.most_team2 == 1, "the marks count the active slots by team");
	// A Serve Only host's local slot does not count.
	ctx.is_mp_session_peer = 0;
	GateServerTick probe;
	gate_update_max_player_counts(ctx, probe);
	expect(probe.most_players == 2, "the local slot counts only on a session peer");
	ctx.is_mp_session_peer = 1;
	// A later remote add raises both process clocks' marks, its team counted; a leave lowers none.
	NapiNPConnection late = rig.slot(2);
	late.reply.player_slot = 3;
	ctx.np_protocol.connection_list.push_back(late);
	gate_clocks_player_added_marks(ctx);
	expect(ctx.gate_clocks.process.most_players == 4 && ctx.gate_clocks.mission.most_team2 == 2,
			"the add raises the process clocks' marks");
	ctx.np_protocol.connection_list.pop_back();
	gate_clocks_player_added_marks(ctx);
	expect(ctx.gate_clocks.process.most_players == 4, "the marks only rise");
	// The mission start (phase 1), then the load's end: 2 while the pre-round delay runs.
	ctx.tick_count_ms = 2000;
	gate_clocks_mission_start(ctx);
	expect(ctx.gate_clocks.process.state == 1 && ctx.gate_clocks.mission.state == 1,
			"the mission start loads");
	ctx.tick_count_ms = 5000;
	gate_clocks_mission_loaded(ctx, /*preround_delay_seconds=*/10);
	expect(ctx.gate_clocks.process.state == 2 && ctx.gate_clocks.process.loading_ms == 3000,
			"the load's end waits to begin under a pre-round delay");
	gate_clocks_mission_loaded(ctx, 0);
	expect(ctx.gate_clocks.process.state == 3, "with no pre-round delay it plays");
	// A non-authority's mission start writes nothing.
	ctx.is_authority = 0;
	gate_clocks_mission_start(ctx);
	expect(ctx.gate_clocks.process.state == 3, "only the authority's mission start writes");
	ctx.is_authority = 1;
	// The remote add's two slot clocks.
	ctx.tick_count_ms = 6000;
	gate_slot_clocks_player_added(ctx, rig.slot(1));
	expect(rig.slot(1).gate_tick.start_ms == 6000 && rig.slot(1).gate_mission_tick.start_ms == 6000,
			"the add constructs both slot clocks");
	// The net player's game states: 9 loads, the same state is no change, 10 plays, 11 ends.
	rig.slot(1).burst.game_state = 8;
	gate_slot_clocks_game_state(rig.slot(1), 9, 7000, 0);
	rig.slot(1).burst.game_state = 9;
	expect(rig.slot(1).gate_tick.state == 1 && rig.slot(1).gate_mission_tick.state == 1,
			"game state 9 loads both slot clocks");
	gate_slot_clocks_game_state(rig.slot(1), 9, 7500, 0);
	expect(rig.slot(1).gate_tick.phase_start_ms == 7000, "an unchanged game state writes nothing");
	gate_slot_clocks_game_state(rig.slot(1), 10, 9000, /*preround_delay_seconds=*/3);
	expect(rig.slot(1).gate_tick.state == 2, "game state 10 waits under a pre-round delay");
	rig.slot(1).burst.game_state = 7;
	gate_slot_clocks_game_state(rig.slot(1), 10, 9500, 0);
	rig.slot(1).burst.game_state = 10;
	expect(rig.slot(1).gate_tick.state == 3 && rig.slot(1).gate_tick.loading_ms == 2000 &&
			rig.slot(1).gate_tick.waiting_ms == 500, "game state 10 plays with none");
	gate_slot_clocks_game_state(rig.slot(1), 11, 12500, 0);
	expect(rig.slot(1).gate_tick.state == 4 && rig.slot(1).gate_tick.mission_completes == 1 &&
			rig.slot(1).gate_tick.playing_ms == 3000, "game state 11 enters the game-over stats");
	expect(rig.sent.empty(), "no writer sends a report");
}

void check_senders() {
	Rig rig;
	NapiNPServerCtx &ctx = *rig.ctx;
	ctx.tick_count_ms = 1000;
	gate_clocks_session_create(ctx, "us");
	gate_slot_clocks_player_added(ctx, rig.slot(1));
	gate_slot_clocks_player_added(ctx, rig.slot(2));
	ctx.tick_count_ms = 3000;
	gate_clocks_mission_start(ctx);
	ctx.tick_count_ms = 8000;
	gate_clocks_mission_loaded(ctx, 0);
	rig.slot(1).burst.game_state = 8;
	gate_slot_clocks_game_state(rig.slot(1), 9, 4000, 0);
	gate_slot_clocks_game_state(rig.slot(1), 10, 9000, 0);
	rig.slot(1).burst.game_state = 10;

	// The linger's expiry: SERVERMISSION, then PLAYERMISSION for the joined slot alone.
	rig.slot(1).slot_state = PlayerSlotState::Joined;
	rig.slot(2).slot_state = PlayerSlotState::PlayerSync;
	ctx.tick_count_ms = 20000;
	Server_ReportLingerExpiry(ctx);
	expect(rig.sent.size() == 2, "the expiry sends SERVERMISSION and one PLAYERMISSION");
	if (rig.sent.size() == 2) {
		const std::string &mission = rig.sent[0];
		expect(mission.find("BLOCK \"SERVERMISSION\"") != std::string::npos, "SERVERMISSION first");
		expect(has_row(mission, "\tGAMETYPE \"TDM\""), "GAMETYPE is the GateTypeAbbrev text");
		expect(has_row(mission, "\tRUNNING 19"), "RUNNING from the create");
		expect(has_row(mission, "\tLOADINGMISSION 5") && has_row(mission, "\tPLAYING 12"),
				"the mission clock's phases, the running one closed");
		expect(has_row(mission, "\tMISSIONFILENAME \"ASH_I5A.BMS\""), "the mission file copied in");
		const std::string &player = rig.sent[1];
		expect(player.find("BLOCK \"PLAYERMISSION\"") != std::string::npos, "PLAYERMISSION second");
		expect(has_row(player, "\tPID \"PCID1\"") && has_row(player, "\tPCC \"us\""),
				"the joined slot's own strings");
		expect(has_row(player, "\tPLAYING 11"), "the slot's mission clock, closed");
	}
	expect(ctx.gate_clocks.mission.state == 0 && ctx.gate_clocks.mission.start_ms == 20000 &&
			ctx.gate_clocks.mission.mission_file == "ASH_I5A.BMS" &&
			ctx.gate_clocks.mission.gcc == "us", "the mission clock constructed again");
	expect(rig.slot(1).gate_mission_tick.start_ms == 20000 && rig.slot(1).gate_mission_tick.state == 0,
			"each slot's mission clock constructed again");
	expect(rig.slot(1).gate_tick.state == 3, "the slot's process clock runs on");

	// The session's end: SERVER over the process clock, then both reset.
	rig.sent.clear();
	ctx.tick_count_ms = 30000;
	Server_ProcessPeriodicTimerUpdate(ctx);
	expect(rig.sent.size() == 1 && rig.sent[0].find("BLOCK \"SERVER\"") != std::string::npos,
			"the session's end sends SERVER");
	if (rig.sent.size() == 1) {
		expect(has_row(rig.sent[0], "\tRUNNING 29") && has_row(rig.sent[0], "\tLOADINGMISSION 5") &&
				has_row(rig.sent[0], "\tPLAYING 22") && has_row(rig.sent[0], "\tGCC \"us\"") &&
				has_row(rig.sent[0], "\tMOSTPLAYERS 3"), "SERVER's rows");
	}
	expect(ctx.gate_clocks.process.gcc.empty() && ctx.gate_clocks.process.max_players == 0 &&
			ctx.gate_clocks.process.start_ms == 30000, "both process clocks reset");

	// A remote leave after it: the slot clock stopped, PLAYER with the reset clock's GCC "".
	rig.sent.clear();
	ctx.tick_count_ms = 31000;
	Server_ReportPlayerLeave(ctx, rig.slot(1));
	expect(rig.slot(1).gate_tick.state == 0 && rig.slot(1).gate_tick.playing_ms == 22000,
			"the leave stops the slot clock");
	expect(rig.sent.size() == 1, "a remote leave sends PLAYER");
	if (rig.sent.size() == 1) {
		expect(rig.sent[0].find("BLOCK \"PLAYER\"") != std::string::npos, "PLAYER");
		expect(has_row(rig.sent[0], "\tGCC \"\""), "PLAYER's GCC after the reset");
		expect(has_row(rig.sent[0], "\tRUNNING 30") && has_row(rig.sent[0], "\tPID \"PCID1\""),
				"PLAYER's slot clock and PID");
	}
	// The local slot's leave reports nothing; off a session, off NovaWorld or with no sink neither.
	rig.sent.clear();
	Server_ReportPlayerLeave(ctx, rig.slot(0));
	expect(rig.sent.empty(), "the local slot's leave sends nothing");
	ctx.transport_mode = NetworkType::Lan;
	Server_ReportPlayerLeave(ctx, rig.slot(2));
	ctx.transport_mode = NetworkType::NovaWorld;
	ctx.is_in_session = 0;
	Server_ProcessPeriodicTimerUpdate(ctx);
	ctx.is_in_session = 1;
	ctx.gate_metrics = GateMetricsSink{};
	Server_ReportLingerExpiry(ctx);
	Server_ReportPlayerLeave(ctx, rig.slot(2));
	expect(rig.sent.empty(), "no report off a NovaWorld session or with no sink");
}

// A held server's player at the next host's session reset: the stop's removal callback takes the
// node after its burst, and the leave stops the node's slot clock and sends PLAYER under what the
// new host's setup left (its game type, the CC.BIN its tick init read again), on a NovaWorld host
// alone; a node whose player was never added takes no callback.
// [orig: Server_HandlePlayerDisconnect @0x51B5C0 - CServerTick_SetState(0) @0x51B690,
//  Server_SendPlayerMetricsToGate @0x51B696; HostDialog_StartSession @0x558760 - g_GameType
//  @0x5588A8, Session_ResetForStart (the tail jump @0x5588C5); Server_InitServerTicks @0x5018C0 - @0x501900]
void check_held_player_leave() {
	Rig rig;
	NapiNPServerCtx &old_ctx = *rig.ctx;
	old_ctx.tick_count_ms = 1000;
	gate_clocks_session_create(old_ctx, "us");
	gate_slot_clocks_player_added(old_ctx, rig.slot(1));
	rig.slot(1).burst.game_state = 8;
	gate_slot_clocks_game_state(rig.slot(1), 9, 2000, 0);
	gate_slot_clocks_game_state(rig.slot(1), 10, 5000, 0);
	UnstoppedServer held;
	held.running = true;
	held.nodes.push_back(UnstoppedNode{rig.slot(1), {}, {}});
	UnstoppedNode never_added{rig.slot(2), {}, {}};
	never_added.conn.phase = ConnectionPhase::Joined;
	never_added.conn.link.owned_entity = {};
	held.nodes.push_back(never_added);

	std::vector<std::string> sent;
	GateMetricsSink sink = rig.ctx->gate_metrics;
	sink.send = [&sent](const std::string &text) { sent.push_back(text); };
	std::vector<std::string> order;
	uint32_t stopped_state = 99;
	stop_unstopped_server(held, ServerLogs{}, nullptr, nullptr, [&](NapiNPConnection &slot) {
		order.push_back("removed " + std::to_string(slot.reply.player_slot));
		Server_ReportHeldPlayerLeave(sink, true, 0x10000u, "de", 61000, slot);
		stopped_state = slot.gate_tick.state;
	});
	expect(order.size() == 1 && order[0] == "removed 1", "one node took the slot's teardown");
	expect(stopped_state == 0, "the leave stops the held slot's clock");
	expect(sent.size() == 1, "a held player's leave sends PLAYER on a NovaWorld host");
	if (sent.size() == 1) {
		expect(sent[0].find("BLOCK \"PLAYER\"") != std::string::npos, "PLAYER");
		expect(has_row(sent[0], "\tGAMETYPE \"TDM\""), "GAMETYPE: the new host's game type");
		expect(has_row(sent[0], "\tGCC \"de\""), "GCC: the CC.BIN the new host's tick init read");
		expect(has_row(sent[0], "\tRUNNING 60") && has_row(sent[0], "\tLOADINGMISSION 3") &&
				has_row(sent[0], "\tPLAYING 56"), "the held slot's clock, closed at the reset");
		expect(has_row(sent[0], "\tPID \"PCID1\"") && has_row(sent[0], "\tPCC \"us\""),
				"the held player's own strings");
	}
	expect(!held.running && held.nodes.empty(), "the stop clears the held server");

	// Off NovaWorld or with no sink: the clock stops, nothing is sent.
	sent.clear();
	NapiNPConnection again = rig.slot(2);
	again.gate_tick.construct(1000, "us", 16, 0, 0, 0);
	again.gate_tick.set_state(3, 2000);
	Server_ReportHeldPlayerLeave(sink, false, 0x10000u, "de", 9000, again);
	expect(sent.empty() && again.gate_tick.state == 0, "a LAN host's reset sends no PLAYER");
	again.gate_tick.set_state(3, 9000);
	Server_ReportHeldPlayerLeave(GateMetricsSink{}, true, 0x10000u, "de", 9500, again);
	expect(sent.empty() && again.gate_tick.state == 0, "no sink: no PLAYER");
}

} // namespace

int main() {
	check_client_auth_strings();
	check_writers();
	check_senders();
	check_held_player_leave();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("gate_metrics_reports: ok\n");
	return 0;
}
