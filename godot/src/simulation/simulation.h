#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4i.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <runtime/mission/event_runtime.h>
#include <runtime/mission/promote.h>
#include <runtime/hud/end_round_overlay.h> // EndRoundOverlayInput (the end-round ladder feed)
#include <runtime/hud/hud_frame.h> // HudVehiclePanelState / HudLfpZone (the panel feed seams)
#include <runtime/hud/hud_minimap.h>
#include <runtime/hud/hud_minimap_feed.h> // the marker feed layout the snapshot carries
#include <runtime/world/deploy_screen_feed.h> // kDeployRefreshTicks (the death deploy screen cadence)
#include <formats/playersav/weapon_sav.h> // weapon.sav: the per-side profile class + kit pages
#include <runtime/terrain_query/terrain_field_store.h>
#include <runtime/wac/wac_system.h>

namespace godot {

class Weather;
class RtxtStringFile; // the gametext table the end-round / deploy feeds resolve through
class EntityCard;     // the typed per-entity debug card (world::inspect, ADR 0042 d5)
class EntityRow;      // one typed entity-directory row
class EndRoundState;  // the typed end-of-round session facts (simulation_end_round.cpp)
}

#include "wac/wac_program.h"
#include <formats/def/def.h> // the retained weapon.def parse (S6b)
#include <runtime/simassets/adm_clip_index.h> // the equipped rig's clip lengths (S6b)
#include <runtime/world/player_loadout.h> // the moved loadout cluster (S7b, ADR 0028)
#include <runtime/world/player_weapon.h> // the moved equipped-weapon cluster (S7a, ADR 0028)
#include <runtime/world/present_rows.h> // the engine-owned PF_* present-row layout (ADR 0031)
#include <runtime/simassets/collision_resolve.h> // the collision/occlusion resolution sweep (ADR 0031)
#include <runtime/simassets/sim_collision_pose.h> // the engine-side pose provider (S3, ADR 0028)
#include <runtime/simassets/sim_model_cache.h> // the sim's own .3di source (ADR 0028)
#include <runtime/simassets/mounted_pose.h> // reusable PANM part matrices for mounted attachments
#include <net/inmatch/session.h>
#include <runtime/world/ai.h>
#include <runtime/world/inspect.h> // the typed entity inspection API (ADR 0042 d5)
#include <runtime/world/tick_accumulator.h>
#include <runtime/world/collision.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/player_input.h>
#include <runtime/world/player_look.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/player_view.h>
#include <runtime/world/vehicle_attach.h> // the attach-command ids + the seat mirror
#include <runtime/world/round_sim.h> // the hit-zone damage tables (re-exported statics)
#include <runtime/world/spawn_select.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/world.h>
#include <formats/score/score.h> // the retained score.ini parse (score_config_)

#include "mission/mission_data.h"
#include <runtime/mission/mission_kernel.h>
#include <runtime/renderer/precipitation_frame.h>
#include <runtime/devtools/environment_snapshot.h> // the ONE mission boot + state + no-net tick (ADR 0042 d3)
#include <runtime/devtools/physics_snapshot.h>
#include <runtime/devtools/rays_snapshot.h>
#include <runtime/simassets/adm_root_motion.h> // the engine-side IRootMotionSource (ADR 0028)

#include <net/inmatch/listen_host.h>              // ListenHostState + the listen bring-up/frame (ADR 0042 d3)
#include <net/netsim/loopback_channel.h>          // host_loop_ (the host's own dcb-2 client)
#include <net/netsim/item_replication_catalog.h> // canonical items.def replication traits
#include <net/netsim/client_world_materializer.h> // header-only joiner pools 1..3
#include <net/netsim/udp_session_transport.h>     // PeerLink::transport (the LAN per-peer transport)

#include <net/npwire/peer_addr.h>    // PeerAddr / PeerAddrHash
#include "network/udp_pump.h"

#include <formats/mission/bms.h>                      // bms::File (persisted so ctx_.mission outlives the match)
#include <net/npruntime/napi_np_server_ctx.h>     // NapiNPServerCtx / GameConfig / ConnectionMode / SocketMode
#include <net/npruntime/napi_np_protocol.h>       // HostAcceptEvent + the host owner-loop entry points
#include <net/npruntime/client_runtime.h>         // ClientRuntime (HostClient / Joiner roles)
#include <net/npruntime/host_session.h>           // HostOwner + host_session_pump (the shared host owner loop)
#include <net/npruntime/joiner_world_bridge.h>    // the joiner's per-frame world<->net bridge (S10a)

#include "simulation/inmatch_session_values.h"
#include "devtools/frame_stats.h"

namespace opennova::hud {
struct ScoreboardEntry; // hud/hud_scoreboard.h — the Tab-board drawer row
}

namespace godot {

class TerrainData;
class ObjectData;
class SkeletalAnim;
class ItemDatabase;
class AvatarDatabase;
class ResourceRoot;

// The Godot adapter for one portable in-match tick target. It owns the World
// and logic systems (WAC VM, BMS evaluator, AI); inmatch::Session owns
// lifecycle, input retention, fixed cadence, and terminal outcomes. One target
// advance is the original's 62 Hz engine tick (current_tick in
// Game_ProcessMainFrame @0x5263f0), while advance_session_frame runs 0..N of
// those, faithful to Game_MainLoop @0x52b630. Per-system cadences live INSIDE
// the systems, as in the original: the WAC VM self-gates to every 62nd tick
// (WacScript_AdvanceTick @0x4f81b1) and the BMS evaluator quarter-passes every
// 16th (Server_TickUpdate @0x51d7e0). MainGame/GameWorld is the sole live
// owner for this path; focused tests and non-gameplay tools may instantiate it
// directly: promote a parsed BMS mission into the world (mission/promote.h),
// register the systems in the faithful order (mission/mission_systems.h), run
// a pre-mission pass, then tick. Entity transforms (mission -> Godot space)
// and the part-anim phase are exposed for a scene/renderer to draw;
// presentation side effects (text/dialog/win) drain out of the World EffectLog
// each tick. Runtime transport and fixture teardown share the
// play/pause/step/restart surface.
class Simulation : public Node3D,
                       private opennova::inmatch::TickTarget {
	GDCLASS(Simulation, Node3D)

public:
	// The bound enum/constant surface — a class-body fragment (its file
	// header carries the rules).
	#include "simulation/simulation_bound_constants.h"

	// The private state block — a class-body fragment; it opens its own
	// `private:`, and `public:` below re-opens the API surface.
	#include "simulation/simulation_members.h"


public:
	// --- the weather home (world::WeatherState, ADR 0042 d2/d5) --------------
	// The World's weather, the ONE home the WAC handlers write, the kernel's
	// weather tick advances, the 0x0A projection serializes and a joiner's
	// decoder writes back. Null without a kernel; C++ seams for the sibling
	// native nodes (the Weather node binds its render owner here).
	opennova::world::WeatherState *weather_state();
	const opennova::world::WeatherState *weather_state() const;
	bool weather_state_bound() const { return weather_state() != nullptr; }
	// The mission-start seed (the embedder's ONE derivation, env::weather_seed_from_config).
	void seed_weather(const opennova::world::WeatherSeed &p_seed);
	// The render owner the kernel's weather tick calls after the sim legs
	// (null detaches); remembered so the World's death releases the owner's
	// pointer before the environment can read a freed WeatherState.
	void set_weather_render_owner(Weather *p_owner);
	// The authority's mission-start boundary after the eager WAC execution:
	// currents snap to targets, clamps install, 255 full ticks settle (retail
	// Environment_MissionStartInit @ 0x57f1e0). False for a joiner / no world.
	bool settle_weather_mission_start();
	// The precipitation drop pool's per-render update + compile for a camera:
	// {positions (three per drop), drops, color (ARGB int), snow}
	// (renderer/precipitation_frame.h carries the cites).
	Dictionary compile_precipitation_frame(const Vector3 &p_camera,
			const Vector3 &p_camera_right, const Vector3 &p_camera_up,
			int p_terrain_light_rgb);
	// Thunder one-shots since the last drain: [{distance, bearing}] (weather_state.h carries the cites).
	Array drain_weather_sounds();
	// The probe/test view of the weather home in native units.
	Dictionary get_weather_state() const;
	// The F3 Environment window's record (ADR 0042 d6): the ENGINE join over
	// the weather home; false without a world.
	bool native_environment_snapshot(opennova::devtools::EnvironmentSnapshot &out) const;
	// The MCP/debug rows' authority-gated weather commands (the F3 window
	// reaches EntityCommands natively through DevTools). False on a joiner.
	bool command_rain(int p_percent, int p_seconds);
	bool command_snow(int p_percent, int p_seconds);
	bool command_overcast(int p_percent, int p_seconds);
	bool command_fog_distance(int p_metres);
	bool command_move_fog(int p_metres, int p_seconds);
	bool command_sky_speed(int p_rate);
	bool command_quake(int p_seconds);
	bool command_time_of_day_minutes(int p_minute_of_day);
	// The exact dev-tool scrub (not the WAC `tod` math).
	bool debug_set_time_of_day_minutes(double p_minute_of_day);
	bool command_fog_type(int p_type);
	bool command_lightning_flash();
	bool command_lightning_far_flash();
	bool command_wind_scale(int p_value);

private:
	// The shared post-kernel-boot binding legs: session-header capture, HUD
	// map zoom, score-row re-resolve, and the held-WacProgram re-apply.
	void finish_kernel_boot();
	// The kernel boot's bringup_net_session hook for this sim's role.
	std::function<void()> role_bringup_hook();
	void apply_host_session_mission_header(const opennova::bms::File &file);
	void refresh_host_accept_config();

protected:
	static void _bind_methods();

public:
	Simulation();
	// A dying joiner sim ships the retail goodbye burst before the socket drops — retail sends
	// its disconnect packets from the connection teardown that Destroy also runs, so freeing the
	// sim (ESC abort, watchdog abort, return-to-menu) must not leak an admitted peer on the host.
	// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0, called by Destroy]
	~Simulation() override;

	// Load + promote an in-memory bms::File. This remains a narrow fixture/tooling seam;
	// ONED gameplay launches only from a saved loose .bms (GameWorld.load_mission).
	bool load_from_mission_data(const Ref<MissionData> &p_mission);
	// S9 (ADR 0028): the ordered mission boot — engine/runtime/mission
	// runtime_boot owns the sequence + the file-resolution policy; this entry
	// supplies the step bodies over the existing feeds. The shell composes
	// role bring-up before it and presentation after it. Returns OK or
	// ERR_CANT_OPEN (mission missing / load failed; the sequence aborted).
	int64_t boot_mission(const Ref<MissionData> &p_mission,
			const Ref<ResourceRoot> &p_resource_root,
			const Ref<ItemDatabase> &p_item_db,
			const Ref<TerrainData> &p_terrain,
			const PackedByteArray &p_terrain_til, const String &p_wac_basename,
			const String &p_infantry_adm, const String &p_mission_file_basename,
			bool p_playable);
	// Build + promote a small synthetic patrol mission (no file) for the headless unit test.
	void build_demo_mission();
	bool is_loaded() const;

	// Transport.
	bool is_playing() const {
		return session_.state() == opennova::inmatch::State::Running;
	}
	// Advance exactly ONE 62 Hz logic tick — the original's engine tick. The per-system
	// dividers gate INSIDE the systems (the WAC VM self-gates to every 62nd tick, the BMS
	// evaluator quarter-passes every 16th), exactly where the original keeps them. Returns
	// false when the session cannot take a direct local/test tick. Banking wall
	// clock and dispatching 0..N ticks per render frame belongs to inmatch::Session
	// (the Game_MainLoop @0x52b630 accumulator) — a render frame is NOT one tick.
	// [orig: Game_ProcessMainFrame @0x5263f0 (one current_tick++ @0x24c1968)]
	bool step();

	// Turn the sim into an SP in-process listen server (ADR 0011): the host serializes
	// real entity state onto an in-process loopback (Server_TickUpdate's per-connection S2C
	// fan), the local client decodes it, and the present pass reads that decoded state. Call
	// BEFORE loading a mission — the next load stands up the npruntime host runtime. Disabling
	// reverts to the direct AI-pool present used by explicit non-network fixtures.
	void enable_listen_server(bool p_enable);
	bool is_listen_server() const { return listen_server_; }

	// Feed the mission's raw terrain-tile (.til) file bytes so the listen host streams the S2C 0x45
	// terrain-tile load to joiners (climbs the client's g_loading_progress 5 -> 6; §5.37). The Godot
	// shell owns the resource root, so it read_file()s the .til (named by the .trn tileinfo) and passes
	// the bytes here BEFORE loading the mission. Empty / not-called => 0x45 is faithfully skipped.
	void set_terrain_til_data(const PackedByteArray &p_til_bytes);
	// Feed the raw RTXT mission string table before load. Native lookup avoids a
	// UTF-8 round-trip and resolves briefing2's retail briefing fallback.
	void set_mission_text_data(const PackedByteArray &p_rtxt_bytes);
	// Overlay the active game-type scoring row from retail's loose VERSION 40
	// score.ini. Returns false for absent, malformed, or unsupported data.
	bool set_score_config_data(const PackedByteArray &p_score_ini_bytes);

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
	// tests/npruntime/handshake_server_test, the P2 retarget of the retired
	// novaworld host_session_accept_test). Returns true if an entity was
	// spawned + bound. No-op unless host listening is on.
	bool admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team);

