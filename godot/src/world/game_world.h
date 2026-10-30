#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

#include <runtime/inmatch/mission_rotation.h> // HostRotation (the process's rotation words)
#include <runtime/inmatch/client_runtime.h> // ClientProcessState (the process's client words)
#include <runtime/inmatch/main_loop_clock.h> // MainLoopClock (the frame's timing)
#include <runtime/inmatch/remote_admin.h> // RemoteAdminConsoles (the remote admin)
#include <runtime/inmatch/server_log_devices.h> // ServerLogDevices (the server log switches)
#include <runtime/mission/mission_load.h> // MissionLoadLegs (the load plan's legs)

#include <memory>
#include <runtime/renderer/object_lod.h> // the object detail rungs
#include <runtime/renderer/particle_density.h> // the particle_density rungs
#include <runtime/renderer/device_texture.h> // the terrain_texdetail word
#include <runtime/renderer/texture_compression.h> // the texcompression_level word
#include <runtime/renderer/texture_filter.h> // the texfilter rungs

#include "network/admin_tcp_pump.h"
#include "audio/mission_audio.h"
#include "devtools/frame_stats.h"
#include "env/celestial.h"
#include "env/environment_cube_capture.h"
#include "env/mission_environment.h"
#include "env/precipitation.h"
#include "env/sky_dome.h"
#include "env/slot_shadow.h"
#include "env/sun_shadow.h"
#include "env/water.h"
#include "env/weather.h"
#include "hud/player_hud_weapon_def.h"
#include "lights/effect_light_director.h"
#include "lights/effect_light_report.h"
#include "mission/mission_data.h"
#include "mission/mission_object_placer.h"
#include "mission/mission_placement_stats.h"
#include "mission/mission_root.h"
#include "mission/mission_setup_options.h"
#include "mission/static_source_provider.h"
#include "network/host_session_options.h"
#include "network/join_target.h"
#include "object/character_join_profile.h"
#include "render/scene_overlay_compositor.h"
#include "object/item_database.h"
#include "object/env_light_state.h"
#include "object/object_model.h"
#include "object/weapon_database.h"
#include "particle/effect_world.h"
#include "player/first_person_arms_witness.h"
#include "player/game_config_block.h"
#include "player/local_player_presenter.h"
#include "player/local_player_visuals.h"
#include "player/player_spawn_loadout.h"
#include "player/player_viewmodel_def.h"
#include "render/frame_fx.h"
#include "resource_index/resource_root.h"
#include "simulation/corpse_ragdoll_presenter.h"
#include "simulation/inmatch_session_values.h"
#include "simulation/player_local_view.h"
#include "simulation/player_weapon_event.h"
#include "simulation/player_weapon_view.h"
#include "simulation/present_event_records.h"
#include "simulation/present_stats.h"
#include "simulation/simulation.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/terrain.h"
#include "terrain/terrain_data.h"
#include "terrain/terrain_tile_info.h"
#include "world/item_effect_director.h"
#include "world/load_timeline.h"
#include "world/loading_screen_info.h"
#include "world/occlusion_frame.h"
#include "world/resource_root_resolver.h"
#include "world/runtime_perf_counters.h"
#include "world/session_drive.h"
#include "world/world_scene.h"
#include "world/world_view.h"

namespace godot {

class HudInsetScope;
class HudOverlay;

// Loads a playable world (terrain + environment + vegetation + foliage) from
// ONE resource root and wires it onto the engine nodes it contains (Terrain,
// MissionEnvironment, Water). The data core is shared engine code
// (TerrainData, EnvFile, FoliageDispatcher + its vegetation asset resolver); this node is just
// runtime orchestration -- one loader, one root, no fallbacks. The scene lives
// in game_world.tscn so the game shell and focused engine tests can instance
// it; production play mounts the selected runtime resource directory. The
// former game_world.gd + game_frame_pipeline.gd + world_device_frame.gd +
// world_load_stages.gd + world_effect_router.gd + net_session_drive.gd, one
// C++ Node with real ownership (ADR 0043 decision 9, slice G10).
//
// EMBEDDER CONTRACT: a shell or focused test instances game_world.tscn,
// optionally injects a root, calls one load_* entry, then
//   * drives tick(camera_position) once per frame while playing (foliage ->
//     runtime logic+present -> audio, in that order; pausing = not ticking),
//   * provides the camera that position comes from,
//   * consumes mission_effects (HUD text / win / waypoints / dialog routing),
//   * calls unload() to tear the played world down (placed objects, runtime,
//     audio, env overrides) before loading another mission or leaving.
//
// THE FRAME: the witnessed per-frame order (the original main loop's
// server-tick-then-client-render) is ONE static leg table (kFrameLegs, the
// witness citations on the leg bodies in game_world_frame.cpp) run by one loop
// with one RAII LegScope timing; the frozen-pose capture replay is a second
// table (kFrozenPoseRefresh) through the same loop. inmatch::Session decides
// lifecycle, cadence, input consumption, catch-up, and cancellation; the
// table orders the actual Godot devices around that one typed call. There is
// deliberately no generic renderer interface: every leg is a direct method
// over the one renderer we ship.
class GameWorld : public Node3D, private StaticSourceProvider {
	GDCLASS(GameWorld, Node3D)

	friend class SessionDrive;

public:
	// The per-frame carrier the leg table threads: the one sampled input and
	// the session outcome the runtime leg produced (null on a frame with no
	// playing runtime).
	struct FrameContext {
		Ref<MissionFrameInput> input;
		Ref<MissionFrameOutcome> outcome;
		// The render gate's verdict (leg_scene_gate, engine inmatch::Session::renders): false
		// while a joiner's mission start is in its final wait for the S2C 0x0F, when no kSceneLeg
		// row runs (scene_drawn is false with it) and the audio row runs its drains alone, its
		// listener and emitter mix (the render callback's head) standing down.
		bool rendered = true;
		// The scene gate's verdict (leg_scene_gate): false while the status page stands in for
		// the scene frame, when no kSceneLeg row runs.
		bool scene_drawn = true;
		// The drawn scene frame skips its world (engine inmatch::FrameOutcome::world_skipped):
		// no kWorldLeg row runs.
		bool world_skipped = false;
		// One of the first frames past a mission load: its legs nest as spans under the frame's
		// span of the post-load timeline (an instrument; null on every other frame).
		LoadTimeline *post_load_frame = nullptr;
	};
	enum FrameLegFlags : uint32_t {
		kLegNone = 0,
		// A kLegStop return from this leg STOPS the frame: the later legs (the
		// finish included) do not run.
		kStopsFrame = 1u << 0,
		// The row draws the scene (retail's Render_ProcessMainSceneFrame): it does not run while
		// the status page stands in for the scene frame (engine hud_server_status.h
		// main_frame_draws_scene carries the witness).
		kSceneLeg = 1u << 1,
		// The row is the scene frame's world block, whose state legs a frame that skips its
		// world (a cine playing, the command map over the menu) does not run: the terrain
		// frame's composes, the water pass, the collect, the foliage and the sun veil's
		// stop-down target (engine inmatch::FrameOutcome::world_skipped carries the witness).
		kWorldLeg = 1u << 2,
	};
	// What a leg body reports back to the loop: it ran (its LegScope banks
	// the span), it did not run this frame (nothing is banked -- a skipped
	// leg never dilutes its slot's sample count), or it ran and the frame
	// must stop here (a kStopsFrame row only).
	enum LegResult {
		kLegRan,
		kLegSkipped,
		kLegStop,
	};
	// One row of the leg table: the literal name (frame_leg_names), the
	// FrameStats slot its LegScope banks (-1 = untimed), the body, the flags.
	struct FrameLeg {
		const char *name;
		int slot;
		LegResult (GameWorld::*run)(FrameContext &);
		uint32_t flags;
	};

	GameWorld();

