// GameWorld's mission load plan (the engine's mission_load_plan.h names the
// order): the four load entries, the ONE internal mission path, mount, place,
// environment, terrain, the tile info and join-wire assets, the runtime
// start, audio, the minimap mask, the effect world, and the explicit unload
// order. The former world_load_stages.gd plus game_world.gd's load entries
// (slice G10). This is not a second load coordinator: the shell's
// WorldLoadCoordinator owns the operation and its cancel/settle edges.

#include "world/game_world.h"

#include <cmath>

#include <base/gameprofile/resource_missing.h>
#include <formats/mission/bms_edit.h>
#include <runtime/inmatch/joiner_role.h> // run_loading_step (the loading steps' periodic update)
#include <runtime/inmatch/loaded_model_list.h> // avatar_combo_models (the 0x11's avatar loads)
#include <runtime/ragdoll/ragdoll_option.h>
#include <runtime/environment/celestial_frame.h> // builds_celestial_draws (the bodies' arm)
#include <runtime/environment/environment_state.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/mission/placement_traits.h> // builds_placement_draws (the dedicated host's placement)
#include <runtime/terrain/terrain_load_legs.h> // the dedicated arm's terrain legs
#include <runtime/renderer/environment_cube.h> // creates_environment_cube (the render subsystems)
#include <runtime/renderer/frame_fx_effects.h> // creates_view_effect_shaders (the mission textures)
#include <runtime/renderer/tracer_frame.h> // creates_tracer_pool_shaders (the mission textures)

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "audio/mission_audio_records.h"
#include "env/env_file.h"
#include "env/mission_environment_overrides.h"
#include "mission/mission_info.h"
#include "object/avatar_database.h"
#include "object/object_data.h"
#include "resource_index/launch_flags.h"
#include "simulation/entity_presenter.h"
#include "terrain/terrain_tile_info.h"
#include "util/string_convert.h"

using namespace godot;

namespace {

constexpr const char *kSignalWorldLoaded = "world_loaded";
constexpr const char *kSignalLoadFailed = "load_failed";
constexpr const char *kSignalProcessExitRequested = "process_exit_requested";
constexpr const char *kSignalLoadProgress = "load_progress";
constexpr const char *kSignalMinimapWaterChanged = "minimap_water_changed";
constexpr const char *kSignalMusicContextOpened = "music_context_opened";
constexpr const char *kSignalMusicContextClosed = "music_context_closed";
constexpr const char *kSignalHudOverlayInit = "hud_overlay_init";
constexpr const char *kSignalHudTexturesLoad = "hud_textures_load";

} // namespace

// --- the resource root -----------------------------------------------------------

String GameWorld::resolver_expansion() {
	return root_resolver_.is_valid() ? root_resolver_->expansion() : String();
}

String GameWorld::resolver_game() {
	return root_resolver_.is_valid() ? root_resolver_->game() : String("jo");
}

// The root a load resolves through: the injected one, else a fresh runtime
// mount of `dir` (or the shell's persisted resource directory when empty).
// Emits load_failed and returns null when nothing resolves.
Ref<ResourceRoot> GameWorld::resolve_root(const String &p_dir) {
	if (injected_root_.is_valid()) {
		return injected_root_;
	}
	String dir = p_dir;
	if (dir.is_empty() && root_resolver_.is_valid()) {
		dir = root_resolver_->resource_dir();
	}
	if (dir.is_empty()) {
		emit_signal(kSignalLoadFailed, "no resource directory set");
		return Ref<ResourceRoot>();
	}
	return mount_runtime_root(dir);
}

// Mount `dir` as the runtime resource root: PFF archives are the packed game
// data, the `/exp <name>` flag (or persisted setting) layers an expansion over
// the base, loose files override the archives only under the `/d` dev flag,
// and the `/game <code>` flag (or persisted setting, default "jo") selects the
// SCR decode key so demo data decodes correctly. Emits load_failed and
// returns null on a bad root.
// The expansion here is the LOCAL choice, which is only authoritative for
// single-player and for hosting. A joiner's is the HOST's, learned after this
// mount and reconciled by SessionDrive::reconcile_join_expansion before any
// host data is read (D-NET-178).
Ref<ResourceRoot> GameWorld::mount_runtime_root(const String &p_dir) {
	Ref<ResourceRoot> resource_root;
	resource_root.instantiate();
	const String expansion = LaunchFlags::expansion(resolver_expansion());
	const String game = LaunchFlags::game(resolver_game());
	if (resource_root->mount_runtime(p_dir, expansion, LaunchFlags::loose_override_enabled(), game) != OK) {
		emit_signal(kSignalLoadFailed, resource_root->get_last_error());
		return Ref<ResourceRoot>();
	}
	return resource_root;
}

// --- the load entries --------------------------------------------------------------

// Load the world from `dir`, or from the persisted resource directory when
// empty. Returns OK, or ERR_FILE_NOT_FOUND when the directory is unset/missing
// the terrain (the caller decides whether to prompt). No fallbacks: the
// chosen directory is the only place looked.
int GameWorld::load_world(const String &p_dir) {
	if (!mission_file_.is_empty()) {
		return load_mission(mission_file_, p_dir);
	}
	Ref<ResourceRoot> resource_root = resolve_root(p_dir);
	if (resource_root.is_null()) {
		return ERR_CANT_OPEN;
	}
	if (!resource_root->has_file(terrain_file_)) {
		emit_signal(kSignalLoadFailed,
				vformat("%s not found in %s", terrain_file_, resource_root->get_root_dir()));
		return ERR_FILE_NOT_FOUND;
	}
	if (!resource_root->has_file(env_file_)) {
		emit_signal(kSignalLoadFailed,
				vformat("%s not found in %s", env_file_, resource_root->get_root_dir()));
		return ERR_FILE_NOT_FOUND;
	}

	set_water_world_rendering_enabled(false);
	set_mission_water_height_override(NAN);
	clear_mission_tile_info();
	resource_root_ = resource_root;
	load_environment(terrain_file_, env_file_);
	// A world with no mission start loads its celestial bodies with its environment and draws them.
	scene_.load_celestial_models(resource_root_, /*p_draws=*/true);
	if (!load_terrain(terrain_file_, String(), env_file_.get_basename())) {
		emit_signal(kSignalLoadFailed, vformat("failed to load %s", terrain_file_));
		return ERR_CANT_OPEN;
	}
	// A world with no mission has no mission start: the terrain's mission textures stage (its
	// texture slots, its build and the foliage's, D-NET-NEW-L2-46) follows the connect leg at once.
	Ref<LoadTimeline> timeline = LoadTimeline::begin(vformat("World load %s", terrain_file_));
	last_load_timeline_ = timeline;
	load_terrain_textures(timeline, /*p_dedicated_host=*/false);
	load_sky_maps(timeline, /*p_dedicated_host=*/false);
	load_frame_fx_textures(timeline, /*p_dedicated_host=*/false);
	timeline->finish();

	world_ready_ = true;
	prepare_autonomous_weather();
	set_water_world_rendering_enabled(true);
	emit_signal(kSignalWorldLoaded);
	return OK;
}

// Load a mission (.bms): its header selects the terrain + environment, which
// are resolved from `dir` (or the persisted resource directory) and loaded
// through the same path as load_world, then the mission's placed objects are
// populated into the world. Returns OK, or the same error codes as
// load_world.
int GameWorld::load_mission(const String &p_bms_name, const String &p_dir) {
	emit_signal(kSignalLoadProgress,
			MissionData::load_progress_percent(MissionData::LOAD_STAGE_MISSION_SETUP));
	// The load's timeline from its entry: the root's mount and the document's parse lead its
	// spans (an instrument).
	Ref<LoadTimeline> timeline = LoadTimeline::begin(vformat("Mission load %s", p_bms_name));
	last_load_timeline_ = timeline;
	timeline->span("mount");
	Ref<ResourceRoot> resource_root = resolve_root(p_dir);
	timeline->end_span();
	if (resource_root.is_null()) {
		timeline->finish();
		return ERR_CANT_OPEN;
	}
	return load_mission_on_root(p_bms_name, resource_root, timeline);
}

int GameWorld::load_mission_on_root(const String &p_bms_name, const Ref<ResourceRoot> &p_root,
		const Ref<LoadTimeline> &p_timeline) {
	Ref<LoadTimeline> timeline = p_timeline;
	if (timeline.is_null()) {
		emit_signal(kSignalLoadProgress,
				MissionData::load_progress_percent(MissionData::LOAD_STAGE_MISSION_SETUP));
		timeline = LoadTimeline::begin(vformat("Mission load %s", p_bms_name));
		last_load_timeline_ = timeline;
	}
	// The runtime BMS path bypasses loose overrides even under /d.
	// [orig: Mission_LoadBMSFromPFF @ 0x40d43c]
	if (!p_root->has_file(p_bms_name, ResourceRoot::LOOKUP_FORCE_ARCHIVE_ONLY)) {
		emit_signal(kSignalLoadFailed,
				vformat("%s not found in %s", p_bms_name, p_root->get_root_dir()));
		timeline->finish();
		return ERR_FILE_NOT_FOUND;
	}
	timeline->span("document");
	Ref<MissionData> mission;
	mission.instantiate();
	if (mission->open_from_resource_root(p_root, p_bms_name,
				ResourceRoot::LOOKUP_FORCE_ARCHIVE_ONLY) != OK) {
		emit_signal(kSignalLoadFailed,
				vformat("failed to parse %s: %s", p_bms_name, mission->get_last_error()));
		timeline->finish();
		return ERR_CANT_OPEN;
	}
	timeline->end_span();
	entry_timeline_ = timeline;
	return load_mission_internal(mission, p_bms_name, p_root);
}

// Load the exact saved, top-level loose BMS from the selected resource root.
// This tooling/test seam deliberately differs from load_mission(), whose
// retail contract remains archive-only even when the session has /d. Only
// the BMS itself is forced to disk; terrain, environment, objects and
// sidecars continue through the mounted runtime root and its normal /d
// policy.
int GameWorld::load_loose_mission(const String &p_bms_name, const String &p_dir) {
	emit_signal(kSignalLoadProgress,
			MissionData::load_progress_percent(MissionData::LOAD_STAGE_MISSION_SETUP));
	const String mission_name = p_bms_name.strip_edges().replace("\\", "/");
	if (mission_name.is_empty() || mission_name != mission_name.get_file() ||
			mission_name.get_extension().to_lower() != "bms") {
		emit_signal(kSignalLoadFailed, "loose mission must be a top-level .bms file");
		return ERR_INVALID_PARAMETER;
	}
	Ref<ResourceRoot> resource_root = resolve_root(p_dir);
	if (resource_root.is_null()) {
		return ERR_CANT_OPEN;
	}
	const String mission_path = resource_root->get_root_dir().path_join(mission_name);
	if (!FileAccess::file_exists(mission_path)) {
		emit_signal(kSignalLoadFailed,
				vformat("%s not found in %s", mission_name, resource_root->get_root_dir()));
		return ERR_FILE_NOT_FOUND;
	}
	Ref<MissionData> mission;
	mission.instantiate();
	if (mission->open_file(mission_path) != OK) {
		emit_signal(kSignalLoadFailed,
				vformat("failed to parse %s: %s", mission_name, mission->get_last_error()));
		return ERR_CANT_OPEN;
	}
	return load_mission_internal(mission, mission_name, resource_root);
}