	// --- co-op LAN joiner (D.2) -------------------------------------------
	// Turn the sim into a co-op LAN JOINER: dial the host and run the witnessed in-match JOIN
	// as a non-authority client. `player_name` rides the game ClientAuth.NA — the key the host
	// echoes into our organic-spawn record (name-match self-ID, wire handle H). Call BEFORE
	// loading the mission; a sim is host XOR joiner; false when the socket can't be dialed.
	// `join_role` 1 = the retail spectator role (ClientAuth JSR=1 + optional JSPP).
	bool enable_join(const String &p_host_ip, int p_port, const String &p_player_name,
			int p_join_role = 0, const String &p_spectator_password = String());
	bool is_joiner() const { return joiner_; }
	// Live player-slot spectator state: joiner = S2C 0x75 latch; authority = Server_SetPlayerSpectator.
	bool is_local_spectator() const;
	bool set_local_spectator(bool p_spectator);
	// The client-local death screen latch (retail g_death_screen_active): gates the
	// friendly-tags walks + camera arbiter sub-mode; fed by simulation_player_view.cpp.
	bool local_death_screen_active() const;
	// True while a live net session owns this sim: the world tick is the ONLY pump for the
	// session socket, so the Play/Step/Stop transport locks out (retail MP has no pause; a
	// stopped listen host reaps every joiner at cs_dir0.timeout_ms [orig: CNapiNetwork_Init
	// @ 0x4ca4a0]). The single home for the rule — F3 transport, MCP, and ESC pause read it.
	bool is_transport_locked() const { return joiner_ || host_listen_; }
	// The portable session's live role/state records (net/inmatch/session.h),
	// re-exported so the debug/MCP shell derives authority and role labels from
	// the session instead of re-deriving them from the transport flags. The
	// role values mirror inmatch::Role (the assignments make drift impossible);
	// session_state() reports inmatch::State in the same values
	// MissionFrameOutcome.STATE_* carries.
	enum SessionRole {
		ROLE_SINGLE_PLAYER = static_cast<int>(opennova::inmatch::Role::SinglePlayer),
		ROLE_LISTEN_HOST = static_cast<int>(opennova::inmatch::Role::ListenHost),
		ROLE_JOINER = static_cast<int>(opennova::inmatch::Role::Joiner),
		ROLE_DEDICATED_HOST = static_cast<int>(opennova::inmatch::Role::DedicatedHost),
	};
	int session_role() const { return static_cast<int>(session_.role()); }
	int session_state() const { return static_cast<int>(session_.state()); }
	// The fixed logic-tick quantum (1/62.5 s) — the ONE cadence constant,
	// re-exported from the engine accumulator for GDScript composition.
	static double tick_dt() { return opennova::world::TickAccumulator::kTickDt; }
	// The tick cadence as a rate, and wall-clock ms -> whole logic ticks —
	// re-exports of the engine tick home (world/tick_accumulator.h carries
	// the current_tick witness).
	static int ticks_from_ms(int64_t p_ms) {
		return opennova::world::ticks_from_ms(p_ms);
	}
	// The epilog/debrief ESC-less exit timeout in seconds, derived from the
	// engine tick constants (world/world.h kEpilogExitTimeoutTicks).
	static double epilog_exit_timeout_seconds() {
		return opennova::world::kEpilogExitTimeoutTicks *
				opennova::world::TickAccumulator::kTickDt;
	}
	// The epilog/debrief screen fade-in in seconds (world/world.h
	// kEpilogFadeInTicks, the 48+48-tick cine fade pair).
	static double epilog_fade_in_seconds() {
		return opennova::world::kEpilogFadeInTicks *
				opennova::world::TickAccumulator::kTickDt;
	}
	// The DEATH deploy screen's content refresh cadence in seconds
	// (world/deploy_screen_feed.h kDeployRefreshTicks).
	static double deploy_refresh_interval_seconds() {
		return opennova::world::kDeployRefreshTicks *
				opennova::world::TickAccumulator::kTickDt;
	}

	// --- Portable session frame (ADR 0035) --------------------------------
	// The input and outcomes are typed values. The one temporary tick sink keeps
	// per-tick Godot presentation synchronous during catch-up without installing
	// a persistent callback bus; GameFramePipeline orders concrete devices around this call.
	Ref<MissionFrameOutcome> advance_session_frame(
			const Ref<MissionFrameInput> &p_input,
			const Callable &p_tick_sink = Callable());
	Ref<MissionFrameOutcome> step_session_frame(
			const Ref<MissionFrameInput> &p_input,
			const Callable &p_tick_sink = Callable());
	bool pause_session();
	bool resume_session();
	bool reset_session();
	void close_session();
	// Last frame's spans — the probe/F3 accounting seam. Always: frame_us,
	// tick_us, ticks. Only while runtime profiling is on: sim_us, sink_us,
	// net_us and the phase keys flattened from frame_phase_perf_ (the
	// HostSessionPerf/ServerTickPerf/ClientFramePerf fields plus the shell
	// legs). The frame-stats board receives the same spans natively
	// (fold_frame_stats); this Dictionary stays the probes' transport edge.
	Dictionary get_session_perf() const;
	// The dev tools' board: SIM_STEP and the SIM_* phase slots are fed here
	// while the profiling clocks run and the board captures.
	void set_frame_stats(const Ref<FrameStats> &p_stats);
	Ref<FrameStats> get_frame_stats() const;
	int64_t get_last_session_sim_us() const { return frame_sim_us_; }
	// Set the per-side character ids/classes/avatar bytes carried by ClientAuth.
	// Must be called before enable_join; later runtime rebuilds retain the values.
	void set_join_character_profile(const Dictionary &p_profile);
	void set_local_character_profile(const Dictionary &p_profile);
	// Select a registered retail resource-corpus profile for S2C 0x30/0x31.
	// Empty clears it; an unknown id also clears it and returns false.
	bool set_join_integrity_profile(const String &p_profile_id);
	// The game-session APPID join token the client recovers from the NWJoin .joi
	// CK; a NovaWorld host validates it (reject code 9). Empty/"0" is the LAN
	// default. Retained across runtime rebuilds like the character/integrity data.
	void set_join_token(const String &p_token);
	// The CD identity cookie (packed PUB* blob) for the C2S 0x00 JOIN — the
	// NovaWorld-issued NAMEINFO/PCID/SQUADINFO/JOINTICKET the host validates
	// (codes 23/24/25/28). Empty for LAN. Retained across runtime rebuilds.
	void set_join_cd_cookie(const PackedByteArray &p_cookie);
	// The install root whose loose expansion/<name>/version.txt feeds the JOIN
	// VERSIONCRCSTRING checksum (D-NET-166). Empty keeps the golden "0".
	// Retained across runtime rebuilds like the character/integrity data.
	void set_join_expansion_version_root(const String &p_game_root);
	// Load the process-scoped anti-cheat CHARACTER table before the first join
	// network pump. Missing/empty charattr.def is soft and leaves all rows inactive,
	// matching Game_Run's continue-after-error behavior.
	bool load_charattr_challenge(
			const Ref<class ResourceRoot> &p_resource_root);
	// Ship the 0x46 ClientGoodBye burst now (idempotent; joiner-only no-op otherwise). The
	// destructor calls this too, so explicit calls are only needed when the socket must close
	// before the sim is freed.
	void leave_net_session();
	// Retail connects before constructing the wire-header world: drive only the socket/session
	// legs until the terminal pre-world sync marker has been received and ACKed
	// (S2C 0x7B identifies the mission earlier), then resume the same connection
	// after the caller has constructed it. No World tick or gameplay uplink runs here.
	void set_join_world_ready(bool p_ready);
	// Freeze the renderer's unique loaded, non-foliage .3DI definition count
	// into the joiner's C2S 0x3D paging seam. GameWorld calls this once after
	// mission/render model setup and before revealing the loaded world; later
	// S2C entity spawns and presentation loads deliberately cannot mutate it.
	void finalize_loaded_model_challenge_snapshot();
	bool poll_join_preload();
	bool is_join_preload_ready() const;
	// The joiner's admission-FSM stage name (diagnostics: the shell's post-load join
	// watchdog names the stage a stalled join is parked in). Empty when not joining.
	String get_join_admission_stage() const;
	bool has_join_mission() const;
	String get_join_server_name() const;
	String get_join_mission_name() const;
	String get_join_mission_file() const;
	// Consume the latest decoded S2C 0x0A phase-2 state once per receive
	// revision. Empty means no new authoritative sample.
	// The S2C 0x81 hit-confirm edge: {} unless a positive/negative score delta
	// landed since the last take, else {score, delta, tone} with the tone name
	// ("" / "HITTONE" / "KILLTONE" / "HEADSHOTTONE") the presenter plays as a
	// 2D interface sound behind the enable_slotmachine setting
	// [orig: NapiNPClientMsg_ScoreDeltaSound @0x42a0b0, see hud/score_fanfare.h].
	Dictionary take_score_feedback();
	// Exact pre-world payloads retained by the joiner from retail's initial
	// state stream. The mission header is exactly 616 bytes when available. TIL
	// bytes are exposed only in COMPLETE; the explicit state distinguishes a
	// valid omitted 0x45 from a partial or malformed stream.
	PackedByteArray get_join_mission_header() const;
	int64_t get_join_terrain_til_state() const;
	PackedByteArray get_join_terrain_til() const;
	String get_join_expansion() const;
	int64_t get_join_game_type() const;
	String get_join_error() const;
	// Session loss: empty while healthy, else a player-facing reason the shell surfaces the
	// way it surfaces a join failure. Two causes — the host's explicit close (the punt
	// channel) and in-match silence past the reap window. Retail exits the mission with a
	// mapped exit reason here and shows no in-world dialog.
	// [orig: the cs_dir0.timeout_ms = 120000 reap installed by CNapiNetwork_Init @0x4ca4a0
	//  and the punt record CNapiNPConnection_HandleDescriptionPacket @0x621ae0, both ->
	//  CNapiNetwork_OnDisconnectedFromServer @0x4c63d0]
	String get_session_loss_reason() const;
	// The same edge as a state test rather than a presentation string: in-world surfaces
	// (the deploy screen) need to know the session is gone, not what to tell the player.
	bool is_session_lost() const;
	// True once the joiner has name-matched its organic-spawn record and received the
	// applicable deployment release (self handle H known and gameplay uplink enabled).
	bool is_joined_in_match() const;
	// Monotonic initial-admission boundary: policy + both initial loadout grants
	// have landed. Deploy-pick state is intentionally exposed separately.
	bool is_join_initial_admission_complete() const;
	// The per-second joiner trace is deliberately opt-in for release play: the
	// `net_joiner_diagnostics` debug control (F3 / MCP game_debug) switches it
	// on. The snapshot remains available so tests/debug UI can distinguish a
	// real ordered gap from ordinary idle traffic.
	bool is_joiner_network_diagnostics_enabled() const;
	void set_joiner_network_diagnostics_enabled(bool p_enabled);
	Dictionary get_joiner_network_diagnostics() const;
	// The pcap this session's datagrams are recorded to (`--capture-pcap`,
	// LaunchFlags); applied to the pump when the host binds or the joiner
	// dials, so set it before the session opens. "" records nothing.
	void set_capture_pcap_path(const String &p_path);
	// Player-paced deployment (the deploy-map screen; net-re §5.61/§5.0d). True while
	// the join owes the player a deployment pick or awaits the host's release of one —
	// the shell shows the DEATH deploy screen and the join watchdog stops (the
	// remaining transitions are player-paced).
	bool is_join_deploy_pick_pending() const;
	// The deploy-map overlay signal (retail g_deploy_screen_active). UI only.
	bool is_join_deploy_overlay_active() const;
	// The frame loop's open decision for death.mnu's DEATH screen: true once
	// per arming of the overlay (the engine-side open latch stamps itself and
	// clears when the host drops the bit). The shell calls it only while no
	// other screen is up and opens the presenter on true.
	bool take_join_deploy_overlay_open();
	// The DEATH screen's SPAWNPOINTS_LIST rows: {param:int, letter:String,
	// name_key:String} per team-owned secured deploy zone, letters/names keyed by the
	// spawn-zone registry index. Row 0 (the Default Spawn, param 0) is the shell's.
	// [orig: UI_UpdateDeathScreenContent @0x5536a0]
	TypedArray<Dictionary> get_deploy_spawn_zones();
	// The compiled SPAWNPOINTS_LIST rows {text, value}: the engine builder's two
	// witnessed loops (world/deploy_screen_feed.h) over the zone rows above, the
	// team colour tag, the Menu default-row tokens and the embedder-resolved
	// WPNames strings (name_key -> text). value 0 = default, index+1 = zone,
	// -1 = occupant/blank (never a pick). [orig: UI_UpdateDeathScreenContent @0x5536a0, see world/deploy_screen_feed.h]
	TypedArray<Dictionary> get_deploy_list_rows(const String &p_default_key,
			const String &p_default_home, const Dictionary &p_zone_names);
	// The DEATH screen's STATIC facts: the 0x0A sub-block-0 timers, the queued
	// wave line, the psp/medic show gates, and the medic-call cooldown.
	Dictionary get_deploy_status();
	// The STATIC_RESPAWN_MSG1 text of the current status line, resolved
	// through the gametext table (the engine's deploy_status_text); "" when the static is hidden.
	String get_deploy_status_text(const Ref<RtxtStringFile> &p_gametext);
	// The dead player's medic call (C2S 0x2E): gated on a dead local player and
	// the 310-tick cooldown; a joiner queues it, the listen host loops it back.
	// [orig: Input_HandleActionBinding case 217 @0x49b4b4..0x49b51b, see docs/net/novaworld-net-re.md 0x2E]
	bool request_local_player_medic();
	int local_medic_request_cooldown_ticks() const;
	int local_medic_request_serial() const;
	// The rtxt "Server" table's STRSRV_MEDREQ format for the host's broadcast.
	void set_server_text(const String &p_medic_request_format);
	// The one role-agnostic read of the local player's dead bit.
	bool local_player_dead() const;
	// The end-of-round presentation feed (net-re §5.68; simulation_end_round.cpp):
	// the 0x1D header edge + the 0x56 board through the ONE ClientEndRoundStats
	// every role's view folds; the overlay text ladder (hud/end_round_overlay.h)
	// and the stat.mnu RESULTLIST columns/rows (npruntime/stat_screen_feed.h).
	Ref<EndRoundState> get_end_round_state() const;
	TypedArray<Dictionary> get_end_round_lines() const;
	// The retail is_in_session fact for the shell's round-cycle and HUD
	// arms: the world's mp_session bit (the 0x1D header form).
	bool is_mp_session() const;
	// The ladder's input from this role's view (C++ only, not bound): the
	// shared producer of get_end_round_lines / get_end_round_overlay.
	opennova::hud::EndRoundOverlayInput end_round_overlay_input() const;
	// The overlay ladder resolved through the gametext Overlays table (the
	// folds + printf forms are the engine's end_round_overlay_resolve):
	// {texts, ys, top, bottom} ready for HudOverlay::set_end_round_overlay.
	Dictionary get_end_round_overlay(const Ref<RtxtStringFile> &p_gametext) const;
	// The RESULTLIST columns with their header text resolved through the
	// gametext Overlays table (null gametext = the "!..." fallbacks).
	TypedArray<Dictionary> get_end_round_columns(int p_table_width,
			const Ref<RtxtStringFile> &p_gametext) const;
	// The rows, filtered by stat.mnu's tab (0 all, 1 team 2, 2 team 1 — the
	// engine's stat_screen_row_visible).
	TypedArray<Dictionary> get_end_round_rows(int p_tab) const;
	// hud::kEndRoundStatScreenDelayMsec — the 6 s stat.mnu delay.
	static int end_round_stat_screen_delay_msec();
	// hud::strip_inline_tags — retail's `<...>` markup stripper.
	static String strip_inline_tags(const String &p_text);
	// The SP Show Score statistics counters (hud/end_round_statistics.h):
	// the 0xC846xx block the toggled panel draws. Empty when no host world.
	Dictionary get_end_round_statistics() const;
	// Send the player's deploy pick: 0 = default spawn (0xFFFF), 65534 = auto team
	// spawn (0xFFFE), else the 1-based registry index resolved to its entity handle.
	// Re-picks while awaiting the release match retail (the host silently drops an
	// invalid pick and the screen stays). [orig: Input_HandleActionBinding case 12]
	bool send_deployment_pick(int p_param);
	// The joiner's server-assigned team — the S2C 0x04 tail-byte latch the deploy
	// screen colors/filters by [orig: byte_A85B48]. 0 when not joining.
	int get_join_assigned_team() const;
	// The class-availability policy for the active session. A joiner reads the
	// retail host's S2C 0x76; an authority exposes its configured writer source.
	int get_class_allow_mask() const;
	// The JoinerConnection phase as an int (JoinerConnection::Phase), -1 when not joining.
	int get_joiner_phase() const;
	// The learned wire handle H, 0 until in-match (debug / test).
	int get_joiner_self_handle() const;

