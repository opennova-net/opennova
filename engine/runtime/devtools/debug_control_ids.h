// The wire ids of the debug-control table (ADR 0043 d12): every row F3 and
// MCP share, one constant per id, in the table's registration order (the
// `game_debug op=list` order; godot/tests/debug_controls_test.gd pins it).
// The table (godot/src/devtools/debug_control_table.cpp) registers each row
// under its constant and the F3 windows queue their ControlRequests by the
// same names, so an id is spelled once. A new row joins its page's run; an
// id never changes (it is the MCP wire).
#pragma once

namespace opennova::devtools::control_id {

// Terrain / Particles / Player pages: the world and player-camera toggles.
inline constexpr const char *kHideFoliage = "hide_foliage";
inline constexpr const char *kHideParticles = "hide_particles";
inline constexpr const char *kForceFpArms = "force_fp_arms";
inline constexpr const char *kBodyInFirstPerson = "body_in_first_person";
inline constexpr const char *kThirdPersonOnFoot = "third_person_on_foot";
// Terrain page: the renderer's debug knobs.
inline constexpr const char *kTerrainDrawMode = "terrain_draw_mode";
inline constexpr const char *kTerrainLodQuality = "terrain_lod_quality";
inline constexpr const char *kTerrainNoFrustum = "terrain_no_frustum";
inline constexpr const char *kTerrainNoNearfar = "terrain_no_nearfar";
inline constexpr const char *kTerrainNoSideplanes = "terrain_no_sideplanes";
inline constexpr const char *kTerrainNoPartialSubdiv = "terrain_no_partial_subdiv";
inline constexpr const char *kTerrainForceLeaves = "terrain_force_leaves";
inline constexpr const char *kTerrainForceLod0 = "terrain_force_lod0";
// Rendering page.
inline constexpr const char *kViewportDebugDraw = "viewport_debug_draw";
inline constexpr const char *kOcclusionCulling = "occlusion_culling";
// Player / Entities pages: the edit actions.
inline constexpr const char *kTeleportLocalPlayer = "teleport_local_player";
inline constexpr const char *kCycleMapMode = "cycle_map_mode";
inline constexpr const char *kSetEntityHealth = "set_entity_health";
inline constexpr const char *kSetEntityPosition = "set_entity_position";
inline constexpr const char *kSetEntityItemAttrib = "set_entity_item_attrib";
// Audio page.
inline constexpr const char *kSetAudioBusVolume = "set_audio_bus_volume";
inline constexpr const char *kSetAudioBusMute = "set_audio_bus_mute";
inline constexpr const char *kSetAudioBusSolo = "set_audio_bus_solo";
inline constexpr const char *kSetAudioBusBypass = "set_audio_bus_bypass";
// Sim / Vars pages: the runtime transport and the script state.
inline constexpr const char *kRuntimeTransport = "runtime_transport";
inline constexpr const char *kRuntimeReturnToMenu = "runtime_return_to_menu";
inline constexpr const char *kRuntimeWacPaused = "runtime_wac_paused";
inline constexpr const char *kSetMissionVariable = "set_mission_variable";
// Environment page: the clock, the wind, the lightning, the WAC weather
// commands (world::WeatherState carries the handler cites) and the home read.
inline constexpr const char *kEnvironmentTimeOfDay = "environment_time_of_day";
inline constexpr const char *kEnvironmentWindStrength = "environment_wind_strength";
inline constexpr const char *kEnvironmentLightningShort = "environment_lightning_short";
inline constexpr const char *kEnvironmentLightningLong = "environment_lightning_long";
inline constexpr const char *kEnvironmentRain = "environment_rain";
inline constexpr const char *kEnvironmentSnow = "environment_snow";
inline constexpr const char *kEnvironmentOvercast = "environment_overcast";
inline constexpr const char *kEnvironmentFogDistance = "environment_fog_distance";
inline constexpr const char *kEnvironmentMoveFog = "environment_move_fog";
inline constexpr const char *kEnvironmentSkySpeed = "environment_sky_speed";
inline constexpr const char *kEnvironmentQuake = "environment_quake";
inline constexpr const char *kEnvironmentFogType = "environment_fog_type";
inline constexpr const char *kEnvironmentSkyHeight = "environment_sky_height";
inline constexpr const char *kEnvironmentTimeOfDayMinutes = "environment_time_of_day_minutes";
inline constexpr const char *kEnvironmentSunFade = "environment_sun_fade";
inline constexpr const char *kEnvironmentColorFade = "environment_color_fade";
inline constexpr const char *kEnvironmentWindScale = "environment_wind_scale";
inline constexpr const char *kEnvironmentBlockColor = "environment_block_color";
inline constexpr const char *kEnvironmentLightningColor = "environment_lightning_color";
inline constexpr const char *kEnvironmentWeatherSnapshot = "environment_weather_snapshot";
// Sim / Player / Net / Entities pages: the automation actions.
inline constexpr const char *kDeployPick = "deploy_pick";
inline constexpr const char *kSetViewmodelWeapon = "set_viewmodel_weapon";
inline constexpr const char *kClearViewmodelWeapon = "clear_viewmodel_weapon";
inline constexpr const char *kNetJoinerDiagnostics = "net_joiner_diagnostics";
inline constexpr const char *kKillGroup = "kill_group";
inline constexpr const char *kCrewVehicle = "crew_vehicle";
inline constexpr const char *kCrewLocalPlayer = "crew_local_player";
inline constexpr const char *kLocalPlayerLook = "local_player_look";
inline constexpr const char *kApplyLocalPose = "apply_local_pose";
inline constexpr const char *kLocalSpectator = "local_spectator";

}  // namespace opennova::devtools::control_id
