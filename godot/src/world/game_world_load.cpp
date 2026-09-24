// GameWorld's mission load plan (the engine's mission_load_plan.h names the
// order): the four load entries, the ONE internal mission path, mount, place,
// environment, terrain, the tile info and join-wire assets, the runtime
// start, audio, the minimap mask, the effect world, and the explicit unload
// order. The former world_load_stages.gd plus game_world.gd's load entries
// (slice G10). This is not a second load coordinator: the shell's
// WorldLoadCoordinator owns the operation and its cancel/settle edges.

#include "world/game_world.h"

#include <cmath>

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

using namespace godot;

namespace {

constexpr const char *kSignalWorldLoaded = "world_loaded";
constexpr const char *kSignalLoadFailed = "load_failed";
constexpr const char *kSignalLoadProgress = "load_progress";
constexpr const char *kSignalMinimapWaterChanged = "minimap_water_changed";
constexpr const char *kSignalMusicContextOpened = "music_context_opened";
constexpr const char *kSignalMusicContextClosed = "music_context_closed";

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
	if (!load_environment(env_file_)) {
		emit_signal(kSignalLoadFailed, vformat("failed to load %s", env_file_));
		return ERR_CANT_OPEN;
	}
	if (!load_terrain(terrain_file_, String())) {
		emit_signal(kSignalLoadFailed, vformat("failed to load %s", terrain_file_));
		return ERR_CANT_OPEN;
	}

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
	Ref<ResourceRoot> resource_root = resolve_root(p_dir);
	if (resource_root.is_null()) {
		return ERR_CANT_OPEN;
	}
	// The runtime BMS path bypasses loose overrides even under /d.
	// [orig: Mission_LoadBMSFromPFF @ 0x40d43c]
	if (!resource_root->has_file(p_bms_name, ResourceRoot::LOOKUP_FORCE_ARCHIVE_ONLY)) {
		emit_signal(kSignalLoadFailed,
				vformat("%s not found in %s", p_bms_name, resource_root->get_root_dir()));
		return ERR_FILE_NOT_FOUND;
	}
	Ref<MissionData> mission;
	mission.instantiate();
	if (mission->open_from_resource_root(resource_root, p_bms_name,
				ResourceRoot::LOOKUP_FORCE_ARCHIVE_ONLY) != OK) {
		emit_signal(kSignalLoadFailed,
				vformat("failed to parse %s: %s", p_bms_name, mission->get_last_error()));
		return ERR_CANT_OPEN;
	}
	return load_mission_internal(mission, p_bms_name, resource_root);
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
	join_wire_assets_pending_ = wire_header_join;
	join_wire_til_applied_ = false;
	join_wire_assets_failed_ = false;
	join_wire_asset_failure_emitted_ = false;
	// The local-asset gate holds for wire-header joins too — there is no world
	// without terrain/env, and retail joiners also resolve both from the local
	// install by the names the wire supplies (net-re section 5.28: custom
	// missions reference stock assets). Only the REPORT is join-aware: a wire
	// join names the host's stream and the mounted expansion so a live punt
	// reads as "your install lacks X", not as a bad local file.
	// The document's references are basenames from either header source (the
	// wire 0x0B header's extension is dropped by mission_info()).
	const String trn = p_mission->get_terrain_ref() + ".trn";
	if (!p_resource_root->has_file(trn)) {
		emit_signal(kSignalLoadFailed,
				missing_mission_asset_reason(trn, p_bms_name, p_resource_root, wire_header_join));
		return ERR_FILE_NOT_FOUND;
	}
	const String env_name = p_mission->get_environment_ref() + ".env";
	if (!p_resource_root->has_file(env_name)) {
		emit_signal(kSignalLoadFailed,
				missing_mission_asset_reason(env_name, p_bms_name, p_resource_root, wire_header_join));
		return ERR_FILE_NOT_FOUND;
	}