	// --- the local player (ADR 0012; net-re §5.2b/§5.38) -------------------
	// Spawn the host's own player as an authoritative pool-0 entity at a Godot-space position
	// (yaw in mission degrees). Call AFTER a mission is loaded (the spawn needs the AI system
	// wired). Returns false if no mission is loaded or pool 0 is full. The player then runs
	// the infantry motor from input (set_player_input), not AI think.
	bool spawn_local_player(Vector3 p_position, float p_yaw_deg, int p_team);
	// The session g_GameType word an SP/offline mission implies: the mission's attrib mode
	// through the catalog's for_mission_mode map (no multiplayer bit -> stock Co-op 0x10020).
	// The listen host seeds its GameConfig from it before the auto-spawn, the same word the
	// LAN-host dialog derives on the GDScript side (HostSessionConfig.game_type_auto).
	// [orig: AI_GetTaskTypeFromFlags @0x40DAE0 -> Game_StartMission @0x524360, see
	// docs/net/novaworld-net-re.md 5.2c]
	uint32_t mission_game_type() const;
	// Spawn the host's own player at the mission's player-START marker, selected the way the
	// original engine does — by game type, FARTHEST from the enemy set — NOT at any NPC's
	// position (net-re §5.2c). A stock SP mission resolves the Co-op 6094 -> 6001 chain. Call AFTER a
	// mission is loaded. Returns: 1 = spawned at a real start marker; 0 = no start marker, spawned
	// at a safe fallback origin (never an NPC); -1 = failed (no mission / pool 0 full).
	// [orig: Server_PositionPlayerForSpawn @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0]
	int spawn_local_player_at_start();
	// True once a local player has been spawned (World::cached.local_player valid).
	bool has_local_player() const;
	// The local player's wire identity ((pool<<12)|slot). Packed zero is valid: callers that
	// need presence use has_local_player()/the runtime's has_self_handle() instead of a sentinel.
	// The host returns its pool-0 player; a joiner returns H, the host-assigned identity that its
	// wire-present pass excludes while LocalPlayerPresenter draws the distinct local motor entity L.
	int get_local_player_wire_handle() const;
	// Feed one frame of player input: the move keys + look yaw/pitch (mission degrees). Applied
	// to the player's body input at the top of the next frame. Movement keys + the lean
	// keys (Q/E, catalog ids 6/7) + jump; stance and look are SIM-owned state
	// (request_local_player_stance / add_local_player_look). There is no run key in the
	// original's catalog — running is the automatic forward-walk promotion in the body
	// selection [orig: @0x4b729d].
	void set_player_input(bool p_forward, bool p_back, bool p_left, bool p_right,
	                      bool p_lean_left, bool p_lean_right, bool p_jump);
	// One frame of mouse pixels (screen sense: +x right, +y down) applied to the local
	// player's look through the witnessed integer pipeline: sens = setting << 11,
	// scoped zoom reduction, (px*sens+0x8000)>>16 per axis; yaw wraps; pitch clamps
	// +-80 deg with the up-limit +40 deg while prone. [orig: Input_ProcessMouseAxisBindings
	// @ 0x499680; axis cases 166/164 @ 0x4e109d/@ 0x4e0fed]
	void add_local_player_look(float p_dx_px, float p_dy_px);
	// Mouse options: sensitivity [1,511], default 128; Y invert (flipmouse, default off).
	// [orig: dword_24D207C / dword_24D2078; profile +0x590/+0x594; defaults @ 0x54bbc0]
	void set_local_player_mouse(int p_sensitivity, bool p_invert_y);
	// Stance SELECT request (0 stand / 1 crouch / 2 prone) — the 3-key semantics: each
	// key selects its stance, mutual exclusion at apply, REFUSED while the equipped
	// weapon has ForceCrouch (0x40000). Returns whether the stance changed. [orig:
	// input cases 169/170/172 @ 0x4e0d77.. -> C2S 0x1D ->
	// NapiNPServerMsg_HandleStanceChange @ 0x501c60]
	bool request_local_player_stance(int p_stance);
	// The local player's authoritative position in Godot world space (for the follow camera);
	// Vector3() when no player is spawned.
	Vector3 get_local_player_position() const;
	// The AI row's 16.16 position (mission x/y ground, z up) the retail hashes read; false without a player row.
	bool local_player_position_q16(int32_t (&r_pos)[3]) const;
	// Raw engine heading (BAM32) for the heading-up spinmap.
	int64_t get_local_player_heading_bam() const;
	// Radar zoom: positive = radarout (x1.15 toward 0x100000), negative =
	// radarin (x0.85 toward 4096), zero = the spawn reset (the witnessed step
	// lives in hud::spinmap_zoom_step).
	int request_hud_radar_zoom(int p_direction);
	int get_hud_radar_zoom_q16() const;
	// map_toggle's 0->2->3->0 cycle (witness at HudMinimapInput::map_mode).
	int request_hud_map_cycle();
	int get_hud_map_mode() const;
	int get_hud_big_zoom_q16() const;
	// Mission attrib bit5 (AttribFlags::RotateMap180) rotates the gameplay
	// map 180 degrees; the witness rides HudMinimapInput::flip_180.
	bool get_hud_map_flip_180() const;
	// The local player's authoritative look yaw / pitch in mission degrees (for the first-person
	// camera). yaw = 90 - heading; pitch up positive. 0 when no player is spawned.
	float get_local_player_yaw_deg() const;
	float get_local_player_pitch_deg() const;
	// The local player's current/max health and team for the HUD, mirroring the original
	// per-frame HUD info. [orig: HUD_BuildEntityInfo @0x4b8440 — health ratio +92, team +374]
	int get_local_player_health() const;
	int get_local_player_max_health() const;
	// The gamemus Var7 projection (world/music_vars.h carries the witness).
	int get_local_player_health_percent() const;
	int get_local_player_team() const;
	// The packed Avatars.def character id (npwire/character_id.h) the authority
	// stamped on the local player — entity+0x15C on the host's own spawn, the
	// named 0x0C record's id on a joiner (also before L exists); 0 = none yet.
	int get_local_player_character_id() const;
	// Authoritative armory on-show state from the spawned entity. The class is
	// playerClass +0x294; the name resolves equipped AdmDef index +0x2B0.
	// [orig: Armory_ResolveSelectedClass @0x5642f0; Player_MountWeaponSlot @0x4dfa40]
	int get_local_player_class() const;
	String get_local_player_weapon_name() const;
	// The local player's canonical body-anim slot (BodyAnim; -1 when no player). The shell
	// animates the 3rd-person avatar from this, mirroring how the present pass drives NPC models.
	int get_local_player_body_anim_slot() const;
	// The local player's full anim-state clip key ("anim_<name>", "" when no player). Carries
	// stance + jump the 8-slot BodyAnim enum can't (anim_idle_crouch / anim_jump_loop / ...); the
	// shell plays it on the avatar via ObjectModel.play_body_clip for full stance fidelity.
	String get_local_player_anim_key() const;
	// The sim-owned stance latch (0 stand, 1 crouch, 2 prone) — the
	// dword_B76484 prone-latch equivalent the render-slot drape gate reads
	// [orig: RenderSlot_DrawAllDrapes @0x5d6e81 reads
	// g_PlayerStanceProneLatch, see docs/render/render-lighting-re.md].
	int get_local_player_stance_latch() const { return kernel_->stance_latch(); }
	// The HUD stance icon index (0 stand / 1 crouch / 2 prone) from the sim's
	// authoritative stance state [orig: HUD_BuildEntityInfo @0x4b860c —
	// entity+300 flags 0x200=crouch -> 1, 0x100=prone -> 2]. The witnessed
	// source is the body state, never the anim clip name.
	int get_local_player_stance() const;
	int get_local_player_anim_phase_ticks() const;
	String get_local_player_anim_source_key() const;
	int get_local_player_anim_source_phase_ticks() const;
	float get_local_player_anim_blend_weight() const;
	// The local player's third-person aim-overlay state — the torso bend. Dictionary:
	//   valid: bool; aim_state: bool (anim-state flag 0x40 — the bend branch);
	//   body: Vector3 mission-euler degrees (pitch, yaw, roll) for the avatar node basis;
	//   angles: PackedVector3Array[9] mission-euler degrees per anim::OverlayClass.
	// The blends run in exact BAM int math [orig: Entity_BuildBoneTransformMatrices
	// @0x4b1290; docs/world/world-wac-ai-re.md §14]; the shell converts each triple with
	// MissionObjectPlacer.bms_to_godot_basis (the single-sourced frame conversion) and
	// feeds ObjectModel.set_aim_overlay. Empty/invalid when no player.
	Dictionary get_local_player_aim_overlay() const;
	// The third-person held-weapon model name for an ADM index (weapon.def gfx3).
	String get_weapon_third_person_model(int p_adm_index) const;

