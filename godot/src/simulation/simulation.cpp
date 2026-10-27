// Simulation — core lifecycle: construction/reset, mission load + step,
// the kernel-boot binding legs, restart, WAC install/state, mission
// variables, perf counters.
// The class spans several TUs; see simulation_internal.h for the map.
#include "simulation/simulation_internal.h"
#include "audio/music_director.h"
#include <godot_cpp/core/object.hpp>
#include "simulation/environment_snapshot.h" // the F3 Environment record as a typed read
#include "simulation/hud_view_records.h" // RoundOutcome
#include "simulation/present_event_records.h" // SoundEmitterRow (the bound emitter drain)

#include "env/weather.h"
#include "util/string_convert.h"
#include <runtime/devtools/scripted_input.h> // ScriptPose (the joiner setup pose)
#include <runtime/environment/environment_state.h>
#include <base/io/fixed.h>
#include <base/gameprofile/game_type.h> // game_type::for_mission_attribs

#include <runtime/inmatch/host_settings.h> // host_player_slot_limit (the BMS admission limit)
#include <runtime/inmatch/slot_staging.h> // stage_slot_position (the parity runner's slot staging)
#include <runtime/mission/mission_sidecars.h> // mission_base_name
#include <runtime/mission/runtime_boot.h> // the S9 boot order + file-resolution policy
#include <runtime/terrain_query/terrain_field_build.h> // the ONE cpt/trn(+charmap) field builder (ADR 0042 d4)

using namespace sim_internal;

namespace {

// The BMS admission limits (promote.cpp's player_limit/team_count gates). A
// host admits against its live slot limit (host_player_slot_limit over the
// configure_host_session clamp, net.host_max_players: a dedicated host's own
// slot included); the no-net world keeps the session config's slot limit
// (admission ignores it outside a session). A joiner's legacy full-BMS load
// (tools and direct joins; the production joiner boots a wire-header-only
// mission with nothing to admit) takes the joined session's values once its
// pre-load burst has delivered them: the cap the host published in its S2C
// 0x64 session block (the session's cap: a dedicated host, retail's and ours,
// publishes it while admitting against cap+1, so this leg admits against one
// fewer there) and the replicated scoreboard team count; else the
// host-equivalent defaults. Retail joiners never promote the .bms (admission
// is host-only), so the joiner leg is a port decision with no binary witness.
void stamp_admission_limits(opennova::mission::KernelBootOptions &options,
		const godot::SimulationNetState &net, bool host_listening,
		const opennova::inmatch::ClientRuntime *joined) {
	// The cfg block's Shadow Quality word, which gates the face slots (engine
	// KernelBootOptions::shadow_quality carries the witness).
	if (net.game_config.is_valid())
		options.shadow_quality = net.game_config->native().cfg().shadow_quality;
	options.player_limit = host_listening
			? static_cast<int32_t>(opennova::inmatch::host_player_slot_limit(
					static_cast<int32_t>(net.host_max_players), net.host_serve_and_play,
					net.host_session_config.title))
			: static_cast<int32_t>(net.host_session_config.player_slot_limit());
	options.team_count = net.host_session_config.num_teams;
	if (!options.joiner || joined == nullptr) return;
	if (joined->session_max_players() != 0)
		options.player_limit = static_cast<int32_t>(joined->session_max_players());
	if (joined->state().scoreboard.team_count != 0)
		options.team_count = joined->state().scoreboard.team_count;
}

// The bring-up's session create refused on the mpreset word
// (inmatch::HostRole::session_create): the in-memory loads fail as the file
// boot's phase A does (inmatch::HostBoot::session_create), and
// session_create_ended_process() tells the embedder, which ends the process
// with exit code 0 (the game shell's GameWorld.process_exit_requested).
bool session_create_refused(const opennova::inmatch::HostRole *p_role, std::string &r_error) {
	if (p_role == nullptr ||
			p_role->session_create() != opennova::inmatch::CreateSessionResult::ProcessExit)
		return false;
	r_error = "mpreset is set: the session create ends the process (exit code 0)";
	return true;
}

} // namespace

bool Simulation::session_create_ended_process() const {
	return host_role_ != nullptr &&
			host_role_->session_create() == opennova::inmatch::CreateSessionResult::ProcessExit;
}

Simulation::Simulation() {
	session_.set_tick_observer(this);
	// The bare local role from construction; the shell's session choices
	// (enable_listen_server / enable_host_listen / enable_join) replace it.
	(void)install_role(std::make_unique<opennova::inmatch::LocalRole>());
	reset_world();
}

Simulation::~Simulation() {
	_release_weather_owner();
	(void)session_.close();
}

void Simulation::reset_world() {
	// The bound Weather node points into this kernel's World (its
	// WeatherState is the environment's live view): release it before the
	// kernel is replaced.
	_release_weather_owner();
	if (joiner_role_ != nullptr) joiner_role_->reset_world_stream();
	invalidate_present_effect_pose_cache();
	// A fresh EntityRegistry restarts its spawn ids at 1, so the per-handle
	// dead/respawn mirrors cannot tell the next mission's occupant apart
	// from this one's by epoch: the lifecycle table dies with the world.
	present_.pool_lifecycle.clear();
	// One fresh kernel per load (ADR 0042 d3): the world, its systems, the
	// sim asset caches, the local-player weapon/loadout/view state and the
	// terrain field store all reset inside it. Retail reloads its model cache
	// per mission too, so the parse-once caches dying here is faithful. The
	// retained pieces cross over through the kernel's own carry
	// (MissionKernel::carry_across_load_from) before the old kernel dies.
	auto next_kernel = std::make_unique<opennova::mission::MissionKernel>();
	if (kernel_ != nullptr)
		next_kernel->carry_across_load_from(*kernel_);
	kernel_ = std::move(next_kernel);
	role_->bind(*kernel_);
	// The retained .til (set_terrain_til_data, the boot's) onto the fresh
	// store's placed-tile overlay; a file boot replaces it with its own.
	kernel_->set_placed_tiles(net_.terrain_til_data);
	kernel_->set_assets(
			assets_.root.is_valid() ? &assets_.root->native_assets() : nullptr);
	kernel_->collision.set_trace_profile_enabled(runtime_profiling_enabled_);
	kernel_->profile.set_active(runtime_profiling_enabled_);
	// player_.weapon is player-scoped and outlives every load [orig:
	// PlayerProfile_LoadAllFromDisk @0x54f4d0 runs from the startup path, not
	// Game_StartMission], as the spawn buffer is process-scoped (the role binds the
	// ClientProcessState's); every peer's mission start re-copies its side's page.
	player_.weapon.seeded_side = -1;
	// Round init clears the map mode and the zooms return to the spawn
	// defaults (witness at hud::HudMapControl — Game_InitNewRound /
	// Player_InitPlayer lifecycle).
	kernel_->local.hud_map_control.reset_spawn();
	apply_character_traits_to_world();
	// The fresh World's per-class ATTRIBUTES words (the medic plate / map
	// marker feed) come from the retained charattr table [orig: the
	// process-scoped g_CharAttr outlives every mission, CharAttr_LoadFromDef
	// @0x412140 runs once at boot; see docs/interface/hud-re.md].
	sync_class_attribute_flags();
	apply_session_rules();
	world_installed_ = false;
	last_sim_tick_us_ = 0;
	last_net_tick_us_ = 0;
	present_.last_occlusion_build_us = 0;
	present_.last_occlusion_probe_us = 0;
	present_.last_snapshot_us = 0;
	present_.last_entity_count = 0;
	reset_occlusion_apply_baseline();
	present_.snapshot.reset();
	present_.layout.clear();
	++present_.layout_revision;
	// The retained per-load shell inputs are mission-scoped: drop them with
	// the world (the boot re-supplies them).
	assets_.collision_item_db.unref();
	assets_.item_traits_db.unref();
	assets_.infantry_adm_resource_root.unref();
	assets_.infantry_adm_item_db.unref();
	present_.occlusion_culled_bms.clear();
	present_.minimap_snapshot_valid = false;
	present_.minimap_overlays_valid = false;
	// Rebuild the kernel's terrain store from the retained TerrainData (the
	// legacy pre-load set_terrain_height_field seam), then layer the shell-fed
	// surface extras back on.
	if (assets_.terrain_data.is_valid() && assets_.terrain_data->is_loaded()) {
		const std::vector<uint8_t> &charmap = assets_.terrain_data->get_charmap_indices();
		const std::vector<uint8_t> &foliage = assets_.terrain_data->get_foliagemap_indices();
		opennova::terrain::terrain_field_store_build(kernel_->terrain_store,
				assets_.terrain_data->get_cpt(), assets_.terrain_data->get_trn(),
				charmap.empty() ? nullptr : charmap.data(),
				assets_.terrain_data->get_charmap_width(),
				assets_.terrain_data->get_charmap_height(),
				foliage.empty() ? nullptr : foliage.data(),
				assets_.terrain_data->get_foliagemap_width(),
				assets_.terrain_data->get_foliagemap_height());
	}
	apply_terrain_to_ai();
	// The fresh world's collision/mounted-pose providers wire immediately (the
	// old reset did this unconditionally): the kernel registers itself so a
	// direct-loaded world resolves mounted poses before any boot step runs.
	// The kernel is the ONE registered section-matrix/mounted-pose provider;
	// wire_collision points the world/AI systems at its collision world.
	kernel_->wire_collision();
}

