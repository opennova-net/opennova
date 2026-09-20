# opennova_runtime — the engine runtime (world sim, AI, terrain build/query,
# renderer math, anim, audio, particle, wac, mission runtime, assets, hud,
# controls), one STATIC group target (Shape A).
#
# The mission FORMAT half lives in engine/formats/mission (the Shape A
# guardrail — see engine/formats/CMakeLists.txt); this group keeps the runtime
# half (event runtime + promotion + boot). Both halves expose <mission/...>
# with disjoint header sets (ADR 0030 decision 2).

add_library(opennova_runtime STATIC
    # anim — skeletal sampling + the aim overlay (.adm parse: engine/formats/adm)
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/adm_clip_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/adm_root_motion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/skeletal_clips.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/aim_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/anim_sample.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/clip_timeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/skeletal_pose.cpp
    # audio — mixers, bank chain, envs markers, selectors, profiles
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/ambient_mixer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/bank_chain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/dialog_queue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/envs_markers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/oneshot_play.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/sound_selector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/sound_profile.cpp
    # Frame statistics remain available without the debug-only ImGui tools.
    ${CMAKE_CURRENT_SOURCE_DIR}/devtools/frame_stats_board.cpp
    # controls — the JO input-binding catalog + CONTROL_MAPPING model
    ${CMAKE_CURRENT_SOURCE_DIR}/controls/controls.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/controls/binding_set.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/controls/key_strings.cpp
    # environment — the TOD environment state owner + the weather runtime
    # embedding (the 62 Hz tick credit, reset epoch, prewarm, per-tick
    # target-feed/writeback order) + the mission-start network sample
    # derivation (ENV resource + BMS header -> world::EnvNetworkState); the
    # witnessed math cores stay in engine/formats/env
    ${CMAKE_CURRENT_SOURCE_DIR}/environment/precipitation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/environment/environment_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/environment/sky_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/environment/water_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/environment/weather_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/environment/weather_seed.cpp
    # hud — pure witnessed HUD math (the shell owns every draw/Font blit)
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_math.cpp
    # menu — the .mnu screen draw-list compiler (ADR 0033 R2; witness:
    # docs/mnu/menu-re.md "Widget render dispatch")
    ${CMAKE_CURRENT_SOURCE_DIR}/menu/menu_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/menu/menu_frame_scrollbar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/menu/menu_frame_table.cpp
    # menu backdrop movie policy (slots/gating/resolution; witness:
    # docs/mnu/menu-re.md "The menu backdrop (Bink underlay)")
    ${CMAKE_CURRENT_SOURCE_DIR}/menu/menu_video.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/menu/player_info_kit.cpp # the PLAYER_INFO voice list + weapon.sav kit page order
    # mission (runtime half) — the BMS event runtime + mission->world promotion,
    # the boot policy, and the ONE mission kernel (boot + state + no-net tick,
    # ADR 0042 d3) evaluated against the shared runtime world
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/collision_resolve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/item_traits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/seat_spec_extract.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/event_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_catalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_kernel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/teammate_spawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/placement_traits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/promote.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/runtime_boot.cpp
    # particle — the emitter/effect-scene sim (.ptl model+parse+write: engine/formats/particle)
    ${CMAKE_CURRENT_SOURCE_DIR}/particle/emitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/particle/effect_scene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/particle/effect_catalog_names.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/particle/script_effects.cpp
    # hud — the retail game-font text engine + element frame math
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/game_font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_declutter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_layout_from_hudpos.cpp    # the hudpos.def parse applied to the layout globals
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_game_text.cpp    # the HUD's game-text key compositions and fallbacks
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_combat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_optical_cues.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/inset_scope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/sight_overlay.cpp # the SIGHTS card row modes + the dotsize cycle
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/scope_circle_mask.cpp # the scoped-view circle mask + its reticle cross/grid
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/feed_format.cpp   # the 0x1E feed line/color policy
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/loading_screen.cpp # the loading screen's wrapped text block
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_toggles.cpp    # the key-driven HUD toggles and their edge polls
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/end_round_overlay.cpp # the 0x1D end-round text ladder + column layout
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_scoreboard.cpp   # the Tab player list's row policy
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_vehicle_panel.cpp # the mounted-vehicle seat markers
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_frame_vehicle_panel.cpp # its compile element
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_frame_message_log.cpp   # the Recent Messages (J) window element
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_frame_end_round_statistics.cpp # the SP Show Score panel element
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_frame_lfp_panel.cpp     # the AAS zone status panel element
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_minimap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/hud_minimap_feed.cpp
    # renderer — fixed-function material/light/order math
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/authored_occluder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/light_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/light_scene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/scar_draw_list.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/light_terrain_pass.cpp # the per-light terrain projected pass
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/material_eval.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/material_texture.cpp
    renderer/material_texture_producers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/material_classify.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/object_shader_template.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/object_lod.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/foliage_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/particle_atlas.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/particle_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/q3_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/fp_viewmodel_spec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/render_order.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/render_slot_shadow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/precipitation_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/tracer_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/uv_anim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/asset_store.cpp
    # terrain — runtime terrain collection, rendering, and queries
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/foliage_detail_collector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/lighting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/quadtree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow_alpha.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow_geometry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow_planner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow_raster.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_scorch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_tile_composer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_tile_composition_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/texture_preprocess.cpp
    # terrain_query — the world->terrain seam leaf (ADR 0020): height query +
    # the retail segment raycast (format-free), plus the placed-tile surface
    # resolvers (surface_tiles.cpp — the one TU with .til format access,
    # D-SND-15; the seam headers themselves stay format-free)
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/height_field.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/surface_tiles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/terrain_field_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/terrain_raycast.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/terrain_scorch_record.cpp
    # wac — the WAC compiler (binds names/vars against the live world) + the VM;
    # the language front end (lexer, parser/AST, command table, Program model)
    # is engine/formats/wac
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/compiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/remote_command.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/vm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/wac_system.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/wac_layered_load.cpp
    # world — the ONE runtime World: entities, AI, collision, players, vehicles,
    # weapons, zones (62 Hz tick)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/model_geometry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/mounted_pose.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/entity_pose.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/entity_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/match.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/world.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/entity_commands.cpp   # the Entity_* command layer (split from world.cpp)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/weather_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/tick_accumulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/waypoint_track.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/ai_combat.cpp     # acquisition, engagement, commands
    ${CMAKE_CURRENT_SOURCE_DIR}/world/ai_aircraft.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/ai_handlers.cpp   # scoring, state names, per-state handlers + the table
    ${CMAKE_CURRENT_SOURCE_DIR}/world/ai_system.cpp     # event queue + the AiSystem core and tick
    ${CMAKE_CURRENT_SOURCE_DIR}/world/ai_waypoints.cpp  # waypoint targeting/movement + vehicle drive staging
    ${CMAKE_CURRENT_SOURCE_DIR}/world/collision_los.cpp      # ground casts, LOS, sound occlusion
    ${CMAKE_CURRENT_SOURCE_DIR}/world/collision_model.cpp    # model bounds + the fixed-point matrix ops
    ${CMAKE_CURRENT_SOURCE_DIR}/world/collision_query.cpp    # the stateless target queries
    ${CMAKE_CURRENT_SOURCE_DIR}/world/collision_vehicle.cpp # vehicle probe contacts
    ${CMAKE_CURRENT_SOURCE_DIR}/world/collision_resolve.cpp  # contact resolution + the debug views
    ${CMAKE_CURRENT_SOURCE_DIR}/world/collision_trace.cpp    # target views, broad phase, trace_projectile
    ${CMAKE_CURRENT_SOURCE_DIR}/world/collision_world.cpp    # instance registry + the proximity table build
    ${CMAKE_CURRENT_SOURCE_DIR}/world/occlusion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/iris_march.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/parachute.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_carrier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_slope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/collision_force.cpp   # Entity_ApplyCollisionForce: burn state + the four force modes
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_attention.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_detour.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/teammate_operations.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_escort.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_ladder.cpp         # the D-COL-5 climb legs
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_board.cpp           # the 123..127 command legs (ex-D-INF-2)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_combat.cpp          # the D-AI-4 combat pass
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_animation.cpp     # motor-head channel update
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_remote_anim.cpp   # remote-player body anim (size gate)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_sound.cpp          # the foley/footstep slot-sound leg
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_water.cpp          # the §29.1 water/float channel
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_weapon_channel.cpp # the secondary AnimMap channel (§14.8)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_wire.cpp            # replicated anim/input mirror
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/entity_spawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/doors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/facial_animation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/infantry_spawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/inspect.cpp   # the entity directory + debug cards (ADR 0042 d5)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/item_effects.cpp   # the ITEMS.DEF particlefx attach law: pool gates, identity aliases, attach plan
    ${CMAKE_CURRENT_SOURCE_DIR}/world/present_passes.cpp # the present passes' shared law: husk render pick, husk identity, settle tilt retention
    ${CMAKE_CURRENT_SOURCE_DIR}/world/minimap_footprint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/minimap_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/player_input.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/player_loadout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/player_look.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/player_present.cpp  # the local player's presentation law: the FP submit, the fire-effect gate, the batch order, the spawn-loadout projection
    ${CMAKE_CURRENT_SOURCE_DIR}/world/present_drains.cpp  # the present-pass row fills: throwables, vehicle trails, impacts, fires, death pieces, round glows
    ${CMAKE_CURRENT_SOURCE_DIR}/world/objectives_feed.cpp  # the SP objectives panel's row walk
    ${CMAKE_CURRENT_SOURCE_DIR}/world/music_vars.cpp       # the gamemus var pump from the local player
    ${CMAKE_CURRENT_SOURCE_DIR}/world/player_spawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/player_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/local_player.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/local_player_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/hud_combat_feed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/hud_impact.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/hud_vehicle_sights.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/player_weapon.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/player_weapon_view.cpp  # the per-tick equipped-weapon snapshot
    ${CMAKE_CURRENT_SOURCE_DIR}/world/player_weapon_pose.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/destruction.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/item_events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/item_squib.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/collision_squib.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/item_sections.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/death_effects.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/destruction_motion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/ground_conform.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/impact_scar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/fire_sound.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/friendly_tags.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/deploy_screen_feed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/death_camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/lfp_feed.cpp           # the AAS zone status panel's zone rows
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_panel_feed.cpp # the mounted-vehicle panel's seat rows
    ${CMAKE_CURRENT_SOURCE_DIR}/world/guided_missile_flight.cpp
    world/guided_round.cpp  # the stng pursuit integrator (D-NET-64)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/round_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/sound_emitter_mailbox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/terrain_scorch_events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/throwables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/minefield.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/round_source_fire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_countermeasures.cpp
    world/vehicle_lifecycle.cpp
    world/vehicle_spawn_markers.cpp
    world/vehicle_effects.cpp
    world/vehicle_trails.cpp
    world/rotor_wash.cpp
    renderer/water_wake_frame.cpp
    world/vehicle_chassis.cpp
    world/vehicle_impulse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/spawn_select.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/tracer_trails.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_attach.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_traction.cpp # retained skid/drive direction
    ${CMAKE_CURRENT_SOURCE_DIR}/world/carrier_motion.cpp
    world/vehicle_contact.cpp # shared probe/impact contract
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_contact_solve.cpp # the air/ground contact + suspension solves (split of vehicle_motor.cpp)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_motor.cpp
    world/vehicle_simple_motor.cpp
    world/vehicle_simple_contact.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_mount.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_system.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_motor_air.cpp # the CHel/cpln mover: input block, prediction leg, authority legs (split of vehicle_motor.cpp)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_part_anim.cpp # the rotor spin machine + wheel phase (split of the mover tails)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_sound.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/vehicle_suspension.cpp # the spring leg: latch pick + per-wheel step (split of the contact solves)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/ammo_table_build.cpp   # ammo.def -> AmmoTable + the round_type resolve
    ${CMAKE_CURRENT_SOURCE_DIR}/world/weapon_table_build.cpp # weapon.def -> WeaponTable + the D-NET-141 loadout rules
    ${CMAKE_CURRENT_SOURCE_DIR}/world/script_projectiles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/script_effects.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/script_sounds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/radio_call.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/script_voice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/weapon_fsm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/weapon_fire_gate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/weapon_scope_zero.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/local_player_targeting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/wire_body_sound.cpp    # the wire-fed remote body's footstep/foley consume
    ${CMAKE_CURRENT_SOURCE_DIR}/world/weapon_inventory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/zone_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/zone_chain.cpp
    # replication — the world<->wire seam (ex net/netsim, ADR 0043 d4):
    # connection-fan primitives, client replica state and its folds, the
    # client world materializer, the roster/scoreboard views
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_body_arbitration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_body_ticks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_death.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_endround.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_feed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_lifecycle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_guided.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_minimap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_dispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_script_commands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_placed_device.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_replica_scoreboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_roster_tags.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_scoreboard_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/client_world_materializer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/connection_fan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/entity_wire_bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/replication/item_replication_catalog.cpp
    # inmatch — the in-match session (ex net/inmatch + net/npruntime, ADR 0043
    # d4): the fixed-tick session, the listen-host frame, the IDA-faithful
    # server/client state machines and frame loops, the transports
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/character_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/charattr_challenge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/client_replica_card.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/client_replica_emplaced.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/client_replica_present_projection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/present_rows.cpp            # the PF_* row collectors, both roles (ADR 0043 G3)
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/client_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/client_effects.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/client_weapon_replay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/end_round_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/host_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/host_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/integrity_challenge_profile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/join_session_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/joiner_connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/joiner_connection_disconnect.cpp  # the punt/goodbye/reap/leave family of JoinerConnection
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/host_role.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/joiner_role.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/local_role.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/loadout_submit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/loopback_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/minimap_markers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/napi_np_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/server_initial_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/server_loadout_grant.cpp        # the C2S 0x2F grant family (split off server_message_dispatch.cpp)
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/server_message_dispatch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/server_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/server_spawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/server_tick.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/session_status.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/stat_screen_feed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/inmatch/udp_session_transport.cpp
)

