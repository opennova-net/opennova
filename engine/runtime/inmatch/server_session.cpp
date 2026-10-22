#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_message_dispatch.h>

#include <runtime/inmatch/server_spawn.h> // Server_InitNewRoundState (§5.2a step 1)

#include <runtime/world/world.h> // World::crt_rand — the session-seeded CRT stream

#include <base/io/log.h>
#include <net/npwire/nw_session_framing.h> // next_connection_index, manager_connection_keys
#include <net/npwire/session_hello.h> // the self-join's 0x41 / 0x42 (their lengths)
#include <net/npwire/session_keys.h> // SESSION_OPCODE_CLIENT_HELLO / _AUTH
#include <net/title/title_identity.h> // TitleIdentity::pn (the self-join's PROTOCOL lines)
#include <runtime/inmatch/napi_np_protocol.h> // host_server_hello (the self-join's 0x81)
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
	// The rebuilt server-info stream [orig: Game_SerializeMissionInfoToDataStream @0x5247EB].
	ctx.server_info_stream_emptied = false;
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
	// no live-update API is allowed to mutate connection residency mid-match. The
	// connection counter the dcbs come from is the process's and runs on (D-NET-713).
	ctx.np_protocol.connection_list.clear();
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
		// §5.2a step 1: the round-state init runs AHEAD of the session create, the server start
		// included: CNapiGameSession_BuildAndCreateSession and SinglePlayer_StartMission call it
		// before CNapiGameSession_CreateSession. Its key draw reads GetTickCount in the create's
		// frame, the start tick ours stamps, so the stamp lands first; the draws the create makes
		// follow its own (the slot table's seed and draw above, the key). The listen host's
		// own-slot legs need the own connection, which the create adds: start_host_session runs
		// them after it, ahead of the spawn pump.
		// [orig: CNapiGameSession_BuildAndCreateSession @0x5694D0 - Server_InitNewRoundState
		//  @0x5695B3 ahead of CNapiGameSession_CreateSession @0x56997D; SinglePlayer_StartMission
		//  @0x561AF0 - @0x561C23 ahead of @0x561E65; Server_InitNewRoundState - the key's
		//  GetTickCount @0x51CB69]
		ctx.np_protocol.host_start_tick = startup.host_start_tick;
		Server_InitNewRoundState(ctx);
		start_server(ctx, startup);
	}
	// The magic packet's reflection: a NovaWorld authority in a session sets it and draws its
	// key as NapiNPTimer_GenerateRandomId does, (GetTickCount() + rand()) % 900000 + 100000, the
	// rand the session CRT stream's next draw after the round init's (retail's round init runs
	// ahead of the create: CNapiGameSession_BuildAndCreateSession @0x5694D0, the round init's
	// call @0x5695B3, then the create's @0x56997D), the tick the create's (the session's start
	// tick, as the round init's key takes it); any other create zeroes both. Retail draws it
	// ahead of the create's NapiNPProtocol_StartServer call (@0x4C9B52), which never touches
	// either word. (D-NET-NEW-L1-20)
	// [orig: CNapiGameSession_CreateSession @0x4C97C0 - in session `cmp [ebp+58h]` @0x4C9B05, the
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
		// The self-join (D-NET-NEW-L1-22). The host's own client connection (type 2) takes the
		// protocol's next index, its CI, and joins its own server inside the create's wait,
		// through the manager's internal FIFO: an address-0 send queues the packet with address
		// 0 and the send's port, and the manager's receive hands it over with no RAW or COOKED
		// line. Its enumerator, the announce flag set, sends the 0x41 to address 0, port 0 (no
		// PM, EIP, EPN or ET); the server's hello handler answers the 0x81 to that address,
		// before any server-side node exists (no NP, no NC); the client's handler queues it
		// (the announce flag skips the session-discovered callback) and the queued pass records
		// the session; the connect draws the client's key and SCRK from the manager PRNG and the
		// state machine sends the 0x42 to the session's address, 0 and 0 (no SIP or SPN, and no
		// CU: the game's tags ride only a joiner's connect); the server takes it on its socket
		// pass and its queued pass creates the server-side node (the next index, its key and
		// SCRK drawn), whose new connection turns the log off. With the log on, each send writes
		// its NET SENDING line (the protocol's client send callback) and each receipt its
		// PROTOCOL line, from 0.0.0.0:0, ahead of the disabling line; NA is the session's
		// player name, "???" when empty.
		// [orig: CNapiGameSession_CreateSession @0x4C97C0 - "???" @0x4C9927..0x4C993B, NP's
		//  proto+0x520 zeroed @0x4C9A6E, the type-2 create @0x4C9B9C, CNapiNPConnection_SetName @0x4C9BB7, the wait's call
		//  @0x4C9C6F; CNapiGameSession_WaitForHostResponse @0x62B2D0 - the announce flag
		//  @0x62B31F, the enumerator on @0x62B340, the pumps @0x62B36A / @0x62B389, the session
		//  test @0x62B3B4, CNapiNPConnection_InitFromSession @0x626320 (the call @0x62B46C; the
		//  key's draw @0x6263F4, the SCRK's @0x626401), the pumps to state 5 @0x62B47D..0x62B4BB;
		//  CNapiNPConnection_PumpEnumeratorAndSend @0x6290C0 (NapiNPSession_SendAnnouncePacket
		//  (conn, 0, 0, ..) @0x629170); NapiNPSession_SendAnnouncePacket @0x61FA00 (PM only
		//  without the flag @0x61FCCA, the send callback proto+0x270 @0x61FDCA, the SendTo
		//  call @0x61FDDD); CNapiNetwork_Init @0x4CA4A0 (NapiLog_OnSend into proto+0x270 @0x4CA978,
		//  the server send slot proto+0x2E0 an empty stub @0x4CA918); NapiLog_OnSend @0x4C4760;
		//  CNapiNPManager_SendTo @0x61EC20 (the address-0 arm @0x61EC59, the FIFO write
		//  @0x61ECC6); NapiNPManager_RecvFrom @0x61ECE0; NapiNPManager_PumpReceive @0x623010 (the
		//  FIFO read @0x6230B3, no RAW / COOKED for it @0x62318B, HandlePacket @0x623284);
		//  NapiNPManager_HandlePacket @0x622F10 (cb_on_dispatch @0x622FCD);
		//  NapiNPProtocol_HandleClientHello @0x6213B0 (the reply @0x6218FC);
		//  NapiNPProtocol_SendServerInfoPacket @0x6204B0 (NC @0x620A55, the SendTo call @0x620C89);
		//  Nwu_HandleServerHello @0x626D20 (the socket pass's queue @0x627F45 and return 1
		//  @0x627F5A, the discovered callback skipped under the flag @0x6278D7);
		//  CNapiNPConnection_PumpStateMachine @0x6292E0 (CNapiNPConnection_SendClientJoin
		//  called @0x629595); CNapiNPConnection_SendClientJoin @0x61FE20 (the send callback
		//  @0x6203F0, the SendTo call @0x62040C); NapiNPProtocol_HandleClientJoin @0x62B750 (the
		//  type-1 create @0x62BF06, the SK's draw @0x62BF5E)]
		const TitleIdentity *identity = ctx.np_protocol.identity;
		const PeerAddr internal{}; // address 0, port 0
		const uint32_t own_index = next_connection_index();
		ClientHello hello;
		ServerHello reply;
		if (identity != nullptr) {
			hello = make_title_client_hello(*identity, own_index);
			reply = host_server_hello(ctx, hello, internal);
		}
		const ConnectionKeys own_keys = manager_connection_keys();
		// The pair's name: the session's player name, "???" when empty, the client's
		// SetName and the twin's NA (D-NET-NEW-L8-21 reads it in the pair's /INOUT lines).
		const std::string name =
				config.player_name.empty() ? "???" : config.player_name.substr(0, 63);
		NapiNPConnection self;
		self.player_name = name;
		// The host knows its own ConnectionId locally, so latch self_id_seen (no 0x48 ack arrives
		// for the loopback); the dcb itself is the twin's index, set at its create below.
		self.self_id_seen = true;
		self.type = 2;          // client-side connection [orig: CNapiNPConnection_Create type 2]
		self.phase = ConnectionPhase::New; // advanced by the host's own-player spawn flow (P3)
		self.link.transport = local_client;
		self.link.mode = replication::TransportMode::Loopback; // socketless mode 1
		// The pair's keys: the client's CI, key and SCRK; the server-side node's key and SCRK
		// (a forced mint stands for the draw, as a remote join's does).
		self.client_ci = own_index;
		self.client_ck = own_keys.key;
		self.client_scrk = own_keys.scrk;
		// Retail's self-join creates two connections on the game protocol, each stepping the
		// process's connection counter: the host's own client (type 2) above, then the
		// server-side twin its own 0x42 creates (type 1). The twin's index is the pair's dcb: its
		// state change makes it the twin's connection_id, the twin's 0x82 carries it as MI, the
		// own client latches it as its own (the local id the client's self-match reads) and the
		// new-connection callback's player, the host's own, carries it at entity+0x78, so a fresh
		// process's own player is dcb 2 and one that browsed, joined or hosted before numbers
		// higher. Never 0, the dedicated-server reservation (the counter's 0 -> 1 wrap). (D-NET-713)
		// [orig: CNapiNPConnection_Create @0x62ACB0 - the step @0x62B282..0x62B296;
		//  NapiNPProtocol_HandleClientJoin @0x62B750 - the twin's create @0x62BF06;
		//  CNapiNPConnection_OnStateChange @0x626060 - connection_id @0x626199..0x62619C;
		//  CNapiNPConnection_SendSessionInit @0x620EF0 - MI @0x620FD4..0x620FE8;
		//  NapiNP_HandleServerJoinResponse @0x629840 - the own client's connection_id = MI
		//  @0x629D50; NapiNP_GetLocalConnectionId @0x4C6D40; NapiNPServer_HandleNewConnection
		//  @0x4C8040 - the player's id @0x4C80B9]
		self.connection_id = next_connection_index();
		if (ctx.server_key_mint.forced) {
			self.server_sk = ctx.server_key_mint.server_sk;
			self.server_scrk = ctx.server_key_mint.server_scrk;
		} else {
			const ConnectionKeys node_keys = manager_connection_keys();
			self.server_sk = node_keys.key;
			self.server_scrk = node_keys.scrk;
		}
		ctx.np_protocol.connection_list.push_back(self);
		if (ctx.logs.connect != nullptr && ctx.logs.connect->enabled()) {
			ConnectLog &log = *ctx.logs.connect;
			if (identity != nullptr) {
				const std::string pn = identity->pn != nullptr ? identity->pn : "";
				const int32_t hello_len =
						static_cast<int32_t>(client_hello_to_bytes(hello).size() + 1);
				log.line(net_sending_text(hello_len, internal, SESSION_OPCODE_CLIENT_HELLO, 0));
				log.line(napi_got_protocol_text(hello_len, internal, pn));
				const int32_t reply_len =
						static_cast<int32_t>(server_hello_to_bytes(reply).size() + 1);
				log.line(napi_got_protocol_text(reply_len, internal, pn));
				const ClientAuth auth = make_title_client_auth(
						*identity, own_index, own_keys.key, reply.hk, name, own_keys.scrk);
				const int32_t join_len =
						static_cast<int32_t>(client_auth_to_bytes(auth).size() + 1);
				log.line(net_sending_text(join_len, internal, SESSION_OPCODE_CLIENT_AUTH, 0));
				log.line(napi_got_protocol_text(join_len, internal, pn));
			}
			// The server-side node coming up turns the _connectlog.txt debug log off
			// [orig: NapiNPServer_HandleNewConnection @0x4C8040 - the disable
			//  @0x4C8054..0x4C80AF at its head].
			log.disable_at_server_connection();
		}
		// ...and counts a login like every other [orig: NapiNPServer_HandleNewConnection
		// @0x4C8040 - the login @0x4c8203, ahead of its local-connection arm @0x4c8213].
		++ctx.total_logins;
	}
	return CreateSessionResult::Created;
}

} // namespace opennova::inmatch