	// --- the injection seams ---------------------------------------------------
	// Inject the resource root the next load resolves through. Runtime hosts
	// and focused tests use this to keep one already-mounted resource session.
	// Null returns to the game's settings-driven mount.
	void set_resource_root(const Ref<ResourceRoot> &p_root) { injected_root_ = p_root; }
	// The shell's persisted resource settings (ResourceRootResolver): the
	// game path (no injection) mounts from them; the joiner's expansion
	// remount reads its game code through them.
	void set_resource_root_resolver(const Ref<ResourceRootResolver> &p_resolver) {
		root_resolver_ = p_resolver;
	}
	// Configure the local player's staged PLAYER_INFO selection for the next
	// mission runtime start (PlayerSpawnLoadout; the shell decodes its profile
	// snapshot through PlayerSpawnLoadout.from_profile). The record is
	// consumed once the runtime exists (or discarded by unload after a
	// failed/abandoned load). Null preserves the historical fallback; a record
	// carrying empty slot names explicitly requests an all-NONE kit.
	void set_local_player_spawn_loadout(const Ref<PlayerSpawnLoadout> &p_loadout);
	void set_playable(bool p_enabled) { playable_ = p_enabled; }
	bool is_playable() const { return playable_; }
	// The object detail (engine: renderer/object_lod.h, game.cfg's
	// object_polydetail): the shell hands over its persisted options word
	// whenever it changes, and each mission start copies it into the session
	// detail the frames draw at, so a change made mid-mission reaches the draw
	// at the next mission. The fresh profile's word and the config load's
	// clamp are the engine's, re-exported for the shell's options owner.
	void set_object_polydetail(int p_level) { object_polydetail_ = p_level; }
	int get_object_polydetail() const { return object_polydetail_; }
	// The options' aspect mode (renderer/aspect_ratio.h): the shell hands it over
	// whenever it changes, and each mission start selects it for the local view
	// ahead of the start's loads (LocalPlayerPresenter::prepare_view_projection).
	void set_aspect_mode(int p_mode) { aspect_mode_ = p_mode; }
	int get_aspect_mode() const { return aspect_mode_; }
	int get_object_detail() const { return object_detail_; }
	static int object_detail_fresh_profile();
	static int clamp_object_detail(int p_level);
	// game.cfg's texfilter_level (engine: renderer/texture_filter.h): the shell
	// hands over its persisted options word whenever it changes. The model
	// effects' filter follows it at once (retail reloads every effect at the
	// Options' Accept); each mission start copies it into the session level
	// whose device mode (level + 1) the terrain detail family samples at, so
	// that half reaches the draw at the next mission.
	void set_texfilter_level(int p_level);
	int get_texfilter_level() const { return texfilter_level_; }
	int get_session_texfilter_level() const { return session_texfilter_level_; }
	// The state the world publishes: the device mode (session level + 1) and
	// the effects' mode (from the options word), and the shader filter codes
	// the device leg last published for the terrain detail family and the
	// model stages (render/texture_filter_device).
	int get_texfilter_device_mode() const;
	int get_texfilter_effect_mode() const;
	static int get_texfilter_device_filter();
	static int get_texfilter_effect_filter();
	static int texfilter_level_fresh_profile();
	static int clamp_texfilter_level(int p_level);
	// game.cfg's texcompression_level (engine: renderer/texture_compression.h)
	// and particle_density (renderer/particle_density.h): the shell hands over
	// its persisted options words whenever they change, and each mission start
	// copies them into the session's words, which the mission builds its
	// compressed textures and its particle pages with and the scene passes
	// draw their particle stride from, so a change reaches the draw at the
	// next mission (D-RMAT-24).
	void set_texcompression_level(int p_level) { texcompression_level_ = p_level; }
	int get_texcompression_level() const { return texcompression_level_; }
	int get_session_texcompression_level() const { return session_texcompression_level_; }
	// game.cfg's terrain_texdetail (0..3; engine: renderer/device_texture.h, which cites the
	// session copy), handed over and copied at each mission start as texcompression_level is:
	// the terrain's detail layers this mission builds are halved by the session's copy.
	void set_terrain_texdetail(int p_level) { terrain_texdetail_ = p_level; }
	int get_terrain_texdetail() const { return terrain_texdetail_; }
	int get_session_terrain_texdetail() const { return session_terrain_texdetail_; }
	static int terrain_texdetail_fresh_profile();
	// game.cfg's object_texdetail (0..3; engine: renderer/device_texture.h), handed over and copied
	// at each mission start as terrain_texdetail is: the model textures this mission loads are
	// halved by the session's copy (the resource root's set_object_texdetail).
	void set_object_texdetail(int p_level) { object_texdetail_ = p_level; }
	int get_object_texdetail() const { return object_texdetail_; }
	int get_session_object_texdetail() const { return session_object_texdetail_; }
	static int object_texdetail_fresh_profile();
	void set_particle_density(int p_density);
	int get_particle_density() const { return particle_density_; }
	int get_session_particle_density() const { return session_particle_density_; }
	// The session's copy of game.cfg NoBlood's ragdoll bit (ADR 0060), and a
	// dev override of it for the running session; the census probes read.
	bool is_session_ragdolls() const { return ragdolls_.enabled(); }
	void set_session_ragdolls(bool p_enabled) { ragdolls_.set_enabled(p_enabled); }
	// [awake, sleeping, blending back, started since the session began].
	PackedInt32Array get_ragdoll_counts() const;
	static int texcompression_level_fresh_profile();
	static int particle_density_fresh_profile();
	static int clamp_particle_density(int p_density);

	// --- the load entries ------------------------------------------------------
	// Load a mission (.bms): its header selects the terrain + environment,
	// which are resolved from `dir` (or the persisted resource directory, the
	// only place looked), then the mission's placed objects are populated into
	// the world. Returns OK, or ERR_FILE_NOT_FOUND when the directory is
	// unset or misses a file (the caller decides whether to prompt).
	int load_mission(const String &p_bms_name, const String &p_dir = String());
	// Load the exact saved, top-level loose BMS from the selected resource
	// root. This tooling/test seam deliberately differs from load_mission(),
	// whose retail contract remains archive-only even when the session has
	// /d. Only the BMS itself is forced to disk; terrain, environment,
	// objects and sidecars continue through the mounted runtime root and its
	// normal /d policy.
	int load_loose_mission(const String &p_bms_name, const String &p_dir = String());
	// Load a mission as a LAN co-op HOST: the typed session request (ADR
	// 0017, the sim-shaped HostSessionOptions the host screen's config
	// projects) is staged and driven by the session drive through the same
	// load path as load_mission. Returns the same codes as load_mission.
	int load_mission_as_host(const Ref<HostSessionOptions> &p_options);
	// Load as a LAN co-op JOINER (a non-authority client): the typed dial
	// target is staged and driven by the session drive (authenticate before
	// the wire-header world load; S2C 0x7B supplies the mission identity).
	// Returns the same codes as load_mission.
	int load_mission_as_joiner(const Ref<JoinTarget> &p_target);
	// Switch the mounted root to a join's host expansion BEFORE the dial, so the
	// profile the join vars come from is the host expansion's
	// (SessionDrive::switch_join_expansion). Returns the failure text, empty on
	// success; the post-auth reconcile remains the authoritative check.
	String mount_join_expansion(const String &p_expansion);
	// The return to the main menu's restore of the expansion a join's switch left
	// (SessionDrive::restore_kept_expansion; D-NET-622): the shell runs it ahead of the
	// menu's entry over its live mount.
	void restore_join_expansion(const Ref<ResourceRoot> &p_root);
	// The NovaWorld session (a NovaWorldClient), handed over by the shell with
	// a NovaWorld join or host: the world keeps it playing or hosting through
	// the match (SessionDrive::adopt_nw_client). A normal exit back to the
	// NovaWorld menu takes it back out, still connected (null when none);
	// otherwise it goes down with the world.
	void adopt_novaworld_client(Node *p_client);
	Node *release_novaworld_client();
	// The post-mission route for an exit reason (SessionDrive::post_mission_route): the
	// shell's Post Menu router, called once where it routes the session's final exit to the
	// menus, so its destroy arm also destroys the session's process globals (the remote
	// admin's game type, inmatch::RemoteAdminConsoles::route_mission_exit).
	Ref<PostMissionRoute> post_mission_route(int p_reason);
	// The mission's session closes ahead of the Post Menu router, which reads the exit the
	// teardown left: the own client's teardown can remap it through the process latch (a stale
	// kick turns a listen host's quit into 7 or 9..19, D-NET-823). Returns that exit, the
	// requested one when the session stored none (or there is none). The witness is the
	// engine's (inmatch/host_role.cpp HostRole::close, inmatch/mission_exit.h).
	int close_session_for_route(int p_requested_reason);
	// The pre-game menu's cancel (its ABORT_CANCEL, the join choice's BACK): the join
	// machine's cleanup destroys the session (RemoteAdminConsoles::session_destroyed).
	void pre_game_cancelled();
	// The PLAYER_INFO screen's portrait drew a frame: its 16:00 becomes the process's time
	// of day (RemoteAdminConsoles::player_preview_drawn).
	void player_preview_drawn();
	// What the main frame does with a stored exit reason over the loaded sim's
	// session facts (inmatch::main_frame_exit): a MAIN_FRAME_EXIT_* value.
	int main_frame_exit(int p_reason) const;
	// The map change (D-NET-331, inmatch/map_change.h), each keeping the live
	// session for the next load (SessionDrive::keep_session):
	// begin_map_change runs an in-session host's teardown arms and router and
	// returns the next map's file (empty: the rotation ended, nothing kept);
	// load_next_mission is its load. begin_joiner_reload starts a joiner's
	// reload legs on its kept connection; reload_joiner is its load.
	String begin_map_change();
	// The map change's loading screen, raised ahead of its load: the next map's band and server
	// message as its session will serialize them, over `p_kept`, the kept session's record.
	Ref<LoadingScreenInfo> map_change_loading_info(const Ref<HostSessionOptions> &p_kept) const;
	int load_next_mission(const String &p_bms_name);
	bool begin_joiner_reload();
	int reload_joiner();
	// The error record of the join that last failed (SessionDrive::last_connection_error).
	Ref<ConnectionError> get_last_connection_error() const;
	// The join screen's status while a join preload runs (SessionDrive::join_screen_status).
	Ref<JoinScreenStatus> get_join_screen_status() const;
	// Load an in-memory mission through the shared world pipeline. This is
	// retained as a focused engine-test/tool seam; normal game launches always
	// use a saved .bms through load_mission() or load_loose_mission().
	int load_mission_data(const Ref<MissionData> &p_mission, const String &p_bms_name,
			const String &p_dir = String());
	// ESC/abort for the joiner's pre-load connect/session wait. Returns true
	// when an in-flight preload was aborted (see SessionDrive::cancel_preload).
	bool cancel_join_preload();
	// ESC in the joiner's final wait for the S2C 0x0F: it leaves the wait
	// for the game loop and aborts nothing. Returns true when the final wait
	// took it (see SessionDrive::cancel_admission_wait).
	bool cancel_join_admission();
	// True while this world is a live network session (a co-op JOINER or a
	// LISTEN HOST). The shell uses it to keep the world ticking through the
	// in-game menu: the world tick is the only pump for the session socket,
	// so freezing it silences the connection and a peer eventually drops us
	// on its connection timeout. Retail multiplayer cannot pause at all -- the
	// ESC menu overlays a running match [orig: the pause path has no MP leg;
	// the reaping side is cs_dir0.timeout_ms = 120000, CNapiNetwork_Init
	// @0x4ca4a0].
	bool is_net_session() const;
	// The shell's ESC-pause session leg: the engine session pauses and resumes
	// WITH the shell's pause overlay, so the paused state is a session fact
	// every reader agrees on, not a shell-only tick skip. No role gate here --
	// the runtime reports the engine's own verdict (inmatch::Session::pause is
	// SinglePlayer-only: retail multiplayer cannot pause, its ESC menu overlays
	// a running match, and the world tick is the net session's only socket
	// pump). Resuming an unpaused session is a NoOp, so every shell resume leg
	// (ESC, armory close, menu RESUME, the MCP resume verb) can call this
	// safely.
	void set_shell_paused(bool p_paused);
	// The in-game options screen's audio shutdown on its open out of a session,
	// run on the device MissionAudio::shutdown_audio_channels (engine
	// WorldOutbox::audio_channels_shutdown carries the witness).
	void shutdown_audio_channels();
	// The missing-terrain/env reason, join-aware: a wire-header join has no
	// local .bms -- the host streamed the mission identity -- so the reason
	// names the stream and what is mounted/installed, making a live punt read
	// as "your install lacks X" rather than a bad local file.
	String missing_mission_asset_reason(const String &p_asset, const String &p_bms_name,
			const Ref<ResourceRoot> &p_resource_root, bool p_wire_header_join) const;
	// Tear down a loaded world so the shell can return to the menu (or load a
	// different mission) without the previous world lingering. Frees the
	// dynamically placed MissionObjects subtree and resets the load state; the
	// terrain / environment scene nodes are kept in place and rebuilt by the
	// next load_*(). Safe to call when nothing is loaded.
	void unload();
	// Complete the wire-only part of a retail join once the protocol reaches
	// its deployment/admission boundary. The session drive calls this before
	// revealing the world (or deploy map), guaranteeing the optional 0x45
	// overlay and the renderer-backed C2S 0x3D snapshot reflect the completed
	// initial stream.
	bool settle_join_wire_assets();
	// The revealed world must never race the budgeted cold wire
	// materialization: the session drive holds the join-admission edge until
	// the wire presenter's deferred-spawn queue drains behind the loading/DEATH
	// hold. Trivially true with no runtime or no wire presenter.
	bool is_join_wire_present_drained() const;
	// Fail the streamed-asset leg once per join. Both the per-frame runtime
	// driver and the frame-polled admission observer can observe the same
	// protocol edge; routing them through one latch prevents duplicate
	// load_failed emissions.
	void report_join_wire_asset_failure(const String &p_reason);
	// Build retail's depthspin shore mask directly from the raw CPT height
	// atlas. Streamed .til art does not participate in either the sharp
	// colormap base or this independent water pass.
	void build_minimap_water_mask();

