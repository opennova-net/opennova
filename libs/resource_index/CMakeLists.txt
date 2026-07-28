add_library(opennova_resource_index STATIC
    src/resource_index.cpp
)

target_include_directories(opennova_resource_index
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# PUBLIC: resource_index.h exposes opennova::VfsMountMode (from vfs/vfs.h), so the vfs
# include dir must propagate to consumers (the GDExtension, tests).
target_link_libraries(opennova_resource_index PUBLIC opennova_io opennova_vfs)

target_compile_features(opennova_resource_index PUBLIC cxx_std_17)

set_target_properties(opennova_resource_index PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
