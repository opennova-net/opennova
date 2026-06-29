#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include <mission/event_runtime.h>
#include <mission/promote.h>
#include <terrain/height_field.h>
#include <wac/wac_system.h>

#include "wac/nova_wac_program.h"
#include <world/ai.h>
#include <world/player_input.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include "mission/nova_mission_data.h"
#include "simulation/infantry_root_motion.h"

#include "netsim/loopback_channel.h"          // host_loop_ (the host's own dcb-2 client)
#include "netsim/udp_session_transport.h"     // PeerLink::transport (the LAN per-peer transport)

#include <novaworld/peer_addr.h>    // PeerAddr / PeerAddrHash
#include "network/nova_udp_pump.h"

#include <mission/bms.h>                      // bms::File (persisted so ctx_.mission outlives the match)
#include <npruntime/napi_np_server_ctx.h>     // NapiNPServerCtx / NapiGameSettings / ConnectionMode / SocketMode
#include <npruntime/napi_np_protocol.h>       // HostAcceptEvent + the host owner-loop entry points
#include <npruntime/client_runtime.h>         // ClientRuntime (HostClient / Joiner roles)
#include <npruntime/host_session.h>           // HostOwner + host_session_pump (the shared host owner loop)

#include <unordered_map>

namespace godot {

class NovaTerrainData;

// THE mission runtime binding: a thin host shell over the portable libs/world runtime.
// Owns one World + the three logic systems (WAC VM, BMS event evaluator, AI) and drives
// them through World::run_logic_tick — one logic tick per host frame, the original's
// 62 Hz engine tick (current_tick in Game_ProcessMainFrame @0x5263f0). The per-system
// cadences live INSIDE the systems, as in the original: the WAC VM self-gates to every
// 62nd tick (sub_4F81A0 @0x4f81b1) and the BMS evaluator quarter-passes every 16th
// (Server_TickUpdate @0x51d7e0). Both the editor "Play the mission" preview and the game
// runtime go through this one path: promote a parsed BMS mission into the world
// (mission/promote.h), register the systems in the faithful order
// (mission/mission_systems.h), run a pre-mission pass, then tick. Entity transforms
// (mission space -> Godot space) and the part-anim phase are exposed for a scene/renderer
// to draw; host-presentation side effects (text/dialog/win) drain out of the World
// EffectLog each tick. Play/Pause/Step + Stop (snapshot/restore).
class NovaSimulation : public Node3D {
	GDCLASS(NovaSimulation, Node3D)

public:
	enum TickMode {
		// Both modes run one logic tick per process frame today (the 62-divider moved
		// into WacSystem where the original keeps it); the enum survives for API
		// stability and for a future fixed-62 Hz accumulator in DIVIDED.
		TICK_DIVIDED = 0,
		TICK_EVERY_PROCESS = 1
	};

