// Simulation — core lifecycle: construction/reset, mission load + step,
// the kernel-boot binding legs, restart, WAC install/state, mission
// variables, perf counters.
// The class spans several TUs; see simulation_internal.h for the map.
#include "simulation/simulation_internal.h"
#include "simulation/weather_home_state.h" // the weather home's probe/test view
#include "util/axes.h"

#include "env/weather.h"
#include <runtime/environment/environment_state.h>
#include <base/io/fixed.h>

#include <runtime/mission/runtime_boot.h> // the S9 boot order + file-resolution policy
#include <runtime/inmatch/server_tick.h> // Server_RearmMinimapInitialScan (restart)
#include <runtime/terrain_query/surface_tiles.h> // the D-SND-15 placed-tile resolvers
#include <runtime/terrain_query/terrain_field_build.h> // the ONE cpt/trn(+charmap) field builder (ADR 0042 d4)

using namespace sim_internal;

namespace {

// Build the same synthetic patrol mission the C++ promote_test uses: 3 markers forming a
// path, one looping waypoint record (channel 1), 2 organics on that route, 1 building.
opennova::bms::File make_demo_mission() {
	opennova::bms::File m{};

	auto marker = [](int32_t x, int32_t y, int32_t z) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Marker;
		e.x = x; e.y = y; e.z = z;
		return e;
	};
	auto organic = [](int32_t x, int32_t y, int32_t z, uint8_t team, uint8_t wp_id) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Organic;
		e.x = x; e.y = y; e.z = z;
		e.yaw = 90;
		e.team = team;
		e.waypoint_id = wp_id;
		e.wp_number = 0;
		e.min_engagement_distance = 50 << 16;
		e.max_engagement_distance = 500 << 16;
		return e;
	};

	m.markers.push_back(marker(100 << 16, 0, 0));
	m.markers.push_back(marker(200 << 16, 0, 0));
	m.markers.push_back(marker(300 << 16, 0, 0));

	// The waypoint table is POSITIONAL — slot index == authored list id, slot 0
	// = the reserved no-route id. The looping patrol the organics author as
	// wp_id 1 sits at slot 1; the BLUE-flagged HUD route at slot 2.
	m.waypoint_records.resize(3);
	m.waypoint_records[1].flags = opennova::bms::WaypointFlags::None; // loops
	m.waypoint_records[1].marker_count = 3;
	m.waypoint_records[1].waypoint_numbers = {0, 1, 2};

	// A BLUE-flagged player route over the same markers: promotion builds the
	// HUD waypoint track from the first such record (marker 0 authors a wide
	// radius + a name id so the view surfaces meaningful fields).
	m.markers[0].wp_distance = 25;
	m.markers[0].ttool_index = 1;
	m.waypoint_records[2].flags = opennova::bms::WaypointFlags::BlueTeam;
	m.waypoint_records[2].marker_count = 3;
	m.waypoint_records[2].waypoint_numbers = {0, 1, 2};

	m.organics.push_back(organic(0, 0, 0, /*team=*/1, /*wp_id=*/1));
	m.organics.push_back(organic(50 << 16, 0, 0, /*team=*/2, /*wp_id=*/1));

	opennova::bms::Entity bldg{};
	bldg.type = opennova::bms::ItemType::Building;
	bldg.x = 999 << 16;
	m.buildings.push_back(bldg);

	// Authored SSNs (promotion copies record ids verbatim, like the original).
	m.organics[0].id = 1;
	m.organics[1].id = 2;
	m.buildings[0].id = 3;
	m.markers[0].id = 10;
	m.markers[1].id = 11;
	m.markers[2].id = 12;
	return m;
}

} // namespace