opennova::world::WeatherState *Simulation::weather_state() {
	return world_installed_ && kernel_ ? &kernel_->world.weather : nullptr;
}

const opennova::world::WeatherState *Simulation::weather_state() const {
	return world_installed_ && kernel_ ? &kernel_->world.weather : nullptr;
}

void Simulation::set_weather_render_owner(Weather *p_owner) {
	assets_.weather_owner_id = p_owner != nullptr ? ObjectID(p_owner->get_instance_id())
										: ObjectID();
	if (kernel_ == nullptr) return;
	kernel_->weather_render =
			p_owner != nullptr ? p_owner->render_tick_interface() : nullptr;
}

void Simulation::_release_weather_owner() {
	if (kernel_ != nullptr) {
		kernel_->weather_render = nullptr;
	}
	if (!assets_.weather_owner_id.is_valid()) return;
	Weather *owner = Object::cast_to<Weather>(
			ObjectDB::get_instance(assets_.weather_owner_id));
	assets_.weather_owner_id = ObjectID();
	if (owner != nullptr) {
		owner->release_simulation();
	}
}

bool Simulation::start_mission(opennova::env::WeatherRuntime *p_weather,
		opennova::env::EnvironmentState *p_environment, const std::function<void()> &p_bind) {
	if (!world_installed_ || host_boot_.kernel != kernel_.get() ||
			session_.state() != opennova::inmatch::State::Loading)
		return false;
	opennova::inmatch::HostStartDevice device;
	device.weather = p_weather;
	device.environment = p_environment;
	device.bind_render = p_bind;
	std::string error;
	if (!opennova::inmatch::start_host_mission(host_boot_, device, error)) {
		fail_session_load(error.c_str());
		return false;
	}
	// Direct/local simulations historically start paused; the live GameWorld
	// frame resumes them after its presentation setup.
	if (session_.kind() == opennova::inmatch::RoleKind::SinglePlayer) (void)session_.pause();
	return true;
}

void Simulation::pump_host_render_arm_ahead_of_textures() {
	if (host_role_ != nullptr && host_boot_.kernel == kernel_.get())
		host_role_->pump_render_arm_ahead_of_textures();
}

void Simulation::end_host_mission_load() {
	if (host_boot_.kernel == kernel_.get()) opennova::inmatch::end_host_mission_load(host_boot_);
}

const opennova::renderer::PrecipitationDrawFrame &Simulation::compile_precipitation_frame(
		const Vector3 &p_camera, const Vector3 &p_camera_right, const Vector3 &p_camera_up,
		int p_terrain_light_rgb, int p_camera_mode) {
	if (!world_installed_ || kernel_ == nullptr) {
		assets_.precipitation_frame.clear();
		assets_.precipitation_frame.snow = false;
		return assets_.precipitation_frame;
	}
	opennova::world::WeatherState &weather = kernel_->world.weather;
	const float position[3] = { p_camera.x, p_camera.y, p_camera.z };
	const float right[3] = { p_camera_right.x, p_camera_right.y, p_camera_right.z };
	const float up[3] = { p_camera_up.x, p_camera_up.y, p_camera_up.z };
	const opennova::renderer::PrecipitationCamera camera =
			opennova::renderer::precipitation_camera_from_render(position, right, up, p_camera_mode);
	// The per-render update precedes the compile (retail the drawer calls
	// WeatherParticle_UpdatePositions first @ 0x5dee65).
	if (weather.raining())
		kernel_->update_precipitation(camera.position_q16[0], camera.position_q16[1], camera.position_q16[2]);
	opennova::renderer::PrecipitationDrawFrame &frame = assets_.precipitation_frame;
	opennova::renderer::compile_precipitation_frame(weather.precipitation,
			weather.core.scalar_channels.rain_pct_fp, weather.precipitation_kind,
			static_cast<uint32_t>(p_terrain_light_rgb), camera, kernel_->precipitation_draw, frame);
	return frame;
}

void Simulation::drain_weather_sounds(std::vector<opennova::world::WeatherSoundEvent> &r_events) {
	r_events.clear();
	if (!world_installed_ || kernel_ == nullptr) return;
	r_events.swap(kernel_->world.out.weather_sounds);
	kernel_->world.out.weather_sounds.clear();
}

void Simulation::drain_script_sounds(std::vector<opennova::world::ScriptSoundEvent> &r_events) {
	r_events.clear();
	if (!world_installed_ || kernel_ == nullptr) return;
	r_events.swap(kernel_->world.out.script_sounds);
}

bool Simulation::take_audio_stops(bool &r_dialog_line, bool &r_channels) {
	r_dialog_line = false;
	r_channels = false;
	if (!world_installed_ || kernel_ == nullptr) return false;
	opennova::world::WorldOutbox &out = kernel_->world.out;
	r_dialog_line = out.dialog_line_stop;
	r_channels = out.audio_channels_shutdown;
	out.dialog_line_stop = false;
	out.audio_channels_shutdown = false;
	return r_dialog_line || r_channels;
}

bool Simulation::world_sound_parked() const {
	return world_installed_ && kernel_ != nullptr && kernel_->world.epilog.world_sound_parked;
}