	// --- the occlusion consumer ------------------------------------------------
	// The occlusion consumer as a live device switch (the F3/MCP
	// "occlusion_culling" row): it reads and writes the world viewport
	// directly, never on a viewport without a RenderingDevice (headless and
	// Compatibility expose no occlusion path), and the next mission load
	// re-applies the policy default (a fresh mission gets fresh debug state;
	// nothing replays).
	void set_occlusion_culling_enabled(bool p_enabled);
	bool is_occlusion_culling_enabled() const;
	// How many placed buildings carry authored OOBJ occluders in the loaded
	// mission (0 = the occluder pass has nothing to cull with).
	int get_authored_occluder_model_count() const;

	// --- the read seams --------------------------------------------------------
	Ref<MissionData> get_loaded_mission() const { return loaded_mission_; }
	String get_loaded_mission_file() const { return loaded_mission_file_; }
	Ref<Simulation> get_sim() const;
	// The mounted world's shared weapon.def database. ArmoryPresenter consumes
	// this on first open so its canonical parent tuples and its visible rows
	// resolve against the same catalog; the FP viewmodel reuses it (ADR 0018
	// resource seam).
	Ref<WeaponDatabase> get_weapon_database();
	// The timing of the most recent mission load (null before the first load).
	Ref<LoadTimeline> last_load_timeline() const { return last_load_timeline_; }
	// The first frames past the most recent mission load (null before the first load): a span per
	// frame, the legs that ran nested in it, the session's updates counted and, on a joiner, each
	// client net pass the frame ran (its loop, its send countdown, an open send block, a C2S 0x0C),
	// the time between two frame spans being the device draw and the shell's own process (an
	// instrument, printed under --verbose once its frames are in: post_load_frames_continue).
	Ref<LoadTimeline> last_post_load_frames() const { return post_load_frames_; }
	// Whether the post-load timeline still times frames (the slow-frame readout prints every frame's
	// slots while it does).
	bool is_post_load_frames_open() const { return post_load_open_; }
	// Whether the post-load timeline times another frame past `p_frames_timed`: kPostLoadFrames at
	// least; past them only while a joiner awaits its first C2S 0x0C past the final wait (the S2C
	// 0x0F's first-send window, the third pong's), and never past kPostLoadFramesMax. The caller
	// passes the wait only under --verbose, whose PostLoadFrame lines read those frames.
	static bool post_load_frames_continue(int p_frames_timed, bool p_awaits_uplink);
	MissionRoot *get_runtime() const;
	Ref<MissionPlacementStats> get_mission_stats() const { return mission_stats_; }
	// The placer's static populations that currently carry at least one live
	// row (the per-frame RLOD selection empties and refills them); 0 before a
	// mission is placed.
	int get_static_live_population_count() const;
	// Process-exit-only release for renderer resources intentionally retained
	// by unload() so world-to-menu and mission-to-mission transitions stay
	// warm.
	void release_runtime_renderer_resources();
	// Runtime water exposes a render-aware predicate so its retained authored
	// height cannot leak into frame clear/occlusion while a load is absent or
	// in progress.
	bool is_water_render_active() const;
	// The waterline the scene's eye side and transparent split compare against,
	// a zero height included (Water::is_waterline_active).
	bool is_waterline_active() const;
	Ref<TerrainData> get_terrain_data() const { return terrain_data_; }
	Ref<ResourceRoot> get_resource_root() const { return resource_root_; }
	// The world's FrameFX node (the shell's SIGHTS card feeds the NVG Sighted
	// arm through it); null before the scene is wired.
	FrameFx *get_frame_fx() const { return framefx_; }
	// The narrow live view of this world (its sim and resource root,
	// re-resolved per call) the in-world screens and the click picker depend
	// on.
	Ref<WorldView> world_view();
	// The armory screen's live view (WorldView plus the weapon table and the
	// local player's viewmodel verbs).
	Ref<ArmoryWorldView> armory_view();
	bool is_loaded() const { return world_ready_; }
	// The loading presentation's hold, the shell's from a mission start to its reveal: while it holds,
	// the world's viewport draws no 3D pass and the water no mirror pass, so a frame under the loading
	// screen (a joiner's wait for its admission, the start-mission splash) draws the screen alone:
	// retail's presents draw the screen alone, and its joiner's final wait inside Game_StartMission
	// pumps the window and the session and draws no frame, its 100 % present standing. The world's
	// nodes stay shown from the load's forced frame on, so the reveal drops a viewport flag and
	// re-pairs no instance (D-NET-1378).
	void set_presentation_held(bool p_held);
	bool is_presentation_held() const { return presentation_held_; }
	Color get_current_frame_clear_color() const;