// Load an in-memory mission through the shared world pipeline. This is
// retained as a focused engine-test/tool seam; normal game launches always
// use a saved .bms through load_mission() or load_loose_mission().
int GameWorld::load_mission_data(const Ref<MissionData> &p_mission, const String &p_bms_name,
		const String &p_dir) {
	emit_signal(kSignalLoadProgress,
			MissionData::load_progress_percent(MissionData::LOAD_STAGE_MISSION_SETUP));
	if (p_mission.is_null() || !p_mission->is_loaded()) {
		emit_signal(kSignalLoadFailed, "no mission document to load");
		return ERR_INVALID_PARAMETER;
	}
	Ref<ResourceRoot> resource_root = resolve_root(p_dir);
	if (resource_root.is_null()) {
		return ERR_CANT_OPEN;
	}
	return load_mission_internal(p_mission, p_bms_name, resource_root);
}

// --- the ONE mission path ------------------------------------------------------------

// The file entry (load_mission) and the in-memory entry (load_mission_data)
// converge here: resolve the header's terrain + environment from
// `resource_root`, apply the mission's env overrides, build the world, place
// objects, start the runtime + audio.
int GameWorld::load_mission_internal(const Ref<MissionData> &p_mission, const String &p_bms_name,
		const Ref<ResourceRoot> &p_resource_root) {
	const bool wire_header_join = p_mission->is_wire_header_only();
	// A join ran the head at its S2C 0x11 (load_join_terrain): this load, at its S2C 0x1A, goes
	// on from the objects (engine inmatch/joiner_connection.h set_sync_tail_loading carries the
	// witness).
	const bool head_loaded = wire_header_join && join_terrain_preloaded_;
	join_terrain_preloaded_ = false;
	// The simulation whose host session this load runs: a fresh one's, staged with its create,
	// else a map change's kept one (null on any other load).
	Ref<Simulation> host_sim;
	// load_mission's entry timeline, when it ran (its mount and document spans).
	Ref<LoadTimeline> timeline = entry_timeline_;
	entry_timeline_.unref();
	if (head_loaded) {
		if (timeline.is_null()) {
			timeline = LoadTimeline::begin(vformat("Mission load %s", p_bms_name));
			last_load_timeline_ = timeline;
		}
	} else {
		// A fresh host's or single player's session stands before the mission start's every load:
		// its simulation staged (the host's bound socket) and its session created here, ahead of
		// the head's terrain and the objects (create_fresh_session, D-NET-NEW-L1-50).
		if (drive_.fresh_authority_load()) {
			const int create_error = create_fresh_session(p_mission, p_bms_name, p_resource_root);
			if (create_error != OK) {
				if (timeline.is_valid()) timeline->finish();
				return create_error;
			}
		}
		host_sim = staged_runtime_options_.is_valid() ? staged_runtime_options_->get_simulation()
													  : drive_.kept_simulation();
		// A host's mission start runs its head ahead of its terrain, a fresh session's past its
		// create and a map change's on the kept session, and pumps the session through the
		// load's window and stages, as retail's load does (engine inmatch/host_role.h
		// begin_mission_load / begin_next_mission_load / pump_load_stage, D-NET-116).
		if (host_sim.is_valid())
			host_sim->begin_host_mission_load(p_mission, p_resource_root, p_bms_name);
		// A NovaWorld listen host's mission-start server-info update, past its session's create or
		// the map change's head and ahead of the effect system and every load (engine
		// inmatch/server_info.h apply_mission_start_columns carries the witness, D-NET-NEW-L1-50).
		drive_.publish_host_mission_start(host_sim, p_mission, p_resource_root, p_bms_name);
		if (timeline.is_null()) {
			timeline = LoadTimeline::begin(vformat("Mission load %s", p_bms_name));
			last_load_timeline_ = timeline;
		}
		copy_mission_start_options();
		// Then the mission start's effect system at the copies' words, past the head's per-slot
		// reset and ahead of the window's first pumps and the terrain, on every load but a
		// dedicated host's: retail's skips it in session on the authority with no client of its
		// own (engine inmatch/host_role.h open_mission_load_window carries the witness,
		// D-NET-NEW-L3-22). A join's mission-data load built one already
		// (load_join_effect_system); a single player's restart, whose effect system retail keeps
		// from its first load, builds one here too, its unload having freed the last.
		// start_effect_world binds it to the mission once the runtime stands.
		if (!drive_.pending_dedicated() && ObjectDB::get_instance(join_effect_world_id_) == nullptr) {
			timeline->span("effect_system");
			free_head_effect_world();
			head_effect_world_id_ = ObjectID(create_effect_world(p_resource_root, timeline)->get_instance_id());
			timeline->end_span();
		}
		if (host_sim.is_valid()) host_sim->open_host_load_window();
		const int head_error = load_mission_head(p_mission, p_bms_name, p_resource_root, timeline);
		if (head_error != OK) {
			staged_runtime_options_.unref(); // the created session goes with its simulation
			return head_error;
		}
		if (host_sim.is_valid()) host_sim->pump_host_load_stage(); // after the terrain
	}
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_OBJECTS));
	// A join's loading steps run its session's periodic update after each present.
	pump_join_load_step();

	loaded_mission_ = p_mission;
	timeline->span("objects");
	start_mission_root();
	// A join whose S2C 0x11 ran the head keeps that load's placer, the avatar models in it.
	if (!head_loaded) placer_.unref();
	// A host's load: the stages after the mission's document and after the avatars and the
	// clock, ahead of the objects.
	if (host_sim.is_valid()) {
		host_sim->pump_host_load_stage();
		host_sim->pump_host_load_stage();
	}
	place_mission_objects(p_mission);
	timeline->end_span();
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_RUNTIME));
	pump_join_load_step();
	timeline->span("runtime");
	// Open and seed GAME music for sessions with a local client, including SP;
	// dedicated hosts close the context. The shell owns the actual music VM.
	// The connection-mode is_client gate and the original open/seed sequence
	// are witnessed in docs/audio/mus-sbf-re.md (Context lifecycle, D-MUS-SPGATE).
	// M# resolves this context while WAC compiles, so opening must precede
	// boot: the original mission start opens music before its WAC init call,
	// see docs/world/world-wac-ai-re.md section 33.15a.
	if (drive_.pending_dedicated()) emit_signal(kSignalMusicContextClosed);
	else emit_signal(kSignalMusicContextOpened, resource_root_);
	const int runtime_error = start_runtime(p_mission, p_bms_name);
	timeline->end_span();
	if (runtime_error != OK) {
		timeline->finish();
		// A load abort leaves the mission the way the player's own quit does: the
		// shell's post-mission route decides the NovaWorld session, so it outlives
		// this unload.
		NovaWorldClient *session = drive_.release_nw_client();
		unload();
		drive_.adopt_nw_client(session);
		return runtime_error;
	}
	// The C2S 0x3D page freezes once here (the load plan's freeze witness): the
	// mission start's model-def list computed from the mount and the loaded tables
	// (engine: inmatch/loaded_model_list.h, D-NET-527); late network spawns must not
	// change this page.
	Ref<Simulation> challenge_sim = get_runtime()->get_sim();
	if (challenge_sim.is_valid() && !wire_header_join) {
		if (challenge_sim->is_joiner()) {
			player_visuals_->prewarm_loaded_model_challenge_definitions();
		}
		challenge_sim->finalize_loaded_model_challenge_snapshot();
	}
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_AUDIO));
	pump_join_load_step();
	timeline->span("audio");
	start_mission_audio(p_mission, p_bms_name);
	timeline->end_span();
	const bool joiner_start = challenge_sim.is_valid() && challenge_sim->is_joiner();
	// A joiner's mission start initializes every streamed row from its model, then queues
	// its first 0x2F: its sound banks, the entity models' reload and the rows' init all
	// run ahead of the first submission, and the model freeze after it (engine:
	// inmatch::JoinerRole::MissionStartLegs carries the witness, D-NET-NEW-L2-42).
	// The reload's pool-0 leg loads and registers each person row's animation map here, in
	// the load, not on the first frame past it (engine: inmatch::JoinerRole::
	// reload_pool0_row_channels, D-NET-NEW-L2-56).
	if (joiner_start) {
		timeline->span("pool0_channels");
		challenge_sim->reload_join_pool0_row_channels();
		timeline->end_span();
	}
	bool wire_world_placed = false;
	if (joiner_start && wire_header_join) {
		timeline->span("streamed_world");
		wire_world_placed = place_join_wire_world(challenge_sim, timeline);
		timeline->end_span();
	}
	const bool dedicated_host = challenge_sim.is_valid() &&
			challenge_sim->session_role() == Simulation::ROLE_DEDICATED_HOST;
	// The celestial bodies' models past the rows' init, ahead of the HUD's overlay init, on every
	// arm (a joiner's inside its S2C 0x1A load, not its 0x11 head); their draw objects on the full
	// arm alone (engine environment/celestial_frame.h builds_celestial_draws carries the witness,
	// D-NET-NEW-L3-38).
	timeline->span("celestial");
	scene_.load_celestial_models(resource_root_,
			opennova::env::builds_celestial_draws(dedicated_host));
	timeline->end_span();
	// The HUD's overlay init, on every mission start and every role: past the rows' init
	// and ahead of a joiner's first 0x2F. The shell's HUD builds here, its layout and its
	// map-grid origin scan, not on the first frame past the load (engine
	// mission/mission_load_plan.h carries the witness, D-NET-NEW-L8-40).
	timeline->span("hud_init");
	emit_signal(kSignalHudOverlayInit, p_bms_name);
	timeline->end_span();
	if (joiner_start) {
		// The definitions and the world are in: the mission start's first leg queues the first
		// 0x2F and runs the pumps ahead of the effect builds and the freeze (engine:
		// ClientRuntime::push_mission_start_first_loadout, D-NET-NEW-L2-35).
		timeline->span("first_leg");
		challenge_sim->push_join_mission_start_first_loadout();
		timeline->end_span();
		// A wire-header joiner's C2S 0x3D page freezes here, its streamed world in, after the
		// first leg as the freeze follows the first 0x2F; the mission textures stage's freeze
		// below resolves its models' materials, as a host's does (D-NET-527, D-NET-NEW-L2-42,
		// D-NET-NEW-L3-31).
		if (wire_world_placed) {
			timeline->span("model_list");
			player_visuals_->prewarm_loaded_model_challenge_definitions();
			challenge_sim->finalize_loaded_model_challenge_snapshot();
			timeline->end_span();
		}
	}
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_EFFECTS));
	pump_join_load_step();
	timeline->span("effects");
	// A join's mission-data load built and warmed the effect world ahead of its 0x09; the
	// rest binds it to this mission (load_join_effect_system).
	const bool effects_preloaded = ObjectDB::get_instance(join_effect_world_id_) != nullptr;
	start_effect_world();
	timeline->end_span();
	// A host's render arm: its load window's pumps ahead of the mission's textures, which the
	// warm below stands for (engine HostRole::render_arm_pumps carries the witness).
	if (challenge_sim.is_valid()) challenge_sim->pump_host_render_arm_ahead_of_textures();
	// Warm the effect catalog while the loading screen still covers the frame:
	// the first live spawn otherwise pays the deferred texture resolves + the
	// renderer's first-draw pipeline compiles as a ~90 ms hitch on the player's
	// first shot (measured: first-fire tap 92.9 ms -> repeat 12.5 ms). Retail
	// pays this at load (the load plan's effect-system witness).
	emit_signal(kSignalLoadProgress,
			MissionData::load_progress_percent(MissionData::LOAD_STAGE_EFFECTS_WARM));
	pump_join_load_step();
	// The mission textures stage opens with the terrain's, on every arm, a dedicated host's reduced
	// one too: the texture slots and the tile set's atlas the head's connect leg held, then the
	// terrain's build (engine terrain/terrain_load_legs.h carries the witness, D-NET-NEW-L2-46).
	// The terrain's LOD cache, its tile cache's page targets and the foliage's definitions build
	// with the render subsystems, which a dedicated host's start jumps over (the same header's
	// builds_lod_cache, builds_tile_cache_targets and loads_foliage_definitions, D-NET-NEW-L3-32,
	// D-NET-NEW-L3-33, D-NET-NEW-LF-3).
	load_terrain_textures(timeline, dedicated_host);
	load_sky_maps(timeline, dedicated_host);
	// Then the freeze, on every arm: the listed models' materials resolve and no model builds, a
	// wire-header joiner's list too (it froze above, its world stream landed). The full arm's
	// freeze is the reduced arm's, with the render arm's device inits after it (engine
	// inmatch/loaded_model_list.h LoadedModelList::model_files carries the witness,
	// D-NET-NEW-L3-25, D-NET-NEW-L3-31).
	// Each listed file's resolve presents the loading screen at its stored checkpoint, the
	// stage's start (MissionObjectPlacer::resolve_model_materials carries the witness).
	timeline->span("model_materials");
	if (placer_.is_valid() && challenge_sim.is_valid()) {
		placer_->resolve_model_materials(challenge_sim->get_mission_start_model_files(),
				callable_mp(this, &GameWorld::pulse_load_progress)
						.bind(MissionData::load_progress_percent(MissionData::LOAD_STAGE_EFFECTS_WARM)));
	}
	timeline->end_span();
	// The HUD's textures and fonts at the mission textures stage, past the freeze: the full
	// arm loads the HUD's textures and their materials there, and every arm, a dedicated
	// host's reduced one too, then loads the HUD's fonts and the stdbox frame the status page
	// draws, with the connection icons both arms load ahead of the split. The shell's HUD
	// loads them here, inside the load, by the arm (engine mission/mission_load_plan.h carries
	// the witness, D-NET-NEW-L8-40).
	timeline->span("hud_textures");
	emit_signal(kSignalHudTexturesLoad, !dedicated_host);
	timeline->end_span();
	// The view effects', the tracer pool's and FrameFX's device objects past the HUD's textures,
	// FrameFX's "ffscan" scanlines with its pixel shaders: the full arm's, none on a dedicated
	// host's reduced arm (load_frame_fx_textures carries the witnesses, D-NET-NEW-L3-35,
	// D-NET-NEW-LF-13).
	load_frame_fx_textures(timeline, dedicated_host);
	if (!dedicated_host) {
		// The render arm's device inits past the freeze: the effect catalog and the presenters'
		// fire pipelines warm in the forced frames; a dedicated host's reduced arm runs none (no
		// effect spawn, no pipeline warm, no forced frame).
		timeline->span("effects_warm");
		warm_effect_world_catalog(!effects_preloaded, timeline);
		timeline->end_span();
	}
	// A host's mission start past its device stages: phase B opens with the render arm's
	// last pump, then the PreMission pass, the round init and Player_InitPlayer, as retail's
	// start runs them past its mission textures (engine inmatch/host_boot.h start_host_mission
	// carries the witness).
	if (challenge_sim.is_valid() && challenge_sim->host_mission_start_deferred()) {
		timeline->span("mission_start");
		run_mission_start();
		timeline->end_span();
	}
	timeline->span("script_effects");
	route_script_effects(); // the start's WAC/BMS descriptors, past the warm-scene reset
	timeline->end_span();
	// The load end past the mission start: a host's S2C 0x7B and the spawn gate's clear
	// (engine inmatch::end_host_mission_load).
	timeline->span("load_end");
	if (challenge_sim.is_valid()) challenge_sim->end_host_mission_load();
	timeline->end_span();
	// The tail's legs are spans too (an instrument): the world-ready edges and their
	// listeners run inside the load's call.
	timeline->span("finish");
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_FINISH));
	pump_join_load_step();
	timeline->end_span();
	loaded_mission_file_ = p_bms_name;
	world_ready_ = true;
	// The water's world rendering opens at the load's end; its minimap mask was made with the
	// terrain's textures (load_terrain_textures).
	timeline->span("water");
	set_water_world_rendering_enabled(true);
	timeline->end_span();
	timeline->span("world_loaded");
	emit_signal(kSignalLoadProgress, MissionData::LOAD_PROGRESS_WORLD_READY);
	emit_signal(kSignalWorldLoaded);
	timeline->end_span();
	timeline->finish();
	// The first frames past the load, timed on a timeline of their own (an instrument): a host's
	// S2C 0x7B leaves at the first open send boundary of their updates (engine
	// HostRole::finish_mission_load), and the mission start's first frames bank no render time
	// (engine inmatch::Session::complete_load), so their draws are the load's tail on the wire.
	begin_post_load_frames(p_bms_name);
	return OK;
}