Ref<EnvironmentSnapshot> Simulation::get_environment_snapshot() const {
	opennova::devtools::EnvironmentSnapshot snapshot;
	if (!native_environment_snapshot(snapshot)) return Ref<EnvironmentSnapshot>();
	Ref<EnvironmentSnapshot> out;
	out.instantiate();
	out->assign(snapshot);
	return out;
}

bool Simulation::native_environment_snapshot(
		opennova::devtools::EnvironmentSnapshot &out) const {
	out = opennova::devtools::EnvironmentSnapshot{};
	if (!world_installed_ || kernel_ == nullptr) return false;
	// (engine: runtime/devtools/environment_snapshot.h — the rows' sources)
	out = opennova::devtools::environment_snapshot(*kernel_, !is_joiner());
	return true;
}

// The MCP/debug rows' commands: authority-gated forwarders into the ONE
// command layer (world::EntityCommands, ADR 0042 d5).
#define OPENNOVA_WEATHER_COMMAND(call)                                    \
	if (!world_installed_ || is_joiner() || kernel_ == nullptr) return false; \
	kernel_->world.commands.call;                                         \
	return true

bool Simulation::command_rain(int p_percent, int p_seconds) { OPENNOVA_WEATHER_COMMAND(set_rain(p_percent, p_seconds)); }
bool Simulation::debug_set_time_of_day_minutes(double p_minute_of_day) { OPENNOVA_WEATHER_COMMAND(debug_set_time_of_day_minutes(p_minute_of_day)); }
bool Simulation::command_snow(int p_percent, int p_seconds) { OPENNOVA_WEATHER_COMMAND(set_snow(p_percent, p_seconds)); }
bool Simulation::command_overcast(int p_percent, int p_seconds) { OPENNOVA_WEATHER_COMMAND(set_overcast(p_percent, p_seconds)); }
bool Simulation::command_fog_distance(int p_metres) { OPENNOVA_WEATHER_COMMAND(set_fog_distance(p_metres)); }
bool Simulation::command_move_fog(int p_metres, int p_seconds) { OPENNOVA_WEATHER_COMMAND(move_fog(p_metres, p_seconds)); }
bool Simulation::command_sky_speed(int p_rate) { OPENNOVA_WEATHER_COMMAND(set_sky_speed(p_rate)); }
bool Simulation::command_quake(int p_seconds) { OPENNOVA_WEATHER_COMMAND(quake(p_seconds)); }
bool Simulation::command_time_of_day_minutes(int p_minute_of_day) { OPENNOVA_WEATHER_COMMAND(set_time_of_day_minutes(p_minute_of_day)); }
bool Simulation::command_fog_type(int p_type) { OPENNOVA_WEATHER_COMMAND(set_fog_type(p_type)); }
bool Simulation::command_lightning_flash() { OPENNOVA_WEATHER_COMMAND(lightning_flash()); }
bool Simulation::command_lightning_far_flash() { OPENNOVA_WEATHER_COMMAND(lightning_far_flash()); }
bool Simulation::command_wind_scale(int p_value) { OPENNOVA_WEATHER_COMMAND(set_wind_scale(p_value)); }
bool Simulation::command_sky_height(int p_height_raw) { OPENNOVA_WEATHER_COMMAND(set_sky_height(p_height_raw)); }
bool Simulation::command_sun_fade(int p_percent, int p_seconds) { OPENNOVA_WEATHER_COMMAND(sun_fade(p_percent, p_seconds)); }
bool Simulation::command_color_fade(int p_seconds) { OPENNOVA_WEATHER_COMMAND(set_color_fade(p_seconds)); }
bool Simulation::command_weather_color(int p_target, int p_rgb) {
	OPENNOVA_WEATHER_COMMAND(set_weather_color(
			static_cast<opennova::world::WeatherColorTarget>(p_target), static_cast<uint32_t>(p_rgb)));
}
bool Simulation::command_lightning_color(int p_rgb) { OPENNOVA_WEATHER_COMMAND(set_lightning_color(static_cast<uint32_t>(p_rgb))); }

#undef OPENNOVA_WEATHER_COMMAND

// Wire the kernel's terrain field into the world/AI/collision systems (the
// store's placed-tile overlay, D-SND-15, rides the surface view), then layer
// the sound-profile chain the kernel deliberately leaves to the embedder.
void Simulation::apply_terrain_to_ai() {
	kernel_->wire_terrain();
	apply_sound_state_to_world();
}

// (Re)apply the persisted sound-profile chain state to the current world: the parsed
// SndProf.def table and the water plane. Runs from apply_terrain_to_ai
// (reset_world / load) and from the setters when live. (The mission attrib
// dword the scream's night gate reads is stamped by the kernel boot from the BMS
// header — not re-applied here.)
void Simulation::apply_sound_state_to_world() {
	if (!kernel_) return;
	kernel_->world.script.voice.set_set_resolver(voice_set_resolver_);
	if (assets_.sound_profiles_override) {
		kernel_->world.tables.sound_profiles.clear();
		if (!assets_.sndprof_text.empty())
			kernel_->world.tables.sound_profiles.parse(
					reinterpret_cast<const char *>(assets_.sndprof_text.data()), assets_.sndprof_text.size());
	}
	if (const auto *items = kernel_->items_table())
		opennova::mission::resolve_item_event_sounds(kernel_->world, *items);
	kernel_->world.env.water_z = assets_.env_water_z_q16;
	kernel_->sync_water_plane();
}

void Simulation::set_terrain_height_field(const Ref<TerrainData> &p_terrain) {
	// Clear first so a null/unloaded terrain disables grounding. The store is
	// the engine's one owning cpt/trn(+charmap) field builder (ADR 0042 d4),
	// living on the kernel; the Ref is retained so a reload (which recreates
	// the kernel) can rebuild the store from the same source.
	assets_.terrain_data = p_terrain;
	kernel_->terrain_store.clear();
	if (p_terrain.is_valid() && p_terrain->is_loaded()) {
		const std::vector<uint8_t> &charmap = p_terrain->get_charmap_indices();
		const std::vector<uint8_t> &foliage = p_terrain->get_foliagemap_indices();
		opennova::terrain::terrain_field_store_build(kernel_->terrain_store,
				p_terrain->get_cpt(), p_terrain->get_trn(),
				charmap.empty() ? nullptr : charmap.data(),
				p_terrain->get_charmap_width(), p_terrain->get_charmap_height(),
				foliage.empty() ? nullptr : foliage.data(),
				p_terrain->get_foliagemap_width(), p_terrain->get_foliagemap_height());
	}
	apply_terrain_to_ai();
}

void Simulation::set_sound_profiles(const PackedByteArray &p_sndprof_text) {
	assets_.sound_profiles_override = true;
	assets_.sndprof_text.assign(p_sndprof_text.ptr(), p_sndprof_text.ptr() + p_sndprof_text.size());
	apply_sound_state_to_world();
}

void Simulation::apply_character_traits_to_world() {
	if (!kernel_) return;
	kernel_->world.tables.character_traits.clear();
	for (const CharacterSexRow &row : assets_.character_sex_rows)
		kernel_->world.tables.character_traits.set(row.character_id, row.female);
}