Simulation::Simulation() {
	session_.set_tick_observer(this);
	install_joiner_kit_seams();
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
	// The drawer's last-camera latch is mission-scoped: a stale one would
	// hand the next mission's first rain frame a bogus (clamped) streak.
	precipitation_draw_ = opennova::renderer::PrecipitationDrawState{};
	joiner_role_.reset_world_stream();
	invalidate_present_effect_pose_cache();
	// A fresh EntityRegistry restarts its spawn ids at 1, so the per-handle
	// dead/respawn mirrors cannot tell the next mission's occupant apart
	// from this one's by epoch: the lifecycle table dies with the world.
	pool_present_lifecycle_.clear();
	// One fresh kernel per load (ADR 0042 d3): the world, its systems, the
	// sim asset caches, the local-player weapon/loadout/view state and the
	// terrain field store all reset inside it. Retail reloads its model cache
	// per mission too, so the parse-once caches dying here is faithful.
	// Three pieces deliberately SURVIVE the swap, as they survived reset_world
	// before the kernel: the seat/mount table (it installs before mission
	// promotion — the wire-header join prewarms it pre-load), its graphic
	// sources, and the player's mouse settings.
	std::vector<opennova::mission::ItemSeatSpec> kept_seat_specs;
	std::unordered_map<int32_t, std::string> kept_mounted_graphics;
	opennova::world::PlayerLookSettings kept_look_settings;
	if (kernel_ != nullptr) {
		kept_seat_specs = std::move(kernel_->seat_specs);
		kept_mounted_graphics = std::move(kernel_->mounted_graphics);
		kept_look_settings = kernel_->local.look_settings;
	}
	kernel_ = std::make_unique<opennova::mission::MissionKernel>();
	local_role_.bind(*kernel_);
	host_role_.bind(*kernel_);
	joiner_role_.bind(*kernel_);
	kernel_->seat_specs = std::move(kept_seat_specs);
	kernel_->mounted_graphics = std::move(kept_mounted_graphics);
	kernel_->local.look_settings = kept_look_settings;
	kernel_->set_asset_index(
			asset_root_.is_valid() ? &asset_root_->native_index() : nullptr);
	kernel_->collision.set_trace_profile_enabled(runtime_profiling_enabled_);
	kernel_->profile.set_active(runtime_profiling_enabled_);
	// Mission-scoped, while weapon_profile_ is player-scoped and outlives every
	// load [orig: PlayerProfile_LoadAllFromDisk @0x54f4d0 runs from the startup
	// path, not Game_StartMission]: the RESIDENT BUFFER is rebuilt per mission,
	// so the next session must re-copy its side's page.
	weapon_profile_seeded_side_ = -1;
	// Round init clears the map mode and the zooms return to the spawn
	// defaults (witness at hud::HudMapControl — Game_InitNewRound /
	// Player_InitPlayer lifecycle).
	hud_map_control_.reset_spawn();
	apply_character_traits_to_world();
	// The fresh World's per-class ATTRIBUTES words (the medic plate / map
	// marker feed) come from the retained charattr table [orig: the
	// process-scoped g_CharAttr outlives every mission, CharAttr_LoadFromDef
	// @0x412140 runs once at boot; see docs/interface/hud-re.md].
	sync_class_attribute_flags();
	kernel_->world.rules.projectile_authority = !joiner_;
	kernel_->world.rules.mp_session = host_listen_ || joiner_;
	world_installed_ = false;
	last_sim_tick_us_ = 0;
	last_net_tick_us_ = 0;
	last_occlusion_build_us_ = 0;
	last_occlusion_probe_us_ = 0;
	last_present_snapshot_us_ = 0;
	last_present_entity_count_ = 0;
	reset_occlusion_apply_baseline();
	present_layout_.clear();
	++present_layout_revision_;
	// The retained per-load shell inputs are mission-scoped: drop them with
	// the world (the boot re-supplies them).
	collision_item_db_.unref();
	item_traits_db_.unref();
	infantry_adm_resource_root_.unref();
	infantry_adm_item_db_.unref();
	occlusion_culled_bms_.clear();
	minimap_snapshot_valid_ = false;
	// Rebuild the kernel's terrain store from the retained TerrainData (the
	// legacy pre-load set_terrain_height_field seam), then layer the shell-fed
	// surface extras back on.
	if (terrain_data_.is_valid() && terrain_data_->is_loaded()) {
		const std::vector<uint8_t> &charmap = terrain_data_->get_charmap_indices();
		opennova::terrain::terrain_field_store_build(kernel_->terrain_store,
				terrain_data_->get_cpt(), terrain_data_->get_trn(),
				charmap.empty() ? nullptr : charmap.data(),
				terrain_data_->get_charmap_width(),
				terrain_data_->get_charmap_height());
	}
	apply_terrain_to_ai();
	// The fresh world's collision/mounted-pose providers wire immediately (the
	// old reset did this unconditionally): the kernel registers itself so a
	// direct-loaded world resolves mounted poses before any boot step runs.
	apply_collision_to_ai();
}

opennova::world::WeatherState *Simulation::weather_state() {
	return world_installed_ && kernel_ ? &kernel_->world.weather : nullptr;
}

const opennova::world::WeatherState *Simulation::weather_state() const {
	return world_installed_ && kernel_ ? &kernel_->world.weather : nullptr;
}

void Simulation::seed_weather(const opennova::world::WeatherSeed &p_seed) {
	if (!world_installed_ || kernel_ == nullptr) return;
	kernel_->world.weather.seed(p_seed);
}

void Simulation::set_weather_render_owner(Weather *p_owner) {
	weather_owner_id_ = p_owner != nullptr ? ObjectID(p_owner->get_instance_id())
										: ObjectID();
	if (kernel_ == nullptr) return;
	kernel_->weather_render =
			p_owner != nullptr ? p_owner->render_tick_interface() : nullptr;
}

void Simulation::_release_weather_owner() {
	if (kernel_ != nullptr) {
		kernel_->weather_render = nullptr;
	}
	if (!weather_owner_id_.is_valid()) return;
	Weather *owner = Object::cast_to<Weather>(
			ObjectDB::get_instance(weather_owner_id_));
	weather_owner_id_ = ObjectID();
	if (owner != nullptr) {
		owner->release_simulation();
	}
}

bool Simulation::settle_weather_mission_start() {
	// Both roles run the initializer + settle (Game_StartMission is the
	// shared client/host path); only the WAC execution before it is the
	// authority's.
	if (!world_installed_ || kernel_ == nullptr) return false;
	kernel_->settle_weather_mission_start();
	return true;
}

const opennova::renderer::PrecipitationDrawFrame &Simulation::compile_precipitation_frame(
		const Vector3 &p_camera, const Vector3 &p_camera_right, const Vector3 &p_camera_up,
		int p_terrain_light_rgb) {
	if (!world_installed_ || kernel_ == nullptr) {
		precipitation_frame_.clear();
		precipitation_frame_.snow = false;
		return precipitation_frame_;
	}
	opennova::world::WeatherState &weather = kernel_->world.weather;
	// Godot (x, y, z) -> mission 16.16 (x, -z, y).
	const int32_t cam_q16[3] = {
		opennova::io::float_to_fp16_16_round_sat(p_camera.x),
		opennova::io::float_to_fp16_16_round_sat(-p_camera.z),
		opennova::io::float_to_fp16_16_round_sat(p_camera.y),
	};
	// The per-render update precedes the compile (retail the drawer calls
	// update_weather_particle_positions first @ 0x5dee65).
	if (weather.raining()) kernel_->update_precipitation(cam_q16[0], cam_q16[1], cam_q16[2]);
	opennova::renderer::PrecipitationCamera camera;
	for (int i = 0; i < 3; ++i) camera.position_q16[i] = cam_q16[i];
	camera.right[0] = p_camera_right.x;
	camera.right[1] = p_camera_right.y;
	camera.right[2] = p_camera_right.z;
	camera.up[0] = p_camera_up.x;
	camera.up[1] = p_camera_up.y;
	camera.up[2] = p_camera_up.z;
	opennova::renderer::PrecipitationDrawFrame &frame = precipitation_frame_;
	opennova::renderer::compile_precipitation_frame(weather.precipitation,
			weather.core.scalar_channels.rain_pct_fp, weather.precipitation_kind,
			static_cast<uint32_t>(p_terrain_light_rgb), camera, precipitation_draw_, frame);
	return frame;
}