	set_weather_world_tick_driven(true);
	set_water_world_rendering_enabled(false);
	// Keep stage attribution stable so load timelines remain comparable.
	Ref<LoadTimeline> timeline = LoadTimeline::begin(vformat("Mission load %s", p_bms_name));
	last_load_timeline_ = timeline;
	resource_root_ = p_resource_root;
	// The shared .3DI definition cache resets before the environment and
	// terrain stages (the load plan's first order witness): Celestial resolves
	// its models from load_environment, and foliage loaded by terrain must
	// remain present-but-excluded in the same generation.
	ObjectData::reset_network_challenge_model_registry();
	load_mission_tile_info(p_bms_name, p_resource_root, PackedByteArray(), p_mission->is_wire_header_only());
	// Each stage below presents the plan's anchor when it starts.
	emit_signal(kSignalLoadProgress,
			MissionData::load_progress_percent(MissionData::LOAD_STAGE_ENVIRONMENT));
	timeline->span("environment");
	if (!load_environment(env_name)) {
		emit_signal(kSignalLoadFailed, vformat("failed to load %s", env_name));
		timeline->finish();
		return ERR_CANT_OPEN;
	}
	apply_mission_environment_overrides(p_mission);
	// Initialize the exact mission clock and the reset weather owner before the
	// runtime is constructed. The authority publishes this T0 sample after setup
	// but before play, so its first network tick cannot observe stale/default data.
	Ref<MissionInfo> mission_info = p_mission->get_info();
	mission_clock_start_q8_8_ = mission_info->get_start_time();
	mission_clock_minutes_per_day_ = mission_info->get_minutes_per_day();
	if (env_ != nullptr) {
		env_->configure_mission_clock(mission_clock_start_q8_8_, mission_clock_minutes_per_day_);
	}
	prepare_world_driven_weather();
	timeline->end_span();
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_TERRAIN));
	timeline->span("terrain");
	if (!load_terrain(trn, p_mission->get_tile_set_ref())) {
		emit_signal(kSignalLoadFailed, vformat("failed to load %s", trn));
		timeline->finish();
		return ERR_CANT_OPEN;
	}
	timeline->end_span();
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_OBJECTS));

	loaded_mission_ = p_mission;
	// The mission attribute that forces the indoors accum bit every frame.
	// [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8]
	mission_forces_indoors_ =
			(p_mission->get_info()->get_attrib_flags() & MissionData::ATTRIB_FORCE_INDOORS) != 0;
	timeline->span("objects");
	start_mission_root();
	place_mission_objects(p_mission);
	timeline->end_span();
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_RUNTIME));
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
		unload();
		return runtime_error;
	}
	// The non-foliage loaded-.3DI page freezes once here (the load plan's
	// freeze witness): MissionRoot.setup has now resolved the placed/wire
	// mission models (including collision/husk definitions); late network spawns
	// must not change this page.
	Ref<Simulation> challenge_sim = get_runtime()->get_sim();
	if (challenge_sim.is_valid() && !wire_header_join) {
		if (challenge_sim->is_joiner()) {
			player_visuals_->prewarm_loaded_model_challenge_definitions();
		}
		challenge_sim->finalize_loaded_model_challenge_snapshot();
	}
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_AUDIO));
	timeline->span("audio");
	start_mission_audio(p_mission, p_bms_name);
	timeline->end_span();
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_EFFECTS));
	timeline->span("effects");
	start_effect_world();
	timeline->end_span();
	// Warm the effect catalog while the loading screen still covers the frame:
	// the first live spawn otherwise pays the deferred texture resolves + the
	// renderer's first-draw pipeline compiles as a ~90 ms hitch on the player's
	// first shot (measured: first-fire tap 92.9 ms -> repeat 12.5 ms). Retail
	// pays this at load (the load plan's effect-system witness).
	emit_signal(kSignalLoadProgress,
			MissionData::load_progress_percent(MissionData::LOAD_STAGE_EFFECTS_WARM));
	timeline->span("effects_warm");
	warm_effect_world_catalog();
    route_script_effects(); // initial WAC/BMS descriptors survived the warm-scene reset
	timeline->end_span();
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_FINISH));
	timeline->finish();
	loaded_mission_file_ = p_bms_name;
	world_ready_ = true;
	set_water_world_rendering_enabled(true);
	build_minimap_water_mask();
	emit_signal(kSignalLoadProgress, MissionData::LOAD_PROGRESS_WORLD_READY);
	emit_signal(kSignalWorldLoaded);
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

