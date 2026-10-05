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
#include "object/object_model.h"
#include "object/weapon_database.h"
#include "particle/effect_world.h"
#include "player/first_person_arms_witness.h"
#include "player/local_player_presenter.h"
#include "player/local_player_visuals.h"
#include "player/player_spawn_loadout.h"
#include "player/player_viewmodel_def.h"
#include "render/frame_fx.h"
#include "resource_index/resource_root.h"
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
#include "world/world_view.h"

namespace godot {

class HudInsetScope;

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
	};
	enum FrameLegFlags : uint32_t {
		kLegNone = 0,
		// A kLegStop return from this leg STOPS the frame: the later legs (the
		// finish included) do not run.
		kStopsFrame = 1u << 0,
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

	// --- the exported boot options ------------------------------------------
	// A mission (.bms) to boot into. When set, the mission's header selects the
	// terrain + environment (terrain_file below is ignored) and its
	// placed objects are populated into the world. Empty = load bare terrain +
	// environment.
	void set_mission_file(const String &p_value) { mission_file_ = p_value; }
	String get_mission_file() const { return mission_file_; }
	// The terrain loaded, by name, from the resource directory (beside the
	// fixed default environment, env_file_). Used only when mission_file is empty.
	void set_terrain_file(const String &p_value) { terrain_file_ = p_value; }
	String get_terrain_file() const { return terrain_file_; }

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

	// --- the load entries ------------------------------------------------------
	// Load the world from `dir`, or from the persisted resource directory when
	// empty. Returns OK, or ERR_FILE_NOT_FOUND when the directory is
	// unset/missing the terrain (the caller decides whether to prompt). No
	// fallbacks: the chosen directory is the only place looked.
	int load_world(const String &p_dir = String());
	// Load a mission (.bms): its header selects the terrain + environment,
	// which are resolved from `dir` (or the persisted resource directory) and
	// loaded through the same path as load_world, then the mission's placed
	// objects are populated into the world. Returns OK, or the same error
	// codes as load_world.
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
	// The NovaWorld session (a NovaWorldClient), handed over by the shell with
	// a NovaWorld join or host: the world keeps it playing or hosting through
	// the match (SessionDrive::adopt_nw_client). A normal exit back to the
	// NovaWorld menu takes it back out, still connected (null when none);
	// otherwise it goes down with the world.
	void adopt_novaworld_client(Node *p_client);
	Node *release_novaworld_client();
	// The post-mission route for an exit reason (SessionDrive::post_mission_route).
	Ref<PostMissionRoute> post_mission_route(int p_reason) const;
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
	// ESC/abort for the joiner's post-load admission wait. Returns true when
	// a live admission wait was told to abort (see
	// SessionDrive::cancel_admission_wait).
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
	Color get_current_frame_clear_color() const;

	// --- the frame ------------------------------------------------------------
	// The game shell hands its FrameStats here; the world re-hands it to every
	// MissionRoot it creates and feeds its own tick legs.
	void set_music_director(MusicDirector *director);
	MusicDirector *get_music_director() const;
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
	// Main-thread display clock, shared by every world and menu. The first
	// reader samples once per Engine process frame, including menu-only frames.
	static int64_t current_frame_clock_ms();
	int64_t get_frame_clock_ms() const { return current_frame_clock_ms(); }
	// The leg table as literal names, in order (the frame-order pin).
	static PackedStringArray frame_leg_names();
	static PackedStringArray frozen_pose_leg_names();
	// Whether the named live leg stops the frame on a false return.
	static bool frame_leg_stops_frame(const String &p_name);
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
	void mix_audio_frame(int p_ticks_run);
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
	// RenderingServer frame_post_draw: stamps when the frame finished
	// rendering, the clock a mission-start frame re-bases to.
	void on_frame_post_draw();
	// Seconds since the last frame finished rendering (negative before the
	// first). The shell's frame loop hands it to the session on each frame
	// (MissionFrameInput.since_render_seconds); a synthetic driver stepping
	// tick() leaves it unsampled and banks its own delta.
	double get_seconds_since_render() const;
	void on_runtime_simulation_restarted();
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
	LegResult leg_local_view(FrameContext &r_ctx);
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
	LegResult leg_materials(FrameContext &r_ctx);
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
	// The weapon Inset pass's camera while that pass renders this frame (the
	// local-view leg's hand-over), else null.
	Camera3D *inset_pass_camera() const;
	Transform3D render_camera_xform() const;
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
	// The ONE mission path -- the file entry (load_mission) and the in-memory
	// entry (load_mission_data) converge here.
	int load_mission_internal(const Ref<MissionData> &p_mission, const String &p_bms_name,
			const Ref<ResourceRoot> &p_resource_root);
	void start_mission_root();
	void place_mission_objects(const Ref<MissionData> &p_mission);
	// The placer's per-model progress pulse (the load plan's per-model pulse
	// witness): the loading screen at the object stage's constant value.
	void pulse_object_stage_progress();
	// The named .env, or the engine defaults when it is not there; never fails.
	void load_environment(const String &p_env_path);
	void apply_mission_environment_overrides(const Ref<MissionData> &p_mission);
	void set_mission_water_height_override(float p_world_height);
	void set_water_world_rendering_enabled(bool p_enabled);
	void set_weather_world_tick_driven(bool p_enabled);
	void prepare_world_driven_weather();
	void prepare_autonomous_weather();
	void run_mission_start_environment_boundary();
	void load_mission_tile_info(const String &p_bms_name, const Ref<ResourceRoot> &p_resource_root,
			const PackedByteArray &p_wire_til_bytes, bool p_wire_is_authoritative);
	bool apply_join_wire_til_if_ready();
	void place_streamed_mission_objects(const Ref<Simulation> &p_sim);
	void clear_mission_tile_info();
	bool load_terrain(const String &p_trn_path, const String &p_tile_set);
	void configure_foliage();
	int start_runtime(const Ref<MissionData> &p_mission, const String &p_bms_name);
	void load_player_weapon_profile();
	void start_mission_audio(const Ref<MissionData> &p_mission, const String &p_bms_name);
	int warm_effect_world_catalog();
	void start_effect_world();
	void apply_occlusion_culling_policy();

	// --- the effect fan-out (game_world_effects.cpp) ---
	void route_round_impacts();
    void route_script_effects();
	void route_terrain_scorches();
	void resync_weather_after_restore();

	// --- the world's retained scene nodes (game_world.tscn; absent from a
	//     code-built world) ---
	Terrain *terrain_ = nullptr;
	MissionEnvironment *env_ = nullptr;
	Water *water_ = nullptr;
	EnvironmentCubeCapture *environment_cube_ = nullptr;
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
	bool world_ready_ = false;
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
	Ref<WeaponDatabase> weapon_db_; // weapon.def, lazy per mounted root (FP viewmodel)
	ObjectID music_director_id_;
	ObjectID mission_audio_id_;
	ObjectID effect_world_id_; // the runtime .ptl effect world (render-only, per mission)
	// Frame-clear cache (divergence #21): recompute only when the env
	// generation moves or the camera crosses the water plane.
	int64_t clear_env_generation_ = -1;
	// The post-particle overlay frames published (the submission id), and
	// the glare/glint model surfaces the tail draws (geometry read once).
	uint64_t scene_overlay_frame_id_ = 0;
	SceneOverlayModelSurfaces scene_overlay_bodies_;
	void append_water_mirror_overlays(SceneOverlaySubmission &r_submission);
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
	// The mission attribute that forces the indoors accum bit every frame.
	// Stays on the world (mission state); handed to the occlusion frame's
	// entries as an argument. [orig: g_BmsAttribFlags & 0x10 @ 0x5ca1c8-0x5ca1cd]
	bool mission_forces_indoors_ = false;
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
	// Time::get_ticks_usec() at the last frame_post_draw (0: none yet).
	uint64_t last_post_draw_usec_ = 0;
	bool frame_probe_enabled_ = false;
	bool frame_stats_on_ = false;
	bool frame_timing_ = false;
	bool frame_skip_occlusion_ = false;
	int64_t device_frame_start_us_ = 0;
	// Identity latch for measured render time on the water reflection RTT.
	ObjectID stats_water_vp_id_;
	// Edge latch for the compositor passes' RD GPU timestamps (F3 capture
	// only).
	bool stats_aux_gpu_timing_ = false;

	// --- the exported boot options ---
	String mission_file_;
	String terrain_file_ = "Dvxi5.trn";
	String env_file_ = "full_00.env";
};

} // namespace godot