TypedArray<WeatherSoundRow> Simulation::drain_weather_sounds() {
	TypedArray<WeatherSoundRow> out;
	if (!world_installed_ || kernel_ == nullptr) return out;
	for (const opennova::world::WeatherSoundEvent &ev : kernel_->world.out.weather_sounds) {
		Ref<WeatherSoundRow> d;
		d.instantiate();
		d->set_distance(static_cast<float>(ev.distance_q16) / 65536.0f);
		d->set_bearing(static_cast<int>(ev.bearing));
		out.push_back(d);
	}
	kernel_->world.out.weather_sounds.clear();
	return out;
}

Ref<WeatherHomeState> Simulation::get_weather_state() const {
	const opennova::world::WeatherState *w = weather_state();
	if (w == nullptr) return Ref<WeatherHomeState>();
	Ref<WeatherHomeState> out;
	out.instantiate();
	out->set_valid(w->valid);
	out->set_generation(static_cast<int64_t>(w->generation));
	out->set_command_generation(static_cast<int64_t>(w->command_generation));
	out->set_fog_target_q16(static_cast<int64_t>(w->fog_target_q16()));
	out->set_fog_current_q16(static_cast<int64_t>(w->fog_current_q16()));
	out->set_fog_accel_clamp(static_cast<int64_t>(w->fog_accel_clamp()));
	out->set_fog_type(w->fog_type);
	out->set_tod_fixed24(static_cast<int64_t>(w->tod_fixed24));
	out->set_tod_advance_per_tick(static_cast<int64_t>(w->tod_advance_per_tick));
	out->set_quake_ticks(static_cast<int64_t>(w->quake_ticks));
	out->set_cloud_scroll_rate_target(static_cast<int64_t>(w->cloud_scroll_rate_target));
	out->set_cloud_scroll_rate(static_cast<int64_t>(w->cloud_scroll_rate()));
	out->set_rain_pct_current_q16(static_cast<int64_t>(w->rain_pct_current_q16()));
	out->set_rain_pct_target_q16(static_cast<int64_t>(w->rain_pct_target_q16()));
	out->set_overcast_blend_q16(static_cast<int64_t>(w->overcast_blend_q16()));
	out->set_overcast_target_q16(static_cast<int64_t>(w->overcast_target_q16()));
	out->set_sun_dim_pct_q16(static_cast<int64_t>(w->sun_dim_pct_q16()));
	out->set_sky_height_q16(static_cast<int64_t>(w->sky_height_q16()));
	out->set_precipitation_kind(static_cast<int64_t>(w->precipitation_kind));
	out->set_lightning_color(static_cast<int64_t>(w->lightning_color));
	out->set_color_fade_ticks(static_cast<int64_t>(w->color_fade_ticks));
	out->set_wind_scale(static_cast<int64_t>(w->wind_scale()));
	out->set_lightning_timer_a(w->core.lightning.timer_a);
	out->set_lightning_timer_b(w->core.lightning.timer_b);
	out->set_lightning_level(w->core.lightning.level);
	out->set_night(w->is_night_phase());
	return out;
}

bool Simulation::native_environment_snapshot(
		opennova::devtools::EnvironmentSnapshot &out) const {
	out = opennova::devtools::EnvironmentSnapshot{};
	if (!world_installed_ || kernel_ == nullptr) return false;
	const opennova::world::WeatherState &w = kernel_->world.weather;
	const opennova::env::WeatherCore &core = w.core;
	// (retail Debug_DrawEnvironmentValues @ 0x4ef000 — the rows' sources)
	out.valid = true;
	out.logic_tick = kernel_->world.logic_tick;
	out.env_name = kernel_->mission.get_environment();
	out.trn_name = kernel_->mission.get_terrain();
	out.blink_flags = kernel_->collision.local_player_blink_flags;
	out.fog_type = w.fog_type;
	out.fog_dist_metres = w.fog_current_q16() >> 16;
	out.fog_target_metres = w.fog_target_q16() >> 16;
	out.color_fade_seconds = (w.color_fade_ticks + 31) / 62;
	out.sun_fade_pct = w.sun_dim_pct_q16() >> 16;
	out.night = w.is_night_phase();
	const auto rgb = [](uint32_t packed) { return packed & 0x00FFFFFFu; };
	out.fog_rgb = rgb(core.fog_block.render_color);
	out.skyfog_rgb = rgb(core.sky_color_blocks.skyfog.render_color);
	out.cloud_rgb = rgb(core.sky_color_blocks.cloud.render_color);
	out.sun_rgb = rgb(core.sun_block.render_color);
	out.lightning_rgb = rgb(w.lightning_color);
	out.sky_rgb = rgb(core.sky_block.render_color);
	out.ground_rgb = rgb(core.fill_block.render_color);
	out.ceiling_rgb = rgb(core.sky_color_blocks.ceiling.render_color);
	out.floor_rgb = rgb(core.sky_color_blocks.floor.render_color);
	// Env_TerrainLightCombined = light x 0xB5/256 + sky; Env_CeilingFloorBlend
	// = ceiling x 0xB5/256 + floor x 0xB5/256 (retail @ 0x57f0b3..0x57f110).
	const auto combine = [](uint32_t a, uint32_t a_scale, uint32_t b, uint32_t b_scale) {
		uint32_t out_rgb = 0;
		for (int shift = 0; shift < 24; shift += 8) {
			const uint32_t ca = ((a >> shift) & 0xFFu) * a_scale >> 8;
			const uint32_t cb = ((b >> shift) & 0xFFu) * b_scale >> 8;
			const uint32_t sum = ca + cb;
			out_rgb |= (sum > 0xFFu ? 0xFFu : sum) << shift;
		}
		return out_rgb;
	};
	out.outdoor_rgb = combine(out.sun_rgb, 0xB5u, out.sky_rgb, 256u);
	out.indoor_rgb = combine(out.ceiling_rgb, 0xB5u, out.floor_rgb, 0xB5u);
	out.gain_rgb = rgb(core.modulator_chain.modulator.render_color);
	out.iris_rgb = rgb(core.modulator_chain.modulator2.render_color);
	out.fov_degrees = static_cast<int32_t>(
			opennova::world::player_view_fov_h_deg(kernel_->local.view, 0, 1.0f));
	out.sky_height_metres = w.sky_height_q16() >> 16;
	out.sky_speed = w.cloud_scroll_rate() >> 10;
	out.rain_pct = static_cast<int32_t>((100u * w.rain_pct_current_q16()) >> 16);
	out.rain_target_pct = static_cast<int32_t>((100u * w.rain_pct_target_q16()) >> 16);
	out.overcast_pct = static_cast<int32_t>((100u * w.overcast_blend_q16()) >> 16);
	out.overcast_target_pct = static_cast<int32_t>((100u * w.overcast_target_q16()) >> 16);
	out.complexity = kernel_->world.cached.local_player.valid()
			? kernel_->collision.candidate_count(kernel_->world.cached.local_player)
			: 0;
	out.minute_of_day = static_cast<int32_t>(
			opennova::env::EnvironmentState::hhmm_to_minute_of_day(w.tod_hhmm()));
	out.quake_ticks = static_cast<int32_t>(w.quake_ticks);
	out.precipitation_kind = static_cast<int32_t>(w.precipitation_kind);
	out.wind_scale = w.wind_scale();
	out.lightning_timer_a = core.lightning.timer_a;
	out.lightning_timer_b = core.lightning.timer_b;
	out.lightning_level = core.lightning.level;
	out.authority = !joiner_;
	out.tod_keyframed = kernel_->weather_render == nullptr || kernel_->weather_render->tod_keyframed();
	return true;
}

