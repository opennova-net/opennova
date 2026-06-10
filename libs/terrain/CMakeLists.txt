add_library(opennova_terrain STATIC
    src/brush.cpp
    src/builder.cpp
    src/build_quadtree.cpp
    src/cdep_constraint.cpp
    src/cpt_export.cpp
    src/depthmap.cpp
    src/height_field.cpp
    src/lod.cpp
    src/lighting.cpp
    src/mesh_data.cpp
    src/mesh_simp.cpp
    src/packing.cpp
    src/quadtree.cpp
    src/terrain_mesh.cpp
    src/tristrip.cpp
)

target_include_directories(opennova_terrain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(opennova_terrain
    PUBLIC
        opennova_cpt
        opennova_pcx
        opennova_tpj
        opennova_trn
)

target_compile_features(opennova_terrain PUBLIC cxx_std_17)

set_target_properties(opennova_terrain PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Disable FMA contraction to match the original x87 bake precision.
if(MSVC)
    target_compile_options(opennova_terrain PRIVATE /fp:precise)
else()
    target_compile_options(opennova_terrain PRIVATE -ffp-contract=off)
endif()