void Simulation::set_water_z(double p_water_y) {
	assets_.env_water_z_q16 = static_cast<int32_t>(p_water_y * 65536.0);
	if (kernel_) {
		kernel_->world.env.water_z = assets_.env_water_z_q16;
		kernel_->sync_water_plane();
	}
}

void Simulation::drain_slot_sounds(std::vector<opennova::world::SoundSlotEvent> &r_events) {
	r_events.clear();
	if (!world_installed_) return;
	// The rows cross in the mission frame (16.16); the fire pass axis-maps
	// (x, z, -y) as it plays them, the same mapping as the fire drain.
	r_events.swap(kernel_->world.out.slot_sounds);
	kernel_->world.out.slot_sounds.clear();
}

void Simulation::drain_sound_emitter_events(
		std::vector<opennova::world::SoundEmitterEvent> &r_events) {
	r_events.clear();
	if (!world_installed_) return;
	// Mission coordinates on the rows; the audio layer axis-maps (x, z, -y)
	// like every other positional presentation drain.
	r_events = kernel_->world.out.sound_emitters.drain();
}

TypedArray<SoundEmitterRow> Simulation::drain_sound_emitters() {
	TypedArray<SoundEmitterRow> out;
	std::vector<opennova::world::SoundEmitterEvent> events;
	drain_sound_emitter_events(events);
	for (const opennova::world::SoundEmitterEvent &ev : events) {
		Ref<SoundEmitterRow> d;
		d.instantiate();
		d->assign(ev);
		out.push_back(d);
	}
	return out;
}

// The binding legs every kernel boot shares, run AFTER kernel_->boot():
// session-header capture for the LAN 0x0B burst, the HUD map zoom seed, the
// score-row re-resolve, and the held-WacProgram re-apply (a program installed
// through compile_and_set_wac survives reloads; the kernel's own layered load
// wins whenever the mission authored .wac files).
void Simulation::finish_kernel_boot() {
	world_installed_ = true;
	apply_host_session_mission_header(kernel_->mission);
	// The mission's authored map_zoom scales BOTH radar-zoom spawn defaults
	// (witness at hud::HudMapControl::set_mission_map_zoom — the
	// Player_InitPlayer derivation off the BMS header float).
	kernel_->local.hud_map_control.set_mission_map_zoom(kernel_->mission.header.map_zoom);
	if (!kernel_->wac_loaded && assets_.wac_program && assets_.wac_program->is_ok())
		kernel_->wac.set_program(assets_.wac_program->native_program(), kernel_->world);
}

// The kernel boot's bringup_net_session hook: the active role's own bring-up
// (the listen host binds the mission's World under its inmatch session between
// the world wiring and the system registration, its session created ahead of the
// load by the host boot, or here for a kernel-direct load (engine:
// runtime/inmatch/host_role.cpp HostRole::create_session);
// a joiner (re)builds its non-authority ClientRuntime at the same point; the
// bare no-net world installs nothing), then the binding's tail: the live
// runtime pointer, the retained join inputs on a fresh joiner runtime, the
// joiner's per-load loadout/zone resets, and the decode-view class table.
std::function<void()> Simulation::role_bringup_hook() {
	opennova::inmatch::Role &role = active_role();
	if (host_role_ != nullptr) host_role_->stage_bringup(host_bringup());
	return [&role, tail = role_bringup_tail()] { tail(role.bring_up()); };
}

std::function<void(bool)> Simulation::role_bringup_tail() {
	return [this](bool fresh_joiner_runtime) {
		runtime_ = active_role().client_runtime();
		install_console_seams();
		if (fresh_joiner_runtime) {
			install_charattr_table();
			install_character_join_vars();
			install_auto_medic_preference();
			install_expansion_version_root();
			install_join_mission_start();
		}
		if (is_joiner()) {
			net_.joiner_applied_loadout_revision = 0;
			net_.deploy_zone_registry.invalidate(); // fresh world -> fresh zone registry
			// Re-arm the 0x2F submission seam from the carried sim state. The fresh
			// kernel holds an EMPTY weapon catalog, so this is a deliberate no-op
			// that leaves the capture-default fallback armed; the real arm happens
			// when the shell's load_weapon_table lands.
			push_joiner_loadout_kit();
		}
		install_item_catalog();
		install_character_registry();
	};
}

void Simulation::apply_host_session_mission_header(const opennova::bms::File &file) {
	std::vector<uint8_t> header_blob;
	std::string error;
	if (opennova::bms::encode_loaded_header_blob(file, header_blob, error)) {
		net_.host_session_config.mission_header_blob = std::move(header_blob);
	} else {
		net_.host_session_config.mission_header_blob.clear();
	}

	const std::string mission_name = file.get_mission_name();
	if (!mission_name.empty()) {
		net_.host_session_config.mission_name = mission_name;
		if (net_.host_session_config.spawn_names.empty()) {
			net_.host_session_config.spawn_names.push_back(mission_name);
		}
	}
	// net_.host_session_config is consumed by create_session at the next load through
	// bringup_host_runtime; there is deliberately nothing to refresh live.
}

