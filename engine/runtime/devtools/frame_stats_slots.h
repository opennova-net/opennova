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
    /* The queued flush split by its queuer: the shell drops a deferred marker at each boundary of */ \
    /* its gameplay legs (RootFramePhaseSampler.queue_flush_leg), FIFO as the marker above, so each */ \
    /* slot is what the leg ahead of its marker queued (deferred calls, redraws, skeleton updates). */ \
    X(FRAME_FLUSH_AHEAD, "the queued flush's head: _flush_ugc and what was queued ahead of the shell's gameplay legs (input, physics, earlier nodes, the last flush's tail)") \
    X(FRAME_FLUSH_PLAYER, "what LocalPlayerPresenter's before / after legs queued") \
    X(FRAME_FLUSH_WORLD, "what GameWorld.tick queued (its legs' deferred calls, redraws and skeleton updates)") \
    X(FRAME_FLUSH_HUD, "what the HUD presenters' tick queued") \
    X(FRAME_FLUSH_REST, "what the shell's round flow and the later nodes queued, up to the late marker") \
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
    X(WORLD_RAGDOLL, "ragdoll corpses: observe, step, pose writes (ADR 0060)")     X(WORLD_MATERIAL, "awake ObjectModel runtime/material/animation advance") \
    X(MODEL_CLOCK_ANIMATION, "native model clocks + part/body animation") \
    X(MODEL_PANM, "native PANM transform evaluation/publication") \
    X(MODEL_MATERIAL, "native dynamic material generator/texture writes") \
    X(MODEL_ORDER_BOUNDS, "alpha-strip ordering + changed model bounds") \
    X(MODEL_AWAKE_MODELS, "VALUE: models visited by the shared awake walk") \
    X(MODEL_RENDERABLE_MODELS, "VALUE: visited models currently camera-submitted") \
    X(MODEL_VIEW_WALK, "the per-view RLOD walk, the held-weapon gates and the weapon Inset / water-mirror split reconcile (ObjectModel::update_authored_lod_views)") \
    X(MODEL_VIEW_MARKED, "VALUE: models the weapon Inset and water-mirror views reconciled (refresh_view_split over the Inset's marked set and the models the mirrors' lists hold)") \
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
    X(SIM_ADVANCE, "Simulation.advance_session_frame total: the session frame (its bank, its updates with their ticks and sinks, its render legs, its latch and CPU-share record) and its marshalling") \
    X(SIM_FRAME_BANK, "the session frame's bank (Session::bank_frame): the EMA and the drain count, the frame lock's 1 ms sleeps included, and the FR and CPU-share publication") \
    X(SIM_FRAME_UPDATES, "the session frame's updates (Session::run_updates): each update's input frame, its tick (SIM_STEP) and its sink (SIM_SINK)") \
    X(SIM_FRAME_INPUT_ROWS, "inside SIM_FRAME_UPDATES: the running updates' device-router passes (TickInputSource: the keyboard pass, the sample, the records' queue)") \
    X(SIM_FRAME_INPUT_DRAIN, "inside SIM_FRAME_UPDATES: the running updates' input-event drains (Role::drain_input_events)") \
    X(SIM_FRAME_INPUT_APPLY, "inside SIM_FRAME_UPDATES: every tick's input prologue (Role::apply_input, ahead of the tick SIM_STEP times)") \
    X(SIM_FRAME_RENDER_LEGS, "the session frame's legs past the drain: the scene head, the HUD cursor and radar, the map overlay's clear, the cine pass") \
    X(SIM_FRAME_MARSHAL, "the session frame's marshalling: its typed input (begin_session_frame), the stats fold and its outcome (godot_outcome)") \
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
    X(SIM_UPDATE_WALKS, "the pool-1 slot walk (brains, vehicle motors, ewep, items) + the pool-0 organic walk")     /* The pool-1 slot visit's legs (World::update_pool1_slot), disjoint from SIM_AI_INFANTRY. */     X(SIM_WALK_THINK, "pool-1 think visits: the blink refresh, then a brain, a minefield or an item's damage callback")     X(SIM_WALK_MOTOR, "pool-1 vehicle motors")     X(SIM_WALK_EWEP, "pool-1 emplaced-weapon class updates")     X(SIM_WALK_ITEMS, "pool-1 item legs: destroy fade, squib, death motion, dropped objects, elevators")     X(SIM_WALK_DEVICES, "pool-1 placed devices (throwables): clock, think, parent-follow")     X(SIM_WALK_MOUNTED, "pool-1 mounted brains: the seat shortcut and the re-pose") \
    /* A joiner's replica legs inside the World's walks (JoinerRole's World hooks, D-NET-196). */ \
    X(SIM_WALK_REPLICA_ROWS, "joiner: the decoded pool-1 rows' visits at their World slots (World::pool1_slot_leg)") \
    X(SIM_WALK_REPLICA_POOL0, "joiner: the replica guns' channels, the remote rows' motion through the own body, the proxies' live words (World::before_pool0_walk)") \
    X(SIM_WALK_REPLICA_GUNS, "joiner: the replica guns' channels (tick_replica_emplaced_channels' pool-1 leg)") \
    X(SIM_WALK_REPLICA_MIDDLE, "joiner: the remote rows' motion through the own body's slot (tick_remote_motion_through_self)") \
    X(SIM_WALK_REPLICA_PROXIES, "joiner: every decoded row's proxy live words (sync_wire_collision_proxies_live)") \
    X(SIM_REPLICA_COLLISION, "joiner: the decoded persons' movement resolver and slope-conform ground probes, wherever their visits run") \
    X(SIM_AI_INFANTRY, "the organic body updates (AiSystem::update_organic), pool-0 and pool-1 alike") \
    /* Its children, in the body tick's order: one lap per body (tick_infantry), each mark closing the */ \
    /* leg its slot names, the few stores and tests between two legs riding the leg after them. The */ \
    /* update's head (the death screen's free-fly with it), org1's post-think tail (recoil, spread, */ \
    /* torso roll, heading chase, eye) and its post-resolve tail (edges, ladder, water) stay the */ \
    /* residual: glue. */ \
    X(SIM_AI_INFANTRY_ITEM_MOVERS, "pool-0 bodies bound to an item row: the whole update of such a body, the row's mover over its words (person_item_mover)") \
    X(SIM_AI_INFANTRY_ANIMATION, "weapon/body channels + root-motion sampling (the motor head)") \
    X(SIM_AI_INFANTRY_PLAYER, "org2's own legs: the queued use, the network chase, the rain ambient, the view tick, the row event and radio latch; the kick; the seat site, lean, weapon channel, eye, stance sound, 4th-tick select and ladder override; gravity + parachute; the jump / landing edges and the water block") \
    X(SIM_AI_INFANTRY_CORPSE, "the death edge, the dead tail and corpse legs, the corpse's respawn (both bodies)") \
    X(SIM_AI_INFANTRY_MOTION, "the slope pass, the attachment sample, the carrier ride and the terrain gradient (org1 ahead of its think, org2 around its kick and mover), then the slide decay, the attachment move and the root integrate") \
    X(SIM_AI_INFANTRY_THINK, "NPC think every 16th staggered tick: the 64-tick seat upgrade, then the route / command / board think") \
    X(SIM_AI_INFANTRY_COMBAT, "NPC perception, reactions, and aim") \
    X(SIM_AI_INFANTRY_SELECT, "NPC selection after combat: the burn / forced-animation arbiter, the detour and gait select, the attachment select") \
    X(SIM_AI_INFANTRY_ATTENTION, "NPC attention think") \
    X(SIM_AI_INFANTRY_SEAT, "a mounted-live NPC's seat pose (pose_if_mounted: the seat bone, the scrub, the ground link)") \
    X(SIM_AI_INFANTRY_FIRE, "the anim-event consume: the foley / footstep sound pass (both bodies), then the NPC fire and mounted fire passes") \
    X(SIM_AI_INFANTRY_COLLISION, "the movement resolver call (mounted and ordinary; its terrain settle without a collision world), org1's gravity step riding it") \
    X(SIM_AI_INFANTRY_COLLISION_CONTACTS, "candidate/model contact passes") \
    X(SIM_AI_INFANTRY_COLLISION_REPULSION, "person-sphere separation") \
    X(SIM_AI_INFANTRY_COLLISION_GROUND, "final terrain/model ground ray") \
    X(SIM_AI_INFANTRY_FINISH, "finish_infantry_tick past the motor head, whatever it runs: the motor-to-registry mirror") \
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
    X(SIM_PROXIMITY_REPLICA, "joiner: the unvisited decoded rows' visits, the predicted vehicles' mirror, the wire proxies' rebuild (World::before_proximity_build)") \
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
    X(SIM_CLIENT_ROW_ADM, "joiner: the frame's providers and the decoded rows' .adm resolution, ahead of the receive") \
    X(SIM_CLIENT_DEPOSIT, "joiner: the hello and the socket drain into the runtime's receive queue") \
    X(SIM_CLIENT_SETUP, "client role clock + keepalive setup") \
    X(SIM_CLIENT_RECEIVE, "loopback/wire receive + state fold") \
    X(SIM_CLIENT_AFTER_RECEIVE, "the embedder's receive-handler writes on L (kit, L's birth, its record, the 0x0F's pose)") \
    X(SIM_CLIENT_LOCAL_BIRTH, "joiner: L's birth on a seeded session's in-match edge (a join's is in its mission start)") \
    X(SIM_CLIENT_TIMERS, "revive countdown, zone timers, tracked window, minimap live markers") \
    X(SIM_CLIENT_MAINTENANCE, "decoded-state movers: the frame tick, the lean / dip / tracked-target legs, the remote motion") \
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
    X(SIM_FRAME_LOCK_SLEEPS, "VALUE: the frame lock's 1 ms sleeps in this frame's bank (game.cfg lock_framerate)") \
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
    X(PRESENT_SNAPSHOT, "native build_present_snapshot")     X(PRESENT_SNAPSHOT_BODY, "its organic rows' body columns: pose, weapon channel, aim overlays, held weapon, person overlays") \
    X(PRESENT_MISSION, "MissionPresentPass.present_snapshot") \
    X(PRESENT_MISSION_CORE, "row scan, transform, and submission gate") \
    X(PRESENT_MISSION_AIM, "right-hand collapse + aim-overlay publication") \
    X(PRESENT_MISSION_CONTROLS, "PANM phase and CTRL-bus publication") \
    X(PRESENT_MISSION_VISIBILITY, "sim visibility + section-mask publication") \
    X(PRESENT_MISSION_BODY, "skeletal body-pose selection/publication") \
    X(PRESENT_MISSION_ROWS, "VALUE: rows visited by the mission presenter") \
    X(PRESENT_MISSION_SUBMITTED_ROWS, "VALUE: camera-submitted rows") \
    X(PRESENT_MISSION_BODY_ROWS, "VALUE: rows eligible for a body pose") \
    X(PRESENT_WIRE, "EntityPresenter wire walk (present_snapshot)") \
    X(PRESENT_WIRE_LIVE, "VALUE: wire-direct nodes alive (EntityPresenter wire walk)") \
    X(PRESENT_WIRE_PENDING, "VALUE: wire rows still owed a cold spawn") \
    X(PRESENT_WIRE_PLAN, "the wire walk's plan: its currency check, or the cold re-plan with its spawns, the retire sweep and the spectator framing") \
    X(PRESENT_WIRE_POSE, "wire rows: the ragdoll note, the husk, parachute, march, terrain and destroy stamps, the transform compose and write, the right-hand collapse, the aim overlay") \
    X(PRESENT_WIRE_CTRL, "wire rows: the CTRL/PART block (procedural part phases, the CTRL writers)") \
    X(PRESENT_WIRE_BODY, "wire rows: the respawn reset and the remote body animation dispatch / blend ticks") \
    X(PRESENT_WIRE_WEAPON, "wire rows: the upper-body weapon channel dispatch") \
    X(PRESENT_WIRE_SOUNDS, "wire rows: the remote-body footstep / foley walk") \
    X(PRESENT_WIRE_HELD, "wire rows: the held weapon (the skeleton walk, the bone-16 attach, its transform) and the item overlays") \
    X(PRESENT_WIRE_VIEWS, "wire rows: the section mask, the per-view verdicts and the node visibility") \
    X(PRESENT_WIRE_CULLED, "wire rows: the collector-culled rows' legs") \
    X(PRESENT_WIRE_ROWS, "VALUE: wire rows the walk presented") \
    X(PRESENT_WIRE_COLD_PLANS, "VALUE: the wire walk's cold re-plans (the plan was not current)") \
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
    X(RENDER_SLOT_READBACKS, "VALUE: slot caster surfaces read back through the server this frame (a stable frame reads 0)") \
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