	// --- the frame ------------------------------------------------------------
	// The game shell hands its FrameStats here; the world re-hands it to every
	// MissionRoot it creates and feeds its own tick legs.
	void set_music_director(MusicDirector *director);
	MusicDirector *get_music_director() const;
	// The shell's player profile, whose current records every mission's sim
	// takes (Simulation.use_player_profile). Null = a fresh profile's defaults.
	void set_player_profiles(const Ref<PlayerProfiles> &p_profiles) { player_profiles_ = p_profiles; }
	// The shell's process-lifetime cfg block (game.cfg): every mission's sim
	// reads its mpreset word at the session create, and a NovaWorld
	// ServerCommand's config change lands in it and is saved. Null = none.
	void set_game_config(const Ref<GameConfigBlock> &p_block) { game_config_ = p_block; }
	// The shell's live binding table (ControlsBindings' model): every session's
	// Simulation takes it for the console's SAVEKEYS (Simulation::set_controls_model).
	void set_controls_model(const Ref<ControlsModel> &p_model) { controls_model_ = p_model; }
	// The shell's in-game menu screen state (true while one is up), read live by the remote
	// admin's GOTO MENUSTATE (AdminConsole::Seams::menu_screen_open; engine inmatch/remote_admin.h
	// ExitSite::AdminOverMenuScreen carries the witness). Unset: none up.
	void set_menu_screen_probe(const Callable &p_probe) { menu_screen_probe_ = p_probe; }
	// The next load is the single player's restart (Game_RestartRoundSP's Game_StartMission(1)):
	// its sim boots with the engine's restart flag (KernelBootOptions::restart). The load
	// consumes it.
	void set_restart_boot(bool p_restart) { restart_boot_ = p_restart; }
	Ref<GameConfigBlock> get_game_config() const { return game_config_; }
	void set_frame_stats(const Ref<FrameStats> &p_board);
	bool is_water_render_stats_measured() const;
	// Enables the manual frame-span/A-B probe. Disabling restores every skip
	// request to its retail default.
	void set_perf_probe_enabled(bool p_enabled);
	// The perf probe's A/B switches (only honored while the probe is enabled).
	void set_perf_probe_skip_occlusion(bool p_skip) { perf_probe_skip_occl_ = p_skip; }
	void set_perf_probe_skip_effect_tick(bool p_skip) { perf_probe_skip_effect_tick_ = p_skip; }
	void set_perf_probe_skip_fixed_handlers(bool p_skip) { perf_probe_skip_fixed_handlers_ = p_skip; }
	// The handoff from the local-player presenter (its setup binds, its
	// teardown releases): the local-view device leg and the fixed-tick weapon
	// drain reach the presenter through this seam.
	void set_local_view_presenter(LocalPlayerPresenter *p_presenter);
	// The presenter the local-view leg and the fixed-tick weapon drain reach
	// (null in worlds without one).
	LocalPlayerPresenter *local_view_presenter() const;
	// The handoff from the HUD (its scope build binds, its teardown releases):
	// the weapon Inset scope the local-view leg refreshes over the view it
	// just composed, so every Inset leg of the frame reads this frame's
	// camera (present_local_view_frame).
	void set_inset_scope(HudInsetScope *p_scope);
	Ref<EffectLightReport> get_effect_light_report() const;
	// The frame's light director (its scene's published static-row atlas is
	// what a static row draws with).
	Ref<EffectLightDirector> get_effect_light_director() const { return light_director_; }
	// The Godot per-frame order, faithful to the original main loop's
	// server-tick-then-client-render: the leg table, once per display frame.
	void tick(const Vector3 &p_camera_pos, const Transform3D &p_camera_xform = Transform3D(),
			double p_delta = -1.0, const Ref<MissionFrameInput> &p_frame_input = Ref<MissionFrameInput>());
	// The frame driver (the ex GameFramePipeline.advance): the input prep,
	// then kFrameLegs through the one loop. C++ callers get the outcome.
	Ref<MissionFrameOutcome> advance_frame(const Vector3 &p_camera_pos,
			const Transform3D &p_camera_xform, double p_delta,
			const Ref<MissionFrameInput> &p_input);
	// Main-thread display clock, shared by every world and menu: the process's one
	// clock (engine world::frame_tick_count_ms, GetTickCount's DWORD), which every
	// render-time reader retail has takes; the first reader samples it once per Engine
	// process frame, including menu-only frames, so the frame's readers agree.
	static int64_t current_frame_clock_ms();
	int64_t get_frame_clock_ms() const { return current_frame_clock_ms(); }
	// The same clock read now, unlatched: for a reader that runs outside the frame's
	// sample, the loading screen, which presents inside a blocking load.
	static int64_t tick_count_ms();
	// The leg table as literal names, in order (the frame-order pin).
	static PackedStringArray frame_leg_names();
	static PackedStringArray frozen_pose_leg_names();
	// Whether the named live leg stops the frame on a false return.
	static bool frame_leg_stops_frame(const String &p_name);
	// Whether the live table's row `p_name` draws the scene (kSceneLeg).
	static bool frame_leg_draws_scene(const String &p_name);
	// Whether the live table's row `p_name` is the world block's (kWorldLeg).
	static bool frame_leg_in_world(const String &p_name);
	// The live table's rows whose bodies the last frame ran, in table order (a structural seam:
	// the scene gate's skip is visible here).
	PackedStringArray get_last_frame_legs() const;
	// The last frame's scene gate verdict (FrameContext::scene_drawn): the shell's own scene
	// legs after the world frame (the death.mnu latch) run on it.
	bool get_last_frame_scene_drawn() const { return last_frame_scene_drawn_; }
	// The last frame drew its world: the scene frame, with its world-skip word down (a cine
	// playing or the command map over the menu skips the world block). The death.mnu latch's
	// open and close legs sit in that block.
	bool get_last_frame_world_drawn() const {
		return last_frame_scene_drawn_ && !last_frame_world_skipped_;
	}
	// The last frame's scene frame called its HUD pass (engine inmatch::FrameOutcome::
	// hud_pass_called): the HUD presenter's gameplay overlay pass compiles on it
	// (HudOverlay::set_overlay_pass_called). A frame the session did not advance leaves the
	// HUD's own gates (true).
	bool get_last_frame_hud_pass_called() const { return last_frame_hud_pass_called_; }
	// The last frame was one of the three drawn past a mission start, which present black
	// (engine inmatch::FrameOutcome::start_blank): the StartBlankCover layer showed over the
	// frame's every layer.
	bool get_last_frame_start_blank() const { return last_frame_start_blank_; }
	// Re-evaluates only camera-dependent production render state for an
	// exact-pose visual capture (the settle contract and the frozen-pose leg
	// order live with the replay table in game_world_frame.cpp).
	Error debug_refresh_render_pose(Camera3D *p_camera);
	Ref<RuntimePerfCounters> get_runtime_perf_counters() const;
	// Fire-presentation counters (FirePresentStats, typed per ADR 0017; null
	// until a mission runs).
	Ref<FirePresentStats> get_fire_present_stats() const;
	// Scar-presentation counters (ScarPresentStats, typed per ADR 0017).
	Ref<ScarPresentStats> get_scar_present_stats() const;

	// The leg bodies, each a public device operation over the one renderer
	// we ship (the table rows wrap them; a test drives one directly).
	void present_local_view_frame();
	void apply_scene_environment_frame();
	void render_environment_nodes_frame();
	void render_terrain_frame();
	void render_water_frame();
	void render_inset_water_frame();
	void render_foliage_frame();
	bool drive_network_frame();
	void apply_blink_frame();
	void apply_occlusion_frame();
	void sample_iris_frame();
	void render_sun_veil_frame();
	void render_light_frame();
	void render_material_frame();
	void sync_framefx_frame();
	void render_slot_shadow_frame();
	void render_particle_frame();
	void render_precipitation_frame();
	void plan_screen_effects_frame();
	void render_scene_overlay_frame();
	void append_celestial_overlays(SceneOverlaySubmission &r_submission);
	void append_nvg_laser_overlays(SceneOverlaySubmission &r_submission);
	void mix_audio_frame(int p_ticks_run, bool p_rendered);
	void update_clear_frame();
	void render_environment_cube_frame();
	void finish_device_frame();

	// --- local-player visuals (viewmodel / avatar / loadout) --------------------
	// The builder/apply/decode bodies live in LocalPlayerVisuals
	// (godot/src/player, ADR 0043 slice G8); these one-line delegates keep the
	// presenter/probe/test names on GameWorld (the component reaches the sim,
	// the runtime's placer, the resource root and the environment through
	// this node's public surface).
	Ref<LocalPlayerVisuals> local_player_visuals() const { return player_visuals_; }
	// The packed character id the authority stamped on the local player (the
	// host's own spawn from its installed profile, a joiner's named 0x0C
	// record) -- the one word its third-person body/head and first-person arms
	// key on, read from the sim rather than re-derived from team + profile
	// here.
	int local_player_character_id() const;
	bool set_local_player_weapon_by_name(const String &p_weapon_name, bool p_preserve_slot_state = false);
	void clear_local_player_weapon();
	// Whether the last first-person build found a model for the equipped
	// weapon (the sim's flag; a read seam for the viewmodel pins).
	bool local_player_first_person_model_available() const;
	// The joiner's pre-freeze model warm
	// (prewarm_loaded_model_challenge_definitions): resolves the present
	// snapshot's wire graphics, the player body and the current viewmodel
	// through the placer's cache and returns the graphics it resolved, in
	// order (empty before a mission is placed).
	PackedStringArray prewarm_challenge_models();
	Node3D *build_local_player_viewmodel();
	// The FP viewmodel's typed model parts (arms/gun), rebuilt with the
	// container -- the rig consumes this instead of scanning children.
	TypedArray<ObjectModel> local_player_viewmodel_parts() const;
	Ref<FirstPersonArmsWitness> local_player_first_person_arms_witness();
	String local_player_weapon_name() const;
	Ref<PlayerLocalView> local_player_view() const;
	// The equipped weapon's HUD slice (error table, HUDCLIPGFX/HUDRNDGFX,
	// clipsize, name), decoded from the resolved weapon.def row at this edge
	// (ADR 0017) -- the HUD reads it per frame, mirroring the original HUD info
	// struct's weapon-def pointer [orig: HUD_BuildEntityInfo @0x4b8561 ->
	// hudInfo+552]. Null until a weapon resolves.
	Ref<PlayerHudWeaponDef> local_player_hud_weapon_def() const;
	Ref<PlayerWeaponView> local_player_weapon_view() const;
	TypedArray<PlayerWeaponEvent> drain_local_player_weapon_events();
	Ref<PlayerViewmodelDef> local_player_viewmodel_def();

