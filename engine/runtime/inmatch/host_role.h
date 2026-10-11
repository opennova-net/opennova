// The host role: the SP/LAN listen server and the dedicated host. It owns the
// in-match host state (the loopback the host's own client rides, the np host
// owner, the HostClient replica runtime) and runs the ONE listen frame
// [orig: Game_ProcessMainFrame @0x5263f0] every fixed tick. ADR 0043 d3: the
// role replaces inmatch::listen_host::frame and the embedders' pumps around it.
#pragma once

#include <net/npwire/idatagram_socket.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/replica_track.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_text.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::playersav {
struct ProfileRecord;
}

namespace opennova::inmatch {

struct ListenHostState {
	replication::LoopbackChannel host_loop; // the local dcb-2 client; declared before the runtime
	inmatch::HostOwner host_owner;
	std::unique_ptr<inmatch::ClientRuntime> client_runtime;
	// Deterministic overrides for the clock-minted session identity (the tick
	// digest pins them); zero = mint as the original does.
	uint32_t host_key = 0;
	uint32_t host_start_tick = 0;
	uint32_t session_seed_id = 0;
};

// What a shell hands the general bring-up beyond the host config: the S2C
// 0x45 terrain-tile source, the mission text the initial-state burst streams
// (the briefing pages and the location-name table), and the gametext
// "Server" strings the host's handlers print through, which the bring-up
// installs on the fresh server context (D-NET-344: every host carries them,
// a Serve Only host too) [orig: Game_InitSubsystems @0x4A6CD0 loads
// gametext.bin; Server_BroadcastMedicRequest @0x5153C9; the team change's
// GameText_GetString("server", "C2Blue" / "C2Red") @0x51902E / @0x51909C].
struct HostBringup {
	inmatch::HostConfig host_cfg;
	std::vector<uint8_t> terrain_til_data;
	mission::MissionText mission_text;
	ServerTextTable server_text;
	// The map change's bring-up: the next mission inside the kept session
	// (inmatch/map_change.h), not a fresh session.
	bool next_mission = false;
};

// The SP listen server's session config: SINGLEPLAYERGAME, the literal
// attribute word and one player, the mission's own game type. The SP launcher
// stores the attribute word LITERALLY after Game_SaveConfig (not the cfg
// default 0x3A02) and one player, then copies both into the game settings
// [orig: SinglePlayer_StartMission — `multiplayerAttributeFlags_34C = 14854`
//  @0x561bb7 -> game_settings.mp_attributes @0x561cdb; maxPlayers_3F4 = 1
//  @0x561c1d -> game_settings.max_players = 1 @0x561cec].
// Outside a session the session words a host takes from game.cfg come from
// the current player profile record instead: the four charattr restriction
// words (the record's +1352..+1364; null = a fresh profile's, every one 0)
// [orig: Game_ApplySessionSettingsToGlobals @0x551F15..0x551F3F] and the
// difficulty word, +1380 (profile_difficulty, which the bring-up seeds the
// world's word with, @0x551F6F..0x551F75). The difficulty byte is the word
// the mission start leaves out of a session (world::mission_start_difficulty),
// which overwrites the profile's before any serializer reads it [orig:
// Game_StartMission @0x525CDD].
GameConfig singleplayer_game_config(uint32_t game_type,
		const playersav::ProfileRecord *profile = nullptr);

class HostRole final : public Role {
public:
	HostRole();
	// The embedder's form: the session kind it runs under and the items.def
	// catalog every HostClient view it builds takes (null = none yet; the
	// embedder installs one through set_item_catalog once it exists).
	explicit HostRole(RoleKind kind,
			std::shared_ptr<const replication::ItemReplicationCatalog> item_catalog = {});
	RoleKind kind() const override { return kind_; }
	// The session kind this host runs under: a shell's SP listen server keeps
	// SinglePlayer (pause/step/reset stay available); a LAN host is ListenHost
	// or DedicatedHost by serve_and_play.
	void set_kind(RoleKind kind) { kind_ = kind; }

	ListenHostState state;

	// The socket the host pump reads and writes; null = the socketless
	// (SP / test) host, every datagram dropped.
	void set_socket(opennova::IDatagramSocket *socket) { socket_ = socket; }
	// The session-level rotation state the embedder owns for its whole run
	// (inmatch/mission_rotation.h); null = no rotation (the session ends at
	// its first round's end). Every bring-up installs it on the context.
	void set_rotation(HostRotation *rotation) { rotation_ = rotation; }
	HostRotation *rotation() const { return rotation_; }
	// The map change's state between its steps (inmatch/map_change.h).
	HostMapChange map_change;
	// The host's network pump while it loads a mission (host_session_load_pump
	// over the role's socket).
	void pump_load();
	// The map change's round init over the kept slots, which the host boot
	// runs after the kernel boot's PreMission pass and the round counters
	// (map_change.h) [orig: Game_StartMission @0x524360, the
	// Server_InitAllPlayerEntitiesForRound call @0x525BAF].
	void init_round_after_premission();
	// The mission load's end on the authority: the S2C 0x7B to every slot,
	// and after a map change's bring-up a pump. Phase B of the host boot runs
	// it [orig: Game_StartMission @0x52625F..0x526267].
	void finish_mission_load();
	// The shell's items.def catalog for the HostClient's view (the wire class
	// and the def facts of a type id); re-installed whenever the role rebuilds
	// that runtime.
	void set_item_catalog(std::shared_ptr<const replication::ItemReplicationCatalog> catalog);