// The MCP/debug rows' commands: authority-gated forwarders into the ONE
// command layer (world::EntityCommands, ADR 0042 d5).
#define OPENNOVA_WEATHER_COMMAND(call)                                    \
	if (!world_installed_ || joiner_ || kernel_ == nullptr) return false; \
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

#undef OPENNOVA_WEATHER_COMMAND

// Wire the kernel's terrain field into the world/AI/collision systems, then
// layer the shell-fed surface extras back on (the placed-tile override and the
// sound-profile chain the kernel deliberately leaves to the embedder).
void Simulation::apply_terrain_to_ai() {
	kernel_->wire_terrain();
	// The placed-tile override rides the surface view (D-SND-15). With no
	// charmap the sampler's early return-1 skips the walk exactly like
	// retail, so attaching the tiles unconditionally is faithful.
	kernel_->world.tables.surface_map.tiles =
			surface_tiles_.empty() ? nullptr : surface_tiles_.data();
	kernel_->world.tables.surface_map.tile_count = static_cast<int32_t>(surface_tiles_.size());
	kernel_->world.tables.surface_map.tile_surface = tile_surface_table_.data();
	apply_sound_state_to_world();
}

// (Re)apply the persisted sound-profile chain state to the current world: the parsed
// SndProf.def table and the water plane. Runs from apply_terrain_to_ai
// (reset_world / load) and from the setters when live. (The mission attrib
// dword the scream's night gate reads is stamped by the kernel boot from the BMS
// header — not re-applied here.)
void Simulation::apply_sound_state_to_world() {
	if (!kernel_) return;
	kernel_->world.tables.sound_profiles.clear();
	if (!sndprof_text_.empty())
		kernel_->world.tables.sound_profiles.parse(reinterpret_cast<const char *>(sndprof_text_.data()),
		                             sndprof_text_.size());
	kernel_->world.env.water_z = env_water_z_q16_;
	kernel_->sync_water_plane();
}

void Simulation::set_terrain_height_field(const Ref<TerrainData> &p_terrain) {
	// Clear first so a null/unloaded terrain disables grounding. The store is
	// the engine's one owning cpt/trn(+charmap) field builder (ADR 0042 d4),
	// living on the kernel; the Ref is retained so a reload (which recreates
	// the kernel) can rebuild the store from the same source.
	terrain_data_ = p_terrain;
	kernel_->terrain_store.clear();
	if (p_terrain.is_valid() && p_terrain->is_loaded()) {
		const std::vector<uint8_t> &charmap = p_terrain->get_charmap_indices();
		opennova::terrain::terrain_field_store_build(kernel_->terrain_store,
				p_terrain->get_cpt(), p_terrain->get_trn(),
				charmap.empty() ? nullptr : charmap.data(),
				p_terrain->get_charmap_width(), p_terrain->get_charmap_height());
	}
	apply_terrain_to_ai();
}

void Simulation::set_sound_profiles(const PackedByteArray &p_sndprof_text) {
	sndprof_text_.assign(p_sndprof_text.ptr(), p_sndprof_text.ptr() + p_sndprof_text.size());
	apply_sound_state_to_world();
}

void Simulation::apply_character_traits_to_world() {
	if (!kernel_) return;
	kernel_->world.tables.character_traits.clear();
	for (const CharacterSexRow &row : character_sex_rows_)
		kernel_->world.tables.character_traits.set(row.character_id, row.female);
}

void Simulation::set_water_z(double p_water_y) {
	env_water_z_q16_ = static_cast<int32_t>(p_water_y * 65536.0);
	if (kernel_) {
		kernel_->world.env.water_z = env_water_z_q16_;
		kernel_->sync_water_plane();
	}
}