	// --- the dev tools' seams --------------------------------------------------
	// The dev tools' Particles seams (the existing get_effect_world() is the
	// data source; these are the two debug toggles). Delegates to the
	// item-effect director; the name stays on GameWorld for the typed
	// debug-control table (DebugControlTable) + probe calls.
	void set_particles_hidden(bool p_hidden);
	bool is_particles_hidden() const;
	// Hide foliage (the dev tools' "Hide foliage"). The dispatcher renders
	// scattered vegetation through retained scenario instances; its visibility
	// notification hides those instances without touching placement caches,
	// so re-showing is instant and the next dispatch is already current.
	void set_foliage_hidden(bool p_hidden);
	bool is_foliage_hidden() const { return foliage_hidden_; }
	// Mission-effect routing -- the WAC/BMS effect fan-out (dialog audio,
	// fx2ssn emitters); the runtime handlers and the tests drive it.
	void route_mission_effects(const Array &p_effects);
	Ref<ImageTexture> get_minimap_water_mask() const { return minimap_water_mask_; }
	EffectWorld *get_effect_world() const;
	// The per-item effect director (built once with the world; its
	// attach/anchor bookkeeping reads through ItemEffectDirector's public
	// seams).
	Ref<ItemEffectDirector> get_item_effect_director() const { return item_fx_; }
	// The live terrain node, for the F3 Terrain & foliage page's
	// counters/knobs.
	Terrain *get_terrain_node() const { return terrain_; }
	// The foliage dispatcher beside Terrain (null in a code-built world without
	// one): the vegetation asset caches live on it for the world's whole life
	// (the shell's exit empties them through clear_asset_cache).
	FoliageDispatcher *get_foliage_dispatcher() const { return dispatcher_; }
	// The meshes whose surfaces the overlay tail's cache holds (a read seam: the load's celestial
	// leg reads the sky bodies', D-NET-1341).
	int get_scene_overlay_cached_meshes() const { return scene_overlay_bodies_.cached_meshes(); }
	// The mounted item database (null before a mission), for the F3 snapshot
	// writer's display-name/graphic enrichment.
	Ref<ItemDatabase> get_item_db() const;
	// The live environment / weather / water nodes, for the F3 Environment
	// page's readouts and scrub knobs.
	MissionEnvironment *get_environment_node() const { return env_; }
	Weather *get_weather_node() const { return weather_; }
	// Hosted mission-clock knob used by F3 and runtime MCP. MissionEnvironment
	// owns the fixed-point clock; GameWorld coordinates the weather/audio
	// consumers so a paused scrub is an immediate visible state change rather
	// than just a readback value waiting for the next simulation tick.
	double get_debug_mission_minute_of_day() const;
	Error debug_set_mission_minute_of_day(double p_minute_of_day);
	Water *get_water_node() const { return water_; }
	Celestial *get_celestial_node() const { return celestial_; }
	SkyDome *get_sky_dome_node() const { return sky_dome_; }
	SunShadow *get_sun_shadow_node() const { return sun_shadow_; }
	// Whether this world advances the env presenters (weather, sun shadow,
	// sky, celestial, water) itself instead of their own _process.
	bool drives_environment_presenters() const { return env_presenters_world_driven_; }
	WorldEnvironment *get_clear_color_node() const { return clear_color_; }
	MissionAudio *get_mission_audio() const;

	// --- Godot ---------------------------------------------------------------------
	void _ready() override;
	void _process(double p_delta) override;

	// The bound signal targets.
	void on_runtime_effects(const Array &p_effects);
	void on_runtime_fixed_tick(int p_logic_tick);
	// RenderingServer frame_post_draw: stamps when the frame finished rendering on the main
	// loop's clock, the clock a mission-start frame re-bases to.
	void on_frame_post_draw();
	// Seconds since the last frame finished rendering on the main loop's clock, in its steps
	// (negative before the first). The shell's frame loop hands it to the session on each
	// frame (MissionFrameInput.since_render_seconds); a synthetic driver stepping tick() leaves
	// it unsampled and banks its own delta.
	double get_seconds_since_render() const;
	// The frame's elapsed wall time since this method's previous call, in whole
	// milliseconds of the process's monotonic clock (0 on the first call): what
	// the shell's frame loop hands the session as the frame's delta, as retail's
	// main loop banks GetTickCount's difference; Godot's process step is capped
	// (D-NET-1003). A synthetic driver stepping tick() banks its own delta.
	double take_frame_elapsed_seconds();
	void on_wire_node_spawned(ObjectModel *p_node, int p_kind, int p_item_id);
	void on_frame_stats_capture_changed(bool p_active);
	void on_nw_host_server_command(const String &p_verb, const String &p_target,
			const PackedStringArray &p_args);
	void on_nw_host_player_enter_result(int64_t p_connection_id, int p_success, int p_msg_code,
			const String &p_player_ticket, const String &p_access_code_list);

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	// --- the StaticSourceProvider the two directors read (the placer's static
	//     sources, resolved lazily: a placer exists only once a mission is
	//     placed) ---
	std::vector<opennova::mission::StaticEffectSource> static_item_effect_sources() override;
	std::vector<opennova::mission::StaticLightDrawSource> static_light_draw_sources() override;
	uint64_t static_light_draw_source_revision() override;
	Ref<ItemDatabase> static_source_item_db() override;
	Ref<ObjectData> static_source_object_data(uint64_t asset_id) const override;

	// --- the frame (game_world_frame.cpp) ---
	static const FrameLeg kFrameLegs[];
	static const int kFrameLegCount;
	static const FrameLeg kFrozenPoseRefresh[];
	static const int kFrozenPoseRefreshCount;
	// The one loop: every row through one LegScope over the frame's latched
	// board (null when the frame is untimed or the board is not capturing);
	// a kLegSkipped body cancels its scope, a kLegStop from a kStopsFrame
	// row ends the walk.
	void run_leg_table(const FrameLeg *p_legs, int p_count, FrameContext &r_ctx, bool p_timed);
	LegResult leg_begin(FrameContext &r_ctx);
	LegResult leg_session(FrameContext &r_ctx);
	LegResult leg_scene_gate(FrameContext &r_ctx);
	LegResult leg_script_voice(FrameContext &r_ctx);
	LegResult leg_local_view(FrameContext &r_ctx);
	LegResult leg_facial(FrameContext &r_ctx);
	LegResult leg_scene_environment(FrameContext &r_ctx);
	LegResult leg_environment_nodes(FrameContext &r_ctx);
	LegResult leg_terrain(FrameContext &r_ctx);
	LegResult leg_water(FrameContext &r_ctx);
	LegResult leg_foliage(FrameContext &r_ctx);
	LegResult leg_network(FrameContext &r_ctx);
	LegResult leg_blink(FrameContext &r_ctx);
	LegResult leg_occlusion(FrameContext &r_ctx);
	LegResult leg_iris(FrameContext &r_ctx);
	LegResult leg_sun_veil(FrameContext &r_ctx);
	LegResult leg_lights(FrameContext &r_ctx);
	LegResult leg_ragdolls(FrameContext &r_ctx);
	LegResult leg_materials(FrameContext &r_ctx);
	LegResult leg_inset_water(FrameContext &r_ctx);
	LegResult leg_framefx(FrameContext &r_ctx);
	LegResult leg_slot_shadows(FrameContext &r_ctx);
	LegResult leg_particles(FrameContext &r_ctx);
	LegResult leg_precipitation(FrameContext &r_ctx);
	LegResult leg_screen_effects(FrameContext &r_ctx);
	LegResult leg_scene_overlay(FrameContext &r_ctx);
	LegResult leg_audio(FrameContext &r_ctx);
	LegResult leg_clear(FrameContext &r_ctx);
	LegResult leg_environment_cube(FrameContext &r_ctx);
	LegResult leg_finish(FrameContext &r_ctx);
	// The frozen-pose replay's own rows.
	LegResult leg_celestial_settle(FrameContext &r_ctx);
	LegResult leg_iris_stamp(FrameContext &r_ctx);
	LegResult leg_weather_settle(FrameContext &r_ctx);
	LegResult leg_sky_settle(FrameContext &r_ctx);
	LegResult leg_water_settle(FrameContext &r_ctx);
	void begin_device_frame();
	void sample_panm_clock();
	void session_frame_failed(const String &p_reason);
	Camera3D *render_camera() const;
	// The camera whose frustum the frame's image is drawn through: the local
	// view presenter's stretched-frame camera while its target is live (the
	// surface camera then only carries a culling superset), else
	// render_camera().
	Camera3D *image_camera() const;
	// The surface (window) width in pixels: the retail viewport width.
	float surface_width() const;
	float surface_height() const;
	// The weapon Inset pass's camera while that pass renders this frame (the
	// local-view leg's hand-over), else null.
	Camera3D *inset_pass_camera() const;
	Transform3D render_camera_xform() const;
	// The eye the main scene's water mirror collects from: the view block the
	// frame's compose left (LocalPlayerPresenter::get_view_block_transform)
	// while the local view's camera draws the frame, else render_camera_xform().
	Transform3D main_mirror_collect_eye() const;
	void stamp_iris_samples(const Transform3D &p_camera_xform);
	void render_terrain_light_leg();
	void sample_auxiliary_render_stats(bool p_stats_on);
	void sample_water_render_stats(bool p_stats_on);
	void stop_water_render_stats();
	void restore_idle_frame_clear_color();
	void update_frame_clear_color();
	void music_var_pump();
	void sync_runtime_profiling();