	// --- the local player's equipped-weapon FSM (net-re §5.62) -------------
	// Install the equipped weapon: p_def is the WeaponDatabase weapon dict (the
	// {actions, flags, clipsize, startrounds} slice is consumed) and p_clip_seconds
	// maps each .adm clip key to its VARIANT lengths in SECONDS — a
	// PackedFloat32Array in .adm file order (SkeletalAnim.get_clip_variant_lengths;
	// a plain float is accepted as a single-variant convenience). The lengths seed the
	// per-slot rings and the Anim_InitActions bake consumes them ring-wise: one
	// serve-then-advance read per 'auto' delay field [orig: @ 0x541fa0;
	// Anim_GetDurationTicks @ 0x53ee10]. A normal install is a real mount and
	// resets the personal slot unless p_preserve_slot_state selects an already-live
	// UseGun parent/personal slot.
	void set_local_player_weapon(const Dictionary &p_def, const Dictionary &p_clip_seconds,
	                             bool p_preserve_slot_state = false);
	// The production mount (S6b, ADR 0028): find the row in the RETAINED
	// weapon.def parse, bake the FSM def from it, and seed the clip rings from
	// the rig's own .adm through the sim's mounted index — one step at ACCEPT
	// time, no shell dictionary and no render dependency [orig: the ACCEPT
	// chain rebuilds the slot table + mounts with no render dependency —
	// WeaponSlotTable_LoadAllFromDefs @ 0x5414e0 + Player_MountWeaponSlot
	// @ 0x4dfa40; Anim_InitActions @ 0x541fa0 bakes the delays]. Returns false
	// when the name is not in the retained table (caller keeps the current
	// weapon, mirroring the armory guard). The Dictionary pair above survives
	// as the GUT synthetic-def seam and retires with S7a.
	bool install_local_player_weapon_by_name(const String &p_weapon_name,
	                                         bool p_preserve_slot_state = false);
	// Render-side late binding of .adm clip lengths for the already-mounted def.
	// This is the only path allowed to preserve a same-name live action slot and
	// queued presentation [orig: FP model resolve @ 0x4ded60 is not a mount].
	void rebake_local_player_weapon(const Dictionary &p_def,
	                                const Dictionary &p_clip_seconds,
	                                bool p_preserve_slot_state = false);
	void clear_local_player_weapon();
	void set_local_player_first_person_model_available(bool p_available);
	// Per-frame trigger state: fire held + edge, raw reload edge (the dispatch
	// gate runs sim-side) [orig: the binding-149/reload input dispatch,
	// Input_HandleActionBinding_0 @ 0x4e0420].
	void set_local_player_weapon_input(bool p_fire_held, bool p_fire_pressed,
	                                   bool p_reload_pressed);
	// The ADS toggle request: gated by the dispatcher rules (no toggle during
	// RELOAD/SWITCHFROM, def Flags & 3 required), flips the sim-owned engaged bit
	// and queues the scopeup/scopedown FSM states. Returns whether it toggled.
	// [orig: input case 6 @ 0x4e0420; Player_ToggleWeaponScope @ 0x4df0c0;
	//  WeaponSlot_TryQueueScopeUp @ 0x53f050 / ..ScopeDown @ 0x53f080]
	bool request_local_player_scope_toggle();
	// Retail action 26 (default B): toggles the persistent binocular request.
	// The effective raised/view bits are derived each tick from movement, life,
	// round-end, and camera mode. Returns the new requested state; false also
	// represents a refused toggle.
	bool request_local_player_binoculars_toggle();
	// Retail action 41 (default N), deliberately independent of the mission's
	// EnableNVG night-semantics flag. Returns the new active state.
	bool request_local_player_nvg_toggle();
	// Retail actions 56/57 (default +/-), available even while NVG is off.
	// Returns the clamped gain in [0,4].
	int request_local_player_nvg_gain(int p_delta);
	// The view actions' chase preference (view1st/viewwithgun -> false,
	// viewchase -> true) [orig: g_camera_third_person_selected @ 0xA860DF]; the
	// camera mode itself is RESOLVED per tick by the arbiter from the
	// preference and the seat (world/player_view.h player_view_resolve_mode).
	void set_local_player_third_person_selected(bool p_selected);
	// The debug menu's on-foot third person — stock JO never resolves it
	// (net-re §5.39, the onhook debug affordance); never a gameplay key.
	void set_local_player_debug_third_person(bool p_enabled);
	// The shell-sampled head-bone eye (Godot space) for the 3P anchor chase; pass
	// valid=false when no skeleton sample exists (falls back to Position + 1.0).
	void set_local_player_eye(const Vector3 &p_eye_godot, bool p_valid);
	// The view-state snapshot: {scope_engaged, scope_fraction, fov_h_deg,
	// tp_anchor (Godot space), tp_anchor_valid}. Read-only; ticked at 62.5 Hz.
	Dictionary get_local_player_view() const;
	// Horizontal -> vertical projection fov (degrees) through the aspect — the
	// ONE conversion both cameras use [orig: @ 0x58d900].
	static float fov_vertical_from_horizontal(float p_fov_h_deg, float p_aspect);
	// The presentation frame's view forward for mission-euler angles, the aim
	// ray's far point and the binocular rangefinder readout — the engine's
	// world/presentation_frame.h (no presenter spells the swizzle).
	static Vector3 presentation_forward(float p_yaw_deg, float p_pitch_deg);
	static Vector3 aim_ray_endpoint(const Vector3 &p_eye, float p_yaw_deg, float p_pitch_deg);
	static int rangefinder_units(const Vector3 &p_position, const Vector3 &p_endpoint);
	// The FP viewmodel rig's frame math (simassets/fp_viewmodel_spec.h): the
	// view-frame -> camera-local axis map, the def rotation bias as camera
	// euler radians, the rig yaw, the renderfov default, the TEX_TEAM byte.
	static Vector3 viewmodel_camera_local_from_view(const Vector3 &p_view_units);
	static Vector3 viewmodel_bias_euler_rad(const Vector3 &p_rot_bias_deg);
	static float viewmodel_rig_yaw_deg();
	static float weapon_render_fov_h_deg_default();
	static int viewmodel_team_byte(int p_team);
	// The MCP/probe mirror of the attach-command seat selection over a flat
	// seat list [{type, occupied, ...}] (world/vehicle_attach.h
	// predict_seat_selection): {command, seat_index, seat, candidates}.
	static Dictionary predict_mount_seat(const Array &p_seats, int p_command_id);
	// The waypoint-track snapshot for the HUD label: {show, count, current,
	// number, name_id, position (Godot space), done}. current is -1 with no
	// selection; number is the 1-based display index [orig: hudInfo+373 =
	// list index + 1 @ 0x4b88e8]. Read-only; the track advances in the world
	// tick. (docs/interface/hud-re.md §Waypoint HUD)
	Dictionary get_waypoint_hud_view() const;
	// Header {version, stride, row_count}, followed by rows {bank, handle, x,
	// y, z, heading_bam, icon, argb, flags, source, remaining_ticks,
	// entity_known, policy_flags, half_x_q16, half_y_q16, floor_px}.
	// Coordinates are mission 16.16; the policy tail is resolved from the
	// LOCAL entity's def class exactly where retail resolves it (witness at
	// world::minimap_blip_draw_policy). No native pointers escape.
	PackedInt32Array get_hud_minimap_snapshot() const;
	// Static footprint polygons for footprint-class markers (buildings/zones
	// with marker models): {version=1, count} then per row {handle,
	// fill_argb, fill_value_count, xy_q16..., edge_value_count, xy_q16...}.
	// Baked once per world from the OOBJ occlusion ground-slice mesh transformed
	// by the entity pose (witness at world::minimap_footprint_from_occlusion).
	PackedInt32Array get_hud_minimap_footprints() const;

private:
	// get_hud_minimap_snapshot cache: the retained banks bump
	// ClientMinimapState::revision, and every other input — entity resolves,
	// per-row draw policies, the restored local row — advances only with the
	// world logic tick, so the display frames between 62 Hz ticks reuse the
	// built array instead of re-walking the 1160 retained slots.
	mutable PackedInt32Array minimap_snapshot_cache_;
	mutable uint64_t minimap_snapshot_revision_ = 0;
	mutable uint64_t minimap_snapshot_tick_ = 0;
	mutable uint16_t minimap_snapshot_local_handle_ = 0xFFFF;
	mutable bool minimap_snapshot_valid_ = false;

public:
	// { present: bool, position: Vector3 } — the type-2043 grid-origin marker.
	Dictionary get_hud_map_grid_origin() const;
	// The objectives-panel rows: an Array of {slot, text_id, shown, done} for
	// header slots 1..8, terminated at the first 0/255 win-condition id —
	// exactly the panel's row walk [orig: HUD_DrawWinConditions @0x5ba9e0..;
	// shown = show-win bit, done = won bit].
	Array get_objectives_view() const;
	// The FSM snapshot for the shell: latest clip/action payloads, diagnostic serials,
	// ammo, kick, and the 3P body channel. Ordered presentation events drain through
	// drain_local_player_weapon_events(); the snapshot alone is not an event queue.
	Dictionary get_local_player_weapon_state() const;
	// Destructively drain the ordered presentation outputs accumulated since the
	// previous render frame. Each Dictionary encodes one PlayerWeaponEvent.
	Array drain_local_player_weapon_events();
	// Destructively drain the flight sim's resolved round impacts, each row already
	// mapped through the ammo effects_table to {position, direction, effect, sound}
	// [orig: Projectile_SpawnImpactEffect @ 0x4e9b80; world/round_sim.h RoundImpact].
	Array drain_round_impacts();
	// Destructively drain permanent terrain-cache scorch insertions. Bounds are
	// already folded from mission (x,y) to terrain/Godot horizontal (x,z).
	Array drain_terrain_scorches();
	// Drain this frame's folded S2C 0x1E game events as feed rows — one per
	// line the original would post to its message feed [orig: the 0x426270
	// handler]. Each row carries the actor NAMES (resolved here, where the
	// decoded roster lives), the canned-message key (plus the camp rows'
	// WPNames level key), and the witnessed line color; the embedder resolves
	// the keys against gametext and calls the format helpers below.
	// Suppressed types (the LFP result set + the tip-only 58) never appear.
	Array drain_feed_events();
	// The folded Tab board's HEADER (netsim ClientScoreboard counts + the
	// session strings): known/team_mode/timed, the witnessed players count
	// (accepted rows minus the spectator trailer, netsim::scoreboard_header),
	// in_game/spectators, game_type, server and mission names. The rows no
	// longer round-trip through script — HudOverlay pulls them natively via fill_scoreboard_rows.
	Dictionary get_scoreboard() const;
	// The native Tab-board row handoff: fills the drawer's entries via the
	// netsim projection (netsim::project_scoreboard — wire order, the server
	// sorts and the client never re-sorts). NOT ClassDB-bound; HudOverlay
	// calls it through this typed seam. Returns false (rows cleared) when no runtime exists.
	bool fill_scoreboard_rows(
			std::vector<opennova::hud::ScoreboardEntry> &r_rows) const;
	// The folded board's team-table count (netsim ClientScoreboard::team_count,
	// the host's configured side count as the 0x16 carries it); 0 without a
	// runtime. NOT ClassDB-bound; HudOverlay reads it beside the rows.
	int scoreboard_team_count() const;
	// The mounted-vehicle panel (hud/hud_vehicle_panel.h, world/vehicle_panel_feed.h):
	// {shown, item_id} — the panel's root vehicle (the attached gun child
	// re-roots to its parent), whose items.def sid the shell joins to its
	// VEHICLE_HUD block. Read-only.
	Dictionary get_vehicle_panel_view() const;
	// The native panel handoff (NOT ClassDB-bound; HudOverlay::set_vehicle_panel
	// calls it): the hull's health band + one row per authored seat pair
	// (occupancy, rider health, the seat-select digit, the own seat). Returns
	// false (rows cleared) when the local player rides nothing.
	bool fill_vehicle_panel(const DefVehicleHudBlock &p_block,
			opennova::hud::HudVehiclePanelState &r_state) const;
	// The AAS zone status panel feed (world/lfp_feed.h), NOT ClassDB-bound:
	// one HudLfpZone per spawn-zone list entry joined with the client
	// runtime's zone-timer entry and the zone's transient minimap slot flags.
	bool fill_lfp_zones(int p_local_team,
			std::vector<opennova::hud::HudLfpZone> &r_zones);
	// The in-match game type for every role (joiner header / HostClient view).
	int64_t get_session_game_type() const;
	// The S2C 0x14 player-chat lines since the last drain, each routed by the
	// witnessed channel table: [{text, argb, sink, channel}] where sink 0 =
	// the SYSTEM ring, 1 = the CHAT ring, 2 = the message queue, 3 = channel 3.
	Array drain_chat_lines();
	// Substitute actor names into a canned template [orig: Chat_FormatMessage
	// @0x422C60]: the STRCND48 bonus re-compose when `extra` names the local
	// player, then $A/$B sequential case-insensitive replace-all. Exposed so
	// the string lookup can live with the string table while the substitution
	// rule stays in engine C++.
	String format_feed_line(const String &p_template, const String &p_attacker,
			const String &p_victim, const String &p_extra,
			const String &p_bonus_template) const;
	// Compose a camp line — the template's %s takes the level's WPNames string
	// [orig: the case-59/60 sprintf @0x427327/@0x42736B].
	String format_feed_camp_line(const String &p_template,
			const String &p_wpname) const;

