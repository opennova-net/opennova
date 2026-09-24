// The Stats window's row tree (ADR 0039), transcribed from the retired GDScript
// Stats page: one row per measured system, parented by depth (each row nests
// under the nearest shallower row above it). Kinds:
//   SPAN      one slot: window mean + worst frame
//   GROUP     the sum of several slots: mean only (maxes do not add)
//   HEADER    a label with no time
//   RESIDUAL  a base slot minus a slot list: mean only
// Included by stats_window.cpp only.
#pragma once

#include <runtime/devtools/frame_stats_slots.h>

namespace opennova::devtools {

enum class RowKind { SPAN, GROUP, HEADER, RESIDUAL };

struct StatsRow {
	const char *id;
	const char *label;
	int depth;
	RowKind kind;
	Slot slot;            // SPAN: the slot; RESIDUAL: the base slot
	const Slot *slots;    // GROUP: the summed slots; RESIDUAL: the subtracted slots
	int slot_count;
};

namespace stats_rows {

inline constexpr Slot k_ai_infantry_collision_unattributed[] = {Slot::SIM_AI_INFANTRY_COLLISION_CONTACTS, Slot::SIM_AI_INFANTRY_COLLISION_REPULSION, Slot::SIM_AI_INFANTRY_COLLISION_GROUND};
inline constexpr Slot k_ai_infantry_unattributed[] = {Slot::SIM_AI_INFANTRY_REMOTE, Slot::SIM_AI_INFANTRY_COMBAT, Slot::SIM_AI_INFANTRY_ANIMATION, Slot::SIM_AI_INFANTRY_COLLISION};
inline constexpr Slot k_update_walks_other[] = {Slot::SIM_AI_INFANTRY};
inline constexpr Slot k_update_entities_unattributed[] = {Slot::SIM_UPDATE_WALKS, Slot::SIM_UPDATE_ATTACHMENTS, Slot::SIM_UPDATE_HELILIFT_FACES, Slot::SIM_UPDATE_PRECIPITATION, Slot::SIM_UPDATE_PIECES_EVENTS, Slot::SIM_UPDATE_PROJECTILES, Slot::SIM_UPDATE_EXPLOSIONS, Slot::SIM_UPDATE_PROXIMITY};
inline constexpr Slot k_attachment_unattributed[] = {Slot::SIM_ATTACHMENT_ORPHANS, Slot::SIM_ATTACHMENT_CHILDREN, Slot::SIM_ATTACHMENT_RIDERS};
inline constexpr Slot k_world_unattributed[] = {Slot::SIM_WORLD_SETUP, Slot::SIM_WORLD_SCRIPTS, Slot::SIM_UPDATE_ENTITIES, Slot::SIM_WORLD_HOUSEKEEPING};
inline constexpr Slot k_replication_query_grid_unattributed[] = {Slot::SIM_REPLICATION_QUERY_GRID_SPAN, Slot::SIM_REPLICATION_QUERY_GRID_BUCKET, Slot::SIM_REPLICATION_QUERY_GRID_WORKSPACE};
inline constexpr Slot k_replication_query_unattributed[] = {Slot::SIM_REPLICATION_QUERY_COLLECT, Slot::SIM_REPLICATION_QUERY_GRID};
inline constexpr Slot k_replication_entity_los_unattributed[] = {Slot::SIM_REPLICATION_ENTITY_LOS_TERRAIN, Slot::SIM_REPLICATION_ENTITY_LOS_SECTOR};
inline constexpr Slot k_replication_entity_score_math[] = {Slot::SIM_REPLICATION_ENTITY_LOS};
inline constexpr Slot k_replication_entity_unattributed[] = {Slot::SIM_REPLICATION_ENTITY_SETUP, Slot::SIM_REPLICATION_ENTITY_SCORE, Slot::SIM_REPLICATION_ENTITY_SORT, Slot::SIM_REPLICATION_ENTITY_BUDGET};
inline constexpr Slot k_replication_fan_unattributed[] = {Slot::SIM_REPLICATION_FAN_SETUP, Slot::SIM_REPLICATION_ROUNDS, Slot::SIM_REPLICATION_ENTITIES, Slot::SIM_REPLICATION_ENCODE, Slot::SIM_REPLICATION_ENQUEUE};
inline constexpr Slot k_replication_unattributed[] = {Slot::SIM_REPLICATION_QUERY_PREP, Slot::SIM_REPLICATION_SNAPSHOT, Slot::SIM_REPLICATION_FAN};
inline constexpr Slot k_server_unattributed[] = {Slot::SIM_SERVER_INPUT, Slot::SIM_SERVER_WORLD, Slot::SIM_MATCH, Slot::SIM_SERVER_RULES, Slot::SIM_SERVER_REPLICATION};
inline constexpr Slot k_host_unattributed[] = {Slot::SIM_HOST_RECEIVE, Slot::SIM_HOST_CONNECTIONS, Slot::SIM_HOST_ADAPTER, Slot::SIM_SERVER_TICK, Slot::SIM_HOST_SEND};
inline constexpr Slot k_client_unattributed[] = {Slot::SIM_CLIENT_SETUP, Slot::SIM_CLIENT_RECEIVE, Slot::SIM_CLIENT_MAINTENANCE, Slot::SIM_CLIENT_SEND};
inline constexpr Slot k_sim_unattributed[] = {Slot::SIM_HOST_PREP, Slot::SIM_HOST_PUMP, Slot::SIM_PLAYER_TAIL, Slot::SIM_WEAPON_WALK, Slot::SIM_NET, Slot::SIM_CLIENT_MATERIALIZE, Slot::SIM_CLIENT_MIRROR, Slot::SIM_CLIENT_PROXIES, Slot::SIM_CLIENT_WORLD, Slot::SIM_CLIENT_ATTACH, Slot::SIM_CLIENT_PLAYER, Slot::SIM_ADM_RESOLVE};
inline constexpr Slot k_trace[] = {Slot::TRACE_TERRAIN, Slot::TRACE_STATIC, Slot::TRACE_DYNAMIC, Slot::TRACE_PERSON};
inline constexpr Slot k_present[] = {Slot::PRESENT_SNAPSHOT, Slot::PRESENT_MISSION, Slot::PRESENT_WIRE, Slot::PRESENT_FIRE, Slot::PRESENT_DESTRUCTION, Slot::PRESENT_THROWABLE, Slot::PRESENT_SCARS};
inline constexpr Slot k_mission_rows_remainder[] = {Slot::PRESENT_MISSION_CORE, Slot::PRESENT_MISSION_AIM, Slot::PRESENT_MISSION_CONTROLS, Slot::PRESENT_MISSION_VISIBILITY, Slot::PRESENT_MISSION_BODY};
inline constexpr Slot k_runtime_overhead[] = {Slot::SIM_STEP, Slot::SIM_SINK, Slot::PRESENT_SNAPSHOT, Slot::PRESENT_MISSION, Slot::PRESENT_WIRE, Slot::PRESENT_FIRE, Slot::PRESENT_DESTRUCTION, Slot::PRESENT_THROWABLE, Slot::PRESENT_SCARS};
inline constexpr Slot k_occl[] = {Slot::OCCL_BUILD, Slot::OCCL_PROBE, Slot::OCCL_APPLY, Slot::OCCL_GLUE};
inline constexpr Slot k_occl_apply_remainder[] = {Slot::OCCL_BUILDING_QUERY, Slot::OCCL_BUILDING_APPLY, Slot::OCCL_CULL_QUERY, Slot::OCCL_CULL_APPLY, Slot::OCCL_LIGHT_QUERY, Slot::OCCL_LIGHT_APPLY, Slot::OCCL_WATER_APPLY};
inline constexpr Slot k_env[] = {Slot::WORLD_WEATHER, Slot::WORLD_BLINK, Slot::WORLD_IRIS};
inline constexpr Slot k_model_runtime_remainder[] = {Slot::MODEL_CLOCK_ANIMATION, Slot::MODEL_PANM, Slot::MODEL_MATERIAL, Slot::MODEL_ORDER_BOUNDS};
inline constexpr Slot k_world_remainder[] = {Slot::WORLD_RUNTIME, Slot::WORLD_LOCAL_VIEW, Slot::WORLD_FRAMEFX, Slot::WORLD_SCENE_ENV, Slot::WORLD_ENV_NODES, Slot::WORLD_WATER, Slot::WORLD_TERRAIN, Slot::WORLD_FOLIAGE, Slot::WORLD_NETWORK_FRAME, Slot::WORLD_WEATHER, Slot::WORLD_BLINK, Slot::OCCL_BUILD, Slot::OCCL_PROBE, Slot::OCCL_APPLY, Slot::OCCL_GLUE, Slot::WORLD_IRIS, Slot::WORLD_SUN_VEIL, Slot::WORLD_LIGHT, Slot::WORLD_MATERIAL, Slot::WORLD_SLOT_SHADOW, Slot::WORLD_PARTICLES, Slot::WORLD_AUDIO, Slot::WORLD_CLEAR, Slot::WORLD_ENV_CUBE};
inline constexpr Slot k_other_process[] = {Slot::FRAME_PLAYER_BEFORE, Slot::FRAME_WORLD, Slot::FRAME_PLAYER_AFTER, Slot::FRAME_HUD, Slot::FRAME_STATS_SAMPLE, Slot::FRAME_SHELL_CONTROL, Slot::FRAME_ROUND_FLOW, Slot::FRAME_MENU_SHELL, Slot::FRAME_MENU_VIDEO, Slot::FRAME_DEBUG_REFRESH};
inline constexpr Slot k_engine_frame[] = {Slot::FRAME_PROCESS_CALLBACKS, Slot::FRAME_PHYSICS_CALLBACKS, Slot::FRAME_DEFERRED_FLUSH, Slot::FRAME_DRAW, Slot::FRAME_PACING_INPUT};

inline constexpr StatsRow kRows[] = {
	{"frame", "Frame (wall)", 0, RowKind::SPAN, Slot::FRAME_WALL, nullptr, 0},
	{"before", "Player presenter (pre)", 1, RowKind::SPAN, Slot::FRAME_PLAYER_BEFORE, nullptr, 0},
	{"world", "World tick", 1, RowKind::SPAN, Slot::FRAME_WORLD, nullptr, 0},
	{"runtime", "Mission session + presentation", 2, RowKind::SPAN, Slot::WORLD_RUNTIME, nullptr, 0},
	{"sim", "Sim step", 3, RowKind::SPAN, Slot::SIM_STEP, nullptr, 0},
	{"host_prep", "Host setup/input", 4, RowKind::SPAN, Slot::SIM_HOST_PREP, nullptr, 0},
	{"host_pump", "Host owner pump", 4, RowKind::SPAN, Slot::SIM_HOST_PUMP, nullptr, 0},
	{"host_receive", "Receive/service", 5, RowKind::SPAN, Slot::SIM_HOST_RECEIVE, nullptr, 0},
	{"host_connections", "Connections/spawn", 5, RowKind::SPAN, Slot::SIM_HOST_CONNECTIONS, nullptr, 0},
	{"host_adapter", "Adapter registration", 5, RowKind::SPAN, Slot::SIM_HOST_ADAPTER, nullptr, 0},
	{"server_tick", "Authoritative server tick", 5, RowKind::SPAN, Slot::SIM_SERVER_TICK, nullptr, 0},
	{"server_input", "C2S/input prepass", 6, RowKind::SPAN, Slot::SIM_SERVER_INPUT, nullptr, 0},
	{"server_world", "World update", 6, RowKind::SPAN, Slot::SIM_SERVER_WORLD, nullptr, 0},
	{"world_setup", "Tick setup", 7, RowKind::SPAN, Slot::SIM_WORLD_SETUP, nullptr, 0},
	{"world_scripts", "WAC + BMS", 7, RowKind::SPAN, Slot::SIM_WORLD_SCRIPTS, nullptr, 0},
	{"update_entities", "Entity update (retail order)", 7, RowKind::SPAN, Slot::SIM_UPDATE_ENTITIES, nullptr, 0},
	{"update_walks", "Pool-1 + pool-0 walks", 8, RowKind::SPAN, Slot::SIM_UPDATE_WALKS, nullptr, 0},
	{"ai_infantry", "Infantry/player bodies", 9, RowKind::SPAN, Slot::SIM_AI_INFANTRY, nullptr, 0},
	{"ai_infantry_remote", "Remote player body", 10, RowKind::SPAN, Slot::SIM_AI_INFANTRY_REMOTE, nullptr, 0},
	{"ai_infantry_combat", "NPC combat/perception", 10, RowKind::SPAN, Slot::SIM_AI_INFANTRY_COMBAT, nullptr, 0},
	{"ai_infantry_animation", "Animation + root motion", 10, RowKind::SPAN, Slot::SIM_AI_INFANTRY_ANIMATION, nullptr, 0},
	{"ai_infantry_collision", "Movement collision resolver", 10, RowKind::SPAN, Slot::SIM_AI_INFANTRY_COLLISION, nullptr, 0},
	{"ai_infantry_collision_contacts", "Model contact passes", 11, RowKind::SPAN, Slot::SIM_AI_INFANTRY_COLLISION_CONTACTS, nullptr, 0},
	{"ai_infantry_collision_repulsion", "Person repulsion", 11, RowKind::SPAN, Slot::SIM_AI_INFANTRY_COLLISION_REPULSION, nullptr, 0},
	{"ai_infantry_collision_ground", "Ground settle ray", 11, RowKind::SPAN, Slot::SIM_AI_INFANTRY_COLLISION_GROUND, nullptr, 0},
	{"ai_infantry_collision_unattributed", "Resolver remainder", 11, RowKind::RESIDUAL, Slot::SIM_AI_INFANTRY_COLLISION, k_ai_infantry_collision_unattributed, 3},
	{"ai_infantry_unattributed", "Infantry remainder", 10, RowKind::RESIDUAL, Slot::SIM_AI_INFANTRY, k_ai_infantry_unattributed, 4},
	{"update_walks_other", "Brains, vehicle motors, items", 9, RowKind::RESIDUAL, Slot::SIM_UPDATE_WALKS, k_update_walks_other, 1},
	{"update_attachments", "Emplacement attachments", 8, RowKind::SPAN, Slot::SIM_UPDATE_ATTACHMENTS, nullptr, 0},
	{"attachment_orphans", "Orphan-chain cleanup", 9, RowKind::SPAN, Slot::SIM_ATTACHMENT_ORPHANS, nullptr, 0},
	{"attachment_children", "Child attachment pose", 9, RowKind::SPAN, Slot::SIM_ATTACHMENT_CHILDREN, nullptr, 0},
	{"attachment_riders", "Mounted-rider refresh", 9, RowKind::SPAN, Slot::SIM_ATTACHMENT_RIDERS, nullptr, 0},
	{"attachment_unattributed", "Attachment unattributed", 9, RowKind::RESIDUAL, Slot::SIM_UPDATE_ATTACHMENTS, k_attachment_unattributed, 3},
	{"update_helilift_faces", "HeliLift + facial interpolation", 8, RowKind::SPAN, Slot::SIM_UPDATE_HELILIFT_FACES, nullptr, 0},
	{"update_precipitation", "Precipitation fall", 8, RowKind::SPAN, Slot::SIM_UPDATE_PRECIPITATION, nullptr, 0},
	{"update_pieces_events", "Death pieces + timed AI events", 8, RowKind::SPAN, Slot::SIM_UPDATE_PIECES_EVENTS, nullptr, 0},
	{"update_projectiles", "Rotor wash + projectile rounds", 8, RowKind::SPAN, Slot::SIM_UPDATE_PROJECTILES, nullptr, 0},
	{"update_explosions", "Explosions, round hits, pool-2/3 walks, doors", 8, RowKind::SPAN, Slot::SIM_UPDATE_EXPLOSIONS, nullptr, 0},
	{"update_proximity", "Pool-0/1 proximity tables", 8, RowKind::SPAN, Slot::SIM_UPDATE_PROXIMITY, nullptr, 0},
	{"update_entities_unattributed", "Entity-update unattributed", 8, RowKind::RESIDUAL, Slot::SIM_UPDATE_ENTITIES, k_update_entities_unattributed, 8},
	{"world_housekeeping", "World housekeeping", 7, RowKind::SPAN, Slot::SIM_WORLD_HOUSEKEEPING, nullptr, 0},
	{"world_unattributed", "World-update unattributed", 7, RowKind::RESIDUAL, Slot::SIM_SERVER_WORLD, k_world_unattributed, 4},
	{"match", "Match update", 6, RowKind::SPAN, Slot::SIM_MATCH, nullptr, 0},
	{"server_rules", "Rules/events", 6, RowKind::SPAN, Slot::SIM_SERVER_RULES, nullptr, 0},
	{"server_replication", "Snapshot replication", 6, RowKind::SPAN, Slot::SIM_SERVER_REPLICATION, nullptr, 0},
	{"replication_query_prep", "LOS candidate index build", 7, RowKind::SPAN, Slot::SIM_REPLICATION_QUERY_PREP, nullptr, 0},
	{"replication_query_collect", "Solid target collection", 8, RowKind::SPAN, Slot::SIM_REPLICATION_QUERY_COLLECT, nullptr, 0},
	{"replication_query_grid", "Cell bucket publication", 8, RowKind::SPAN, Slot::SIM_REPLICATION_QUERY_GRID, nullptr, 0},
	{"replication_query_grid_span", "Candidate cell coverage", 9, RowKind::SPAN, Slot::SIM_REPLICATION_QUERY_GRID_SPAN, nullptr, 0},
	{"replication_query_grid_bucket", "Cell bucket population", 9, RowKind::SPAN, Slot::SIM_REPLICATION_QUERY_GRID_BUCKET, nullptr, 0},
	{"replication_query_grid_workspace", "Query workspace reset", 9, RowKind::SPAN, Slot::SIM_REPLICATION_QUERY_GRID_WORKSPACE, nullptr, 0},
	{"replication_query_grid_unattributed", "Publication remainder", 9, RowKind::RESIDUAL, Slot::SIM_REPLICATION_QUERY_GRID, k_replication_query_grid_unattributed, 3},
	{"replication_query_unattributed", "Index setup/remainder", 8, RowKind::RESIDUAL, Slot::SIM_REPLICATION_QUERY_PREP, k_replication_query_unattributed, 2},
	{"replication_snapshot", "World wire snapshot", 7, RowKind::SPAN, Slot::SIM_REPLICATION_SNAPSHOT, nullptr, 0},
	{"replication_fan", "Per-recipient frame fan", 7, RowKind::SPAN, Slot::SIM_REPLICATION_FAN, nullptr, 0},
	{"replication_fan_setup", "Anchor + header setup", 8, RowKind::SPAN, Slot::SIM_REPLICATION_FAN_SETUP, nullptr, 0},
	{"replication_rounds", "Round-event selection", 8, RowKind::SPAN, Slot::SIM_REPLICATION_ROUNDS, nullptr, 0},
	{"replication_entities", "Entity priority selection", 8, RowKind::SPAN, Slot::SIM_REPLICATION_ENTITIES, nullptr, 0},
	{"replication_entity_setup", "Age + recipient setup", 9, RowKind::SPAN, Slot::SIM_REPLICATION_ENTITY_SETUP, nullptr, 0},
	{"replication_entity_score", "Distance/view/LOS scoring", 9, RowKind::SPAN, Slot::SIM_REPLICATION_ENTITY_SCORE, nullptr, 0},
	{"replication_entity_los", "LOS collision raycasts", 10, RowKind::SPAN, Slot::SIM_REPLICATION_ENTITY_LOS, nullptr, 0},
	{"replication_entity_los_terrain", "Terrain heightfield march", 11, RowKind::SPAN, Slot::SIM_REPLICATION_ENTITY_LOS_TERRAIN, nullptr, 0},
	{"replication_entity_los_sector", "Sector solid walk", 11, RowKind::SPAN, Slot::SIM_REPLICATION_ENTITY_LOS_SECTOR, nullptr, 0},
	{"replication_entity_los_unattributed", "LOS setup/remainder", 11, RowKind::RESIDUAL, Slot::SIM_REPLICATION_ENTITY_LOS, k_replication_entity_los_unattributed, 2},
	{"replication_entity_score_math", "Distance/view score math", 10, RowKind::RESIDUAL, Slot::SIM_REPLICATION_ENTITY_SCORE, k_replication_entity_score_math, 1},
	{"replication_entity_sort", "Priority ordering", 9, RowKind::SPAN, Slot::SIM_REPLICATION_ENTITY_SORT, nullptr, 0},
	{"replication_entity_budget", "Byte-budget selection", 9, RowKind::SPAN, Slot::SIM_REPLICATION_ENTITY_BUDGET, nullptr, 0},
	{"replication_entity_unattributed", "Selection unattributed", 9, RowKind::RESIDUAL, Slot::SIM_REPLICATION_ENTITIES, k_replication_entity_unattributed, 4},
	{"replication_encode", "Frame serialization", 8, RowKind::SPAN, Slot::SIM_REPLICATION_ENCODE, nullptr, 0},
	{"replication_enqueue", "Transport enqueue", 8, RowKind::SPAN, Slot::SIM_REPLICATION_ENQUEUE, nullptr, 0},
	{"replication_fan_unattributed", "Frame-fan unattributed", 8, RowKind::RESIDUAL, Slot::SIM_REPLICATION_FAN, k_replication_fan_unattributed, 5},
	{"replication_unattributed", "Replication unattributed", 7, RowKind::RESIDUAL, Slot::SIM_SERVER_REPLICATION, k_replication_unattributed, 3},
	{"server_unattributed", "Server-tick unattributed", 6, RowKind::RESIDUAL, Slot::SIM_SERVER_TICK, k_server_unattributed, 5},
	{"host_send", "Send/flush", 5, RowKind::SPAN, Slot::SIM_HOST_SEND, nullptr, 0},
	{"host_unattributed", "Host-pump unattributed", 5, RowKind::RESIDUAL, Slot::SIM_HOST_PUMP, k_host_unattributed, 5},
	{"player_tail", "Frame tail: weather/view/medic", 4, RowKind::SPAN, Slot::SIM_PLAYER_TAIL, nullptr, 0},
	{"weapon_walk", "Weapon action walk", 4, RowKind::SPAN, Slot::SIM_WEAPON_WALK, nullptr, 0},
	{"net", "Client network/decode", 4, RowKind::SPAN, Slot::SIM_NET, nullptr, 0},
	{"client_setup", "Client clock/setup", 5, RowKind::SPAN, Slot::SIM_CLIENT_SETUP, nullptr, 0},
	{"client_receive", "Receive + state fold", 5, RowKind::SPAN, Slot::SIM_CLIENT_RECEIVE, nullptr, 0},
	{"client_maintenance", "Replica maintenance/movers", 5, RowKind::SPAN, Slot::SIM_CLIENT_MAINTENANCE, nullptr, 0},
	{"client_send", "Joiner C2S build/send", 5, RowKind::SPAN, Slot::SIM_CLIENT_SEND, nullptr, 0},
	{"client_unattributed", "Client-frame unattributed", 5, RowKind::RESIDUAL, Slot::SIM_NET, k_client_unattributed, 4},
	{"client_materialize", "Joiner stream materialize + folds", 4, RowKind::SPAN, Slot::SIM_CLIENT_MATERIALIZE, nullptr, 0},
	{"client_mirror", "Joiner wire pose mirror", 4, RowKind::SPAN, Slot::SIM_CLIENT_MIRROR, nullptr, 0},
	{"client_proxies", "Joiner collision proxies", 4, RowKind::SPAN, Slot::SIM_CLIENT_PROXIES, nullptr, 0},
	{"client_world", "Joiner local world tick (see World update)", 4, RowKind::SPAN, Slot::SIM_CLIENT_WORLD, nullptr, 0},
	{"client_attach", "Joiner attachment recompose", 4, RowKind::SPAN, Slot::SIM_CLIENT_ATTACH, nullptr, 0},
	{"client_player", "Joiner view/weapon devices", 4, RowKind::SPAN, Slot::SIM_CLIENT_PLAYER, nullptr, 0},
	{"adm_resolve", "Animation registry resolve", 4, RowKind::SPAN, Slot::SIM_ADM_RESOLVE, nullptr, 0},
	{"sim_unattributed", "Sim-step unattributed", 4, RowKind::RESIDUAL, Slot::SIM_STEP, k_sim_unattributed, 12},
	{"trace", "Projectile trace (attributed)", 4, RowKind::GROUP, Slot::COUNT, k_trace, 4},
	{"trace_terrain", "Terrain", 5, RowKind::SPAN, Slot::TRACE_TERRAIN, nullptr, 0},
	{"trace_static", "Static", 5, RowKind::SPAN, Slot::TRACE_STATIC, nullptr, 0},
	{"trace_dynamic", "Dynamic", 5, RowKind::SPAN, Slot::TRACE_DYNAMIC, nullptr, 0},
	{"trace_person", "Person", 5, RowKind::SPAN, Slot::TRACE_PERSON, nullptr, 0},
	{"effects_drain", "Effects drain", 3, RowKind::SPAN, Slot::EFFECTS_DRAIN, nullptr, 0},
	{"sim_sink", "Godot tick sink", 3, RowKind::SPAN, Slot::SIM_SINK, nullptr, 0},
	{"effects", "Effects tick", 3, RowKind::SPAN, Slot::EFFECTS_TICK, nullptr, 0},
	{"present", "Present", 3, RowKind::GROUP, Slot::COUNT, k_present, 7},
	{"snapshot", "Row snapshot", 4, RowKind::SPAN, Slot::PRESENT_SNAPSHOT, nullptr, 0},
	{"mission_rows", "Mission rows", 4, RowKind::SPAN, Slot::PRESENT_MISSION, nullptr, 0},
	{"mission_rows_core", "Row scan/transform/submission", 5, RowKind::SPAN, Slot::PRESENT_MISSION_CORE, nullptr, 0},
	{"mission_rows_aim", "Aim overlay", 5, RowKind::SPAN, Slot::PRESENT_MISSION_AIM, nullptr, 0},
	{"mission_rows_controls", "Control publication", 5, RowKind::SPAN, Slot::PRESENT_MISSION_CONTROLS, nullptr, 0},
	{"mission_rows_visibility", "Visibility/section masks", 5, RowKind::SPAN, Slot::PRESENT_MISSION_VISIBILITY, nullptr, 0},
	{"mission_rows_body", "Body pose", 5, RowKind::SPAN, Slot::PRESENT_MISSION_BODY, nullptr, 0},
	{"mission_rows_remainder", "Mission-row orchestration remainder", 5, RowKind::RESIDUAL, Slot::PRESENT_MISSION, k_mission_rows_remainder, 5},
	{"wire_rows", "Wire rows", 4, RowKind::SPAN, Slot::PRESENT_WIRE, nullptr, 0},
	{"fire", "Fire", 4, RowKind::SPAN, Slot::PRESENT_FIRE, nullptr, 0},
	{"destruction", "Destruction", 4, RowKind::SPAN, Slot::PRESENT_DESTRUCTION, nullptr, 0},
	{"throwable", "Throwable", 4, RowKind::SPAN, Slot::PRESENT_THROWABLE, nullptr, 0},
	{"scars", "Scars", 4, RowKind::SPAN, Slot::PRESENT_SCARS, nullptr, 0},
	{"runtime_overhead", "Session/presentation overhead", 3, RowKind::RESIDUAL, Slot::WORLD_RUNTIME, k_runtime_overhead, 9},
	{"local_view", "Local view publication", 2, RowKind::SPAN, Slot::WORLD_LOCAL_VIEW, nullptr, 0},
	{"framefx", "Q3 compile", 2, RowKind::SPAN, Slot::WORLD_FRAMEFX, nullptr, 0},
	{"scene_env", "Scene fog/ambient publication", 2, RowKind::SPAN, Slot::WORLD_SCENE_ENV, nullptr, 0},
	{"env_nodes", "Sky/celestial/sun/weather smoothing", 2, RowKind::SPAN, Slot::WORLD_ENV_NODES, nullptr, 0},
	{"terrain", "Terrain frame publication", 2, RowKind::SPAN, Slot::WORLD_TERRAIN, nullptr, 0},
	{"water", "Water strip + mirror camera", 2, RowKind::SPAN, Slot::WORLD_WATER, nullptr, 0},
	{"foliage", "Foliage", 2, RowKind::SPAN, Slot::WORLD_FOLIAGE, nullptr, 0},
	{"network_frame", "Network session devices", 2, RowKind::SPAN, Slot::WORLD_NETWORK_FRAME, nullptr, 0},
	{"occl", "Occlusion", 2, RowKind::GROUP, Slot::COUNT, k_occl, 4},
	{"occl_build", "Build (native)", 3, RowKind::SPAN, Slot::OCCL_BUILD, nullptr, 0},
	{"occl_probe", "Probe (native)", 3, RowKind::SPAN, Slot::OCCL_PROBE, nullptr, 0},
	{"occl_glue", "Native call setup/binding", 3, RowKind::SPAN, Slot::OCCL_GLUE, nullptr, 0},
	{"occl_apply", "Visibility/lighting publication", 3, RowKind::SPAN, Slot::OCCL_APPLY, nullptr, 0},
	{"occl_building_query", "Building delta query", 4, RowKind::SPAN, Slot::OCCL_BUILDING_QUERY, nullptr, 0},
	{"occl_building_apply", "Building node writes", 4, RowKind::SPAN, Slot::OCCL_BUILDING_APPLY, nullptr, 0},
	{"occl_cull_query", "Entity-cull delta query", 4, RowKind::SPAN, Slot::OCCL_CULL_QUERY, nullptr, 0},
	{"occl_cull_apply", "Entity-cull node writes", 4, RowKind::SPAN, Slot::OCCL_CULL_APPLY, nullptr, 0},
	{"occl_light_query", "Sun-visibility query", 4, RowKind::SPAN, Slot::OCCL_LIGHT_QUERY, nullptr, 0},
	{"occl_light_apply", "Lighting-context writes", 4, RowKind::SPAN, Slot::OCCL_LIGHT_APPLY, nullptr, 0},
	{"occl_water_apply", "Water visibility write", 4, RowKind::SPAN, Slot::OCCL_WATER_APPLY, nullptr, 0},
	{"occl_apply_remainder", "Publication overhead", 4, RowKind::RESIDUAL, Slot::OCCL_APPLY, k_occl_apply_remainder, 7},
	{"env", "Env (weather/blink/iris)", 2, RowKind::GROUP, Slot::COUNT, k_env, 3},
	{"sun_veil", "Sun-veil publication", 2, RowKind::SPAN, Slot::WORLD_SUN_VEIL, nullptr, 0},
	{"light", "Point-light selection/publication", 2, RowKind::SPAN, Slot::WORLD_LIGHT, nullptr, 0},
	{"material", "Model runtime", 2, RowKind::SPAN, Slot::WORLD_MATERIAL, nullptr, 0},
	{"model_clock_animation", "Clocks + animation", 3, RowKind::SPAN, Slot::MODEL_CLOCK_ANIMATION, nullptr, 0},
	{"model_panm", "PANM transforms", 3, RowKind::SPAN, Slot::MODEL_PANM, nullptr, 0},
	{"model_material", "Material generators/textures", 3, RowKind::SPAN, Slot::MODEL_MATERIAL, nullptr, 0},
	{"model_order_bounds", "Render order + bounds", 3, RowKind::SPAN, Slot::MODEL_ORDER_BOUNDS, nullptr, 0},
	{"model_runtime_remainder", "Awake-set/gating remainder", 3, RowKind::RESIDUAL, Slot::WORLD_MATERIAL, k_model_runtime_remainder, 4},
	{"slot_shadow", "Slot-shadow planning/publication", 2, RowKind::SPAN, Slot::WORLD_SLOT_SHADOW, nullptr, 0},
	{"particles", "Particle-frame publication", 2, RowKind::SPAN, Slot::WORLD_PARTICLES, nullptr, 0},
	{"audio", "Audio", 2, RowKind::SPAN, Slot::WORLD_AUDIO, nullptr, 0},
	{"clear", "Frame clear-color publication", 2, RowKind::SPAN, Slot::WORLD_CLEAR, nullptr, 0},
	{"env_cube", "Environment-cube submission", 2, RowKind::SPAN, Slot::WORLD_ENV_CUBE, nullptr, 0},
	{"world_remainder", "World orchestration remainder", 2, RowKind::RESIDUAL, Slot::FRAME_WORLD, k_world_remainder, 24},
	{"after", "Player presenter (post)", 1, RowKind::SPAN, Slot::FRAME_PLAYER_AFTER, nullptr, 0},
	{"hud", "HUD tick", 1, RowKind::SPAN, Slot::FRAME_HUD, nullptr, 0},
	{"hud_scalars", "Scalars", 2, RowKind::SPAN, Slot::HUD_SCALARS, nullptr, 0},
	{"hud_attach", "Attach labels", 2, RowKind::SPAN, Slot::HUD_ATTACH, nullptr, 0},
	{"hud_waypoint", "Waypoint", 2, RowKind::SPAN, Slot::HUD_WAYPOINT, nullptr, 0},
	{"hud_info", "Info build", 2, RowKind::SPAN, Slot::HUD_INFO, nullptr, 0},
	{"hud_flush", "Flush", 2, RowKind::SPAN, Slot::HUD_FLUSH, nullptr, 0},
	{"stats_sample", "Stats sampling", 1, RowKind::SPAN, Slot::FRAME_STATS_SAMPLE, nullptr, 0},
	{"shell_control", "Shell state/control", 1, RowKind::SPAN, Slot::FRAME_SHELL_CONTROL, nullptr, 0},
	{"round_flow", "Round/session transitions", 1, RowKind::SPAN, Slot::FRAME_ROUND_FLOW, nullptr, 0},
	{"menu_shell", "Menu shell process", 1, RowKind::SPAN, Slot::FRAME_MENU_SHELL, nullptr, 0},
	{"menu_video", "Menu video decode/upload", 1, RowKind::SPAN, Slot::FRAME_MENU_VIDEO, nullptr, 0},
	{"debug_refresh", "F3 refresh", 1, RowKind::SPAN, Slot::FRAME_DEBUG_REFRESH, nullptr, 0},
	{"frame_overhead", "Frame overhead", 1, RowKind::HEADER, Slot::COUNT, nullptr, 0},
	{"other_process", "Other process callbacks", 2, RowKind::RESIDUAL, Slot::FRAME_PROCESS_CALLBACKS, k_other_process, 10},
	{"physics_callbacks", "Physics callbacks", 2, RowKind::SPAN, Slot::FRAME_PHYSICS_CALLBACKS, nullptr, 0},
	{"deferred_flush", "Deferred flush (draw callbacks, transforms)", 2, RowKind::SPAN, Slot::FRAME_DEFERRED_FLUSH, nullptr, 0},
	{"flush_queued", "Queued by callbacks (deferred calls, draws)", 3, RowKind::SPAN, Slot::FRAME_FLUSH_QUEUED, nullptr, 0},
	{"hud_draw_compile", "HUD draw compile", 4, RowKind::SPAN, Slot::HUD_DRAW_COMPILE, nullptr, 0},
	{"hud_draw_emit", "HUD draw emit", 4, RowKind::SPAN, Slot::HUD_DRAW_EMIT, nullptr, 0},
	{"flush_tail", "Flush tail (transforms, timers, node frees, RS sync)", 3, RowKind::SPAN, Slot::FRAME_FLUSH_TAIL, nullptr, 0},
	{"render_draw", "RenderingServer draw (all viewports)", 2, RowKind::SPAN, Slot::FRAME_DRAW, nullptr, 0},
	{"pacing_input", "Servers/input/pacing", 2, RowKind::SPAN, Slot::FRAME_PACING_INPUT, nullptr, 0},
	{"engine_frame", "Unattributed engine time", 2, RowKind::RESIDUAL, Slot::FRAME_WALL, k_engine_frame, 5},
	{"render", "Render", 0, RowKind::HEADER, Slot::COUNT, nullptr, 0},
	{"render_main", "Main view", 1, RowKind::HEADER, Slot::COUNT, nullptr, 0},
	{"render_shadow", "Shadow passes", 1, RowKind::HEADER, Slot::COUNT, nullptr, 0},
	{"render_root_cpu", "Viewport CPU", 1, RowKind::SPAN, Slot::RENDER_ROOT_CPU, nullptr, 0},
	{"render_root_gpu", "Viewport GPU", 1, RowKind::SPAN, Slot::RENDER_ROOT_GPU, nullptr, 0},
	{"render_water", "Water mirror", 1, RowKind::HEADER, Slot::COUNT, nullptr, 0},
	{"render_water_cpu", "Water RTT CPU", 2, RowKind::SPAN, Slot::RENDER_WATER_CPU, nullptr, 0},
	{"render_water_gpu", "Water RTT GPU", 2, RowKind::SPAN, Slot::RENDER_WATER_GPU, nullptr, 0},
	{"render_q3", "FrameFX focused Q3", 1, RowKind::HEADER, Slot::COUNT, nullptr, 0},
	{"render_q3_gpu", "Q3 pass GPU", 2, RowKind::SPAN, Slot::RENDER_Q3_GPU, nullptr, 0},
	{"render_slot", "Slot-shadow captures", 1, RowKind::HEADER, Slot::COUNT, nullptr, 0},
	{"render_slot_gpu", "Slot captures GPU", 2, RowKind::SPAN, Slot::RENDER_SLOT_GPU, nullptr, 0},
};

inline constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

}  // namespace stats_rows
}  // namespace opennova::devtools
