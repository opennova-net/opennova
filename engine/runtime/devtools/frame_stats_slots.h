// The frame-stats slot table: one entry per span or value the shell and the
// engine measure per render frame (ADR 0039). Transcribed from the retired
// GDScript FrameStatsBoard enum; the trailing text is each slot's description.
// Times are microseconds unless the description says VALUE (a plain count).
//
// One list, three consumers: the engine enum below, the FrameStats GDExtension
// enum constants (godot/src/devtools/frame_stats.cpp) and the Stats window
// rows (stats_window_rows.h). Add a slot here and every consumer sees it.
#pragma once

// X(NAME, DESCRIPTION)
#define OPENNOVA_FRAME_STATS_SLOTS(X) \
    /* main_game frame legs (game shell _process) */ \
    X(FRAME_WALL, "true wall time between consecutive shell frames") \
    X(FRAME_PLAYER_BEFORE, "LocalPlayerPresenter.before_world_tick") \
    X(FRAME_WORLD, "GameWorld.tick total") \
    X(FRAME_PLAYER_AFTER, "LocalPlayerPresenter.after_world_tick") \
    X(FRAME_HUD, "GameHudPresenter.tick total") \
    X(FRAME_STATS_SAMPLE, "root render counters + menu-video timing collection") \
    X(FRAME_SHELL_CONTROL, "shell state/input/end-screen preamble before gameplay devices") \
    X(FRAME_ROUND_FLOW, "post-HUD round-cycle/session transition checks") \
    X(FRAME_MENU_SHELL, "visible MenuShell driver + portrait model frame") \
    X(FRAME_MENU_VIDEO, "native Bink decode + texture upload") \
    X(FRAME_DEBUG_REFRESH, "the dev tools' cost on the frames they are open: the ImGui layout pass, the request drains and the record pushes") \
    X(FRAME_PROCESS_CALLBACKS, "earliest-to-latest idle Node callback window") \
    X(FRAME_PHYSICS_CALLBACKS, "summed earliest-to-latest physics callback windows") \
    /* The engine time outside every Node callback, split at Godot's draw */ \
    /* signals (RootFramePhaseSampler): what used to be one residual row. */ \
    X(FRAME_DEFERRED_FLUSH, "latest idle callback -> frame_pre_draw: MessageQueue flush (call_deferred, queue_redraw -> _draw), transform flush, SceneTree tail, RS sync") \
    /* The flush split at the deferred marker the late boundary queues (MessageQueue is FIFO): */ \
    X(FRAME_FLUSH_QUEUED, "latest idle callback -> the marker: _flush_ugc + every call_deferred / queue_redraw -> _draw the callbacks queued (HUD overlay, view effects)") \
    X(FRAME_FLUSH_TAIL, "the marker -> frame_pre_draw: draws/sorts the flush itself queued, transform notifications, timers/tweens, node frees, accessibility, RS sync") \
    X(FRAME_NODES_FREED, "VALUE: SceneTree nodes gone since the previous frame (the delete-queue flush)") \
    X(FRAME_NODES_ADDED, "VALUE: SceneTree nodes added since the previous frame") \
    X(FRAME_DRAW, "frame_pre_draw -> frame_post_draw: RenderingServer.draw for every viewport (cull, draw lists, submit, present)") \
    X(FRAME_PACING_INPUT, "frame_post_draw -> next earliest idle callback: audio/script frame hooks, input pump, physics servers, SceneTree head") \
    X(FRAME_TIME_PROCESS, "VALUE (us): Performance.TIME_PROCESS (process + flush + sync + draw); Godot publishes it once per second as that second's worst iteration, so read the PEAK, never the mean") \
    X(FRAME_PHYSICS_SERVER, "VALUE (us): Performance.TIME_PHYSICS_PROCESS (physics servers' window), the same once-per-second peak") \
    X(FRAME_PHYSICS_ITERATIONS, "VALUE: physics iterations run before this render frame") \
    /* GameWorld.tick legs */ \
    X(WORLD_FOLIAGE, "") \
    X(WORLD_RUNTIME, "") \
    X(WORLD_WEATHER, "") \
    X(WORLD_BLINK, "") \
    X(WORLD_IRIS, "") \
    X(WORLD_AUDIO, "") \
    X(WORLD_LOCAL_VIEW, "local-player camera/viewmodel publication") \
    X(WORLD_FRAMEFX, "focused Q3 compile (typed draw list for the terminal compositor)") \
    X(WORLD_SCENE_ENV, "render-eye fog/ambient + water classifier") \
    X(WORLD_ENV_NODES, "weather smoothing, sun direction, sky dome, celestial bodies") \
    X(WORLD_WATER, "water strip march + mirror camera") \
    X(WORLD_TERRAIN, "terrain draw-list and MATCHTERRAIN publication") \
    X(WORLD_NETWORK_FRAME, "net-session edge observation/environment apply") \
    X(WORLD_SUN_VEIL, "celestial exposure feed") \
    X(WORLD_LIGHT, "point-light selection and terrain/shadow context") \
    X(WORLD_MATERIAL, "awake ObjectModel runtime/material/animation advance") \
    X(MODEL_CLOCK_ANIMATION, "native model clocks + part/body animation") \
    X(MODEL_PANM, "native PANM transform evaluation/publication") \
    X(MODEL_MATERIAL, "native dynamic material generator/texture writes") \
    X(MODEL_ORDER_BOUNDS, "alpha-strip ordering + changed model bounds") \
    X(MODEL_AWAKE_MODELS, "VALUE: models visited by the shared awake walk") \
    X(MODEL_RENDERABLE_MODELS, "VALUE: visited models currently camera-submitted") \
    X(WORLD_SLOT_SHADOW, "render-slot ground-shadow planning/publication") \
    X(WORLD_PARTICLES, "EffectWorld render-frame publication") \
    X(WORLD_CLEAR, "viewport clear-color publication") \
    X(WORLD_ENV_CUBE, "environment-cube update submission") \
    /* The render-occlusion frame, split native collection from shell publication. */ \
    X(OCCL_BUILD, "native OcclusionWorld::build_frame (portal walk)") \
    X(OCCL_PROBE, "native per-entity render-gate loop") \
    X(OCCL_APPLY, "complete post-run visibility/lighting publication") \
    X(OCCL_GLUE, "run_occlusion_frame call minus build+probe (marshalling)") \
    X(OCCL_BUILDING_QUERY, "native building visibility-delta build + marshalling") \
    X(OCCL_BUILDING_APPLY, "section-mask and building visibility node writes") \
    X(OCCL_CULL_QUERY, "native entity-cull delta build + marshalling") \
    X(OCCL_CULL_APPLY, "entity-cull visibility node writes") \
    X(OCCL_LIGHT_QUERY, "native per-drawn-entity sun-visibility queries") \
    X(OCCL_LIGHT_APPLY, "placed/wire lighting-context writes") \
    X(OCCL_WATER_APPLY, "final blink-water visibility write") \
    /* MissionRoot legs */ \
    X(SIM_STEP, "sim.step() total, summed over the frame's logic ticks") \
    X(SIM_NET, "native wire leg of step: the joiner's recv/uplink pump, or the host's local ClientState decode/fold") \
    X(SIM_HOST_PREP, "viewport/input/request setup before the portable host pump") \
    X(SIM_HOST_PUMP, "complete inmatch host owner iteration") \
    X(SIM_HOST_RECEIVE, "recv drain + missing-sequence service") \
    X(SIM_HOST_CONNECTIONS, "connection/spawn service") \
    X(SIM_HOST_ADAPTER, "binding callback at the pre-server registration seam") \
    X(SIM_SERVER_TICK, "authoritative C2S -> world -> rules -> replication loop") \
    X(SIM_SERVER_INPUT, "C2S apply + human/breath prepass") \
    X(SIM_SERVER_WORLD, "World::run_logic_tick total") \
    X(SIM_WORLD_SETUP, "per-tick shared state/fire-sound setup") \
    X(SIM_WORLD_SCRIPTS, "registered authored systems (WAC + BMS)") \
    /* World::update_all_entities, lapped in retail order (Entity_UpdateAllEntities). */ \
    X(SIM_UPDATE_ENTITIES, "World::update_all_entities total, every lap below in retail order") \
    X(SIM_UPDATE_WALKS, "the pool-1 slot walk (brains, vehicle motors, ewep, items) + the pool-0 organic walk") \
    X(SIM_AI_INFANTRY, "infantry/player body rows") \
    X(SIM_AI_INFANTRY_REMOTE, "authority animation/collision for remote players") \
    X(SIM_AI_INFANTRY_COMBAT, "NPC perception, reactions, and aim") \
    X(SIM_AI_INFANTRY_ANIMATION, "weapon/body channels + root-motion sampling") \
    X(SIM_AI_INFANTRY_COLLISION, "mounted and ordinary movement resolver calls") \
    X(SIM_AI_INFANTRY_COLLISION_CONTACTS, "candidate/model contact passes") \
    X(SIM_AI_INFANTRY_COLLISION_REPULSION, "person-sphere separation") \
    X(SIM_AI_INFANTRY_COLLISION_GROUND, "final terrain/model ground ray") \
    X(SIM_UPDATE_ATTACHMENTS, "emplacement attachment posing") \
    X(SIM_ATTACHMENT_ORPHANS, "dead-parent chain cleanup") \
    X(SIM_ATTACHMENT_CHILDREN, "child userpoint/root posing") \
    X(SIM_ATTACHMENT_RIDERS, "riders refreshed from attached children") \
    X(SIM_UPDATE_HELILIFT_FACES, "HeliLift update + facial interpolation") \
    X(SIM_UPDATE_PRECIPITATION, "precipitation fall tick") \
    X(SIM_UPDATE_PIECES_EVENTS, "death pieces + the timed AI event queue") \
    X(SIM_UPDATE_PROJECTILES, "rotor wash + projectile rounds") \
    X(SIM_UPDATE_EXPLOSIONS, "explosion queue, round-hit reactions, pool-2/3 cohort walks, doors") \
    X(SIM_UPDATE_PROXIMITY, "pool-0/1 proximity table rebuild") \
    X(SIM_WORLD_HOUSEKEEPING, "waypoint/recount/mailbox tail") \
    X(SIM_MATCH, "") \
    X(SIM_SERVER_RULES, "deaths, respawn, win/capture, maintenance events") \
    X(SIM_SERVER_REPLICATION, "snapshot + per-connection S2C fan + linger tail") \
    X(SIM_REPLICATION_QUERY_PREP, "stable post-movement solid candidate index build") \
    X(SIM_REPLICATION_QUERY_COLLECT, "effective-solid target/bound collection") \
    X(SIM_REPLICATION_QUERY_GRID, "stable spatial cell publication") \
    X(SIM_REPLICATION_QUERY_GRID_SPAN, "candidate -> covered-cell range calculation") \
    X(SIM_REPLICATION_QUERY_GRID_BUCKET, "dense/hash bucket clear/population") \
    X(SIM_REPLICATION_QUERY_GRID_WORKSPACE, "query marks/results workspace reset") \
    X(SIM_REPLICATION_SNAPSHOT, "registry -> wire snapshot build") \
    X(SIM_REPLICATION_FAN, "complete per-recipient S2C fan") \
    X(SIM_REPLICATION_FAN_SETUP, "recipient anchor + frame header setup") \
    X(SIM_REPLICATION_ROUNDS, "round-event selection") \
    X(SIM_REPLICATION_ENTITIES, "entity priority/budget selection") \
    X(SIM_REPLICATION_ENTITY_SETUP, "age/self anchor preparation") \
    X(SIM_REPLICATION_ENTITY_SCORE, "distance/view/LOS priority scoring") \
    X(SIM_REPLICATION_ENTITY_LOS, "collision-world LOS raycasts inside scoring") \
    X(SIM_REPLICATION_ENTITY_LOS_TERRAIN, "heightfield march") \
    X(SIM_REPLICATION_ENTITY_LOS_SECTOR, "static/dynamic solid walk") \
    X(SIM_REPLICATION_ENTITY_SORT, "descending priority ordering") \
    X(SIM_REPLICATION_ENTITY_BUDGET, "byte-budget selection + cache stamps") \
    X(SIM_REPLICATION_ENCODE, "selected 0x0A body serialization") \
    X(SIM_REPLICATION_ENQUEUE, "semantic transport enqueue") \
    X(SIM_HOST_SEND, "remote transport drain/frame/send") \
    X(SIM_CLIENT_SETUP, "client role clock + keepalive setup") \
    X(SIM_CLIENT_RECEIVE, "loopback/wire receive + state fold") \
    X(SIM_CLIENT_MAINTENANCE, "decoded-state timers and movers") \
    X(SIM_CLIENT_SEND, "joiner-only C2S build/frame") \
    /* The joiner frame after its wire leg (JoinerRole::pump phases). */ \
    X(SIM_CLIENT_MATERIALIZE, "joiner: stream materialize + decoded-state folds (spawn/health/mount/ammo/events/weather)") \
    X(SIM_CLIENT_MIRROR, "joiner: wire pose mirror into the registry (both passes + predicted vehicles)") \
    X(SIM_CLIENT_PROXIES, "joiner: wire collision proxy rebuild") \
    X(SIM_CLIENT_WORLD, "joiner: local World::run_logic_tick (its phases land on the World update rows)") \
    X(SIM_CLIENT_ATTACH, "joiner: remote attachment recompose + local seat re-pose") \
    X(SIM_CLIENT_PLAYER, "joiner: input/weather/heading/view/weapon device pumps") \
    /* The authority frame tail after the tick (the host and bare local roles). */ \
    X(SIM_PLAYER_TAIL, "authority frame tail: weather tick, local view, medic cooldown, reload relay") \
    X(SIM_WEAPON_WALK, "WeaponAction_ProcessAllEntities in the frame tail (outside the world tick)") \
    X(SIM_ADM_RESOLVE, "late .adm resolution after the tick (every role)") \
    X(SIM_SINK, "typed per-tick Godot presentation/effects callback") \
    X(SIM_TICKS, "VALUE: logic ticks run this frame") \
    X(SIM_ENTITY_COUNT, "VALUE: live world entities after the frame's ticks") \
    X(SIM_ROLE, "VALUE: the session role the shell folds from Simulation.session_role(): 0 single player, 1 host (listen or dedicated), 2 joiner") \
    X(NET_PEER_COUNT, "VALUE: remote peers on the host's connection table (0 on a joiner or offline)") \
    X(TRACE_TERRAIN, "projectile trace terrain leg") \
    X(TRACE_STATIC, "projectile trace static-entity leg") \
    X(TRACE_DYNAMIC, "projectile trace dynamic-entity leg") \
    X(TRACE_PERSON, "projectile trace person leg") \
    X(TRACE_CALLS, "VALUE: projectile traces") \
    X(TRACE_STATIC_SURVIVORS, "VALUE: static broad-phase survivors") \
    X(TRACE_DYNAMIC_SURVIVORS, "VALUE: dynamic broad-phase survivors") \
    X(TRACE_PERSON_SURVIVORS, "VALUE: person broad-phase survivors") \
    X(TRACE_STATIC_FACES, "VALUE: static survivor face-set sizes") \
    X(TRACE_DYNAMIC_FACES, "VALUE: dynamic survivor face-set sizes") \
    X(EFFECTS_DRAIN, "sim.drain_effects, summed over ticks") \
    X(EFFECTS_TICK, "EffectWorld.advance_fixed_tick, summed over ticks") \
    X(PRESENT_SNAPSHOT, "native get_present_snapshot build") \
    X(PRESENT_MISSION, "MissionPresentPass.present_snapshot") \
    X(PRESENT_MISSION_CORE, "row scan, transform, and submission gate") \
    X(PRESENT_MISSION_AIM, "right-hand collapse + aim-overlay publication") \
    X(PRESENT_MISSION_CONTROLS, "PANM phase and CTRL-bus publication") \
    X(PRESENT_MISSION_VISIBILITY, "sim visibility + section-mask publication") \
    X(PRESENT_MISSION_BODY, "skeletal body-pose selection/publication") \
    X(PRESENT_MISSION_ROWS, "VALUE: rows visited by the mission presenter") \
    X(PRESENT_MISSION_SUBMITTED_ROWS, "VALUE: camera-submitted rows") \
    X(PRESENT_MISSION_BODY_ROWS, "VALUE: rows eligible for a body pose") \
    X(PRESENT_WIRE, "WirePresentPass.present_snapshot") \
    X(PRESENT_WIRE_LIVE, "VALUE: wire-direct nodes alive (WirePresentPass)") \
    X(PRESENT_WIRE_PENDING, "VALUE: wire rows still owed a cold spawn") \
    X(PRESENT_FIRE, "") \
    X(PRESENT_DESTRUCTION, "") \
    X(PRESENT_THROWABLE, "") \
    X(PRESENT_SCARS, "") \
    /* GameHudPresenter.tick legs */ \
    X(HUD_SCALARS, "") \
    X(HUD_ATTACH, "") \
    X(HUD_WAYPOINT, "") \
    X(HUD_INFO, "") \
    X(HUD_FLUSH, "") \
    /* HudOverlay._draw runs in the deferred flush, not the HUD tick: the native */ \
    /* compile + canvas emit of the previous frame's redraw (consumed per tick). */ \
    X(HUD_DRAW_COMPILE, "") \
    X(HUD_DRAW_EMIT, "") \
    /* Measured render times (RenderingServer, previous frame), stored as us */ \
    X(RENDER_ROOT_CPU, "") \
    X(RENDER_ROOT_GPU, "") \
    X(RENDER_WATER_CPU, "") \
    X(RENDER_WATER_GPU, "") \
    /* The focused Q3 pass (the FrameFX compositor) and the slot captures (the */ \
    /* PRE_OPAQUE compositor pass) render inside the root viewport, so their */ \
    /* time also rides RENDER_ROOT_*. The two slots below carve their own GPU */ \
    /* spans back out via RenderingDevice timestamps captured inside each */ \
    /* pass; results surface with Godot's frame delay, so they describe the */ \
    /* previous completed frame. */ \
    X(RENDER_Q3_GPU, "focused Q3 pass GPU span (RD timestamps, previous completed frame)") \
    X(RENDER_SLOT_GPU, "slot-capture pass GPU span (RD timestamps, previous completed frame)") \
    /* Per-pass render counts (RenderingServer per-viewport render info for the */ \
    /* previous frame). VALUE slots: what each pass actually submitted, so pass */ \
    /* cost attribution (main view vs shadow maps vs the water mirror) is read */ \
    /* off the board instead of guessed. */ \
    X(RENDER_MAIN_OBJECTS, "VALUE: root viewport visible-pass objects") \
    X(RENDER_MAIN_DRAWS, "VALUE: root viewport visible-pass draw calls") \
    X(RENDER_SHADOW_OBJECTS, "VALUE: root viewport shadow-pass objects") \
    X(RENDER_SHADOW_DRAWS, "VALUE: root viewport shadow-pass draw calls") \
    X(RENDER_WATER_OBJECTS, "VALUE: water mirror visible-pass objects") \
    X(RENDER_WATER_DRAWS, "VALUE: water mirror visible-pass draw calls") \
    X(RENDER_Q3_OBJECTS, "VALUE: focused Q3 compiler draw commands") \
    X(RENDER_Q3_DRAWS, "VALUE: focused Q3 RenderingDevice draw calls") \
    X(RENDER_SLOT_OBJECTS, "VALUE: slot capture surfaces compiled (the RD pass's draw commands)") \
    X(RENDER_SLOT_DRAWS, "VALUE: slot capture RenderingDevice draw calls") \
    X(RENDER_SLOT_CAPTURES, "VALUE: slot captures drawn this frame (armed by the retail cadence)") \
    X(RENDER_SLOT_PACKED_VERTICES, "VALUE: vertices packed for the slot captures this frame (a stable frame packs 0)") \
    X(RENDER_SLOT_SKINNED, "VALUE: skinned slot capture commands (GPU bone palette)") \
    /* end */

namespace opennova::devtools {

enum class Slot : int {
#define OPENNOVA_FRAME_STATS_SLOT_ENUM(name, description) name,
    OPENNOVA_FRAME_STATS_SLOTS(OPENNOVA_FRAME_STATS_SLOT_ENUM)
#undef OPENNOVA_FRAME_STATS_SLOT_ENUM
    COUNT
};

constexpr int kSlotCount = static_cast<int>(Slot::COUNT);

// The slot's identifier ("FRAME_WALL"); nullptr for an out-of-range value.
const char *slot_name(Slot slot);
// The slot's description from the table above; "" for an out-of-range value.
const char *slot_description(Slot slot);

}  // namespace opennova::devtools