	// Field layout of one entity record in get_present_snapshot()'s flat float buffer. ONE batched
	// PackedFloat32Array call replaces the per-entity scalar getters in the per-tick present loop
	// (the scalar getters box a Variant each; see feedback_dispatcher_callable_perf). Mirrored on the
	// GDScript side via these bound constants so the layout has a single source of truth (C++).
	// Rotation is emitted as mission-space degrees (pitch, yaw, roll) so the host builds the basis
	// through the one placer convention (MissionObjectPlacer.bms_to_godot_basis); position is already
	// in Godot space (x, z, -y). Pitch/roll are 0 today (yaw-only locomotion) — reserved for parity.
	enum PresentField {
		PF_KIND = 0,   // mission ItemType (3 = Organic), -1 if none
		PF_INDEX,      // index within its kind's list
		PF_BMS_ID,     // file entity id; host maps this to a placed node (primary key)
		PF_NET_ID,     // runtime SSN (WAC/BMS addressing)
		PF_POS_X,      // Godot-space position (mission (x,y,z) 16.16 -> (x, z, -y) units)
		PF_POS_Y,
		PF_POS_Z,
		PF_PITCH_DEG,  // mission-space rotation, degrees (0 today; reserved)
		PF_YAW_DEG,
		PF_ROLL_DEG,   // (0 today; reserved)
		PF_PHASE1,     // PANM channel 1 phase 0..65535
		PF_ACTIVE1,    // 1 when channel 1 has a live part-anim to render, else 0
		PF_PHASE2,     // PANM channel 2 phase
		PF_ACTIVE2,
		PF_ANIM_SLOT,  // Entity.anim_slot (main-body .bad/.adm clip; consumed only by the deferred seam)
		PF_ANIM_STATE, // InfantryState.anim_state (full off_8135F0 state id; -1 when unavailable)
		PF_ANIM_PHASE_TICKS, // InfantryState.clip_phase, in IDA half-frame ticks
		PF_HIDDEN,     // 1 when the entity is hidden
		PF_ALIVE,      // 1 when alive
		PF_TYPE_ID,    // items.def runtime type id from the wire (0 = none); keys the joiner's wire avatars
		PF_WIRE_HANDLE,// (pool<<12)|slot wire handle from the decoded stream (joiner render key; 0 = none)
		PF_STRIDE      // record length; also the count of fields above
	};

private:
	std::unique_ptr<opennova::world::World> world_;
	std::unique_ptr<opennova::world::AiSystem> ai_;
	std::unique_ptr<opennova::mission::BmsEventSystem> bms_;
	std::unique_ptr<opennova::wac::WacSystem> wac_;
	// The installed script program. Held as a Ref so it survives reset_world();
	// finish_load() re-applies it onto the fresh WacSystem each (re)load.
	Ref<NovaWacProgram> wac_program_;
	opennova::world::World::Snapshot baseline_; // play-start state, for Stop -> restore
	opennova::mission::PromoteResult promo_;
	bool loaded_ = false;
	bool playing_ = false;
	bool have_baseline_ = false;
	int tick_mode_ = TICK_DIVIDED;

	// --- in-match net runtime (P7, ADR 0009/0011): the SP / LAN host in-process listen server. OFF
	// by default, so the editor / non-net preview path is the direct AI-pool present, untouched. When
	// enabled (before load), bringup_host_runtime stands up the npruntime ctx_ + host_loop_ + runtime_
	// (declared in the P7 block below) and the present pass reads the client-decoded ClientState
	// (ADR 0011 Decision 1) instead of the AI pool. Server_TickUpdate owns the per-frame tick.
	bool listen_server_ = false;
	uint64_t last_sim_tick_us_ = 0;
	uint64_t last_net_tick_us_ = 0;
	mutable uint64_t last_present_snapshot_us_ = 0;
	mutable int last_present_entity_count_ = 0;

	// --- co-op LAN host: a real UDP socket (NovaUdpPump) over the npruntime runtime. enable_host_listen
	// binds the socket (it implies the listen server); host_pump drives the owner loop, and
	// dispatch_event/admit_peer admit joiners + stream the named dcb-bearing 0x0C. host_session_config_
	// holds the GDScript-facing session options (the Dictionary getter + the §5.1 reactive-reply config
	// fed to configure_session_runtime). Sockets live here, the protocol/crypto in libs (ADR 0010).
	bool host_listen_ = false;
	Ref<NovaUdpPump> pump_;
	opennova::np::SessionReplyConfig host_session_config_; // P8: was GameServerRuntimeConfig
	uint16_t host_bind_port_ = 64220;                      // the lobby-advertised bind port (UI only)
	// UI server-type: serve-and-play (default true) spawns + renders the host's own player and folds
	// host_loop_ into runtime_; a DEDICATED host (false) runs the listen server with NO local player and
	// lets host_session_pump discard the loopback (step 5). Mirrors HostConfig.serve_and_play /
	// start_host_session's gating [orig: the §5.0 listen-host bring-up, SinglePlayer_StartMission @0x561af0].
	bool host_serve_and_play_ = true;
	uint32_t host_max_players_ = 16; // the lobby-advertised player cap; clamped host-side to the witnessed 1..65 [orig +0xC0]
	// Build the PF_* present buffer from the client-decoded ClientState (runtime_->state()).
	PackedFloat32Array present_snapshot_from_client_view() const;