// A join's S2C 0x11 load (SessionDrive::step_preload): the mission start's head and its terrain
// from the host's 0x0B header, ahead of the game-start wait's 0x0A; its S2C 0x1A load
// (load_mission_internal) goes on from the objects. Every mission start of a join runs it, a
// reload's too, whose previous world the shell unloaded first (engine
// inmatch/joiner_connection.h set_sync_tail_loading carries the witness). Emits load_failed and
// returns the error on a failure.
int GameWorld::load_join_terrain(const Ref<MissionData> &p_mission, const String &p_bms_name,
		const Ref<ResourceRoot> &p_resource_root) {
	join_terrain_preloaded_ = false;
	copy_mission_start_options();
	// The load's steps run the session's periodic update ahead of the terrain and past it.
	pump_join_load_step();
	Ref<LoadTimeline> timeline;
	const int err = load_mission_head(p_mission, p_bms_name, p_resource_root, timeline);
	if (err != OK) return err;
	pump_join_load_step();
	// Then the avatar combos' models, at the header's climate: their data loads into the
	// placer the S2C 0x1A's load keeps, and that load's freeze resolves their materials with the
	// rest of the list (engine: inmatch/loaded_model_list.h avatar_combo_models, D-NET-NEW-L2-19,
	// D-NET-NEW-L3-31).
	timeline->span("avatars");
	placer_ = MissionObjectPlacer::create(resource_root_, Ref<ItemDatabase>());
	const opennova::mission::BootFileSource files =
			opennova::mission::boot_files_from_index(p_resource_root->native_index());
	const auto climate = static_cast<opennova::bms::ClimateType>(p_mission->get_info()->get_climate());
	for (const std::string &name : opennova::inmatch::avatar_combo_models(&files, climate)) {
		const String file = opennova::to_gd(opennova::inmatch::LoadedModelList::node_name(name));
		if (p_resource_root->has_file(file)) placer_->object_data_for(file);
	}
	timeline->end_span();
	timeline->finish();
	join_terrain_preloaded_ = true;
	return OK;
}

// The mission start's copies of the options, which its effect system and its head build at.
void GameWorld::copy_mission_start_options() {
	// The mission start's copy of the options' object detail: the frames draw
	// at it until the next mission start (engine: renderer/object_lod.h).
	object_detail_ = object_polydetail_;
	// The same copy of the texfilter level: the device mode the terrain detail
	// family samples at follows it until the next mission start (engine:
	// renderer/texture_filter.h).
	session_texfilter_level_ = texfilter_level_;
	publish_texfilter_state();
	// The same copy of texcompression_level and particle_density: the terrain
	// and the particle pages this mission builds, and the scene passes' particle
	// stride, follow them until the next mission start (engine:
	// renderer/texture_compression.h, renderer/particle_density.h).
	session_texcompression_level_ = texcompression_level_;
	session_terrain_texdetail_ = terrain_texdetail_;
	session_particle_density_ = particle_density_;
	publish_session_render_settings();
	// The same copy of NoBlood's ragdoll bit: this mission's corpses ragdoll
	// or keep their clips until the next mission start (engine:
	// ragdoll/ragdoll_option.h).
	ragdolls_.set_enabled(game_config_.is_valid() &&
			opennova::ragdoll::ragdolls_enabled(game_config_->native().cfg().no_blood));
}

// The mission start's head through its terrain, past the start's copies of the options
// (copy_mission_start_options): the terrain's gate, the tile info, the environment (the
// mission's overrides, its clock, the weather owner) and the terrain. Emits load_failed and
// returns the error on a failure; `r_timeline` is the load's timeline, begun here from the tile
// info on when the caller began none.
int GameWorld::load_mission_head(const Ref<MissionData> &p_mission, const String &p_bms_name,
		const Ref<ResourceRoot> &p_resource_root, Ref<LoadTimeline> &r_timeline) {
	const bool wire_header_join = p_mission->is_wire_header_only();
	join_wire_assets_pending_ = wire_header_join;
	join_wire_til_applied_ = false;
	join_wire_assets_failed_ = false;
	join_wire_asset_failure_emitted_ = false;
	// The local-asset gate holds for wire-header joins too — there is no world
	// without terrain, and retail joiners also resolve it from the local
	// install by the name the wire supplies (net-re section 5.28: custom
	// missions reference stock assets). Only the REPORT is join-aware: a wire
	// join names the host's stream and the mounted expansion so a live punt
	// reads as "your install lacks X", not as a bad local file.
	// The document's references are basenames from either header source (the
	// wire 0x0B header's extension is dropped by mission_info()).
	const String trn = p_mission->get_terrain_ref() + ".trn";
	if (!p_resource_root->has_file(trn)) {
		emit_signal(kSignalLoadFailed,
				missing_mission_asset_reason(trn, p_bms_name, p_resource_root, wire_header_join));
		if (r_timeline.is_valid()) r_timeline->finish();
		return ERR_FILE_NOT_FOUND;
	}
	// The environment gates nothing: a .env that is not there is skipped and the
	// mission starts on the engine's defaults (load_environment).
	const String env_name = p_mission->get_environment_ref() + ".env";

	set_weather_world_tick_driven(true);
	set_water_world_rendering_enabled(false);
	// Keep stage attribution stable so load timelines remain comparable.
	if (r_timeline.is_null()) {
		r_timeline = LoadTimeline::begin(vformat("Mission load %s", p_bms_name));
		last_load_timeline_ = r_timeline;
	}
	resource_root_ = p_resource_root;
	load_mission_tile_info(p_bms_name, p_mission, p_resource_root, PackedByteArray(),
			p_mission->is_wire_header_only());
	// Each stage below presents the plan's anchor when it starts.
	emit_signal(kSignalLoadProgress,
			MissionData::load_progress_percent(MissionData::LOAD_STAGE_ENVIRONMENT));
	r_timeline->span("environment");
	load_environment(trn, env_name);
	apply_mission_environment_overrides(p_mission);
	// Initialize the exact mission clock and the reset weather owner before the
	// runtime is constructed. The authority publishes this T0 sample after setup
	// but before play, so its first network tick cannot observe stale/default data.
	Ref<MissionInfo> mission_info = p_mission->get_info();
	mission_clock_start_q8_8_ = mission_info->get_start_time();
	mission_clock_minutes_per_day_ = mission_info->get_minutes_per_day();
	scene_.configure_mission_clock(mission_clock_start_q8_8_, mission_clock_minutes_per_day_);
	prepare_world_driven_weather();
	r_timeline->end_span();
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_TERRAIN));
	r_timeline->span("terrain");
	if (!load_terrain(trn, p_mission->get_tile_set_ref(), p_mission->get_environment_ref())) {
		emit_signal(kSignalLoadFailed, vformat("failed to load %s", trn));
		r_timeline->finish();
		return ERR_CANT_OPEN;
	}
	r_timeline->end_span();
	return OK;
}