TypedArray<SlotSoundRow> Simulation::drain_slot_sounds() {
	TypedArray<SlotSoundRow> out;
	if (!world_installed_) return out;
	for (const opennova::world::SoundSlotEvent &ev : kernel_->world.out.slot_sounds) {
		Ref<SlotSoundRow> d;
		d.instantiate();
		d->set_soundset(String(ev.set_name));
		// Mission-frame 16.16 -> godot (x, z, -y), same mapping as the fire drain.
		d->set_pos(Vector3(static_cast<float>(ev.pos[0]) / 65536.0f,
				static_cast<float>(ev.pos[2]) / 65536.0f,
				static_cast<float>(-ev.pos[1]) / 65536.0f));
		d->set_handle(static_cast<int>(ev.source_handle));
		d->set_slot(static_cast<int>(ev.slot));
		out.push_back(d);
	}
	kernel_->world.out.slot_sounds.clear();
	return out;
}

TypedArray<SoundEmitterRow> Simulation::drain_sound_emitters() {
	TypedArray<SoundEmitterRow> out;
	if (!world_installed_) return out;
	const std::vector<opennova::world::SoundEmitterEvent> events =
			kernel_->world.out.sound_emitters.drain();
	for (const opennova::world::SoundEmitterEvent &ev : events) {
		Ref<SoundEmitterRow> d;
		d.instantiate();
		d->set_source_spawn_id(static_cast<int64_t>(ev.source_spawn_id));
		d->set_handle(static_cast<int>(ev.source_handle));
		d->set_source_bms_id(static_cast<int>(ev.source_bms_id));
		// Mission coordinates -> Godot (x, z, -y), matching every other
		// positional presentation drain.
		d->set_pos(mission_to_godot(ev.pos));
		d->set_lane(static_cast<int>(ev.lane));
		d->set_slot(static_cast<int>(ev.slot));
		d->set_lifetime(static_cast<int>(ev.lifetime_ticks));
		d->set_emitted_tick(static_cast<int64_t>(ev.emitted_tick));
		d->set_pitch_q16(static_cast<int>(ev.pitch_q16));
		d->set_volume_q8_8(static_cast<int>(ev.volume_q8_8));
		d->set_source_only(ev.source_only);
		d->set_soundset(String(ev.set_name.c_str()));
		out.push_back(d);
	}
	return out;
}

// The binding legs every kernel boot shares, run AFTER kernel_->boot():
// session-header capture for the LAN 0x0B burst, the HUD map zoom seed, the
// score-row re-resolve, and the held-WacProgram re-apply (a program installed
// through set_wac_program survives reloads; the kernel's own layered load
// wins whenever the mission authored .wac files).
void Simulation::finish_kernel_boot() {
	world_installed_ = true;
	apply_host_session_mission_header(kernel_->mission);
	// The mission's authored map_zoom scales BOTH radar-zoom spawn defaults
	// (witness at hud::HudMapControl::set_mission_map_zoom — the
	// Player_InitPlayer derivation off the BMS header float).
	hud_map_control_.set_mission_map_zoom(kernel_->mission.header.map_zoom);
	// The score row keys off the mission's game-mode bit, so re-resolve it now
	// that the flags are known (the config may load before OR after the boot).
	refresh_score_rules();
	if (!kernel_->wac_loaded && wac_program_ && wac_program_->is_ok())
		kernel_->wac.set_program(wac_program_->native_program());
}

// The kernel boot's bringup_net_session hook for this sim's role: the listen
// host stands its npruntime session up between the world wiring and
// the system registration [orig: SinglePlayer_StartMission @0x561af0]; a
// joiner (re)builds its non-authority ClientRuntime at the same point. The
// bare no-net world installs only the decode-view class table.
std::function<void()> Simulation::role_bringup_hook() {
	if (listen_server_ && !joiner_) {
		return [this] {
			bringup_host_runtime();
			install_item_class_resolver();
		};
	}
	if (joiner_) {
		return [this] {
			// A retail-style menu join has already authenticated and learned the
			// map from S2C 0x7B before this local load. Preserve that exact
			// runtime/socket; rebuilding it here would silently reconnect and
			// discard the witnessed pre-load session. Direct-loaded callers have
			// not started yet and retain the historical fresh-runtime reset.
			if (!joiner_role_.started() || !runtime_) {
				runtime_ = &joiner_role_.create_runtime(joiner_player_name_, join_role_,
						join_spectator_password_);
				joiner_role_.reset_for_runtime_rebuild();
				install_charattr_challenge_table();
				install_character_join_vars();
				install_join_integrity_profile();
				install_expansion_version_root();
			}
			runtime_->set_world_ready(true);
			joiner_role_.reset_for_load(runtime_->deployment_release_revision());
			joiner_applied_loadout_revision_ = 0;
			kernel_->local.loadout.pending_player_class = -1; // the shell re-applies the kit after each load
			deploy_zone_registry_built_ = false; // fresh world -> fresh zone registry
			// Re-arm the 0x2F submission seam from the carried sim state. The fresh
			// kernel holds an EMPTY weapon catalog, so this is a deliberate no-op
			// that leaves the capture-default fallback armed; the real arm happens
			// when the shell's load_weapon_table lands.
			push_joiner_loadout_kit();
			install_item_class_resolver();
		};
	}
	return [this] { install_item_class_resolver(); };
}