	// --- co-op LAN joiner: a pure non-authority np::ClientRuntime (Joiner role, built in enable_join /
	// finish_load; the runtime_ member is declared in the P7 block below). joiner_pump drives the
	// connect legs + the per-frame S2C->ClientState fold + the C2S 0x0C uplink over a dialed NovaUdpPump.
	// It runs run_logic_tick(false) for its own player L (a motor-driven pool-0 entity spawned at the
	// H-learned pose); remote entities render wire-direct (present + wire_present_pass). enable_join
	// turns it on; a sim is host XOR joiner. [orig: NapiNPClientMsg_0x00C @0x42E730 self name-match]
	bool joiner_ = false;
	bool joiner_started_ = false;          // ClientHello emitted (Idle -> Hello)
	bool joiner_local_spawned_ = false;    // L spawned at reached_in_match (one-shot guard)
	uint16_t joiner_self_wire_handle_ = 0; // H: stamped in the C2S 0x0C + present self-filter
	// Send one framed datagram to the dialed host (the joiner's send_datagram).
	void ship_to_host(const std::vector<uint8_t> &dg);
	// SelfSpawn (mission i32 16.16 + full BAM32 orientation) -> PlayerSpawn for L.
	opennova::world::PlayerSpawn spawn_from_self(const opennova::np::JoinerConnection::SelfSpawn &s) const;

	// Phase 2 (the moving player): the latest input from the host controller, applied to the
	// local player's AiEntity at the TOP of each frame (net-before-logic, ADR 0009). The
	// player then locomotes through the same infantry motor as an NPC. [net-re §5.38]
	opennova::world::PlayerInput player_input_{};
	void apply_player_input_pre_tick();

	// --- P7: the in-match runtime as a THIN ADAPTER over libs/npruntime ----------------
	// One in-match runtime funnels every path: the host/SP/editor-preview is the §5.0 mode-3
	// listen server (NapiNPServerCtx ctx_ + its own loopback client over host_loop_, driven by
	// the npruntime owner loop = Server_TickUpdate + tick_connections + handle_server_datagram);
	// the joiner is a non-authority np::ClientRuntime. The Godot net bindings stay PURE socket
	// pumps — all protocol/crypto/framing lives in libs (ADR 0009-0012, .agents/network.md).
	// host_loop_ MUST be declared before runtime_: the HostClient ClientRuntime holds a
	// non-owning reference into host_loop_, so the loopback has to outlive (and not move under)
	// the runtime.
	// The host state — ctx + per-peer transports + now_tick + serve_and_play — shared with the promoted
	// owner loop host_session_pump (libs/npruntime). MUST be declared before ctx_ (the alias) and before
	// host_loop_ (host_owner_.host_loopback points at host_loop_, set at bring-up). Replaces the old
	// ctx_/peers_/PeerLink members; admit_peer/dispatch_event moved into libs (np::, over host_owner_).
	opennova::np::HostOwner host_owner_;
	opennova::np::NapiNPServerCtx &ctx_ = host_owner_.ctx;    // alias: host only (is_authority)
	opennova::netsim::LoopbackChannel host_loop_;             // the host's own dcb-2 client; Server_TickUpdate's 0x0A target
	std::unique_ptr<opennova::np::ClientRuntime> runtime_;    // HostClient (host/SP) OR Joiner; the present-snapshot source
	opennova::bms::File mission_file_;                        // persisted so ctx_.mission outlives the match (the 0x0B burst body)
	opennova::np::NapiGameSettings settings_;                 // filled from configure_host_session (LAN host)
	std::string joiner_player_name_;                          // persisted for the Joiner runtime ctor on (re)load
	uint32_t now_tick_ = 0;                                   // the JOINER's per-frame clock (the host uses host_owner_.now_tick)
	// Per-load host bring-up: mode 3 -> create_session(&host_loop_) -> configure_session_runtime
	// -> Server_InitNewRoundState -> the faithful host-player auto-spawn. Mirrors apps/nw_server.
	void bringup_host_runtime(const opennova::bms::File &file);
	// The per-frame host owner loop (recv-drain -> tick_connections -> Server_TickUpdate -> S2C
	// flush -> fold host_loop_ into ClientState). Socket legs gated on host_listen_ (pure SP has none).
	void host_pump();
	// The per-frame non-authority client loop (recv -> run_logic_tick(false) for L's motor ->
	// Client_ProcessNetworkFrame -> ship the C2S 0x0C; spawn L on the in-match edge).
	void joiner_pump();