	// --- the load plan (game_world_load.cpp) ---
	// The root a load resolves through: the injected one, else a fresh
	// runtime mount of `dir` (or the persisted resource directory when
	// empty). Emits load_failed and returns null when nothing resolves.
	Ref<ResourceRoot> resolve_root(const String &p_dir);
	Ref<ResourceRoot> mount_runtime_root(const String &p_dir);
	String resolver_expansion();
	String resolver_game();
	// load_mission past its mount: the document from `p_root` (archive-only) and the one mission
	// path. `p_timeline` is the entry's timeline (null: the load begins its own). A host's map
	// change loads the next map on the session's mount this way (SessionDrive).
	int load_mission_on_root(const String &p_bms_name, const Ref<ResourceRoot> &p_root,
			const Ref<LoadTimeline> &p_timeline);
	// The ONE mission path -- the file entry (load_mission) and the in-memory
	// entry (load_mission_data) converge here.
	int load_mission_internal(const Ref<MissionData> &p_mission, const String &p_bms_name,
			const Ref<ResourceRoot> &p_resource_root);
	// A join's effect system at its final S2C 0x64, ahead of the 0x09: a fresh effect world
	// with every mounted .ptl parsed and its catalog warmed, which the S2C 0x1A's load adopts
	// (start_effect_world) (engine: inmatch/joiner_connection.h set_mission_data_loading,
	// D-NET-1054). An earlier one (an abandoned mission start's) is freed first.
	void load_join_effect_system(const Ref<ResourceRoot> &p_resource_root);
	// A join's S2C 0x11 load (SessionDrive::step_preload): the mission start's head and its
	// terrain off the host's 0x0B header; its S2C 0x1A load_mission_internal goes on from the
	// objects.
	int load_join_terrain(const Ref<MissionData> &p_mission, const String &p_bms_name,
			const Ref<ResourceRoot> &p_resource_root);
	// The mission start's copies of the options (the object detail, the texfilter,
	// texcompression and particle-density words, the ragdoll bit), ahead of its effect system
	// and its head.
	void copy_mission_start_options();
	// The load plan's legs over this world and the load's simulation (engine
	// mission/mission_load.h): the legs hold `r_timeline` (the load's, which they begin when it is
	// null), `r_host_sim` (the host session its head resolves) and `r_runtime_options` (the
	// runtime's start's) by reference, so the caller keeps them for the load's call.
	opennova::mission::MissionLoadLegs mission_load_legs(const Ref<MissionData> &p_mission,
			const String &p_bms_name, const Ref<ResourceRoot> &p_resource_root,
			Ref<LoadTimeline> &r_timeline, Ref<Simulation> &r_host_sim,
			Ref<MissionSetupOptions> &r_runtime_options);
	void start_mission_root();
	void place_mission_objects(const Ref<MissionData> &p_mission);
	// One load-progress present at `p_percent` (the loading screen's due rule gates it): the
	// placer's per-model pulse at the object stage's constant value (the load plan's per-model
	// pulse witness) and the freeze's per-file present, each at its stored checkpoint.
	void pulse_load_progress(int p_percent);
	// One loading-screen present at the checkpoint it stored last (its due rule gates it),
	// from inside a loader: the effect system's load and its atlas build (engine
	// hud/loading_screen.h carries the witness).
	void present_stored_load_progress();
	// A loading step of a join's declared loads (the S2C 0x11's terrain, the final 0x64's
	// effects, the 0x1A's stage emits): the NovaWorld session's periodic update on the join's
	// simulation, the drive's until the runtime adopts it (engine
	// inmatch::JoinerRole::pump_mission_start_periodic_update, D-NET-1209); on any other load
	// (a NovaWorld host's) the held session's half alone (engine: inmatch::run_loading_step,
	// D-NET-1238); nothing without a NovaWorld session.
	void pump_join_load_step();
	// The terrain's .trn, overcast.def and the named .env over the engine
	// defaults, as the time-of-day load reads them; never fails.
	void load_environment(const String &p_trn_path, const String &p_env_path);
	void apply_mission_environment_overrides(const Ref<MissionData> &p_mission);
	void set_mission_water_height_override(float p_world_height);
	void set_water_world_rendering_enabled(bool p_enabled);
	// The hold's device legs (set_presentation_held), lifted alone across the load's forced frame.
	void apply_presentation_hold_(bool p_held);
	void set_weather_world_tick_driven(bool p_enabled);
	void prepare_world_driven_weather();
	void prepare_autonomous_weather();
	void run_mission_start_environment_boundary();
	void load_mission_tile_info(const String &p_bms_name, const Ref<MissionData> &p_mission,
			const Ref<ResourceRoot> &p_resource_root, const PackedByteArray &p_wire_til_bytes,
			bool p_wire_is_authoritative);
	bool apply_join_wire_til_if_ready();
	bool place_join_wire_world(const Ref<Simulation> &p_sim, const Ref<LoadTimeline> &p_timeline);
	// `p_timeline` (may be null) takes the placement's legs as nested spans (an instrument).
	void place_streamed_mission_objects(const Ref<Simulation> &p_sim,
			const Ref<LoadTimeline> &p_timeline = Ref<LoadTimeline>());
	void clear_mission_tile_info();
	// The terrain's connect leg (engine terrain/terrain_load_legs.h carries the witness):
	// the .trn, then overcast.def and the mission's .env (`p_environment`, its name without the
	// extension; empty: none) through the terrain's parser (D-TERRAIN-18), with the mission's tile
	// set over its tilestrip, the height data and the char and foliage maps, bound to every row
	// that reads the terrain; the terrain node left unbuilt for the mission textures stage.
	bool load_terrain(
			const String &p_trn_path, const String &p_tile_set, const String &p_environment);
	// The mission textures stage's terrain, every arm's (D-NET-1098): the texture slots and
	// the tile set's atlas the connect leg held and the terrain node's build over them, then the
	// foliage's. A dedicated host's start (`p_dedicated_host`), which jumps over the render
	// subsystems, builds no LOD cache (the node's tile meshes and patch pool), no tile cache page
	// array, and configures no foliage (engine terrain/terrain_load_legs.h builds_lod_cache,
	// builds_tile_cache_targets and loads_foliage_definitions, D-NET-1158, D-NET-1161,
	// D-NET-1133). `p_timeline` takes it as one span. Nothing when no connect leg is pending.
	void load_terrain_textures(const Ref<LoadTimeline> &p_timeline, bool p_dedicated_host);
	// The same stage's view effects, tracer pool and FrameFX past the HUD's textures: the full
	// arm creates the NVG view's device objects (FrameFx::init_view_effects), the tracer pool's
	// (EffectWorld::create_tracer_pool_devices) and FrameFX's pixel shaders with its "ffscan"
	// scanlines (WorldScene::init_frame_fx_mission_textures), a dedicated host's reduced arm
	// none (D-NET-1172, D-NET-1231).
	void load_frame_fx_textures(bool p_dedicated_host);
	void configure_foliage();
	// The runtime's start (the host boot's phase A inside MissionRoot::setup), its options into
	// `r_options`; its device tail past the mission start's place (finish_runtime_start).
	int start_runtime(const Ref<MissionData> &p_mission, const String &p_bms_name,
			Ref<MissionSetupOptions> &r_options);
	void finish_runtime_start(const Ref<MissionSetupOptions> &p_options, const String &p_bms_name);
	// The runtime's options the load knows at its start: the mission's names, the process's words
	// and profiles, the playable gate and the session request the drive stages (start_runtime adds
	// the terrain, the item table, the .til and the placer once the load built them).
	Ref<MissionSetupOptions> make_runtime_options(const Ref<MissionData> &p_mission,
			const String &p_bms_name);
	// A fresh host's or single player's session at the load's start, ahead of the head's terrain
	// and the objects: the runtime's options, the simulation staged over them
	// (MissionRoot::stage_simulation: its role, its record, its bound socket) and its session
	// created (Simulation::create_host_session), kept for start_runtime. Emits load_failed (or
	// process_exit_requested for the create's mpreset exit) and returns the error on a failure.
	int create_fresh_session(const Ref<MissionData> &p_mission, const String &p_bms_name,
			const Ref<ResourceRoot> &p_resource_root);
	// The local player's character profile (local_character_profile_) from Avatars.def's table
	// (null: the empty table's projection) over the spawn loadout, in session or out. The fresh
	// session's create reads it at the load's start, the placement again over the placer's table.
	void project_local_character_profile(const Ref<AvatarDatabase> &p_avatar_db);
	void load_player_weapon_profile();
	void start_mission_audio(const Ref<MissionData> &p_mission, const String &p_bms_name);
	// `p_spawn_effects` false: the catalog was warmed already (a join's effect system load), so
	// only the presenters' fire pipelines draw in the forced frames.
	// `p_timeline` (may be null) takes the warm's legs as nested spans (an instrument).
	int warm_effect_world_catalog(bool p_spawn_effects = true,
			const Ref<LoadTimeline> &p_timeline = Ref<LoadTimeline>());
	// The forced frame's warm of the skinned models the first revealed frame would make: the
	// local body and its first-person view model (when the local player's type is known), and one
	// model per visual identity the wire walk would cold-spawn, under a container at `p_position`.
	// The caller draws the frame and frees the container (null when nothing was built).
	// `p_timeline` (may be null) takes the builds as nested spans and their count as the
	// zero-length span person_models_<n> (an instrument).
	Node3D *build_person_pipeline_warm(const Vector3 &p_position, const Ref<LoadTimeline> &p_timeline);
	// A new effect world under this world with every mounted .ptl of `p_resource_root` parsed
	// at the session's words and its texture atlases built (the mission start's head's,
	// load_join_effect_system's and start_effect_world's for a load that built none; a
	// dedicated host's builds none). `p_timeline` (may be null) takes the atlas build as a
	// nested span (an instrument).
	EffectWorld *create_effect_world(const Ref<ResourceRoot> &p_resource_root,
			const Ref<LoadTimeline> &p_timeline = Ref<LoadTimeline>());
	void free_join_effect_world();
	void free_head_effect_world();
	void start_effect_world();
	void apply_occlusion_culling_policy();

