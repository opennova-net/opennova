// The host role: the SP/LAN listen server and the dedicated host. It owns the
// in-match host state (the loopback the host's own client rides, the np host
// owner, the HostClient replica runtime) and runs the ONE listen frame
// [orig: Game_ProcessMainFrame @0x5263f0] every fixed tick. ADR 0043 d3: the
// role replaces inmatch::listen_host::frame and the embedders' pumps around it.
#pragma once

#include <net/npwire/idatagram_socket.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/disconnect_reason.h> // ConnectionErrorRecord (the router's reason text)
#include <runtime/inmatch/host_session.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/replica_track.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_text.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

// Dialog_UpdatePlayback's load of a line on the authority: the line's chat legs on its own
// client (none on a Serve Only host, which plays no dialog), then the S2C 0x28 to every in-match
// slot but the listen host's own (Server_BroadcastDialogLine). The device has loaded and played
// the line's wave first. [orig: Dialog_UpdatePlayback @0x44E470 - Dialog_LoadAudioClip
//  @0x44E599, then Server_SendEntityStateToAll(dialog+4, entry) @0x44E5A5]
void authority_dialog_line_loaded(NapiNPServerCtx &ctx, ClientRuntime *own_client,
		const world::World &world, const audio::DialogLineRef &line);

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
	// over the role's socket): an in-session authority's load runs the server
	// protocol's receive, its handlers and its send at the load's pump points,
	// with the load's spawn gate set (NapiNPServerCtx::net_spawn_suspended,
	// D-NET-116): a map change's teardown pump (the gate clear) and its load's first,
	// then the load's window (load_window_pumps): a map change's from its load's head
	// through its stages and its bring-up, a fresh session's all after its bring-up
	// (with its first) [orig: Server_PumpNetworkTransport @0x4FD960, called by
	//  Game_TeardownMission @0x522588 and Game_StartMission @0x5245E8 .. @0x525AC5]. `count` pumps back to back: each steps every
	// connection's send clock, as each of retail's does (its receive's holdoff step).
	void pump_load(int count = 1);
	// The load's pump count from the round-over clear to the PreMission pass.
	int load_window_pumps() const;
	// The window's render arm, its last pumps, which retail's mission start runs between its
	// render device's mission inits: one after Render_InitRenderingSubsystems (an own
	// client's only: the dedicated test skips that init, its pump and the terrain's), one
	// after Render_InitTerrainSubsystems, one after Render_InitMissionTextures and the fonts.
	// [orig: Game_StartMission - Render_InitRenderingSubsystems @0x5259D1,
	//  Server_PumpNetworkTransport @0x5259EC, Render_InitTerrainSubsystems @0x5259FD (the
	//  dedicated test @0x5259B4 skips the three), @0x525A5C, Render_InitMissionTextures
	//  @0x525A6E, HUD_InitAllFonts @0x525A9A, BoxTexture_LoadAndSetupUVRegions @0x525AC0,
	//  @0x525AC5]
	int render_arm_pumps() const;
	// An embedder that runs its render device's mission inits after the boot's phase A
	// (HostBootRequest::render_arm_deferred): the bring-up's window leaves the render arm's
	// pumps owed, for the embedder's device stages and the load end (finish_mission_load).
	void set_render_arm_deferred(bool deferred) { render_arm_deferred_ = deferred; }
	// The render arm's pumps ahead of the mission's textures: all the owed ones but the last
	// (an own client's two, a dedicated host's one) [orig: Game_StartMission @0x5259EC,
	//  @0x525A5C, ahead of Render_InitMissionTextures @0x525A6E].
	void pump_render_arm_ahead_of_textures();
	// The render arm's pumps still owed: its last, after the mission's textures and the
	// fonts (@0x525AC5), and any the embedder's stages left. Phase B of a deferred boot runs
	// it ahead of the PreMission pass, else the load end (finish_mission_load).
	void pump_render_arm_owed();
	int render_arm_pumps_owed() const { return render_arm_pumps_owed_; }
	// A map change's next mission start, its world-free head, ahead of the load's terrain:
	// the file-transfer session and the report clocks, the mission text and the "Server"
	// strings, the continued session (its streams rebuilt) and the per-slot reset. The
	// embedder's effect system loads next (a host with its own client), then
	// open_next_mission_load_window. `head` is the bring-up record as the boot builds it (its
	// integrity tables the previous mission's until the load's def loads;
	// HostBringup::next_mission). The embedder runs it, or phase A does when no one did.
	// [orig: Game_StartMission @0x524360 - the add @0x5244B4 / the store @0x5244C6,
	//  CServerTick_SetState @0x5244A4 / @0x5244AF, TextResource_LoadMissionTextBin @0x5247CE,
	//  Game_SerializeMissionInfoToDataStream @0x5247EB, CNapiGameSession_InitRandomSeedOrRequest
	//  @0x5248A7, Server_ResetAllPlayerSlotFrameState @0x5248BC; the effect system's init
	//  @0x524980..0x5249D4 follows]
	void begin_next_mission_load(HostBringup head);
	// The head's window past the effect system: the console's clear, then the window's first
	// two pumps, so the reset's 0x25s and a kept joiner's reload legs meet the load's pumps
	// while it loads. Between the head and the window the embedder loads its effect system on
	// every load but a dedicated host's (the game's world load: GameWorld::load_mission_internal),
	// ahead of the terrain, the objects and the runtime (D-NET-NEW-L3-22). A dedicated host
	// loads none at all; its freeze still builds the mission's model buffers, with none of the
	// render arm's device inits (D-NET-NEW-L3-23) [orig: Render_InitMissionTextures @0x587120 -
	//  the dedicated test @0x58713C -> @0x58727A: PolyTrn_InitTextures @0x58727A,
	//  CEffectWorld_RebuildAllModelBuffers @0x58728C; Game_StartMission - the render arm's test
	//  @0x5259B4, the freeze's call @0x525A6E].
	// [orig: Game_StartMission - the dedicated test (in session, the authority, no own client)
	//  @0x524958..0x524979, CEffectSystem_Init @0x524980, CEffectManager_RegisterAllMaterials
	//  @0x52499E, CEffectSystem_BuildTextureAtlases @0x5249B9, Precipitation_Reset @0x5249D4, all
	//  under is_first_load @0x52496C; Chat_ClearAllChannels @0x524A10, Server_PumpNetworkTransport
	//  @0x524A63 and @0x524F76; the router's pump @0x5245E8 precedes the head]
	void open_next_mission_load_window();
	// One of the load's stage pumps between the head and the bring-up: after the terrain,
	// after the mission's document, after the avatars and the environment clock.
	// [orig: Game_StartMission - Server_PumpNetworkTransport @0x524F8C (after
	//  Game_LoadTerrainDuringConnect @0x524F7C), @0x525256 (after Mission_LoadBMSFile
	//  @0x524FFA), @0x5253EA (after Avatars_LoadComboModels @0x525267 and
	//  CAIPathData_LoadAINFile @0x52534F)]
	void pump_load_stage();
	// The head ran for the load in hand (bring_up_next_mission spends it).
	bool next_mission_load_begun() const { return next_mission_head_run_; }
	// The map change's round init over the kept slots, which the host boot
	// runs after the kernel boot's PreMission pass and the round counters
	// (map_change.h) [orig: Game_StartMission @0x524360, the
	// Server_InitAllPlayerEntitiesForRound call @0x525BAF].
	void init_round_after_premission();
	// Player_InitPlayer's body leg at every mission start, after the round
	// init (the class and animSlot by the body's team, the local profile's name,
	// the hidden bit cleared), then the mission start's own local-player block,
	// which leaves every host's local body hidden (a playing host's own join
	// clears it in frame 1's receive pump)
	// [orig: Game_StartMission -- the Player_InitPlayer call @0x525BBC, the block
	//  @0x525C6B..0x525C9E; Player_InitPlayer @0x4E15F0 -- the side words
	//  @0x4E1809..0x4E1843, the Name @0x4E184F..0x4E186A, `Flags &= ~1` @0x4E19C2].
	void init_local_player_at_mission_start();
	// A fresh session's first mission start, at its round init: the own slot's buffer
	// seeded from the spawn buffer, which the mission load filled by then (a map
	// change's round init seeds its own, init_all_player_entities_for_round).
	// [orig: Game_StartMission @0x525BAF -> Server_InitAllPlayerEntitiesForRound
	//  @0x516BC4..0x516BE3, after the load's Mission_LoadBMSFile @0x40F4E0]
	void seed_own_slot_at_first_round_init();
	// The playing host's own client's mission-start end, as every session
	// peer's: its C2S 0x0B queued and pumped onto the loopback, which the next
	// frame's receive pump hands to the server's 0x0B receiver, whose join
	// enters the host's own slot into the game (D-NET-NEW-L4-5). The kernel's
	// complete_mission_start runs it last (MissionKernel::net_mission_start_end,
	// installed with the client view), ahead of finish_mission_load
	// [orig: Game_StartMission @0x526189..0x526255, ahead of @0x526267].
	void send_own_mission_status();
	// The mission load's end: the render arm's pumps still owed (pump_render_arm_owed; none
	// past a deferred boot's phase B, which ran them), then on the authority the S2C 0x7B to
	// every slot, then the load's spawn gate clears. No pump follows: the 0x7B leaves at the
	// first tick's send pump. Phase B of the host boot runs it, or the embedder after it
	// (end_host_mission_load) [orig: Game_StartMission @0x52625F..0x526267, the clear
	//  @0x52631B; its last server pump @0x525AC5; Server_TickUpdate's send pump @0x51E487].
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
	// the shell's mission text and terrain tiles). The rules word is the
	// config's mpattrib in a session and 0x4204 for a socketless host, the
	// shell's single player (D-NET-405).
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

	// The C2S drain the host's own client feeds: the first leg of the receive pump the
	// server tick runs after its per-player walk (host_session_pump's own-client hook).
	void drain_host_client_gameplay_requests();
	// The authority-local fire gate for this frame's weapon walk: in an MP
	// session, the host's own player slot must be active against its own
	// client's tick (PlayerSlot_IsActive); a host with no slot for its player,
	// or outside a session, admits. [orig: Entity_FireWeaponAndSendPacket
	// @0x42be12..0x42be44 -- PlayerSlot_IsActive @0x42be3a over
	// Entity_ValidatePtr(shooter) @0x42be0a and g_ClientCurrentTick]
	bool local_fire_admitted() const;

	bool send_medic_request() override;
	bool send_respawn_request() override;
	// A stance select on the authority past the dispatcher's head
	// (Role::request_stance): the key's own refusals, then the C2S 0x1D on its own
	// client connection -- every press sends, the selected stance included, and
	// nothing latches until its own server handles the 0x1D on the next frame.
	// 0 stand / 1 crouch / 2 prone.
	// [orig: Input_HandleActionBinding_0 cases 169/170/172 @0x4e0d77..0x4e0e87,
	//  CNapiNetwork_QueueReliableMessage @0x4e0de7 with no is_authority test]
	bool send_stance_request(int stance) override;
	void run_tick(const TickInput &input) override;
	// The mission exit the frame stored: the round end's linger (3 or 4,
	// the map change's cue), the NovaWorld session's end, or a world-side
	// writer (the SP end screens, the round-over keys, the in-game RESTART)
	// [orig: Server_TickUpdate @0x51DB57/@0x51DB63 g_MissionExitReason 4/3;
	//  read by Game_ProcessMainFrame @0x526806..0x526867].
	bool session_lost(SessionError &error) const override;
	bool reset_to_baseline(SessionError &error) override;
	// The host's mission exit: the round-reset 0x25 queued to every in-match remote
	// (nothing transmits it), then on the destroy arm the "NP.C:SH:STOP" burst to
	// every connection.
	void close() override;
	// The process's network shutdown (opennova-serve's exit tail): a server an error-arm exit
	// left unstopped writes its /INOUT lines, its socket already gone
	// [orig: NapiLib_Shutdown - ResetActiveSession called @0x4C958B behind +0x68 @0x4C9586].
	void stop_unstopped_server_at_exit(const ServerLogs &logs);
	// The record the post-mission router's reason text reads on this host (exits 9..19):
	// ctx+0xE60, the own client connection, its record its leave latched or none; absent on a
	// host with no own client, whose text is ERR1 (disconnect_reason.h).
	// [orig: PostMenu_RouteMissionExit @0x568460 - CNapiNetwork_GetDisconnectReasonString called
	//  @0x5686D0; CNapiNetwork_GetDisconnectReasonString @0x4C7000 - +0xE60 read @0x4C7001, null
	//  ERR1 @0x4C701D, the record's DC @0x4C708B, DSTR @0x4C709C, DPC @0x4C70D4]
	std::optional<ConnectionErrorRecord> own_client_error_record() const;
	ClientRuntime *client_runtime() override { return state.client_runtime.get(); }
	int64_t last_net_us() const override { return last_net_us_; }
	// The FR counter also reaches the server context: the send window's
	// frame-pressure term and the 0x0A server-fps byte read it.
	void observe_frame_rate(int32_t fps) override;
	// ... as does the window's CPU share, ahead of the drain: the 0x0A
	// server-status CPU byte reads it in this frame's ticks.
	void observe_cpu_share(int32_t cpu_percent) override;
	// ... and the frame statistics its status page shows.
	void observe_frame_statistics(int32_t frames_last_second, int32_t cpu_percent) override;
	// The /PROFILE frame leg: run_tick's tail and every paused update's.
	void write_profile_frame() override;