// The per-mission subtree (ADR 0043 d9): MissionRoot owns the placed
// MissionObjects container the placer fills next, the Entities presenter
// and, after start_runtime, the sim. It exists before placement so the
// container parents under it; unload() frees the container's nodes BEFORE
// the root's own teardown legs, exactly the order the world-parented
// container had.
void GameWorld::start_mission_root() {
	// A same-frame reload (a test's unload + load) still finds the previous
	// root queued for deletion under the world: queue_free is deferred to the
	// frame flush. Detach it now so the new root owns the "MissionRoot" name
	// and the directors' MissionRoot/MissionObjects lookups resolve the live
	// subtree, never the dying one. Its own teardown runs on that detach; the
	// shipped game reloads across frames and never reaches this leg.
	Node *stale = get_node_or_null(NodePath("MissionRoot"));
	if (stale != nullptr) {
		remove_child(stale);
	}
	MissionRoot *runtime = memnew(MissionRoot);
	runtime->set_name("MissionRoot");
	add_child(runtime);
	runtime_id_ = ObjectID(runtime->get_instance_id());
}

void GameWorld::pulse_load_progress(int p_percent) {
	emit_signal(kSignalLoadProgress, p_percent);
}

// Each loading step's present is followed by the session's periodic update, which a join's
// simulation runs (engine inmatch::JoinerRole::pump_mission_start_periodic_update carries the
// witness, D-NET-NEW-L2-54): the drive holds that simulation until the runtime adopts it. The
// join's update runs the NovaWorld session's own half past its manager receive; any other load (a
// NovaWorld host's) runs that half alone, the process's held session's pump (engine:
// inmatch::run_loading_step, D-NET-NEW-L9-25).
void GameWorld::pump_join_load_step() {
	Ref<Simulation> sim = get_sim();
	if (sim.is_null()) sim = drive_.join_preload_simulation();
	opennova::inmatch::run_loading_step(sim.is_valid() ? sim->loading_step_joiner() : nullptr,
			client_process_);
}

// Populate the world with the mission's placed objects under the mission
// root's MissionObjects node. Uses the same shell-agnostic placer as every
// other mission load path.
void GameWorld::place_mission_objects(const Ref<MissionData> &p_mission) {
	if (resource_root_.is_null() || p_mission.is_null()) {
		return;
	}
	if (placer_.is_null()) placer_ = MissionObjectPlacer::create(resource_root_, Ref<ItemDatabase>());
	// The placer's Avatars.def is the one registry every player visual resolves
	// against; the same table projects the local profile the sim stamps.
	Ref<AvatarDatabase> avatar_db = placer_->get_avatar_db();
	Ref<Simulation> sim = get_sim();
	if (sim.is_valid()) {
		sim->set_character_avatar_database(avatar_db);
	}
	if (avatar_db.is_null()) {
		UtilityFunctions::push_warning("GameWorld: Avatars.def unavailable; players draw their item model");
		if (!resource_root_->has_file("Avatars.def")) {
			// The log line the editor's Play reads back into a Problems row (ADR 0046 DI-27).
			ResourceRoot::report_missing(opennova::gameprofile::resource_kind::kFile, "Avatars.def", String(),
					"players draw their item model");
		}
	}
	project_local_character_profile(avatar_db);
	panm_clock_->sample(get_frame_clock_ms(),
			static_cast<int64_t>(Engine::get_singleton()->get_process_frames()));
	placer_->set_panm_clock(panm_clock_);
	Dictionary options;
	// A wire-header join deliberately has no authored body records. The load stream
	// creates native pools 2/1/3 from S2C 0x10/0x0D/0x20 at exact handles; remote
	// pool-0 organics arrive in 0x0C and every live pose advances through 0x0A.
	// MissionRoot presents those decoded rows directly instead of deferring them
	// onto nonexistent local BMS placements (D-NET-194). Explicit-mission/debug
	// joins still use their complete document.
	// Pulse the load-progress screen from inside the model-load loop at the
	// object stage's constant value (the load plan's per-model pulse witness).
	options["progress"] = callable_mp(this, &GameWorld::pulse_load_progress)
			.bind(MissionData::load_progress_percent(MissionData::LOAD_STAGE_OBJECTS));
	// The placement's model builds split by phase (ObjectModel::take_build_profile), from here.
	(void)ObjectModel::take_build_profile();
	// A dedicated host's load builds no placed entity's draw object: the placement runs with no
	// parent, an empty census (engine mission::builds_placement_draws carries the witness,
	// D-NET-NEW-L3-30). The sim's entities and their collision come from the mission and the
	// mount, not from these nodes.
	const bool draws = opennova::mission::builds_placement_draws(drive_.pending_dedicated());
	mission_stats_ = placer_->place(p_mission, draws ? get_runtime() : nullptr, options);
	const PackedInt64Array build_profile = ObjectModel::take_build_profile();
	apply_occlusion_culling_policy();
	// Static tile shadows are composed from the placer's resolved ObjectData and
	// exact entity transforms. Attach only after place() has finished building
	// that immutable mission snapshot; Terrain invalidates any pre-placement
	// cache pages when the producer becomes live.
	if (terrain_ != nullptr) {
		terrain_->set_static_shadow_placer(placer_);
	}
	UtilityFunctions::print_verbose(vformat(
			"GameWorld: placed %d mission objects (%d batched / %d animated, %d unresolved, %d markers)",
			mission_stats_->get_placed(), mission_stats_->get_batched(), mission_stats_->get_animated(),
			mission_stats_->get_unresolved(), mission_stats_->get_markers()));
	// The placement run's own unit spans (an instrument), as the streamed placement prints them.
	UtilityFunctions::print_verbose(vformat(
			"GameWorld: placement units: bucket %d ms, statics %d ms, models %d ms (%d graphics, %d batches, %d static shadow batches)",
			static_cast<int>(mission_stats_->get_span_bucket_entities_usec() / 1000),
			static_cast<int>(mission_stats_->get_span_static_batches_usec() / 1000),
			static_cast<int>(mission_stats_->get_span_animated_models_usec() / 1000),
			mission_stats_->get_graphics(), mission_stats_->get_batches(),
			mission_stats_->get_static_shadow_batches()));
	UtilityFunctions::print_verbose(String("GameWorld: placement scene builds: ") +
			ObjectModel::format_build_profile(build_profile));
}

// Tear down a loaded world so the shell can return to the menu (or load a
// different mission) without the previous world lingering. Frees the
// dynamically placed MissionObjects subtree and resets the load state; the
// terrain / environment scene nodes are kept in place and rebuilt by the next
// load_*(). Safe to call when nothing is loaded.
void GameWorld::unload() {
	world_ready_ = false;
	// The next mission's first sampled frame only stamps the frame clock: retail's
	// mission start is the game mode's initialize, after which the main loop re-reads
	// its clock, so no leg sees the time since this mission's last frame (engine
	// world::TickAccumulator::bank carries the witness; D-NET-NEW-LI-6).
	frame_clock_ms_ = 0;
	// A session the load's start created for a runtime that never came up goes with it.
	staged_runtime_options_.unref();
	// A post-load timeline still timing its frames prints what it has (an instrument).
	end_post_load_frames();
	ragdolls_.clear();
	scene_overlay_bodies_.clear();
	minimap_water_mask_.unref();
	emit_signal(kSignalMinimapWaterChanged, Variant());
	join_wire_assets_pending_ = false;
	join_wire_til_applied_ = false;
	join_wire_assets_failed_ = false;
	join_wire_asset_failure_emitted_ = false;
	join_terrain_preloaded_ = false;
	terrain_textures_pending_ = false;
	stop_water_render_stats();
	// Net-session teardown: the preload sim/root, the notification latches, the
	// typed request staging, and the NovaWorld gate registration.
	drive_.reset();
	// The environment's weather view points into the departing sim's World:
	// detach before the runtime (and the sim it owns) is freed.
	if (weather_ != nullptr) {
		weather_->bind_simulation(nullptr);
	}
	set_weather_world_tick_driven(true);
	// The water goes dormant with its mission (its strips and its mirror's target with it), so no
	// loading present ahead of the next load's head draws the mirror: a present draws the screen
	// alone (engine hud/loading_screen.h carries the witness, D-NET-NEW-L3-37).
	set_water_world_rendering_enabled(false);
	if (env_ != nullptr) {
		env_->set_underwater_view(false);
		env_->set_underwater_overlay_view(false);
	}
	local_character_profile_.unref();
	clear_mission_tile_info();
	restore_idle_frame_clear_color();
	// Point-light output is a RenderingServer global, so retire it before the
	// placed nodes begin their deferred queue_free teardown. The director also
	// disconnects its wire-node exit hooks here; those hooks must not race the
	// whole-world reset or leak a prior mission's pool into the menu frame.
	if (light_director_.is_valid()) {
		light_director_->reset();
	}
	// Release Terrain's Ref before the MissionObjects nodes and owning placer.
	// This also invalidates pages composed with the departing caster snapshot.
	if (terrain_ != nullptr) {
		terrain_->set_static_shadow_placer(Ref<MissionObjectPlacer>());
	}
	apply_occlusion_culling_policy();
	// The MissionObjects container is the MissionRoot's child (ADR 0043 d9) and
	// is queued for deletion FIRST, before the root's own queue_free below: the
	// delete queue keeps that order, so the container's nodes (the wire bodies,
	// husk grafts, throwable models, scar meshes) are gone before
	// MissionRoot._exit_tree runs EntityPresenter.teardown -- exactly as when the
	// container was the world's own child. A root that never reached
	// start_runtime has no sim; its container still frees the same way.
	MissionRoot *runtime = get_runtime();
	Node *container = runtime != nullptr ? runtime->get_node_or_null(NodePath("MissionObjects")) : nullptr;
	if (container != nullptr) {
		container->queue_free();
	}
	// Per-item attached-effect owner keys reference nodes in that container —
	// never let a reload's provider resolve against freed instances.
	item_fx_->reset();
	MissionAudio *mission_audio = get_mission_audio();
	if (mission_audio != nullptr) {
		mission_audio->teardown();
		mission_audio->queue_free(); // the node is the audio root; the world owned it
	}
	// Tear down the game music context [orig: AudioVM_StopMusicContext @ 0x671e00].
	// The game shell re-opens menu music on its return to the front end
	// (MainGame connects this to MusicService.stop_context).
	emit_signal(kSignalMusicContextClosed);
	// Blink frame gates and every occlusion override reset with the mission
	// [orig: the letter-bit clear @ 0x525c45 at mission start] — an unload while
	// indoors must not leave the next mission's terrain/sky/water hidden. The
	// release clears the occlusion-hidden bit, so every node lands on its own
	// present intent — see OcclusionFrame.reset.
	occlusion_->reset();
	player_visuals_->set_local_player_nvg_view(false, 0);
	if (env_ != nullptr && env_->get_environment_data().is_valid()) {
		env_->get_environment_data()->clear_mission_overrides();
	}
	set_mission_water_height_override(NAN);
	loaded_mission_.unref();
	loaded_mission_file_ = "";
	if (runtime != nullptr) {
		runtime->queue_free(); // drops its sim too (MissionRoot._exit_tree, after the container above)
	}
	runtime_id_ = ObjectID();
	EffectWorld *effect_world = get_effect_world();
	if (effect_world != nullptr) {
		effect_world->release_runtime_renderer_resources();
		effect_world->queue_free();
	}
	effect_world_id_ = ObjectID();
	free_join_effect_world();
	free_head_effect_world();
	// The teardown destroys the mission's device objects and keeps the archives open: the
	// mount's decoded textures go, its index and parsed assets stay for the next load on it
	// (engine inmatch/map_change.h carries the witness, D-NET-NEW-L3-24).
	if (resource_root_.is_valid()) resource_root_->release_decoded_textures();
	mission_audio_id_ = ObjectID();
	placer_.unref();
	weapon_db_.unref(); // re-resolves against the next load's mounted root
	// The staged spawn loadout and every equipped-weapon memo drop with the
	// mission (LocalPlayerVisuals.reset carries the memo/override rules).
	player_visuals_->reset();
	mission_stats_.unref();
}

