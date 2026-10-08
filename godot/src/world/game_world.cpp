#include "world/game_world.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "network/net_session_policy.h"
#include "network/novaworld_client.h"
#include <runtime/inmatch/mission_exit.h>
#include "audio/music_director.h"
#include "hud/hud_inset_scope.h"
#include "render/texture_filter_device.h"

using namespace godot;

namespace {

constexpr const char *kSignalWorldLoaded = "world_loaded";
constexpr const char *kSignalLoadFailed = "load_failed";
constexpr const char *kSignalJoinSessionIdentified = "join_session_identified";
constexpr const char *kSignalJoinAdmissionReady = "join_admission_ready";
constexpr const char *kSignalJoinDeployPickRequired = "join_deploy_pick_required";
constexpr const char *kSignalSessionLost = "session_lost";
constexpr const char *kSignalLoadProgress = "load_progress";
constexpr const char *kSignalMissionEffects = "mission_effects";
constexpr const char *kSignalMinimapWaterChanged = "minimap_water_changed";
constexpr const char *kSignalMusicContextOpened = "music_context_opened";
constexpr const char *kSignalMusicContextClosed = "music_context_closed";
constexpr const char *kSignalMusicVarChanged = "music_var_changed";
constexpr const char *kSignalCaptureChanged = "capture_changed";

} // namespace

GameWorld::GameWorld() :
		drive_(this) {
	// The render-occlusion frame: plain RefCounted (no tree presence),
	// direct-called from the device frame every frame. Constructed exactly
	// once; its scene-node wiring waits for _ready (the retained nodes).
	occlusion_.instantiate();
	panm_clock_.instantiate();
	player_visuals_.instantiate();
	player_visuals_->setup(this);
	// The item-effect director and the light director, wired to this world:
	// their two / three lent seams are typed reads off the placer's static
	// item-effect sources, its static light draw sources (+ revision) and
	// its item database, resolved lazily through the StaticSourceProvider
	// this world implements (a placer exists only once a mission is placed).
	item_fx_.instantiate();
	item_fx_->setup_with_provider(this, this);
	light_director_.instantiate();
	light_director_->setup_with_provider(this, this);
}

void GameWorld::_ready() {
	// The world's own process step exists for exactly one thing: the joiner
	// preload wait the session drive steps (switched on by load_as_joiner,
	// off when the preload ends).
	set_process(drive_.is_preload_pending());
	RenderingServer *rs = RenderingServer::get_singleton();
	const Callable post_draw = callable_mp(this, &GameWorld::on_frame_post_draw);
	if (rs != nullptr && !rs->is_connected("frame_post_draw", post_draw)) {
		rs->connect("frame_post_draw", post_draw);
	}
	terrain_ = Object::cast_to<Terrain>(get_node_or_null(NodePath("Terrain")));
	env_ = Object::cast_to<MissionEnvironment>(get_node_or_null(NodePath("MissionEnvironment")));
	water_ = Object::cast_to<Water>(get_node_or_null(NodePath("Water")));
	environment_cube_ = Object::cast_to<EnvironmentCubeCapture>(
			get_node_or_null(NodePath("EnvironmentCubeCapture")));
	framefx_ = Object::cast_to<FrameFx>(get_node_or_null(NodePath("FrameFx")));
	weather_ = Object::cast_to<Weather>(get_node_or_null(NodePath("Weather")));
	precipitation_ = Object::cast_to<Precipitation>(get_node_or_null(NodePath("Precipitation")));
	celestial_ = Object::cast_to<Celestial>(get_node_or_null(NodePath("Celestial")));
	sky_dome_ = Object::cast_to<SkyDome>(get_node_or_null(NodePath("SkyDome")));
	clear_color_ = Object::cast_to<WorldEnvironment>(get_node_or_null(NodePath("ClearColor")));
	// The foliage dispatcher sits beside the terrain, never under it: the
	// indoors letter hides the terrain but closes only the dispatcher's
	// detail passes (the occlusion frame's gates), so the MODEL masks draw on.
	dispatcher_ = Object::cast_to<FoliageDispatcher>(get_node_or_null(NodePath("FoliageDispatcher")));
	// The occlusion frame's retained render nodes exist from here on; its
	// per-mission members arrive with each load.
	occlusion_->setup(terrain_, dispatcher_, sky_dome_, celestial_, water_, env_);
	if (clear_color_ != nullptr && clear_color_->get_environment().is_valid()) {
		idle_frame_clear_color_ = clear_color_->get_environment()->get_bg_color();
	}
	if (dispatcher_ != nullptr) {
		// The applier reads the native detail-cell handoff and the composed
		// surface textures through this wired owner (never a parent probe).
		dispatcher_->set_terrain(terrain_);
		// The detail sway phase reads the weather oscillator's ring slot 0
		// (retail g_EnvWaveOscRing[0] in Foliage_SetupVertexShaderConstants).
		dispatcher_->set_weather(weather_);
	}
	// The two shadow nodes live in game_world.tscn (after Celestial) like the
	// other env presenters, absent from a code-built world; the environment
	// node is wired here (no NodePath property).
	sun_shadow_ = Object::cast_to<SunShadow>(get_node_or_null(NodePath("SunShadow")));
	if (sun_shadow_ != nullptr) {
		sun_shadow_->set_environment_node(env_);
	}
	// The render-slot entity ground shadows: the per-slot silhouette capture
	// device + the terrain drape publisher (retail's per-entity RT pipeline --
	// engine/runtime/renderer/render_slot_shadow.h carries the witness map).
	slot_shadow_ = Object::cast_to<SlotShadow>(get_node_or_null(NodePath("SlotShadow")));
	if (slot_shadow_ != nullptr) {
		// The highest selectable retail profile is SHADOWQUALITY=3. Detail 4 is
		// an internal oversample tier (1024px slot 0 and all slots every frame),
		// not the shipped maximum; profile 3 uses 512px captures and retail's
		// half-rate stagger for non-player slots.
		slot_shadow_->set_shadow_detail(3);
		slot_shadow_->set_environment_node(env_);
	}
	// The retained water renderer starts dormant until a successful load
	// chooses its runtime mode. In particular, do not let an authored scene
	// height make initial/menu frames look underwater.
	set_water_world_rendering_enabled(false);
	// Freeze the retained weather node until a load selects autonomous
	// bare/net rendering or prepares a mission-owned fixed tick.
	set_weather_world_tick_driven(true);
	// The env presenters never self-clock: the leg table drives their
	// advance_frame at a defined ladder slot (render_environment_nodes_frame
	// and render_water_frame); the render diagnostics report that.
	env_presenters_world_driven_ = true;
}