	// Terrain the AI grounds on. We own copies of the host's depth buffer + 16x16 sector grid so
	// the portable TerrainHeightField's raw pointers outlive the source NovaTerrainData and survive
	// a reload (reset_world rebuilds ai_; apply_terrain_to_ai re-points it). Empty = no grounding.
	std::vector<uint16_t> terrain_heightmap_;
	std::vector<int> terrain_sector_grid_;
	opennova::terrain::TerrainHeightField terrain_field_;
	void apply_terrain_to_ai();

	// Anim-driven soldier locomotion: the .adm/.bad-backed root-motion source the infantry
	// motor integrates (world/infantry.h). Owned here so it survives reset_world; the fresh
	// ai_ is re-pointed at it like the terrain field. Empty = soldiers hold and stand.
	InfantryRootMotion infantry_anim_;
	void apply_root_motion_to_ai();
	std::vector<opennova::mission::ItemSeatSpec> item_seat_specs_;
	opennova::mission::PromoteOptions promote_options() const;

	void reset_world();
	// Shared post-promote wiring: load the BMS arrays, register the systems, run the
	// pre-mission pass, capture the restore baseline. Marks the sim loaded.
	void finish_load(const opennova::bms::File &file);
	void apply_host_session_mission_header(const opennova::bms::File &file);
	void refresh_host_accept_config();

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	NovaSimulation();

	// Load + promote the editor's live mission (the in-memory bms::File, including unsaved
	// edits). This is the editor-integration entry: simulate exactly what is on screen.
	bool load_from_mission_data(const Ref<NovaMissionData> &p_mission);
	// Load + promote a .bms mission from disk; false on parse failure.
	bool load_mission_file(const String &path);
	// Build + promote a small synthetic patrol mission (no file) for the headless unit test.
	void build_demo_mission();
	bool is_loaded() const { return loaded_; }

	// Transport.
	void set_playing(bool p_playing) { playing_ = p_playing; }
	bool is_playing() const { return playing_; }
	void step();           // advance exactly one logic tick (editor Step)
	bool advance_frame();  // one host frame = one logic tick (the WAC VM self-gates inside)
	void restart();        // Stop: restore the play-start baseline (rewinds world + AI)
	void set_tick_mode(int p_mode) { tick_mode_ = p_mode; }
	int get_tick_mode() const { return tick_mode_; }

	// Turn the sim into an SP in-process listen server (ADR 0011): the host serializes
	// real entity state onto an in-process loopback (Server_TickUpdate's per-connection S2C
	// fan), the local client decodes it, and the present pass reads that decoded state. Call
	// BEFORE loading a mission — the next load stands up the npruntime host runtime. Disabling
	// reverts to the direct AI-pool present (the editor default).
	void enable_listen_server(bool p_enable);
	bool is_listen_server() const { return listen_server_; }

	// --- co-op LAN host (Increment C) ------------------------------------
	// Turn the sim into a co-op LAN HOST: bind a UDP listen socket on `p_port`
	// (0 = an OS-assigned ephemeral port) and accept joiners through the
	// witnessed session handshake, spawning each into the live World on join.
	// Implies enable_listen_server(true) — call BEFORE loading a mission.
	// Returns false if the socket can't bind.
	bool enable_host_listen(int p_port);
	bool is_host_listening() const { return host_listen_; }
	int get_host_listen_port() const;  // the bound UDP port (0 when not listening)
	int get_host_peer_count() const;   // joiners in handshake or admitted
	void configure_host_session(Dictionary p_options);
	Dictionary get_host_session_config() const;
	// Debug/test hook: directly admit a synthetic remote peer at a Godot-space
	// position, exercising the admit_peer + connection wiring without a live
	// socket handshake (the handshake itself is unit-tested in libs —
	// tests/novaworld/host_session_accept_test). Returns true if an entity was
	// spawned + bound. No-op unless host listening is on.
	bool admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team);

