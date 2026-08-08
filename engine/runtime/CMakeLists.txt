# opennova_runtime — the engine runtime (world sim, AI, terrain build/query,
# renderer math, anim, audio, particle, wac, mission runtime, simassets, hud,
# controls), one STATIC group target (Shape A).
#
# The mission_format half of runtime/mission builds in opennova_formats (the
# Shape A guardrail — see engine/formats/CMakeLists.txt); only the runtime half
# (event runtime + promotion + boot) lives here. runtime/mission/include is
# propagated by the formats group.

add_library(opennova_runtime STATIC
    # anim — skeletal sampling + the aim overlay (.adm parse: engine/formats/adm)
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/src/aim_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/src/anim_sample.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/anim/src/skeletal_pose.cpp
    # audio — mixers, bank chain, envs markers, selectors, profiles
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/src/ambient_mixer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/src/bank_chain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/src/envs_markers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/src/sound_selector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/src/sound_profile.cpp
    # controls — the JO input-binding catalog + CONTROL_MAPPING model
    ${CMAKE_CURRENT_SOURCE_DIR}/controls/src/controls.cpp
    # hud — pure witnessed HUD math (the shell owns every draw/Font blit)
    ${CMAKE_CURRENT_SOURCE_DIR}/hud/src/hud_math.cpp
    # mission (runtime half) — the BMS event runtime + mission->world promotion,
    # evaluated against the shared runtime world
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/src/event_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/src/promote.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/src/runtime_boot.cpp
    # particle — the emitter/effect-scene sim (.ptl model+parse+write: engine/formats/ptl)
    ${CMAKE_CURRENT_SOURCE_DIR}/particle/src/emitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/particle/src/effect_scene.cpp
    # renderer — fixed-function material/light/order math
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src/light_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src/material_eval.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src/material_classify.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src/object_shader_template.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src/particle_atlas.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src/particle_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src/render_order.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src/tracer_frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src/uv_anim.cpp
    # simassets — sim-consumed asset resolution (ADR 0028; Model B only, never
    # part of the C ABI)
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/adm_clip_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/adm_root_motion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/adm_skeletal_clips.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/fp_viewmodel_spec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/item_traits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/model_builders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/mounted_pose.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/seat_spec_extract.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/sim_collision_pose.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/simassets/src/sim_model_cache.cpp
    # terrain — the byte-identical TrnGen bake port + editor kernels
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/brush.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/builder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/build_quadtree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/cdep_constraint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/cpt_export.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/depthmap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/foliage_detail_collector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/lod.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/lighting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/mesh_simp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/quadtree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/terrain_mesh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/texture_preprocess.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/tristrip.cpp
    # terrain_query — the world->terrain seam leaf (ADR 0020): height query +
    # the retail segment raycast; zero format dependencies
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/src/height_field.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/src/terrain_raycast.cpp
    # wac — the WAC scripting VM
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/src/command_table.gen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/src/command_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/src/lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/src/parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/src/compiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/src/vm.cpp
    # world — the ONE runtime World: entities, AI, collision, players, vehicles,
    # weapons, zones (62 Hz tick)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/entity_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/world.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/tick_accumulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/waypoint_track.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/ai_combat.cpp     # acquisition, engagement, commands
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/ai_handlers.cpp   # scoring, state names, per-state handlers + the table
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/ai_system.cpp     # event queue + the AiSystem core and tick
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/ai_waypoints.cpp  # waypoint targeting/movement + vehicle drive staging
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/collision_los.cpp      # ground casts, LOS, sound occlusion
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/collision_model.cpp    # model bounds + the fixed-point matrix ops
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/collision_query.cpp    # the stateless target queries
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/collision_resolve.cpp  # contact resolution + the debug views
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/collision_trace.cpp    # target views, broad phase, trace_projectile
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/collision_world.cpp    # instance registry + the proximity table build
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/occlusion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/infantry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/infantry_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/entity_spawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/player_input.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/player_loadout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/player_look.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/player_spawn.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/player_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/player_weapon.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/destruction.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/fire_sound.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/round_sim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/sound_emitter_mailbox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/throwables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/spawn_select.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/tracer_trails.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/vehicle_attach.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/vehicle_contact_solve.cpp # the air/ground contact + suspension solves (split of vehicle_motor.cpp)
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/vehicle_motor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/vehicle_sound.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/weapon_fsm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/weapon_inventory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/zone_capture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/world/src/zone_chain.cpp
)

target_include_directories(opennova_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/anim/include
        ${CMAKE_CURRENT_SOURCE_DIR}/audio/include
        ${CMAKE_CURRENT_SOURCE_DIR}/controls/include
        ${CMAKE_CURRENT_SOURCE_DIR}/hud/include
        ${CMAKE_CURRENT_SOURCE_DIR}/particle/include
        ${CMAKE_CURRENT_SOURCE_DIR}/renderer/include
        ${CMAKE_CURRENT_SOURCE_DIR}/simassets/include
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/include
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/include
        ${CMAKE_CURRENT_SOURCE_DIR}/wac/include
        ${CMAKE_CURRENT_SOURCE_DIR}/world/include
        # mission/include propagates from opennova_formats (the mission_format
        # half owns the directory's target home).
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src
)

# The Shape A chain: runtime -> base -> formats -> io (all PUBLIC).
target_link_libraries(opennova_runtime PUBLIC opennova_base)

target_compile_features(opennova_runtime PUBLIC cxx_std_17)

set_target_properties(opennova_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# The terrain bake + height-query TUs keep their FP environment: no FMA
# contraction, matching the original x87 bake precision. Scoped per-source so
# the rest of the runtime group keeps the default codegen.
set(_opennova_terrain_fp_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/brush.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/builder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/build_quadtree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/cdep_constraint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/cpt_export.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/depthmap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/foliage_detail_collector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/lod.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/lighting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/mesh_simp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/quadtree.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/terrain_mesh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/texture_preprocess.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain/src/tristrip.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/src/height_field.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/terrain_query/src/terrain_raycast.cpp
)
if(MSVC)
    set_source_files_properties(${_opennova_terrain_fp_sources}
        PROPERTIES COMPILE_OPTIONS "/fp:precise")
else()
    set_source_files_properties(${_opennova_terrain_fp_sources}
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
unset(_opennova_terrain_fp_sources)