// --- environment ---------------------------------------------------------------------

void GameWorld::load_environment(const String &p_trn_path, const String &p_env_path) {
	if (env_ == nullptr) {
		return;
	}
	// The load and its post-steps are the scene's (world/world_scene.h); the
	// game words what it found.
	const WorldEnvironmentLoad loaded =
			scene_.load_environment(resource_root_, p_trn_path, p_env_path);
	if (!loaded.loaded) {
		UtilityFunctions::push_warning(vformat(
				"GameWorld: environment '%s' did not load; the engine defaults stand", p_env_path));
	}
	// One that is not there (one that does not parse is no miss): the log line the editor's Play
	// reads back into a Problems row on the mission naming it (ADR 0046 DI-27).
	if (loaded.environment_missing) {
		ResourceRoot::report_missing(opennova::gameprofile::resource_kind::kEnvironment,
				p_env_path.get_file(), String(),
				"the mission runs on the engine's default environment");
	}
	// The engine's env::kOvercastFile carries the name and the witness.
	if (loaded.overcast_missing) {
		ResourceRoot::report_missing(opennova::gameprofile::resource_kind::kFile, opennova::env::kOvercastFile, String(),
				"the overcast weather has no table of its own to blend toward");
	}
}

// Apply the mission's attrib-gated water/fog overrides onto the loaded env via
// EnvFile's non-persistent override layer (engine: formats/env/env.h). The base .env is never mutated.
void GameWorld::apply_mission_environment_overrides(const Ref<MissionData> &p_mission) {
	if (p_mission.is_null()) {
		return;
	}
	scene_.apply_mission_overrides(p_mission->get_environment_overrides());
}

void GameWorld::set_mission_water_height_override(float p_world_height) {
	scene_.set_mission_water_height_override(p_world_height);
}

void GameWorld::set_water_world_rendering_enabled(bool p_enabled) {
	if (water_ != nullptr) {
		water_->set_world_rendering_enabled(p_enabled);
	}
}

void GameWorld::set_weather_world_tick_driven(bool p_enabled) {
	if (weather_ != nullptr) {
		weather_->set_world_tick_driven(p_enabled);
	}
}

void GameWorld::prepare_world_driven_weather() {
	scene_.prepare_world_driven_weather();
}

void GameWorld::prepare_autonomous_weather() {
	scene_.prepare_autonomous_weather();
}

// The witnessed mission-start environment boundary, phase B of the one host
// boot, runs natively on the weather device (Weather.run_mission_start_boundary):
// the World's weather seed from the loaded .env + the BMS clock, the
// authority's WAC direct execution, the initializer + 255-tick settle, the
// baseline seal, then the session's load end. A world with no weather device
// runs it on the boot's own environment, as the headless host does.
void GameWorld::run_mission_start_environment_boundary() {
	Ref<Simulation> sim = get_sim();
	if (weather_ != nullptr) {
		weather_->run_mission_start_boundary(sim.ptr());
	} else if (sim.is_valid()) {
		(void)sim->start_mission(nullptr, nullptr, {});
	}
}

// --- the tile info + join-wire assets ------------------------------------------------

// Retail loads <mission>.til (else the terrain's polytrn_tileinfo) into one
// shared g_TerrainTileArray used by terrain overlays/surface overrides, network
// initial state, and both foliage generators' radius-2 blocker. The file rule
// is the engine's (mission::read_placed_tiles carries its witness).
// [orig: Terrain_LoadTileInfoFile @ 0x60a740, policy force @ 0x60a74e;
// Terrain_GetSurfaceTypeAtPosition @ 0x606510;
// Foliage_PathBlockedByPlacedTile @ 0x606490]
void GameWorld::load_mission_tile_info(const String &p_bms_name, const Ref<MissionData> &p_mission,
		const Ref<ResourceRoot> &p_resource_root, const PackedByteArray &p_wire_til_bytes,
		bool p_wire_is_authoritative) {
	clear_mission_tile_info();
	if (p_resource_root.is_null()) {
		return;
	}
	// A joining retail client consumes the host's paged S2C 0x45 bytes. An empty
	// payload means the host emitted no terrain overlay; it must not fall back to
	// a same-named local .til and accidentally render a different custom map.
	if (p_wire_is_authoritative) {
		if (p_wire_til_bytes.is_empty()) {
			return;
		}
		Ref<TerrainTileInfo> wire_tile_info;
		wire_tile_info.instantiate();
		if (wire_tile_info->load_from_bytes(p_wire_til_bytes) != OK) {
			UtilityFunctions::push_warning("GameWorld: failed to parse host S2C 0x45 terrain tile stream.");
			return;
		}
		mission_tile_info_ = wire_tile_info;
		mission_til_bytes_ = p_wire_til_bytes;
		return;
	}
	if (p_mission.is_null()) {
		return;
	}
	const opennova::mission::MissionInfo info =
			opennova::mission::mission_info(p_mission->native_file());
	MissionTilesRead read;
	const Ref<TerrainTileInfo> tile_info = read_mission_tiles(
			p_resource_root, opennova::to_std(p_bms_name), info.terrain, info.environment, &read);
	if (read.file.empty()) {
		return;
	}
	if (tile_info.is_null()) {
		UtilityFunctions::push_warning(vformat(
				"GameWorld: failed to parse mission tile file '%s'.", opennova::to_gd(read.file)));
		return;
	}
	mission_tile_info_ = tile_info;
	mission_til_bytes_ = read.bytes;
}

bool GameWorld::apply_join_wire_til_if_ready() {
	if (!join_wire_assets_pending_) {
		return !join_wire_assets_failed_;
	}
	Ref<Simulation> sim = get_sim();
	if (sim.is_null() || !sim->is_joiner()) {
		return true;
	}
	const int64_t til_state = sim->get_join_terrain_til_state();
	if (til_state == Simulation::JOIN_TERRAIN_TIL_INVALID) {
		join_wire_assets_failed_ = true;
		return false;
	}
	if (til_state != Simulation::JOIN_TERRAIN_TIL_ABSENT && til_state != Simulation::JOIN_TERRAIN_TIL_RECEIVING &&
			til_state != Simulation::JOIN_TERRAIN_TIL_COMPLETE) {
		join_wire_assets_failed_ = true;
		return false;
	}
	// Check Invalid before this latch: an extra semantic 0x45 after a completed
	// stream must not be hidden by an already-applied terrain override.
	if (join_wire_til_applied_) {
		return true;
	}
	if (til_state == Simulation::JOIN_TERRAIN_TIL_ABSENT || til_state == Simulation::JOIN_TERRAIN_TIL_RECEIVING) {
		return true;
	}
	const PackedByteArray til_bytes = sim->get_join_terrain_til();
	if (til_bytes.is_empty()) {
		join_wire_assets_failed_ = true;
		return false;
	}
	Ref<TerrainTileInfo> tile_info;
	tile_info.instantiate();
	if (tile_info->load_from_bytes(til_bytes) != OK) {
		join_wire_assets_failed_ = true;
		return false;
	}
	mission_tile_info_ = tile_info;
	mission_til_bytes_ = til_bytes;
	join_wire_til_applied_ = true;
	// S2C 0x45 arrives only after the client releases the world-ready gate, so
	// terrain already exists. Both setters invalidate/rebuild their derived data;
	// the loading screen remains raised through the mission start's load.
	if (terrain_ != nullptr) {
		terrain_->set_tile_info_override(tile_info);
	}
	if (dispatcher_ != nullptr) {
		dispatcher_->set_tile_info(tile_info);
	}
	build_minimap_water_mask();
	return true;
}

// A wire-header joiner's streamed world at its mission start's load, ahead of its first
// submission (load_mission_internal carries the witness): the S2C 0x45 overlay applied, the
// stream landed in the World with its statics' placed identity, and the statics placed. False on
// a failure, which latches join_wire_assets_failed_ for the settle to report.
bool GameWorld::place_join_wire_world(const Ref<Simulation> &p_sim, const Ref<LoadTimeline> &p_timeline) {
	if (!join_wire_assets_pending_) {
		return !join_wire_assets_failed_;
	}
	p_timeline->span("til");
	const bool til_applied = apply_join_wire_til_if_ready();
	p_timeline->end_span();
	if (!til_applied) {
		return false;
	}
	const int64_t til_state = p_sim->get_join_terrain_til_state();
	if (til_state == Simulation::JOIN_TERRAIN_TIL_RECEIVING || til_state == Simulation::JOIN_TERRAIN_TIL_INVALID ||
			(til_state == Simulation::JOIN_TERRAIN_TIL_COMPLETE && !join_wire_til_applied_) ||
			(til_state != Simulation::JOIN_TERRAIN_TIL_ABSENT && til_state != Simulation::JOIN_TERRAIN_TIL_COMPLETE)) {
		join_wire_assets_failed_ = true;
		return false;
	}
	// The stream lands in the World and its statics take their placed identity first: the
	// load runs in the mission start's call, ahead of any session update, whose frame phase
	// does it (engine: inmatch::JoinerRole::materialize_world_stream, D-NET-NEW-L2-38).
	p_timeline->span("materialize");
	p_sim->materialize_join_world_stream();
	p_timeline->end_span();
	place_streamed_mission_objects(p_sim, p_timeline);
	return true;
}

// Complete the wire-only part of a retail join once the protocol reaches its
// deployment/admission boundary: the load placed the streamed world, froze the C2S 0x3D page
// (inmatch/loaded_model_list.h) over the completed initial stream and warmed its models
// (load_mission_internal); SessionDrive calls this before revealing the world (or deploy map)
// and reports a failure the load latched.
bool GameWorld::settle_join_wire_assets() {
	if (join_wire_assets_failed_) {
		return false;
	}
	if (!join_wire_assets_pending_) {
		return true;
	}
	Ref<Simulation> sim = get_sim();
	if (sim.is_null() || !sim->is_joiner()) {
		join_wire_assets_failed_ = true;
		return false;
	}
	join_wire_assets_pending_ = false;
	return true;
}