void GameWorld::_process(double p_delta) {
	drive_.step_preload();
}

void GameWorld::_notification(int p_what) {
	if (p_what == NOTIFICATION_EXIT_TREE) {
		stop_water_render_stats();
		return;
	}
	if (p_what != NOTIFICATION_VISIBILITY_CHANGED || !is_node_ready()) {
		return;
	}
	if (world_ready_ && is_visible_in_tree()) {
		apply_scene_environment_frame();
		clear_env_generation_ = -1;
		update_frame_clear_color();
	} else {
		if (env_ != nullptr) {
			env_->set_underwater_view(false);
			env_->set_underwater_overlay_view(false);
		}
		restore_idle_frame_clear_color();
	}
}

// --- the StaticSourceProvider the two directors read -----------------------

std::vector<opennova::mission::StaticEffectSource> GameWorld::static_item_effect_sources() {
	return placer_.is_valid() ? placer_->static_item_effect_sources()
							  : std::vector<opennova::mission::StaticEffectSource>();
}

std::vector<opennova::mission::StaticLightDrawSource> GameWorld::static_light_draw_sources() {
	return placer_.is_valid() ? placer_->static_light_draw_sources()
							  : std::vector<opennova::mission::StaticLightDrawSource>();
}

uint64_t GameWorld::static_light_draw_source_revision() {
	return placer_.is_valid() ? placer_->static_light_draw_source_revision() : 0;
}

Ref<ItemDatabase> GameWorld::static_source_item_db() {
	return placer_.is_valid() ? placer_->get_item_db() : Ref<ItemDatabase>();
}

Ref<ObjectData> GameWorld::static_source_object_data(uint64_t asset_id) const {
	return placer_.is_valid() ? placer_->static_source_object_data(asset_id) : Ref<ObjectData>();
}

// --- the injection seams ---------------------------------------------------

int GameWorld::object_detail_fresh_profile() {
	return opennova::renderer::kObjectLodDetailFreshProfile;
}

int GameWorld::clamp_object_detail(int p_level) {
	return opennova::renderer::clamp_object_lod_detail(p_level);
}

int GameWorld::texfilter_level_fresh_profile() {
	return opennova::renderer::kTexFilterLevelFreshProfile;
}

int GameWorld::clamp_texfilter_level(int p_level) {
	return opennova::renderer::clamp_texfilter_level(p_level);
}

void GameWorld::set_texfilter_level(int p_level) {
	texfilter_level_ = opennova::renderer::clamp_texfilter_level(p_level);
	publish_texfilter_state();
}

// The device leg: the effects' code from the options word, the device mode's
// code and the world viewport's anisotropy from the session copy.
void GameWorld::publish_texfilter_state() {
	const opennova::renderer::TexFilterState state =
			opennova::renderer::texfilter_state(session_texfilter_level_, texfilter_level_);
	TextureFilterDevice::publish(state);
	if (is_inside_tree()) {
		TextureFilterDevice::apply_viewport(get_viewport(), state);
	}
}

int GameWorld::get_texfilter_device_mode() const {
	return opennova::renderer::texfilter_state(session_texfilter_level_, texfilter_level_).device_mode;
}

int GameWorld::get_texfilter_effect_mode() const {
	return opennova::renderer::texfilter_state(session_texfilter_level_, texfilter_level_).effect_mode;
}

int GameWorld::get_texfilter_device_filter() {
	return TextureFilterDevice::device_filter_code();
}

int GameWorld::get_texfilter_effect_filter() {
	return TextureFilterDevice::effect_filter_code();
}

void GameWorld::set_local_player_spawn_loadout(const Ref<PlayerSpawnLoadout> &p_loadout) {
	player_visuals_->set_spawn_loadout(p_loadout);
}

// --- the net-session entries -----------------------------------------------

int GameWorld::load_mission_as_host(const Ref<HostSessionOptions> &p_options) {
	return drive_.load_as_host(p_options);
}

int GameWorld::load_mission_as_joiner(const Ref<JoinTarget> &p_target) {
	return drive_.load_as_joiner(p_target);
}

String GameWorld::mount_join_expansion(const String &p_expansion) {
	return drive_.switch_join_expansion(resource_root_, p_expansion);
}

void GameWorld::adopt_novaworld_client(Node *p_client) {
	drive_.adopt_nw_client(Object::cast_to<NovaWorldClient>(p_client));
}

Node *GameWorld::release_novaworld_client() {
	return drive_.release_nw_client();
}

Ref<PostMissionRoute> GameWorld::post_mission_route(int p_reason) const {
	return drive_.post_mission_route(p_reason);
}

int GameWorld::main_frame_exit(int p_reason) const {
	Ref<Simulation> sim = get_sim();
	const bool in_session = sim.is_valid() && sim->is_mp_session();
	const bool authority = sim.is_null() || !sim->is_joiner();
	return static_cast<int>(opennova::inmatch::main_frame_exit(p_reason, in_session, authority));
}

String GameWorld::begin_map_change() {
	MissionRoot *runtime = get_runtime();
	Ref<Simulation> sim = runtime != nullptr ? runtime->get_sim() : Ref<Simulation>();
	if (sim.is_null() || sim->is_joiner() || !sim->is_mp_session()) return String();
	const String next = sim->begin_host_map_change();
	if (next.is_empty() || !drive_.keep_session(runtime)) return String();
	return next;
}

int GameWorld::load_next_mission(const String &p_bms_name) {
	return drive_.load_next_host_mission(p_bms_name);
}