void Simulation::apply_host_session_mission_header(const opennova::bms::File &file) {
	std::vector<uint8_t> header_blob;
	std::string error;
	if (opennova::bms::encode_loaded_header_blob(file, header_blob, error)) {
		host_session_config_.mission_header_blob = std::move(header_blob);
	} else {
		host_session_config_.mission_header_blob.clear();
	}

	const std::string mission_name = file.get_mission_name();
	if (!mission_name.empty()) {
		host_session_config_.mission_name = mission_name;
		if (host_session_config_.spawn_names.empty()) {
			host_session_config_.spawn_names.push_back(mission_name);
		}
	}
	// host_session_config_ is consumed by create_session at the next load through
	// bringup_host_runtime; there is deliberately nothing to refresh live.
}

// The production boot (S9/ADR 0042 d3): the kernel owns the ordered step
// sequence (MissionKernel::boot, recorded in its boot_trace); this entry only
// converts the Godot Refs into the kernel's sources (the mounted index, the
// parsed items.def rows, the terrain documents, the mission text) and runs the
// binding-side legs the kernel deliberately leaves to the shell (net role
// bring-up context, score.ini, the joiner's decoded-row resolver inputs, the
// session-kit profile seed).
int64_t Simulation::boot_mission(const Ref<MissionData> &p_mission,
		const Ref<ResourceRoot> &p_resource_root,
		const Ref<ItemDatabase> &p_item_db,
		const Ref<TerrainData> &p_terrain,
		const PackedByteArray &p_terrain_til, const String &p_wac_basename,
		const String &p_infantry_adm, const String &p_mission_file_basename,
		bool p_playable) {
	namespace ms = opennova::mission;
	if (p_mission.is_null() || !begin_session_load()) {
		fail_session_load("mission boot failed");
		return ERR_CANT_OPEN;
	}
	ms::BootFileSource files;
	if (p_resource_root.is_valid()) {
		const opennova::ResourceIndex *index = &p_resource_root->native_index();
		files.has_file = [index](const std::string &name) {
			return index->has_file(name);
		};
		files.read_file = [index](const std::string &name,
				std::vector<uint8_t> &out) {
			return index->read_file(name, out);
		};
	}
	// The mission text resolves BEFORE the kernel boots: the parsed briefings/
	// locations feed the host bring-up context (S2C 0x7E/0x0F) and the
	// [PeopleNames] table feeds the kernel's promote-time name resolver.
	{
		std::vector<uint8_t> text;
		(void)ms::resolve_mission_text(files,
				std::string(p_mission_file_basename.utf8().get_data()), text);
		PackedByteArray bytes;
		bytes.resize(static_cast<int64_t>(text.size()));
		if (!text.empty()) std::memcpy(bytes.ptrw(), text.data(), text.size());
		set_mission_text_data(bytes);
	}
	// The mission's raw .til bytes: the S2C 0x45 stream source AND the
	// placed-tile surface array (D-SND-15).
	set_terrain_til_data(p_terrain_til);
	// The previous mission's retained terrain must not rebuild into the fresh
	// kernel: set_terrain_height_field below builds the store exactly once.
	terrain_data_.unref();

	reset_world();
	if (p_resource_root.is_valid()) set_asset_root(p_resource_root);
	if (p_item_db.is_valid()) {
		// Hand the shell's parsed items.def rows over as the kernel's item
		// table (the Ref pins their lifetime for the kernel's).
		item_traits_db_ = p_item_db;
		kernel_->set_items_table(&p_item_db->native_items());
	}
	joiner_role_.set_wire_header_world(p_mission->is_wire_header_only());
	kernel_->open_document(p_mission->native_file(),
			std::string(p_mission_file_basename.utf8().get_data()), files);
	// Terrain fills the kernel store BEFORE boot (has_terrain gates on it),
	// exactly the ctest embedder's order; the D-SND-15 .TSD tile table rides
	// beside it.
	set_terrain_height_field(p_terrain);
	tile_surface_table_.fill(0);
	if (p_terrain.is_valid() && p_terrain->is_loaded() && files.valid()) {
		opennova::terrain::SurfaceTileFileSource tile_files;
		tile_files.has_file = files.has_file;
		tile_files.read_file = files.read_file;
		opennova::terrain::resolve_tileset_surface_table(tile_files,
				p_terrain->get_trn().tilestrip, tile_surface_table_.data());
	}
	apply_terrain_to_ai();
	// SndProf.def -> the footstep/foley/landing/scream slot table: the kernel
	// leaves the sound-profile chain to the presentation-owning embedder.
	if (files.valid() && files.has_file("SndProf.def")) {
		std::vector<uint8_t> text;
		if (files.read_file("SndProf.def", text)) {
			PackedByteArray bytes;
			bytes.resize(static_cast<int64_t>(text.size()));
			if (!text.empty()) std::memcpy(bytes.ptrw(), text.data(), text.size());
			set_sound_profiles(bytes);
		}
	}

	ms::KernelBootOptions options;
	options.playable = p_playable;
	options.joiner = joiner_;
	// The shell owns the terrain field's parsed-document entry (the store the
	// setter above built, or none): the kernel never loads one from files here.
	options.terrain = false;
	options.wac = !p_wac_basename.is_empty();
	options.wac_basename = std::string(p_wac_basename.utf8().get_data());
	options.game_type = opennova::game_type::for_mission_attribs(kernel_->mission.header.attrib_flags);
	options.infantry_adm = p_infantry_adm.is_empty()
			? std::string(ms::kDefaultInfantryAdm)
			: std::string(p_infantry_adm.utf8().get_data());
	options.people_name_resolver = [this](int32_t index) {
		const auto it = mission_people_names_.find(index);
		return it != mission_people_names_.end() ? it->second : std::string();
	};
	options.bringup_net_session = role_bringup_hook();
	std::string boot_error;
	if (!kernel_->boot(options, boot_error)) {
		fail_session_load(boot_error.c_str());
		return ERR_CANT_OPEN;
	}
	finish_kernel_boot();
	// The binding-side resolver inputs (the joiner's decoded rows read the
	// anim root/item db Refs) and the net-typed re-stamps the kernel's
	// net-free boot cannot make: the wire entity classes from the netsim
	// ItemReplicationCatalog, then the collision Ref retention.
	if (p_resource_root.is_valid() && p_item_db.is_valid()) {
		infantry_adm_resource_root_ = p_resource_root;
		infantry_adm_item_db_ = p_item_db;
	}
	if (p_item_db.is_valid()) {
		resolve_item_traits(p_item_db);
		collision_item_db_ = p_item_db;
	}
	// score.ini rides the boot's session-data step. DIVERGENCE (placement):
	// retail loads it far earlier, when it builds the default gametype settings
	// [orig: GameType_CreateDefaultSettings @0x52DD00], not at mission boot.
	// The observable behaviour is the same because refresh_score_rules
	// re-resolves the row from the mission's game-mode bit in either order.
	if (p_resource_root.is_valid() &&
			load_score_config(p_resource_root, "score.ini") != OK)
		UtilityFunctions::push_warning(
				"MissionRoot: score.ini not loaded — kill scoring inert (no 0x81)");
	// In a live session the resident kit buffer is the assigned side's profile
	// page (retail's Game_StartMission copy into restrictionData
	// [orig: @0x525813]); the kernel's table load built the pool from the
	// mission kit, so re-seed + rebuild when a session profile applies.
	if (seed_session_kit_from_profile())
		rebuild_local_player_loadout(/*p_select_spawn_default=*/true);
	push_joiner_loadout_kit();
	complete_session_load();
	return OK;
}