// A header-only joiner owns no authored body records, so its pools 1-3
// arrive as the host's S2C 0x10/0x0D/0x20 world stream and are materialized
// into the native World at their exact wire handles. Once that stream's
// static pools are complete the sim stamps each row with a placed identity;
// this places them through the SAME MissionObjectPlacer path single player
// and the host use (batched static populations, terrain static shadows,
// occlusion keying, the per-entity sun query) and re-keys the presenters'
// index so the wire pass stops drawing them as individual animated nodes.
// Retail's client draws its streamed pools through the same sector renderer
// as the host; there is no per-role render path.
void GameWorld::place_streamed_mission_objects(const Ref<Simulation> &p_sim,
		const Ref<LoadTimeline> &p_timeline) {
	if (placer_.is_null() || p_sim.is_null()) {
		UtilityFunctions::print_verbose(vformat("GameWorld: streamed objects not placed (placer %s)",
				placer_.is_null() ? "none" : "kept"));
		return;
	}
	// The legs as nested spans of the caller's timeline (an instrument).
	const auto leg = [&p_timeline](const char *p_name) {
		if (p_timeline.is_valid()) p_timeline->span(p_name);
	};
	const auto leg_end = [&p_timeline]() {
		if (p_timeline.is_valid()) p_timeline->end_span();
	};
	leg("records");
	const Array records = p_sim->get_streamed_placement_records();
	leg_end();
	if (records.is_empty()) {
		UtilityFunctions::print_verbose("GameWorld: streamed objects not placed (no placement records)");
		return;
	}
	Dictionary options;
	Array skip_kinds;
	skip_kinds.push_back(MissionData::KIND_ORGANIC);
	options["skip_kinds"] = skip_kinds;
	MissionRoot *runtime = get_runtime();
	leg("place");
	mission_stats_ = placer_->place_entities(records, runtime, options);
	leg_end();
	leg("rebind");
	if (runtime != nullptr) {
		runtime->rebind_placed_entities(placer_);
	}
	leg_end();
	leg("occlusion");
	occlusion_->rebind_placed_nodes();
	leg_end();
	// The authored .def item effects and effect lights attached at load against
	// an empty placer; re-attach against the placed sources (the same pair the
	// effect-catalog warm-up re-runs).
	leg("reattach");
	item_fx_->reattach();
	if (light_director_.is_valid()) {
		light_director_->reattach();
	}
	leg_end();
	UtilityFunctions::print_verbose(vformat(
			"GameWorld: placed %d streamed mission objects (%d batched / %d animated, %d unresolved, %d markers)",
			mission_stats_->get_placed(), mission_stats_->get_batched(), mission_stats_->get_animated(),
			mission_stats_->get_unresolved(), mission_stats_->get_markers()));
	// The placement run's own unit spans (an instrument).
	UtilityFunctions::print_verbose(vformat(
			"GameWorld: streamed placement units: bucket %d ms, statics %d ms, models %d ms (%d graphics, %d batches, %d static shadow batches)",
			static_cast<int>(mission_stats_->get_span_bucket_entities_usec() / 1000),
			static_cast<int>(mission_stats_->get_span_static_batches_usec() / 1000),
			static_cast<int>(mission_stats_->get_span_animated_models_usec() / 1000),
			mission_stats_->get_graphics(), mission_stats_->get_batches(),
			mission_stats_->get_static_shadow_batches()));
}

// The revealed world must never race the budgeted cold wire materialization:
// SessionDrive holds the join-admission edge until the wire presenter's
// deferred-spawn queue drains behind the loading/DEATH hold. Trivially true
// with no runtime or no wire presenter.
bool GameWorld::is_join_wire_present_drained() const {
	MissionRoot *runtime = get_runtime();
	return runtime == nullptr || runtime->join_wire_present_pending() == 0;
}

// Fail the streamed-asset leg once per join. Both the per-frame runtime
// driver and the frame-polled admission observer can observe the same
// protocol edge; routing them through one latch prevents duplicate
// load_failed emissions.
void GameWorld::report_join_wire_asset_failure(const String &p_reason) {
	join_wire_assets_failed_ = true;
	join_wire_assets_pending_ = false;
	if (join_wire_asset_failure_emitted_) {
		return;
	}
	join_wire_asset_failure_emitted_ = true;
	emit_signal(kSignalLoadFailed, p_reason);
}

void GameWorld::clear_mission_tile_info() {
	mission_tile_info_.unref();
	mission_til_bytes_ = PackedByteArray();
	if (terrain_ != nullptr) {
		terrain_->set_tile_info_override(Ref<TerrainTileInfo>());
	}
	if (dispatcher_ != nullptr) {
		dispatcher_->set_tile_info(Ref<TerrainTileInfo>());
	}
}

// --- terrain + foliage ---------------------------------------------------------------

bool GameWorld::load_terrain(
		const String &p_trn_path, const String &p_tile_set, const String &p_environment) {
	terrain_textures_pending_ = false;
	Ref<TerrainData> data;
	data.instantiate();
	prepare_terrain_load(**data, p_tile_set, p_environment);
	if (data->load_config_from_resource_root(resource_root_, p_trn_path) != OK) {
		return false;
	}
	if (terrain_ == nullptr) {
		return false;
	}
	terrain_data_ = data;
	// The previous mission's build goes (an unload keeps it), then every row that reads the
	// terrain takes the new one (world/world_scene.h); the node builds at the mission textures
	// stage (load_terrain_textures).
	terrain_->clear_built();
	scene_.bind_terrain(data, mission_tile_info_);
	terrain_textures_pending_ = true;
	return true;
}

void GameWorld::load_terrain_textures(const Ref<LoadTimeline> &p_timeline, bool p_dedicated_host) {
	if (!terrain_textures_pending_) {
		return;
	}
	terrain_textures_pending_ = false;
	if (terrain_ == nullptr || terrain_data_.is_null()) {
		return;
	}
	p_timeline->span("terrain_textures");
	terrain_data_->load_textures();
	// The minimap's depthspin water mask among the terrain's textures, on both arms, ahead of the
	// freeze and the HUD's textures (engine mission/mission_load_plan.h carries the witness,
	// D-NET-NEW-L3-37).
	build_minimap_water_mask();
	// The render subsystems' environment cube, ahead of the tile cache's page targets: its device
	// objects create here, ahead of the first capture, on the full arm alone (engine
	// renderer/environment_cube.h creates_environment_cube carries the witness, D-NET-NEW-LF-13).
	if (environment_cube_ != nullptr &&
			opennova::renderer::creates_environment_cube(p_dedicated_host)) {
		environment_cube_->create_cube_device();
	}
	terrain_->build(opennova::terrain::builds_lod_cache(p_dedicated_host),
			opennova::terrain::builds_tile_cache_targets(p_dedicated_host));
	if (opennova::terrain::loads_foliage_definitions(p_dedicated_host)) configure_foliage();
	p_timeline->end_span();
}

// The mission textures stage's sky maps, past the terrain's textures and ahead of the freeze: the
// full arm's, none on a dedicated host's reduced arm (engine environment/sky_frame.h
// loads_mission_sky_maps carries the witness, D-NET-NEW-L3-34).
void GameWorld::load_sky_maps(const Ref<LoadTimeline> &p_timeline, bool p_dedicated_host) {
	p_timeline->span("sky_maps");
	scene_.load_sky_maps(p_dedicated_host);
	p_timeline->end_span();
}

// The mission textures stage's view effects, tracer pool and FrameFX, past the HUD's textures and
// in that order, on the full arm alone: the NVG view's device objects, the tracer pool's
// materials' (the distortion ribbons' and the overlay stages', whose NVG laser draws the pool's
// laser material), then FrameFX's pixel shaders and its scanline texture (engine
// renderer/frame_fx_effects.h creates_view_effect_shaders, creates_frame_fx_pixel_shaders and
// builds_frame_fx_scanline_texture, renderer/tracer_frame.h creates_tracer_pool_shaders;
// D-NET-NEW-L3-35, D-NET-NEW-LF-13).
void GameWorld::load_frame_fx_textures(const Ref<LoadTimeline> &p_timeline, bool p_dedicated_host) {
	p_timeline->span("frame_fx_textures");
	if (framefx_ != nullptr && opennova::renderer::creates_view_effect_shaders(p_dedicated_host)) {
		framefx_->init_view_effects();
	}
	EffectWorld *effect_world = get_effect_world();
	if (effect_world != nullptr && opennova::renderer::creates_tracer_pool_shaders(p_dedicated_host)) {
		effect_world->create_tracer_pool_devices();
	}
	scene_.init_frame_fx_mission_textures(p_dedicated_host);
	p_timeline->end_span();
}

// Runtime foliage: Terrain supplies the retail 16-unit detail-cell set; the
// sim's crouched/prone infantry supply the distant silhouette anchors (see
// render_foliage_frame). Sampling and deterministic candidate generation
// stay in the fresh native runtime.
void GameWorld::configure_foliage() {
	if (dispatcher_ == nullptr || terrain_data_.is_null()) {
		return;
	}
	// The definitions' load creates the foliage's vertex shaders first: the MODEL masks' device
	// shader here (engine terrain/terrain_load_legs.h loads_foliage_definitions carries the
	// witness, D-NET-NEW-LF-13).
	dispatcher_->create_mask_device();
	const Array diagnostics = scene_.configure_foliage(resource_root_, terrain_data_);
	for (int64_t i = 0; i < diagnostics.size(); ++i) {
		const Dictionary diagnostic = diagnostics[i];
		const String status = diagnostic.get("status", "");
		if (status == "missing_mesh" || status == "invalid_mesh") {
			UtilityFunctions::push_warning(vformat("GameWorld: foliage slot %d graphic '%s' disabled (%s).",
					int(diagnostic.get("slot", -1)), String(diagnostic.get("graphic", "")), status));
		} else if (status == "enabled" && !bool(diagnostic.get("fd_texture_loaded", false))) {
			UtilityFunctions::push_warning(vformat(
					"GameWorld: foliage slot %d graphic '%s' has no :fd texture; appearance is degraded.",
					int(diagnostic.get("slot", -1)), String(diagnostic.get("graphic", ""))));
		}
	}
}

// --- the runtime -----------------------------------------------------------------------

// Start the shared mission runtime (the MissionRoot start_mission_root
// created before placement): it promotes the mission, builds the present
// index over the placed MissionObjects, and each tick applies every entity's
// transform + part animations (PLAYPARTANIM, applied in-engine) + visibility
// onto its model. The game runs it at the faithful 62-frame cadence and
// drives it explicitly from tick(); its drained side effects route through
// on_runtime_effects. A reload reuses this GameWorld, so any prior runtime is
// freed in unload() first.
// See game_world.h.
Ref<MissionSetupOptions> GameWorld::make_runtime_options(const Ref<MissionData> &p_mission,
		const String &p_bms_name) {
	String mission_file = p_bms_name.get_file();
	if (mission_file.is_empty()) {
		mission_file = p_bms_name;
	}
	String mission_label = p_mission->get_mission_name().strip_edges();
	if (mission_label.is_empty()) {
		mission_label = mission_file.get_basename();
	}
	Ref<MissionSetupOptions> opts;
	opts.instantiate();
	// The resource root, so soldiers resolve their .adm/.bad root-motion clips.
	opts->set_resource_root(resource_root_);
	// The by-name readers' base: the mission's name cut at its first '.'
	// (runtime/mission/mission_sidecars.h mission_base_name).
	opts->set_wac_basename(opennova::to_gd(opennova::mission::mission_base_name(opennova::to_std(p_bms_name))));
	opts->set_music_director(get_music_director());
	opts->set_mission_file(mission_file);
	opts->set_mission_name(mission_label);
	PackedStringArray spawn_names;
	spawn_names.push_back(mission_label);
	opts->set_spawn_names(spawn_names);
	opts->set_local_character_profile(local_character_profile_);
	opts->set_player_profiles(player_profiles_);
	opts->set_game_config(game_config_);
	opts->set_host_rotation(&host_rotation_);
	opts->set_client_process(&client_process_);
	// The single player's restart, which this load consumes (set_restart_boot).
	opts->set_restart_boot(restart_boot_);
	restart_boot_ = false;
	opts->set_log_devices(log_devices_.get());
	opts->set_controls_model(controls_model_);
	// Serve-and-play hosts run the listen server AND spawn their own player
	// (ADR 0011/0012, net-re §5.2b/§5.38). A DEDICATED host (config
	// "dedicated") serves WITHOUT a local player — same listen server, just no
	// own-player spawn; main_game skips the HUD when there is no local player.
	// Diagnostic previews opt out via playable_.
	opts->set_playable(playable_ && !drive_.pending_dedicated());
	// This load runs the boot's phase B itself, after its device stages.
	opts->set_mission_start_deferred(true);
	// Stamp the staged net-session request (typed record + derived staging +
	// the surrendered preload sim, consumed once per load) onto the runtime's
	// options — MissionRoot alone adopts opts.simulation (ADR 0011/0012).
	drive_.stage_runtime_options(opts);
	return opts;
}