// The production boot: phase A of the engine's one host boot (ADR 0051 d4,
// inmatch/host_boot.h), the order opennova-serve runs too. This entry only
// converts the Godot Refs into the boot's sources (the mounted index and
// assets, the parsed items.def rows, the terrain documents) and supplies the
// binding's seams (the fresh kernel, the shell's terrain store, the bring-up
// tail); then it retains what the boot read and runs its own legs (the
// joiner's decoded-row resolver inputs, the session-kit profile seed). The
// session stays Loading until the world's load runs phase B (start_mission)
// after its device stages.
int64_t Simulation::boot_mission(const Ref<MissionData> &p_mission,
		const Ref<ResourceRoot> &p_resource_root,
		const Ref<ItemDatabase> &p_item_db,
		const Ref<TerrainData> &p_terrain,
		const PackedByteArray &p_terrain_til, const String &p_wac_basename,
		const String &p_infantry_adm, const String &p_mission_file,
		bool p_playable) {
	namespace ms = opennova::mission;
	namespace inmatch = opennova::inmatch;
	if (p_mission.is_null() || !prepare_session_load()) {
		fail_session_load("mission boot failed");
		return ERR_CANT_OPEN;
	}
	// The previous mission's retained terrain must not rebuild into the fresh
	// kernel: the boot's terrain seam builds the store exactly once.
	assets_.terrain_data.unref();
	if (p_resource_root.is_valid()) assets_.root = p_resource_root;
	if (p_item_db.is_valid()) ensure_item_replication_catalog(p_item_db);
	const opennova::bms::File &document = p_mission->native_file();
	inmatch::HostBootRequest request;
	request.mission = document;
	// The by-name readers' base: the mission file's name cut at its first '.'.
	const std::string mission_file = opennova::to_std(p_mission_file);
	request.mission_basename = ms::mission_base_name(mission_file);
	if (p_resource_root.is_valid()) {
		request.files = ms::boot_files_from_index(p_resource_root->native_index());
		// The mount's expansion text-override table (the mission text's
		// lookups search it first).
		const PackedByteArray override_table = p_resource_root->get_expansion_override_table();
		request.text_override_table.assign(override_table.ptr(),
				override_table.ptr() + override_table.size());
	}
	request.assets = assets_.root.is_valid() ? &assets_.root->native_assets() : nullptr;
	if (p_item_db.is_valid()) {
		request.items = &p_item_db->native_items();
		request.item_catalog = assets_.item_replication_catalog;
	}
	request.session = &session_;
	request.role = &active_role();
	request.host = host_role_;
	request.render_arm_deferred = render_arm_deferred_;
	const uint32_t mission_game_type = opennova::game_type::for_mission_attribs(document.header.attrib_flags);
	const bool next_mission = net_.map_change_pending && host_role_ != nullptr;
	if (host_role_ != nullptr && is_host_listening()) {
		if (next_mission) {
			stage_next_mission_session_config(document);
		} else if (!host_role_->create_ran()) {
			seed_host_rotation(net_, p_resource_root);
		}
		host_role_->set_rotation(&net_.rotation());
	}
	if (host_role_ != nullptr) {
		request.host_cfg = host_session_cfg(mission_game_type, mission_file);
		request.session_score_ini = is_host_listening();
		request.next_mission = next_mission;
	}
	// A joiner's tiles are the host's S2C 0x45 stream (or none), and a caller
	// may hand a host its bytes; otherwise the boot reads the mission's .til.
	if (is_joiner() || !p_terrain_til.is_empty())
		request.terrain_til = std::vector<uint8_t>(p_terrain_til.ptr(),
				p_terrain_til.ptr() + p_terrain_til.size());
	request.wire_header_world = p_mission->is_wire_header_only();
	ms::KernelBootOptions &options = request.boot_options;
	options.music_globals = wac_music_globals();
	options.playable = p_playable;
	options.joiner = is_joiner();
	options.mp_session = is_host_listening() || is_joiner();
	// The single player's restart: Game_StartMission(1), which skips the fresh start's
	// profile word and intro cine and keeps the first start's UP-TIME stamp (D-PWR-8; the
	// engine's KernelBootOptions::restart carries the witness).
	options.restart = restart_boot_ && !options.mp_session;
	// The shell owns the terrain field's parsed-document entry (the store the
	// seam below builds, or none): the kernel never loads one from files here.
	options.terrain = false;
	options.wac = !p_wac_basename.is_empty();
	options.wac_basename = opennova::to_std(p_wac_basename);
	options.game_type = mission_game_type;
	stamp_admission_limits(options, net_, is_host_listening(),
			joiner_role_ != nullptr ? joiner_role_->client_runtime() : nullptr);
	options.infantry_adm = p_infantry_adm.is_empty()
			? std::string(ms::kDefaultInfantryAdm)
			: opennova::to_std(p_infantry_adm);
	request.fresh_kernel = [this, p_item_db]() -> ms::MissionKernel & {
		reset_world();
		// The shell's parsed items.def rows are the kernel's item table; the
		// Ref pins their lifetime for the kernel's.
		if (p_item_db.is_valid()) assets_.item_traits_db = p_item_db;
		return *kernel_;
	};
	// Terrain fills the kernel store BEFORE boot (has_terrain gates on it).
	request.before_terrain = [this, p_terrain](ms::MissionKernel &) {
		set_terrain_height_field(p_terrain);
	};
	request.after_bringup = role_bringup_tail();
	std::string boot_error;
	net_.map_change_pending = false;
	if (!inmatch::boot_host_mission(std::move(request), host_boot_, boot_error)) {
		fail_session_load(boot_error.c_str());
		return ERR_CANT_OPEN;
	}
	// What the boot read, retained for the binding's own reads and the next
	// fresh kernel: the mission text (the HUD's people names), the .til, the
	// water plane (apply_sound_state_to_world re-stamps it) and, on a host,
	// charattr.def, which the world's resync keeps (D-NET-345).
	net_.mission_text = host_boot_.mission_text;
	net_.terrain_til_data = host_boot_.terrain_til;
	assets_.env_water_z_q16 = host_boot_.water_z_q16;
	if (host_role_ != nullptr) net_.charattr_table = host_boot_.charattr;
	finish_kernel_boot();
	// The binding-side resolver inputs (the joiner's decoded rows read the
	// anim root/item db Refs) and the collision Ref retention.
	if (p_resource_root.is_valid() && p_item_db.is_valid()) {
		assets_.infantry_adm_resource_root = p_resource_root;
		assets_.infantry_adm_item_db = p_item_db;
	}
	if (p_item_db.is_valid()) assets_.collision_item_db = p_item_db;
	// In a live session the resident kit buffer is the assigned side's profile
	// page (retail's Game_StartMission copy into restrictionData
	// [orig: @0x525813]); the kernel's table load built the pool from the
	// mission kit, so re-seed + rebuild when a session profile applies.
	if (seed_session_kit_from_profile())
		rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	push_joiner_loadout_kit();
	return OK;
}

// The world load's session create (engine inmatch/host_boot.h create_host_session carries the
// witness): the session record boot_mission builds, the rotation seeded first on a listening host,
// as boot_mission seeds it for a create it runs itself.
int64_t Simulation::create_host_session(const Ref<MissionData> &p_mission,
		const Ref<ResourceRoot> &p_resource_root, const String &p_mission_file) {
	namespace inmatch = opennova::inmatch;
	if (host_role_ == nullptr || net_.map_change_pending || p_mission.is_null() ||
			p_resource_root.is_null())
		return OK;
	const opennova::bms::File &document = p_mission->native_file();
	const std::string mission_file = opennova::to_std(p_mission_file);
	if (is_host_listening()) {
		seed_host_rotation(net_, p_resource_root);
		host_role_->set_rotation(&net_.rotation());
	}
	const PackedByteArray table = p_resource_root->get_expansion_override_table();
	inmatch::HostConfig cfg = host_session_cfg(
			opennova::game_type::for_mission_attribs(document.header.attrib_flags), mission_file);
	cfg.mission_type = static_cast<uint8_t>(document.header.mission_type);
	const inmatch::CreateSessionResult created = inmatch::create_host_session(*host_role_,
			opennova::mission::boot_files_from_index(p_resource_root->native_index()),
			opennova::mission::mission_base_name(mission_file),
			std::vector<uint8_t>(table.ptr(), table.ptr() + table.size()), std::move(cfg),
			/*session_score_ini=*/is_host_listening());
	return created == inmatch::CreateSessionResult::ProcessExit ? ERR_CANT_CREATE : OK;
}

bool Simulation::load_from_mission_data(const Ref<MissionData> &p_mission) {
	if (p_mission.is_null()) return false;
	if (!begin_session_load()) return false;
	// An item table supplied before the load participates in admission and
	// definition callbacks, just as it does for a file-fed mission.
	const Ref<ItemDatabase> pending_item_db = pending_item_traits_db_;
	pending_item_traits_db_.unref();
	reset_world();
	// Do not infer this from the role: tests/tools and legacy direct joins may
	// still load a complete BMS, whose authored promotion is already canonical.
	// Only the production 616-byte S2C header needs wire-time materialization.
	kernel_->wire_header_world = p_mission->is_wire_header_only();
	// The editor's live, in-memory mission (unsaved edits included) adopts
	// into the kernel with no file source; definition tables remain available
	// to the native admission and initialization steps.
	kernel_->open_document(p_mission->native_file(),
			std::string(), opennova::mission::BootFileSource{});
	if (pending_item_db.is_valid()) {
		assets_.item_traits_db = pending_item_db;
		kernel_->set_items_table(&pending_item_db->native_items());
	}
	opennova::mission::KernelBootOptions options;
	options.music_globals = wac_music_globals();
	options.playable = false; // callers spawn explicitly (or a host role's round init spawns)
	options.joiner = is_joiner();
	options.mp_session = is_host_listening() || is_joiner();
	options.game_type = opennova::game_type::for_mission_attribs(kernel_->mission.header.attrib_flags);
	stamp_admission_limits(options, net_, is_host_listening(),
			joiner_role_ != nullptr ? joiner_role_->client_runtime() : nullptr);
	options.bringup_net_session = role_bringup_hook();
	std::string boot_error;
	if (!kernel_->boot(options, boot_error) || session_create_refused(host_role_, boot_error)) {
		fail_session_load(boot_error.c_str());
		return false;
	}
	finish_kernel_boot();
	if (pending_item_db.is_valid()) resolve_item_traits(pending_item_db);
	complete_session_load();
	return true;
}