	// --- the effect fan-out (game_world_effects.cpp) ---
	void route_round_impacts();
    void open_script_effects();
    void drain_unshown_script_effects();
	void route_dialog_lines();
	void route_terrain_scorches();

	// --- the world's retained scene nodes (game_world.tscn; absent from a
	//     code-built world), bound through the one scene table ---
	WorldScene scene_;
	Terrain *terrain_ = nullptr;
	MissionEnvironment *env_ = nullptr;
	Water *water_ = nullptr;
	EnvironmentCubeCapture *environment_cube_ = nullptr;
	// The weather's time-set reset count the cube last answered (render_environment_cube_frame).
	uint32_t cube_time_set_resets_ = 0;
	FrameFx *framefx_ = nullptr;
	Weather *weather_ = nullptr;
	Precipitation *precipitation_ = nullptr;
	Celestial *celestial_ = nullptr;
	SkyDome *sky_dome_ = nullptr;
	WorldEnvironment *clear_color_ = nullptr;
	FoliageDispatcher *dispatcher_ = nullptr;
	SunShadow *sun_shadow_ = nullptr;
	SlotShadow *slot_shadow_ = nullptr;
	// True once the env presenters' own _process is off and this world
	// advances them from render_environment_nodes_frame (the render
	// diagnostics report it).
	bool env_presenters_world_driven_ = false;

