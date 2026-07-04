# One directory, two targets along the format/runtime seam.

# Format side: .mis/.bms parse+write, schema reflection, authoring edits.
# Standalone - no world/runtime dependency; the flat C ABI (mission.h) and
# parse-only consumers (refs, npruntime's bms use) build against this.
add_library(opennova_mission_format STATIC
    src/authoring.cpp
    src/bms.cpp
    src/mission.cpp
    src/mission_schema.cpp
)

target_include_directories(opennova_mission_format
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# io/export.h is used from public headers (mission.h).
target_link_libraries(opennova_mission_format PUBLIC opennova_io)

target_compile_features(opennova_mission_format PUBLIC cxx_std_17)

set_target_properties(opennova_mission_format PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Runtime side: the BMS event runtime + mission->world promotion, evaluated
# against the shared runtime world.
add_library(opennova_mission STATIC
    src/event_runtime.cpp
    src/promote.cpp
)

target_link_libraries(opennova_mission PUBLIC
    opennova_mission_format
    opennova_world
)

target_compile_features(opennova_mission PUBLIC cxx_std_17)

set_target_properties(opennova_mission PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
