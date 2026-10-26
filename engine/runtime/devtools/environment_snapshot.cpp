#include <runtime/devtools/environment_snapshot.h>

#include <formats/env/env_weather.h> // combine_terrain_light_packed, combine_ceiling_floor_packed
#include <runtime/environment/environment_state.h>
#include <runtime/mission/mission_kernel.h>

namespace opennova::devtools {

EnvironmentSnapshot environment_snapshot(const mission::MissionKernel &kernel, bool authority) {
	EnvironmentSnapshot out;
	const world::WeatherState &w = kernel.world.weather;
	const env::WeatherCore &core = w.core;
	out.valid = true;
	out.logic_tick = kernel.world.logic_tick;
	out.env_name = kernel.mission.get_environment();
	out.trn_name = kernel.mission.get_terrain();
	out.blink_flags = kernel.collision.local_player_blink_flags;
	out.fog_type = w.fog_type;
	out.fog_dist_metres = w.fog_current_q16() >> 16;
	out.fog_target_metres = w.fog_target_q16() >> 16;
	out.color_fade_seconds = (w.color_fade_ticks + 31) / 62;
	out.sun_fade_pct = w.sun_dim_pct_q16() >> 16;
	out.sun_fade_target_pct = core.scalar_channels.sun_dim_target_fp >> 16;
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
	// The page's OutDoor and InDoor rows are the tick's two blends of those blocks
	// (g_EnvTerrainLightCombined, g_EnvCeilingFloorBlend; env_weather.h carries the lanes).
	out.outdoor_rgb = env::combine_terrain_light_packed(out.sun_rgb, out.sky_rgb);
	out.indoor_rgb = env::combine_ceiling_floor_packed(out.ceiling_rgb, out.floor_rgb);
	out.gain_rgb = rgb(core.modulator_chain.modulator.render_color);
	out.iris_rgb = rgb(core.modulator_chain.modulator2.render_color);
	out.fov_degrees = core.scalar_channels.camera_fov_fp >> 16;
	out.sky_height_metres = w.sky_height_q16() >> 16;
	out.sky_speed = w.cloud_scroll_rate() >> 10;
	out.sky_speed_target = w.cloud_scroll_rate_target >> 10;
	out.rain_pct = static_cast<int32_t>((100u * w.rain_pct_current_q16()) >> 16);
	out.rain_target_pct = static_cast<int32_t>((100u * w.rain_pct_target_q16()) >> 16);
	out.overcast_pct = static_cast<int32_t>((100u * w.overcast_blend_q16()) >> 16);
	out.overcast_target_pct = static_cast<int32_t>((100u * w.overcast_target_q16()) >> 16);
	out.complexity = kernel.world.cached.local_player.valid()
			? kernel.collision.candidate_count(kernel.world.cached.local_player)
			: 0;
	out.minute_of_day = static_cast<int32_t>(
			env::EnvironmentState::hhmm_to_minute_of_day(w.tod_hhmm()));
	out.quake_ticks = static_cast<int32_t>(w.quake_ticks);
	out.precipitation_kind = static_cast<int32_t>(w.precipitation_kind);
	out.wind_scale = w.wind_scale();
	out.lightning_timer_a = core.lightning.timer_a;
	out.lightning_timer_b = core.lightning.timer_b;
	out.lightning_level = core.lightning.level;
	out.authority = authority;
	out.tod_keyframed = kernel.weather_render == nullptr || kernel.weather_render->tod_keyframed();
	return out;
}

} // namespace opennova::devtools
