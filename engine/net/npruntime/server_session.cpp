#include <net/npruntime/server_session.h>
#include <net/npruntime/server_initial_state.h>
#include <net/npruntime/server_message_dispatch.h>

#include <net/npruntime/server_spawn.h> // Server_InitNewRoundState (§5.2a step 1)

#include <runtime/world/world.h> // World::crt_rand — the session-seeded CRT stream

namespace opennova::np {

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
	p.host_key = startup.host_key;          // HK TLV (validated against the client HK on join)
	p.host_start_tick = startup.host_start_tick; // uptime base
	if (p.gen_session_seed_flag) p.session_seed_id = startup.session_seed_id;
	p.max_players = ctx.config.max_players; // MP TLV mirror
	p.host_stop_tick = 0;                   // cleared until StopServer
	p.host_run_duration_ms = 0;
	p.host_running = 1;                      // StartServer succeeded
}

// [orig: CNapiGameSession_CreateSession @0x4c97c0] — see header.
void create_session(NapiNPServerCtx &ctx, const GameConfig &config,
                    const SessionStartup &startup, netsim::ISessionTransport *local_client) {
	ctx.config = config;
	// Game_StartMission runs Nbstat_StartupInit once per mission, clearing the
	// shared scoreboard/integrity counter but deliberately leaving the separate
	// process-global 0x30/0x31 family toggle untouched.
	// [orig: Game_StartMission @0x526108 -> Nbstat_StartupInit @0x4FDE30;
	// timer store @0x4FDE41]
	ctx.scoreboard_broadcast_timer = 0;
	ctx.round_end_announced = false;
	ctx.round_end_linger_ticks = 0;
	ctx.round_end_board_stream.clear();
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
	ctx.mission_metadata_blob = build_mission_metadata_blob(
			ctx.config, ctx.is_mp_session_peer != 0);
	ctx.np_protocol.session_name = config.server_name; // "HOST STARTED \"%s\"" log name
	ctx.np_protocol.max_players = config.max_players;
	ctx.is_in_session = 1; // gates the whole replication loop (+0x58)
	// The original snapshots both advertised flag words while building the
	// session config. P2 and the S2C 0x08 tail are the same BuildFlags value;
	// computing it once here prevents those two wire legs from drifting.
	ctx.np_protocol.server_flags = config.game_type;
	ctx.np_protocol.build_flags = build_server_config_flags(ctx);

	if (ctx.is_authority) {
		start_server(ctx, startup);
		// §5.2a step 1 — round/local-player context init at session create. [orig:
		// CNapiGameSession_BuildAndCreateSession @0x5694d0 + SinglePlayer_StartMission @0x561af0 both
		// call Server_InitNewRoundState @0x51c8e0 here.] Previously declared but never invoked in
		// production (the burst drove everything); wiring it at its faithful call site retires the
		// dead-code path.
		Server_InitNewRoundState(ctx);
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
		self.type = 2;          // client-side connection [orig: NapiNPConnection_Create type 2]
		self.phase = ConnectionPhase::New; // advanced by the host's own-player spawn flow (P3)
		self.link.transport = local_client;
		self.link.mode = netsim::TransportMode::Loopback; // socketless mode 1
		ctx.np_protocol.connection_list.push_back(self);
		ctx.np_protocol.next_connection_id = kFirstJoinerDcb;
	}
}

} // namespace opennova::np