void GameWorld::pulse_object_stage_progress() {
	emit_signal(kSignalLoadProgress, MissionData::load_progress_percent(MissionData::LOAD_STAGE_OBJECTS));
}

// Populate the world with the mission's placed objects under the mission
// root's MissionObjects node. Uses the same shell-agnostic placer as every
// other mission load path.
void GameWorld::place_mission_objects(const Ref<MissionData> &p_mission) {
	if (resource_root_.is_null() || p_mission.is_null()) {
		return;
	}
	placer_ = MissionObjectPlacer::create(resource_root_, Ref<ItemDatabase>());
	// The placer's Avatars.def is the one registry every player visual resolves
	// against; the same table projects the local profile the sim stamps.
	Ref<AvatarDatabase> avatar_db = placer_->get_avatar_db();
	Ref<Simulation> sim = get_sim();
	if (sim.is_valid()) {
		sim->set_character_avatar_database(avatar_db);
	}
	if (avatar_db.is_valid()) {
		local_character_profile_ =
				avatar_db->character_join_profile_from_loadout(player_visuals_->spawn_loadout());
	} else {
		UtilityFunctions::push_warning("GameWorld: Avatars.def unavailable; players draw their item model");
		local_character_profile_.unref();
	}
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
	options["progress"] = callable_mp(this, &GameWorld::pulse_object_stage_progress);
	mission_stats_ = placer_->place(p_mission, get_runtime(), options);
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
}

// Tear down a loaded world so the shell can return to the menu (or load a
// different mission) without the previous world lingering. Frees the
// dynamically placed MissionObjects subtree and resets the load state; the
// terrain / environment scene nodes are kept in place and rebuilt by the next
// load_*(). Safe to call when nothing is loaded.
void GameWorld::unload() {
	world_ready_ = false;
	minimap_water_mask_.unref();
	emit_signal(kSignalMinimapWaterChanged, Variant());
	join_wire_assets_pending_ = false;
	join_wire_til_applied_ = false;
	join_wire_assets_failed_ = false;
	join_wire_asset_failure_emitted_ = false;
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
	mission_forces_indoors_ = false;
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
	mission_audio_id_ = ObjectID();
	placer_.unref();
	weapon_db_.unref(); // re-resolves against the next load's mounted root
	// The staged spawn loadout and every equipped-weapon memo drop with the
	// mission (LocalPlayerVisuals.reset carries the memo/override rules).
	player_visuals_->reset();
	mission_stats_.unref();
}

// --- environment ---------------------------------------------------------------------

bool GameWorld::load_environment(const String &p_env_path) {
	if (env_ == nullptr) {
		return true;
	}
	Ref<EnvFile> env;
	env.instantiate();
	if (env->load_from_resource_root(resource_root_, p_env_path) != OK) {
		UtilityFunctions::push_warning(vformat("GameWorld: failed to load environment '%s'", p_env_path));
		return false;
	}
	// MissionEnvironment's setter reloads + pushes shader globals on assignment.
	env_->set_environment_data(env);
	// The overcast table the overcast blend cross-fades against: overcast.def
	// appended after the .trn pass (stock .trn files carry no TOD blocks)
	// (retail Environment_LoadTimeOfDayConfig @ 0x57db30).
	Ref<EnvFile> overcast;
	overcast.instantiate();
	if (overcast->load_from_resource_root(resource_root_, "overcast.def") == OK) {
		env_->set_overcast_data(overcast);
	} else {
		env_->set_overcast_data(Ref<EnvFile>());
	}
	// GameWorld retains one Weather node across loads. A replacement ENV is
	// a discrete state change: retail snaps every color block to the new mission
	// targets instead of easing over from the previous mission's currents.
	if (weather_ != nullptr) {
		weather_->resync_colors();
	}
	if (celestial_ != nullptr) {
		celestial_->set_resource_root(resource_root_);
	}
	if (precipitation_ != nullptr) {
		precipitation_->set_resource_root(resource_root_);
	}
	// FrameFX's mission texture (the "ffscan" scanlines) draws from the render
	// CRT stream here, as Render_InitMissionTextures does.
	if (framefx_ != nullptr) {
		framefx_->init_mission_textures();
	}
	if (environment_cube_ != nullptr) {
		environment_cube_->force_capture();
	}
	return true;
}

