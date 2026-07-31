// NovaSimulation — ClassDB registration (_bind_methods) and _notification.
#include "simulation/nova_simulation_internal.h"

using namespace novasim;

// The GDScript-facing seat codes are the SAME values libs/world computes with —
// a drifted copy here would silently corrupt every binding-side seat-spec walk.
static_assert(NovaSimulation::SEAT_NONE == static_cast<int>(opennova::world::SeatType::None));
static_assert(NovaSimulation::SEAT_PASSENGER == static_cast<int>(opennova::world::SeatType::Passenger));
static_assert(NovaSimulation::SEAT_CONTROLLER == static_cast<int>(opennova::world::SeatType::Controller));
static_assert(NovaSimulation::SEAT_GUNNER == static_cast<int>(opennova::world::SeatType::Gunner));
static_assert(NovaSimulation::SEAT_ARMORY_POINT == static_cast<int>(opennova::world::SeatType::ArmoryPoint));
static_assert(NovaSimulation::SEAT_DRIVER == static_cast<int>(opennova::world::SeatType::Driver));

void NovaSimulation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_mission_data", "mission"), &NovaSimulation::load_from_mission_data);
	ClassDB::bind_method(D_METHOD("load_mission_file", "path"), &NovaSimulation::load_mission_file);
	ClassDB::bind_method(D_METHOD("build_demo_mission"), &NovaSimulation::build_demo_mission);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaSimulation::is_loaded);
	ClassDB::bind_method(D_METHOD("set_playing", "playing"), &NovaSimulation::set_playing);
	ClassDB::bind_method(D_METHOD("is_playing"), &NovaSimulation::is_playing);
	ClassDB::bind_method(D_METHOD("step"), &NovaSimulation::step);
	ClassDB::bind_method(D_METHOD("restart"), &NovaSimulation::restart);
	ClassDB::bind_method(D_METHOD("enable_listen_server", "enable"), &NovaSimulation::enable_listen_server);
	ClassDB::bind_method(D_METHOD("set_terrain_til_data", "til_bytes"), &NovaSimulation::set_terrain_til_data);
	ClassDB::bind_method(D_METHOD("is_listen_server"), &NovaSimulation::is_listen_server);
	ClassDB::bind_method(D_METHOD("enable_host_listen", "port"), &NovaSimulation::enable_host_listen);
	ClassDB::bind_method(D_METHOD("is_host_listening"), &NovaSimulation::is_host_listening);
	ClassDB::bind_method(D_METHOD("get_host_listen_port"), &NovaSimulation::get_host_listen_port);
	ClassDB::bind_method(D_METHOD("get_host_peer_count"), &NovaSimulation::get_host_peer_count);
	ClassDB::bind_method(D_METHOD("configure_host_session", "options"), &NovaSimulation::configure_host_session);
	ClassDB::bind_method(D_METHOD("get_host_session_config"), &NovaSimulation::get_host_session_config);
	ClassDB::bind_method(D_METHOD("admit_test_remote_peer", "position", "yaw_deg", "team"), &NovaSimulation::admit_test_remote_peer);
	ClassDB::bind_method(D_METHOD("enable_join", "host_ip", "port", "player_name"), &NovaSimulation::enable_join);
	ClassDB::bind_method(D_METHOD("is_joiner"), &NovaSimulation::is_joiner);
	ClassDB::bind_method(D_METHOD("set_join_character_profile", "profile"),
	                     &NovaSimulation::set_join_character_profile);
	ClassDB::bind_method(D_METHOD("load_charattr_challenge", "resource_root"),
	                     &NovaSimulation::load_charattr_challenge);
	ClassDB::bind_method(D_METHOD("leave_net_session"), &NovaSimulation::leave_net_session);
	ClassDB::bind_method(D_METHOD("set_join_world_ready", "ready"), &NovaSimulation::set_join_world_ready);
	ClassDB::bind_method(D_METHOD("finalize_loaded_model_challenge_snapshot"),
	                     &NovaSimulation::finalize_loaded_model_challenge_snapshot);
	ClassDB::bind_method(D_METHOD("poll_join_preload"), &NovaSimulation::poll_join_preload);
	ClassDB::bind_method(D_METHOD("is_join_preload_ready"), &NovaSimulation::is_join_preload_ready);
	ClassDB::bind_method(D_METHOD("get_join_admission_stage"), &NovaSimulation::get_join_admission_stage);
	ClassDB::bind_method(D_METHOD("has_join_mission"), &NovaSimulation::has_join_mission);
	ClassDB::bind_method(D_METHOD("get_join_server_name"), &NovaSimulation::get_join_server_name);
	ClassDB::bind_method(D_METHOD("get_join_mission_name"), &NovaSimulation::get_join_mission_name);
	ClassDB::bind_method(D_METHOD("get_join_mission_file"), &NovaSimulation::get_join_mission_file);
	ClassDB::bind_method(D_METHOD("get_join_expansion"), &NovaSimulation::get_join_expansion);
	ClassDB::bind_method(D_METHOD("get_join_game_type"), &NovaSimulation::get_join_game_type);
	ClassDB::bind_method(D_METHOD("get_join_error"), &NovaSimulation::get_join_error);
	ClassDB::bind_method(D_METHOD("get_session_loss_reason"),
	                     &NovaSimulation::get_session_loss_reason);
	ClassDB::bind_method(D_METHOD("is_session_lost"), &NovaSimulation::is_session_lost);
	ClassDB::bind_method(D_METHOD("is_joined_in_match"), &NovaSimulation::is_joined_in_match);
	ClassDB::bind_method(D_METHOD("is_joiner_network_diagnostics_enabled"),
	                     &NovaSimulation::is_joiner_network_diagnostics_enabled);
	ClassDB::bind_method(D_METHOD("get_joiner_network_diagnostics"),
	                     &NovaSimulation::get_joiner_network_diagnostics);
	ClassDB::bind_method(D_METHOD("is_join_deploy_pick_pending"),
	                     &NovaSimulation::is_join_deploy_pick_pending);
	ClassDB::bind_method(D_METHOD("get_deploy_spawn_zones"),
	                     &NovaSimulation::get_deploy_spawn_zones);
	ClassDB::bind_method(D_METHOD("send_deployment_pick", "param"),
	                     &NovaSimulation::send_deployment_pick);
	ClassDB::bind_method(D_METHOD("get_join_assigned_team"),
	                     &NovaSimulation::get_join_assigned_team);
	ClassDB::bind_method(D_METHOD("get_joiner_phase"), &NovaSimulation::get_joiner_phase);
	ClassDB::bind_method(D_METHOD("get_joiner_self_handle"), &NovaSimulation::get_joiner_self_handle);
	ClassDB::bind_method(D_METHOD("spawn_local_player", "position", "yaw_deg", "team"), &NovaSimulation::spawn_local_player);
	ClassDB::bind_method(D_METHOD("spawn_local_player_at_start"), &NovaSimulation::spawn_local_player_at_start);
	ClassDB::bind_method(D_METHOD("has_local_player"), &NovaSimulation::has_local_player);
	ClassDB::bind_method(D_METHOD("get_local_player_wire_handle"), &NovaSimulation::get_local_player_wire_handle);
	ClassDB::bind_method(D_METHOD("set_player_input", "forward", "back", "left", "right", "lean_left", "lean_right", "jump"), &NovaSimulation::set_player_input);
	ClassDB::bind_method(D_METHOD("add_local_player_look", "dx_px", "dy_px"), &NovaSimulation::add_local_player_look);
	ClassDB::bind_method(D_METHOD("set_local_player_mouse", "sensitivity", "invert_y"), &NovaSimulation::set_local_player_mouse);
	ClassDB::bind_method(D_METHOD("request_local_player_stance", "stance"), &NovaSimulation::request_local_player_stance);
	ClassDB::bind_method(D_METHOD("get_local_player_position"), &NovaSimulation::get_local_player_position);
	ClassDB::bind_method(D_METHOD("get_waypoint_hud_view"), &NovaSimulation::get_waypoint_hud_view);
	ClassDB::bind_method(D_METHOD("get_objectives_view"), &NovaSimulation::get_objectives_view);
	ClassDB::bind_method(D_METHOD("get_local_player_yaw_deg"), &NovaSimulation::get_local_player_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_pitch_deg"), &NovaSimulation::get_local_player_pitch_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_body_anim_slot"), &NovaSimulation::get_local_player_body_anim_slot);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_key"), &NovaSimulation::get_local_player_anim_key);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_phase_ticks"), &NovaSimulation::get_local_player_anim_phase_ticks);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_source_key"), &NovaSimulation::get_local_player_anim_source_key);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_source_phase_ticks"), &NovaSimulation::get_local_player_anim_source_phase_ticks);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_blend_weight"), &NovaSimulation::get_local_player_anim_blend_weight);
	ClassDB::bind_method(D_METHOD("get_local_player_aim_overlay"), &NovaSimulation::get_local_player_aim_overlay);
	ClassDB::bind_method(
			D_METHOD("set_local_player_weapon", "def", "clip_seconds",
					"preserve_slot_state"),
			&NovaSimulation::set_local_player_weapon, DEFVAL(false));
	ClassDB::bind_method(
			D_METHOD("rebake_local_player_weapon", "def", "clip_seconds",
					"preserve_slot_state"),
			&NovaSimulation::rebake_local_player_weapon, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("clear_local_player_weapon"), &NovaSimulation::clear_local_player_weapon);
	ClassDB::bind_method(D_METHOD("set_local_player_first_person_model_available", "available"),
			&NovaSimulation::set_local_player_first_person_model_available);
	ClassDB::bind_method(D_METHOD("set_local_player_weapon_input", "fire_held", "fire_pressed", "reload_pressed"), &NovaSimulation::set_local_player_weapon_input);
	ClassDB::bind_method(D_METHOD("request_local_player_scope_toggle"), &NovaSimulation::request_local_player_scope_toggle);
	ClassDB::bind_method(D_METHOD("request_local_player_binoculars_toggle"),
			&NovaSimulation::request_local_player_binoculars_toggle);
	ClassDB::bind_method(D_METHOD("request_local_player_nvg_toggle"),
			&NovaSimulation::request_local_player_nvg_toggle);
	ClassDB::bind_method(D_METHOD("request_local_player_nvg_gain", "delta"),
			&NovaSimulation::request_local_player_nvg_gain);
	ClassDB::bind_method(D_METHOD("set_local_player_eye", "eye_godot", "valid"), &NovaSimulation::set_local_player_eye);
	ClassDB::bind_method(D_METHOD("set_local_player_camera_third_person", "third_person"), &NovaSimulation::set_local_player_camera_third_person);
	ClassDB::bind_method(D_METHOD("get_local_player_view"), &NovaSimulation::get_local_player_view);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("fov_vertical_from_horizontal", "fov_h_deg", "aspect"), &NovaSimulation::fov_vertical_from_horizontal);
	ClassDB::bind_method(D_METHOD("get_local_player_weapon_state"), &NovaSimulation::get_local_player_weapon_state);
	ClassDB::bind_method(D_METHOD("drain_local_player_weapon_events"), &NovaSimulation::drain_local_player_weapon_events);
	ClassDB::bind_method(D_METHOD("drain_round_impacts"), &NovaSimulation::drain_round_impacts);
	ClassDB::bind_method(D_METHOD("get_local_player_health"), &NovaSimulation::get_local_player_health);
	ClassDB::bind_method(D_METHOD("get_local_player_max_health"), &NovaSimulation::get_local_player_max_health);
	ClassDB::bind_method(D_METHOD("get_local_player_team"), &NovaSimulation::get_local_player_team);
	ClassDB::bind_method(D_METHOD("get_local_player_class"), &NovaSimulation::get_local_player_class);
	ClassDB::bind_method(D_METHOD("get_local_player_weapon_name"), &NovaSimulation::get_local_player_weapon_name);
	ClassDB::bind_method(D_METHOD("get_weapon_third_person_model", "adm_index"),
	                     &NovaSimulation::get_weapon_third_person_model);
	ClassDB::bind_method(D_METHOD("drain_effects"), &NovaSimulation::drain_effects);
	ClassDB::bind_method(D_METHOD("drain_fire_presentation_events"),
			&NovaSimulation::drain_fire_presentation_events);
	ClassDB::bind_method(D_METHOD("get_tracer_trails"), &NovaSimulation::get_tracer_trails);
	ClassDB::bind_method(D_METHOD("drain_destruction_events"),
			&NovaSimulation::drain_destruction_events);
	ClassDB::bind_method(D_METHOD("get_death_pieces"), &NovaSimulation::get_death_pieces);
	ClassDB::bind_method(D_METHOD("get_destruction_debug", "bms_id"),
			&NovaSimulation::get_destruction_debug);
	ClassDB::bind_method(D_METHOD("set_sound_profiles", "sndprof_text"),
			&NovaSimulation::set_sound_profiles);
	ClassDB::bind_method(D_METHOD("set_water_z", "water_y"), &NovaSimulation::set_water_z);
	ClassDB::bind_method(D_METHOD("drain_slot_sounds"), &NovaSimulation::drain_slot_sounds);
	ClassDB::bind_method(D_METHOD("drain_sound_emitters"), &NovaSimulation::drain_sound_emitters);
	ClassDB::bind_method(D_METHOD("set_wac_program", "program"), &NovaSimulation::set_wac_program);
	ClassDB::bind_method(D_METHOD("get_wac_program"), &NovaSimulation::get_wac_program);
	ClassDB::bind_method(D_METHOD("compile_and_set_wac", "sources"), &NovaSimulation::compile_and_set_wac);
	ClassDB::bind_method(D_METHOD("get_wac_state"), &NovaSimulation::get_wac_state);
	ClassDB::bind_method(D_METHOD("get_runtime_perf_counters"), &NovaSimulation::get_runtime_perf_counters);
	ClassDB::bind_method(D_METHOD("set_runtime_profiling_enabled", "enabled"),
			&NovaSimulation::set_runtime_profiling_enabled);
	ClassDB::bind_method(D_METHOD("is_runtime_profiling_enabled"),
			&NovaSimulation::is_runtime_profiling_enabled);
	ClassDB::bind_method(D_METHOD("get_last_projectile_trace_times_us"),
			&NovaSimulation::get_last_projectile_trace_times_us);
	ClassDB::bind_method(D_METHOD("get_last_projectile_trace_counts"),
			&NovaSimulation::get_last_projectile_trace_counts);
	ClassDB::bind_method(D_METHOD("get_last_projectile_trace_faces"),
			&NovaSimulation::get_last_projectile_trace_faces);
	ClassDB::bind_method(D_METHOD("get_last_sim_tick_us"), &NovaSimulation::get_last_sim_tick_us);
	ClassDB::bind_method(D_METHOD("get_last_net_tick_us"), &NovaSimulation::get_last_net_tick_us);
	ClassDB::bind_method(D_METHOD("get_last_present_snapshot_us"),
			&NovaSimulation::get_last_present_snapshot_us);
	ClassDB::bind_method(D_METHOD("get_last_occlusion_build_us"),
			&NovaSimulation::get_last_occlusion_build_us);
	ClassDB::bind_method(D_METHOD("get_last_occlusion_probe_us"),
			&NovaSimulation::get_last_occlusion_probe_us);
	ClassDB::bind_method(D_METHOD("set_wac_paused", "paused"), &NovaSimulation::set_wac_paused);
	ClassDB::bind_method(D_METHOD("is_wac_paused"), &NovaSimulation::is_wac_paused);
	ClassDB::bind_method(D_METHOD("set_mission_variable", "index", "value"), &NovaSimulation::set_mission_variable);
	ClassDB::bind_method(D_METHOD("get_mission_variable", "index"), &NovaSimulation::get_mission_variable);
	ClassDB::bind_method(D_METHOD("has_event_fired", "index"), &NovaSimulation::has_event_fired);
	ClassDB::bind_method(D_METHOD("get_event_count"), &NovaSimulation::get_event_count);
	ClassDB::bind_method(D_METHOD("get_logic_tick"), &NovaSimulation::get_logic_tick);
	ClassDB::bind_method(D_METHOD("set_panm_time_ms", "time_ms"),
			&NovaSimulation::set_panm_time_ms);
	ClassDB::bind_method(D_METHOD("get_panm_time_ms"),
			&NovaSimulation::get_panm_time_ms);
	ClassDB::bind_method(D_METHOD("debug_set_panm_time_ms", "time_ms"),
			&NovaSimulation::debug_set_panm_time_ms);
	ClassDB::bind_method(D_METHOD("get_mission_variables_snapshot"), &NovaSimulation::get_mission_variables_snapshot);
	ClassDB::bind_method(D_METHOD("get_global_variables_snapshot"), &NovaSimulation::get_global_variables_snapshot);
	ClassDB::bind_method(D_METHOD("get_music_variables_snapshot"), &NovaSimulation::get_music_variables_snapshot);
	ClassDB::bind_method(D_METHOD("set_global_variable", "index", "value"), &NovaSimulation::set_global_variable);
	ClassDB::bind_method(D_METHOD("get_global_variable", "index"), &NovaSimulation::get_global_variable);
	ClassDB::bind_method(D_METHOD("get_fired_events_snapshot"), &NovaSimulation::get_fired_events_snapshot);
	ClassDB::bind_method(D_METHOD("get_entity_debug", "index"), &NovaSimulation::get_entity_debug);
	ClassDB::bind_method(D_METHOD("debug_set_entity_health", "index", "hp"), &NovaSimulation::debug_set_entity_health);
	ClassDB::bind_method(D_METHOD("debug_set_entity_position", "index", "mission_pos"), &NovaSimulation::debug_set_entity_position);
	ClassDB::bind_method(D_METHOD("get_world_entity_debug", "net_id"), &NovaSimulation::get_world_entity_debug);
	ClassDB::bind_method(D_METHOD("debug_set_world_entity_position", "net_id", "mission_pos"), &NovaSimulation::debug_set_world_entity_position);
	ClassDB::bind_method(D_METHOD("debug_teleport_local_player", "mission_pos", "yaw_deg", "pitch_deg"),
	                     &NovaSimulation::debug_teleport_local_player);
	ClassDB::bind_method(D_METHOD("set_ai_muzzle_world", "net_id", "godot_pos"), &NovaSimulation::set_ai_muzzle_world);
	ClassDB::bind_method(D_METHOD("get_round_outcome_debug"), &NovaSimulation::get_round_outcome_debug);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("ai_state_name", "state"), &NovaSimulation::ai_state_name);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("infantry_anim_key", "state"), &NovaSimulation::infantry_anim_key);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("infantry_anim_flags", "state"), &NovaSimulation::infantry_anim_flags);
	ClassDB::bind_method(D_METHOD("get_entity_count"), &NovaSimulation::get_entity_count);
	ClassDB::bind_method(D_METHOD("get_entity_kind", "index"), &NovaSimulation::get_entity_kind);
	ClassDB::bind_method(D_METHOD("get_entity_index", "index"), &NovaSimulation::get_entity_index);
	ClassDB::bind_method(D_METHOD("get_entity_position", "index"), &NovaSimulation::get_entity_position);
	ClassDB::bind_method(D_METHOD("get_entity_yaw", "index"), &NovaSimulation::get_entity_yaw);
	ClassDB::bind_method(D_METHOD("get_entity_yaw_deg", "index"), &NovaSimulation::get_entity_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_entity_state", "index"), &NovaSimulation::get_entity_state);
	ClassDB::bind_method(D_METHOD("get_entity_net_id", "index"), &NovaSimulation::get_entity_net_id);
	ClassDB::bind_method(D_METHOD("get_foliage_mask_anchor_positions"),
			&NovaSimulation::get_foliage_mask_anchor_positions);
	ClassDB::bind_method(D_METHOD("get_entity_effect_state_for_ssn", "ssn"),
	                     &NovaSimulation::get_entity_effect_state_for_ssn);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_ssn", "ssn"),
	                     &NovaSimulation::get_present_effect_state_for_ssn);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_wire_handle", "wire_handle"),
	                     &NovaSimulation::get_present_effect_state_for_wire_handle);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_bms_id", "bms_id"),
	                     &NovaSimulation::get_present_effect_state_for_bms_id);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_origin", "kind", "index"),
	                     &NovaSimulation::get_present_effect_state_for_origin);
	ClassDB::bind_method(D_METHOD("get_entity_bms_id", "index"), &NovaSimulation::get_entity_bms_id);
	ClassDB::bind_method(D_METHOD("get_entity_owner_connection_id", "index"),
	                     &NovaSimulation::get_entity_owner_connection_id);
	ClassDB::bind_method(D_METHOD("get_entity_wire_handle", "index"),
	                     &NovaSimulation::get_entity_wire_handle);
	ClassDB::bind_static_method("NovaSimulation",
			D_METHOD("decode_present_part_anim_phase",
					"snapshot", "base", "channel"),
			&NovaSimulation::decode_present_part_anim_phase);
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_phase", "index", "channel"), &NovaSimulation::get_entity_part_anim_phase);
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_active", "index", "channel"), &NovaSimulation::get_entity_part_anim_active);
	ClassDB::bind_method(D_METHOD("get_entity_body_anim_slot", "index"), &NovaSimulation::get_entity_body_anim_slot);
	ClassDB::bind_method(D_METHOD("get_entity_hidden", "index"), &NovaSimulation::get_entity_hidden);
	ClassDB::bind_method(D_METHOD("get_present_snapshot"), &NovaSimulation::get_present_snapshot);
	ClassDB::bind_method(D_METHOD("get_present_layout_revision"),
			&NovaSimulation::get_present_layout_revision);
	ClassDB::bind_method(D_METHOD("get_present_stride"), &NovaSimulation::get_present_stride);
	ClassDB::bind_method(D_METHOD("set_terrain_height_field", "terrain"), &NovaSimulation::set_terrain_height_field);
	ClassDB::bind_method(D_METHOD("set_item_seat_specs", "specs"), &NovaSimulation::set_item_seat_specs);
	ClassDB::bind_method(D_METHOD("set_infantry_anim_map", "resource_root", "adm_name"), &NovaSimulation::set_infantry_anim_map);
	ClassDB::bind_method(D_METHOD("resolve_infantry_adm_ids", "resource_root", "item_db"), &NovaSimulation::resolve_infantry_adm_ids);
	ClassDB::bind_method(D_METHOD("resolve_item_traits", "item_db"), &NovaSimulation::resolve_item_traits);
	ClassDB::bind_method(D_METHOD("resolve_ai_weapons", "item_db"), &NovaSimulation::resolve_ai_weapons);
	ClassDB::bind_method(D_METHOD("resolve_collision_instances", "item_db", "placer"),
	                     &NovaSimulation::resolve_collision_instances);
	ClassDB::bind_method(D_METHOD("occlusion_init_mission"),
	                     &NovaSimulation::occlusion_init_mission);
	ClassDB::bind_method(D_METHOD("run_occlusion_frame", "camera", "fov_y_deg", "aspect",
	                              "near", "fog_dist_units", "water_z_units", "force_indoors"),
	                     &NovaSimulation::run_occlusion_frame);
	ClassDB::bind_method(D_METHOD("get_building_visibility"),
	                     &NovaSimulation::get_building_visibility);
	ClassDB::bind_method(D_METHOD("get_render_culled_bms_ids"),
	                     &NovaSimulation::get_render_culled_bms_ids);
	ClassDB::bind_method(D_METHOD("get_building_visibility_changes"),
	                     &NovaSimulation::get_building_visibility_changes);
	ClassDB::bind_method(D_METHOD("get_render_culled_changes"),
	                     &NovaSimulation::get_render_culled_changes);
	ClassDB::bind_method(D_METHOD("entity_present_visible", "bms_id"),
	                     &NovaSimulation::entity_present_visible);
	ClassDB::bind_method(D_METHOD("reset_occlusion_apply_baseline"),
	                     &NovaSimulation::reset_occlusion_apply_baseline);
	ClassDB::bind_method(D_METHOD("occlusion_water_visible"),
	                     &NovaSimulation::occlusion_water_visible);
	ClassDB::bind_method(D_METHOD("occlusion_camera_indoors"),
	                     &NovaSimulation::occlusion_camera_indoors);
	ClassDB::bind_method(D_METHOD("get_collision_debug"), &NovaSimulation::get_collision_debug);
	ClassDB::bind_method(D_METHOD("get_occlusion_debug"), &NovaSimulation::get_occlusion_debug);
	ClassDB::bind_method(D_METHOD("get_round_debug"), &NovaSimulation::get_round_debug);
	ClassDB::bind_method(D_METHOD("get_throwable_visuals"), &NovaSimulation::get_throwable_visuals);
	ClassDB::bind_method(D_METHOD("debug_spawn_round", "from_godot", "dir_godot", "ammo_name"),
	                     &NovaSimulation::debug_spawn_round);
	ClassDB::bind_method(D_METHOD("debug_pick_entity", "from_godot", "dir_godot", "max_range_units"),
	                     &NovaSimulation::debug_pick_entity);
	ClassDB::bind_method(D_METHOD("get_hitbox_debug"), &NovaSimulation::get_hitbox_debug);
	ClassDB::bind_method(D_METHOD("get_occlusion_portal_debug", "anchor", "range_units"),
	                     &NovaSimulation::get_occlusion_portal_debug);
	ClassDB::bind_method(D_METHOD("local_player_indoors"), &NovaSimulation::local_player_indoors);
	ClassDB::bind_method(D_METHOD("local_player_blink_flags"), &NovaSimulation::local_player_blink_flags);
	ClassDB::bind_method(D_METHOD("local_player_interior_item_id"),
	                     &NovaSimulation::local_player_interior_item_id);
	BIND_CONSTANT(BLINK_INDOORS);
	BIND_CONSTANT(BLINK_WATER_OFF);
	ClassDB::bind_method(D_METHOD("compute_iris_samples", "cam_pos", "cam_forward", "light_dir"),
	                     &NovaSimulation::compute_iris_samples);
	ClassDB::bind_method(D_METHOD("sound_occlusion_distance_q16", "listener_pos", "source_pos",
	                              "distance_q16", "source_bms_id"),
	                     &NovaSimulation::sound_occlusion_distance_q16, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("local_player_in_armory_zone"), &NovaSimulation::local_player_in_armory_zone);
	ClassDB::bind_method(D_METHOD("local_player_in_vehicle_loadout_zone"), &NovaSimulation::local_player_in_vehicle_loadout_zone);
	ClassDB::bind_method(D_METHOD("local_player_toggle_mount"), &NovaSimulation::local_player_toggle_mount);
	ClassDB::bind_method(D_METHOD("get_attach_labels"), &NovaSimulation::get_attach_labels);
	ClassDB::bind_method(D_METHOD("apply_local_player_loadout", "kit", "player_class"),
	                     &NovaSimulation::apply_local_player_loadout);
	ClassDB::bind_method(D_METHOD("set_spawn_loadout", "kit", "filter_by_availability"),
	                     &NovaSimulation::set_spawn_loadout);
	ClassDB::bind_method(D_METHOD("has_explicit_spawn_loadout"),
	                     &NovaSimulation::has_explicit_spawn_loadout);
	ClassDB::bind_method(D_METHOD("set_weapon_availability", "pairs"),
	                     &NovaSimulation::set_weapon_availability);
	ClassDB::bind_method(D_METHOD("get_weapon_availability", "weapon_name"),
	                     &NovaSimulation::get_weapon_availability);
	ClassDB::bind_method(D_METHOD("respawn_local_player_loadout"),
	                     &NovaSimulation::respawn_local_player_loadout);
	ClassDB::bind_method(D_METHOD("set_local_player_class", "player_class"),
	                     &NovaSimulation::set_local_player_class);
	ClassDB::bind_method(D_METHOD("load_weapon_profile", "path"),
	                     &NovaSimulation::load_weapon_profile);
	ClassDB::bind_method(D_METHOD("get_weapon_profile_summary"),
	                     &NovaSimulation::get_weapon_profile_summary);
	ClassDB::bind_method(D_METHOD("request_local_player_weapon_category", "category"),
	                     &NovaSimulation::request_local_player_weapon_category);
	ClassDB::bind_method(D_METHOD("request_local_player_weapon_cycle", "direction"),
	                     &NovaSimulation::request_local_player_weapon_cycle);
	ClassDB::bind_method(D_METHOD("get_local_player_inventory"),
	                     &NovaSimulation::get_local_player_inventory);
	ClassDB::bind_method(D_METHOD("get_local_player_loadout"),
	                     &NovaSimulation::get_local_player_loadout);
	ClassDB::bind_method(D_METHOD("load_weapon_table", "resource_root", "name"),
	                     &NovaSimulation::load_weapon_table, DEFVAL(String("weapon.def")));
	ClassDB::bind_method(D_METHOD("load_ammo_table", "resource_root", "name"),
	                     &NovaSimulation::load_ammo_table, DEFVAL(String("ammo.def")));
	ClassDB::bind_method(D_METHOD("get_infantry_clip_count"), &NovaSimulation::get_infantry_clip_count);
	ClassDB::bind_method(D_METHOD("set_loco_scale", "scale"), &NovaSimulation::set_loco_scale);
	ClassDB::bind_method(D_METHOD("get_loco_scale"), &NovaSimulation::get_loco_scale);
	ClassDB::bind_method(D_METHOD("get_spawned_count"), &NovaSimulation::get_spawned_count);
	ClassDB::bind_method(D_METHOD("get_brain_count"), &NovaSimulation::get_brain_count);

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
	BIND_ENUM_CONSTANT(PF_HIDDEN);
	BIND_ENUM_CONSTANT(PF_LOCAL_VIEW_SUPPRESSED);
	BIND_ENUM_CONSTANT(PF_ALIVE);
	BIND_ENUM_CONSTANT(PF_RESPAWN_REVISION);
	BIND_ENUM_CONSTANT(PF_TYPE_ID);
	BIND_ENUM_CONSTANT(PF_WIRE_HANDLE);
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
	BIND_ENUM_CONSTANT(PF_TEX_TEAM_VALID);
	BIND_ENUM_CONSTANT(PF_TEX_TEAM);
	BIND_ENUM_CONSTANT(PF_ZONE_CTRL_VALID);
	BIND_ENUM_CONSTANT(PF_TEAMSWING);
	BIND_ENUM_CONSTANT(PF_LFP_CAMPPERCENT_VALID);
	BIND_ENUM_CONSTANT(PF_LFP_CAMPPERCENT);
	BIND_ENUM_CONSTANT(PF_WORLD_HEAT_GLOW_VALID);
	BIND_ENUM_CONSTANT(PF_WORLD_HEAT_GLOW);
	BIND_ENUM_CONSTANT(PF_RIGHT_HAND_COLLAPSED);
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

	BIND_ENUM_CONSTANT(MOUNT_COMMAND_PASSENGER_ONLY);
	BIND_ENUM_CONSTANT(MOUNT_COMMAND_SKIP_CONTROLLER);
	BIND_ENUM_CONSTANT(MOUNT_COMMAND_ANY_SEAT);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "playing"), "set_playing", "is_playing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loco_scale"), "set_loco_scale", "get_loco_scale");
}

void NovaSimulation::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS && playing_ && loaded_) {
		step();
	}
}
