# One directory, two targets along the format/runtime seam.

# Format side: .mis/.bms parse+write, schema reflection, authoring edits.
# Standalone - no world/runtime dependency; the flat C ABI (mission.h) and
# parse-only consumers (refs, npruntime's bms use) build against this.
add_library(opennova_mission_format STATIC
    src/authoring.cpp
    src/bms.cpp
    src/mission.cpp          # MissionDocument — the document and its edit operations
    src/mission_capi.cpp     # the flat C ABI over it
    src/mission_mis_parser.cpp
    src/mission_mis_writer.cpp
    src/mission_names.cpp    # BMS event-logic enum display names
    src/mission_records.cpp  # bms:: structs <-> typed editor records
    src/mission_schema.cpp
)

target_include_directories(opennova_mission_format
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# io/export.h is used from public headers (mission.h).
target_link_libraries(opennova_mission_format PUBLIC opennova_io)
# DefItemType (items.def `type` values) — authoring derives the entity list
# from them (entity_kind_for_item_type); .cpp-only use, so PRIVATE.
target_link_libraries(opennova_mission_format PRIVATE opennova_def)

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