bool GameWorld::begin_joiner_reload() {
	MissionRoot *runtime = get_runtime();
	Ref<Simulation> sim = runtime != nullptr ? runtime->get_sim() : Ref<Simulation>();
	if (sim.is_null() || !sim->is_joiner() || !sim->begin_joiner_reload()) return false;
	return drive_.keep_session(runtime);
}

int GameWorld::reload_joiner() {
	return drive_.reload_as_joiner();
}

Ref<ConnectionError> GameWorld::get_last_connection_error() const {
	return drive_.last_connection_error();
}

Ref<JoinScreenStatus> GameWorld::get_join_screen_status() const {
	return drive_.join_screen_status();
}

bool GameWorld::cancel_join_preload() {
	return drive_.cancel_preload();
}

bool GameWorld::cancel_join_admission() {
	return drive_.cancel_admission_wait();
}

bool GameWorld::is_net_session() const {
	Ref<Simulation> sim = get_sim();
	if (sim.is_null()) {
		return false;
	}
	return sim->is_joiner() || sim->is_host_listening();
}

void GameWorld::set_shell_paused(bool p_paused) {
	MissionRoot *runtime = get_runtime();
	if (runtime == nullptr) {
		return;
	}
	if (p_paused) {
		runtime->pause();
	} else {
		runtime->play();
	}
}

String GameWorld::missing_mission_asset_reason(const String &p_asset, const String &p_bms_name,
		const Ref<ResourceRoot> &p_resource_root, bool p_wire_header_join) const {
	if (!p_wire_header_join) {
		return vformat("%s (from %s) not found in %s", p_asset, p_bms_name,
				p_resource_root->get_root_dir());
	}
	const String mounted = p_resource_root->get_expansion();
	const String mounted_text = !mounted.is_empty() ? vformat("'%s'", mounted) : String("base game");
	return vformat("%s (named by the host's streamed mission %s) not found in %s (mounted: %s, installed: %s)",
			p_asset, p_bms_name, p_resource_root->get_root_dir(), mounted_text,
			NetSessionPolicy::describe_installed(
					p_resource_root->list_expansions(p_resource_root->get_root_dir())));
}

// --- the occlusion consumer ------------------------------------------------

// Godot's occlusion consumer is a world-level decision (a conservative second
// layer under the retail section/portal verdict, docs/render/
// render-occlusion-re.md "Conservative device occluders"); no ObjectModel
// flips viewport state. It stays OFF by default: measured 2026-08-30 through
// the "occlusion_culling" debug row (1600x900, Ryzen 7735HS iGPU, medians of
// p50 over two runs, the missions carrying 19 / 26 buildings with authored
// occluders), the occluder pass cost 0.64 / 0.59 ms of render_root_cpu
// (frame 14.47 -> 13.55 ms on 00TRa, 14.50 -> 13.68 ms on CP01) and culled
// nothing (487 -> 489 and 416 -> 416 root draw calls) because the retail
// section verdict already hides what the OOBJ faces would. A mission load
// and an unload both re-apply the default; the debug row switches the pass
// on live while the mission carries occluders.
void GameWorld::apply_occlusion_culling_policy() {
	set_occlusion_culling_enabled(false);
}

void GameWorld::set_occlusion_culling_enabled(bool p_enabled) {
	Viewport *viewport = is_inside_tree() ? get_viewport() : nullptr;
	if (viewport == nullptr) {
		return;
	}
	viewport->set_use_occlusion_culling(
			p_enabled && RenderingServer::get_singleton()->get_rendering_device() != nullptr);
}

bool GameWorld::is_occlusion_culling_enabled() const {
	Viewport *viewport = is_inside_tree() ? get_viewport() : nullptr;
	return viewport != nullptr && viewport->is_using_occlusion_culling();
}

int GameWorld::get_authored_occluder_model_count() const {
	return mission_stats_.is_valid() ? mission_stats_->get_authored_occluder_models() : 0;
}

// --- the read seams --------------------------------------------------------

MissionRoot *GameWorld::get_runtime() const {
	return Object::cast_to<MissionRoot>(ObjectDB::get_instance(runtime_id_));
}

Ref<Simulation> GameWorld::get_sim() const {
	MissionRoot *runtime = get_runtime();
	return runtime != nullptr ? runtime->get_sim() : Ref<Simulation>();
}

Ref<WeaponDatabase> GameWorld::get_weapon_database() {
	if (weapon_db_.is_null()) {
		if (resource_root_.is_null()) {
			return Ref<WeaponDatabase>();
		}
		weapon_db_.instantiate();
		if (weapon_db_->load_from_resource_root(resource_root_, "weapon.def") != OK) {
			UtilityFunctions::push_warning(vformat(
					"GameWorld: weapon.def unavailable (%s) — weapon presentation/loadout lookup disabled",
					weapon_db_->get_last_error()));
			return Ref<WeaponDatabase>();
		}
	}
	return weapon_db_->is_loaded() ? weapon_db_ : Ref<WeaponDatabase>();
}

int GameWorld::get_static_live_population_count() const {
	return placer_.is_valid() ? placer_->get_static_live_population_count() : 0;
}

void GameWorld::release_runtime_renderer_resources() {
	// FrameFx publishes Q3 frames that retain sampled producer resources.
	// Drain its compositor callback before Water releases those source
	// textures.
	if (framefx_ != nullptr) {
		framefx_->shutdown();
	}
	if (water_ != nullptr) {
		water_->release_runtime_renderer_resources();
	}
}

bool GameWorld::is_water_render_active() const {
	return water_ != nullptr && water_->is_water_render_active();
}

Ref<WorldView> GameWorld::world_view() {
	Ref<LiveWorldView> view;
	view.instantiate();
	view->bind(this);
	return view;
}

Ref<ArmoryWorldView> GameWorld::armory_view() {
	Ref<LiveArmoryWorldView> view;
	view.instantiate();
	view->bind(this);
	return view;
}

Color GameWorld::get_current_frame_clear_color() const {
	if (clear_color_ == nullptr || clear_color_->get_environment().is_null()) {
		return Color(0, 0, 0);
	}
	return clear_color_->get_environment()->get_bg_color();
}