bool Simulation::load_from_mission_data(const Ref<MissionData> &p_mission) {
	if (p_mission.is_null()) return false;
	if (!begin_session_load()) return false;
	reset_world();
	// Do not infer this from `joiner_`: tests/tools and legacy direct joins may
	// still load a complete BMS, whose authored promotion is already canonical.
	// Only the production 616-byte S2C header needs wire-time materialization.
	joiner_role_.set_wire_header_world(p_mission->is_wire_header_only());
	// The editor's live, in-memory mission (unsaved edits included) adopts
	// into the kernel with NO file source: the file-fed boot steps skip and
	// this stays the bare promote + systems + role bring-up path.
	kernel_->open_document(p_mission->native_file(),
			std::string(), opennova::mission::BootFileSource{});
	opennova::mission::KernelBootOptions options;
	options.playable = false; // callers spawn explicitly (or the listen bring-up auto-spawns)
	options.joiner = joiner_;
	options.game_type = opennova::game_type::for_mission_attribs(kernel_->mission.header.attrib_flags);
	options.bringup_net_session = role_bringup_hook();
	std::string boot_error;
	if (!kernel_->boot(options, boot_error)) {
		fail_session_load(boot_error.c_str());
		return false;
	}
	finish_kernel_boot();
	complete_session_load();
	return true;
}

void Simulation::build_demo_mission() {
	if (!begin_session_load()) return;
	reset_world();
	host_session_config_.mission_file = "demo.bms";
	kernel_->open_document(make_demo_mission(), std::string(),
			opennova::mission::BootFileSource{});
	opennova::mission::KernelBootOptions options;
	options.playable = false;
	options.joiner = joiner_;
	options.game_type = opennova::game_type::for_mission_attribs(kernel_->mission.header.attrib_flags);
	options.bringup_net_session = role_bringup_hook();
	std::string boot_error;
	if (!kernel_->boot(options, boot_error)) {
		fail_session_load(boot_error.c_str());
		return;
	}
	finish_kernel_boot();
	complete_session_load();
}

// The Godot legs after the role's baseline restore (reset_session): the
// engine half — the local weapon's epoch resets (the borrowed-UseGun
// reinstall event), the world + WAC runtime rewind, the view reset, the
// fresh-soldier .adm re-ground and the shell's item-trait re-stamp — is the
// kernel's restore; the host role rebuilt its HostClient view from the
// restored pools and re-armed the minimap scan; the joiner role re-armed its
// rematerialization fold.
void Simulation::restore_world_baseline() {
	if (!world_installed_) return;
	runtime_ = active_role().client_runtime();
	// The logic tick rewinds and the runtime may be recreated below — a cached
	// minimap snapshot keyed on (revision, tick) could collide across epochs.
	minimap_snapshot_valid_ = false;
	if (joiner_role_.wire_header_world()) {
		// ClientState survives Stop/Start, while the body-empty baseline removes
		// its registry carriers. Force one exact rematerialization fold; retain the
		// already-built portal tables because their handles remain identical and
		// the occlusion models' weld records are intentionally one-shot mutable.
		deploy_zone_registry_built_ = false;
	}
	if (collision_item_db_.is_valid())
		resolve_collision_instances(collision_item_db_);
	// The restored world can share a tick number with a previously cached view.
	// Force the next FollowOwner query to rebuild against the post-restart epoch.
	invalidate_present_effect_pose_cache();
}

void Simulation::set_wac_program(std::shared_ptr<WacProgram> p_program) {
	wac_program_ = std::move(p_program);
	if (!world_installed_) {
		return; // the next kernel boot applies it (finish_kernel_boot)
	}
	if (wac_program_ && wac_program_->is_ok()) {
		kernel_->wac.set_program(wac_program_->native_program());
	} else {
		kernel_->wac.set_program(opennova::wac::Program());
	}
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
	env.registry = &kernel_->world.registry;
	opennova::wac::Program program = opennova::wac::compile_program(sources, env);
	auto holder = std::make_shared<WacProgram>();
	// Adopt the registry-compiled program into the holder so the retained
	// WacProgram carries its diagnostics either way.
	holder->adopt(std::move(program));
	wac_program_ = std::move(holder);
	if (!wac_program_->is_ok()) {
		return false;
	}
	kernel_->wac.set_program(wac_program_->native_program());
	return true;
}

bool Simulation::run_mission_start_wac() {
	if (!world_installed_ || joiner_) return false;
	return kernel_->wac.execute_initial(kernel_->world);
}

