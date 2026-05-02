add_library(opennova_resource_index STATIC
    src/resource_index.cpp
)

target_include_directories(opennova_resource_index
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_resource_index PUBLIC cxx_std_17)

set_target_properties(opennova_resource_index PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