private:
	void reset_state(const inmatch::GameConfig &config, bool serve_and_play, bool in_session,
			const TitleIdentity *title);
	// The rule words the world reads at tick time, from the session config and the title the
	// session speaks (null: JO's).
	void apply_rule_words(const inmatch::GameConfig &config, bool serve_and_play, bool in_session,
			const TitleIdentity *title);
	// The context's spawn-time body bind over this role's kernel
	// (NapiNPServerCtx::bind_spawned_body), installed after a bring-up's own spawn.
	void install_spawned_body_bind();
	void make_client_runtime(uint32_t game_type);
	// The own client's S2C 0x0F local-player block on the host's own body (the one body the
	// host and its client share), run once per decoded 0x0F.
	static void apply_own_world_state_load(mission::MissionKernel &kernel,
			const replication::ClientState &own);
	// The session create's UP-TIME stamp, kept across a single player's restart boot.
	void keep_round_state_init_tick();
	// Out of a session, the single player's fill of the spawn buffer: its static arm.
	void fill_spawn_buffer_out_of_session();
public:
	// A line the world's dialog queue loads on this host (authority_dialog_line_loaded).
	void dialog_line_loaded(const audio::DialogLineRef &line);
private:
	// The client words the host's own runtime steps: the embedder's (HostConfig::client_process),
	// else the role's own, which every runtime the role makes (a map change, a retry) shares.
	ClientProcessState own_client_process_;
	ClientProcessState *client_process_ = &own_client_process_;
	// The map change's bring-up: the kept session onto the next mission's
	// kernel (map_change.h).
	void bring_up_next_mission(const HostBringup &bringup);

	RoleKind kind_ = RoleKind::ListenHost;
	HostBringup staged_bringup_;
	CreateSessionResult session_create_ = CreateSessionResult::Created;
	// A map change's load: its head ran (begin_next_mission_load), and the window's pumps
	// the head and the stages ran ahead of the bring-up, which runs the rest.
	bool next_mission_head_run_ = false;
	int window_pumps_run_ = 0;
	// The embedder runs the render arm (set_render_arm_deferred), and its pumps the bring-up
	// left owed.
	bool render_arm_deferred_ = false;
	int render_arm_pumps_owed_ = 0;
	opennova::IDatagramSocket *socket_ = nullptr;
	HostRotation *rotation_ = nullptr;
	std::shared_ptr<const replication::ItemReplicationCatalog> item_catalog_;
	int64_t last_net_us_ = 0;
    uint64_t local_round_reset_seen_ = 0;
    // The own client's 0x0F / local 0x50 track legs consumed (replica_track.h).
    ReplicaTrackSeen track_seen_;
    // The host's own connection as the kernel's last baseline capture found it (the
    // mission start's, ahead of its own 0x0B, or a later one): a rewind restores both
    // (reset_to_baseline).
    std::optional<NapiNPConnection> own_connection_at_baseline_;
};

} // namespace opennova::inmatch