	// The SP listen server: SINGLEPLAYERGAME, one player, the mission's own
	// game type, socketless, its session words the profile record's
	// (singleplayer_game_config) [orig: SinglePlayer_StartMission @0x561af0].
	void bring_up_singleplayer(const playersav::ProfileRecord *profile = nullptr);
	// The shell's general bring-up (the LAN host or its SP listen server with
	// the shell's mission text and terrain tiles).
	void bring_up(const HostBringup &bringup);
	// The embedder's bring-up record for the next boot-hook bring_up(): staged
	// right before the kernel boots (the text, tiles and config are final by
	// then), consumed by the hook.
	void stage_bringup(HostBringup bringup) {
		staged_bringup_ = std::move(bringup);
		session_create_ = CreateSessionResult::Created;
	}
	bool bring_up() override;
	// The last bring-up's session create: ProcessExit when the config's
	// `mpreset` word refused it (server_session.h), which leaves the role with
	// no session and the embedder to end its process with code 0; a map
	// change's bring-up continues the session and creates none, so it is
	// Created [orig: CNapiGameSession_CreateSession @0x4C97E7..0x4C97F0].
	CreateSessionResult session_create() const { return session_create_; }

	// The C2S drain the host's own client feeds before the server tick.
	void drain_host_client_gameplay_requests();
	// The authority-local fire gate for this frame's weapon walk: in an MP
	// session, the host's own player slot must be active against its own
	// client's tick (PlayerSlot_IsActive); a host with no slot for its player,
	// or outside a session, admits. [orig: Entity_FireWeaponAndSendPacket
	// @0x42be12..0x42be44 -- PlayerSlot_IsActive @0x42be3a over
	// Entity_ValidatePtr(shooter) @0x42be0a and g_ClientCurrentTick]
	bool local_fire_admitted() const;

	bool send_medic_request() override;
	// A stance key on the authority: the key's own refusals, then the C2S 0x1D
	// on its own client connection -- every press sends, the selected stance
	// included, and nothing latches until its own server handles the 0x1D on
	// the next frame. 0 stand / 1 crouch / 2 prone.
	// [orig: Input_HandleActionBinding_0 cases 169/170/172 @0x4e0d77..0x4e0e87,
	//  CNapiNetwork_QueueReliableMessage @0x4e0de7 with no is_authority test]
	bool request_stance(int stance);
	void run_tick(const TickInput &input) override;
	// The mission exit the frame stored: the round end's linger (3 or 4,
	// the map change's cue), the NovaWorld session's end, or a world-side
	// writer (the SP end screens, the round-over keys, the in-game RESTART)
	// [orig: Server_TickUpdate @0x51DB57/@0x51DB63 g_MissionExitReason 4/3;
	//  read by Game_ProcessMainFrame @0x526806..0x526867].
	bool session_lost(SessionError &error) const override;
	bool reset_to_baseline(SessionError &error) override;
	// The host's mission exit: the round-reset 0x25 to every in-match remote,
	// the "NP.C:SH:STOP" description on every connection, one final flush.
	void close() override;
	ClientRuntime *client_runtime() override { return state.client_runtime.get(); }
	int64_t last_net_us() const override { return last_net_us_; }
	// The FR counter also reaches the server context: the send window's
	// frame-pressure term and the 0x0A server-fps byte read it.
	void observe_frame_rate(int32_t fps) override;
	// ... and the frame statistics its status page shows.
	void observe_frame_statistics(int32_t frames_last_second, int32_t cpu_percent) override;

private:
	void reset_state(const inmatch::GameConfig &config, bool serve_and_play, bool in_session);
	// The rule words the world reads at tick time, from the session config.
	void apply_rule_words(const inmatch::GameConfig &config, bool serve_and_play, bool in_session);
	// The context's spawn-time body bind over this role's kernel
	// (NapiNPServerCtx::bind_spawned_body), installed after a bring-up's own spawn.
	void install_spawned_body_bind();
	void make_client_runtime(uint32_t game_type);
	// The map change's bring-up: the kept session onto the next mission's
	// kernel (map_change.h).
	void bring_up_next_mission(const HostBringup &bringup);

	RoleKind kind_ = RoleKind::ListenHost;
	HostBringup staged_bringup_;
	CreateSessionResult session_create_ = CreateSessionResult::Created;
	opennova::IDatagramSocket *socket_ = nullptr;
	HostRotation *rotation_ = nullptr;
	std::shared_ptr<const replication::ItemReplicationCatalog> item_catalog_;
	int64_t last_net_us_ = 0;
    uint64_t local_round_reset_seen_ = 0;
    // The own client's 0x0F / local 0x50 track legs consumed (replica_track.h).
    ReplicaTrackSeen track_seen_;
};

} // namespace opennova::inmatch