void GameWorld::project_local_character_profile(const Ref<AvatarDatabase> &p_avatar_db) {
	// The empty table still projects the record's voice override (engine
	// join_character_profile_without_table).
	local_character_profile_ = p_avatar_db.is_valid()
			? p_avatar_db->character_join_profile_from_loadout(
					  player_visuals_->spawn_loadout(), drive_.pending_session())
			: AvatarDatabase::character_join_profile_without_table(
					  player_visuals_->spawn_loadout(), drive_.pending_session());
}

// See game_world.h. Retail's host opens its socket at the host screen's START and creates its
// session in the pre-game menu, both ahead of the Game Loop's mission start; a single player's
// start creates its session ahead of it too. So the session stands before the mission start's
// every load (D-NET-NEW-L1-50; engine inmatch/host_boot.h create_host_session carries the
// witness).
int GameWorld::create_fresh_session(const Ref<MissionData> &p_mission, const String &p_bms_name,
		const Ref<ResourceRoot> &p_resource_root) {
	staged_runtime_options_.unref();
	resource_root_ = p_resource_root;
	// The create reads the player's character words: the session settings' apply copies the
	// profile's ahead of it, over the Avatars.def the process loaded at its start, so the local
	// profile is projected here, from the mount's table, ahead of the options that carry it
	// (D-NET-NEW-L1-50, V-ROLL-BE C-1; the witness is HostRole::create_session's,
	// engine/runtime/inmatch/host_role.cpp).
	Ref<AvatarDatabase> avatar_db;
	avatar_db.instantiate();
	if (resource_root_.is_null() ||
			avatar_db->load_from_resource_root(resource_root_, "Avatars.def") != OK)
		avatar_db.unref();
	project_local_character_profile(avatar_db);
	Ref<MissionSetupOptions> opts = make_runtime_options(p_mission, p_bms_name);
	Ref<Simulation> sim = opts->get_simulation();
	const int stage_error = MissionRoot::stage_simulation(sim, p_mission, opts);
	if (stage_error != OK) {
		// Any LAN setup failure reads as the bind's, as the runtime's setup reports it.
		if (opts->get_net_transport() == "lan") {
			emit_signal(kSignalLoadFailed,
					vformat("host start: could not bind LAN UDP port %d", opts->get_bind_port()));
		} else {
			emit_signal(kSignalLoadFailed, "failed to start mission runtime");
		}
		return stage_error;
	}
	if (sim->create_host_session(p_mission, p_resource_root,
				opts->get_mission_file().replace("\\", "/").get_file()) != OK) {
		// The session create ended the process (exit code 0): no load failure to show, the
		// shell exits; the load returns what the boot's refusal returned.
		emit_signal(kSignalProcessExitRequested, 0);
		return ERR_CANT_OPEN;
	}
	opts->set_simulation(sim);
	opts->set_session_staged(true);
	staged_runtime_options_ = opts;
	return OK;
}

int GameWorld::start_runtime(const Ref<MissionData> &p_mission, const String &p_bms_name) {
	MissionRoot *runtime = get_runtime();
	Node *container = runtime->get_node_or_null(NodePath("MissionObjects"));
	// A null board is a no-op on the root (its own board is null too).
	runtime->set_frame_stats(frame_stats_);
	// The options: the ones the load's start staged with its created session, else this load's.
	Ref<MissionSetupOptions> opts = staged_runtime_options_;
	staged_runtime_options_.unref();
	if (opts.is_null()) opts = make_runtime_options(p_mission, p_bms_name);
	// A mission with no AI still ticks (BMS events / WAC); only a promote
	// failure leaves a null sim. Hand the loaded terrain to the runtime so
	// promoted AI grounds on it (entities hug the terrain).
	opts->set_terrain(terrain_data_);
	// The placer's item database (item_id -> anim_def), so each soldier
	// grounds off its own model's .adm clip set (per-entity capsule_bottom),
	// not the shared default. [D-INF-6]
	opts->set_item_db(placer_.is_valid() ? placer_->get_item_db() : Ref<ItemDatabase>());
	// Terrain-tile (.til) bytes for the S2C 0x45 terrain-tile load a listen
	// host streams to joiners so their g_LoadingProgress climbs 5 -> 6 and
	// terrain finishes loading (net-re §5.37). The tile-overlay .til is named
	// after the MISSION (localres.pff: ASH_I5A.til), not the terrain tileinfo
	// [orig: Terrain_LoadTileInfoFile @ 0x60a740;
	// Terrain_SerializeTiles @ 0x6080f0]. Reuse the payload parsed before
	// terrain build.
	if (!mission_til_bytes_.is_empty()) {
		opts->set_terrain_til(mission_til_bytes_);
	}
	// The placer + environment node let the wire present pass resolve + light
	// its remote-entity avatars (build_player_animated_model): every remote
	// row on a joiner, and the admitted players' synthetic-origin rows on the
	// host. The present passes' other collaborators (audio, effect world,
	// lights, environment, anchors) bind in start_effect_world, once they
	// exist.
	opts->set_placer(placer_);
	runtime->setup(p_mission, container, opts);
	if (runtime->get_sim().is_null()) {
		const int setup_error = runtime->get_setup_error();
		const bool process_exit = runtime->is_process_exit();
		const bool lan_bind_failure = opts->get_net_transport() == "lan";
		const int bind_port = opts->get_bind_port();
		// Free before emitting: a load_failed handler may synchronously tear
		// the world down (the game shell returns to the menu via unload()),
		// and unload() frees the runtime — emitting first turned this leg into
		// a null-instance free on reentry. The free is IMMEDIATE on purpose
		// (not queue_free) and takes the root's MissionObjects child with it:
		// nothing may present a half-built mission before the handler runs.
		runtime_id_ = ObjectID();
		memdelete(runtime);
		if (process_exit) {
			// The session create ended the process (exit code 0): no load
			// failure to show, the shell exits.
			emit_signal(kSignalProcessExitRequested, 0);
		} else if (lan_bind_failure) {
			emit_signal(kSignalLoadFailed, vformat("host start: could not bind LAN UDP port %d", bind_port));
		} else {
			emit_signal(kSignalLoadFailed, "failed to start mission runtime");
		}
		return setup_error != OK ? setup_error : ERR_CANT_CREATE;
	}
	// The occlusion frame's per-mission members: the sim whose verdicts it
	// applies, the placed-node index it resolves buildings/entities through,
	// and the entity presenter carrying the wire render gates + lighting
	// contexts. Re-handed per load; unload's reset() forgets them.
	occlusion_->bind_mission(runtime->get_sim(), runtime->get_entity_index(), runtime->get_entity_presenter(),
			placer_);
	// Phase B of the one host boot, here unless the boot left the host's mission-start head
	// to it: then it runs past the device stages (load_mission_internal).
	if (!runtime->get_sim()->host_mission_start_deferred()) run_mission_start();
	sync_runtime_profiling();
	runtime->set_presentation_time_ms(panm_clock_->get_time_ms());
	runtime->connect("effects_drained", callable_mp(this, &GameWorld::on_runtime_effects));
	runtime->connect("fixed_tick_completed", callable_mp(this, &GameWorld::on_runtime_fixed_tick));
	runtime->connect("simulation_restarted", callable_mp(this, &GameWorld::on_runtime_simulation_restarted));
	// A browsable listen host: register it with the NovaWorld gate (F1), if
	// one was configured. No-op for single-player, joiners, and pure-LAN play.
	drive_.on_runtime_started(opts, p_bms_name);
	return OK;
}

// Phase B of the one host boot (inmatch/host_boot.h): on a host whose boot left it the
// mission-start head (the render arm's last pump, the PreMission pass, the round init,
// Player_InitPlayer), then the water plane the vehicle init grounds hulls against (07TR's
// offshore LCACs otherwise settle on the seabed before crews board), the environment
// boundary and the session's load end; then the local player's spawn kit, and the game
// starts running.
void GameWorld::run_mission_start() {
	run_mission_start_environment_boundary();
	// The player profile's saved weapon kits, loaded before ANY kit is applied
	// or submitted: in a net session the original's spawn kit is a page of
	// this file, selected by the very class byte it also puts on the wire
	// [orig: Game_StartMission @ 0x525767-0x525836].
	load_player_weapon_profile();
	player_visuals_->apply_local_player_spawn_loadout();
	// The game starts running past the session's load end, which leaves a single-player
	// session paused (Simulation::start_mission): tick() gates on is_playing, so the
	// overlay's transport can pause/step a live mission.
	if (MissionRoot *runtime = get_runtime()) runtime->play();
}

// The player profile's current weapon.sav record onto the sim, again after the
// boot now that the session and the catalog exist: five profile-slot records
// the shell loaded at the menu's start, each carrying a per-side class byte and
// the five 2048-byte class kit pages the MP loadout submit indexes BY that
// class byte (engine: runtime/profile/player_profiles.h). With no profile the
// shipped defaults stay installed, as a missing weapon.sav leaves them.
void GameWorld::load_player_weapon_profile() {
	Ref<Simulation> sim = get_sim();
	if (sim.is_null() || player_profiles_.is_null()) {
		return;
	}
	sim->use_player_profile(player_profiles_);
}

// Place real ambient sounds at the mission's sound markers: load the global
// banks, resolve each marker to a sound set by name, and spawn
// looping 3D voices. Reuses the placer's item database for the item_id ->
// soundloop_1..4 lookup.
void GameWorld::start_mission_audio(const Ref<MissionData> &p_mission, const String &p_bms_name) {
	Ref<ItemDatabase> item_db = placer_.is_valid() ? placer_->get_item_db() : Ref<ItemDatabase>();
	MissionAudio *mission_audio = MissionAudio::create(resource_root_, item_db);
	mission_audio_id_ = ObjectID(mission_audio->get_instance_id());
	// Sound occlusion runs LOS through the sim's collision world + terrain
	// [orig: Sound_ApplyOcclusionDistance @ 0x529970]; hosts without a sim mix
	// unoccluded.
	mission_audio->set_simulation(get_sim());
	Ref<MissionAudioStats> stats = mission_audio->setup(p_mission, p_bms_name, this);
	if (env_ != nullptr) {
		mission_audio->set_time_of_day_hhmm(env_->get_time_of_day());
	}
	UtilityFunctions::print_verbose(vformat(
			"GameWorld: mission audio — %d/%d sound markers resolved, %d bank(s), %d ambient candidate(s), %d/%d physical channel(s) allocated",
			stats->get_markers_resolved(), stats->get_markers_total(), stats->get_banks_loaded(),
			stats->get_ambient_candidates(), stats->get_physical_channels(), stats->get_channel_budget()));

}

// Build retail's depthspin shore mask directly from the raw CPT height atlas.
// Streamed .til art does not participate in either the sharp colormap base or
// this independent water pass.
void GameWorld::build_minimap_water_mask() {
	minimap_water_mask_.unref();
	if (terrain_data_.is_valid() && !Engine::get_singleton()->is_editor_hint()) {
		const float live_water = water_ != nullptr ? water_->get_water_height() : NAN;
		minimap_water_mask_ = terrain_data_->build_minimap_water_mask(live_water);
	}
	emit_signal(kSignalMinimapWaterChanged, minimap_water_mask_);
}

