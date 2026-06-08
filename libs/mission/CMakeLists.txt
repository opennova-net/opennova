add_library(opennova_mission STATIC
    src/bms.cpp
    src/mission.cpp
    src/mission_schema.cpp
    src/event_runtime.cpp
    src/promote.cpp
)

target_include_directories(opennova_mission
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# event_runtime evaluates BMS events against the shared runtime world.
target_link_libraries(opennova_mission PUBLIC opennova_world)

target_compile_features(opennova_mission PUBLIC cxx_std_17)

set_target_properties(opennova_mission PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