bool Simulation::compile_and_set_wac(const PackedStringArray &p_sources) {
	if (!world_installed_) {
		UtilityFunctions::push_warning(
				"compile_and_set_wac needs a loaded world (the registry resolves symbolic names).");
		return false;
	}
	std::vector<std::string> sources;
	sources.reserve(static_cast<size_t>(p_sources.size()));
	for (int64_t i = 0; i < p_sources.size(); ++i) {
		const CharString utf8 = p_sources[i].utf8();
		sources.emplace_back(utf8.get_data(), static_cast<size_t>(utf8.length()));
	}
	opennova::wac::CompileEnv env;
	env.music_globals = wac_music_globals();
	env.registry = &kernel_->world.registry;
	env.ammo = &kernel_->world.tables.ammo;
	env.effects = &kernel_->script_effect_catalog;
	env.sounds = &kernel_->script_sound_catalog;
	opennova::wac::Program program = opennova::wac::compile_program(sources, env);
	auto holder = std::make_shared<WacProgram>();
	// Adopt the registry-compiled program into the holder so the retained
	// WacProgram carries its diagnostics either way.
	holder->adopt(std::move(program));
	assets_.wac_program = std::move(holder);
	if (!assets_.wac_program->is_ok()) {
		return false;
	}
	kernel_->wac.set_program(assets_.wac_program->native_program(), kernel_->world);
	return true;
}

void Simulation::set_runtime_profiling_enabled(bool p_enabled) {
	if (runtime_profiling_enabled_ == p_enabled) return;
	runtime_profiling_enabled_ = p_enabled;
	last_trace_profile_ = opennova::world::CollisionWorld::TraceProfile{};
	last_sim_tick_us_ = 0;
	last_net_tick_us_ = 0;
	present_.last_snapshot_us = 0;
	present_.last_occlusion_build_us = 0;
	present_.last_occlusion_probe_us = 0;
	frame_sim_us_ = 0;
	frame_sink_us_ = 0;
	if (kernel_ != nullptr) {
		kernel_->profile.reset();
		kernel_->profile.set_active(p_enabled);
		kernel_->collision.set_trace_profile_enabled(p_enabled);
	}
}

Vector4i Simulation::get_last_projectile_trace_times_us() const {
	const opennova::world::CollisionWorld::TraceProfile &tp = last_trace_profile_;
	return Vector4i(trace_profile_lane(tp.terrain_us),
			trace_profile_lane(tp.static_us),
			trace_profile_lane(tp.dynamic_us),
			trace_profile_lane(tp.person_us));
}

Vector4i Simulation::get_last_projectile_trace_counts() const {
	const opennova::world::CollisionWorld::TraceProfile &tp = last_trace_profile_;
	return Vector4i(trace_profile_lane(tp.calls),
			trace_profile_lane(tp.static_survivors),
			trace_profile_lane(tp.dynamic_survivors),
			trace_profile_lane(tp.person_survivors));
}

void Simulation::take_projectile_trace_profile() {
	if (kernel_ == nullptr) return;
	last_trace_profile_ = kernel_->collision.trace_profile();
	kernel_->collision.clear_trace_profile();
}

Vector2i Simulation::get_last_projectile_trace_faces() const {
	const opennova::world::CollisionWorld::TraceProfile &tp = last_trace_profile_;
	return Vector2i(trace_profile_lane(tp.static_faces),
			trace_profile_lane(tp.dynamic_faces));
}

Dictionary Simulation::get_runtime_perf_counters() const {
	Dictionary out;
	out["loaded"] = is_loaded();
	out["listen_server"] = listen_server_;
	out["ai_count"] = kernel_ ? kernel_->world.ai.count() : 0;
	out["present_entity_count"] = present_.last_entity_count;
	out["sim_tick_us"] = static_cast<int64_t>(last_sim_tick_us_);
	out["net_tick_us"] = static_cast<int64_t>(last_net_tick_us_);
	out["present_snapshot_us"] = static_cast<int64_t>(present_.last_snapshot_us);
	out["occlusion_build_us"] = static_cast<int64_t>(present_.last_occlusion_build_us);
	out["occlusion_probe_us"] = static_cast<int64_t>(present_.last_occlusion_probe_us);
	out["runtime_profiling_enabled"] = runtime_profiling_enabled_;
	// The last tick's projectile-trace attribution (collision.h TraceProfile), as the
	// tick's sink took it (take_projectile_trace_profile).
	const opennova::world::CollisionWorld::TraceProfile &tp = last_trace_profile_;
	out["trace_profiling_enabled"] = kernel_->collision.trace_profile_enabled();
	out["trace_calls"] = tp.calls;
	out["trace_terrain_us"] = tp.terrain_us;
	out["trace_static_us"] = tp.static_us;
	out["trace_dynamic_us"] = tp.dynamic_us;
	out["trace_person_us"] = tp.person_us;
	out["trace_static_survivors"] = tp.static_survivors;
	out["trace_dynamic_survivors"] = tp.dynamic_survivors;
	out["trace_person_survivors"] = tp.person_survivors;
	out["trace_static_faces"] = tp.static_faces;
	out["trace_dynamic_faces"] = tp.dynamic_faces;
	return out;
}

void Simulation::set_wac_paused(bool p_paused) {
	if (kernel_) {
		kernel_->wac.paused = p_paused; // [orig: dword_C6EB28]
	}
}

bool Simulation::is_wac_paused() const {
	return kernel_ != nullptr && kernel_->wac.paused;
}

void Simulation::set_mission_variable(int index, int value) {
	if (kernel_) kernel_->world.script.vars.set_mission(index, value);
}

Error Simulation::debug_kill_player_entity(int p_handle) {
	if (!kernel_ || is_joiner()) return ERR_UNAVAILABLE;
	// The engine transaction (EntityCommands::kill_player): the health write
	// the real damage path makes plus the RoundDeath record, the local player
	// as the killer.
	return kernel_->world.commands.kill_player(
				   opennova::world::EntityHandle{static_cast<uint16_t>(p_handle)},
				   kernel_->world.cached.local_player)
			? OK
			: ERR_INVALID_PARAMETER;
}

// Probe/diagnostic seam: delegate to the engine's both-store health mutator
// (EntityCommands::set_entity_health) so in-game probes can shorten a fight
// without bypassing the damage/death chain under test.
Error Simulation::debug_set_entity_health(int p_index, int p_hp) {
	if (!kernel_) return ERR_UNAVAILABLE;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return ERR_INVALID_PARAMETER;
	return kernel_->world.commands.set_entity_health(e->handle, p_hp)
			? OK
			: ERR_UNAVAILABLE;
}