// Apply the mission's attrib-gated water/fog overrides onto the loaded env via
// EnvFile's non-persistent override layer [orig: Game_LoadTerrainDuringConnect
// @ 0x520710]. The base .env is never mutated.
void GameWorld::apply_mission_environment_overrides(const Ref<MissionData> &p_mission) {
	if (p_mission.is_null()) {
		return;
	}
	Ref<MissionEnvironmentOverrides> overrides = p_mission->get_environment_overrides();
	// EnvFile owns the other live-view overrides, while water keeps the BMS
	// rung distinct so a flagged zero still beats a nonzero TRN height.
	if (env_ != nullptr) {
		Ref<EnvFile> env_data = env_->get_environment_data();
		if (env_data.is_valid()) {
			if (overrides->is_empty()) {
				env_data->clear_mission_overrides();
			} else {
				env_data->apply_mission_overrides(overrides);
			}
		}
	}
	float mission_water = NAN;
	if (overrides->get_has_water_height()) {
		mission_water = overrides->get_water_height_world();
	}
	set_mission_water_height_override(mission_water);
}

void GameWorld::set_mission_water_height_override(float p_world_height) {
	if (water_ != nullptr) {
		water_->set_mission_water_height_override(p_world_height);
	}
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
	if (weather_ != nullptr) {
		weather_->prepare_world_driven();
	} else {
		set_weather_world_tick_driven(true);
	}
}

void GameWorld::prepare_autonomous_weather() {
	if (weather_ != nullptr) {
		// A world without a mission runs the environment's standalone weather
		// home: drop any bound Simulation first.
		weather_->bind_simulation(nullptr);
		weather_->prepare_autonomous();
	} else {
		set_weather_world_tick_driven(false);
	}
}

// The witnessed mission-start environment boundary runs natively on the
// weather device (Weather.run_mission_start_boundary): the World's weather
// seed from the loaded .env + the BMS clock, the authority's WAC direct
// execution, the initializer + 255-tick settle, the baseline seal.
void GameWorld::run_mission_start_environment_boundary() {
	if (weather_ != nullptr) {
		weather_->run_mission_start_boundary(get_sim().ptr(), mission_clock_start_q8_8_,
				mission_clock_minutes_per_day_);
	}
}

// --- the tile info + join-wire assets ------------------------------------------------

// Retail loads <mission>.til into one shared g_TerrainTileArray used by
// terrain overlays/surface overrides, network initial state, and both foliage
// generators' radius-2 blocker.
// Its file probe/read force loose-first around this one load.
// [orig: Terrain_LoadTileInfoFile @ 0x60a740, policy force @ 0x60a74e;
// Terrain_GetSurfaceTypeAtPosition @ 0x606510;
// Foliage_PathBlockedByPlacedTile @ 0x606490]
void GameWorld::load_mission_tile_info(const String &p_bms_name, const Ref<ResourceRoot> &p_resource_root,
		const PackedByteArray &p_wire_til_bytes, bool p_wire_is_authoritative) {
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
	String mission_name = p_bms_name.get_file();
	if (mission_name.is_empty()) {
		mission_name = p_bms_name;
	}
	const String til_name = mission_name.get_basename() + ".til";
	if (!p_resource_root->has_file(til_name, ResourceRoot::LOOKUP_FORCE_LOOSE_FIRST)) {
		return;
	}
	const PackedByteArray til_bytes = p_resource_root->read_file(til_name, ResourceRoot::LOOKUP_FORCE_LOOSE_FIRST);
	if (til_bytes.is_empty()) {
		return;
	}
	Ref<TerrainTileInfo> tile_info;
	tile_info.instantiate();
	if (tile_info->load_from_bytes(til_bytes) != OK) {
		UtilityFunctions::push_warning(vformat("GameWorld: failed to parse mission tile file '%s'.", til_name));
		return;
	}
	mission_tile_info_ = tile_info;
	mission_til_bytes_ = til_bytes;
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
	// the loading screen remains raised until settle_join_wire_assets below.
	if (terrain_ != nullptr) {
		terrain_->set_tile_info_override(tile_info);
	}
	if (dispatcher_ != nullptr) {
		dispatcher_->set_tile_info(tile_info);
	}
	build_minimap_water_mask();
	return true;
}