	// --- the local player's loadout: slot pool, spawn kit, map rules -------------------
	// (the 2026-07-18 loadout grill; witness map in docs/net/novaworld-net-re.md §5.57)
	// The spawn kit [orig: the 2048-B tuple buffer 'restrictionData' @ 0x24D4E00]:
	// rows {name, ammo_primary, ammo_secondary, flags} (values default -1). When
	// p_filter_by_availability, the kit is filtered through the availability table
	// with the knife fallback — the SP .bms promote leg [orig: Mission_LoadBMSFile
	// @ 0x40f7ae]; the armory/profile legs store unfiltered (the server validates).
	// An empty kit resets to the engine default {WPN_M4AUTO} [orig: @ 0x5246be].
	void set_spawn_loadout(const TypedArray<Dictionary> &p_kit, bool p_filter_by_availability);
	// True only after a mission/profile explicitly supplied a spawn kit; the
	// WPN_M4AUTO engine fallback created by load_weapon_table leaves this false.
	bool has_explicit_spawn_loadout() const { return kernel_->loadout.spawn_kit_set; }
	// The map weapon-availability rules [orig: g_armoryWeaponAvailability @ 0x24D5600]:
	// reset to all-allowed, then apply {name, value} pairs (the .mis item_availability
	// chunk shape; -1 maps to 3, sub-weapons inherit the parent's value)
	// [orig: build_item_restriction_table @ 0x54DDB0 name-list mode].
	void set_weapon_availability(const TypedArray<Dictionary> &p_pairs);
	// Availability by weapon name: 0 banned / 1 allowed / 2 armory-zone-only /
	// 3 mission-allowed; unknown names read 1. The armory UI filter term
	// [orig: populate_three_category_lists @ 0x566e6b nonzero test].
	int get_weapon_availability(const String &p_weapon_name) const;
	// The armory ACCEPT apply [orig: WeaponLoadout_ApplyFromBuffer @ 0x565cd0 offline
	// leg]: the accepted kit becomes the spawn kit, the slot pool refills from it
	// (sub-weapons expanded), pools reseed + clips recalc, and the equipped slot
	// re-selects. Rows whose weapon is availability-banned are refused (the server
	// 0x2F validation shape, availability 2 requires the armory zone the ACCEPT is
	// gated on anyway [orig: @ 0x515a4a]). Also stamps player_class when 5..9.
	bool apply_local_player_loadout(const TypedArray<Dictionary> &p_kit, int p_player_class);
	// Commit the profile class without replacing a mission-authored weapon kit.
	bool set_local_player_class(int p_player_class);
	// Load the player's weapon profile (weapon.sav) from an ABSOLUTE filesystem path.
	// This is a save file, not a mounted PFF/loose resource, so it is read through
	// FileAccess rather than the resource root. Header gate: magic "FPBC" + version
	// "0211", then five 0x1080C profile-slot records; slot 0 becomes the active
	// record and its class bytes are clamped to [5,9]. A missing or malformed file is
	// NOT fatal — the shipped defaults stay installed and an Error is returned so the
	// caller can warn. [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0 (the
	// "expansion\\<g_ExpansionName>\\weapon.sav" path build @0x54f68c..@0x54f6b7);
	// the session-start clamp apply_session_settings_to_globals @0x5516ab]
	Error load_weapon_profile(const String &p_path);
	// Read slot 0's two character headers (raw bytes, no session clamp) without
	// requiring a live Simulation. Returns {error, loaded, blue, red}; each side
	// carries player_class, avatar_a (nationality id), avatar_b (division id),
	// and avatar_packed. This is the menu boot seam over the same five-record
	// file as load_weapon_profile().
	static Dictionary read_weapon_profile_summary(const String &p_path);
	// Persist PLAYER_INFO's ACCEPT snapshot into active profile slot 0:
	// `profile.player_class` (5..9) is written to BOTH side blocks and each
	// non-empty `profile.side_profiles[side]` {avatar_a, avatar_b, avatar_packed}
	// to its own block — playersav::update_avatar_selection carries the
	// save_player_info_from_dialog witness. The other four slots and every kit
	// page survive; the file is replaced atomically.
	// ERR_INVALID_PARAMETER when the snapshot carries no committable side.
	static Error save_weapon_profile_selection(const String &p_path,
			const Dictionary &p_profile);
	// The profile file's path RULE relative to the mount root (playersav
	// weapon_sav_relpath): with an active expansion retail looks ONLY under
	// "expansion/<name>/", never the root [orig: the path build
	// @0x54f68c..@0x54f6b7]. Static so shell path assembly stays a join.
	static String weapon_profile_relpath(const String &p_expansion_name);
	// The FP viewmodel submit spec {gun, arms, adm, show_arms} (simassets
	// fp_viewmodel_spec [orig: Player_RenderFirstPersonViewModel @0x4ded60;
	// the emplaced arms omission @0x4dedc7]). `character_arms` is the local
	// player's resolved combo arms graphic (retail's CharacterEntity arms model,
	// the ONLY arms source — weapon.def gfx1a/gfx1b are discarded tokens);
	// has_def=false is the bring-up path; an empty gun on a resolved def means
	// submit no FP gun.
	// The witnessed viewmodel placement units, re-exported from engine
	// simassets/fp_viewmodel_spec.h.
	static double weapon_def_pos_scale();
	static Vector3 viewmodel_fallback_pos_units();
	static Vector3 viewmodel_fallback_tpos_units();
	static Vector3 viewmodel_fallback_rot_bias_deg();
	static double viewmodel_pass_near_z();
	static String viewmodel_bringup_fallback_weapon();
	// Player-view calibration re-exports (world/player_view.h).
	static double player_eye_min_above_position();
	static double player_non_person_eye_bump();
	static int player_head_bone_index();
	static double player_aim_project_range();
	// The witnessed person hit-zone -> damage-multiplier table and the
	// Landable seat-branch bone leg (world/round_sim.h carries the witness;
	// the engine 6.0 seat leg is the truth the debug views mirror).
	static double hit_zone_damage_multiplier(int p_section) {
		return opennova::world::hit_zone_damage_multiplier(p_section);
	}
	static double seat_hit_bone_damage_multiplier(int p_bone) {
		return opennova::world::seat_hit_bone_damage_multiplier(p_bone);
	}
	// The per-axis portal-slot collection range, world units
	// (world/occlusion.h kPortalSlotCollectRadius).
	static double portal_slot_collect_radius() {
		return opennova::world::kPortalSlotCollectRadius;
	}
	// The mission coordinate domain in world units (world/geom.h — the signed
	// 16.16 carrier span the debug/edit fields clamp to).
	static double mission_coord_min() {
		return opennova::world::kMissionCoordMinUnits;
	}
	static double mission_coord_max() {
		return opennova::world::kMissionCoordMaxUnits;
	}
	static Dictionary fp_viewmodel_spec(bool p_has_def, const String &p_gfx1,
			const String &p_character_arms, const String &p_animadm, int p_flags);
	// Read-only view of the active profile record for the shell's status copy:
	// {loaded, blue: {player_class, avatar_a, avatar_b, avatar_packed, kit: [names]},
	//  red: {...}}. The kit array is the SELECTED page — the one the class byte picks.
	Dictionary get_weapon_profile_summary() const;
	// Rebuild the local player's slot pool from the spawn kit and select the spawn
	// default — the Player_InitPlayer weapon leg [orig: @ 0x4e15f0: display list ->
	// table fill -> pool seed -> clip recalc -> SelectWeaponSlot(195) ->
	// SwitchToWeaponByHandle(195)]. Runs automatically after load_weapon_table; call
	// again on respawn.
	void respawn_local_player_loadout();
	// The category keys [orig: input actions 200-210 @ 0x4e1144 ->
	// Player_SwitchToWeaponByHandle((action-200)*65) @ 0x4e0170]. Category 1..9 =
	// the retail Knife/Sidearm/Primary/Flashbang/Frag/Smoke/Accessory/Detonator/
	// Medpack keys ('1'..'9').
	void request_local_player_weapon_category(int p_category);
	// Next/previous weapon [orig: input cases 212/214 -> Player_CycleWeaponSlot
	// @ 0x4dfe70, direction +1/-1].
	void request_local_player_weapon_cycle(int p_direction);
	// Inventory snapshot for hosts/tests: {equipped_combo, equipped_name, slots:
	// [{combo, name, clip}], pools: {class_name: rounds}, carry_flags}.
	Dictionary get_local_player_inventory() const;
	// Canonical, unexpanded current tuples for the armory host. Retail preselects
	// visible parent rows from g_armoryLoadoutBufferByClass, never from the expanded
	// weaponSlotArrayBase [orig: populate_ammo_type_combo_boxes @ 0x564930].
	TypedArray<Dictionary> get_local_player_loadout() const;

	// --- WAC scripts ------------------------------------------------------
	// Install a compiled program on the script VM (WacProgram). Applied now if
	// loaded and re-applied on every (re)load. Pass null to uninstall.
	void set_wac_program(const Ref<WacProgram> &p_program);
	// Compile `sources` against the LIVE promoted world (symbolic group/area names
	// resolve through the registry) and install on success. False (program not
	// installed) when compilation has errors; the retained WacProgram holder
	// carries the diagnostics.
	bool compile_and_set_wac(const PackedStringArray &p_sources);
	// Retail executes the freshly installed WAC once before the 255-tick
	// environment settle. Host/standalone authority only; idempotent per load.
	bool run_mission_start_wac();
	// Replace the early post-BMS restore point with the fully settled play-start
	// state, including WAC temporal/RNG state.
	void seal_mission_start_baseline();
	// { loaded, paused, runs, event_count, code_size } for transport/debug UI.
	Dictionary get_wac_state() const;
	// Last-frame microsecond counters for the runtime hot path. Allocates only when queried.
	Dictionary get_runtime_perf_counters() const;
	// One opt-in seam for native sim/net/present/occlusion timings and
	// projectile collision attribution. Disabled by default so ordinary play
	// performs no native profiling clock reads or timing-counter writes.
	void set_runtime_profiling_enabled(bool p_enabled);
	bool is_runtime_profiling_enabled() const {
		return runtime_profiling_enabled_;
	}
	// Allocation-free last-tick trace sampling for the F3 hot path. Vector
	// lanes are times=(terrain, static, dynamic, person),
	// counts=(calls, static survivors, dynamic survivors, person survivors),
	// and faces=(static, dynamic).
	Vector4i get_last_projectile_trace_times_us() const;
	Vector4i get_last_projectile_trace_counts() const;
	Vector2i get_last_projectile_trace_faces() const;
	// Allocation-free int forms of the same last-frame counters, for per-frame
	// sampling by the F3 frame-stats board (a Dictionary per frame would churn).
	int64_t get_last_net_tick_us() const { return static_cast<int64_t>(last_net_tick_us_); }
	int64_t get_last_present_snapshot_us() const {
		return static_cast<int64_t>(last_present_snapshot_us_);
	}
	int64_t get_last_occlusion_build_us() const {
		return static_cast<int64_t>(last_occlusion_build_us_);
	}
	int64_t get_last_occlusion_probe_us() const {
		return static_cast<int64_t>(last_occlusion_probe_us_);
	}
	// Script-disable gate [orig: dword_C6EB28].
	void set_wac_paused(bool p_paused);
	bool is_wac_paused() const;

	// Drain the World EffectLog as an Array of Dictionaries {kind, a, b, c, d, str} and clear
	// it. Presentation-only (text/dialog/win/subgoal/show_waypoints/set_light); state mutation
	// is applied in-engine, never here.
	Array drain_effects();

	// The shell fire-presentation drain: one Dictionary per round spawned since the
	// last call — {origin: Vector3 (godot), forward: Vector3 (godot, unit),
	// shooter_handle, is_local_player, ammo_index, sound_set, effect, mf_light} with
	// the ammo-def 'ai_launch'/'ai_launcheffect' names resolved. The fire present
	// pass plays/spawns per event, skipping the local player (whose action-slot
	// presentation is already ported). [orig: WeaponSlot_FireAndSpawnEffects
	// @0x53F440 — the firing host presents its own rounds inline at fire time;
	// world-wac-ai-re §17.4]
	Array drain_fire_presentation_events();

	// The fire-sound legs on the logic clock (world/fire_sound.h): the shell
	// stamps the camera listener each frame before the tick batch, and drains
	// the ready one-shots ({set, pos, source_bms_id} rows) each present.
	// [orig: listener_pos @ 0x24D6630; Sound_TickPendingSlots @ 0x529310]
	void set_sound_listener(const Vector3 &p_listener_godot);
	Array drain_fire_sounds();

	// The eased FP viewmodel view-offset in VIEW-FRAME world units (X=fwd,
	// Y=left, Z=up) from raw weapon.def pos/tpos units — the /256 blend +
	// NoCardSwitch suppression run in world/player_view (S8), then the
	// per-frame motion lead (the damped movement-delta tracker) and the 0x500
	// z drop when the viewport frames 4:3 or narrower — the rig samples the
	// viewport SIZE (device work) and the 3w<=4h rule itself is the engine's
	// (world/player_view.h player_view_narrow_aspect). The rig maps view axes
	// onto its camera frame. [orig: Player_UpdateFirstPersonCamera @ 0x4dd380
	// — lead @ 0x4dd4f2..0x4dd56c, narrow-aspect drop @ 0x4dd571]
	Vector3 local_player_viewmodel_bias_view_units(
			const Vector3 &p_pos_raw_units, const Vector3 &p_tpos_raw_units,
			int p_viewport_w, int p_viewport_h);

	// The sound-profile chain [orig: SoundProfile_LoadAll @ 0x527490 /
	// Entity_GetProfileSlotSound @ 0x528300]: feed SndProf.def text (VFS
	// bytes) — parsed into world.sound_profiles now and re-applied on
	// reset_world; per-entity bindings resolve in resolve_ai_weapons.
	void set_sound_profiles(const PackedByteArray &p_sndprof_text);
	// The mission water plane (godot Y units) the footstep water pick and the
	// landing legs compare feet against [orig: Env_WaterHeightFixed @ 0x26C6454].
	void set_water_z(double p_water_y);
	// Drain the per-tick slot-sound emissions (footsteps/foley/landing/screams):
	// one Dictionary per event — {set: String, pos: Vector3 (godot), handle,
	// slot} — played by the fire present pass at full volume
	// [orig: Entity_PlaySound3D_FullVolume @ 0x528e20].
	Array drain_slot_sounds();