	// --- the mission state ---
	Ref<TerrainData> terrain_data_;
	Ref<ResourceRoot> resource_root_;
	Ref<TerrainTileInfo> mission_tile_info_;
	PackedByteArray mission_til_bytes_;
	bool join_wire_assets_pending_ = false;
	bool join_wire_til_applied_ = false;
	bool join_wire_assets_failed_ = false;
	bool join_wire_asset_failure_emitted_ = false;
	// A join's S2C 0x11 ran the mission start's head (load_join_terrain), which its S2C 0x1A load
	// skips.
	bool join_terrain_preloaded_ = false;
	// The connect leg loaded the terrain's configuration and heights; its textures and build wait
	// for the mission textures stage (load_terrain_textures).
	bool terrain_textures_pending_ = false;
	bool world_ready_ = false;
	// The loading presentation's hold (set_presentation_held).
	bool presentation_held_ = false;
	Ref<MissionData> loaded_mission_;
	// The BMS clock the weather home is seeded with at the mission-start
	// boundary.
	int mission_clock_start_q8_8_ = 0;
	int mission_clock_minutes_per_day_ = MissionEnvironment::DEFAULT_MINUTES_PER_DAY;
	// The BMS argument that completed the active mission load. This is
	// runtime state, deliberately separate from mission_file (the exported
	// boot option).
	String loaded_mission_file_;
	// The one mission root (sim + present pass + index + the MissionObjects
	// container), DIVIDED cadence; by identity, a reload queue_frees it.
	ObjectID runtime_id_;
	Ref<PanmClock> panm_clock_;
	Ref<MissionPlacementStats> mission_stats_;
	Ref<MissionObjectPlacer> placer_; // kept so mission audio reuses its item database
	Ref<LoadTimeline> last_load_timeline_; // the most recent load_mission timing
	// The first frames past the most recent mission load (last_post_load_frames): whether it still
	// times frames, the frames timed, whether a joiner's first C2S 0x0C past the final wait has left,
	// and the joiner net readout's next unread record.
	static constexpr int kPostLoadFrames = 8;
	static constexpr int kPostLoadFramesMax = 120;
	Ref<LoadTimeline> post_load_frames_;
	bool post_load_open_ = false;
	// The running leg's post-load frame while one of those frames runs it (null otherwise): the
	// scene legs' parts nest under it as spans (post_load_part, an instrument).
	LoadTimeline *post_load_leg_ = nullptr;
	void post_load_part(const char *p_name);
	void post_load_part_end();
	int post_load_frame_count_ = 0;
	bool post_load_uplink_seen_ = false;
	uint32_t post_load_trace_seq_ = 0;
	void begin_post_load_frames(const String &p_bms_name);
	void end_post_load_frames();
	// The frame's client net passes as spans of no length under its session leg (an instrument).
	void mark_post_load_net_passes(LoadTimeline &r_frames);
	// A post-load frame's deferred flush by leg under --verbose (an instrument): a marker queued
	// ahead of the legs and one after each leg that ran fire in the flush right after the work
	// queued ahead of them (MessageQueue is FIFO), so the time from one marker to the next is
	// what that leg queued (deferred calls, redraws, skeleton updates): a "flush_<leg>" span of
	// the post-load timeline when it reaches kPostLoadFlushSpanUs.
	static constexpr int64_t kPostLoadFlushSpanUs = 1000;
	int64_t post_load_flush_mark_us_ = 0;
	void queue_post_load_flush_mark(int p_leg);
	void post_load_flush_mark(int p_leg);
	// load_mission's timeline (its mount and document spans, an instrument), which
	// load_mission_internal goes on with.
	Ref<LoadTimeline> entry_timeline_;
	// The runtime's options create_fresh_session staged with their created session, which
	// start_runtime consumes (unload drops them).
	Ref<MissionSetupOptions> staged_runtime_options_;
	Ref<WeaponDatabase> weapon_db_; // weapon.def, lazy per mounted root (FP viewmodel)
	ObjectID music_director_id_;
	ObjectID mission_audio_id_;
	ObjectID effect_world_id_; // the runtime .ptl effect world (render-only, per mission)
	// The effect world a join's mission-data load built ahead of its 0x09, until the S2C 0x1A's
	// load adopts it (load_join_effect_system).
	ObjectID join_effect_world_id_;
	// The effect world the mission start's head built ahead of its terrain (a host's with its
	// own client, a single player's), until start_effect_world binds it (load_mission_internal).
	ObjectID head_effect_world_id_;
	// Frame-clear cache (divergence #21): recompute only when the env
	// generation moves or the camera crosses the water plane.
	int64_t clear_env_generation_ = -1;
	// The post-particle overlay frames published (the submission id), and
	// the glare/glint model surfaces the tail draws (geometry read once).
	uint64_t scene_overlay_frame_id_ = 0;
	SceneOverlayModelSurfaces scene_overlay_bodies_;
	void append_water_mirror_overlays(SceneOverlaySubmission &r_submission);
	// Each rendering water mirror's tracer ribbons, under the reflected scene's
	// pass fog (EntityPresenter::append_mirror_tracer_ribbons).
	void append_mirror_tracer_overlays(SceneOverlaySubmission &r_submission);
	// The frame's light values (null with no environment or light state),
	// shared by the overlay legs.
	Ref<EnvLightValues> frame_light_values() const;
	// The local player's FrameFX view facts: the defaults with no local
	// player or for a spectator.
	opennova::renderer::FrameFxViewInputs local_frame_fx_view() const;
	bool clear_above_water_ = true;
	bool clear_nvg_scene_ = false;
	// The render-occlusion frame (OcclusionFrame): the blink letter gates, the
	// per-frame section-mask/portal apply, the probe A/B seam edges and the
	// unload reset. Constructed once, wired to the retained scene nodes in
	// _ready, re-handed each mission's runtime members by the load; the
	// device legs call it directly (hot path -- no Callables).
	Ref<OcclusionFrame> occlusion_;
	// The local-player visuals (LocalPlayerVisuals): the FP viewmodel/arms
	// composition, the third-person avatar + held-gun builders, the armory
	// weapon apply/clear and spawn-loadout projection, and the typed
	// local-player view decodes; it OWNS the equipped-weapon state and the
	// staged spawn loadout.
	Ref<LocalPlayerVisuals> player_visuals_;
	// The per-item ITEMS.DEF effect director (ItemEffectDirector): the
	// attached/static/controller item emitters, the effect-anchor resolvers,
	// and the retail master particle switch (plain RefCounted -- it owns no
	// Nodes).
	Ref<ItemEffectDirector> item_fx_;
	Ref<EffectLightDirector> light_director_;
	// The loaded mission's force-indoors attribute, the kernel's mission's
	// (Simulation::mission_forces_indoors); handed to the occlusion frame's
	// entries as an argument.
	bool mission_forces_indoors() const;
	// The options' object detail (set_object_polydetail) and the session's
	// copy of it taken at each mission start (load_mission_internal), which
	// the LOD frames and the occlusion frame's death pieces read.
	int object_polydetail_ = opennova::renderer::kObjectLodDetailFreshProfile;
	int object_detail_ = opennova::renderer::kObjectLodDetailFreshProfile;
	// The options' aspect mode (set_aspect_mode); -1 selects the surface's own ratio.
	int aspect_mode_ = -1;
	// The options' texfilter_level (set_texfilter_level) and the session's copy
	// of it taken at each mission start; publish_texfilter_state hands both to
	// the device leg (render/texture_filter_device).
	int texfilter_level_ = opennova::renderer::kTexFilterLevelFreshProfile;
	int session_texfilter_level_ = opennova::renderer::kTexFilterLevelFreshProfile;
	void publish_texfilter_state();
	// The options' texcompression_level and particle_density and the session's
	// copies taken at each mission start; the terrain and the effect world
	// take the copies (publish_session_render_settings).
	int texcompression_level_ = opennova::renderer::kTexCompressionLevelFreshProfile;
	int session_texcompression_level_ = opennova::renderer::kTexCompressionLevelFreshProfile;
	int terrain_texdetail_ = opennova::renderer::kTerrainTexDetailFreshProfile;
	int session_terrain_texdetail_ = opennova::renderer::kTerrainTexDetailFreshProfile;
	int object_texdetail_ = opennova::renderer::kObjectTexDetailFreshProfile;
	int session_object_texdetail_ = opennova::renderer::kObjectTexDetailFreshProfile;
	int particle_density_ = opennova::renderer::kParticleDensityFreshProfile;
	int session_particle_density_ = opennova::renderer::kParticleDensityFreshProfile;
	CorpseRagdollPresenter ragdolls_;
	void publish_session_render_settings();
	Color idle_frame_clear_color_ = Color(0, 0, 0);
	// A shell-injected resource root (main_game hands its boot mount over;
	// tests hand fixture roots). When set, the load_* entries skip the
	// settings lookup + their own mount and resolve through it; the game path
	// (no injection) still mounts from the persisted resource directory
	// through the resolver.
	Ref<ResourceRoot> injected_root_;
	Ref<ResourceRootResolver> root_resolver_;
	// Debug: hide the scattered foliage (the dev tools' "Hide foliage"). Off
	// by default.
	bool foliage_hidden_ = false;
	bool playable_ = true;
	// The net-session drive: typed request staging, the joiner
	// preload/admission state machines, the ESC aborts, and NovaWorld gate
	// registration. The session signals live on THIS node -- the shell
	// contract pins them here -- and the drive emits them through its world.
	SessionDrive drive_;
	// The local player's two per-side character selections + classes
	// projected for the sim (the listen host's own type-2 connection / a
	// joiner's ClientAuth).
	Ref<CharacterJoinProfile> local_character_profile_;
	Ref<PlayerProfiles> player_profiles_;
	Ref<GameConfigBlock> game_config_;
	Ref<ControlsModel> controls_model_; // set_controls_model
	Callable menu_screen_probe_;        // set_menu_screen_probe
	bool restart_boot_ = false;          // set_restart_boot, consumed by the next load
	// The process's map-rotation words, one for the world's whole run: retail's are
	// process globals no session start resets, so a session that ended mid-map
	// hands its half toggle, SETNEXT latch and side-to-team map to the next
	// (Simulation.use_host_rotation; inmatch/mission_rotation.h).
	opennova::inmatch::HostRotation host_rotation_;
	// The process's client words (the 0x4C report's count), which every hosted session's own
	// client runtime steps on (Simulation.use_client_process; inmatch/client_runtime.h
	// ClientProcessState).
	opennova::inmatch::ClientProcessState client_process_;
	// The server log devices the process's command-line switches arm (/PROFILE, /PUNTLOG,
	// /PUNT.TXT, /CHEATLOG, /NETLOG, /INOUT), alive for the world's run as retail's globals
	// are for the process's, every hosted session writing them
	// (Simulation.use_log_devices; inmatch/server_log_devices.h). Armed at _ready, closed at
	// the exit from the tree.
	std::unique_ptr<opennova::inmatch::ServerLogDevices> log_devices_;
	// The process's remote admin (engine inmatch/remote_admin.h): admin.cfg and the listener
	// once the process block is known, the console bound over the running session (its host
	// context, else a joiner's own client, else session-less; any change of the binding
	// rebinds), the CRT stream its challenge draws from outside a mission, and the TCP device.
	// Pumped at the end of every update a playing mission's session runs (the session's update
	// tail, install_admin_update_tail) and by the internal process step on the frames
	// remote_admin_idle_scene names (the menus, a paused single player, whose stored exit
	// reason leaves the mission instead: remote_admin_idle_frame_exits).
	opennova::inmatch::RemoteAdminConsoles admin_consoles_;
	opennova::io::CrtRand admin_rand_;
	std::unique_ptr<opennova::AdminServer> admin_server_;
	std::unique_ptr<AdminTcpPump> admin_pump_;
	bool admin_opened_ = false;
	// Open on the first pump with a process block; false until then.
	bool open_remote_admin();
	// One pump in `p_scene` over the running session (none: session-less).
	void pump_remote_admin(opennova::inmatch::AdminScene p_scene);
	// The session's update tail over `p_sim`: the Game Loop's pump past every update's tick
	// while the mission plays, as opennova-serve's update tail pumps its own (engine
	// inmatch::Session::set_update_tail carries the witness).
	void install_admin_update_tail(const Ref<Simulation> &p_sim);
	// The console's seams over `p_binding` (the session's, else the process block's).
	opennova::inmatch::AdminConsole::Seams remote_admin_seams(const Ref<Simulation> &p_sim,
			const opennova::inmatch::RemoteAdminBinding &p_binding);
	// The HUD the console's CHAT window and echo reach (its group); null with none.
	HudOverlay *admin_hud_overlay() const;
	// The last frame's world-tick leg counters (RuntimePerfCounters).
	int64_t perf_tick_us_ = 0;
	int64_t perf_foliage_us_ = 0;
	int64_t perf_runtime_us_ = 0;
	int64_t perf_audio_us_ = 0;
	// The manual frame-span/A-B probe (the perf probes' seam).
	bool perf_probe_enabled_ = false;
	bool perf_probe_skip_occl_ = false;
	bool perf_probe_skip_effect_tick_ = false;
	bool perf_probe_skip_fixed_handlers_ = false;
	bool perf_probe_occlusion_skipped_ = false;
	// The shared F3 frame-stats board (null outside the game shell). Feeds
	// gate on board capture so a closed Stats tab costs nothing; the
	// occlusion split spans land from OcclusionFrame.apply_frame, the tick
	// legs from the leg table's LegScope.
	Ref<FrameStats> frame_stats_;
	// The shell-owned local-player presenter whose camera/viewmodel placement
	// the local-view device leg runs inside the frame (null in worlds without
	// one -- tests, dedicated). D-RORD-8: placing it before the
	// occlusion/iris/particle legs lets them read the camera THIS frame's tick
	// produced, not last frame's.
	ObjectID local_view_presenter_id_;
	// The HUD's weapon Inset scope (set_inset_scope); the same D-RORD-8 order
	// for the Inset camera: the local-view leg stamps it and hands it over.
	ObjectID inset_scope_id_;
	// The 256x256 depthspin-equivalent shore mask. The base map always binds
	// the original sharp colormap; this texture carries only transparent/blue
	// water.
	Ref<ImageTexture> minimap_water_mask_;

	// --- the per-frame device latch (begin_device_frame stamps it for the
	//     legs) ---
	Vector3 frame_camera_pos_;
	Transform3D frame_camera_xform_;
	double frame_delta_ = 0.0;
	// The main loop's clock (engine inmatch/main_loop_clock.h): the frame's stamp the elapsed
	// time and the frame lock measure from, and the last render's (frame_post_draw).
	opennova::inmatch::MainLoopClock main_loop_clock_;
	bool frame_probe_enabled_ = false;
	bool frame_stats_on_ = false;
	bool frame_timing_ = false;
	bool frame_skip_occlusion_ = false;
	int64_t device_frame_start_us_ = 0;
	// Bit i: the live table's row i ran its body in the last frame (get_last_frame_legs).
	uint64_t last_frame_run_mask_ = 0;
	bool last_frame_scene_drawn_ = true;
	bool last_frame_world_skipped_ = false;
	bool last_frame_hud_pass_called_ = true;
	bool last_frame_start_blank_ = false;
	// The black cover the three drawn frames past a mission start present (present_start_blank),
	// made at the first of them.
	ObjectID start_blank_cover_id_;
	void present_start_blank(bool p_blank);
	// Identity latch for measured render time on the water reflection RTT.
	ObjectID stats_water_vp_id_;
	// Edge latch for the compositor passes' RD GPU timestamps (F3 capture
	// only).
	bool stats_aux_gpu_timing_ = false;
};

} // namespace godot