# engine/ is the one public include root (ADR 0040): <runtime/<lib>/file.h>.
# The mission runtime half is <runtime/mission/...>; the document model is
# <formats/mission/...> — the group in the path keeps the two apart.
target_include_directories(opennova_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# The chain (ADR 0043 d4): runtime -> net -> base -> formats -> io (all
# PUBLIC). runtime carries the in-match session and replication, so it sits
# above the wire-only net group.
target_link_libraries(opennova_runtime PUBLIC opennova_net opennova_base opennova_crt)

target_compile_features(opennova_runtime PUBLIC cxx_std_17)

# Dear ImGui (ADR 0039) links PRIVATE so no ImGui header reaches a consumer: the
# shell hands the context over as plain pointers (devtools/imgui_abi.h). The
# game's dev-tool windows compile only when the including root leaves
# OPENNOVA_DEVTOOLS on (every build but the release GDExtension flavour); the
# define is PUBLIC so the shell gates its DevTools node on it.
if(OPENNOVA_DEVTOOLS)
    target_link_libraries(opennova_runtime PRIVATE opennova_imgui)
    target_sources(opennova_runtime PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/imgui_pass.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/game_dev_tools.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/game_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/demo_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/entities_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/entity_properties_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/ai_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/environment_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/physics_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/rays_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/stats_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/weapon_window.cpp
    )
    target_compile_definitions(opennova_runtime PUBLIC OPENNOVA_DEVTOOLS=1)
else()
    target_compile_definitions(opennova_runtime PUBLIC OPENNOVA_DEVTOOLS=0)
endif()

set_target_properties(opennova_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Terrain math keeps a stable FP environment with no FMA contraction. Scoped
# per-source so the rest of the runtime group keeps the default codegen.
set(_opennova_terrain_fp_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/foliage_detail_collector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/lighting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/quadtree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow_alpha.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow_geometry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow_planner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_static_shadow_raster.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_scorch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_tile_composer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/terrain_tile_composition_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/texture_preprocess.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/height_field.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/terrain_raycast.cpp
)
if(MSVC)
    set_source_files_properties(${_opennova_terrain_fp_sources}
        PROPERTIES COMPILE_OPTIONS "/fp:precise")
else()
    set_source_files_properties(${_opennova_terrain_fp_sources}
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
unset(_opennova_terrain_fp_sources)
