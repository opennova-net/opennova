// Simulation — ClassDB registration.
#include "simulation/simulation_internal.h"

#include "rtxt/rtxt_string_file.h" // the gametext table type the end-round / deploy feeds bind
#include "simulation/end_round_state.h" // the typed end-of-round record
#include "simulation/entity_card.h" // the typed inspection records (ADR 0042 d5)
#include "simulation/entity_row.h"

#include <formats/threedi/threedi_3di3.h> // THREEDI_USER_POINT_SCAN_LIMIT (pin below)

using namespace sim_internal;

// The GDScript-facing seat codes are the SAME values engine/runtime/world computes with —
// a drifted copy here would silently corrupt every binding-side seat-spec walk.
static_assert(Simulation::SEAT_NONE == static_cast<int>(opennova::world::SeatType::None));
static_assert(Simulation::SEAT_PASSENGER == static_cast<int>(opennova::world::SeatType::Passenger));
static_assert(Simulation::SEAT_CONTROLLER == static_cast<int>(opennova::world::SeatType::Controller));
static_assert(Simulation::SEAT_GUNNER == static_cast<int>(opennova::world::SeatType::Gunner));
static_assert(Simulation::SEAT_ARMORY_POINT == static_cast<int>(opennova::world::SeatType::ArmoryPoint));
static_assert(Simulation::SEAT_DRIVER == static_cast<int>(opennova::world::SeatType::Driver));

// Values the class enum cannot assign straight from the engine home: the
// userpoint scan limit lives in a C header macro, and the integral fov mirror
// pins against the engine float.
static_assert(Simulation::ITEM_USER_POINT_SCAN_LIMIT == THREEDI_USER_POINT_SCAN_LIMIT);
static_assert(static_cast<float>(Simulation::DEFAULT_PLAYER_FOV_H_DEG) ==
              opennova::world::kPlayerCameraFovHDeg);