// Complete the wire-only part of a retail join once the protocol reaches its
// deployment/admission boundary. SessionDrive calls this before revealing
// the world (or deploy map), guaranteeing the optional 0x45 overlay and the
// renderer-backed C2S 0x3D snapshot reflect the completed initial stream.
bool GameWorld::settle_join_wire_assets() {
	if (join_wire_assets_failed_) {
		return false;
	}
	if (!join_wire_assets_pending_) {
		return true;
	}
	if (!apply_join_wire_til_if_ready()) {
		return false;
	}
	Ref<Simulation> sim = get_sim();
	if (sim.is_null() || !sim->is_joiner()) {
		join_wire_assets_failed_ = true;
		return false;
	}
	const int64_t til_state = sim->get_join_terrain_til_state();
	if (til_state == Simulation::JOIN_TERRAIN_TIL_RECEIVING || til_state == Simulation::JOIN_TERRAIN_TIL_INVALID ||
			(til_state == Simulation::JOIN_TERRAIN_TIL_COMPLETE && !join_wire_til_applied_) ||
			(til_state != Simulation::JOIN_TERRAIN_TIL_ABSENT && til_state != Simulation::JOIN_TERRAIN_TIL_COMPLETE)) {
		join_wire_assets_failed_ = true;
		return false;
	}
	place_streamed_mission_objects(sim);
	player_visuals_->prewarm_loaded_model_challenge_definitions();
	sim->finalize_loaded_model_challenge_snapshot();
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
void GameWorld::place_streamed_mission_objects(const Ref<Simulation> &p_sim) {
	if (placer_.is_null() || p_sim.is_null()) {
		return;
	}
	const Array records = p_sim->get_streamed_placement_records();
	if (records.is_empty()) {
		return;
	}
	Dictionary options;
	Array skip_kinds;
	skip_kinds.push_back(MissionData::KIND_ORGANIC);
	options["skip_kinds"] = skip_kinds;
	MissionRoot *runtime = get_runtime();
	mission_stats_ = placer_->place_entities(records, runtime, options);
	if (runtime != nullptr) {
		runtime->rebind_placed_entities(placer_);
	}
	occlusion_->rebind_placed_nodes();
	// The authored .def item effects and effect lights attached at load against
	// an empty placer; re-attach against the placed sources (the same pair the
	// effect-catalog warm-up re-runs).
	item_fx_->reattach();
	if (light_director_.is_valid()) {
		light_director_->reattach();
	}
	UtilityFunctions::print_verbose(vformat(
			"GameWorld: placed %d streamed mission objects (%d batched / %d animated, %d unresolved, %d markers)",
			mission_stats_->get_placed(), mission_stats_->get_batched(), mission_stats_->get_animated(),
			mission_stats_->get_unresolved(), mission_stats_->get_markers()));
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

bool GameWorld::load_terrain(const String &p_trn_path, const String &p_tile_set) {
	Ref<TerrainData> data;
	data.instantiate();
	data->set_mission_tile_set(p_tile_set);
	if (data->load_from_resource_root(resource_root_, p_trn_path) != OK) {
		return false;
	}
	if (terrain_ == nullptr) {
		return false;
	}
	terrain_->set_tile_info_override(mission_tile_info_);
	terrain_data_ = data;
	terrain_->set_terrain_data(data);
	if (slot_shadow_ != nullptr) {
		// The shadow anchor march probes this terrain through the engine's
		// Terrain_GetHeightAtPosition port (the cite lives with the native
		// SlotShadow planner, godot/src/env/slot_shadow.cpp).
		slot_shadow_->set_terrain_data(data);
	}
	terrain_->build();
	if (water_ != nullptr) {
		water_->set_terrain_data(data);
	}
	if (environment_cube_ != nullptr) {
		environment_cube_->set_terrain_data(data);
	}
	if (celestial_ != nullptr) {
		// The glare occlusion rays march this terrain (env #14).
		celestial_->set_terrain_data(data);
	}
	configure_foliage();
	return true;
}

// Runtime foliage: Terrain supplies the retail 16-unit detail-cell set; the
// sim's crouched/prone infantry supply the distant silhouette anchors (see
// render_foliage_frame). Sampling and deterministic candidate generation
// stay in the fresh native runtime.
void GameWorld::configure_foliage() {
	if (dispatcher_ == nullptr || terrain_data_.is_null()) {
		return;
	}
	// The runtime source already supplies height, detail/model foliage indices,
	// colormap, and change invalidation. Binding the same TerrainData again as
	// the fallback colormap source attempts a duplicate terrain_changed connection
	// in Godot and makes mission reloads report ERR_INVALID_PARAMETER.
	dispatcher_->set_terrain_data(terrain_data_);
	dispatcher_->set_tile_info(terrain_->get_tile_info_override());
	const Array defs = terrain_data_->get_foliage_defs();
	// The dispatcher resolves every def's mesh and :fd texture through its own
	// per-root asset caches, then configures the four retail slots.
	dispatcher_->configure_slots_from_defs(resource_root_, defs);
	const Array diagnostics = dispatcher_->get_slot_diagnostics();
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
int GameWorld::start_runtime(const Ref<MissionData> &p_mission, const String &p_bms_name) {
	MissionRoot *runtime = get_runtime();
	Node *container = runtime->get_node_or_null(NodePath("MissionObjects"));
	// A null board is a no-op on the root (its own board is null too).
	runtime->set_frame_stats(frame_stats_);
	String mission_file = p_bms_name.get_file();
	if (mission_file.is_empty()) {
		mission_file = p_bms_name;
	}
	String mission_label = p_mission->get_mission_name().strip_edges();
	if (mission_label.is_empty()) {
		mission_label = mission_file.get_basename();
	}
	// A mission with no AI still ticks (BMS events / WAC); only a promote
	// failure leaves a null sim. Hand the loaded terrain to the runtime so
	// promoted AI grounds on it (entities hug the terrain), and the resource
	// root so soldiers resolve their .adm/.bad root-motion clips.
	Ref<MissionSetupOptions> opts;
	opts.instantiate();
	opts->set_terrain(terrain_data_);
	opts->set_resource_root(resource_root_);
	opts->set_wac_basename(p_bms_name.get_basename());
	opts->set_music_director(get_music_director());
	opts->set_mission_file(mission_file);
	opts->set_mission_name(mission_label);
	PackedStringArray spawn_names;
	spawn_names.push_back(mission_label);
	opts->set_spawn_names(spawn_names);
	// The placer's item database (item_id -> anim_def), so each soldier
	// grounds off its own model's .adm clip set (per-entity capsule_bottom),
	// not the shared default. [D-INF-6]
	opts->set_item_db(placer_.is_valid() ? placer_->get_item_db() : Ref<ItemDatabase>());
	opts->set_local_character_profile(local_character_profile_);
	// Serve-and-play hosts run the listen server AND spawn their own player
	// (ADR 0011/0012, net-re §5.2b/§5.38). A DEDICATED host (config
	// "dedicated") serves WITHOUT a local player — same listen server, just no
	// own-player spawn; main_game skips the HUD when there is no local player.
	// Diagnostic previews opt out via playable_.
	// Terrain-tile (.til) bytes for the S2C 0x45 terrain-tile load a listen
	// host streams to joiners so their g_loading_progress climbs 5 -> 6 and
	// terrain finishes loading (net-re §5.37). The tile-overlay .til is named
	// after the MISSION (localres.pff: ASH_I5A.til), not the terrain tileinfo
	// [orig: Terrain_LoadTileInfoFile @ 0x60a740;
	// serialize_terrain_tiles @ 0x6080f0]. Reuse the payload parsed before
	// terrain build.
	if (!mission_til_bytes_.is_empty()) {
		opts->set_terrain_til(mission_til_bytes_);
	}
	opts->set_playable(playable_ && !drive_.pending_dedicated());
	// Stamp the staged net-session request (typed record + derived staging +
	// the surrendered preload sim, consumed once per load) onto the runtime's
	// options — MissionRoot alone adopts opts.simulation (ADR 0011/0012).
	drive_.stage_runtime_options(opts);
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
		if (lan_bind_failure) {
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
	// Vehicle initialization at the mission-start boundary grounds hulls against
	// the water plane. Seed it before that pass, including unoccupied craft:
	// 07TR's offshore LCACs otherwise settle on the seabed before crews board.
	// The native clamp lives in VehicleSystem::initialize_mission_vehicles.
	if (water_ != nullptr)
		runtime->get_sim()->set_water_z(water_->get_water_height());
	run_mission_start_environment_boundary();
	sync_runtime_profiling();
	// The player profile's saved weapon kits, loaded before ANY kit is applied
	// or submitted: in a net session the original's spawn kit is a page of
	// this file, selected by the very class byte it also puts on the wire
	// [orig: Game_StartMission @ 0x525767-0x525836].
	load_player_weapon_profile();
	player_visuals_->apply_local_player_spawn_loadout();
	runtime->set_presentation_time_ms(panm_clock_->get_time_ms());
	runtime->connect("effects_drained", callable_mp(this, &GameWorld::on_runtime_effects));
	runtime->connect("fixed_tick_completed", callable_mp(this, &GameWorld::on_runtime_fixed_tick));
	runtime->connect("simulation_restarted", callable_mp(this, &GameWorld::on_runtime_simulation_restarted));
	// A browsable listen host: register it with the NovaWorld gate (F1), if
	// one was configured. No-op for single-player, joiners, and pure-LAN play.
	drive_.on_runtime_started(opts, p_bms_name);
	// The game starts running (tick() gates on is_playing, so the overlay's
	// transport can pause/step a live mission).
	runtime->play();
	return OK;
}

// The on-disk path of the player profile's weapon file. Retail builds it from
// the ACTIVE expansion name — with an expansion loaded it looks ONLY under
// that expansion's directory (there is no base-game fallback leg), otherwise
// it reads the game root's copy [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0,
// path build @ 0x54f68c-@ 0x54f6b7: g_ExpansionName[0] ?
// "expansion\<name>\weapon.sav" : "weapon.sav"]. The mount is the authority
// on both halves — for a joiner it has already been reconciled to the HOST's
// expansion (D-NET-178), which is what makes the profile's ADM index space
// agree with the host's.
// Load weapon.sav onto the sim: five profile-slot records, each carrying a
// per-side class byte and the five 2048-byte class kit pages the MP loadout
// submit indexes BY that class byte [orig: PlayerProfile_LoadAllFromDisk
// @ 0x54f4d0 — header check @ 0x54f586 ("FPBC"/"0211"), the 5 x 0x1080C
// record reads]. This is a plain disk file, not archive content, so it is
// read through the mount's directory rather than the VFS. A file that is
// absent or not a profile is NOT a load failure: retail's own miss leaves
// PlayerProfile_InitDefaults' shipped defaults in place (BLUE/RED class 8,
// one weapon name per class page) [orig: @ 0x54bb40].
void GameWorld::load_player_weapon_profile() {
	Ref<Simulation> sim = get_sim();
	if (sim.is_null()) {
		return;
	}
	// The mount root joined with the engine's expansion-scoped relpath (the
	// shell's PlayerProfile.weapon_profile_path computes the same path).
	if (resource_root_.is_null() || resource_root_->get_root_dir().is_empty()) {
		return;
	}
	const String path = resource_root_->get_root_dir().path_join(
			Simulation::weapon_profile_relpath(resource_root_->get_expansion()));
	if (path.is_empty()) {
		return;
	}
	if (!FileAccess::file_exists(path)) {
		UtilityFunctions::print_verbose(
				vformat("GameWorld: no weapon.sav at %s — keeping the shipped profile defaults", path));
		return;
	}
	const int err = sim->load_weapon_profile(path);
	if (err != OK) {
		UtilityFunctions::push_warning(vformat(
				"GameWorld: weapon.sav at %s not accepted (error %d) — keeping the shipped profile defaults",
				path, err));
	}
}

// Place real ambient sounds at the mission's sound markers: load the co-named
// .LWF + gamelocl.LWF, resolve each marker to a sound set by name, and spawn
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
// force-draw two frames SYNCHRONOUSLY so every new material/pipeline draws
// once (no coroutine — the load path stays callable without await), then
// clear the warm spawns exactly like the sim-restart path (reset +
// re-register the persistent item effects). Returns the count.
int GameWorld::warm_effect_world_catalog() {
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
	const bool restore_particles_hidden = effect_world->are_particles_hidden();
	if (restore_particles_hidden) {
		effect_world->set_particles_hidden(false);
	}
	const int spawned = effect_world->warm_all_effects(warm_pos);
	if (spawned <= 0) {
		effect_world->reset_runtime_state();
		if (restore_particles_hidden) {
			effect_world->set_particles_hidden(true);
		}
		return 0;
	}
	// The tracer ribbon pipelines compile in the same forced frames.
	MissionRoot *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->warm_present_pipelines(warm_pos);
	}
	effect_world->advance_simulation_tick(Simulation::tick_dt(),
				get_sim().is_valid() ? get_sim()->particle_force_field() : nullptr);
	effect_world->render_now(get_frame_clock_ms());
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
		rs->force_draw(true);
		effect_world->advance_simulation_tick(Simulation::tick_dt(),
				get_sim().is_valid() ? get_sim()->particle_force_field() : nullptr);
		effect_world->render_now(get_frame_clock_ms());
		rs->force_draw(true);
		// The reset below cancels any unserviced compositor warm request. Drain
		// the forced draws first so threaded renderers cannot race that cancel.
		rs->force_sync();
		set_visible(was_visible);
	}
	effect_world->reset_runtime_state();
	if (restore_particles_hidden) {
		effect_world->set_particles_hidden(true);
	}
	item_fx_->reattach();
	if (light_director_.is_valid()) {
		light_director_->reattach();
	}
	const int64_t unresolved = effect_world->get_unresolved_texture_names().size();
	UtilityFunctions::print_verbose(vformat(
			"GameWorld: effect warm pass — %d effect(s) precompiled, %d unresolved texture(s)", spawned, unresolved));
	return spawned;
}

// Mission-start load of EVERY mounted .ptl into the runtime effect world
// [orig: CEffectSystem_Init @ 0x5f6070 <- Game_StartMission @ 0x524980 — no fixed
// file list: the loose ptl\*.ptl set and every PFF .ptl entry both parse].
void GameWorld::start_effect_world() {
	EffectWorld *effect_world = memnew(EffectWorld);
	effect_world->set_name("EffectWorld");
	add_child(effect_world);
	effect_world_id_ = ObjectID(effect_world->get_instance_id());
	effect_world->set_environment_source(env_);
	if (item_fx_->particles_hidden()) {
		effect_world->set_particles_hidden(true);
	}
	// The mission header's wind drives GLOBALWIND particles
	// (retail Game_StartMission @ 0x524aff -> sub_5DE970 @ 0x5de970).
	if (loaded_mission_.is_valid()) {
		if (const Ref<MissionInfo> info = loaded_mission_->get_info(); info.is_valid()) {
			effect_world->set_mission_wind(info->get_wind_speed(), info->get_wind_direction());
		}
	}
	const int count = effect_world->load_from_resource_root(resource_root_);
    if (const Ref<Simulation> sim = get_sim(); sim.is_valid())
        sim->bind_item_effect_scene(effect_world->shared_native_scene());
	if (water_ != nullptr) {
		effect_world->set_water_plane(water_->get_water_height(), water_->get_reflection_camera());
		// The sim-side water plane (env.water_z): the footstep water pick, the
		// landing legs, AND the destruction paths (submerged wrecks skip pieces,
		// the wreck fire steams out) all gate on it [orig: Env_WaterHeightFixed
		// @ 0x26C6454; world-wac-ai-re §24]. Idempotent; re-pushed after runtime
		// start too (either side may come up first).
		Ref<Simulation> water_sim = get_sim();
		if (water_sim.is_valid()) {
			water_sim->set_water_z(water_->get_water_height());
		}
	}
	UtilityFunctions::print_verbose(vformat("GameWorld: effect world — %d effect(s) across %d .ptl file(s)",
			count, effect_world->file_count()));
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