	// Queue this frame's REMOTE-body footsteps and foley for one wire row.
	// Runs only for wire-RENDERED bodies (a joiner's remote rows, a listen
	// host's admitted players); the authority tick's sound pass never reaches
	// net-snapped peers, so each drawn body has exactly one source. The
	// applier supplies the row's identity, the wire-driven clip playhead span
	// it just crossed, and its world pose; the witnessed consume itself is
	// the portable world::wire_body_slot_sounds (world/wire_body_sound.h),
	// fed through the same SoundSlotEvent drain the authority bodies use
	// [orig: the org1/org2 sound blocks, see docs/audio/lwf-dbf-sound-re.md].
	void present_wire_body_sounds(int p_type_id, int p_character_id,
			int p_wire_handle, int p_carrier_handle, int p_anim_state,
			int p_from_phase, int p_to_phase, const Vector3 &p_pos);
	// Drain persistent entity-attached emitter registrations. Producers refresh
	// a keyed (source_spawn_id, lane) intent; the audio layer expands `set` into
	// LWF layers and owns keep-alive, spatial ranking, and physical voices.
	// Rows are {source_spawn_id, handle, source_bms_id, pos, lane, slot,
	// lifetime, emitted_tick, pitch_q16, volume_q8_8, source_only, set}.
	// [orig: SoundEmitter_Register @0x529270]
	Array drain_sound_emitters();

	// The live tracer TRAIL channels — the per-round point rings behind every streak,
	// framed per channel as [style_id, age, count, then count x (x, y, z, w)] in
	// godot space; the friendly/enemy style is already selected at spawn vs the local
	// team, and killed rounds' channels keep draining until empty. The fire present
	// pass builds the camera-facing ribbons from these.
	// [orig: the 256-channel pool g_TracerEmitterPool @ 0x2BF5270 — alloc
	// RoundData_SpawnRound @0x4ec774, append Projectile_UpdatePhysics (pre-move,
	// 1/tick), drain CEffectEmitterPool_Tick @0x5db830, draw
	// CEffectChannel_RenderRibbon @0x5db8a0; non-tracer rounds have no channel and
	// are invisible in flight (graphicModel zeroed @0x4ec900). The witness map lives
	// in world/tracer_trails.h.]
	PackedFloat32Array get_tracer_trails() const;

	// The in-flight round glows: one row per active round whose ammo authors
	// `light_move` — {id (presentation generation), pos (godot space), radius,
	// color}. The presenter's light pool spawns a permanent (mode 1) light per
	// id, follows it per tick, and despawns dropped ids [orig:
	// RoundData_SpawnRound @0x4ec8da spawn, the per-tick follow @0x4eaa9f,
	// Projectile_ReleaseEffects clear — witness map on engine/runtime/renderer/light_scene.h].
	Array get_round_glow_rows() const;

	// The styled ribbon compile over trail rows (renderer/tracer_frame.h owns
	// the witnessed style tables and the camera-facing build
	// [orig: CEffectChannel_RenderRibbon @ 0x5DB8A0]). Static so the present
	// pass and stub-sim tests share the one native seam:
	// {additive: {positions, colors}, alpha: {positions, colors}, channels} —
	// each family one triangle-strip vertex run (channels joined by degenerate
	// pairs), ready for verbatim ImmediateMesh upload.
	static Dictionary compile_tracer_ribbons(const PackedFloat32Array &rows,
			const Vector3 &camera);

	// The destruction presentation drain (world/destruction.h; world-wac-ai-re
	// §24): {effects[], sounds[], husk_swaps[], debris_triangles, glass_points,
	// explosions_processed, items_destroyed}, godot-space positions, cleared on
	// read. Once per present, beside the fire drain.
	Dictionary drain_destruction_events();
	// The live death-piece pool as dictionaries {slot, generation, item_id,
	// section, type_index, scale, pos, heading, pitch, settled} — each piece renders as its single
	// husk-model section. [orig: DeathPiece_TickAll @0x57b900; §24]
	Array get_death_pieces() const;
	// Per-entity destruction diagnostics by bms_id (probe/F3 seam): health,
	// bound_radius, flags, traits presence, KZ/bridge-DEAD anchors — the damage
	// chain's gate inputs.
	// (entity_card is the per-entity debug card; this one resolves by the
	// placed bms_id and carries the §24 gate fields.)
	Dictionary get_destruction_debug(int p_bms_id) const;

	// Mission scripting state on the shared world (the dword_C6B240 var store + event gates).
	void set_mission_variable(int index, int value);
	int get_mission_variable(int index) const;
	bool has_event_fired(int index) const;
	int get_event_count() const;

	// --- Read-only introspection (dev tools / MCP tooling) -----------------
	// The world's logic tick counter [orig: current_tick @0x24c1968]. The
	// pre-mission pass in finish_load already advanced it once, so a freshly
	// loaded mission reads 1 — consumers should track deltas, not absolutes.
	int64_t get_logic_tick() const;
	void set_panm_time_ms(int64_t p_time_ms);
	int64_t get_panm_time_ms() const;
	void debug_set_panm_time_ms(int64_t p_time_ms);
	// Native pose-path health: cumulative queries/declines for the collision
	// provider and the mounted resolver, plus the installed mounted model
	// sources. The soak gates on declines == 0 — the A/B divergence stats this
	// replaces were retired with the cutover.
	Dictionary debug_native_pose_stats() const;
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
	// The typed entity inspection API (world::inspect, ADR 0042 d5). The join
	// and both card halves are computed engine-side; this binding forwards and
	// converts into typed records — JSON conversion lives on the records
	// themselves and runs only at the MCP boundary (to_json_value). A joiner's
	// directory carries no AI join (the non-authoritative tooling pool never
	// mixes into the decoded view), and its cards ride the decoded replica
	// section (np::client_replica_card).
	TypedArray<EntityRow> entity_directory() const;
	// Native (unbound) form for the in-process C++ dev tools (ADR 0042 d6): the same engine
	// join, returned as the engine vector — no TypedArray/Variant round-trip. Empty without a kernel.
	std::vector<opennova::world::inspect::EntityRow> native_entity_directory() const;
	// Native (unbound): the AI pool index behind a packed wire handle (-1 = no brain / no kernel) —
	// the dev-tools drain resolves a queued request's handle onto the ai_index the delegates key on.
	int native_ai_index_for_handle(int p_handle) const;
	// The engine's tool/probe mutation seam by entity handle (ADR 0042 d5), for the C++
	// embedders (DevTools) that already hold a handle; null without a kernel.
	opennova::world::EntityCommands *entity_commands();
	// Native (unbound): the engine card by value for the C++ dev tools; invalid without a kernel
	// or a resolving handle.
	opennova::world::inspect::EntityCard native_entity_card(int p_handle) const;

	// --- the F3 Weapon window's native seams (devtools; ADR 0042 d6) -------
	// Engine-typed records out (no Variant round-trip); null/-1 without a
	// kernel or nothing equipped. The retained row carries the AUTHORED delays
	// (-1 = `auto`); the ACTIVE slot is the UseGun-borrowed one when engaged;
	// the input block names the pump's gate ("" = accepted). Full contracts:
	// simulation_player_weapon.cpp.
	const opennova::world::LocalPlayerWeapon *native_local_player_weapon() const;
	const DefWeaponDef *native_equipped_weapon_row() const;
	int native_equipped_weapon_adm_index() const;
	const opennova::world::WeaponSlotState *native_active_weapon_slot() const;
	const char *native_weapon_input_block() const;
	std::vector<std::string> native_equipped_weapon_clip_keys() const;
	// Live ACTION edits — never the filesystem. Delays arrive AUTHORED and
	// mirror into the retained row so a re-install keeps them; only explicit
	// legs patch the live baked slot; `p_rebake` (a leg newly `auto`) or an
	// ANIM change takes the same-weapon re-bake that keeps the live slot,
	// serials and scope. p_field/p_trigger pair by static_assert at the drain.
	bool debug_weapon_set_action_delays(int p_action_id, int p_delay_start, int p_delay_end,
			bool p_rebake);
	bool debug_weapon_set_action_text(int p_action_id, int p_field, const String &p_text);
	// Queue an action through the REAL input seam; false when its gate refuses.
	bool debug_weapon_trigger(int p_trigger);
	void debug_weapon_set_fire_held(bool p_held);
	bool debug_weapon_fire_held() const { return debug_weapon_fire_held_; }
	// Arm, disarm and clear the pump's 62.5 Hz trace ring.
	void debug_weapon_arm_trace(bool p_armed);
	void debug_weapon_clear_trace();
	// The full card by packed wire handle; null when nothing resolves. The AI-index and SSN forms
	// wrap the same builder (edit seams key on ai_index; pool-1 vehicles have no brain, resolve by SSN).
	Ref<EntityCard> entity_card(int p_handle) const;
	Ref<EntityCard> entity_card_by_ai_index(int p_index) const;
	Ref<EntityCard> entity_card_by_net_id(int p_net_id) const;
	// Probe seam: write an AI entity's health via the scripted-SETHP stores (registry + motor
	// copy) so in-game probes can shorten a fight. Returns ERR_UNAVAILABLE without a live sim,
	// ERR_INVALID_PARAMETER for a missing AI index, and OK only after both mirrors are mutated.
	Error debug_set_entity_health(int p_index, int p_hp);
	Error debug_crew_vehicle(int p_occupant_ssn, int p_vehicle_ssn);
	void set_local_player_eye_offset(const Vector3 &p_offset_godot, bool p_valid);
	bool local_player_fp_weapon_hidden() const;
	Error debug_crew_local_player(int p_vehicle_ssn);
	// Authority test seam: queue a RoundDeath for the player entity at `handle` (killer = the
	// local player) so the next host tick runs the witnessed death transaction
	// (route_round_deaths: 0x13 fan, 0x52 camera, 0x54 medic state, the dead flag on the 0x0A
	// record). Not the health setter above: remote players are not AI rows.
	Error debug_kill_player_entity(int p_handle);
	// Probe seam: teleport an AI entity (mission-space coords) through both position stores, for
	// probes defeated by mission geography. Same truthful Error contract as debug_set_entity_health.
	Error debug_set_entity_position(int p_index, const Vector3 &p_mission_pos);
	// World-registry probe seam by SSN: mission-space teleport (the by-SSN
	// entity card is entity_card_by_net_id above).
	void debug_set_world_entity_position(int p_net_id, const Vector3 &p_mission_pos);
	// Exact-slot parity probe: set the authoritative MountSlot words on a
	// world entity so a real UDP phase-8 sample can prove receiver application.
	Error debug_set_world_entity_weapon_ammo(int p_net_id, int p_clip,
	                                         int p_reserve);
	// Per-entity items.def attrib override by packed wire handle (brainless rows included):
	// EntityCommands::set_entity_item_attrib. ERR_UNAVAILABLE without a kernel,
	// ERR_INVALID_PARAMETER for a handle/word out of range, ERR_DOES_NOT_EXIST when nothing resolves.
	Error debug_set_entity_item_attrib(int p_handle, int64_t p_attrib, int64_t p_attrib2);
	// Land the local player at an exact F3-dumped pose (probe seam). Returns
	// ERR_UNAVAILABLE until the complete local-player subject exists.
	// TEST SCAFFOLDING (host authority): kill a BMS command group outright so an
	// unattended round can reach a scripted win an autofiring bot cannot. Drives the
	// same EntityCommands::kill_group the BMS KILL_GROUP action uses; returns members
	// affected, or -1 with no world.
	int debug_kill_group(int p_group);

	Error debug_teleport_local_player(const Vector3 &p_mission_pos, float p_yaw_deg,
			float p_pitch_deg);
	// Round-outcome card: {ended, winner_team, bluekills, greenkills, enemy_kills,
	// team_kills_by_others, friendly_kills_by_others, enemy_kills_by_others, humans}.
	// The sim-side end-of-round state + the SP kill-stat buckets the epilog score
	// screen and the WAC bluekills/greenkills builtins read (probe + HUD source).
	// [orig: g_spawn_success_gate @0x24c1928 / g_round_winning_team @0x24c1924 /
	// the 0xC846xx buckets]
	Dictionary get_round_outcome_debug() const;
	// Human-readable AI state name, "?" for the id gaps [orig: Entity_LookupAIStateName @0x455cc0].
	static String ai_state_name(int p_state);
	// Infantry anim state id -> ADM clip key ("anim_<off_8135F0 name>"), empty for invalid gaps.
	static String infantry_anim_key(int p_state);
	// The adjacent retail transition-arbitration flags table (off_8139E8).
	static int64_t infantry_anim_flags(int p_state);
	// The queue gate over two of those flag words (world/infantry.h) — the one
	// rule the netsim record fold and the presenter body FSM both apply.
	static bool remote_body_state_defers(int64_t p_current_flags, int64_t p_next_flags);