	// --- co-op LAN joiner (D.2) -------------------------------------------
	// Turn the sim into a co-op LAN JOINER: dial the host at `host_ip:port` and run
	// the witnessed in-match JOIN as a non-authority client. `player_name` rides the
	// ClientHello.co and is the key the host echoes into our organic-spawn record so
	// we self-identify (name-match) and learn our wire handle H. Call BEFORE loading
	// the mission (the next load arms the joiner frame path). Implies the client view;
	// a sim is host XOR joiner. Returns false if the socket can't be dialed.
	bool enable_join(const String &p_host_ip, int p_port, const String &p_player_name);
	bool is_joiner() const { return joiner_; }
	// True once the joiner has name-matched its organic-spawn record (self handle H known).
	bool is_joined_in_match() const;
	// The JoinerSession phase as an int (JoinerSession::Phase), -1 when not joining.
	int get_joiner_phase() const;
	// The learned wire handle H, 0 until in-match (debug / test).
	int get_joiner_self_handle() const;

	// --- the local player (ADR 0012; net-re §5.2b/§5.38) -------------------
	// Spawn the host's own player as an authoritative pool-0 entity at a Godot-space position
	// (yaw in mission degrees). Call AFTER a mission is loaded (the spawn needs the AI system
	// wired). Returns false if no mission is loaded or pool 0 is full. The player then runs
	// the infantry motor from input (set_player_input), not AI think.
	bool spawn_local_player(Vector3 p_position, float p_yaw_deg, int p_team);
	// Spawn the host's own player at the mission's player-START marker, selected the way the
	// original engine does — by game type, FARTHEST from the enemy set — NOT at any NPC's
	// position (net-re §5.2c). Single-player resolves the type-6002 start marker. Call AFTER a
	// mission is loaded. Returns: 1 = spawned at a real start marker; 0 = no start marker, spawned
	// at a safe fallback origin (never an NPC); -1 = failed (no mission / pool 0 full).
	// [orig: CMap_SetupSpawnCamera @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0]
	int spawn_local_player_at_start();
	// True once a local player has been spawned (World::cached.local_player valid).
	bool has_local_player() const;
	// The local player's wire handle ((pool<<12)|slot), 0 when none. The wire present pass
	// excludes it — the local player is drawn by LocalPlayerHost, not from the wire stream
	// (host: its own pool-0 player; joiner: L, never present in the wire stream anyway).
	int get_local_player_wire_handle() const;
	// Feed one frame of player input: the move keys + look yaw/pitch (mission degrees). Applied
	// to the player's body input at the top of the next frame. The original drives
	// entity Yaw@+0x10 / Pitch@+0x14 straight from the mouse [orig: Input_HandleActionBinding_0
	// @0x4e1330]; the caller clamps pitch to ±80°.
	void set_player_input(bool p_forward, bool p_back, bool p_left, bool p_right, bool p_run,
	                      bool p_crouch, bool p_prone, bool p_jump,
	                      float p_look_yaw_deg, float p_look_pitch_deg);
	// The local player's authoritative position in Godot world space (for the follow camera);
	// Vector3() when no player is spawned.
	Vector3 get_local_player_position() const;
	// The local player's authoritative look yaw / pitch in mission degrees (for the first-person
	// camera). yaw = 90 - heading; pitch up positive. 0 when no player is spawned.
	float get_local_player_yaw_deg() const;
	float get_local_player_pitch_deg() const;
	// The local player's current/max health and team for the HUD, mirroring the original
	// per-frame HUD info. [orig: HUD_BuildEntityInfo @0x4b8440 — health ratio +92, team +374]
	int get_local_player_health() const;
	int get_local_player_max_health() const;
	int get_local_player_team() const;
	// The local player's canonical body-anim slot (BodyAnim; -1 when no player). The host
	// animates the 3rd-person avatar from this, mirroring how the present pass drives NPC models.
	int get_local_player_anim_slot() const;
	// The local player's full anim-state clip key ("anim_<name>", "" when no player). Carries
	// stance + jump the 8-slot BodyAnim enum can't (anim_idle_crouch / anim_jump_loop / ...); the
	// host plays it on the avatar via NovaObjectModel.play_body_clip for full stance fidelity.
	String get_local_player_anim_key() const;
	int get_local_player_anim_phase_ticks() const;