// --- the frame stats / perf probe ------------------------------------------

void GameWorld::set_music_director(MusicDirector *director) {
    music_director_id_ = director ? ObjectID(director->get_instance_id()) : ObjectID();
}

MusicDirector *GameWorld::get_music_director() const {
    return Object::cast_to<MusicDirector>(ObjectDB::get_instance(music_director_id_));
}

void GameWorld::set_frame_stats(const Ref<FrameStats> &p_board) {
	if (p_board == frame_stats_) {
		return;
	}
	const Callable capture_changed = callable_mp(this, &GameWorld::on_frame_stats_capture_changed);
	if (frame_stats_.is_valid()) {
		if (frame_stats_->is_connected(kSignalCaptureChanged, capture_changed)) {
			frame_stats_->disconnect(kSignalCaptureChanged, capture_changed);
		}
	}
	stop_water_render_stats();
	frame_stats_ = p_board;
	if (frame_stats_.is_valid()) {
		if (!frame_stats_->is_connected(kSignalCaptureChanged, capture_changed)) {
			frame_stats_->connect(kSignalCaptureChanged, capture_changed);
		}
	}
	occlusion_->set_frame_stats(p_board);
	MissionRoot *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->set_frame_stats(p_board);
	}
}

void GameWorld::on_frame_stats_capture_changed(bool p_active) {
	if (!p_active) {
		stop_water_render_stats();
	}
}

void GameWorld::set_perf_probe_enabled(bool p_enabled) {
	perf_probe_enabled_ = p_enabled;
	// The frame shares the probe's timing gate (see OcclusionFrame::set_probe_timing).
	occlusion_->set_probe_timing(p_enabled);
	if (!p_enabled) {
		perf_probe_skip_occl_ = false;
		perf_probe_skip_effect_tick_ = false;
		perf_probe_skip_fixed_handlers_ = false;
		perf_probe_occlusion_skipped_ = false;
	}
	sync_runtime_profiling();
}

void GameWorld::sync_runtime_profiling() {
	MissionRoot *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->set_runtime_profiling_enabled(perf_probe_enabled_);
	}
}

void GameWorld::set_local_view_presenter(LocalPlayerPresenter *p_presenter) {
	local_view_presenter_id_ = p_presenter != nullptr ? ObjectID(p_presenter->get_instance_id())
													   : ObjectID();
}

LocalPlayerPresenter *GameWorld::local_view_presenter() const {
	return Object::cast_to<LocalPlayerPresenter>(ObjectDB::get_instance(local_view_presenter_id_));
}

void GameWorld::set_inset_scope(HudInsetScope *p_scope) {
	inset_scope_id_ = p_scope != nullptr ? ObjectID(p_scope->get_instance_id()) : ObjectID();
}

Ref<EffectLightReport> GameWorld::get_effect_light_report() const {
	return light_director_.is_valid() ? light_director_->get_report() : Ref<EffectLightReport>();
}

void GameWorld::session_frame_failed(const String &p_reason) {
	const String message = !p_reason.is_empty() ? p_reason : String("mission session lost");
	emit_signal(kSignalSessionLost, message);
}

Ref<FirePresentStats> GameWorld::get_fire_present_stats() const {
	MissionRoot *runtime = get_runtime();
	return runtime != nullptr ? runtime->get_fire_present_stats() : Ref<FirePresentStats>();
}

Ref<ScarPresentStats> GameWorld::get_scar_present_stats() const {
	MissionRoot *runtime = get_runtime();
	return runtime != nullptr ? runtime->get_scar_present_stats() : Ref<ScarPresentStats>();
}

// --- local-player visuals ---------------------------------------------------

int GameWorld::local_player_character_id() const {
	return player_visuals_->local_player_character_id();
}

bool GameWorld::set_local_player_weapon_by_name(const String &p_weapon_name, bool p_preserve_slot_state) {
	return player_visuals_->set_local_player_weapon_by_name(p_weapon_name, p_preserve_slot_state);
}

void GameWorld::clear_local_player_weapon() {
	player_visuals_->clear_local_player_weapon();
}

bool GameWorld::local_player_first_person_model_available() const {
	Ref<Simulation> sim = get_sim();
	return sim.is_valid() && sim->is_local_player_first_person_model_available();
}

PackedStringArray GameWorld::prewarm_challenge_models() {
	return player_visuals_->prewarm_loaded_model_challenge_definitions();
}

Node3D *GameWorld::build_local_player_viewmodel() {
	return player_visuals_->build_local_player_viewmodel();
}

TypedArray<ObjectModel> GameWorld::local_player_viewmodel_parts() const {
	return player_visuals_->local_player_viewmodel_parts();
}

Ref<FirstPersonArmsWitness> GameWorld::local_player_first_person_arms_witness() {
	return player_visuals_->local_player_first_person_arms_witness();
}

String GameWorld::local_player_weapon_name() const {
	return player_visuals_->local_player_weapon_name();
}

Ref<PlayerLocalView> GameWorld::local_player_view() const {
	return player_visuals_->local_player_view();
}

Ref<PlayerHudWeaponDef> GameWorld::local_player_hud_weapon_def() const {
	return PlayerHudWeaponDef::from_weapon_def(player_visuals_->local_weapon());
}

Ref<PlayerWeaponView> GameWorld::local_player_weapon_view() const {
	return player_visuals_->local_player_weapon_view();
}

TypedArray<PlayerWeaponEvent> GameWorld::drain_local_player_weapon_events() {
	return player_visuals_->drain_local_player_weapon_events();
}

Ref<PlayerViewmodelDef> GameWorld::local_player_viewmodel_def() {
	return player_visuals_->local_player_viewmodel_def();
}

// --- the dev tools' seams ---------------------------------------------------

void GameWorld::set_particles_hidden(bool p_hidden) {
	item_fx_->set_particles_hidden(p_hidden);
}

bool GameWorld::is_particles_hidden() const {
	return item_fx_->particles_hidden();
}