// Debug: seat an AI body in a vehicle's control seat, by authored SSN. The
// rotor only spins for a control-seat claimant, so a screenshot of turning
// blades needs a pilot in the chair; 05TRcoop parks its five helicopters empty
// until the (player-gated) script sends a crew.
// Debug: seat the LOCAL PLAYER in a vehicle's control seat, by the vehicle's
// authored SSN. Mounting by SSN is not enough for the player, whose entity is
// spawned at deploy and carries no authored id, so this resolves it from the
// world's cached local-player handle.
// Is the local player in a seat that suppresses the first-person weapon? A
// helicopter pilot has no weapon in hand in retail; a passenger keeps his.
// [orig: Player_RenderFirstPersonViewModel — see
//  world::mount_hides_fp_viewmodel for the witnessed condition]
bool Simulation::local_player_fp_weapon_hidden() const {
	if (!kernel_) return false;
	const opennova::world::Entity *lp =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (lp == nullptr || !lp->mounted) return false;
	const opennova::world::Entity *carrier =
			kernel_->world.registry.get(lp->mount_target);
	return opennova::world::mount_hides_fp_viewmodel(*lp, carrier);
}

Error Simulation::debug_crew_local_player(int p_vehicle_ssn) {
	if (!kernel_) return ERR_UNAVAILABLE;
	const opennova::world::Entity *lp =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (lp == nullptr) return ERR_UNAVAILABLE;
	// The local body by its handle: its script SSN is its DcbId (world::entity_dcb_id), never
	// its record word.
	return kernel_->world.commands.mount(opennova::world::EntityTarget(lp->handle),
	                              static_cast<uint16_t>(p_vehicle_ssn))
			? OK
			: ERR_INVALID_PARAMETER;
}

Error Simulation::debug_crew_vehicle(int p_occupant_ssn, int p_vehicle_ssn) {
	if (!kernel_) return ERR_UNAVAILABLE;
	return kernel_->world.commands.mount(static_cast<uint16_t>(p_occupant_ssn),
	                              static_cast<uint16_t>(p_vehicle_ssn))
			? OK
			: ERR_INVALID_PARAMETER;
}

// Probe seam beside debug_set_entity_health: delegate to the engine's
// both-store position mutator (EntityCommands::set_entity_position) —
// mission-space coordinates. Lets in-game probes bring a reachable victim to
// the player when the mission geography defeats straight-line navigation.
Error Simulation::debug_set_entity_position(int p_index, const Vector3 &p_mission_pos) {
	if (!kernel_) return ERR_UNAVAILABLE;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return ERR_INVALID_PARAMETER;
	return kernel_->world.commands.set_entity_position(e->handle,
				   opennova::world::Vec3{p_mission_pos.x, p_mission_pos.y,
						   p_mission_pos.z})
			? OK
			: ERR_UNAVAILABLE;
}

// Probe/diagnostic seam beside debug_set_world_entity_position: land the
// LOCAL player at an exact dumped pose (mission position + mission yaw/pitch
// as the F3 Player-tab dump records them) — entity + AI-motor stores written
// together so the next motor tick continues from the pose instead of
// snapping back.
// TEST SCAFFOLDING, host-authority only. Kills every member of a BMS command group
// outright so an unattended round can reach a scripted win condition that an
// autofiring bot cannot reliably produce (00TRg's event 31 needs GroupDestroyed(16),
// i.e. six specific AI dead). It drives the SAME EntityCommands::kill_group the BMS
// KILL_GROUP action uses (engine: formats/mission/bms.h); it invents no
// state and fakes no event -- the win chain still has to evaluate on its own.
// Sibling of debug_teleport_local_player, which exists for the same reason.
int Simulation::debug_kill_group(int p_group) {
	if (!kernel_) return -1;
	return kernel_->world.commands.kill_group(p_group);
}

Error Simulation::debug_teleport_local_player(const Vector3 &p_mission_pos,
												  float p_yaw_deg, float p_pitch_deg) {
	if (!kernel_->world.cached.local_player.valid()) {
		return ERR_UNAVAILABLE;
	}
	if (kernel_->local.player() == nullptr || kernel_->local.player_ai() == nullptr) {
		return ERR_UNAVAILABLE;
	}
	// The engine owns the full teleport transaction (both position stores, the
	// input-owned view mirrors, the ladder-latch drop, the resolver reset).
	kernel_->local.teleport_local_player(
			opennova::world::Vec3{p_mission_pos.x, p_mission_pos.y, p_mission_pos.z},
			p_yaw_deg, p_pitch_deg);
	return OK;
}

// The scenario driver's setup pose on every role: the retail bridge's ApplyPose
// write on the local player's L (Position, Rotation X / Y, AirVelocity zeroed),
// which the onHook driver gives a retail actor on any role. It leaves the
// resolver's idle-skip counter alone, as ApplyPose leaves retail's +0x373, so a
// body posed above its floor settles as retail's does; teleport_local_player
// (the dev F3 tool) zeroes it. On a joiner its next C2S 0x0C uplink carries the
// pose. The pose arrives as doubles so the Q16/BAM words match the retail
// driver's conversion of the same script.
Error Simulation::debug_apply_local_pose(double p_x, double p_y, double p_z, double p_yaw_deg,
		double p_pitch_deg) {
	if (!kernel_ || !kernel_->world.cached.local_player.valid()) {
		return ERR_UNAVAILABLE;
	}
	opennova::devtools::ScriptPose pose;
	std::string error;
	if (!opennova::devtools::script_pose_from_mission(p_x, p_y, p_z, p_yaw_deg, p_pitch_deg,
				pose, error)) {
		return ERR_INVALID_PARAMETER;
	}
	return kernel_->local.apply_pose(pose.position_q16, pose.heading_bam, pose.pitch_bam)
			? OK
			: ERR_UNAVAILABLE;
}

// The parity runner's slot staging (godot/probes/net/stage_slot_probe.gd): the
// engine owns the slot walk and the Position write (inmatch::stage_slot_position);
// the binding maps its refusals onto Error codes.
Error Simulation::debug_stage_slot(int p_slot, double p_x, double p_y, double p_z) {
	opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr) return ERR_UNAVAILABLE;
	const opennova::inmatch::SlotStagingResult staged =
			opennova::inmatch::stage_slot_position(*ctx, p_slot, p_x, p_y, p_z);
	switch (staged.status) {
		case opennova::inmatch::SlotStagingStatus::Staged: return OK;
		case opennova::inmatch::SlotStagingStatus::NotAuthority: return ERR_UNAVAILABLE;
		case opennova::inmatch::SlotStagingStatus::NoPlayer: return ERR_DOES_NOT_EXIST;
		case opennova::inmatch::SlotStagingStatus::NoBody: return ERR_UNCONFIGURED;
		case opennova::inmatch::SlotStagingStatus::OutOfRange: return ERR_INVALID_PARAMETER;
	}
	return FAILED;
}