	// --- WAC scripts ------------------------------------------------------
	// Install a compiled program on the script VM (NovaWacProgram). Applied now if
	// loaded and re-applied on every (re)load. Pass null to uninstall.
	void set_wac_program(const Ref<NovaWacProgram> &p_program);
	Ref<NovaWacProgram> get_wac_program() const { return wac_program_; }
	// Compile `sources` against the LIVE promoted world (symbolic group/area names
	// resolve through the registry) and install on success. False (program not
	// installed) when compilation has errors; inspect via get_wac_program().
	bool compile_and_set_wac(const PackedStringArray &p_sources);
	// { loaded, paused, runs, event_count, code_size } for transport/debug UI.
	Dictionary get_wac_state() const;
	// Last-frame microsecond counters for the runtime hot path. Allocates only when queried.
	Dictionary get_runtime_perf_counters() const;
	// Script-disable gate [orig: dword_C6EB28].
	void set_wac_paused(bool p_paused);
	bool is_wac_paused() const;

	// Drain the World EffectLog as an Array of Dictionaries {kind, a, b, c, d, str} and clear
	// it. Presentation-only (text/dialog/win/subgoal/show_waypoints/set_light); state mutation
	// is applied in-engine, never here.
	Array drain_effects();

	// Mission scripting state on the shared world (the dword_C6B240 var store + event gates).
	void set_mission_variable(int index, int value);
	int get_mission_variable(int index) const;
	bool has_event_fired(int index) const;
	int get_event_count() const;

	// --- Read-only introspection (debug overlay / tooling) -----------------
	// The world's logic tick counter [orig: current_tick @0x24c1968]. The
	// pre-mission pass in finish_load already advanced it once, so a freshly
	// loaded mission reads 1 — consumers should track deltas, not absolutes.
	int64_t get_logic_tick() const;
	// Whole-bank snapshots of the script variable stores (V0..V511 / G0..G255 /
	// M0..M15 [orig: dword_C6B240 / dword_C6BA40 / music bank]): ONE packed call
	// for a low-Hz overlay refresh instead of hundreds of boxed scalar reads.
	// Always bank-sized; all zeros when no mission is loaded.
	PackedInt32Array get_mission_variables_snapshot() const;
	PackedInt32Array get_global_variables_snapshot() const;
	PackedInt32Array get_music_variables_snapshot() const;
	// Globals (G#) round out the scalar var API (mission V# already bound).
	void set_global_variable(int index, int value);
	int get_global_variable(int index) const;
	// [i] = 1 when event i has fired (active latch + delay elapsed): the bulk
	// form of has_event_fired for an event readout. Empty when unloaded.
	PackedByteArray get_fired_events_snapshot() const;
	// Scalars-only detail card for ONE selected entity ({} when the index is
	// invalid). The key set is STABLE: a registry-despawned entity (scripted
	// remove) still carries every key, with typed defaults for the registry
	// half (kind/index -1, alive false, empty name, ...). Dictionary/String
	// allocation is fine at selected-entity-only low-Hz use; the per-tick
	// present loop has get_present_snapshot instead.
	Dictionary get_entity_debug(int p_index) const;
	// Human-readable AI state name, "?" for the id gaps
	// [orig: Entity_LookupAIStateName @0x455cc0].
	static String ai_state_name(int p_state);
	// Infantry anim state id -> ADM clip key ("anim_<off_8135F0 name>"), empty for invalid gaps.
	static String infantry_anim_key(int p_state);

