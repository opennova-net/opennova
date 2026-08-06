add_library(opennova_terrain STATIC
    src/brush.cpp
    src/builder.cpp
    src/build_quadtree.cpp
    src/cdep_constraint.cpp
    src/cpt_export.cpp
    src/depthmap.cpp
    src/foliage_detail_collector.cpp
    src/lod.cpp
    src/lighting.cpp
    src/mesh_data.cpp
    src/mesh_simp.cpp
    src/packing.cpp
    src/quadtree.cpp
    src/terrain_mesh.cpp
    src/texture_preprocess.cpp
    src/tristrip.cpp
)

target_include_directories(opennova_terrain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# height_field/coords live in the terrain_query leaf (ADR 0020); the PUBLIC
# link keeps this lib's consumers compiling unchanged (<terrain/...> includes).
target_link_libraries(opennova_terrain
    PUBLIC
        opennova_cpt
        opennova_io
        opennova_pcx
        opennova_terrain_query
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