// The load-time effect warm pass: spawn every catalog effect in front of the
// load camera, advance the fixed tick so fresh emitters actually emit,
// force-draw one frame SYNCHRONOUSLY so every new material/pipeline draws
// once (no coroutine — the load path stays callable without await), then
// clear the warm spawns exactly like the sim-restart path (reset +
// re-register the persistent item effects). Returns the count. The mission
// start's models build nothing here: the freeze ahead of it resolved their
// materials alone (load_mission_internal, D-NET-NEW-L3-31). Retail's load draws
// no world frame at all, its presents the loading screen alone (engine
// hud/loading_screen.h carries the witness): the one forced frame is the
// device's pipeline warm, and a second compiled nothing in either load LP
// read while its GPU work held the next present (D-NET-NEW-L3-47).
int GameWorld::warm_effect_world_catalog(bool p_spawn_effects, const Ref<LoadTimeline> &p_timeline) {
	// The legs as nested spans of the caller's timeline (an instrument).
	const auto leg = [&p_timeline](const char *p_name) {
		if (p_timeline.is_valid()) p_timeline->span(p_name);
	};
	const auto leg_end = [&p_timeline]() {
		if (p_timeline.is_valid()) p_timeline->end_span();
	};
	// A load with no effect world has nothing to warm. A dedicated host never comes here: its
	// reduced arm runs the freeze alone (load_mission_internal, D-NET-NEW-L3-25).
	EffectWorld *effect_world = get_effect_world();
	if (effect_world == nullptr) {
		return 0;
	}
	Vector3 warm_pos;
	Camera3D *cam = is_inside_tree() ? get_viewport()->get_camera_3d() : nullptr;
	if (cam != nullptr) {
		warm_pos = cam->get_global_position() - cam->get_global_transform().basis.get_column(2) * 8.0;
	}
	// The persistent master switch is a gameplay preference, not a reason to
	// leave the catalog cold forever. Lift it only across the loading-screen
	// draws; keep the director's persisted switch unchanged and restore the
	// EffectWorld before persistent item effects are reattached.
	const bool restore_particles_hidden = effect_world != nullptr && effect_world->are_particles_hidden();
	if (restore_particles_hidden) {
		effect_world->set_particles_hidden(false);
	}
	leg("effect_spawn");
	const int spawned =
			p_spawn_effects && effect_world != nullptr ? effect_world->warm_all_effects(warm_pos) : 0;
	leg_end();
	// A warmed catalog still draws the presenters' fire pipelines below.
	if (spawned <= 0 && (p_spawn_effects || get_runtime() == nullptr)) {
		if (effect_world != nullptr) {
			effect_world->reset_runtime_state();
			if (restore_particles_hidden) {
				effect_world->set_particles_hidden(true);
			}
		}
		return 0;
	}
	// The tracer ribbon pipelines compile in the same forced frames.
	leg("present_pipelines");
	leg("fire_pipelines");
	MissionRoot *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->warm_present_pipelines(warm_pos);
	}
	leg_end();
	// The effect world's atlases were built at its creation (create_effect_world): this
	// draw compiles the spawned catalog's lists alone.
	leg("effect_draw");
	if (effect_world != nullptr) {
		effect_world->advance_simulation_tick(Simulation::tick_dt(),
				get_sim().is_valid() ? get_sim()->particle_force_field() : nullptr);
		effect_world->render_now(get_frame_clock_ms());
	}
	leg_end();
	leg_end();
	// Pipeline compiles need real draws. Skip the forced frames inside the
	// editor embedder (re-entrant editor drawing); the texture warm above still
	// runs there, and the shipped game is what the full warm protects.
	if (is_inside_tree() && !Engine::get_singleton()->is_editor_hint()) {
		// MainGame keeps World hidden behind the opaque loading CanvasLayer.
		// Temporarily expose it so the particle domains, tracer MeshInstance,
		// and deterministic helper quads are actually submitted to force_draw.
		const bool was_visible = is_visible();
		set_visible(true);
		RenderingServer *rs = RenderingServer::get_singleton();
		leg("forced_draw_1");
		rs->force_draw(true);
		leg_end();
		// The reset below cancels any unserviced compositor warm request. Drain
		// the forced draw first so threaded renderers cannot race that cancel.
		leg("force_sync");
		rs->force_sync();
		leg_end();
		set_visible(was_visible);
	}
	leg("warm_reset");
	if (effect_world != nullptr) {
		effect_world->reset_runtime_state();
		if (restore_particles_hidden) {
			effect_world->set_particles_hidden(true);
		}
	}
	item_fx_->reattach();
	if (light_director_.is_valid()) {
		light_director_->reattach();
	}
	leg_end();
	const int64_t unresolved =
			effect_world != nullptr ? effect_world->get_unresolved_texture_names().size() : 0;
	UtilityFunctions::print_verbose(vformat(
			"GameWorld: effect warm pass — %d effect(s) precompiled, %d unresolved texture(s)", spawned, unresolved));
	return spawned;
}

// Mission-start load of EVERY mounted .ptl into the runtime effect world
// [orig: CEffectSystem_Init @ 0x5f6070 <- Game_StartMission @ 0x524980 — no fixed
// file list: the loose ptl\*.ptl set and every PFF .ptl entry both parse].
EffectWorld *GameWorld::create_effect_world(const Ref<ResourceRoot> &p_resource_root,
		const Ref<LoadTimeline> &p_timeline) {
	EffectWorld *effect_world = memnew(EffectWorld);
	effect_world->set_name("EffectWorld");
	add_child(effect_world);
	effect_world->set_environment_source(env_);
	if (item_fx_->particles_hidden()) {
		effect_world->set_particles_hidden(true);
	}
	// The session's words, before the pages are built (CParticleTexture_InitTextureAndChannels
	// runs inside the effect load, after the mission start's copy).
	effect_world->set_session_render_settings(session_texcompression_level_, session_particle_density_);
	const int count = effect_world->load_from_resource_root(p_resource_root);
	// Then its texture atlases, each frame's file decoded into its page, as the effect system's
	// init builds them; the warm's first draw then builds none (engine renderer/particle_atlas.h
	// carries the witness, D-NET-NEW-L3-26).
	if (p_timeline.is_valid()) p_timeline->span("texture_atlases");
	effect_world->build_texture_atlases();
	if (p_timeline.is_valid()) p_timeline->end_span();
	UtilityFunctions::print_verbose(vformat("GameWorld: effect world — %d effect(s) across %d .ptl file(s)",
			count, effect_world->file_count()));
	return effect_world;
}

namespace {

// A pending effect world (a join's or the mission start head's), its id cleared.
EffectWorld *take_effect_world(ObjectID &r_id) {
	EffectWorld *pending = Object::cast_to<EffectWorld>(ObjectDB::get_instance(r_id));
	r_id = ObjectID();
	return pending;
}

void free_effect_world(ObjectID &r_id) {
	if (EffectWorld *pending = take_effect_world(r_id)) {
		pending->release_runtime_renderer_resources();
		pending->queue_free();
	}
}

} // namespace

void GameWorld::free_join_effect_world() {
	free_effect_world(join_effect_world_id_);
}

void GameWorld::free_head_effect_world() {
	free_effect_world(head_effect_world_id_);
}

void GameWorld::load_join_effect_system(const Ref<ResourceRoot> &p_resource_root) {
	// Its own timeline (an instrument: its lines print under --verbose).
	Ref<LoadTimeline> timeline = LoadTimeline::begin("Join effects");
	free_join_effect_world();
	// The mission start's copies the effect pages build at (the S2C 0x11's head copies them
	// again for its terrain).
	session_texcompression_level_ = texcompression_level_;
	session_particle_density_ = particle_density_;
	// The load's steps run the session's periodic update around the effect system's build.
	pump_join_load_step();
	timeline->span("effects");
	EffectWorld *effect_world = create_effect_world(p_resource_root, timeline);
	timeline->end_span();
	pump_join_load_step();
	// The catalog warms in front of the load camera as the load's warm does; no mission
	// root lives yet, so no presenter pipeline warms here (the S2C 0x1A's load draws those).
	timeline->span("effects_warm");
	const ObjectID live_effect_world = effect_world_id_;
	effect_world_id_ = ObjectID(effect_world->get_instance_id());
	warm_effect_world_catalog();
	effect_world_id_ = live_effect_world;
	timeline->end_span();
	pump_join_load_step();
	join_effect_world_id_ = ObjectID(effect_world->get_instance_id());
	timeline->finish();
}

void GameWorld::start_effect_world() {
	// The effect world a join's mission-data load built (load_join_effect_system) or the
	// mission start's head did (load_mission_internal). A dedicated host has none: retail's
	// loads no effect system (engine inmatch/host_role.h open_mission_load_window carries
	// the witness), so its passes and its warm run without one and the sim's item emitters keep
	// the kernel's own scene. Any other load without one builds it here.
	EffectWorld *effect_world = take_effect_world(join_effect_world_id_);
	if (effect_world == nullptr) effect_world = take_effect_world(head_effect_world_id_);
	const Ref<Simulation> role_sim = get_sim();
	const bool dedicated_host = role_sim.is_valid() &&
			role_sim->session_role() == Simulation::ROLE_DEDICATED_HOST;
	if (effect_world == nullptr && !dedicated_host) effect_world = create_effect_world(resource_root_);
	if (effect_world != nullptr) {
		effect_world_id_ = ObjectID(effect_world->get_instance_id());
		// The mission header's wind drives GLOBALWIND particles
		// (engine: runtime/particle/emitter.h).
		if (loaded_mission_.is_valid()) {
			if (const Ref<MissionInfo> info = loaded_mission_->get_info(); info.is_valid()) {
				effect_world->set_mission_wind(info->get_wind_speed(), info->get_wind_direction());
			}
		}
		if (role_sim.is_valid()) role_sim->bind_item_effect_scene(effect_world->shared_native_scene());
	}
	if (water_ != nullptr) {
		if (effect_world != nullptr) {
			effect_world->set_water_plane(water_->get_water_height(), water_->get_reflection_camera());
			effect_world->set_inset_mirror_camera(water_->get_inset_reflection_camera());
		}
		// The sim-side water plane (env.water_z): the footstep water pick, the
		// landing legs, AND the destruction paths (submerged wrecks skip pieces,
		// the wreck fire steams out) all gate on it [orig: g_EnvWaterHeightFixed
		// @ 0x26C6454; world-wac-ai-re §24]. Idempotent; re-pushed after runtime
		// start too (either side may come up first).
		Ref<Simulation> water_sim = get_sim();
		if (water_sim.is_valid()) {
			water_sim->set_water_z(water_->get_water_height());
		}
	}
	// Item-effect wiring — the owner-pose provider, the persistent per-item
	// attaches, and the wire-spawn callback — lives in the director.
	item_fx_->on_effect_world_started();
	if (light_director_.is_valid()) {
		light_director_->reattach();
	}
	// One wire-spawn router for both directors (on_wire_node_spawned),
	// subscribed to the entity presenter's spawn signal. This stage runs
	// inside the load, after start_runtime and before the first session frame
	// presents anything, so no wire body exists yet and nothing needs
	// replaying (a subscriber that connects after the first present replays
	// wire_nodes() itself).
	MissionRoot *wire_runtime = get_runtime();
	EntityPresenter *presenter = wire_runtime != nullptr ? wire_runtime->get_entity_presenter() : nullptr;
	if (presenter != nullptr && light_director_.is_valid()) {
		presenter->connect("wire_node_spawned", callable_mp(this, &GameWorld::on_wire_node_spawned));
	}
	// The present passes' typed collaborators (ADR 0043 d9): the mission audio
	// (the stage before this one), this effect world, the light director, the
	// environment node and the owner-anchor registry (GameWorld's
	// ItemEffectDirector). Bound
	// once here, inside the load and before the first session frame presents;
	// the fire/destruction sound legs and the effect legs gate on the objects
	// themselves (a dedicated serve binds no camera listener).
	if (wire_runtime != nullptr) {
		wire_runtime->setup_passes(get_mission_audio(), effect_world, light_director_.ptr(), env_, item_fx_.ptr());
	}
}