void Simulation::seal_mission_start_baseline() {
	if (!world_installed_ || joiner_) return;
	kernel_->capture_baseline();
}

void Simulation::set_runtime_profiling_enabled(bool p_enabled) {
	if (runtime_profiling_enabled_ == p_enabled) return;
	runtime_profiling_enabled_ = p_enabled;
	last_sim_tick_us_ = 0;
	last_net_tick_us_ = 0;
	last_present_snapshot_us_ = 0;
	last_occlusion_build_us_ = 0;
	last_occlusion_probe_us_ = 0;
	frame_net_us_ = 0;
	frame_sim_us_ = 0;
	frame_sink_us_ = 0;
	if (kernel_ != nullptr) {
		kernel_->profile.reset();
		kernel_->profile.set_active(p_enabled);
		kernel_->collision.set_trace_profile_enabled(p_enabled);
	}
}

Vector4i Simulation::get_last_projectile_trace_times_us() const {
	const opennova::world::CollisionWorld::TraceProfile &tp =
			kernel_->collision.trace_profile();
	return Vector4i(trace_profile_lane(tp.terrain_us),
			trace_profile_lane(tp.static_us),
			trace_profile_lane(tp.dynamic_us),
			trace_profile_lane(tp.person_us));
}

Vector4i Simulation::get_last_projectile_trace_counts() const {
	const opennova::world::CollisionWorld::TraceProfile &tp =
			kernel_->collision.trace_profile();
	return Vector4i(trace_profile_lane(tp.calls),
			trace_profile_lane(tp.static_survivors),
			trace_profile_lane(tp.dynamic_survivors),
			trace_profile_lane(tp.person_survivors));
}

Vector2i Simulation::get_last_projectile_trace_faces() const {
	const opennova::world::CollisionWorld::TraceProfile &tp =
			kernel_->collision.trace_profile();
	return Vector2i(trace_profile_lane(tp.static_faces),
			trace_profile_lane(tp.dynamic_faces));
}

Dictionary Simulation::get_runtime_perf_counters() const {
	Dictionary out;
	out["loaded"] = is_loaded();
	out["listen_server"] = listen_server_;
	out["ai_count"] = kernel_ ? kernel_->world.ai.count() : 0;
	out["present_entity_count"] = last_present_entity_count_;
	out["sim_tick_us"] = static_cast<int64_t>(last_sim_tick_us_);
	out["net_tick_us"] = static_cast<int64_t>(last_net_tick_us_);
	out["present_snapshot_us"] = static_cast<int64_t>(last_present_snapshot_us_);
	out["occlusion_build_us"] = static_cast<int64_t>(last_occlusion_build_us_);
	out["occlusion_probe_us"] = static_cast<int64_t>(last_occlusion_probe_us_);
	out["runtime_profiling_enabled"] = runtime_profiling_enabled_;
	// The last tick's projectile-trace attribution (collision.h TraceProfile).
	const opennova::world::CollisionWorld::TraceProfile &tp =
			kernel_->collision.trace_profile();
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
	if (!kernel_ || joiner_) return ERR_UNAVAILABLE;
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
	return kernel_->world.commands.mount(static_cast<uint16_t>(lp->net_id),
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
// KILL_GROUP action uses [orig: EventAction_Dispatch case 2 @0x4542e0]; it invents no
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

opennova::world::EntityCommands *Simulation::entity_commands() {
	return kernel_ ? &kernel_->world.commands : nullptr;
}

int Simulation::get_mission_variable(int index) const {
	return kernel_ ? kernel_->world.script.vars.get_mission(index) : 0;
}

Ref<RoundOutcome> Simulation::get_round_outcome_debug() const {
	if (!kernel_) return Ref<RoundOutcome>();
	Ref<RoundOutcome> out;
	out.instantiate();
	out->set_ended(kernel_->world.match.outcome().ended);
	out->set_winner_team(kernel_->world.match.outcome().winner_team);
	out->set_bluekills(kernel_->world.kill_stats.bluekills_by_player);
	out->set_greenkills(kernel_->world.kill_stats.greenkills_by_player);
	out->set_enemy_kills(kernel_->world.kill_stats.enemy_kills_by_player);
	out->set_team_kills_by_others(kernel_->world.kill_stats.team_kills_by_others);
	out->set_friendly_kills_by_others(kernel_->world.kill_stats.friendly_kills_by_others);
	out->set_enemy_kills_by_others(kernel_->world.kill_stats.enemy_kills_by_others);
	out->set_humans(kernel_->world.cached.humans);
	out->set_mp_session(kernel_->world.rules.mp_session);
	return out;
}

bool Simulation::has_event_fired(int index) const {
	if (!kernel_ || index < 0) return false;
	// The active latch + delay-elapsed window — the same read the original's Event
	// trigger category makes [orig: EventTrigger_EvaluateCondition @0x453620 case 3].
	return kernel_->events.event_fired(static_cast<size_t>(index));
}

int Simulation::get_event_count() const {
	return kernel_ ? static_cast<int>(kernel_->events.events().size()) : 0;
}

int64_t Simulation::get_logic_tick() const {
	// uint32 -> int64 keeps long sessions sign-safe on the GDScript side.
	return kernel_ ? static_cast<int64_t>(kernel_->world.logic_tick) : 0;
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

PackedInt32Array Simulation::get_music_variables_snapshot() const {
	return snapshot_bank(&kernel_->world, opennova::world::ScriptVarStore::kMusicVars,
	                     &opennova::world::ScriptVarStore::get_music);
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

void Simulation::set_loco_scale(int p_scale) {
	if (kernel_) kernel_->world.ai.loco_scale = p_scale;
}

int Simulation::get_loco_scale() const {
	return kernel_ ? kernel_->world.ai.loco_scale : 0;
}
