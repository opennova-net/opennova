# terrain_query — the world→terrain seam (ADR 0020): the portable height-query
# leaf (the TerrainHeightField POD + the height_field_* samplers + the
# world->source coordinate kernel), moved verbatim out of libs/terrain, plus
# the retail segment raycast port (terrain_raycast, ENG-3 B1).
# Deliberately a ZERO-dependency leaf so runtime consumers (libs/world and
# everything above it) stop dragging the terrain-format stack
# (cpt/til/trn/tpj/foliage) through their link closure. Headers keep the
# <terrain/...> include shape and the opennova::terrain namespace, so call
# sites are unchanged from when these files lived in libs/terrain.
add_library(opennova_terrain_query STATIC
    src/height_field.cpp
    src/terrain_raycast.cpp
)

target_include_directories(opennova_terrain_query
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_terrain_query PUBLIC cxx_std_17)

set_target_properties(opennova_terrain_query PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# The same FP environment these TUs compiled under inside opennova_terrain:
# no FMA contraction, matching the original x87 precision (see libs/terrain).
if(MSVC)
    target_compile_options(opennova_terrain_query PRIVATE /fp:precise)
else()
    target_compile_options(opennova_terrain_query PRIVATE -ffp-contract=off)
endif()