void Simulation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("settle_weather_mission_start"),
			&Simulation::settle_weather_mission_start);
	ClassDB::bind_method(D_METHOD("compile_precipitation_frame", "camera", "camera_right",
			"camera_up", "terrain_light_rgb"),
			&Simulation::compile_precipitation_frame);
	ClassDB::bind_method(D_METHOD("drain_weather_sounds"), &Simulation::drain_weather_sounds);
	ClassDB::bind_method(D_METHOD("get_weather_state"), &Simulation::get_weather_state);
	ClassDB::bind_method(D_METHOD("weather_state_bound"), &Simulation::weather_state_bound);
	ClassDB::bind_method(D_METHOD("command_rain", "percent", "seconds"), &Simulation::command_rain);
	ClassDB::bind_method(D_METHOD("command_snow", "percent", "seconds"), &Simulation::command_snow);
	ClassDB::bind_method(D_METHOD("command_overcast", "percent", "seconds"), &Simulation::command_overcast);
	ClassDB::bind_method(D_METHOD("command_fog_distance", "metres"), &Simulation::command_fog_distance);
	ClassDB::bind_method(D_METHOD("command_move_fog", "metres", "seconds"), &Simulation::command_move_fog);
	ClassDB::bind_method(D_METHOD("command_sky_speed", "rate"), &Simulation::command_sky_speed);
	ClassDB::bind_method(D_METHOD("command_quake", "seconds"), &Simulation::command_quake);
	ClassDB::bind_method(D_METHOD("command_time_of_day_minutes", "minute_of_day"),
			&Simulation::command_time_of_day_minutes);
	ClassDB::bind_method(D_METHOD("debug_set_time_of_day_minutes", "minute_of_day"),
			&Simulation::debug_set_time_of_day_minutes);
	ClassDB::bind_method(D_METHOD("command_fog_type", "type"), &Simulation::command_fog_type);
	ClassDB::bind_method(D_METHOD("command_lightning_flash"), &Simulation::command_lightning_flash);
	ClassDB::bind_method(D_METHOD("command_lightning_far_flash"), &Simulation::command_lightning_far_flash);
	ClassDB::bind_method(D_METHOD("command_wind_scale", "value"), &Simulation::command_wind_scale);
	ClassDB::bind_method(D_METHOD("load_from_mission_data", "mission"), &Simulation::load_from_mission_data);
	ClassDB::bind_method(D_METHOD("boot_mission", "mission", "resource_root",
			"item_db", "terrain", "terrain_til",
			"wac_basename", "infantry_adm", "mission_file_basename", "playable"),
			&Simulation::boot_mission);
	ClassDB::bind_method(D_METHOD("build_demo_mission"), &Simulation::build_demo_mission);
	ClassDB::bind_method(D_METHOD("is_loaded"), &Simulation::is_loaded);
	ClassDB::bind_method(D_METHOD("is_playing"), &Simulation::is_playing);
	ClassDB::bind_method(D_METHOD("step"), &Simulation::step);
	ClassDB::bind_static_method("Simulation", D_METHOD("presentation_forward", "yaw_deg", "pitch_deg"),
			&Simulation::presentation_forward);
	ClassDB::bind_static_method("Simulation", D_METHOD("aim_ray_endpoint", "eye", "yaw_deg", "pitch_deg"),
			&Simulation::aim_ray_endpoint);
	ClassDB::bind_static_method("Simulation", D_METHOD("rangefinder_units", "position", "endpoint"),
			&Simulation::rangefinder_units);
	ClassDB::bind_static_method("Simulation", D_METHOD("viewmodel_camera_local_from_view", "view_units"),
			&Simulation::viewmodel_camera_local_from_view);
	ClassDB::bind_static_method("Simulation", D_METHOD("viewmodel_bias_euler_rad", "rot_bias_deg"),
			&Simulation::viewmodel_bias_euler_rad);
	ClassDB::bind_static_method("Simulation", D_METHOD("viewmodel_rig_yaw_deg"),
			&Simulation::viewmodel_rig_yaw_deg);
	ClassDB::bind_static_method("Simulation", D_METHOD("weapon_render_fov_h_deg_default"),
			&Simulation::weapon_render_fov_h_deg_default);
	ClassDB::bind_static_method("Simulation", D_METHOD("viewmodel_team_byte", "team"),
			&Simulation::viewmodel_team_byte);
	ClassDB::bind_static_method("Simulation", D_METHOD("predict_mount_seat", "seats", "command_id"),
			&Simulation::predict_mount_seat);
	ClassDB::bind_static_method("Simulation", D_METHOD("tick_dt"),
			&Simulation::tick_dt);
	ClassDB::bind_static_method("Simulation", D_METHOD("ticks_from_ms", "ms"),
			&Simulation::ticks_from_ms);
	ClassDB::bind_method(D_METHOD("advance_session_frame", "input", "tick_sink"),
			&Simulation::advance_session_frame, DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("step_session_frame", "input", "tick_sink"),
			&Simulation::step_session_frame, DEFVAL(Callable()));
	ClassDB::bind_method(D_METHOD("pause_session"), &Simulation::pause_session);
	ClassDB::bind_method(D_METHOD("resume_session"), &Simulation::resume_session);
	ClassDB::bind_method(D_METHOD("reset_session"), &Simulation::reset_session);
	ClassDB::bind_method(D_METHOD("close_session"), &Simulation::close_session);
	ClassDB::bind_method(D_METHOD("get_session_perf"),
			&Simulation::get_session_perf);
	ClassDB::bind_method(D_METHOD("set_frame_stats", "stats"),
			&Simulation::set_frame_stats);
	ClassDB::bind_method(D_METHOD("get_frame_stats"),
			&Simulation::get_frame_stats);
	ClassDB::bind_method(D_METHOD("get_last_session_sim_us"),
			&Simulation::get_last_session_sim_us);
	ClassDB::bind_method(D_METHOD("is_transport_locked"), &Simulation::is_transport_locked);
	ClassDB::bind_method(D_METHOD("session_role"), &Simulation::session_role);
	ClassDB::bind_method(D_METHOD("session_state"), &Simulation::session_state);
	ClassDB::bind_method(D_METHOD("enable_listen_server", "enable"), &Simulation::enable_listen_server);
	ClassDB::bind_method(D_METHOD("set_terrain_til_data", "til_bytes"), &Simulation::set_terrain_til_data);
	ClassDB::bind_method(D_METHOD("set_score_config_data", "score_ini_bytes"), &Simulation::set_score_config_data);
	ClassDB::bind_method(D_METHOD("is_listen_server"), &Simulation::is_listen_server);
	ClassDB::bind_method(D_METHOD("enable_host_listen", "port"), &Simulation::enable_host_listen);
	ClassDB::bind_method(D_METHOD("is_host_listening"), &Simulation::is_host_listening);
	ClassDB::bind_method(D_METHOD("get_host_listen_port"), &Simulation::get_host_listen_port);
	ClassDB::bind_method(D_METHOD("get_host_peer_count"), &Simulation::get_host_peer_count);
	ClassDB::bind_method(D_METHOD("configure_host_session", "options"), &Simulation::configure_host_session);
	ClassDB::bind_method(D_METHOD("get_host_session_config"), &Simulation::get_host_session_config);
	ClassDB::bind_method(D_METHOD("admit_test_remote_peer", "position", "yaw_deg", "team"), &Simulation::admit_test_remote_peer);
	ClassDB::bind_method(
			D_METHOD("enable_join", "host_ip", "port", "player_name",
					"join_role", "spectator_password"),
			&Simulation::enable_join, DEFVAL(0), DEFVAL(String()));
	ClassDB::bind_method(
			D_METHOD("is_local_spectator"), &Simulation::is_local_spectator);
	ClassDB::bind_method(
			D_METHOD("set_local_spectator", "spectator"),
			&Simulation::set_local_spectator);
	ClassDB::bind_method(D_METHOD("set_join_expansion_version_root", "game_root"),
			&Simulation::set_join_expansion_version_root);
	ClassDB::bind_method(D_METHOD("is_joiner"), &Simulation::is_joiner);
	ClassDB::bind_method(D_METHOD("get_streamed_placement_records"),
			&Simulation::get_streamed_placement_records);
	ClassDB::bind_method(D_METHOD("take_retired_placement_ids"),
			&Simulation::take_retired_placement_ids);
	ClassDB::bind_method(D_METHOD("set_join_character_profile", "profile"),
	                     &Simulation::set_join_character_profile);
	ClassDB::bind_method(D_METHOD("set_local_character_profile", "profile"),
	                     &Simulation::set_local_character_profile);
	ClassDB::bind_method(D_METHOD("set_join_integrity_profile", "profile_id"),
	                     &Simulation::set_join_integrity_profile);
	ClassDB::bind_method(D_METHOD("set_app_id", "token"),
	                     &Simulation::set_app_id);
	ClassDB::bind_method(D_METHOD("set_join_cd_cookie", "cookie"),
	                     &Simulation::set_join_cd_cookie);
	ClassDB::bind_method(D_METHOD("load_charattr_challenge", "resource_root"),
	                     &Simulation::load_charattr_challenge);
	ClassDB::bind_method(D_METHOD("set_join_world_ready", "ready"), &Simulation::set_join_world_ready);
	ClassDB::bind_method(D_METHOD("finalize_loaded_model_challenge_snapshot"),
	                     &Simulation::finalize_loaded_model_challenge_snapshot);
	ClassDB::bind_method(D_METHOD("poll_join_preload"), &Simulation::poll_join_preload);
	ClassDB::bind_method(D_METHOD("is_join_preload_ready"), &Simulation::is_join_preload_ready);
	ClassDB::bind_method(D_METHOD("get_join_admission_stage"), &Simulation::get_join_admission_stage);
	ClassDB::bind_method(D_METHOD("has_join_mission"), &Simulation::has_join_mission);
	ClassDB::bind_method(D_METHOD("get_join_server_name"), &Simulation::get_join_server_name);
	ClassDB::bind_method(D_METHOD("get_join_mission_name"), &Simulation::get_join_mission_name);
	ClassDB::bind_method(D_METHOD("get_join_mission_file"), &Simulation::get_join_mission_file);
	ClassDB::bind_method(D_METHOD("take_score_feedback"), &Simulation::take_score_feedback);
	ClassDB::bind_method(D_METHOD("get_join_mission_header"), &Simulation::get_join_mission_header);
	ClassDB::bind_method(D_METHOD("get_join_terrain_til_state"),
	                     &Simulation::get_join_terrain_til_state);
	ClassDB::bind_method(D_METHOD("get_join_terrain_til"), &Simulation::get_join_terrain_til);
	ClassDB::bind_method(D_METHOD("get_join_expansion"), &Simulation::get_join_expansion);
	ClassDB::bind_method(D_METHOD("get_join_game_type"), &Simulation::get_join_game_type);
	ClassDB::bind_method(D_METHOD("get_join_error"), &Simulation::get_join_error);
	ClassDB::bind_method(D_METHOD("get_session_loss_reason"),
	                     &Simulation::get_session_loss_reason);
	ClassDB::bind_method(D_METHOD("is_session_lost"), &Simulation::is_session_lost);
	ClassDB::bind_method(D_METHOD("is_joined_in_match"), &Simulation::is_joined_in_match);
	ClassDB::bind_method(D_METHOD("is_join_initial_admission_complete"),
	                     &Simulation::is_join_initial_admission_complete);
	ClassDB::bind_method(D_METHOD("is_joiner_network_diagnostics_enabled"),
	                     &Simulation::is_joiner_network_diagnostics_enabled);
	ClassDB::bind_method(D_METHOD("set_joiner_network_diagnostics_enabled", "enabled"),
	                     &Simulation::set_joiner_network_diagnostics_enabled);
	ClassDB::bind_method(D_METHOD("set_capture_pcap_path", "path"),
	                     &Simulation::set_capture_pcap_path);
	ClassDB::bind_method(D_METHOD("get_joiner_network_diagnostics"),
	                     &Simulation::get_joiner_network_diagnostics);
	ClassDB::bind_method(D_METHOD("is_join_deploy_pick_pending"),
	                     &Simulation::is_join_deploy_pick_pending);
	ClassDB::bind_method(D_METHOD("is_join_deploy_overlay_active"),
	                     &Simulation::is_join_deploy_overlay_active);
	ClassDB::bind_method(D_METHOD("take_join_deploy_overlay_open"),
	                     &Simulation::take_join_deploy_overlay_open);
	ClassDB::bind_method(D_METHOD("get_deploy_spawn_zones"),
	                     &Simulation::get_deploy_spawn_zones);
	ClassDB::bind_method(D_METHOD("send_deployment_pick", "param"),
	                     &Simulation::send_deployment_pick);
	ClassDB::bind_method(D_METHOD("get_deploy_list_rows", "default_key", "default_home", "zone_names"),
	                     &Simulation::get_deploy_list_rows);
	ClassDB::bind_method(D_METHOD("get_deploy_status"), &Simulation::get_deploy_status);
	ClassDB::bind_method(D_METHOD("get_deploy_status_text", "gametext"),
	                     &Simulation::get_deploy_status_text);
	ClassDB::bind_method(D_METHOD("request_local_player_medic"),
	                     &Simulation::request_local_player_medic);
	ClassDB::bind_method(D_METHOD("local_medic_request_cooldown_ticks"),
	                     &Simulation::local_medic_request_cooldown_ticks);
	ClassDB::bind_method(D_METHOD("local_medic_request_serial"),
	                     &Simulation::local_medic_request_serial);
	ClassDB::bind_method(D_METHOD("set_server_text", "medic_request_format"),
	                     &Simulation::set_server_text);
	ClassDB::bind_method(D_METHOD("is_local_player_dead"), &Simulation::local_player_dead);
	ClassDB::bind_method(D_METHOD("get_end_round_state"), &Simulation::get_end_round_state);
	ClassDB::bind_method(D_METHOD("is_mp_session"), &Simulation::is_mp_session);
	ClassDB::bind_method(D_METHOD("get_end_round_overlay", "gametext"),
	                     &Simulation::get_end_round_overlay);
	ClassDB::bind_method(D_METHOD("get_end_round_columns", "table_width", "gametext"),
	                     &Simulation::get_end_round_columns);
	ClassDB::bind_method(D_METHOD("get_end_round_rows", "tab"), &Simulation::get_end_round_rows,
	                     DEFVAL(0));
	ClassDB::bind_static_method("Simulation", D_METHOD("end_round_stat_screen_delay_msec"),
	                            &Simulation::end_round_stat_screen_delay_msec);
	ClassDB::bind_static_method("Simulation", D_METHOD("strip_inline_tags", "text"),
	                            &Simulation::strip_inline_tags);
	ClassDB::bind_method(D_METHOD("get_end_round_statistics"),
	                     &Simulation::get_end_round_statistics);
	ClassDB::bind_method(D_METHOD("get_join_assigned_team"),
	                     &Simulation::get_join_assigned_team);
	ClassDB::bind_method(D_METHOD("get_class_allow_mask"),
	                     &Simulation::get_class_allow_mask);
	ClassDB::bind_method(D_METHOD("get_joiner_phase"), &Simulation::get_joiner_phase);
	ClassDB::bind_method(D_METHOD("get_joiner_self_handle"), &Simulation::get_joiner_self_handle);
	ClassDB::bind_method(D_METHOD("spawn_local_player", "position", "yaw_deg", "team"), &Simulation::spawn_local_player);
	ClassDB::bind_method(D_METHOD("spawn_local_player_at_start"), &Simulation::spawn_local_player_at_start);
	ClassDB::bind_method(D_METHOD("has_local_player"), &Simulation::has_local_player);
	ClassDB::bind_method(D_METHOD("get_local_player_wire_handle"), &Simulation::get_local_player_wire_handle);
	ClassDB::bind_method(D_METHOD("set_player_input", "forward", "back", "left", "right", "lean_left", "lean_right", "jump"), &Simulation::set_player_input);
	ClassDB::bind_method(D_METHOD("add_local_player_look", "dx_px", "dy_px"), &Simulation::add_local_player_look);
	ClassDB::bind_method(D_METHOD("set_local_player_mouse", "sensitivity", "invert_y"), &Simulation::set_local_player_mouse);
	ClassDB::bind_method(D_METHOD("request_local_player_stance", "stance"), &Simulation::request_local_player_stance);
	ClassDB::bind_method(D_METHOD("get_local_player_position"), &Simulation::get_local_player_position);
	ClassDB::bind_method(D_METHOD("get_local_player_heading_bam"),
	                     &Simulation::get_local_player_heading_bam);
	ClassDB::bind_method(D_METHOD("request_hud_radar_zoom", "direction"),
	                     &Simulation::request_hud_radar_zoom);
	ClassDB::bind_method(D_METHOD("request_hud_map_cycle"),
	                     &Simulation::request_hud_map_cycle);
	ClassDB::bind_method(D_METHOD("get_hud_map_mode"),
	                     &Simulation::get_hud_map_mode);
	ClassDB::bind_method(D_METHOD("get_hud_big_zoom_q16"),
	                     &Simulation::get_hud_big_zoom_q16);
	ClassDB::bind_method(D_METHOD("get_hud_radar_zoom_q16"),
	                     &Simulation::get_hud_radar_zoom_q16);
	ClassDB::bind_method(D_METHOD("get_hud_map_flip_180"),
	                     &Simulation::get_hud_map_flip_180);
	ClassDB::bind_method(D_METHOD("get_waypoint_hud_view"), &Simulation::get_waypoint_hud_view);
	ClassDB::bind_method(D_METHOD("get_hud_minimap_snapshot"),
	                     &Simulation::get_hud_minimap_snapshot);
	ClassDB::bind_method(D_METHOD("get_hud_minimap_footprints"),
	                     &Simulation::get_hud_minimap_footprints);
	ClassDB::bind_method(D_METHOD("get_hud_map_grid_origin"),
	                     &Simulation::get_hud_map_grid_origin);
	ClassDB::bind_method(D_METHOD("get_objectives_view"), &Simulation::get_objectives_view);
	ClassDB::bind_method(D_METHOD("get_local_player_yaw_deg"), &Simulation::get_local_player_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_pitch_deg"), &Simulation::get_local_player_pitch_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_body_anim_slot"), &Simulation::get_local_player_body_anim_slot);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_key"), &Simulation::get_local_player_anim_key);
	ClassDB::bind_method(D_METHOD("get_local_player_stance_latch"),
			&Simulation::get_local_player_stance_latch);
	ClassDB::bind_method(D_METHOD("get_local_player_stance"), &Simulation::get_local_player_stance);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_phase_ticks"), &Simulation::get_local_player_anim_phase_ticks);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_source_key"), &Simulation::get_local_player_anim_source_key);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_source_phase_ticks"), &Simulation::get_local_player_anim_source_phase_ticks);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_blend_weight"), &Simulation::get_local_player_anim_blend_weight);
	ClassDB::bind_method(D_METHOD("get_local_player_aim_overlay"), &Simulation::get_local_player_aim_overlay);
	ClassDB::bind_method(
			D_METHOD("set_local_player_weapon", "def", "clip_seconds",
					"preserve_slot_state"),
			&Simulation::set_local_player_weapon, DEFVAL(false));
	ClassDB::bind_method(
			D_METHOD("rebake_local_player_weapon", "def", "clip_seconds",
					"preserve_slot_state"),
			&Simulation::rebake_local_player_weapon, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_local_player_weapon"), &Simulation::clear_local_player_weapon);
	ClassDB::bind_method(D_METHOD("set_local_player_first_person_model_available", "available"),
			&Simulation::set_local_player_first_person_model_available);
	ClassDB::bind_method(D_METHOD("set_local_player_weapon_input", "fire_held", "fire_pressed", "reload_pressed"), &Simulation::set_local_player_weapon_input);
	ClassDB::bind_method(D_METHOD("request_local_player_scope_toggle"), &Simulation::request_local_player_scope_toggle);
	ClassDB::bind_method(D_METHOD("request_local_player_binoculars_toggle"),
			&Simulation::request_local_player_binoculars_toggle);
	ClassDB::bind_method(D_METHOD("request_local_player_nvg_toggle"),
			&Simulation::request_local_player_nvg_toggle);
	ClassDB::bind_method(D_METHOD("request_local_player_nvg_gain", "delta"),
			&Simulation::request_local_player_nvg_gain);
	ClassDB::bind_method(D_METHOD("set_local_player_eye", "eye_godot", "valid"), &Simulation::set_local_player_eye);
	ClassDB::bind_method(D_METHOD("set_local_player_third_person_selected", "selected"), &Simulation::set_local_player_third_person_selected);
	ClassDB::bind_method(D_METHOD("set_local_player_debug_third_person", "enabled"), &Simulation::set_local_player_debug_third_person);
	ClassDB::bind_method(D_METHOD("get_local_player_view"), &Simulation::get_local_player_view);
	ClassDB::bind_static_method("Simulation", D_METHOD("fov_vertical_from_horizontal", "fov_h_deg", "aspect"), &Simulation::fov_vertical_from_horizontal);
	ClassDB::bind_method(D_METHOD("get_local_player_weapon_state"), &Simulation::get_local_player_weapon_state);
	ClassDB::bind_method(D_METHOD("drain_local_player_weapon_events"), &Simulation::drain_local_player_weapon_events);
	ClassDB::bind_method(D_METHOD("drain_round_impacts"), &Simulation::drain_round_impacts);
	ClassDB::bind_method(D_METHOD("drain_terrain_scorches"),
			&Simulation::drain_terrain_scorches);
	ClassDB::bind_method(D_METHOD("drain_feed_events"), &Simulation::drain_feed_events);
	ClassDB::bind_method(D_METHOD("drain_chat_lines"), &Simulation::drain_chat_lines);
	ClassDB::bind_method(D_METHOD("get_vehicle_panel_view"),
			&Simulation::get_vehicle_panel_view);
	ClassDB::bind_method(D_METHOD("get_session_game_type"),
			&Simulation::get_session_game_type);
	ClassDB::bind_method(D_METHOD("get_scoreboard"), &Simulation::get_scoreboard);
	ClassDB::bind_method(
			D_METHOD("format_feed_line", "template", "attacker", "victim",
					"extra", "bonus_template"),
			&Simulation::format_feed_line);
	ClassDB::bind_method(
			D_METHOD("format_feed_camp_line", "template", "wpname"),
			&Simulation::format_feed_camp_line);
	ClassDB::bind_method(D_METHOD("get_local_player_health"), &Simulation::get_local_player_health);
	ClassDB::bind_method(D_METHOD("get_local_player_health_percent"),
			&Simulation::get_local_player_health_percent);
	ClassDB::bind_method(D_METHOD("get_local_player_max_health"), &Simulation::get_local_player_max_health);
	ClassDB::bind_method(D_METHOD("get_local_player_team"), &Simulation::get_local_player_team);
	ClassDB::bind_method(D_METHOD("get_local_player_character_id"),
			&Simulation::get_local_player_character_id);
	ClassDB::bind_method(D_METHOD("get_local_player_class"), &Simulation::get_local_player_class);
	ClassDB::bind_method(D_METHOD("get_local_player_weapon_name"), &Simulation::get_local_player_weapon_name);
	ClassDB::bind_method(D_METHOD("drain_effects"), &Simulation::drain_effects);
	ClassDB::bind_method(D_METHOD("drain_fire_presentation_events"),
			&Simulation::drain_fire_presentation_events);
	ClassDB::bind_method(D_METHOD("drain_fire_sounds"),
			&Simulation::drain_fire_sounds);
	ClassDB::bind_method(D_METHOD("local_player_viewmodel_bias_view_units",
					"pos_raw_units", "tpos_raw_units", "viewport_w", "viewport_h"),
			&Simulation::local_player_viewmodel_bias_view_units);
	ClassDB::bind_method(D_METHOD("get_tracer_trails"), &Simulation::get_tracer_trails);
	ClassDB::bind_method(D_METHOD("get_round_glow_rows"),
			&Simulation::get_round_glow_rows);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("compile_tracer_ribbons", "rows", "camera"),
			&Simulation::compile_tracer_ribbons);
	ClassDB::bind_method(D_METHOD("drain_destruction_events"),
			&Simulation::drain_destruction_events);
	ClassDB::bind_method(D_METHOD("get_death_pieces"), &Simulation::get_death_pieces);
	ClassDB::bind_method(D_METHOD("get_destruction_debug", "bms_id"),
			&Simulation::get_destruction_debug);
	ClassDB::bind_method(D_METHOD("set_sound_profiles", "sndprof_text"),
			&Simulation::set_sound_profiles);
	ClassDB::bind_method(D_METHOD("set_water_z", "water_y"), &Simulation::set_water_z);
	ClassDB::bind_method(D_METHOD("drain_slot_sounds"), &Simulation::drain_slot_sounds);
	ClassDB::bind_method(D_METHOD("drain_sound_emitters"), &Simulation::drain_sound_emitters);
	ClassDB::bind_method(D_METHOD("set_wac_program", "program"), &Simulation::set_wac_program);
	ClassDB::bind_method(D_METHOD("compile_and_set_wac", "sources"), &Simulation::compile_and_set_wac);
	ClassDB::bind_method(D_METHOD("run_mission_start_wac"), &Simulation::run_mission_start_wac);
	ClassDB::bind_method(D_METHOD("seal_mission_start_baseline"),
			&Simulation::seal_mission_start_baseline);
	ClassDB::bind_method(D_METHOD("get_wac_state"), &Simulation::get_wac_state);
	ClassDB::bind_method(D_METHOD("get_runtime_perf_counters"), &Simulation::get_runtime_perf_counters);
	ClassDB::bind_method(D_METHOD("set_runtime_profiling_enabled", "enabled"),
			&Simulation::set_runtime_profiling_enabled);
	ClassDB::bind_method(D_METHOD("is_runtime_profiling_enabled"),
			&Simulation::is_runtime_profiling_enabled);
	ClassDB::bind_method(D_METHOD("get_last_projectile_trace_times_us"),
			&Simulation::get_last_projectile_trace_times_us);
	ClassDB::bind_method(D_METHOD("get_last_projectile_trace_counts"),
			&Simulation::get_last_projectile_trace_counts);
	ClassDB::bind_method(D_METHOD("get_last_projectile_trace_faces"),
			&Simulation::get_last_projectile_trace_faces);
	ClassDB::bind_method(D_METHOD("get_last_present_snapshot_us"),
			&Simulation::get_last_present_snapshot_us);
	ClassDB::bind_method(D_METHOD("get_last_occlusion_build_us"),
			&Simulation::get_last_occlusion_build_us);
	ClassDB::bind_method(D_METHOD("get_last_occlusion_probe_us"),
			&Simulation::get_last_occlusion_probe_us);
	ClassDB::bind_method(D_METHOD("set_wac_paused", "paused"), &Simulation::set_wac_paused);
	ClassDB::bind_method(D_METHOD("is_wac_paused"), &Simulation::is_wac_paused);
	ClassDB::bind_method(D_METHOD("set_mission_variable", "index", "value"), &Simulation::set_mission_variable);
	ClassDB::bind_method(D_METHOD("get_mission_variable", "index"), &Simulation::get_mission_variable);
	ClassDB::bind_method(D_METHOD("has_event_fired", "index"), &Simulation::has_event_fired);
	ClassDB::bind_method(D_METHOD("get_event_count"), &Simulation::get_event_count);
	ClassDB::bind_method(D_METHOD("get_logic_tick"), &Simulation::get_logic_tick);
	ClassDB::bind_method(D_METHOD("set_panm_time_ms", "time_ms"),
			&Simulation::set_panm_time_ms);
	ClassDB::bind_method(D_METHOD("get_panm_time_ms"),
			&Simulation::get_panm_time_ms);
	ClassDB::bind_method(D_METHOD("debug_set_panm_time_ms", "time_ms"),
			&Simulation::debug_set_panm_time_ms);
	ClassDB::bind_method(D_METHOD("debug_native_pose_stats"),
			&Simulation::debug_native_pose_stats);
	ClassDB::bind_method(D_METHOD("get_mission_variables_snapshot"), &Simulation::get_mission_variables_snapshot);
	ClassDB::bind_method(D_METHOD("get_global_variables_snapshot"), &Simulation::get_global_variables_snapshot);
	ClassDB::bind_method(D_METHOD("get_music_variables_snapshot"), &Simulation::get_music_variables_snapshot);
	ClassDB::bind_method(D_METHOD("set_global_variable", "index", "value"), &Simulation::set_global_variable);
	ClassDB::bind_method(D_METHOD("get_global_variable", "index"), &Simulation::get_global_variable);
	ClassDB::bind_method(D_METHOD("get_fired_events_snapshot"), &Simulation::get_fired_events_snapshot);
	ClassDB::bind_method(D_METHOD("entity_directory"), &Simulation::entity_directory);
	ClassDB::bind_method(D_METHOD("entity_card", "handle"), &Simulation::entity_card);
	ClassDB::bind_method(D_METHOD("entity_card_by_ai_index", "index"), &Simulation::entity_card_by_ai_index);
	ClassDB::bind_method(D_METHOD("entity_card_by_net_id", "net_id"), &Simulation::entity_card_by_net_id);
	ClassDB::bind_method(D_METHOD("debug_set_entity_health", "index", "hp"), &Simulation::debug_set_entity_health);
	ClassDB::bind_method(D_METHOD("debug_crew_vehicle", "occupant_ssn", "vehicle_ssn"),
	                     &Simulation::debug_crew_vehicle);
	ClassDB::bind_method(D_METHOD("set_local_player_eye_offset", "offset", "valid"),
	                     &Simulation::set_local_player_eye_offset);
	ClassDB::bind_method(D_METHOD("local_player_fp_weapon_hidden"),
	                     &Simulation::local_player_fp_weapon_hidden);
	ClassDB::bind_method(D_METHOD("debug_crew_local_player", "vehicle_ssn"),
	                     &Simulation::debug_crew_local_player);
	ClassDB::bind_method(D_METHOD("debug_kill_player_entity", "handle"), &Simulation::debug_kill_player_entity);
	ClassDB::bind_method(D_METHOD("debug_set_entity_position", "index", "mission_pos"), &Simulation::debug_set_entity_position);
	ClassDB::bind_method(D_METHOD("debug_set_world_entity_position", "net_id", "mission_pos"), &Simulation::debug_set_world_entity_position);
	ClassDB::bind_method(D_METHOD("debug_set_world_entity_weapon_ammo", "net_id", "clip", "reserve"),
	                     &Simulation::debug_set_world_entity_weapon_ammo);
	ClassDB::bind_method(D_METHOD("debug_set_entity_item_attrib", "handle", "attrib", "attrib2"),
	                     &Simulation::debug_set_entity_item_attrib);
	ClassDB::bind_method(D_METHOD("debug_kill_group", "group"), &Simulation::debug_kill_group);
	ClassDB::bind_method(D_METHOD("debug_teleport_local_player", "mission_pos", "yaw_deg", "pitch_deg"),
	                     &Simulation::debug_teleport_local_player);
	ClassDB::bind_method(D_METHOD("get_round_outcome_debug"), &Simulation::get_round_outcome_debug);
	ClassDB::bind_static_method("Simulation", D_METHOD("ai_state_name", "state"), &Simulation::ai_state_name);
	ClassDB::bind_static_method("Simulation", D_METHOD("infantry_anim_key", "state"), &Simulation::infantry_anim_key);
	ClassDB::bind_static_method("Simulation", D_METHOD("infantry_anim_flags", "state"), &Simulation::infantry_anim_flags);
	ClassDB::bind_static_method("Simulation", D_METHOD("remote_body_state_defers", "current_flags", "next_flags"), &Simulation::remote_body_state_defers);
	ClassDB::bind_method(D_METHOD("get_entity_count"), &Simulation::get_entity_count);
	ClassDB::bind_method(D_METHOD("get_entity_kind", "index"), &Simulation::get_entity_kind);
	ClassDB::bind_method(D_METHOD("get_entity_position", "index"), &Simulation::get_entity_position);
	ClassDB::bind_method(D_METHOD("get_entity_yaw_deg", "index"), &Simulation::get_entity_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_entity_state", "index"), &Simulation::get_entity_state);
	ClassDB::bind_method(D_METHOD("get_entity_net_id", "index"), &Simulation::get_entity_net_id);
	ClassDB::bind_method(D_METHOD("get_foliage_mask_anchor_positions"),
			&Simulation::get_foliage_mask_anchor_positions);
	ClassDB::bind_method(D_METHOD("get_entity_effect_state_for_ssn", "ssn"),
	                     &Simulation::get_entity_effect_state_for_ssn);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_ssn", "ssn"),
	                     &Simulation::get_present_effect_state_for_ssn);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_wire_handle", "wire_handle"),
	                     &Simulation::get_present_effect_state_for_wire_handle);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_bms_id", "bms_id"),
	                     &Simulation::get_present_effect_state_for_bms_id);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_origin", "kind", "index"),
	                     &Simulation::get_present_effect_state_for_origin);
	ClassDB::bind_method(D_METHOD("get_entity_owner_connection_id", "index"),
	                     &Simulation::get_entity_owner_connection_id);
	ClassDB::bind_method(D_METHOD("get_entity_wire_handle", "index"),
	                     &Simulation::get_entity_wire_handle);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("decode_present_part_anim_phase",
					"snapshot", "base", "channel"),
			&Simulation::decode_present_part_anim_phase);
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_phase", "index", "channel"), &Simulation::get_entity_part_anim_phase);
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_active", "index", "channel"), &Simulation::get_entity_part_anim_active);
	ClassDB::bind_method(D_METHOD("get_present_snapshot"), &Simulation::get_present_snapshot);
	ClassDB::bind_method(D_METHOD("get_present_layout_revision"),
			&Simulation::get_present_layout_revision);
	ClassDB::bind_method(D_METHOD("get_present_stride"), &Simulation::get_present_stride);
	ClassDB::bind_method(D_METHOD("set_terrain_height_field", "terrain"), &Simulation::set_terrain_height_field);
	ClassDB::bind_method(
			D_METHOD("install_seat_specs_for_type_ids", "item_db", "type_ids"),
			&Simulation::install_seat_specs_for_type_ids);
	ClassDB::bind_method(D_METHOD("set_infantry_anim_map", "resource_root", "adm_name"), &Simulation::set_infantry_anim_map);
	ClassDB::bind_method(D_METHOD("resolve_infantry_adm_ids", "resource_root", "item_db"), &Simulation::resolve_infantry_adm_ids);
	ClassDB::bind_method(D_METHOD("resolve_item_traits", "item_db"), &Simulation::resolve_item_traits);
	ClassDB::bind_method(D_METHOD("set_character_avatar_database", "avatar_db"),
	                     &Simulation::set_character_avatar_database);
	ClassDB::bind_method(D_METHOD("set_asset_root", "resource_root"),
	                     &Simulation::set_asset_root);
	ClassDB::bind_method(D_METHOD("install_local_player_weapon_by_name",
	                             "weapon_name", "preserve_slot_state"),
	                     &Simulation::install_local_player_weapon_by_name,
	                     DEFVAL(false));
	ClassDB::bind_method(D_METHOD("resolve_collision_instances", "item_db"),
	                     &Simulation::resolve_collision_instances);
	ClassDB::bind_method(D_METHOD("occlusion_init_mission"),
	                     &Simulation::occlusion_init_mission);
	ClassDB::bind_method(D_METHOD("run_occlusion_frame", "camera", "fov_y_deg", "aspect",
	                              "near", "fog_dist_units", "water_z_units", "force_indoors"),
	                     &Simulation::run_occlusion_frame);
	ClassDB::bind_method(D_METHOD("get_building_visibility"),
	                     &Simulation::get_building_visibility);
	ClassDB::bind_method(D_METHOD("get_render_culled_bms_ids"),
	                     &Simulation::get_render_culled_bms_ids);
	ClassDB::bind_method(D_METHOD("get_building_visibility_changes"),
	                     &Simulation::get_building_visibility_changes);
	ClassDB::bind_method(D_METHOD("get_render_culled_changes"),
	                     &Simulation::get_render_culled_changes);
	ClassDB::bind_method(D_METHOD("get_wire_render_culled_changes"),
	                     &Simulation::get_wire_render_culled_changes);
	ClassDB::bind_method(D_METHOD("get_draw_lighting_changes", "light_dir"),
	                     &Simulation::get_draw_lighting_changes);
	ClassDB::bind_method(D_METHOD("get_local_player_sun_quality"),
	                     &Simulation::get_local_player_sun_quality);
	ClassDB::bind_method(D_METHOD("sun_quality_factor", "quality"),
	                     &Simulation::sun_quality_factor);
	ClassDB::bind_method(D_METHOD("entity_present_visible", "bms_id"),
	                     &Simulation::entity_present_visible);
	ClassDB::bind_method(D_METHOD("reset_occlusion_apply_baseline"),
	                     &Simulation::reset_occlusion_apply_baseline);
	ClassDB::bind_method(D_METHOD("occlusion_water_visible"),
	                     &Simulation::occlusion_water_visible);
	ClassDB::bind_method(D_METHOD("get_collision_debug"), &Simulation::get_collision_debug);
	ClassDB::bind_method(D_METHOD("get_round_debug"), &Simulation::get_round_debug);
	ClassDB::bind_method(D_METHOD("get_ai_debug"), &Simulation::get_ai_debug);
	ClassDB::bind_method(D_METHOD("get_ray_debug"), &Simulation::get_ray_debug);
	ClassDB::bind_method(D_METHOD("set_ray_debug_recording", "enabled"),
	                     &Simulation::set_ray_debug_recording);
	ClassDB::bind_method(D_METHOD("is_ray_debug_recording"),
	                     &Simulation::is_ray_debug_recording);
	ClassDB::bind_method(D_METHOD("set_ray_debug_filter", "mask", "ttl_ticks"),
	                     &Simulation::set_ray_debug_filter);
	ClassDB::bind_method(D_METHOD("clear_ray_debug"), &Simulation::clear_ray_debug);
	ClassDB::bind_method(D_METHOD("set_contact_debug_capture", "enabled"),
	                     &Simulation::set_contact_debug_capture);
	ClassDB::bind_method(D_METHOD("is_contact_debug_capture"),
	                     &Simulation::is_contact_debug_capture);
	ClassDB::bind_method(D_METHOD("set_contact_debug_kind_mask", "mask"),
	                     &Simulation::set_contact_debug_kind_mask);
	ClassDB::bind_method(D_METHOD("clear_contact_debug"), &Simulation::clear_contact_debug);
	ClassDB::bind_method(D_METHOD("get_throwable_visuals"), &Simulation::get_throwable_visuals);
	ClassDB::bind_method(D_METHOD("get_scar_draw_list", "camera_godot", "fog_distance", "terrain_light"),
	                     &Simulation::get_scar_draw_list);
	ClassDB::bind_method(D_METHOD("debug_spawn_round", "from_godot", "dir_godot", "ammo_name"),
	                     &Simulation::debug_spawn_round);
	ClassDB::bind_method(D_METHOD("debug_pick_entity", "from_godot", "dir_godot", "max_range_units"),
	                     &Simulation::debug_pick_entity);
	ClassDB::bind_method(D_METHOD("get_hitbox_debug"), &Simulation::get_hitbox_debug);
	ClassDB::bind_method(D_METHOD("get_occlusion_portal_debug", "anchor", "range_units"),
	                     &Simulation::get_occlusion_portal_debug);
	ClassDB::bind_method(D_METHOD("local_player_indoors"), &Simulation::local_player_indoors);
	ClassDB::bind_method(D_METHOD("local_player_blink_flags"), &Simulation::local_player_blink_flags);
	ClassDB::bind_method(D_METHOD("local_player_interior_item_id"),
	                     &Simulation::local_player_interior_item_id);
	ClassDB::bind_method(D_METHOD("query_blink_owner_at", "world_pos"),
	                     &Simulation::query_blink_owner_at);
	ClassDB::bind_method(D_METHOD("get_entity_interior_groups"),
	                     &Simulation::get_entity_interior_groups);
	ClassDB::bind_method(D_METHOD("local_player_interior_group"),
	                     &Simulation::local_player_interior_group);
	BIND_CONSTANT(BLINK_INDOORS);
	BIND_CONSTANT(BLINK_WATER_OFF);
	ClassDB::bind_method(D_METHOD("compute_iris_samples", "cam_pos", "cam_forward", "light_dir"),
	                     &Simulation::compute_iris_samples);
	ClassDB::bind_method(D_METHOD("sound_occlusion_distance_q16", "listener_pos", "source_pos",
	                              "distance_q16", "source_bms_id"),
	                     &Simulation::sound_occlusion_distance_q16, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("local_player_in_armory_zone"), &Simulation::local_player_in_armory_zone);
	ClassDB::bind_method(D_METHOD("local_player_toggle_mount"), &Simulation::local_player_toggle_mount);
	ClassDB::bind_method(D_METHOD("get_attach_labels"), &Simulation::get_attach_labels);
	ClassDB::bind_method(D_METHOD("get_friendly_tags"), &Simulation::get_friendly_tags);
	ClassDB::bind_method(D_METHOD("apply_local_player_loadout", "kit", "player_class"),
	                     &Simulation::apply_local_player_loadout);
	ClassDB::bind_method(D_METHOD("set_spawn_loadout", "kit", "filter_by_availability"),
	                     &Simulation::set_spawn_loadout);
	ClassDB::bind_method(D_METHOD("has_explicit_spawn_loadout"),
	                     &Simulation::has_explicit_spawn_loadout);
	ClassDB::bind_method(D_METHOD("set_weapon_availability", "pairs"),
	                     &Simulation::set_weapon_availability);
	ClassDB::bind_method(D_METHOD("get_weapon_availability", "weapon_name"),
	                     &Simulation::get_weapon_availability);
	ClassDB::bind_method(D_METHOD("respawn_local_player_loadout"),
	                     &Simulation::respawn_local_player_loadout);
	ClassDB::bind_method(D_METHOD("set_local_player_class", "player_class"),
	                     &Simulation::set_local_player_class);
	ClassDB::bind_method(D_METHOD("load_weapon_profile", "path"),
	                     &Simulation::load_weapon_profile);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("read_weapon_profile_summary", "path"),
			&Simulation::read_weapon_profile_summary);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("save_weapon_profile_selection", "path", "profile"),
			&Simulation::save_weapon_profile_selection);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("weapon_profile_relpath", "expansion_name"),
			&Simulation::weapon_profile_relpath);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("fp_viewmodel_spec", "has_def", "gfx1", "character_arms",
					"animadm", "flags"),
			&Simulation::fp_viewmodel_spec);
	ClassDB::bind_static_method("Simulation", D_METHOD("weapon_def_pos_scale"),
			&Simulation::weapon_def_pos_scale);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("viewmodel_fallback_pos_units"),
			&Simulation::viewmodel_fallback_pos_units);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("viewmodel_fallback_tpos_units"),
			&Simulation::viewmodel_fallback_tpos_units);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("viewmodel_fallback_rot_bias_deg"),
			&Simulation::viewmodel_fallback_rot_bias_deg);
	ClassDB::bind_static_method("Simulation", D_METHOD("viewmodel_pass_near_z"),
			&Simulation::viewmodel_pass_near_z);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("viewmodel_bringup_fallback_weapon"),
			&Simulation::viewmodel_bringup_fallback_weapon);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("player_eye_min_above_position"),
			&Simulation::player_eye_min_above_position);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("player_non_person_eye_bump"),
			&Simulation::player_non_person_eye_bump);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("player_head_bone_index"),
			&Simulation::player_head_bone_index);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("player_aim_project_range"),
			&Simulation::player_aim_project_range);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("hit_zone_damage_multiplier", "section"),
			&Simulation::hit_zone_damage_multiplier);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("seat_hit_bone_damage_multiplier", "bone"),
			&Simulation::seat_hit_bone_damage_multiplier);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("portal_slot_collect_radius"),
			&Simulation::portal_slot_collect_radius);
	ClassDB::bind_static_method("Simulation", D_METHOD("mission_coord_min"),
			&Simulation::mission_coord_min);
	ClassDB::bind_static_method("Simulation", D_METHOD("mission_coord_max"),
			&Simulation::mission_coord_max);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("epilog_exit_timeout_seconds"),
			&Simulation::epilog_exit_timeout_seconds);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("epilog_fade_in_seconds"),
			&Simulation::epilog_fade_in_seconds);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("deploy_refresh_interval_seconds"),
			&Simulation::deploy_refresh_interval_seconds);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("spawn_origin_pack", "kind", "index"),
			&Simulation::spawn_origin_pack);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("spawn_origin_kind", "origin"),
			&Simulation::spawn_origin_kind);
	ClassDB::bind_static_method("Simulation",
			D_METHOD("spawn_origin_index", "origin"),
			&Simulation::spawn_origin_index);
	ClassDB::bind_method(D_METHOD("get_weapon_profile_summary"),
	                     &Simulation::get_weapon_profile_summary);
	ClassDB::bind_method(D_METHOD("request_local_player_weapon_category", "category"),
	                     &Simulation::request_local_player_weapon_category);
	ClassDB::bind_method(D_METHOD("request_local_player_weapon_cycle", "direction"),
	                     &Simulation::request_local_player_weapon_cycle);
	ClassDB::bind_method(D_METHOD("get_local_player_inventory"),
	                     &Simulation::get_local_player_inventory);
	ClassDB::bind_method(D_METHOD("get_local_player_loadout"),
	                     &Simulation::get_local_player_loadout);
	ClassDB::bind_method(D_METHOD("load_weapon_table", "resource_root", "name"),
	                     &Simulation::load_weapon_table, DEFVAL(String("weapon.def")));
	ClassDB::bind_method(D_METHOD("load_ammo_table", "resource_root", "name"),
	                     &Simulation::load_ammo_table, DEFVAL(String("ammo.def")));
	ClassDB::bind_method(D_METHOD("get_infantry_clip_count"), &Simulation::get_infantry_clip_count);
	ClassDB::bind_method(D_METHOD("set_loco_scale", "scale"), &Simulation::set_loco_scale);
	ClassDB::bind_method(D_METHOD("get_loco_scale"), &Simulation::get_loco_scale);
	ClassDB::bind_method(D_METHOD("get_spawned_count"), &Simulation::get_spawned_count);
	ClassDB::bind_method(D_METHOD("get_brain_count"), &Simulation::get_brain_count);

	// Present-snapshot field layout (single source of truth for the GDScript present pass).
	BIND_ENUM_CONSTANT(PF_KIND);
	BIND_ENUM_CONSTANT(PF_INDEX);
	BIND_ENUM_CONSTANT(PF_BMS_ID);
	BIND_ENUM_CONSTANT(PF_NET_ID);
	BIND_ENUM_CONSTANT(PF_POS_X);
	BIND_ENUM_CONSTANT(PF_POS_Y);
	BIND_ENUM_CONSTANT(PF_POS_Z);
	BIND_ENUM_CONSTANT(PF_PITCH_DEG);
	BIND_ENUM_CONSTANT(PF_YAW_DEG);
	BIND_ENUM_CONSTANT(PF_ROLL_DEG);
	BIND_ENUM_CONSTANT(PF_PHASE1);
	BIND_ENUM_CONSTANT(PF_ACTIVE1);
	BIND_ENUM_CONSTANT(PF_PHASE2);
	BIND_ENUM_CONSTANT(PF_ACTIVE2);
	BIND_ENUM_CONSTANT(PF_BODY_ANIM_SLOT);
	BIND_ENUM_CONSTANT(PF_ANIM_STATE);
	BIND_ENUM_CONSTANT(PF_ANIM_PHASE_TICKS);
	BIND_ENUM_CONSTANT(PF_ANIM_SOURCE_STATE);
	BIND_ENUM_CONSTANT(PF_ANIM_SOURCE_PHASE_TICKS);
	BIND_ENUM_CONSTANT(PF_ANIM_BLEND_WEIGHT);
	BIND_ENUM_CONSTANT(PF_ANIM_REMOTE_REQUEST);
	BIND_ENUM_CONSTANT(PF_ANIM_STATE_PULSE);
	BIND_ENUM_CONSTANT(PF_ANIM_PULSE_TICKS);
	BIND_ENUM_CONSTANT(PF_HELD_WEAPON_ADM);
	BIND_ENUM_CONSTANT(PF_HELD_WEAPON_PITCH_DEG);
	BIND_ENUM_CONSTANT(PF_HELD_WEAPON_YAW_DEG);
	BIND_ENUM_CONSTANT(PF_HELD_WEAPON_ROLL_DEG);
	BIND_ENUM_CONSTANT(PF_HELD_WEAPON_HAND_FRAME);
	BIND_ENUM_CONSTANT(PF_WPN_ANIM_STATE);
	BIND_ENUM_CONSTANT(PF_WPN_PHASE_TICKS);
	BIND_ENUM_CONSTANT(PF_WPN_SOURCE_STATE);
	BIND_ENUM_CONSTANT(PF_WPN_SOURCE_PHASE_TICKS);
	BIND_ENUM_CONSTANT(PF_WPN_BLEND_WEIGHT);
	BIND_ENUM_CONSTANT(PF_WPN_VARIANT);
	BIND_ENUM_CONSTANT(PF_WPN_SOURCE_VARIANT);
	BIND_ENUM_CONSTANT(PF_HIDDEN);
	BIND_ENUM_CONSTANT(PF_LOCAL_VIEW_SUPPRESSED);
	BIND_ENUM_CONSTANT(PF_ALIVE);
	BIND_ENUM_CONSTANT(PF_RESPAWN_REVISION);
	BIND_ENUM_CONSTANT(PF_TYPE_ID);
	BIND_ENUM_CONSTANT(PF_WIRE_HANDLE);
	BIND_ENUM_CONSTANT(PF_CHARACTER_ID);
	BIND_ENUM_CONSTANT(PF_CARRIER_HANDLE);
	BIND_ENUM_CONSTANT(PF_AIM_OVERLAY_VALID);
	BIND_ENUM_CONSTANT(PF_AIM_BODY_PITCH_DEG);
	BIND_ENUM_CONSTANT(PF_AIM_BODY_YAW_DEG);
	BIND_ENUM_CONSTANT(PF_AIM_BODY_ROLL_DEG);
	BIND_ENUM_CONSTANT(PF_AIM_ANGLES);
	BIND_ENUM_CONSTANT(PF_AIM_CLASS_STRIDE);
	BIND_ENUM_CONSTANT(PF_EMPLACED_CONTROLS_VALID);
	BIND_ENUM_CONSTANT(PF_EWEAP_GUNYAW);
	BIND_ENUM_CONSTANT(PF_EWEAP_GUNPITCH);
	BIND_ENUM_CONSTANT(PF_VEHICLE_MOTION_VALID);
	BIND_ENUM_CONSTANT(PF_VEHICLE_STEERING);
	BIND_ENUM_CONSTANT(PF_VEHICLE_SPEED);
	BIND_ENUM_CONSTANT(PF_VEHICLE_ROTOR);
	BIND_ENUM_CONSTANT(PF_VEHICLE_TAIL_ROTOR);
	BIND_ENUM_CONSTANT(PF_VEHICLE_WHEELS);
	BIND_ENUM_CONSTANT(PF_TEX_TEAM_VALID);
	BIND_ENUM_CONSTANT(PF_TEX_TEAM);
	BIND_ENUM_CONSTANT(PF_ZONE_CTRL_VALID);
	BIND_ENUM_CONSTANT(PF_TEAMSWING);
	BIND_ENUM_CONSTANT(PF_LFP_CAMPPERCENT_VALID);
	BIND_ENUM_CONSTANT(PF_LFP_CAMPPERCENT);
	BIND_ENUM_CONSTANT(PF_WORLD_HEAT_GLOW_VALID);
	BIND_ENUM_CONSTANT(PF_WORLD_HEAT_GLOW);
	BIND_ENUM_CONSTANT(PF_RIGHT_HAND_COLLAPSED);
	BIND_ENUM_CONSTANT(PF_SECTION_MASK_VALID);
	BIND_ENUM_CONSTANT(PF_SECTION_MASK_LO);
	BIND_ENUM_CONSTANT(PF_SECTION_MASK_HI);
	BIND_ENUM_CONSTANT(PF_STANCE_BITS);
	BIND_ENUM_CONSTANT(PF_STRIDE);
	BIND_ENUM_CONSTANT(EFFECT_STATE_POSITION);
	BIND_ENUM_CONSTANT(EFFECT_STATE_ROTATION_DEG);
	BIND_ENUM_CONSTANT(EFFECT_STATE_COUNT);

	BIND_ENUM_CONSTANT(SEAT_NONE);
	BIND_ENUM_CONSTANT(SEAT_PASSENGER);
	BIND_ENUM_CONSTANT(SEAT_CONTROLLER);
	BIND_ENUM_CONSTANT(SEAT_GUNNER);
	BIND_ENUM_CONSTANT(SEAT_ARMORY_POINT);
	BIND_ENUM_CONSTANT(SEAT_DRIVER);

	BIND_ENUM_CONSTANT(JOIN_TERRAIN_TIL_ABSENT);
	BIND_ENUM_CONSTANT(JOIN_TERRAIN_TIL_RECEIVING);
	BIND_ENUM_CONSTANT(JOIN_TERRAIN_TIL_COMPLETE);
	BIND_ENUM_CONSTANT(JOIN_TERRAIN_TIL_INVALID);

	BIND_CONSTANT(WEAPON_ACTION_FIRE);

	BIND_CONSTANT(STANCE_STAND);
	BIND_CONSTANT(STANCE_CROUCH);
	BIND_CONSTANT(STANCE_PRONE);

	BIND_CONSTANT(FACE_FLAG_BOTH_SIDES);
	BIND_CONSTANT(FACE_FLAG_NEVER_HIT);
	BIND_CONSTANT(FACE_FLAG_DOUBLE_SIDED);

	BIND_CONSTANT(BVOL_LADDER_CL);
	BIND_CONSTANT(BVOL_ARMORY_CA);
	BIND_CONSTANT(BVOL_BLINK_BB);
	BIND_CONSTANT(BVOL_DOOR_CD);
	BIND_CONSTANT(BVOL_CHANGE_TEAM_CT);
	BIND_CONSTANT(BVOL_FLAG_CF);
	BIND_CONSTANT(BVOL_DAMAGE_HIGH_DH);
	BIND_CONSTANT(BVOL_DAMAGE_MEDIUM_DM);
	BIND_CONSTANT(BVOL_DAMAGE_LOW_DL);

	BIND_CONSTANT(OCC_REC_OCCLUDER);
	BIND_CONSTANT(OCC_REC_OPEN);
	BIND_CONSTANT(OCC_REC_WINDOW);
	BIND_CONSTANT(OCC_REC_PORTAL);
	BIND_CONSTANT(OCC_REC_WELDED_LINK);

	BIND_CONSTANT(INVALID_WIRE_HANDLE);
	BIND_CONSTANT(ITEM_USER_POINT_SCAN_LIMIT);
	BIND_CONSTANT(ENTITY_HEALTH_MIN);
	BIND_CONSTANT(ENTITY_HEALTH_MAX);
	BIND_CONSTANT(MISSION_VAR_COUNT);
	BIND_CONSTANT(DEFAULT_PLAYER_FOV_H_DEG);
	BIND_CONSTANT(HUD_MINIMAP_SNAPSHOT_VERSION);
	BIND_CONSTANT(HUD_MINIMAP_HEADER_SIZE);
	BIND_CONSTANT(HUD_MINIMAP_STRIDE);

	BIND_CONSTANT(SPAWN_ORIGIN_NONE);

	BIND_ENUM_CONSTANT(ROLE_SINGLE_PLAYER);
	BIND_ENUM_CONSTANT(ROLE_LISTEN_HOST);
	BIND_ENUM_CONSTANT(ROLE_JOINER);
	BIND_ENUM_CONSTANT(ROLE_DEDICATED_HOST);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "loco_scale"), "set_loco_scale", "get_loco_scale");
}