// The by-net-id sibling of debug_set_entity_position: the net-id resolve is
// the binding's lookup, the both-store move is the engine's
// (EntityCommands::set_entity_position).
void Simulation::debug_set_world_entity_position(int p_net_id,
													 const Vector3 &p_mission_pos) {
	if (!kernel_ || p_net_id <= 0 || p_net_id > 0xFFFF) return;
	const opennova::world::EntityHandle h =
			kernel_->world.registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	(void)kernel_->world.commands.set_entity_position(h,
			opennova::world::Vec3{p_mission_pos.x, p_mission_pos.y, p_mission_pos.z});
}

Error Simulation::debug_set_world_entity_weapon_ammo(
		int p_net_id, int p_clip, int p_reserve) {
	if (!kernel_ || p_net_id <= 0 || p_net_id > 0xFFFF)
		return ERR_INVALID_PARAMETER;
	const opennova::world::EntityHandle h =
			kernel_->world.registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	return kernel_->world.commands.set_entity_weapon_ammo(h, p_clip, p_reserve)
			? OK
			: ERR_DOES_NOT_EXIST;
}

Error Simulation::debug_set_entity_item_attrib(int p_handle, int64_t p_attrib,
												   int64_t p_attrib2) {
	if (!kernel_) return ERR_UNAVAILABLE;
	// The one non-authoritative role: a joiner's rows are replicas the wire
	// re-writes, so the seam refuses here as well as in the debug-control
	// table (the ai_index addressability rule does not cover a handle-keyed
	// write).
	if (session_role() == ROLE_JOINER) return ERR_UNAUTHORIZED;
	if (p_handle < 0 || p_handle >= 0xFFFF || p_attrib < 0 || p_attrib > 0xFFFFFFFFLL ||
			p_attrib2 < 0 || p_attrib2 > 0xFFFFFFFFLL)
		return ERR_INVALID_PARAMETER;
	return kernel_->world.commands.set_entity_item_attrib(
				   opennova::world::EntityHandle{static_cast<uint16_t>(p_handle)},
				   static_cast<uint32_t>(p_attrib), static_cast<uint32_t>(p_attrib2))
			? OK
			: ERR_DOES_NOT_EXIST;
}

int Simulation::get_mission_variable(int index) const {
	return kernel_ ? kernel_->world.script.vars.get_mission(index) : 0;
}

Ref<RoundOutcome> Simulation::get_round_outcome_debug() const {
	if (!kernel_) return Ref<RoundOutcome>();
	opennova::world::RoundOutcomeView v;
	v.ended = kernel_->world.match.outcome().ended;
	v.winner_team = kernel_->world.match.outcome().winner_team;
	v.bluekills = kernel_->world.kill_stats.bluekills_by_player;
	v.greenkills = kernel_->world.kill_stats.greenkills_by_player;
	v.enemy_kills = kernel_->world.kill_stats.enemy_kills_by_player();
	v.team_kills_by_others = kernel_->world.kill_stats.team_kills_by_others;
	v.friendly_kills_by_others = kernel_->world.kill_stats.friendly_kills_by_others;
	v.enemy_kills_by_others = kernel_->world.kill_stats.enemy_kills_by_others;
	v.humans = kernel_->world.cached.humans;
	v.mp_session = kernel_->world.rules.mp_session;
	Ref<RoundOutcome> out;
	out.instantiate();
	out->assign(v);
	return out;
}

bool Simulation::has_event_fired(int index) const {
	if (!kernel_ || index < 0) return false;
	// The active latch + delay-elapsed window — the same read the original's Event
	// trigger category makes (engine: formats/mission/mission_params.h).
	return kernel_->events.event_fired(static_cast<size_t>(index));
}

int Simulation::get_event_count() const {
	return kernel_ ? static_cast<int>(kernel_->events.events().size()) : 0;
}

int64_t Simulation::get_logic_tick() const {
	// uint32 -> int64 keeps long sessions sign-safe on the GDScript side.
	return kernel_ ? static_cast<int64_t>(kernel_->world.logic_tick) : 0;
}

int64_t Simulation::get_main_frame_counter() const {
	return kernel_ ? static_cast<int64_t>(kernel_->world.hud_frames.main_frame) : 0;
}

int64_t Simulation::get_session_frame_counter() const {
	return kernel_ ? static_cast<int64_t>(kernel_->world.hud_frames.session_frame) : 0;
}

int64_t Simulation::get_hud_frame_counter() const {
	return kernel_ ? static_cast<int64_t>(kernel_->world.hud_frames.hud_frame) : 0;
}

void Simulation::set_panm_time_ms(int64_t p_time_ms) {
	kernel_->panm_time_override_ms = p_time_ms < 0
			? -1
			: static_cast<int64_t>(static_cast<uint32_t>(p_time_ms));
}

int64_t Simulation::get_panm_time_ms() const {
	return kernel_->panm_time_override_ms;
}

void Simulation::debug_set_panm_time_ms(int64_t p_time_ms) {
	set_panm_time_ms(p_time_ms);
}

namespace {
PackedInt32Array snapshot_bank(const opennova::world::World *world, int count,
							   int32_t (opennova::world::ScriptVarStore::*getter)(int) const) {
	PackedInt32Array out;
	out.resize(count);
	int32_t *w = out.ptrw();
	for (int i = 0; i < count; ++i) {
		w[i] = world ? (world->script.vars.*getter)(i) : 0;
	}
	return out;
}
} // namespace

PackedInt32Array Simulation::get_mission_variables_snapshot() const {
	return snapshot_bank(&kernel_->world, opennova::world::ScriptVarStore::kMissionVars,
	                     &opennova::world::ScriptVarStore::get_mission);
}

PackedInt32Array Simulation::get_global_variables_snapshot() const {
	return snapshot_bank(&kernel_->world, opennova::world::ScriptVarStore::kGlobalVars,
	                     &opennova::world::ScriptVarStore::get_global);
}

void Simulation::set_music_director(MusicDirector *director) {
	music_director_id_ = director ? ObjectID(director->get_instance_id()) : ObjectID();
}

std::shared_ptr<opennova::mus::MusGlobals> Simulation::wac_music_globals() const {
	MusicDirector *director = Object::cast_to<MusicDirector>(ObjectDB::get_instance(music_director_id_));
	return director ? director->globals_for_wac() : nullptr;
}

PackedInt32Array Simulation::get_music_variables_snapshot() const {
	PackedInt32Array values;
	const auto &globals = kernel_->wac.program().music_globals;
	if (globals) {
		values.resize(static_cast<int64_t>(opennova::mus::mus_globals_count(*globals)));
		for (int i = 0; i < values.size(); ++i)
			values.set(i, opennova::mus::mus_globals_read(*globals, static_cast<uint32_t>(i)));
	}
	return values;
}

void Simulation::set_global_variable(int index, int value) {
	if (kernel_) kernel_->world.script.vars.set_global(index, value);
}

int Simulation::get_global_variable(int index) const {
	return kernel_ ? kernel_->world.script.vars.get_global(index) : 0;
}

PackedByteArray Simulation::get_fired_events_snapshot() const {
	PackedByteArray out;
	if (!kernel_) return out;
	const size_t count = kernel_->events.events().size();
	out.resize(static_cast<int64_t>(count));
	uint8_t *w = out.ptrw();
	for (size_t i = 0; i < count; ++i) {
		w[i] = kernel_->events.event_fired(i) ? 1 : 0;
	}
	return out;
}