void GameWorld::set_foliage_hidden(bool p_hidden) {
	foliage_hidden_ = p_hidden;
	if (dispatcher_ != nullptr) {
		dispatcher_->set_visible(!p_hidden);
	}
}

EffectWorld *GameWorld::get_effect_world() const {
	return Object::cast_to<EffectWorld>(ObjectDB::get_instance(effect_world_id_));
}

Ref<ItemDatabase> GameWorld::get_item_db() const {
	return placer_.is_valid() ? placer_->get_item_db() : Ref<ItemDatabase>();
}

double GameWorld::get_debug_mission_minute_of_day() const {
	if (env_ == nullptr || !env_->is_loaded()) {
		return 0.0;
	}
	return env_->get_mission_minute_of_day();
}

Error GameWorld::debug_set_mission_minute_of_day(double p_minute_of_day) {
	if (env_ == nullptr || !env_->is_loaded()) {
		return ERR_UNAVAILABLE;
	}
	Ref<Simulation> sim = get_sim();
	Error err = OK;
	if (sim.is_valid() && sim->weather_state_bound()) {
		if (!sim->debug_set_time_of_day_minutes(p_minute_of_day)) {
			err = ERR_UNAVAILABLE;
		}
	} else {
		err = env_->debug_set_mission_minute_of_day(p_minute_of_day);
	}
	if (err != OK) {
		return err;
	}
	Weather *weather = get_weather_node();
	if (weather != nullptr) {
		weather->resync_colors_now();
	}
	MissionAudio *audio = get_mission_audio();
	if (audio != nullptr) {
		audio->set_time_of_day_hhmm(env_->get_time_of_day());
	}
	return OK;
}

MissionAudio *GameWorld::get_mission_audio() const {
	return Object::cast_to<MissionAudio>(ObjectDB::get_instance(mission_audio_id_));
}

void GameWorld::on_nw_host_server_command(const String &p_verb, const String &p_target,
		const PackedStringArray &p_args) {
	drive_.on_nw_host_server_command(p_verb, p_target, p_args);
}

void GameWorld::on_nw_host_player_enter_result(int64_t p_connection_id, int p_success,
		int p_msg_code, const String &p_player_ticket, const String &p_access_code_list) {
	drive_.on_nw_host_player_enter_result(p_connection_id, p_success, p_msg_code,
			p_player_ticket, p_access_code_list);
}

// --- bindings ----------------------------------------------------------------

