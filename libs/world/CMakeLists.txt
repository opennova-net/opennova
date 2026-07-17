add_library(opennova_world STATIC
    src/entity_registry.cpp
    src/world.cpp
    src/ai.cpp
    src/collision.cpp
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
    src/spawn_select.cpp
    src/vehicle_attach.cpp
    src/vehicle_motor.cpp
    src/weapon_fsm.cpp
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
        opennova_io
)

target_compile_features(opennova_world PUBLIC cxx_std_17)

set_target_properties(opennova_world PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
