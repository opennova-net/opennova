add_library(opennova_world STATIC
    src/entity_registry.cpp
    src/world.cpp
    src/waypoint_track.cpp
    src/ai_combat.cpp     # acquisition, engagement, commands
    src/ai_handlers.cpp   # scoring, state names, per-state handlers + the table
    src/ai_system.cpp     # event queue + the AiSystem core and tick
    src/ai_waypoints.cpp  # waypoint targeting/movement + vehicle drive staging
    src/collision_los.cpp      # ground casts, LOS, sound occlusion
    src/collision_model.cpp    # model bounds + the fixed-point matrix ops
    src/collision_query.cpp    # the stateless target queries
    src/collision_resolve.cpp  # contact resolution + the debug views
    src/collision_trace.cpp    # target views, broad phase, trace_projectile
    src/collision_world.cpp    # instance registry + the proximity table build
    src/occlusion.cpp
    src/infantry.cpp
    src/infantry_tables.cpp
    src/entity_spawn.cpp
    src/player_input.cpp
    src/player_look.cpp
    src/player_spawn.cpp
    src/player_view.cpp
    src/destruction.cpp
    src/round_sim.cpp
    src/sound_emitter_mailbox.cpp
    src/throwables.cpp
    src/spawn_select.cpp
    src/tracer_trails.cpp
    src/vehicle_attach.cpp
    src/vehicle_motor.cpp
    src/vehicle_sound.cpp
    src/weapon_fsm.cpp
    src/weapon_inventory.cpp
    src/zone_capture.cpp
    src/zone_chain.cpp
)

target_include_directories(opennova_world
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The seam (ADR 0020): world consumes only the terrain_query leaf — the
# TerrainHeightField POD + height_field_* samplers — never the format stack.
target_link_libraries(opennova_world
    PUBLIC
        opennova_terrain_query
        opennova_audio
        opennova_io
)

target_compile_features(opennova_world PUBLIC cxx_std_17)

set_target_properties(opennova_world PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