	// Entity query. The (kind, index) pair lets the editor map a sim entity back to its placed
	// mission record + its already-rendered node (MissionController._pickable).
	int get_entity_count() const;
	int get_entity_kind(int p_index) const;         // mission ItemType (3 = Organic), -1 if none
	int get_entity_index(int p_index) const;        // index within its kind's list
	Vector3 get_entity_position(int p_index) const; // mission (x,y,z) -> Godot (x, z, -y), units
	float get_entity_yaw(int p_index) const;        // BAM heading -> radians
	float get_entity_yaw_deg(int p_index) const;    // heading in mission degrees (for the editor remap)
	int get_entity_state(int p_index) const;        // AI state id (16 = GROUND_FOLLOWWP)
	int get_entity_net_id(int p_index) const;       // runtime SSN (WAC/BMS addressing), 0 if none
	int get_entity_bms_id(int p_index) const;       // file entity id; the host maps this to a placed node
	int get_entity_owner_connection_id(int p_index) const; // entity+0x78 dcb; the networked-player identity (D-NET-112)
	int get_entity_wire_handle(int p_index) const;  // (pool<<12)|slot — the per-entity wire identity
	// Part-anim channel phase 0..65535 (PLAYPARTANIM); channel is 1 or 2. The host renders the
	// model part from this (the engine computes it; the host only reads it).
	int get_entity_part_anim_phase(int p_index, int channel) const;
	// True when channel has a live part-anim to render (rate set or phase moved off rest), so the
	// host only poses commanded channels and leaves untouched parts at their default.
	bool get_entity_part_anim_active(int p_index, int channel) const;
	// Entity.anim_slot: the main-body skeletal clip (.bad via .adm) the AI requested. Written by
	// EntityCommands::set_ssn_anim; consumed only by the host's deferred apply_body_anim seam today
	// (skeletal runtime not yet built — AnimMap_PlayAnimBySlot @0x40bda0 / off_8135F0). -1 = none.
	int get_entity_anim_slot(int p_index) const;
	// True when the entity is flagged hidden (HideSingle / held). The present pass maps
	// (not hidden and alive) -> Node3D.visible.
	bool get_entity_hidden(int p_index) const;

	// ONE batched present snapshot for the per-tick render pass: a flat PackedFloat32Array of
	// get_entity_count() records, PF_STRIDE floats each, fields per the PresentField enum. Avoids the
	// ~10 Variant-boxed scalar getter calls per entity the present loop would otherwise make.
	PackedFloat32Array get_present_snapshot() const;
	int get_present_stride() const { return PF_STRIDE; }

	// Wire the terrain the AI grounds on (the host's loaded NovaTerrainData). Copies the depth
	// buffer + sector layout so the portable height field outlives the source and survives reload.
	// Null/unloaded clears grounding (entities keep their authored Z). The editor preview and the
	// game runtime both call this once in MissionRuntime.setup() so they ground identically.
	void set_terrain_height_field(const Ref<NovaTerrainData> &p_terrain);
	void set_item_seat_specs(const Array &p_specs);

	// The AI-speed -> world-units locomotion factor (see AiSystem::loco_scale).
	void set_loco_scale(int p_scale);
	int get_loco_scale() const;

	// Wire the infantry root-motion source: resolve a model's .adm (e.g. "E_STAND.adm")
	// through the host's resource root and keep its clips' root tracks. Returns the number
	// of anim states with a usable clip (0 = nothing loaded; org1 soldiers then stand —
	// motion comes from clips, as in the original). Survives reset_world like the terrain.
	int set_infantry_anim_map(const Ref<class NovaResourceRoot> &p_resource_root, const String &p_adm_name);
	int get_infantry_clip_count() const { return infantry_anim_.clip_count(0); }

	// Per-entity grounding: resolve every active infantry soldier's OWN model .adm (from its
	// items.def type id via the item database) and store its registry adm_id on the entity, so
	// each grounds + locomotes off its own clip rather than the shared default set. Idempotent;
	// call after load and again after spawning the local player.
	void resolve_infantry_adm_ids(const Ref<class NovaResourceRoot> &p_resource_root,
	                              const Ref<class NovaItemDatabase> &p_item_db);

	// Per-entity AI-capability resolution: stamp each live entity's Entity::is_ai_capable from
	// its items.def ItemDefAttrib & 0x100000 (AIData) via the item database. The pool-1 0x0D
	// world-stream gates its AI-trailer on this so the wire matches the stock decoder's own gate
	// (itemDef.attrib & 0x100000 @0x433327) — byte-faithful AND crash-safe. Idempotent; call after
	// load (and again after spawning the local player). [docs/net/novaworld-net-re.md D-NET-97]
	void resolve_item_ai_capability(const Ref<class NovaItemDatabase> &p_item_db);

	int get_spawned_count() const { return promo_.spawned; }
	int get_brain_count() const { return promo_.brains; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaSimulation::TickMode);
VARIANT_ENUM_CAST(godot::NovaSimulation::PresentField);
