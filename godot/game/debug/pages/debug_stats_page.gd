class_name DebugStatsPage
extends DebugPage
## The debug overlay's Stats page: one row per major runtime system with
## window-averaged per-frame milliseconds (avg + worst frame in the window)
## and live counters beside them. The numbers come from the shared
## FrameStatsBoard the hosts feed; this pane only opens/closes the capture
## window and formats what accumulated.
##
## Capture is edge-gated: the board is active only while this tab is the visible
## overlay tab, so a closed overlay costs the hosts nothing.
## Rows read from a fixed table; the Tree is built once and text is updated
## in place. Every read/format runs at the (divided) overlay refresh cadence,
## never per frame.

const MissionPresentation := preload("res://game/world/mission_presentation.gd")

const _KIND_SPAN := 0    # one board slot: avg + max ms
const _KIND_GROUP := 1   # sum of several slots: avg ms only (maxes don't add)
const _KIND_HEADER := 2  # label + info only (no time)
const _KIND_RESIDUAL := 3 # base slot minus a slot list: avg ms only

# Read the board every Nth overlay refresh: at the overlay's 0.25 s cadence
# this makes 0.5 s windows — wide enough that two consecutive readings of a
# steady scene agree.
const _REFRESH_DIVIDER := 2

# The fixed row table. depth parents each row under the nearest shallower row,
# the span-tree convention the Perf tab uses.
const _ROWS := [
	{"id": "frame", "label": "Frame (wall)", "depth": 0, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_WALL},
	{"id": "before", "label": "Player presenter (pre)", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_PLAYER_BEFORE},
	{"id": "world", "label": "World tick", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_WORLD},
	{"id": "runtime", "label": "Mission session + presentation", "depth": 2,
			"kind": _KIND_SPAN,
			"slot": FrameStatsBoard.WORLD_RUNTIME},
	{"id": "sim", "label": "Sim step", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_STEP},
	{"id": "host_prep", "label": "Host setup/input", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_HOST_PREP},
	{"id": "host_pump", "label": "Host owner pump", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_HOST_PUMP},
	{"id": "host_receive", "label": "Receive/service", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_HOST_RECEIVE},
	{"id": "host_connections", "label": "Connections/spawn", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_HOST_CONNECTIONS},
	{"id": "host_adapter", "label": "Adapter registration", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_HOST_ADAPTER},
	{"id": "server_tick", "label": "Authoritative server tick", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_SERVER_TICK},
	{"id": "server_input", "label": "C2S/input prepass", "depth": 6,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_SERVER_INPUT},
	{"id": "server_world", "label": "World update", "depth": 6, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_SERVER_WORLD},
	{"id": "world_setup", "label": "Tick setup", "depth": 7, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_WORLD_SETUP},
	{"id": "world_scripts", "label": "WAC + BMS", "depth": 7, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_WORLD_SCRIPTS},
	{"id": "world_ai", "label": "AI + entities", "depth": 7, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_WORLD_AI},
	{"id": "ai_reactions", "label": "Damage reactions", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_REACTIONS},
	{"id": "ai_collision", "label": "Collision/proximity tables", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_COLLISION},
	{"id": "ai_entities", "label": "Brain + entity body pass", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_ENTITIES},
	{"id": "ai_infantry", "label": "Infantry/player bodies", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_INFANTRY},
	{"id": "ai_infantry_remote", "label": "Remote player body", "depth": 10,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_INFANTRY_REMOTE},
	{"id": "ai_infantry_combat", "label": "NPC combat/perception", "depth": 10,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_INFANTRY_COMBAT},
	{"id": "ai_infantry_animation", "label": "Animation + root motion", "depth": 10,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_INFANTRY_ANIMATION},
	{"id": "ai_infantry_collision", "label": "Movement collision resolver", "depth": 10,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_INFANTRY_COLLISION},
	{"id": "ai_infantry_collision_contacts", "label": "Model contact passes", "depth": 11,
			"kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_AI_INFANTRY_COLLISION_CONTACTS},
	{"id": "ai_infantry_collision_repulsion", "label": "Person repulsion", "depth": 11,
			"kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_AI_INFANTRY_COLLISION_REPULSION},
	{"id": "ai_infantry_collision_ground", "label": "Ground settle ray", "depth": 11,
			"kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_AI_INFANTRY_COLLISION_GROUND},
	{"id": "ai_infantry_collision_unattributed", "label": "Resolver remainder", "depth": 11,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_AI_INFANTRY_COLLISION,
			"minus": [FrameStatsBoard.SIM_AI_INFANTRY_COLLISION_CONTACTS,
					FrameStatsBoard.SIM_AI_INFANTRY_COLLISION_REPULSION,
					FrameStatsBoard.SIM_AI_INFANTRY_COLLISION_GROUND]},
	{"id": "ai_infantry_unattributed", "label": "Infantry remainder", "depth": 10,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_AI_INFANTRY,
			"minus": [FrameStatsBoard.SIM_AI_INFANTRY_REMOTE,
					FrameStatsBoard.SIM_AI_INFANTRY_COMBAT,
					FrameStatsBoard.SIM_AI_INFANTRY_ANIMATION,
					FrameStatsBoard.SIM_AI_INFANTRY_COLLISION]},
	{"id": "ai_other_entities", "label": "Other entity brains", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_OTHER_ENTITIES},
	{"id": "ai_entities_unattributed", "label": "Entity-pass unattributed", "depth": 9,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_AI_ENTITIES,
			"minus": [FrameStatsBoard.SIM_AI_INFANTRY,
					FrameStatsBoard.SIM_AI_OTHER_ENTITIES]},
	{"id": "ai_auth_vehicles", "label": "Authority vehicle motors", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_AUTH_VEHICLES},
	{"id": "ai_vehicle_scan", "label": "Vehicle registry scan", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_VEHICLE_SCAN},
	{"id": "ai_vehicle_motors", "label": "Family motor dispatch", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_VEHICLE_MOTORS},
	{"id": "ai_vehicle_riders", "label": "Final rider refresh", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_VEHICLE_RIDERS},
	{"id": "ai_vehicles_unattributed", "label": "Vehicle-pass unattributed", "depth": 9,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_AI_AUTH_VEHICLES,
			"minus": [FrameStatsBoard.SIM_AI_VEHICLE_SCAN,
					FrameStatsBoard.SIM_AI_VEHICLE_MOTORS,
					FrameStatsBoard.SIM_AI_VEHICLE_RIDERS]},
	{"id": "ai_client_vehicles", "label": "Client vehicle prediction", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_CLIENT_VEHICLES},
	{"id": "ai_events", "label": "Timed AI events", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_AI_EVENTS},
	{"id": "ai_unattributed", "label": "AI unattributed", "depth": 8,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_WORLD_AI,
			"minus": [FrameStatsBoard.SIM_AI_REACTIONS,
					FrameStatsBoard.SIM_AI_COLLISION, FrameStatsBoard.SIM_AI_ENTITIES,
					FrameStatsBoard.SIM_AI_AUTH_VEHICLES,
					FrameStatsBoard.SIM_AI_CLIENT_VEHICLES, FrameStatsBoard.SIM_AI_EVENTS]},
	{"id": "world_attachments", "label": "Emplacement attachments", "depth": 7,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_WORLD_ATTACHMENTS},
	{"id": "attachment_orphans", "label": "Orphan-chain cleanup", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_ATTACHMENT_ORPHANS},
	{"id": "attachment_children", "label": "Child attachment pose", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_ATTACHMENT_CHILDREN},
	{"id": "attachment_riders", "label": "Mounted-rider refresh", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_ATTACHMENT_RIDERS},
	{"id": "attachment_unattributed", "label": "Attachment unattributed", "depth": 8,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_WORLD_ATTACHMENTS,
			"minus": [FrameStatsBoard.SIM_ATTACHMENT_ORPHANS,
					FrameStatsBoard.SIM_ATTACHMENT_CHILDREN,
					FrameStatsBoard.SIM_ATTACHMENT_RIDERS]},
	{"id": "world_throwables", "label": "Throwables", "depth": 7,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_WORLD_THROWABLES},
	{"id": "world_weapons", "label": "Mounted weapon actions", "depth": 7,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_WORLD_WEAPONS},
	{"id": "world_projectiles", "label": "Projectiles", "depth": 7,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_WORLD_PROJECTILES},
	{"id": "world_destruction", "label": "Destruction + debris", "depth": 7,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_WORLD_DESTRUCTION},
	{"id": "world_housekeeping", "label": "World housekeeping", "depth": 7,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_WORLD_HOUSEKEEPING},
	{"id": "world_unattributed", "label": "World-update unattributed", "depth": 7,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_SERVER_WORLD,
			"minus": [FrameStatsBoard.SIM_WORLD_SETUP,
					FrameStatsBoard.SIM_WORLD_SCRIPTS, FrameStatsBoard.SIM_WORLD_AI,
					FrameStatsBoard.SIM_WORLD_ATTACHMENTS,
					FrameStatsBoard.SIM_WORLD_THROWABLES,
					FrameStatsBoard.SIM_WORLD_WEAPONS,
					FrameStatsBoard.SIM_WORLD_PROJECTILES,
					FrameStatsBoard.SIM_WORLD_DESTRUCTION,
					FrameStatsBoard.SIM_WORLD_HOUSEKEEPING]},
	{"id": "match", "label": "Match update", "depth": 6, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_MATCH},
	{"id": "server_rules", "label": "Rules/events", "depth": 6, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_SERVER_RULES},
	{"id": "server_replication", "label": "Snapshot replication", "depth": 6,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_SERVER_REPLICATION},
	{"id": "replication_query_prep", "label": "LOS candidate index build", "depth": 7,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_QUERY_PREP},
	{"id": "replication_query_collect", "label": "Solid target collection", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_QUERY_COLLECT},
	{"id": "replication_query_grid", "label": "Cell bucket publication", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_QUERY_GRID},
	{"id": "replication_query_grid_span", "label": "Candidate cell coverage", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_QUERY_GRID_SPAN},
	{"id": "replication_query_grid_bucket", "label": "Cell bucket population", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_QUERY_GRID_BUCKET},
	{"id": "replication_query_grid_workspace", "label": "Query workspace reset", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_QUERY_GRID_WORKSPACE},
	{"id": "replication_query_grid_unattributed", "label": "Publication remainder", "depth": 9,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_REPLICATION_QUERY_GRID,
			"minus": [FrameStatsBoard.SIM_REPLICATION_QUERY_GRID_SPAN,
					FrameStatsBoard.SIM_REPLICATION_QUERY_GRID_BUCKET,
					FrameStatsBoard.SIM_REPLICATION_QUERY_GRID_WORKSPACE]},
	{"id": "replication_query_unattributed", "label": "Index setup/remainder", "depth": 8,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_REPLICATION_QUERY_PREP,
			"minus": [FrameStatsBoard.SIM_REPLICATION_QUERY_COLLECT,
					FrameStatsBoard.SIM_REPLICATION_QUERY_GRID]},
	{"id": "replication_snapshot", "label": "World wire snapshot", "depth": 7,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_SNAPSHOT},
	{"id": "replication_fan", "label": "Per-recipient frame fan", "depth": 7,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_FAN},
	{"id": "replication_fan_setup", "label": "Anchor + header setup", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_FAN_SETUP},
	{"id": "replication_rounds", "label": "Round-event selection", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_ROUNDS},
	{"id": "replication_entities", "label": "Entity priority selection", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_ENTITIES},
	{"id": "replication_entity_setup", "label": "Age + recipient setup", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_ENTITY_SETUP},
	{"id": "replication_entity_score", "label": "Distance/view/LOS scoring", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_ENTITY_SCORE},
	{"id": "replication_entity_los", "label": "LOS collision raycasts", "depth": 10,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_ENTITY_LOS},
	{"id": "replication_entity_los_terrain", "label": "Terrain heightfield march", "depth": 11,
			"kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_REPLICATION_ENTITY_LOS_TERRAIN},
	{"id": "replication_entity_los_sector", "label": "Sector solid walk", "depth": 11,
			"kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_REPLICATION_ENTITY_LOS_SECTOR},
	{"id": "replication_entity_los_unattributed", "label": "LOS setup/remainder", "depth": 11,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_REPLICATION_ENTITY_LOS,
			"minus": [FrameStatsBoard.SIM_REPLICATION_ENTITY_LOS_TERRAIN,
					FrameStatsBoard.SIM_REPLICATION_ENTITY_LOS_SECTOR]},
	{"id": "replication_entity_score_math", "label": "Distance/view score math", "depth": 10,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_REPLICATION_ENTITY_SCORE,
			"minus": [FrameStatsBoard.SIM_REPLICATION_ENTITY_LOS]},
	{"id": "replication_entity_sort", "label": "Priority ordering", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_ENTITY_SORT},
	{"id": "replication_entity_budget", "label": "Byte-budget selection", "depth": 9,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_ENTITY_BUDGET},
	{"id": "replication_entity_unattributed", "label": "Selection unattributed", "depth": 9,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_REPLICATION_ENTITIES,
			"minus": [FrameStatsBoard.SIM_REPLICATION_ENTITY_SETUP,
					FrameStatsBoard.SIM_REPLICATION_ENTITY_SCORE,
					FrameStatsBoard.SIM_REPLICATION_ENTITY_SORT,
					FrameStatsBoard.SIM_REPLICATION_ENTITY_BUDGET]},
	{"id": "replication_encode", "label": "Frame serialization", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_ENCODE},
	{"id": "replication_enqueue", "label": "Transport enqueue", "depth": 8,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_REPLICATION_ENQUEUE},
	{"id": "replication_fan_unattributed", "label": "Frame-fan unattributed", "depth": 8,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_REPLICATION_FAN,
			"minus": [FrameStatsBoard.SIM_REPLICATION_FAN_SETUP,
					FrameStatsBoard.SIM_REPLICATION_ROUNDS,
					FrameStatsBoard.SIM_REPLICATION_ENTITIES,
					FrameStatsBoard.SIM_REPLICATION_ENCODE,
					FrameStatsBoard.SIM_REPLICATION_ENQUEUE]},
	{"id": "replication_unattributed", "label": "Replication unattributed", "depth": 7,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_SERVER_REPLICATION,
			"minus": [FrameStatsBoard.SIM_REPLICATION_QUERY_PREP,
					FrameStatsBoard.SIM_REPLICATION_SNAPSHOT,
					FrameStatsBoard.SIM_REPLICATION_FAN]},
	{"id": "server_unattributed", "label": "Server-tick unattributed", "depth": 6,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_SERVER_TICK,
			"minus": [FrameStatsBoard.SIM_SERVER_INPUT, FrameStatsBoard.SIM_SERVER_WORLD,
					FrameStatsBoard.SIM_MATCH, FrameStatsBoard.SIM_SERVER_RULES,
					FrameStatsBoard.SIM_SERVER_REPLICATION]},
	{"id": "host_send", "label": "Send/flush", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_HOST_SEND},
	{"id": "host_unattributed", "label": "Host-pump unattributed", "depth": 5,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_HOST_PUMP,
			"minus": [FrameStatsBoard.SIM_HOST_RECEIVE,
					FrameStatsBoard.SIM_HOST_CONNECTIONS, FrameStatsBoard.SIM_HOST_ADAPTER,
					FrameStatsBoard.SIM_SERVER_TICK, FrameStatsBoard.SIM_HOST_SEND]},
	{"id": "host_player", "label": "Local view/weapon devices", "depth": 4,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_HOST_PLAYER},
	{"id": "net", "label": "Client network/decode", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_NET},
	{"id": "client_setup", "label": "Client clock/setup", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_CLIENT_SETUP},
	{"id": "client_receive", "label": "Receive + state fold", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_CLIENT_RECEIVE},
	{"id": "client_maintenance", "label": "Replica maintenance/movers", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_CLIENT_MAINTENANCE},
	{"id": "client_send", "label": "Joiner C2S build/send", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_CLIENT_SEND},
	{"id": "client_unattributed", "label": "Client-frame unattributed", "depth": 5,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_NET,
			"minus": [FrameStatsBoard.SIM_CLIENT_SETUP,
					FrameStatsBoard.SIM_CLIENT_RECEIVE,
					FrameStatsBoard.SIM_CLIENT_MAINTENANCE,
					FrameStatsBoard.SIM_CLIENT_SEND]},
	{"id": "adm_resolve", "label": "Animation registry resolve", "depth": 4,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.SIM_ADM_RESOLVE},
	{"id": "sim_unattributed", "label": "Sim-step unattributed", "depth": 4,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.SIM_STEP,
			"minus": [FrameStatsBoard.SIM_HOST_PREP, FrameStatsBoard.SIM_HOST_PUMP,
					FrameStatsBoard.SIM_HOST_PLAYER, FrameStatsBoard.SIM_NET,
					FrameStatsBoard.SIM_ADM_RESOLVE]},
	{"id": "trace", "label": "Projectile trace (attributed)", "depth": 4,
			"kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.TRACE_TERRAIN, FrameStatsBoard.TRACE_STATIC,
					FrameStatsBoard.TRACE_DYNAMIC, FrameStatsBoard.TRACE_PERSON]},
	{"id": "trace_terrain", "label": "Terrain", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.TRACE_TERRAIN},
	{"id": "trace_static", "label": "Static", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.TRACE_STATIC},
	{"id": "trace_dynamic", "label": "Dynamic", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.TRACE_DYNAMIC},
	{"id": "trace_person", "label": "Person", "depth": 5, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.TRACE_PERSON},
	{"id": "effects_drain", "label": "Effects drain", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.EFFECTS_DRAIN},
	{"id": "sim_sink", "label": "Godot tick sink", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.SIM_SINK},
	{"id": "effects", "label": "Effects tick", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.EFFECTS_TICK},
	{"id": "present", "label": "Present", "depth": 3, "kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.PRESENT_SNAPSHOT, FrameStatsBoard.PRESENT_MISSION,
					FrameStatsBoard.PRESENT_WIRE, FrameStatsBoard.PRESENT_FIRE,
					FrameStatsBoard.PRESENT_DESTRUCTION, FrameStatsBoard.PRESENT_THROWABLE,
					FrameStatsBoard.PRESENT_SCARS]},
	{"id": "snapshot", "label": "Row snapshot", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_SNAPSHOT},
	{"id": "mission_rows", "label": "Mission rows", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_MISSION},
	{"id": "mission_rows_core", "label": "Row scan/transform/submission", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.PRESENT_MISSION_CORE},
	{"id": "mission_rows_aim", "label": "Aim overlay", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.PRESENT_MISSION_AIM},
	{"id": "mission_rows_controls", "label": "Control publication", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.PRESENT_MISSION_CONTROLS},
	{"id": "mission_rows_visibility", "label": "Visibility/section masks", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.PRESENT_MISSION_VISIBILITY},
	{"id": "mission_rows_body", "label": "Body pose", "depth": 5,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.PRESENT_MISSION_BODY},
	{"id": "mission_rows_remainder", "label": "Mission-row orchestration remainder",
			"depth": 5, "kind": _KIND_RESIDUAL,
			"base": FrameStatsBoard.PRESENT_MISSION,
			"minus": [FrameStatsBoard.PRESENT_MISSION_CORE,
					FrameStatsBoard.PRESENT_MISSION_AIM,
					FrameStatsBoard.PRESENT_MISSION_CONTROLS,
					FrameStatsBoard.PRESENT_MISSION_VISIBILITY,
					FrameStatsBoard.PRESENT_MISSION_BODY]},
	{"id": "wire_rows", "label": "Wire rows", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_WIRE},
	{"id": "fire", "label": "Fire", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_FIRE},
	{"id": "destruction", "label": "Destruction", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_DESTRUCTION},
	{"id": "throwable", "label": "Throwable", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_THROWABLE},
	{"id": "scars", "label": "Scars", "depth": 4, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.PRESENT_SCARS},
	{"id": "runtime_overhead", "label": "Session/presentation overhead", "depth": 3,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.WORLD_RUNTIME,
			"minus": [FrameStatsBoard.SIM_STEP, FrameStatsBoard.SIM_SINK,
					FrameStatsBoard.PRESENT_SNAPSHOT,
					FrameStatsBoard.PRESENT_MISSION, FrameStatsBoard.PRESENT_WIRE,
					FrameStatsBoard.PRESENT_FIRE,
					FrameStatsBoard.PRESENT_DESTRUCTION,
					FrameStatsBoard.PRESENT_THROWABLE,
					FrameStatsBoard.PRESENT_SCARS]},
	{"id": "local_view", "label": "Local view publication", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_LOCAL_VIEW},
	{"id": "framefx", "label": "Auxiliary-view pose sync", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_FRAMEFX},
	{"id": "scene_env", "label": "Scene fog/ambient publication", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_SCENE_ENV},
	{"id": "terrain", "label": "Terrain frame publication", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_TERRAIN},
	{"id": "foliage", "label": "Foliage", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.WORLD_FOLIAGE},
	{"id": "network_frame", "label": "Network session devices", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_NETWORK_FRAME},
	{"id": "occl", "label": "Occlusion", "depth": 2, "kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.OCCL_BUILD, FrameStatsBoard.OCCL_PROBE,
					FrameStatsBoard.OCCL_APPLY, FrameStatsBoard.OCCL_GLUE]},
	{"id": "occl_build", "label": "Build (native)", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.OCCL_BUILD},
	{"id": "occl_probe", "label": "Probe (native)", "depth": 3, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.OCCL_PROBE},
	{"id": "occl_glue", "label": "Native call setup/binding", "depth": 3,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.OCCL_GLUE},
	{"id": "occl_apply", "label": "Visibility/lighting publication", "depth": 3,
			"kind": _KIND_SPAN,
			"slot": FrameStatsBoard.OCCL_APPLY},
	{"id": "occl_building_query", "label": "Building delta query", "depth": 4,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.OCCL_BUILDING_QUERY},
	{"id": "occl_building_apply", "label": "Building node writes", "depth": 4,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.OCCL_BUILDING_APPLY},
	{"id": "occl_cull_query", "label": "Entity-cull delta query", "depth": 4,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.OCCL_CULL_QUERY},
	{"id": "occl_cull_apply", "label": "Entity-cull node writes", "depth": 4,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.OCCL_CULL_APPLY},
	{"id": "occl_light_query", "label": "Sun-visibility query", "depth": 4,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.OCCL_LIGHT_QUERY},
	{"id": "occl_light_apply", "label": "Lighting-context writes", "depth": 4,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.OCCL_LIGHT_APPLY},
	{"id": "occl_water_apply", "label": "Water visibility write", "depth": 4,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.OCCL_WATER_APPLY},
	{"id": "occl_apply_remainder", "label": "Publication overhead", "depth": 4,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.OCCL_APPLY,
			"minus": [FrameStatsBoard.OCCL_BUILDING_QUERY,
					FrameStatsBoard.OCCL_BUILDING_APPLY,
					FrameStatsBoard.OCCL_CULL_QUERY,
					FrameStatsBoard.OCCL_CULL_APPLY,
					FrameStatsBoard.OCCL_LIGHT_QUERY,
					FrameStatsBoard.OCCL_LIGHT_APPLY,
					FrameStatsBoard.OCCL_WATER_APPLY]},
	{"id": "env", "label": "Env (weather/blink/iris)", "depth": 2, "kind": _KIND_GROUP,
			"slots": [FrameStatsBoard.WORLD_WEATHER, FrameStatsBoard.WORLD_BLINK,
					FrameStatsBoard.WORLD_IRIS]},
	{"id": "env_nodes", "label": "Sky/celestial/sun/weather smoothing", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_ENV_NODES},
	{"id": "water", "label": "Water strip + mirror camera", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_WATER},
	{"id": "sun_veil", "label": "Sun-veil publication", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_SUN_VEIL},
	{"id": "light", "label": "Point-light selection/publication", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_LIGHT},
	{"id": "material", "label": "Model runtime", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_MATERIAL},
	{"id": "model_clock_animation", "label": "Clocks + animation", "depth": 3,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.MODEL_CLOCK_ANIMATION},
	{"id": "model_panm", "label": "PANM transforms", "depth": 3,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.MODEL_PANM},
	{"id": "model_material", "label": "Material generators/textures", "depth": 3,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.MODEL_MATERIAL},
	{"id": "model_order_bounds", "label": "Render order + bounds", "depth": 3,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.MODEL_ORDER_BOUNDS},
	{"id": "model_runtime_remainder", "label": "Awake-set/gating remainder", "depth": 3,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.WORLD_MATERIAL,
			"minus": [FrameStatsBoard.MODEL_CLOCK_ANIMATION,
					FrameStatsBoard.MODEL_PANM, FrameStatsBoard.MODEL_MATERIAL,
					FrameStatsBoard.MODEL_ORDER_BOUNDS]},
	{"id": "slot_shadow", "label": "Slot-shadow planning/publication", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_SLOT_SHADOW},
	{"id": "particles", "label": "Particle-frame publication", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_PARTICLES},
	{"id": "audio", "label": "Audio", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.WORLD_AUDIO},
	{"id": "clear", "label": "Frame clear-color publication", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_CLEAR},
	{"id": "env_cube", "label": "Environment-cube submission", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.WORLD_ENV_CUBE},
	{"id": "world_remainder", "label": "World orchestration remainder", "depth": 2,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.FRAME_WORLD,
			"minus": [FrameStatsBoard.WORLD_RUNTIME,
					FrameStatsBoard.WORLD_LOCAL_VIEW, FrameStatsBoard.WORLD_FRAMEFX,
					FrameStatsBoard.WORLD_SCENE_ENV, FrameStatsBoard.WORLD_ENV_NODES,
					FrameStatsBoard.WORLD_WATER, FrameStatsBoard.WORLD_TERRAIN,
					FrameStatsBoard.WORLD_FOLIAGE,
					FrameStatsBoard.WORLD_NETWORK_FRAME,
					FrameStatsBoard.WORLD_WEATHER, FrameStatsBoard.WORLD_BLINK,
					FrameStatsBoard.OCCL_BUILD, FrameStatsBoard.OCCL_PROBE,
					FrameStatsBoard.OCCL_APPLY, FrameStatsBoard.OCCL_GLUE,
					FrameStatsBoard.WORLD_IRIS, FrameStatsBoard.WORLD_SUN_VEIL,
					FrameStatsBoard.WORLD_LIGHT, FrameStatsBoard.WORLD_MATERIAL,
					FrameStatsBoard.WORLD_SLOT_SHADOW,
					FrameStatsBoard.WORLD_PARTICLES, FrameStatsBoard.WORLD_AUDIO,
					FrameStatsBoard.WORLD_CLEAR, FrameStatsBoard.WORLD_ENV_CUBE]},
	{"id": "after", "label": "Player presenter (post)", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_PLAYER_AFTER},
	{"id": "hud", "label": "HUD tick", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.FRAME_HUD},
	{"id": "hud_scalars", "label": "Scalars", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_SCALARS},
	{"id": "hud_attach", "label": "Attach labels", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_ATTACH},
	{"id": "hud_waypoint", "label": "Waypoint", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_WAYPOINT},
	{"id": "hud_info", "label": "Info build", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_INFO},
	{"id": "hud_flush", "label": "Flush", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.HUD_FLUSH},
	{"id": "stats_sample", "label": "Stats sampling", "depth": 1,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_STATS_SAMPLE},
	{"id": "shell_control", "label": "Shell state/control", "depth": 1,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_SHELL_CONTROL},
	{"id": "round_flow", "label": "Round/session transitions", "depth": 1,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_ROUND_FLOW},
	{"id": "menu_shell", "label": "Menu shell process", "depth": 1,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_MENU_SHELL},
	{"id": "menu_video", "label": "Menu video decode/upload", "depth": 1,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_MENU_VIDEO},
	{"id": "debug_refresh", "label": "F3 refresh", "depth": 1,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_DEBUG_REFRESH},
	{"id": "frame_overhead", "label": "Frame overhead", "depth": 1,
			"kind": _KIND_HEADER},
	# Earliest-to-latest idle callback time less every callback owner above.
	# This is specifically other Nodes, not an engine/wait catch-all.
	{"id": "other_process", "label": "Other process callbacks", "depth": 2,
			"kind": _KIND_RESIDUAL,
			"base": FrameStatsBoard.FRAME_PROCESS_CALLBACKS,
			"minus": [FrameStatsBoard.FRAME_PLAYER_BEFORE, FrameStatsBoard.FRAME_WORLD,
					FrameStatsBoard.FRAME_PLAYER_AFTER, FrameStatsBoard.FRAME_HUD,
					FrameStatsBoard.FRAME_STATS_SAMPLE, FrameStatsBoard.FRAME_SHELL_CONTROL,
					FrameStatsBoard.FRAME_ROUND_FLOW, FrameStatsBoard.FRAME_MENU_SHELL,
					FrameStatsBoard.FRAME_MENU_VIDEO, FrameStatsBoard.FRAME_DEBUG_REFRESH]},
	{"id": "physics_callbacks", "label": "Physics callbacks", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_PHYSICS_CALLBACKS},
	# The engine time between host frames outside both Node callback windows,
	# split at RenderingServer's draw signals (RootFramePhaseSampler).
	{"id": "deferred_flush", "label": "Deferred flush (draw callbacks, transforms)",
			"depth": 2, "kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_DEFERRED_FLUSH},
	# HudOverlay._draw is the one every-frame _draw in a live mission.
	{"id": "hud_draw_compile", "label": "HUD draw compile", "depth": 3,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.HUD_DRAW_COMPILE},
	{"id": "hud_draw_emit", "label": "HUD draw emit", "depth": 3,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.HUD_DRAW_EMIT},
	{"id": "render_draw", "label": "RenderingServer draw (all viewports)",
			"depth": 2, "kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_DRAW},
	{"id": "pacing_input", "label": "Servers/input/pacing", "depth": 2,
			"kind": _KIND_SPAN, "slot": FrameStatsBoard.FRAME_PACING_INPUT},
	# Whatever the three spans above did not bracket (signal latency, the
	# first frame of a window).
	{"id": "engine_frame", "label": "Unattributed engine time", "depth": 2,
			"kind": _KIND_RESIDUAL, "base": FrameStatsBoard.FRAME_WALL,
			"minus": [FrameStatsBoard.FRAME_PROCESS_CALLBACKS,
					FrameStatsBoard.FRAME_PHYSICS_CALLBACKS,
					FrameStatsBoard.FRAME_DEFERRED_FLUSH,
					FrameStatsBoard.FRAME_DRAW,
					FrameStatsBoard.FRAME_PACING_INPUT]},
	{"id": "render", "label": "Render", "depth": 0, "kind": _KIND_HEADER},
	# Per-pass submission counts (info cells): what the main view, the shadow
	# maps, and the water mirror each rendered last frame — pass attribution is
	# read here, not inferred from the totals above.
	{"id": "render_main", "label": "Main view", "depth": 1, "kind": _KIND_HEADER},
	{"id": "render_shadow", "label": "Shadow passes", "depth": 1, "kind": _KIND_HEADER},
	{"id": "render_root_cpu", "label": "Viewport CPU", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_ROOT_CPU},
	{"id": "render_root_gpu", "label": "Viewport GPU", "depth": 1, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_ROOT_GPU},
	{"id": "render_water", "label": "Water mirror", "depth": 1, "kind": _KIND_HEADER},
	{"id": "render_water_cpu", "label": "Water RTT CPU", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_WATER_CPU},
	{"id": "render_water_gpu", "label": "Water RTT GPU", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_WATER_GPU},
	# The other per-frame scene renders: FrameFx's shared-world Q3 view (the
	# glow/envmap source) and the slot-shadow capture chain (only the slots
	# that rendered are counted). The first-person gun draws inside the main
	# view (its shader-side projection), so it has no pass of its own.
	{"id": "render_q3", "label": "FrameFX Q3 view", "depth": 1, "kind": _KIND_HEADER},
	{"id": "render_q3_cpu", "label": "Q3 CPU", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_Q3_CPU},
	{"id": "render_q3_gpu", "label": "Q3 GPU", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_Q3_GPU},
	{"id": "render_slot", "label": "Slot-shadow captures", "depth": 1, "kind": _KIND_HEADER},
	{"id": "render_slot_cpu", "label": "Captures CPU", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_SLOT_CPU},
	{"id": "render_slot_gpu", "label": "Captures GPU", "depth": 2, "kind": _KIND_SPAN,
			"slot": FrameStatsBoard.RENDER_SLOT_GPU},
]

var status_label: Label
var stats_tree: Tree

var _board: FrameStatsBoard = null
var _overlay_visible := false
var _capture_active := false
var _refresh_count := 0
var _items: Dictionary = {}  # row id -> TreeItem


func page_id() -> StringName:
	return &"Stats"


func page_category() -> StringName:
	return CATEGORY_DIAGNOSTICS


func _build() -> void:
	add_theme_constant_override("separation", 6)

	status_label = Label.new()
	status_label.name = "StatsStatus"
	status_label.text = "No frame stats source."
	status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(status_label)

	stats_tree = Tree.new()
	stats_tree.name = "StatsRows"
	stats_tree.columns = 4
	stats_tree.column_titles_visible = true
	stats_tree.set_column_title(0, "System")
	stats_tree.set_column_title(1, "Avg")
	stats_tree.set_column_title(2, "Peak")
	stats_tree.set_column_title(3, "Info")
	stats_tree.set_column_title_tooltip_text(0, "Runtime system or subsystem")
	stats_tree.set_column_title_tooltip_text(1, "Average milliseconds per frame")
	stats_tree.set_column_title_tooltip_text(2, "Peak milliseconds in one frame")
	stats_tree.set_column_title_tooltip_text(3, "Live counters and runtime context")
	# Keep deep span nesting useful in the narrow dock: every cell clips with an
	# ellipsis, while row tooltips below retain the complete label and info.
	stats_tree.scroll_horizontal_enabled = false
	stats_tree.add_theme_constant_override("item_margin", 10)
	for column in range(stats_tree.columns):
		stats_tree.set_column_clip_content(column, true)
	# The 232 px floor leaves room for the Tree frame inside the 252 px page.
	# System and Info share wider docks; the numeric columns stay compact.
	stats_tree.set_column_expand(0, true)
	stats_tree.set_column_expand_ratio(0, 3)
	stats_tree.set_column_custom_minimum_width(0, 92)
	for column in [1, 2]:
		stats_tree.set_column_expand(column, false)
		stats_tree.set_column_custom_minimum_width(column, 42)
		stats_tree.set_column_title_alignment(column, HORIZONTAL_ALIGNMENT_RIGHT)
	stats_tree.set_column_expand(3, true)
	stats_tree.set_column_expand_ratio(3, 2)
	stats_tree.set_column_custom_minimum_width(3, 56)
	stats_tree.hide_root = true
	stats_tree.focus_mode = Control.FOCUS_ALL
	stats_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(stats_tree)
	_build_rows()


func set_frame_stats_board(board: FrameStatsBoard) -> void:
	if board == _board:
		return
	if _board != null:
		_board.set_capture_active(false)
	_board = board
	_capture_active = false
	_refresh_count = 0
	status_label.text = "No frame stats source." if board == null else "Stats capture paused."
	_sync_capture()


## The overlay's explicit visibility edge: Controls under a hidden CanvasLayer
## don't all observe the layer hide, so the overlay tells us on toggle.
func set_capture_active(overlay_visible: bool) -> void:
	_overlay_visible = overlay_visible
	_sync_capture()


func _notification(what: int) -> void:
	if what == NOTIFICATION_EXIT_TREE:
		_overlay_visible = false
		if _board != null:
			_board.set_capture_active(false)
		_capture_active = false
		return
	# Tab switches flip this Control's own visibility.
	if what == NOTIFICATION_VISIBILITY_CHANGED:
		_sync_capture()


func is_capturing() -> bool:
	return _capture_active


func _sync_capture() -> void:
	var want := _board != null and _overlay_visible and visible and is_inside_tree()
	if want == _capture_active \
			and (_board == null or _board.is_capture_active() == want):
		return
	_capture_active = want
	_refresh_count = 0
	if _board != null:
		_board.set_capture_active(want)
	if want:
		status_label.text = "Capturing…"
		_clear_display_values()
	elif _board != null:
		status_label.text = "Stats capture paused."


## One overlay-cadence refresh. Reads the window only every _REFRESH_DIVIDER
## calls so displayed means cover ~0.5 s of frames.
func refresh() -> void:
	_sync_capture()
	if _board == null:
		status_label.text = "No frame stats source."
		return
	if not _capture_active:
		return
	_refresh_count += 1
	if _refresh_count % _REFRESH_DIVIDER != 0:
		return
	var window := _board.drain()
	if window.frames <= 0:
		return
	status_label.text = "Captured %d render frames; overlay cost is included." % window.frames
	var runtime := _ctx.runtime() if _ctx != null else null
	var sim := _ctx.sim() if _ctx != null else null
	render_window(window.frames, window.sums, window.peaks,
			window.sample_frames, runtime, sim)


## Stable, value-only observation seam for tests and automated probes.
func get_display_snapshot() -> Array[DebugStatsDisplayRow]:
	var out: Array[DebugStatsDisplayRow] = []
	for row_v in _ROWS:
		var row := row_v as Dictionary
		var item := _items[row["id"]] as TreeItem
		out.append(DebugStatsDisplayRow.new(
				StringName(row["id"]), String(row["label"]),
				item.get_text(1), item.get_text(2), item.get_text(3)))
	return out


## Format one drained window into the rows. Split from refresh() so tests can
## drive the pane with fabricated windows.
func render_window(frames: int, sums: PackedInt64Array, maxes: PackedInt64Array,
		counts: PackedInt32Array, runtime: MissionPresentation, sim: Simulation) -> void:
	for row_v in _ROWS:
		var row: Dictionary = row_v
		var item: TreeItem = _items[row["id"]]
		match int(row["kind"]):
			_KIND_SPAN:
				var slot := int(row["slot"])
				if counts[slot] <= 0:
					_set_metric(item, 1, "-")
					_set_metric(item, 2, "-")
				else:
					_set_metric(item, 1,
							"%.2f" % (float(sums[slot]) / 1000.0 / frames))
					_set_metric(item, 2,
							"%.2f" % (float(maxes[slot]) / 1000.0))
			_KIND_GROUP:
				var total := 0
				var seen := false
				for slot_v in row["slots"]:
					var slot := int(slot_v)
					total += sums[slot]
					seen = seen or counts[slot] > 0
				_set_metric(item, 1,
						"%.2f" % (float(total) / 1000.0 / frames) if seen else "-")
				_set_metric(item, 2, "")
			_KIND_RESIDUAL:
				var base_slot := int(row["base"])
				if counts[base_slot] <= 0:
					_set_metric(item, 1, "-")
				else:
					var residual := int(sums[base_slot])
					for slot_v in row["minus"]:
						residual -= sums[int(slot_v)]
					_set_metric(item, 1, "%.2f" %
							(float(maxi(residual, 0)) / 1000.0 / frames))
				_set_metric(item, 2, "")
			_:
				pass
	_refresh_info(sums, counts, frames, runtime, sim)


func _build_rows() -> void:
	stats_tree.clear()
	_items.clear()
	var root := stats_tree.create_item()
	var stack: Array = [root]
	for row_v in _ROWS:
		var row: Dictionary = row_v
		var depth := int(row["depth"])
		while stack.size() > depth + 1:
			stack.pop_back()
		var item := stats_tree.create_item(stack.back())
		var label := String(row["label"])
		item.set_text(0, label)
		item.set_tooltip_text(0, label)
		item.set_text_overrun_behavior(0, TextServer.OVERRUN_TRIM_ELLIPSIS)
		item.set_text_overrun_behavior(3, TextServer.OVERRUN_TRIM_ELLIPSIS)
		_set_metric(item, 1, "-")
		_set_metric(item, 2, "-")
		for column in [1, 2]:
			item.set_text_alignment(column, HORIZONTAL_ALIGNMENT_RIGHT)
		_items[row["id"]] = item
		stack.push_back(item)


func _clear_display_values() -> void:
	for item_v in _items.values():
		var item := item_v as TreeItem
		_set_metric(item, 1, "-")
		_set_metric(item, 2, "-")
		item.set_text(3, "")
		item.set_tooltip_text(3, "")


func _set_metric(item: TreeItem, column: int, text: String) -> void:
	item.set_text(column, text)
	item.set_tooltip_text(column, text)


func _set_info(id: String, text: String) -> void:
	var item := _items[id] as TreeItem
	item.set_text(3, text)
	item.set_tooltip_text(3, text)


# The counter pulls: live Dictionaries/typed stats read at refresh cadence
# only, every source optional (null when its mission-scoped owner is gone) so
# SP, listen-host and joiner sessions all render what they have.
func _refresh_info(sums: PackedInt64Array, counts: PackedInt32Array, frames: int,
		runtime: MissionPresentation, sim: Simulation) -> void:
	# Sources are mission-scoped and can disappear between divided refreshes.
	# Clear every conditional cell first so reload/menu transitions cannot retain
	# counters from the previous world.
	for id in ["sim", "net", "trace", "effects", "fire", "destruction",
			"throwable", "mission_rows", "wire_rows", "occl", "material",
			"render_main", "render_shadow", "render_water", "render_q3",
			"render_slot", "physics_callbacks"]:
		_set_info(id, "")
	var frame_info := "%d fps" % int(Performance.get_monitor(Performance.TIME_FPS))
	# Godot's own TIME_PROCESS (process + deferred flush + RS sync + draw):
	# the cross-check for the callback + flush + draw rows above.
	if frames > 0 and counts[FrameStatsBoard.FRAME_TIME_PROCESS] > 0:
		frame_info += " · process %.2f ms" % (
				float(sums[FrameStatsBoard.FRAME_TIME_PROCESS]) / 1000.0 / frames)
	_set_info("frame", frame_info)
	if frames > 0 and counts[FrameStatsBoard.FRAME_PHYSICS_ITERATIONS] > 0:
		# The servers' window is a per-frame max, the callbacks a sum: the
		# difference bounds the empty-space step tax, it is not a measurement.
		_set_info("physics_callbacks", "server max %.2f ms · %.1f iter/f" % [
				float(sums[FrameStatsBoard.FRAME_PHYSICS_SERVER]) / 1000.0 / frames,
				float(sums[FrameStatsBoard.FRAME_PHYSICS_ITERATIONS]) / frames])
	_set_info("render", "%d draws · %d objs · %s prims · %d nodes" % [
		int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
		int(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME)),
		_compact_count(int(Performance.get_monitor(
				Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))),
		int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)),
	])
	_set_pass_counts("render_main", sums, counts, frames,
			FrameStatsBoard.RENDER_MAIN_OBJECTS, FrameStatsBoard.RENDER_MAIN_DRAWS)
	_set_pass_counts("render_shadow", sums, counts, frames,
			FrameStatsBoard.RENDER_SHADOW_OBJECTS, FrameStatsBoard.RENDER_SHADOW_DRAWS)
	_set_pass_counts("render_water", sums, counts, frames,
			FrameStatsBoard.RENDER_WATER_OBJECTS, FrameStatsBoard.RENDER_WATER_DRAWS)
	_set_pass_counts("render_q3", sums, counts, frames,
			FrameStatsBoard.RENDER_Q3_OBJECTS, FrameStatsBoard.RENDER_Q3_DRAWS)
	if frames > 0 and counts[FrameStatsBoard.RENDER_SLOT_VIEWPORTS] > 0:
		_set_info("render_slot", "%d objs · %d draws · %.1f captures/f" % [
			int(float(sums[FrameStatsBoard.RENDER_SLOT_OBJECTS]) / frames),
			int(float(sums[FrameStatsBoard.RENDER_SLOT_DRAWS]) / frames),
			float(sums[FrameStatsBoard.RENDER_SLOT_VIEWPORTS]) / frames,
		])
	if counts[FrameStatsBoard.MODEL_AWAKE_MODELS] > 0:
		var model_samples := counts[FrameStatsBoard.MODEL_AWAKE_MODELS]
		_set_info("material", "%d awake · %d renderable" % [
				roundi(float(sums[FrameStatsBoard.MODEL_AWAKE_MODELS]) / model_samples),
				roundi(float(sums[FrameStatsBoard.MODEL_RENDERABLE_MODELS]) / model_samples),
		])
	if frames > 0 and counts[FrameStatsBoard.PRESENT_MISSION_ROWS] > 0:
		_set_info("mission_rows", "%d rows · %d submitted · %d body" % [
				roundi(float(sums[FrameStatsBoard.PRESENT_MISSION_ROWS]) / frames),
				roundi(float(sums[FrameStatsBoard.PRESENT_MISSION_SUBMITTED_ROWS]) / frames),
				roundi(float(sums[FrameStatsBoard.PRESENT_MISSION_BODY_ROWS]) / frames),
		])

	var world := _ctx.world() if _ctx != null else null

	# Sim row: ticks/frame from the value slot + entity/role counters.
	var sim_info := ""
	if counts[FrameStatsBoard.SIM_TICKS] > 0:
		sim_info = "%.1f t/f" % (float(sums[FrameStatsBoard.SIM_TICKS]) / frames)
	if world != null:
		var wc: Dictionary = world.get_runtime_perf_counters()
		var simc: Dictionary = (wc.get("runtime", {}) as Dictionary).get("sim", {})
		if not simc.is_empty():
			var role := "local"
			if bool(simc.get("listen_server", false)):
				role = "listen"
			elif sim != null and bool(sim.is_joiner()):
				role = "joiner"
			sim_info += "%s%d ents · %s" % [
				"" if sim_info.is_empty() else " · ",
				int(simc.get("present_entity_count", 0)), role]
	_set_info("sim", sim_info)

	var net_info := ""
	if sim != null:
		var peers := int(sim.get_host_peer_count())
		if peers > 0:
			net_info = "%d peer(s)" % peers
	_set_info("net", net_info)

	if counts[FrameStatsBoard.TRACE_CALLS] > 0:
		_set_info("trace",
				"%.1f calls/f · S %.1f D %.1f P %.1f surv/f · %.1f/%.1f faces/f" % [
					float(sums[FrameStatsBoard.TRACE_CALLS]) / frames,
					float(sums[FrameStatsBoard.TRACE_STATIC_SURVIVORS]) / frames,
					float(sums[FrameStatsBoard.TRACE_DYNAMIC_SURVIVORS]) / frames,
					float(sums[FrameStatsBoard.TRACE_PERSON_SURVIVORS]) / frames,
					float(sums[FrameStatsBoard.TRACE_STATIC_FACES]) / frames,
					float(sums[FrameStatsBoard.TRACE_DYNAMIC_FACES]) / frames,
				])

	if world != null:
		var fx := world.get_effect_world()
		if fx != null and is_instance_valid(fx):
			var drain_ms := float(sums[FrameStatsBoard.EFFECTS_DRAIN]) / 1000.0 / frames
			_set_info("effects", "%d live · drain %.2f ms/f" % [
					int(fx.active_entry_count()), drain_ms])

	if world != null:
		var fire = world.get_fire_present_stats()
		if fire != null:
			_set_info("fire", "%d fires · %d snd" % [
					int(fire.fires), int(fire.sounds)])

	if world != null:
		var destruction = world.get_destruction_present_stats()
		if destruction != null:
			_set_info("destruction", "%d husks · %d debris · %d glass" % [
					int(destruction.husk_swaps),
					int(destruction.debris_triangles),
					int(destruction.glass_points)])

	if runtime != null:
		var throwable = runtime.get_throwable_present_stats()
		if throwable != null:
			_set_info("throwable", "%d live" % int(throwable.live))

	if runtime != null:
		var wire: WirePresentStats = runtime.get_wire_present_stats()
		if wire != null and (wire.live > 0 or wire.unresolved > 0):
			_set_info("wire_rows", "%d live · %d unresolved" % [
					wire.live, wire.unresolved])

	if sim != null:
		var occ: Dictionary = sim.get_occlusion_debug()
		if bool(occ.get("active", false)):
			var occ_counts: Dictionary = occ.get("counts", {})
			_set_info("occl", "%d bld · %d drawn · %d ent culled" % [
					int(occ_counts.get("instances", 0)),
					int(occ_counts.get("visible", 0)),
					int(occ_counts.get("culled_entities", 0))])


# One pass-count info cell: window-averaged objects/draws submitted per frame
# by that render pass. A pass that never sampled (no water, capture just
# opened) keeps its cleared cell.
func _set_pass_counts(id: String, sums: PackedInt64Array,
		counts: PackedInt32Array, frames: int, objects_slot: int,
		draws_slot: int) -> void:
	if frames <= 0 or counts[objects_slot] <= 0:
		return
	_set_info(id, "%d objs · %d draws" % [
		int(float(sums[objects_slot]) / frames),
		int(float(sums[draws_slot]) / frames),
	])


static func _compact_count(value: int) -> String:
	if value >= 1_000_000:
		return "%.1fM" % (value / 1_000_000.0)
	if value >= 10_000:
		return "%dk" % int(value / 1000.0)
	return str(value)