void GameWorld::_bind_methods() {
	ADD_SIGNAL(MethodInfo(kSignalWorldLoaded));
	ADD_SIGNAL(MethodInfo(kSignalLoadFailed, PropertyInfo(Variant::STRING, "reason")));
	// A joiner's authoritative session record (post-auth S2C 0x7B) resolved
	// during the pre-load wait: server/mission names + the exact wire-header
	// world about to be constructed. The shell refreshes its loading screen
	// from this -- retail's connect stream fills the same session vars before
	// its header-backed load [orig: Client_ParseServerSessionVariables @ 0x5202f0].
	ADD_SIGNAL(MethodInfo(kSignalJoinSessionIdentified,
			PropertyInfo(Variant::OBJECT, "info", PROPERTY_HINT_RESOURCE_TYPE, "LoadingScreenInfo")));
	// A joiner crossed the authoritative admission edge. Wire-header world
	// load completion is intentionally separate: the shell keeps the loading
	// presentation raised until this edge (or until the host requests a
	// deployment-zone pick).
	ADD_SIGNAL(MethodInfo(kSignalJoinAdmissionReady));
	// A pick-required join reached the player-paced deployment stage: the
	// host granted the loadouts and holds this player respawn-pending until a
	// deploy pick. The shell opens the DEATH deploy screen; the join watchdog
	// has stopped (everything past this point is player-paced). [orig: 0x0A
	// flags1 bit1 -> the DEATH screen; net-re 5.61]
	ADD_SIGNAL(MethodInfo(kSignalJoinDeployPickRequired));
	// An ESTABLISHED in-match session went silent past the witnessed
	// connection reap window (JO cs_dir0.timeout_ms = 120000 ms). Retail does
	// not raise an in-world dialog for this: its transport reaps the peer and
	// the disconnect event maps an error code onto g_MissionExitReason, i.e.
	// it EXITS THE MISSION with a reason. The shell's analog is return-to-menu
	// with `reason` surfaced the same way a join failure is. Emitted at most
	// ONCE per session.
	// [orig: CNapiNetwork_Init @ 0x4ca4a0 (timeout stores @ 0x4caa81/@ 0x4cab54) ->
	//  CNapiNetwork_OnDisconnectedFromServer @ 0x4c63d0]
	ADD_SIGNAL(MethodInfo(kSignalSessionLost, PropertyInfo(Variant::STRING, "reason")));
	// Mission-load progress, 0..100, emitted when each stage of the engine's
	// mission load plan starts (MissionData.load_progress_percent -- the
	// witnessed per-stage anchors, engine/runtime/mission/mission_load_plan.h)
	// and pulsed at the object stage's value from inside the placement loop.
	ADD_SIGNAL(MethodInfo(kSignalLoadProgress, PropertyInfo(Variant::INT, "percent")));
	// Presentation side effects drained from the mission runtime's EffectLog
	// each tick (kind: "text"/"debug_text"/"win"/"subgoal_*"/"show_waypoints"/
	// "set_light"/"dialog"). Player text is consumed by the HUD; debug_text
	// remains a distinct unrouted channel. "dialog" is also routed straight
	// to mission audio.
	ADD_SIGNAL(MethodInfo(kSignalMissionEffects, PropertyInfo(Variant::ARRAY, "effects")));
	// The CPU-built gameplay-map depthspin water mask changed.
	ADD_SIGNAL(MethodInfo(kSignalMinimapWaterChanged,
			PropertyInfo(Variant::OBJECT, "mask", PROPERTY_HINT_RESOURCE_TYPE, "ImageTexture")));
	// The one interactive-music context is the shell's (MusicService): the
	// world names the points where the original opens the GAME context at
	// mission start, tears it down at mission end, and re-drives the gamemus
	// vars from the local player each frame; MainGame connects them.
	ADD_SIGNAL(MethodInfo(kSignalMusicContextOpened,
			PropertyInfo(Variant::OBJECT, "root", PROPERTY_HINT_RESOURCE_TYPE, "ResourceRoot")));
	ADD_SIGNAL(MethodInfo(kSignalMusicContextClosed));
	ADD_SIGNAL(MethodInfo(kSignalMusicVarChanged, PropertyInfo(Variant::INT, "slot"),
			PropertyInfo(Variant::INT, "value")));

	ClassDB::bind_method(D_METHOD("set_mission_file", "value"), &GameWorld::set_mission_file);
	ClassDB::bind_method(D_METHOD("get_mission_file"), &GameWorld::get_mission_file);
	ClassDB::bind_method(D_METHOD("set_terrain_file", "value"), &GameWorld::set_terrain_file);
	ClassDB::bind_method(D_METHOD("get_terrain_file"), &GameWorld::get_terrain_file);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "mission_file"), "set_mission_file", "get_mission_file");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "terrain_file"), "set_terrain_file", "get_terrain_file");

	ClassDB::bind_method(D_METHOD("set_resource_root", "root"), &GameWorld::set_resource_root);
	ClassDB::bind_method(D_METHOD("set_resource_root_resolver", "resolver"),
			&GameWorld::set_resource_root_resolver);
	ClassDB::bind_method(D_METHOD("set_local_player_spawn_loadout", "loadout"),
			&GameWorld::set_local_player_spawn_loadout);
	ClassDB::bind_method(D_METHOD("set_playable", "enabled"), &GameWorld::set_playable);
	ClassDB::bind_method(D_METHOD("is_playable"), &GameWorld::is_playable);
	ClassDB::bind_method(D_METHOD("set_object_polydetail", "level"),
			&GameWorld::set_object_polydetail);
	ClassDB::bind_method(D_METHOD("get_object_polydetail"), &GameWorld::get_object_polydetail);
	ClassDB::bind_method(D_METHOD("get_object_detail"), &GameWorld::get_object_detail);
	ClassDB::bind_static_method("GameWorld", D_METHOD("object_detail_fresh_profile"),
			&GameWorld::object_detail_fresh_profile);
	ClassDB::bind_static_method("GameWorld", D_METHOD("clamp_object_detail", "level"),
			&GameWorld::clamp_object_detail);
	ClassDB::bind_method(D_METHOD("set_texfilter_level", "level"), &GameWorld::set_texfilter_level);
	ClassDB::bind_method(D_METHOD("get_texfilter_level"), &GameWorld::get_texfilter_level);
	ClassDB::bind_method(D_METHOD("get_session_texfilter_level"),
			&GameWorld::get_session_texfilter_level);
	ClassDB::bind_method(D_METHOD("get_texfilter_device_mode"), &GameWorld::get_texfilter_device_mode);
	ClassDB::bind_method(D_METHOD("get_texfilter_effect_mode"), &GameWorld::get_texfilter_effect_mode);
	ClassDB::bind_static_method("GameWorld", D_METHOD("get_texfilter_device_filter"),
			&GameWorld::get_texfilter_device_filter);
	ClassDB::bind_static_method("GameWorld", D_METHOD("get_texfilter_effect_filter"),
			&GameWorld::get_texfilter_effect_filter);
	ClassDB::bind_static_method("GameWorld", D_METHOD("texfilter_level_fresh_profile"),
			&GameWorld::texfilter_level_fresh_profile);
	ClassDB::bind_static_method("GameWorld", D_METHOD("clamp_texfilter_level", "level"),
			&GameWorld::clamp_texfilter_level);

	ClassDB::bind_method(D_METHOD("load_world", "dir"), &GameWorld::load_world, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("load_mission", "bms_name", "dir"), &GameWorld::load_mission,
			DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("load_loose_mission", "bms_name", "dir"),
			&GameWorld::load_loose_mission, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("load_mission_as_host", "options"), &GameWorld::load_mission_as_host);
	ClassDB::bind_method(D_METHOD("load_mission_as_joiner", "target"), &GameWorld::load_mission_as_joiner);
	ClassDB::bind_method(D_METHOD("adopt_novaworld_client", "client"), &GameWorld::adopt_novaworld_client);
	ClassDB::bind_method(D_METHOD("mount_join_expansion", "expansion"), &GameWorld::mount_join_expansion);
	ClassDB::bind_method(D_METHOD("release_novaworld_client"), &GameWorld::release_novaworld_client);
	ClassDB::bind_method(D_METHOD("post_mission_route", "reason"), &GameWorld::post_mission_route);
	ClassDB::bind_method(D_METHOD("begin_map_change"), &GameWorld::begin_map_change);
	ClassDB::bind_method(D_METHOD("load_next_mission", "bms_name"), &GameWorld::load_next_mission);
	ClassDB::bind_method(D_METHOD("begin_joiner_reload"), &GameWorld::begin_joiner_reload);
	ClassDB::bind_method(D_METHOD("reload_joiner"), &GameWorld::reload_joiner);
	ClassDB::bind_method(D_METHOD("get_last_connection_error"), &GameWorld::get_last_connection_error);
	ClassDB::bind_method(D_METHOD("get_join_screen_status"), &GameWorld::get_join_screen_status);
	// The exit reasons the shell's own exits store (engine: inmatch/mission_exit.h).
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MISSION_EXIT_QUIT",
			opennova::inmatch::kMissionExitQuit);
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MISSION_EXIT_MAP_CYCLE",
			opennova::inmatch::kMissionExitMapCycle);
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MISSION_EXIT_ROUND_OVER",
			opennova::inmatch::kMissionExitRoundOver);
	ClassDB::bind_method(D_METHOD("main_frame_exit", "reason"), &GameWorld::main_frame_exit);
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MAIN_FRAME_EXIT_NONE",
			static_cast<int>(opennova::inmatch::MainFrameExit::None));
	ClassDB::bind_integer_constant(get_class_static(), StringName(),
			"MAIN_FRAME_EXIT_RESTART_ROUND_SP",
			static_cast<int>(opennova::inmatch::MainFrameExit::RestartRoundSP));
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MAIN_FRAME_EXIT_GAME_LOOP",
			static_cast<int>(opennova::inmatch::MainFrameExit::GameLoop));
	ClassDB::bind_integer_constant(get_class_static(), StringName(), "MAIN_FRAME_EXIT_POST_MENU",
			static_cast<int>(opennova::inmatch::MainFrameExit::PostMenu));
	ClassDB::bind_method(D_METHOD("load_mission_data", "mission", "bms_name", "dir"),
			&GameWorld::load_mission_data, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("cancel_join_preload"), &GameWorld::cancel_join_preload);
	ClassDB::bind_method(D_METHOD("cancel_join_admission"), &GameWorld::cancel_join_admission);
	ClassDB::bind_method(D_METHOD("is_net_session"), &GameWorld::is_net_session);
	ClassDB::bind_method(D_METHOD("set_shell_paused", "paused"), &GameWorld::set_shell_paused);
	ClassDB::bind_method(D_METHOD("missing_mission_asset_reason", "asset", "bms_name",
			"resource_root", "wire_header_join"), &GameWorld::missing_mission_asset_reason);
	ClassDB::bind_method(D_METHOD("unload"), &GameWorld::unload);
	ClassDB::bind_method(D_METHOD("report_join_wire_asset_failure", "reason"),
			&GameWorld::report_join_wire_asset_failure);
	ClassDB::bind_method(D_METHOD("build_minimap_water_mask"), &GameWorld::build_minimap_water_mask);

	ClassDB::bind_method(D_METHOD("set_occlusion_culling_enabled", "enabled"),
			&GameWorld::set_occlusion_culling_enabled);
	ClassDB::bind_method(D_METHOD("is_occlusion_culling_enabled"),
			&GameWorld::is_occlusion_culling_enabled);
	ClassDB::bind_method(D_METHOD("get_authored_occluder_model_count"),
			&GameWorld::get_authored_occluder_model_count);

	ClassDB::bind_method(D_METHOD("get_loaded_mission"), &GameWorld::get_loaded_mission);
	ClassDB::bind_method(D_METHOD("get_loaded_mission_file"), &GameWorld::get_loaded_mission_file);
	ClassDB::bind_method(D_METHOD("get_sim"), &GameWorld::get_sim);
	ClassDB::bind_method(D_METHOD("get_weapon_database"), &GameWorld::get_weapon_database);
	ClassDB::bind_method(D_METHOD("last_load_timeline"), &GameWorld::last_load_timeline);
	ClassDB::bind_method(D_METHOD("get_runtime"), &GameWorld::get_runtime);
	ClassDB::bind_method(D_METHOD("get_mission_stats"), &GameWorld::get_mission_stats);
	ClassDB::bind_method(D_METHOD("get_static_live_population_count"),
			&GameWorld::get_static_live_population_count);
	ClassDB::bind_method(D_METHOD("release_runtime_renderer_resources"),
			&GameWorld::release_runtime_renderer_resources);
	ClassDB::bind_method(D_METHOD("is_water_render_active"), &GameWorld::is_water_render_active);
	ClassDB::bind_method(D_METHOD("get_terrain_data"), &GameWorld::get_terrain_data);
	ClassDB::bind_method(D_METHOD("get_resource_root"), &GameWorld::get_resource_root);
	ClassDB::bind_method(D_METHOD("get_frame_fx"), &GameWorld::get_frame_fx);
	ClassDB::bind_method(D_METHOD("world_view"), &GameWorld::world_view);
	ClassDB::bind_method(D_METHOD("armory_view"), &GameWorld::armory_view);
	ClassDB::bind_method(D_METHOD("is_loaded"), &GameWorld::is_loaded);
	ClassDB::bind_method(D_METHOD("get_current_frame_clear_color"),
			&GameWorld::get_current_frame_clear_color);

	ClassDB::bind_method(D_METHOD("set_music_director", "director"), &GameWorld::set_music_director);
	ClassDB::bind_method(D_METHOD("set_player_profiles", "profiles"), &GameWorld::set_player_profiles);
	ClassDB::bind_method(D_METHOD("set_frame_stats", "board"), &GameWorld::set_frame_stats);
	ClassDB::bind_method(D_METHOD("is_water_render_stats_measured"),
			&GameWorld::is_water_render_stats_measured);
	ClassDB::bind_method(D_METHOD("set_perf_probe_enabled", "enabled"), &GameWorld::set_perf_probe_enabled);
	ClassDB::bind_method(D_METHOD("set_perf_probe_skip_occlusion", "skip"),
			&GameWorld::set_perf_probe_skip_occlusion);
	ClassDB::bind_method(D_METHOD("set_perf_probe_skip_effect_tick", "skip"),
			&GameWorld::set_perf_probe_skip_effect_tick);
	ClassDB::bind_method(D_METHOD("set_perf_probe_skip_fixed_handlers", "skip"),
			&GameWorld::set_perf_probe_skip_fixed_handlers);
	ClassDB::bind_method(D_METHOD("set_local_view_presenter", "presenter"),
			&GameWorld::set_local_view_presenter);
	ClassDB::bind_method(D_METHOD("local_view_presenter"), &GameWorld::local_view_presenter);
	ClassDB::bind_method(D_METHOD("set_inset_scope", "scope"), &GameWorld::set_inset_scope);
	ClassDB::bind_method(D_METHOD("get_effect_light_report"), &GameWorld::get_effect_light_report);
	ClassDB::bind_method(D_METHOD("get_effect_light_director"),
			&GameWorld::get_effect_light_director);
	ClassDB::bind_method(D_METHOD("get_seconds_since_render"), &GameWorld::get_seconds_since_render);
	ClassDB::bind_method(D_METHOD("tick", "camera_pos", "camera_xform", "delta", "frame_input"),
			&GameWorld::tick, DEFVAL(Transform3D()), DEFVAL(-1.0), DEFVAL(Variant()));
	ClassDB::bind_static_method("GameWorld", D_METHOD("current_frame_clock_ms"),
			&GameWorld::current_frame_clock_ms);
	ClassDB::bind_method(D_METHOD("get_frame_clock_ms"), &GameWorld::get_frame_clock_ms);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frame_clock_ms"), "", "get_frame_clock_ms");
	ClassDB::bind_static_method("GameWorld", D_METHOD("frame_leg_names"), &GameWorld::frame_leg_names);
	ClassDB::bind_static_method("GameWorld", D_METHOD("frozen_pose_leg_names"),
			&GameWorld::frozen_pose_leg_names);
	ClassDB::bind_static_method("GameWorld", D_METHOD("frame_leg_stops_frame", "name"),
			&GameWorld::frame_leg_stops_frame);
	ClassDB::bind_method(D_METHOD("debug_refresh_render_pose", "camera"),
			&GameWorld::debug_refresh_render_pose);
	ClassDB::bind_method(D_METHOD("get_runtime_perf_counters"), &GameWorld::get_runtime_perf_counters);
	ClassDB::bind_method(D_METHOD("get_fire_present_stats"), &GameWorld::get_fire_present_stats);
	ClassDB::bind_method(D_METHOD("get_scar_present_stats"), &GameWorld::get_scar_present_stats);

	ClassDB::bind_method(D_METHOD("render_environment_nodes_frame"),
			&GameWorld::render_environment_nodes_frame);
	ClassDB::bind_method(D_METHOD("render_light_frame"), &GameWorld::render_light_frame);
	ClassDB::bind_method(D_METHOD("render_particle_frame"), &GameWorld::render_particle_frame);

	ClassDB::bind_method(D_METHOD("local_player_visuals"), &GameWorld::local_player_visuals);
	ClassDB::bind_method(D_METHOD("local_player_character_id"), &GameWorld::local_player_character_id);
	ClassDB::bind_method(D_METHOD("set_local_player_weapon_by_name", "weapon_name", "preserve_slot_state"),
			&GameWorld::set_local_player_weapon_by_name, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_local_player_weapon"), &GameWorld::clear_local_player_weapon);
	ClassDB::bind_method(D_METHOD("local_player_first_person_model_available"),
			&GameWorld::local_player_first_person_model_available);
	ClassDB::bind_method(D_METHOD("prewarm_challenge_models"), &GameWorld::prewarm_challenge_models);
	ClassDB::bind_method(D_METHOD("build_local_player_viewmodel"), &GameWorld::build_local_player_viewmodel);
	ClassDB::bind_method(D_METHOD("local_player_viewmodel_parts"), &GameWorld::local_player_viewmodel_parts);
	ClassDB::bind_method(D_METHOD("local_player_first_person_arms_witness"),
			&GameWorld::local_player_first_person_arms_witness);
	ClassDB::bind_method(D_METHOD("local_player_weapon_name"), &GameWorld::local_player_weapon_name);
	ClassDB::bind_method(D_METHOD("local_player_view"), &GameWorld::local_player_view);
	ClassDB::bind_method(D_METHOD("local_player_hud_weapon_def"), &GameWorld::local_player_hud_weapon_def);
	ClassDB::bind_method(D_METHOD("local_player_weapon_view"), &GameWorld::local_player_weapon_view);
	ClassDB::bind_method(D_METHOD("drain_local_player_weapon_events"),
			&GameWorld::drain_local_player_weapon_events);
	ClassDB::bind_method(D_METHOD("local_player_viewmodel_def"), &GameWorld::local_player_viewmodel_def);

	ClassDB::bind_method(D_METHOD("set_particles_hidden", "hidden"), &GameWorld::set_particles_hidden);
	ClassDB::bind_method(D_METHOD("is_particles_hidden"), &GameWorld::is_particles_hidden);
	ClassDB::bind_method(D_METHOD("set_foliage_hidden", "hidden"), &GameWorld::set_foliage_hidden);
	ClassDB::bind_method(D_METHOD("is_foliage_hidden"), &GameWorld::is_foliage_hidden);
	ClassDB::bind_method(D_METHOD("get_minimap_water_mask"), &GameWorld::get_minimap_water_mask);
	ClassDB::bind_method(D_METHOD("get_effect_world"), &GameWorld::get_effect_world);
	ClassDB::bind_method(D_METHOD("get_item_effect_director"), &GameWorld::get_item_effect_director);
	ClassDB::bind_method(D_METHOD("get_terrain_node"), &GameWorld::get_terrain_node);
	ClassDB::bind_method(D_METHOD("get_foliage_dispatcher"), &GameWorld::get_foliage_dispatcher);
	ClassDB::bind_method(D_METHOD("get_item_db"), &GameWorld::get_item_db);
	ClassDB::bind_method(D_METHOD("get_environment_node"), &GameWorld::get_environment_node);
	ClassDB::bind_method(D_METHOD("get_weather_node"), &GameWorld::get_weather_node);
	ClassDB::bind_method(D_METHOD("debug_set_mission_minute_of_day", "minute_of_day"),
			&GameWorld::debug_set_mission_minute_of_day);
	ClassDB::bind_method(D_METHOD("get_water_node"), &GameWorld::get_water_node);
	ClassDB::bind_method(D_METHOD("get_celestial_node"), &GameWorld::get_celestial_node);
	ClassDB::bind_method(D_METHOD("get_sky_dome_node"), &GameWorld::get_sky_dome_node);
	ClassDB::bind_method(D_METHOD("get_sun_shadow_node"), &GameWorld::get_sun_shadow_node);
	ClassDB::bind_method(D_METHOD("drives_environment_presenters"),
			&GameWorld::drives_environment_presenters);
	ClassDB::bind_method(D_METHOD("get_clear_color_node"), &GameWorld::get_clear_color_node);
	ClassDB::bind_method(D_METHOD("get_mission_audio"), &GameWorld::get_mission_audio);

	ClassDB::bind_method(D_METHOD("on_wire_node_spawned", "node", "kind", "item_id"),
			&GameWorld::on_wire_node_spawned);
}