	// Entity query. The (kind, index) pair lets the shell map a sim entity back to
	// its promoted mission record and already-rendered node.
	int get_entity_count() const;
	int get_entity_kind(int p_index) const;         // mission ItemType (3 = Organic), -1 if none
	Vector3 get_entity_position(int p_index) const; // mission (x,y,z) -> Godot (x, z, -y), units
	float get_entity_yaw_deg(int p_index) const;    // heading in mission degrees (for shell remap)
	int get_entity_state(int p_index) const;        // AI state id (16 = GROUND_FOLLOWWP)
	int get_entity_net_id(int p_index) const;       // runtime SSN (WAC/BMS addressing), 0 if none
	// Empty when no LIVE registry entity owns p_ssn. Unlike the AI-indexed
	// getters, this includes non-AI pools and drops immediately on despawn.
	PackedVector3Array get_entity_effect_state_for_ssn(int p_ssn) const;
	// Compact client-view attachment lookups. These mirror get_present_snapshot's
	// self-filter, wire quantization, and yaw conversion exactly; empty means the
	// identity is absent from this tick's presented view.
	PackedVector3Array get_present_effect_state_for_ssn(int p_ssn) const;
	PackedVector3Array get_present_effect_state_for_wire_handle(int p_wire_handle) const;
	PackedVector3Array get_present_effect_state_for_bms_id(int p_bms_id) const;
	PackedVector3Array get_present_effect_state_for_origin(int p_kind, int p_index) const;
	int get_entity_owner_connection_id(int p_index) const; // entity+0x78 dcb; the networked-player identity (D-NET-112)
	int get_entity_wire_handle(int p_index) const;  // (pool<<12)|slot — the per-entity wire identity
	// Godot-space positions of the entities the distant MODEL/depth-mask foliage
	// tier generates around: crouched or prone (MoveOrder stance bits 8-9) and
	// standing on terrain, not on another entity. Retail tests every visible
	// sector entity, but only infantry ever carry the stance bits.
	// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
	// (MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7]
	PackedVector3Array get_foliage_mask_anchor_positions() const;
	// PF_PHASE/PF_ACTIVE jointly encode one exact signed dword: PHASE carries
	// low16 and ACTIVE carries high16+1 (zero means unpublished). This avoids
	// float32 precision loss in the otherwise-float presentation snapshot.
	static int32_t decode_present_part_anim_phase(
			const PackedFloat32Array &p_snapshot, int p_base, int p_channel);
	// Raw signed part-anim channel dword (PLAYPARTANIM); channel is 1 or 2.
	// Ordinary sweeps occupy 0..0x10000, while zero-time wrapping states are
	// preserved. The shell renders the model part from this value.
	int get_entity_part_anim_phase(int p_index, int channel) const;
	// True when the authority publishes this semantic channel. This is an
	// ownership predicate, not a movement predicate: owned endpoints, including
	// zero, must overwrite a prior pose. Channel 1 is suppressed by
	// ItemDefAttrib 0x1000; channel 2 is unconditional.
	bool get_entity_part_anim_active(int p_index, int channel) const;

	// ONE batched present snapshot for the per-tick render pass: a flat PackedFloat32Array of
	// get_entity_count() records, PF_STRIDE floats each, fields per the PresentField enum. Avoids the
	// ~10 Variant-boxed scalar getter calls per entity the present loop would otherwise make.
	PackedFloat32Array get_present_snapshot() const;
	// Revision for the exact ordered identity layout of the most recently
	// returned snapshot. Pose-only changes keep this stable.
	int64_t get_present_layout_revision() const {
		return static_cast<int64_t>(present_layout_revision_);
	}
	int get_present_stride() const { return PF_STRIDE; }

	// Wire the terrain the AI grounds on (the shell's loaded TerrainData). Copies the depth
	// buffer + sector layout so the portable height field outlives the source and survives reload.
	// Null/unloaded clears grounding (entities keep their authored Z). GameWorld
	// and direct test/tooling fixtures call this through MissionPresentation.setup().
	void set_terrain_height_field(const Ref<TerrainData> &p_terrain);
	// S16 (ADR 0028): the seat/mount table installs through the NATIVE
	// extractor (simassets::extract_item_seat_specs) over the retained def
	// rows + the sim's own model parses. Seeds are full 1xxxxx def ids; the
	// extractor walks authored addeweap children transitively. The shell
	// GDScript extraction + its Dictionary install seam are gone — proven
	// equivalent live before the cutover (29/29 specs identical, 0 mismatches
	// on retail 00TRg, 2026-08-07).
	void install_native_seat_specs(const Ref<class ItemDatabase> &p_item_db,
			const std::vector<int> &p_seed_item_ids);
	// The joiner prewarm's wire-type install (a header-only join has no
	// mission-body table): seeds = streamed type ids + kItemIdOffset,
	// replacing the whole installed table exactly like the boot install.
	// False when the item db or the sim's asset root is missing.
	bool install_seat_specs_for_type_ids(
			const Ref<class ItemDatabase> &p_item_db,
			const PackedInt32Array &p_type_ids);

	// The AI-speed -> world-units locomotion factor (see AiSystem::loco_scale).
	void set_loco_scale(int p_scale);
	int get_loco_scale() const;

	// Wire the infantry root-motion source: resolve a model's .adm (e.g. "E_STAND.adm")
	// through the shell's resource root and keep its clips' root tracks. Returns the number
	// of anim states with a usable clip (0 = nothing loaded; org1 soldiers then stand —
	// motion comes from clips, as in the original). Survives reset_world like the terrain.
	int set_infantry_anim_map(const Ref<class ResourceRoot> &p_resource_root, const String &p_adm_name);
	int get_infantry_clip_count() const { return kernel_->root_motion.clip_count(0); }

	// Per-entity grounding: resolve every active infantry soldier's OWN model .adm (from its
	// items.def type id via the item database) and store its registry adm_id on the entity, so
	// each grounds + locomotes off its own clip rather than the shared default set. The
	// resolver inputs are retained so players spawned later receive their ADM automatically.
	void resolve_infantry_adm_ids(const Ref<class ResourceRoot> &p_resource_root,
	                              const Ref<class ItemDatabase> &p_item_db);

	// Per-entity items.def trait resolution: stamp each live entity's is_ai_capable (AIData
	// attrib — gates the 0x0D AI-trailer, D-NET-97), net_class_code (§5.10b *_function class
	// tag -> the 0x0A serialize class; an unresolved/ewep item must NOT be serialized as a
	// vehicle or the client desyncs), and health_max/health (items.def hp = healthMax
	// [orig: Entity_InitFromItemDef @0x49e550]). Idempotent; call after load (and again after
	// spawning the local player).
	void resolve_item_traits(const Ref<class ItemDatabase> &p_item_db);

	// The D-AI-5 host weapon seed: stamp every AI entity's anim-fire round from its
	// items.def ammo_closeattack + clipsize (AiProfile::ammo_primary/clip_size — the
	// single-ammo stand-in for the entity+0x358..0x35B family, whose load-time
	// block-copy writer is unwitnessed; world-wac-ai-re §17.4/§17.7 item 1), and seed
	// the spawn magazine [orig: Entity_ResetToSpawnState @ 0x4b97a9 — word
	// entity+0x35C = itemDef+0x894]. Ammo NAMES resolve against the mission ammo
	// table, so call AFTER load_ammo_table; unresolved/absent leaves the NPC unarmed
	// (ammo_primary -1, the fire pass skips). Idempotent; returns armed-NPC count.
	int resolve_ai_weapons(const Ref<class ItemDatabase> &p_item_db);
	// Install the packed Avatars.def character-sex registry used by the
	// portable sound-profile selector. Retained across reset_world; returns the
	// number of unique packed character ids installed.
	int set_character_avatar_database(
			const Ref<class AvatarDatabase> &p_avatar_db);

	// World-object collision sweep: for each live entity with an items.def graphic,
	// load its .3di collision block (BVOL volumes + BPLN planes via the placer's
	// ObjectData cache), register one runtime model per graphic on the sim
	// collision world, and attach the per-entity instance. From then on the infantry
	// motor resolves against placed objects — CB wall push-out, standing on roofs,
	// hurt/CA/BB triggers, and the CL ladder legs (frame extraction, entry gate,
	// alignment chase, climb states, exits) [orig: collision resolver @0x4b2bd0
	// + the query set; docs/world/world-wac-ai-re.md §15; D-INF-3].
	// Returns the instance count. Also attaches the render-occlusion portal
	// models (buildings whose graphic carries OVRT/OPLN/OFAC/OOBJ records)
	// with their def bits. Idempotent per load. Model extraction reads the
	// sim's own SimModelCache through the installed asset root
	// (set_asset_root; ADR 0028) — a rootless sim attaches nothing.
	int resolve_collision_instances(const Ref<class ItemDatabase> &p_item_db);
	// Install the mounted root the SIMULATION resolves assets through — the
	// engine-side mirror of the render mount. Pins the root's index for the
	// sim-model cache (ADR 0028).
	void set_asset_root(const Ref<class ResourceRoot> &p_root);

	// Mission-start portal init: register + weld + per-building flag stamp over
	// the attached occlusion models. Call once after resolve_collision_instances.
	// [orig: Terrain_InitBuildingPortals @ 0x5c7480 from Game_StartMission @ 0x525e11]
	void occlusion_init_mission();

	// The per-render-frame occlusion pipeline: building batch + portal slots +
	// occluder planes + the section-mask build + the per-entity render gates
	// (blink-hits + the outdoors three-ray latch). Camera in Godot space; fov_y in
	// degrees; fog/water in mission units; force_indoors mirrors the mission
	// attribute override [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8 -> accum |= 2].
	// [orig: Terrain_CollectVisibleEntities @ 0x5c9160 steps 1-4 + the collector gates]
	void run_occlusion_frame(const Transform3D &p_camera, double p_fov_y_deg,
	                         double p_aspect, double p_near, double p_fog_dist_units,
	                         double p_water_z_units, bool p_force_indoors);

	// Frame results: [bms_id, visible<<32 | mask] pairs for every building the
	// occlusion frame touched (mask = section bits with forced-visible def bits
	// applied; bit N = COBJ section / render part N; bit 0 = exterior).
	PackedInt64Array get_building_visibility() const;
	// bms_ids of non-building entities the collector gates culled this frame.
	PackedInt32Array get_render_culled_bms_ids() const;
	// Delta form of get_building_visibility(): only pairs whose packed value
	// changed since the last call, so the shell applies changes instead of
	// re-walking the whole building set every frame.
	PackedInt64Array get_building_visibility_changes();
	// Delta form of get_render_culled_bms_ids():
	// [n_added, ids..., n_removed, ids...] since the last call.
	PackedInt32Array get_render_culled_changes();
	// Per-draw sun-visibility feed (D-RLIT-3): triples
	// [wire_handle_or_-1, bms_id_or_0, quality 1..4] whose quality changed.
	// Exactly one identity is live per row. This is a cutover API: no bms-only
	// pair form remains. The shell maps quality through sun_visibility_factor;
	// the wire presenter applies it to late-built bodies and held weapons too.
	// The local player's quality is
	// computed but never emitted here — the presenter reads it via
	// get_local_player_sun_quality() so the FP parts can keep their witnessed
	// exemption while the third-person body dims.
	PackedInt64Array get_draw_lighting_changes(const Vector3 &p_light_dir);
	int get_local_player_sun_quality() const { return local_sun_quality_; }
	// Quality (1..4) -> the effectScale the render-state stack multiplies —
	// engine-owned so the mapping has ONE writer (renderer::
	// sun_visibility_factor carries the Entity_ComputeSunVisibility cite,
	// see docs/render/render-lighting-re.md). Both
	// presentation consumers (the occlusion sun feed and the local-player
	// body) call this instead of re-deriving the 0.25 step.
	float sun_quality_factor(int p_quality) const;
	// The present pass's visibility intent for one placed entity — the
	// occlusion release edge lands a node on the sim's CURRENT visibility so a
	// hidden entity never flashes for a frame.
	bool entity_present_visible(int p_bms_id) const;
	// Forget the applied-state baselines: the next delta call re-emits the
	// full frame state (the occlusion A/B seam and shell cache resets use it).
	void reset_occlusion_apply_baseline();
	bool occlusion_water_visible() const;

	// Read-only collision-world geometry for the F3 "Show collision" debug view:
	// { instances: [ { entity_handle, pos (Godot space), heading (mission yaw
	//   deg), volumes: [ { type, min_x..max_z (section-local units), corners:
	//   PackedVector3Array[8] (Godot world; index bit0/1/2 = max x/y/z in
	//   mission axes) } ] } ], player: { valid, position, points
	//   (PackedVector3Array[3]), radii (PackedFloat32Array[3]), capsule_bottom,
	//   capsule_top, foot_clearance } }. Volumes use the SAME fixed-point path
	// the resolver queries (CollisionWorld::debug_instances -> target_view ->
	// collision_matrix_from_heading): the drawn boxes ARE what movement
	// resolves against. Capped to instances within 150u of the local player
	// (first 128 with no player spawned). While the contact capture is armed
	// the report adds "hits" (stride-6 [target, age, kind, x, y, z], mask+TTL
	// filtered) + "hit_stride"/"hit_ttl"/"tick" — the overlay's flash channel.
	Dictionary get_collision_debug() const;

