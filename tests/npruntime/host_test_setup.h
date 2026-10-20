#pragma once

// Shared listen-host bring-up for the npruntime tests, so every test stands the host up through the
// real lifecycle (set_connection_mode -> set_transport_mode -> create_session). After this the host
// is_in_session + (when authority) host_running, so the gated handshake legs admit a join.

#include <runtime/inmatch/joiner_connection.h> // build_mission_status (the own C2S 0x0B)
#include <runtime/inmatch/napi_np_protocol.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h> // Server_HandlePlayerJoinRequest

#include <runtime/inmatch/session_transport.h>

#include "fresh_profile_oracle.h"

#include <base/gameprofile/gameprofile.h>
#include <net/title/title_identity.h>

#include <vector>

namespace opennova::inmatch::test {

// Bring a listen/dedicated host fully up, speaking JO's title identity. `host_key` is seeded onto
// the host (and advertised in ServerHello.hk / checked against ClientAuth.hk); `local_client`, when
// non-null with HostClient, registers the host's own type-2 loopback connection.
inline void bring_up_host(NapiNPServerCtx &ctx, ConnectionMode mode, SocketMode socket,
                          uint32_t host_key, replication::ISessionTransport *local_client,
                          const GameConfig &config) {
	set_connection_mode(ctx, mode);
	set_transport_mode(ctx, socket);
	SessionStartup startup;
	startup.title = title_identity(gameprofile::GAME_JO);
	startup.host_key = host_key;
	create_session(ctx, config, startup, local_client); // P1: is_in_session=1, host_running=1
	// start_host_session installs the host's own per-side character selection on the
	// type-2 loopback right after create_session; the tests carry the stock fresh-profile
	// seed (side A 0x0200 / avatar 1 = the golden host record) unless a test overrides it.
	for (NapiNPConnection &connection : ctx.np_protocol.connection_list) {
		if (connection.type == 2) connection.char_vars = stock_fresh_profile_character_vars();
	}
}

// The stock-config form: GameConfig's defaults, except the cap. Its default of 1 is a single
// player's; slot 0 is the authority's own row on every host (the listen host's player, a Serve Only
// host's connectionless slot, whose published cap is the cfg max plus one), so the smallest cap
// that admits a joiner is 2 (D-NET-350).
// [orig: Server_InitNewRoundState @0x51c99a..0x51ca3c; HostDialog_StartSession @0x5587d7..0x5587e5]
inline void bring_up_host(NapiNPServerCtx &ctx, ConnectionMode mode, SocketMode socket,
                          uint32_t host_key = 0,
                          replication::ISessionTransport *local_client = nullptr) {
	GameConfig config;
	config.max_players = 2;
	bring_up_host(ctx, mode, socket, host_key, local_client, config);
}

// The host's own client's C2S 0x0B: its mission start's end queues it and the server's next
// receive pump hands it to the 0x0B receiver, whose join returns the own slot's messages in
// order (the two zero map checksums every 0x0B carries; the own slot is never punted)
// (D-NET-NEW-L4-5). The own slot is in state 5, its world stream ended.
// [orig: Game_StartMission @0x52624B; NapiNPServerMsg_PlayerJoinRequest @0x51AB10 ->
//  Server_OnPlayerJoin @0x51ABA1]
inline std::vector<InitialStateMessage> host_own_join(NapiNPServerCtx &ctx, NapiNPConnection &own,
                                                      uint32_t now_tick) {
	return Server_OnPlayerJoin(ctx, own, now_tick, {0, 0});
}

// The same 0x0B through its receiver, as the first frame's receive pump hands it over: the
// join's messages go out on the own connection's loopback. False when there is no own
// connection. [orig: NapiNPServerMsg_PlayerJoinRequest @0x51AB10]
inline bool deliver_host_own_mission_status(NapiNPServerCtx &ctx, uint32_t now_tick) {
	for (NapiNPConnection &own : ctx.np_protocol.connection_list) {
		if (own.type != NapiNPConnection::kTypeClientSide) continue;
		std::vector<ProtocolMessage> replies;
		Server_HandlePlayerJoinRequest(&ctx, own, ctx.world, now_tick,
				build_mission_status({}, 0), replies);
		return true;
	}
	return false;
}

// The host's own type-2 burst to its world stream's end (back to back, unpaced), then its
// 0x0B's join: what the bring-up and the next frame's receive pump send it, in order. False
// when the stream does not end in slot state 5.
inline bool drive_host_own_burst_and_join(NapiNPServerCtx &ctx, NapiNPConnection &own,
                                          uint32_t now_tick,
                                          std::vector<InitialStateMessage> &emitted) {
	for (int i = 0; i < 64 && own.slot_state != PlayerSlotState::AwaitJoin; ++i) {
		InitialStateStep step = Server_SendInitialGameStateToPlayer(ctx, own, now_tick);
		if (!step.advanced) break;
		for (InitialStateMessage &m : step.messages) emitted.push_back(std::move(m));
	}
	if (own.slot_state != PlayerSlotState::AwaitJoin) return false;
	for (InitialStateMessage &m : host_own_join(ctx, own, now_tick)) emitted.push_back(std::move(m));
	return true;
}

} // namespace opennova::inmatch::test
