#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_message_dispatch.h>

#include <runtime/inmatch/server_spawn.h> // Server_InitNewRoundState (§5.2a step 1)

#include <runtime/world/world.h> // World::crt_rand — the session-seeded CRT stream

#include <base/io/log.h>
#include <net/npwire/nw_session_framing.h> // next_connection_index (the self-join's two creates)
#include <runtime/inmatch/server_console.h> // the /INOUT host line
#include <runtime/inmatch/host_settings.h> // mission_session_title
#include <runtime/inmatch/punt_log.h> // ConnectLog (a listen host's own connection turns it off)

namespace opennova::inmatch {

// [orig: CGameSession_SetConnectionMode @0x4c49f0] — stores the mode and decomposes it into the
// is_host / is_client booleans (§5.0):
//   mode 0 -> 0/0   mode 1 -> 1/0   mode 2 -> 0/1   mode 3 -> 1/1
void set_connection_mode(NapiNPServerCtx &ctx, ConnectionMode mode) {
	ctx.connection_mode = mode;
	const uint32_t m = static_cast<uint32_t>(mode);
	ctx.is_authority = (m & 0x1u) ? 1u : 0u;        // is_host bit
	ctx.is_mp_session_peer = (m & 0x2u) ? 1u : 0u;  // is_client bit
}

// [orig: CNapiNetwork_SetTransportMode @0x4c8750] — records the socket mode. The original opens a
// socket only for modes 2/3/4 (CNapiNetwork_OpenTransportSocket @0x4c6a40); mode 1 is in-process.
// The owner performs any real socket open — here we just hold the witnessed mode.
void set_transport_mode(NapiNPServerCtx &ctx, SocketMode socket) {
	ctx.socket_state = socket;
}

// [orig: NapiNPProtocol_StartServer @0x62b5e0] — see header. Host-only.
void start_server(NapiNPServerCtx &ctx, const SessionStartup &startup) {
	if (!ctx.is_authority) return;
	NapiNPProtocol &p = ctx.np_protocol;
	p.identity = startup.title;             // the network init's identity block (the 0x81's, the gates')
	p.host_key = startup.host_key;          // HK TLV (validated against the client HK on join)
	p.host_start_tick = startup.host_start_tick; // uptime base
	p.max_players = ctx.config.max_players; // MP TLV mirror
	p.host_stop_tick = 0;                   // cleared until StopServer
	p.host_run_duration_ms = 0;
	p.host_running = 1;                      // StartServer succeeded
	// The socket the host opened before this start hands the /INOUT switch to the
	// protocol's log [orig: CNapiNetwork_OpenTransportSocket @0x4C6D15], then the
	// host line, ahead of the host-start callback [orig: CNapiNPConnection_LogHostStarted
	// @0x62b629].
	open_inout_log(ctx);
	write_inout_line(ctx, inout_host_line(ctx, /*started=*/true));
	// The host start callback zeroes the total logins [orig: NapiNPProtocol_StartServer
	// @0x62b640 -> CNapiServer_OnHostStarted @0x4c94f0].
	ctx.total_logins = 0;
}

namespace {

// The mission start's context resets [orig: Game_StartMission].
void start_mission_state(NapiNPServerCtx &ctx, const GameConfig &config) {
	ctx.config = config;
	// The head of every mission start zeroes the main frame's 62-frame quality
	// countdown and resets the metrics object, whose send window, folded
	// quality (+0x0C, the 0x79 byte) and level (+0x00) all go to 0: a map
	// change's first frame samples into an empty window, as a fresh session's.
	// [orig: Game_StartMission @0x524360 - `xor ebx, ebx` @0x524381, the
	//  countdown store `mov g_NetQualityUpdateCountdown, ebx` @0x52439E, CNetStats_Reset
	//  @0x5243AA (@0x4C2FF0, this = g_NetQualityLevel @0x52438D)]
	ctx.net_quality_sample_countdown = 0;
	replication::net_quality_window_clear(ctx.host_quality_window);
	ctx.host_network_quality = 0;
	ctx.net_quality_level = 0;
	// Game_StartMission runs Nbstat_StartupInit once per mission, clearing the
	// shared scoreboard/integrity counter but deliberately leaving the separate
	// process-global 0x30/0x31 family toggle untouched.
	// [orig: Game_StartMission @0x526108 -> Nbstat_StartupInit @0x4FDE30;
	// timer store @0x4FDE41]
	ctx.scoreboard_broadcast_timer = 0;
	// The end-round block clears at every mission start, on every peer
	// [orig: Game_StartMission @0x5249EA].
	ctx.round_end_announced = false;
	ctx.round_end_linger_ticks = 0;
	ctx.round_end_board_stream.clear();
	// Every mission start advances both transfer counters; the first mission of
	// this context serves id 1 on 0x60 and 0x64, the map cycle's next one 2.
	// [orig: Game_StartMission @0x5247F3 `++g_ReplayBlockMagic`;
	//  CNapiGameSession_InitRandomSeedOrRequest @0x51E9C1 `++dword_C86FC8`]
	++ctx.server_info_transfer_id;
	++ctx.mission_metadata_transfer_id;
}

// The config the session serves from: the resolved send period, the session
// title (the mount's override table `text_override` answering first), the
// mission-data block (its rules word the title identity's, `title`; its +0x74
// the session title), the advertised name, cap and flag words.
void resolve_mission_config(NapiNPServerCtx &ctx, const GameConfig &config,
		const TitleIdentity *title, const rtxt::File *text_override) {
	// Resolve the session-selected retail default once at session creation so
	// every later connection, settings record, and countdown reads the same
	// concrete period. A caller-supplied override wins verbatim.
	// [orig: NapiNPServer_GetSendHoldoffTicks @0x4c4ab0]
	const GameSessionChannel transport_fallback =
			ctx.socket_state == SocketMode::Socketless
					? GameSessionChannel::SinglePlayer
					: GameSessionChannel::Lan;
	ctx.config.send_holdoff_ticks =
			config.effective_send_holdoff_ticks(transport_fallback);
	// The mission start's session title, set by the settings apply ahead of the
	// mission-data block, which copies it [orig: Game_ApplySessionSettingsToGlobals
	// @0x552112..0x55219E, called by Game_StartMission @0x524662 ahead of
	// CNapiGameSession_InitRandomSeedOrRequest @0x5248A7].
	ctx.session_title = mission_session_title(text_override, config.mission_file);
	ctx.mission_metadata_blob = build_mission_metadata_blob(
			ctx.config, ctx.is_mp_session_peer != 0, title, ctx.session_title);
	ctx.np_protocol.session_name = config.server_name; // "HOST STARTED \"%s\"" log name
	// The MP TLV (the 0x82 and the LAN discovery 0x81) carries the session's
	// cap, never the slot limit's dedicated slot [orig: protocol +0x524 =
	// ctx+0xF28, CNapiGameSession_CreateSession @0x4C9A80;
	// NapiNPProtocol_SendServerInfoPacket @0x6209FF].
	ctx.np_protocol.max_players = config.max_players;
	// The original snapshots both advertised flag words while building the
	// session config. P2 and the S2C 0x08 tail are the same BuildFlags value;
	// computing it once here prevents those two wire legs from drifting.
	ctx.np_protocol.server_flags = config.game_type;
	ctx.np_protocol.build_flags = build_server_config_flags(ctx);
}

} // namespace

void continue_session(NapiNPServerCtx &ctx, const GameConfig &config,
		const rtxt::File *text_override) {
	start_mission_state(ctx, config);
	resolve_mission_config(ctx, config, ctx.np_protocol.identity, text_override);
	// The exit the round end stored is spent: the main frame read it before
	// the map change [orig: Game_ProcessMainFrame @0x526806..0x526867].
	ctx.mission_exit_reason = 0;
}

// [orig: CNapiGameSession_CreateSession @0x4c97c0] — see header.
CreateSessionResult create_session(NapiNPServerCtx &ctx, const GameConfig &config,
                                   const SessionStartup &startup,
                                   replication::ISessionTransport *local_client) {
	// The cfg block's mpreset word ends the process before anything of the
	// session exists: retail tests the global after its two null-argument
	// checks and calls crt_exit(0) when it is nonzero, at every session
	// create (single player's mission start and a host's session start; a
	// joiner creates none, and the map change continues the session,
	// continue_session). The context stays as it was; the embedder ends the
	// process with code 0. Retail's entry reset of a live session ahead of
	// the test (@0x4C97C6..0x4C97CC, CNapiGameSession_ResetActiveSession) has
	// no counterpart because it never acts on a reachable create: each create
	// follows a reset of the previous session, the host's in PreMenu_HostSetup
	// (@0x5691F5) and single player's in the mission teardown outside a
	// session with the Post Menu pending (Game_TeardownMission @0x5227A2), so
	// ctx+0x68 is clear by then.
	// [orig: @0x4C97E7..0x4C97F0 `cmp dword_25509FC, 0` -> crt_exit(0); the
	//  null checks @0x4C97D5 / @0x4C97DD; callers SinglePlayer_StartMission
	//  @0x561E65 and CNapiGameSession_BuildAndCreateSession @0x56997D, which
	//  the PreMenu's state 2 calls, MultiPlayer_JoinSessionStateMachine
	//  @0x56A46A]
	if (config.multiplayer_reset != 0) return CreateSessionResult::ProcessExit;
	// A new session owns a new connection table. Clear both remote server-side
	// peers and any prior local client before installing this session's role set;
	// no live-update API is allowed to mutate connection residency mid-match.
	ctx.np_protocol.connection_list.clear();
	ctx.np_protocol.next_connection_id = kFirstJoinerDcb;
	// The previous session's reset freed the squad registry with the slot table
	// [orig: CNapiGameSession_ResetActiveSession -> Server_ReleaseSessionTables (the call
	//  @0x4C8AAC) -> CDynBuffer_Clear(&g_SpawnWaveList.field_C) @0x51B505].
	ctx.squad_registry.clear();
	start_mission_state(ctx, config);
	// Retail seeds the process CRT stream from the clock once when the host
	// allocates its player-slot table and immediately spends one draw on the
	// table's anti-cheat base offset (`rand() % 25145`); the simulation's owned
	// stream takes the session seed instead (a reproducible session, D-NET-115)
	// and spends that same first draw so every later consumer sees retail's
	// draw index. A context without a world keeps the CRT default state.
	// [orig: Server_AllocatePlayerSlotTable @0x51C1A4..0x51C1BC — srand
	// @0x51C1AA, rand @0x51C1AF]
	if (ctx.world != nullptr) {
		ctx.world->crt_rand.seed(startup.session_seed_id);
		(void)ctx.world->crt_rand.next();
	}
	ctx.is_in_session = 1; // gates the whole replication loop (+0x58)
	// The title the start installs below (start_server) builds the block.
	resolve_mission_config(ctx, config, startup.title, startup.text_override);

	if (ctx.is_authority) {
		start_server(ctx, startup);
		// §5.2a step 1 — round/local-player context init at session create. [orig:
		// CNapiGameSession_BuildAndCreateSession @0x5694d0 + SinglePlayer_StartMission @0x561af0 both
		// call Server_InitNewRoundState @0x51c8e0 here.] Previously declared but never invoked in
		// production (the burst drove everything); wiring it at its faithful call site retires the
		// dead-code path.
		Server_InitNewRoundState(ctx);
	}
	// The magic packet's reflection: a NovaWorld authority in a session sets it and draws its
	// key as NapiNPTimer_GenerateRandomId does, (GetTickCount() + rand()) % 900000 + 100000, the
	// rand the session CRT stream's next draw after the round init's (retail's round init runs
	// ahead of the create, CNapiGameSession_BuildAndCreateSession @0x5695B3 then @0x569986), the
	// tick the create's (the session's start tick, as the round init's key takes it); any other
	// create zeroes both. Retail draws it ahead of NapiNPProtocol_StartServer, which never
	// touches either word. (D-NET-NEW-L1-20)
	// [orig: CNapiGameSession_CreateSession - in session `cmp [ebp+58h]` @0x4C9B05, the
	//  authority @0x4C9B0A, transport_mode == 1 @0x4C9B0F, +0x52C = 1 @0x4C9B1A, the draw
	//  @0x4C9B26, else +0x52C = 0 @0x4C9B33, +0x530 = 0 @0x4C9B3F; NapiNPTimer_GenerateRandomId
	//  @0x61E510 - GetTickCount @0x61E514, rand @0x61E51C, the add @0x61E521, div 0xDBBA0
	//  @0x61E52A, + 0x186A0 @0x61E52D, the store @0x61E533]
	{
		NapiNPProtocol &p = ctx.np_protocol;
		if (ctx.is_in_session != 0 && ctx.is_authority != 0 &&
				ctx.transport_mode == NetworkType::NovaWorld) {
			p.magic_reflect = 1;
			const uint32_t draw = ctx.world != nullptr ? ctx.world->crt_rand.next() : 0u;
			p.magic_key = (p.host_start_tick + draw) % 0xDBBA0u + 0x186A0u;
		} else {
			p.magic_reflect = 0;
			p.magic_key = 0;
		}
	}

	// mode 3 (host + client): the listen server connects its own local client in-process.
	if (ctx.connection_mode == ConnectionMode::HostClient && local_client != nullptr) {
		NapiNPConnection self;
		// The host's own player dcb (entity+0x78). NOT 0 — dcb 0 is the dedicated-server reservation
		// that makes the joining client drop the player slot and flood C2S 0x0F (see kHostPlayerDcb).
		// The host knows its own ConnectionId locally, so latch self_id_seen (no 0x48 ack arrives for
		// the loopback). Joiners are assigned from kFirstJoinerDcb up so they never collide with it.
		self.connection_id = kHostPlayerDcb;
		self.self_id_seen = true;
		self.type = 2;          // client-side connection [orig: CNapiNPConnection_Create type 2]
		self.phase = ConnectionPhase::New; // advanced by the host's own-player spawn flow (P3)
		self.link.transport = local_client;
		self.link.mode = replication::TransportMode::Loopback; // socketless mode 1
		ctx.np_protocol.connection_list.push_back(self);
		// Retail's self-join creates two connections on the game protocol, each stepping its
		// connection counter: the host's own client (type 2), then the server-side node its own
		// 0x42 creates (type 1). (D-NET-713)
		// [orig: CNapiGameSession_CreateSession @0x4C97C0 - the type-2 create @0x4C9B9C;
		//  NapiNPProtocol_HandleClientJoin @0x62BF06 (the self-join's 0x42, through
		//  CNapiGameSession_WaitForHostResponse @0x4C9C6F); CNapiNPConnection_Create - the step
		//  @0x62B282..0x62B296]
		(void)next_connection_index();
		(void)next_connection_index();
		// The host's own connection coming up turns the _connectlog.txt debug log off and
		// counts a login like every other [orig: NapiNPServer_HandleNewConnection - the
		// disable @0x4C8054..0x4C80AF at its head, the login @0x4c8203, ahead of its
		// local-connection arm @0x4c8213].
		if (ctx.logs.connect != nullptr && ctx.logs.connect->enabled())
			ctx.logs.connect->disable_at_server_connection();
		++ctx.total_logins;
	}
	return CreateSessionResult::Created;
}

} // namespace opennova::inmatch