	// The AI overlay's per-frame payload (godot-space, the collision-debug
	// shape family): { valid, logic_tick, rows: [ per-brain state/alert/
	// target/aim_dir/muzzle/ranges/timers ], channels: [ nav routes as
	// PackedVector3Array node runs + radii + followers ], groups, counters }.
	// {"valid": false} without a kernel and on a joiner (the tooling AI pool
	// never joins the decoded view); an unloaded kernel reports valid with no
	// rows (the collision-debug contract). Aim directions are Godot-space
	// unit vectors computed natively — GDScript does no BAM math.
	Dictionary get_ai_debug() const;
	// Native (unbound) form for the F3 AI window's pushed record: the same
	// engine join as the engine struct, no Variant round-trip. False without a
	// kernel or on a joiner.
	bool native_ai_debug(opennova::world::inspect::AiDebugReport &r_out) const;
	// Read-only snapshot of the RoundSim debug ring for the F3 "Rounds" tab:
	// { tick, events: [ { tick, kind, kind_name, material, section, face,
	//   secondary_section, fallback, effect_tag, effect_tag_name, entity_handle,
	//   shooter_handle, ammo_index, husk, t, p0, p1, hit (Godot-space Vector3),
	//   entity_name } ] } — oldest first, capped at RoundSim::kDebugTrailCap;
	// every resolved outcome, face-miss fly-ons included.
	Dictionary get_round_debug() const;
	// Engine ray-debug capture (CollisionWorld rings + engine-owned mask/TTL
	// draw filter) behind the F3 "Show rays" view and Rays window; counts ride
	// native_rays_snapshot (ADR 0042 d6). Filter setter: -1 keeps a value.
	Dictionary get_ray_debug() const;
	void set_ray_debug_recording(bool p_enabled);
	bool is_ray_debug_recording() const;
	void set_ray_debug_filter(int64_t p_mask, int64_t p_ttl_ticks);
	void clear_ray_debug();
	bool native_rays_snapshot(opennova::devtools::RaysSnapshot &out) const;
	// Engine contact-debug capture (the CollisionWorld hit/contact ring) behind
	// the collision view's hit flashes and the F3 Physics window; the flashes
	// ride get_collision_debug's "hits" channel, counts ride
	// native_physics_snapshot (ADR 0042 d6). Mask setter clamps to the kinds.
	void set_contact_debug_capture(bool p_enabled);
	bool is_contact_debug_capture() const;
	void set_contact_debug_kind_mask(int64_t p_mask);
	void clear_contact_debug();
	bool native_physics_snapshot(opennova::devtools::PhysicsSnapshot &out) const;
	// Per-frame visual snapshot of item-modeled throwables: tracer-cadence flying
	// rounds with a TrcrID model plus placed devices. Entries: {key, item_id,
	// pos (godot), rotation_deg (pitch, yaw, roll — placer convention)}; the
	// enemy-team item swap follows the viewer team [orig: the S2C 0x59 dual
	// TrcrID words + the spawner's team pick @ 0x4ec79b; world-wac-ai-re §27].
	Array get_throwable_visuals() const;

	// The impact-scar draw list for ScarPresenter (simulation_scars.cpp):
	// World::scars compiled through renderer::compile_scar_draws with the shell's
	// camera (Godot space), fog distance and Env_TerrainLightCombined
	// (EnvFile.combine_terrain_light(sun, sky) — the sun+sky combine).
	// { vertices (PackedVector3Array, Godot axes; world space for shared-ring
	//   batches, SECTION-LOCAL for entity-ring batches), uvs, colors,
	//   batch_owner/texture/section/flags(bit0 entity_local, bit1 building)/
	//   first/count, batch_bms_id, batch_spawn_origin, strip_names,
	//   slots_live, slots_culled, rings_leased }. Empty without a world.
	Dictionary get_scar_draw_list(const Vector3 &p_camera_godot, float p_fog_distance,
			const Color &p_terrain_light) const;
	// The Scar_RenderCache owner gate over OcclusionWorld's section masks and
	// the entity's blink-box quad (see simulation_scars.cpp).
	bool scar_owner_visible(uint16_t p_owner_packed) const;

	// The round hit-detection reality for the F3 hitbox view:
	// { entities: [ { entity_handle, pos, bound_radius, husk, has_faces,
	//   face_total, tris (PackedVector3Array, triangle list, Godot world),
	//   materials (PackedByteArray per tri), flags (PackedInt32Array per tri) } ],
	//   organics: [ { entity_handle, section, pos (sphere center, Godot),
	//   radius, authored_radius, masked, fallback } ] }.
	// Triangles use the SAME husk-aware target_view + full-euler matrices the
	// projectile raycast uses — the drawn mesh IS the tested mesh; capped at 96
	// entities / 24000 item faces within 80 u of the local player (face_total
	// exposes truncation). Organic posed/fallback spheres share the range/actor
	// cap, omit the local avatar, and use the exact CollisionWorld target
	// matrices consumed by RoundSim.
	Dictionary get_hitbox_debug();

	// The F3 entity picker: one plain geometric trace_projectile segment
	// (terrain / water / static + dynamic CFAC / person bone spheres, nearest
	// wins) along a camera or crosshair ray. Read-only. Stable-shape
	// Dictionary; hit=false with blocked = "terrain"/"water"/"proxy" naming
	// why the ray stopped without a pickable entity (proxies = wire geometry).
	Dictionary debug_pick_entity(const Vector3 &p_from_godot,
			const Vector3 &p_dir_godot, float p_max_range_units);
	// Diagnostic round injector: spawns one live round through the REAL
	// RoundSim::spawn (production velocity/tracer/trail path; owner = the
	// local player) from a Godot-space origin along a Godot-space direction,
	// firing the named ammo ("AMMO_556", ...). The world tick flies it and the
	// F3 Rounds ring records the outcome — the pose-replay probe's seam.
	// Returns the round slot, -1 on bad ammo/full pool.
	int debug_spawn_round(const Vector3 &p_from_godot, const Vector3 &p_dir_godot,
	                      const String &p_ammo_name);

	// World-space portal-face geometry for the F3 "Show portal faces" 3D view:
	// { buildings: [ { bms_id, pos, visible, records: [ { type, section_a,
	//   section_b, pos, radius, glow, segments (PackedVector3Array a,b pairs) } ] } ] }.
	// Segments are each record's boundary outline — the OFAC edge words whose
	// low-15-bit shared-edge identity appears once (interior edges pair up and
	// drop, the same cancellation identity the occluder pass uses) — transformed
	// through the SAME render_matrix_from_pose path the engine's frame runs, then
	// mapped to Godot space. p_anchor (Godot) + p_range_units bound the sweep
	// per horizontal axis (range <= 0 = everything), capped at 128 buildings.
	Dictionary get_occlusion_portal_debug(const Vector3 &p_anchor, double p_range_units) const;

	// Local-player blink state [orig: g_LocalPlayerBlinkFlags @0x24C1934; entity Flags
	// 0x800000]. The render/audio hosts gate interior behavior on these.
	bool local_player_indoors() const;
	int local_player_blink_flags() const;
	// items.def id of the pool-2 building encoded by blink_hits[0], or 0 when
	// the player is not inside a blink volume. Entity::item_id is the raw BMS
	// type, so this accessor applies mission::kItemIdOffset for database lookup.
	// Lighting keys from hit PRESENCE, independently of the aggregate "indoors" flag bit.
	int local_player_interior_item_id() const;

	// The blink-box owner for a model-light spawn at a world point: retail runs
	// ONE point query at the spawning entity's position before walking its LGHT
	// records, and slot 0's packed hit names the containing building + section
	// every unattached record binds to [orig: Entity_SpawnGlowEffects
	// @0x56c7fc -> Entity_QueryBlinkBoxesAtPoint @0x4af350, decoded @0x56c8c9
	// and @0x56c8db, see docs/render/render-lighting-re.md]. Returns
	// [containing bms_id, section], or an empty array when the point sits in no
	// blink volume (or the containing entity carries no bms identity). The
	// caller applies retail's ItemDef-type gate: a BUILDING never runs the
	// query at all [orig: @0x56c7ec].
	PackedInt64Array query_blink_owner_at(const Vector3 &p_world);

	// The per-drawn-entity interior light group: every placed entity currently
	// standing inside a blink volume, as [bms_id, containing bms_id, section]
	// triples. Retail pushes this pair per entity draw so an interior room
	// light reaches exactly the entities in its own section [orig:
	// setup_terrain_effect_for_entity @0x5c74a0 -> Lighting_SetInteriorLightGroup
	// @0x5a90e0, the gate Light_PassesActiveGroups @0x5a9120, see
	// docs/render/render-lighting-re.md]. Entities outside every blink volume
	// are absent (their group is (0, 0)).
	PackedInt64Array get_entity_interior_groups() const;

	// The local player's interior light group: [containing bms_id, section], or
	// an empty array outdoors. The local player is a spawned entity with no
	// bms_id, so it is absent from get_entity_interior_groups; its group is what
	// scopes interior lights onto the first-person arms and weapon.
	PackedInt64Array local_player_interior_group() const;

	// Sound-occlusion distance inflation for the audio host [orig:
	// Sound_ApplyOcclusionDistance @0x529970 — two LOS rays through terrain +
	// building solids; occluded sources sound farther]. Positions in Godot
	// world space; distance in/out 16.16.
	int64_t sound_occlusion_distance_q16(const Vector3 &listener_pos,
	                                     const Vector3 &source_pos, int64_t distance_q16,
	                                     int source_bms_id = 0);
	// Authored bms id -> registry handle, rebuilt on the registry's spawn
	// serial (retail's slot carries the entity pointer from registration;
	// this is the lookup that identity stands in for).
	opennova::world::EntityHandle handle_for_bms_id(int p_bms_id) const;

	// The marched iris-exposure sampling (D-RLIT-2): three classification codes
	// for WeatherCore.set_exposure_from_iris_samples — the camera ray runs
	// 8 units forward, clips against terrain, and samples at the end point and
	// two points marched back toward the camera in thirds. Per sample: a blink
	// hit classifies indoor (-1; -2 when the building carries no interior
	// data), else the outdoor sun level 8 minus one per blocked sun-occlusion
	// ray (three entity-only rays, 200 u toward the light, clip radii
	// -0x2000/-0x5000/-0x8000). Positions/directions in Godot world space.
	// Empty when no world/local player is loaded (the caller falls back to the
	// outdoor sample). All three samples reuse the local player's fixed
	// proximity-candidate slice for blink classification, the nonzero-count sun
	// gate, and both pool-1/pool-2 sun blockers.
	// [orig: compute_ambient_light_along_direction @ 0x5c7a00;
	//  terrain_sector_compute_lighting @ 0x5c7550; raycast_entity_collision @ 0x413760]
	PackedInt32Array compute_iris_samples(const Vector3 &cam_pos, const Vector3 &cam_forward,
	                                      const Vector3 &light_dir);

	// Loadout-zone gates for the host's armory key [orig: input action 218 opens
	// weapon.mnu WEAPON only while entity Flags & 0x400000 (a type-6 armory volume
	// contact), vehicle.mnu VEHICLE on Flags & 0x800 (type-11);
	// Input_HandleActionBinding @0x49b848/@0x49b858].
	bool local_player_in_armory_zone() const;

	// The USE-ITEM mount toggle: weapon-busy gate + the witnessed toggle
	// (deck best-seat / nearest-seat scan / seat-swap-or-detach). Returns true when a
	// mount, swap or dismount applied. [orig: Input_ProcessFrame @0x49d6dc ->
	// Entity_ToggleVehicleMount @0x436950]
	bool local_player_toggle_mount();

	// The floating attach labels around the local player, one Dictionary per label:
	// position (mission space, +0.1875 u lift applied), seat_type (world::SeatType,
	// 4 = armory point), armory (bool), nearest (bool, the full-bright highlight),
	// attach_text_key (the USEGUN weapon's attachtextid Overlays key, "" = absent ->
	// the STROVER_USEGUN default). Armory mode rides the zone flag; the nearest-only
	// gate consumes the same complete live fire verdict as body/HUD selection.
	// [orig: draw_vehicle_seat_and_armory_labels @0x5a3290 selection half;
	//  Player_CanFireWeapon @0x5cf780]
	TypedArray<Dictionary> get_attach_labels() const;
	TypedArray<Dictionary> get_friendly_tags() const;

	// Parse weapon.def from the resource root and install the armory table on the sim world
	// (world::World::weapons) — the server-side source for the 0x2F/0x5A loadout service, the
	// extended-uplink equipped-weapon gate, and the player-spawn WPN_M4AUTO default
	// (D-NET-141/143). [orig: Game_StartMission @0x5254bd -> WeaponDefs_LoadFile @0x5450A0,
	// right after AnimDef_InitAll @0x5254b3]. Idempotent; call after load.
	Error load_weapon_table(const Ref<class ResourceRoot> &p_resource_root,
	                        const String &p_name = "weapon.def");

	// Parse score.ini and install this session's scoring awards (world::World::score_rules).
	// Retail builds 12 x 452-byte gametype rows with hardcoded defaults and then OVERLAYS
	// the file onto them, writing the file out when it is absent
	// [orig: GameType_CreateDefaultSettings @0x52DD00 -> ScoreConfig_LoadFile @0x52D8A0].
	// DECLARED GAP: the built-in defaults are NOT ported, so a missing score.ini leaves
	// score_rules !valid (every award a no-op) where retail would still score from its
	// defaults. The shipped file is the retail-parity path.
	// Order-independent with the mission load: whichever of the two lands second
	// re-resolves the row (see refresh_score_rules).
	Error load_score_config(const Ref<class ResourceRoot> &p_resource_root,
	                        const String &p_name = "score.ini");

	// Parse ammo.def and install the ballistics/damage table (world::World::ammo), then
	// resolve every armory entry's round_type to its ammo index — the authoritative round
	// sim's data feed (§5.60). Call after load_weapon_table.
	// [orig: Game_StartMission @0x52548a -> AmmoDef_LoadAll @0x40b0b0]
	Error load_ammo_table(const Ref<class ResourceRoot> &p_resource_root,
	                      const String &p_name = "ammo.def");

	int get_spawned_count() const { return kernel_->promo.spawned; }
	int get_brain_count() const { return kernel_->promo.brains; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::Simulation::PresentField);
VARIANT_ENUM_CAST(godot::Simulation::EffectStateField);
VARIANT_ENUM_CAST(godot::Simulation::SeatCode);
VARIANT_ENUM_CAST(godot::Simulation::MountCommand);
VARIANT_ENUM_CAST(godot::Simulation::JoinTerrainTilState);
VARIANT_ENUM_CAST(godot::Simulation::SessionRole);
