add_library(opennova_world STATIC
    src/entity_registry.cpp
    src/world.cpp
)

target_include_directories(opennova_world
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_world PUBLIC cxx_std_17)

set_target_properties(opennova_world PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
