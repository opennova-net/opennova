# Roundtrip test
add_executable(threedi_roundtrip_test threedi/roundtrip_test.cpp)
target_link_libraries(threedi_roundtrip_test PRIVATE opennova_threedi)
target_include_directories(threedi_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_roundtrip COMMAND threedi_roundtrip_test)

add_executable(threedi_material_class_test threedi/material_class_test.cpp)
target_link_libraries(threedi_material_class_test PRIVATE opennova_threedi)
target_include_directories(threedi_material_class_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_material_class COMMAND threedi_material_class_test)

add_executable(threedi_gp_auto_read_test threedi/gp_auto_read_test.cpp)
target_link_libraries(threedi_gp_auto_read_test PRIVATE opennova_threedi)
target_include_directories(threedi_gp_auto_read_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_gp_auto_read COMMAND threedi_gp_auto_read_test)

# PANM unit tests
foreach(test ctrl decode runtime wave)
    add_executable(threedi_panm_${test}_test threedi/panm_${test}_test.cpp)
    target_link_libraries(threedi_panm_${test}_test PRIVATE opennova_threedi)
    target_include_directories(threedi_panm_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME threedi_panm_${test} COMMAND threedi_panm_${test}_test)
endforeach()

# ADM tests
foreach(test comment trim_value parse)
    add_executable(adm_${test}_test adm/adm_${test}_test.cpp)
    target_link_libraries(adm_${test}_test PRIVATE opennova_adm)
    target_include_directories(adm_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME adm_${test} COMMAND adm_${test}_test)
endforeach()

# BAD tests
add_executable(bad_parse_test bad/bad_parse_test.cpp)
target_link_libraries(bad_parse_test PRIVATE opennova_bad)
target_include_directories(bad_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bad_parse COMMAND bad_parse_test)

# DEF tests
add_executable(def_comment_strip_test def/def_comment_strip_test.cpp)
target_link_libraries(def_comment_strip_test PRIVATE opennova_def)
target_include_directories(def_comment_strip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME def_comment_strip COMMAND def_comment_strip_test)

foreach(test items weapons ammo hudpos)
    add_executable(def_parse_${test}_test def/def_parse_${test}_test.cpp)
    target_link_libraries(def_parse_${test}_test PRIVATE opennova_def)
    target_include_directories(def_parse_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME def_parse_${test} COMMAND def_parse_${test}_test)
endforeach()

# PFF tests
add_executable(pff_unit_test pff/pff_unit_test.cpp)
target_link_libraries(pff_unit_test PRIVATE opennova_pff)
target_include_directories(pff_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pff_unit COMMAND pff_unit_test)

# BFC1 tests
add_executable(bfc1_unit_test bfc1/bfc1_unit_test.cpp)
target_link_libraries(bfc1_unit_test PRIVATE opennova_bfc1)
target_include_directories(bfc1_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bfc1_unit COMMAND bfc1_unit_test)

# SCR tests
add_executable(scr_unit_test scr/scr_unit_test.cpp)
target_link_libraries(scr_unit_test PRIVATE opennova_scr)
target_include_directories(scr_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME scr_unit COMMAND scr_unit_test)

# Resource directory index tests
add_executable(resource_index_test resource_index/resource_index_test.cpp)
target_link_libraries(resource_index_test PRIVATE opennova_resource_index)
target_include_directories(resource_index_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME resource_index COMMAND resource_index_test)

# ASE tests
add_executable(ase_smoke_test ase/ase_smoke_test.cpp)
target_link_libraries(ase_smoke_test PRIVATE opennova_ase)
target_include_directories(ase_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ase_smoke COMMAND ase_smoke_test)

add_executable(ase_parser_unit_test ase/ase_parser_unit_test.cpp)
target_link_libraries(ase_parser_unit_test PRIVATE opennova_ase)
target_include_directories(ase_parser_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ase_parser_unit COMMAND ase_parser_unit_test)

add_executable(ase_writer_test ase/ase_writer_test.cpp)
target_link_libraries(ase_writer_test PRIVATE opennova_ase)
target_include_directories(ase_writer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ase_writer COMMAND ase_writer_test)

# TDP tests
add_executable(tdp_roundtrip_test tdp/tdp_roundtrip_test.cpp)
target_link_libraries(tdp_roundtrip_test PRIVATE opennova_tdp opennova_threedi)
target_include_directories(tdp_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tdp_roundtrip COMMAND tdp_roundtrip_test)

add_executable(tdp_3da_write_test tdp/tdp_3da_write_test.cpp)
target_link_libraries(tdp_3da_write_test PRIVATE opennova_tdp opennova_threedi)
target_include_directories(tdp_3da_write_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tdp_3da_write COMMAND tdp_3da_write_test)

add_executable(tdp_3da_read_test tdp/tdp_3da_read_test.cpp)
target_link_libraries(tdp_3da_read_test PRIVATE opennova_tdp opennova_threedi)
target_include_directories(tdp_3da_read_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tdp_3da_read COMMAND tdp_3da_read_test)

# FlatMesh tests (Phase B)
add_executable(flat_mesh_alloc_test object/flat_mesh_alloc_test.cpp)
target_link_libraries(flat_mesh_alloc_test PRIVATE opennova_object)
target_include_directories(flat_mesh_alloc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME flat_mesh_alloc COMMAND flat_mesh_alloc_test)

add_executable(ir_to_flat_mesh_test object/ir_to_flat_mesh_test.cpp object/synthetic_ir.cpp)
target_link_libraries(ir_to_flat_mesh_test PRIVATE opennova_object opennova_threedi)
target_include_directories(ir_to_flat_mesh_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_to_flat_mesh COMMAND ir_to_flat_mesh_test)

add_executable(ir_to_flat_mesh_fixture_test object/ir_to_flat_mesh_fixture_test.cpp)
target_link_libraries(ir_to_flat_mesh_fixture_test PRIVATE opennova_object opennova_threedi)
target_include_directories(ir_to_flat_mesh_fixture_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ir_to_flat_mesh_fixture COMMAND ir_to_flat_mesh_fixture_test)

add_executable(coords_test object/coords_test.cpp)
target_link_libraries(coords_test PRIVATE opennova_object)
target_include_directories(coords_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME coords_test COMMAND coords_test)

add_executable(smoothing_groups_test object/smoothing_groups_test.cpp)
target_link_libraries(smoothing_groups_test PRIVATE opennova_object)
target_include_directories(smoothing_groups_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME smoothing_groups_test COMMAND smoothing_groups_test)

add_executable(nlascexp_quirks_test object/nlascexp_quirks_test.cpp)
target_link_libraries(nlascexp_quirks_test PRIVATE opennova_object)
target_include_directories(nlascexp_quirks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nlascexp_quirks_test COMMAND nlascexp_quirks_test)

add_executable(ase_vec_test object/ase_vec_test.cpp)
target_link_libraries(ase_vec_test PRIVATE opennova_object)
target_include_directories(ase_vec_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ase_vec_test COMMAND ase_vec_test)

add_executable(flat_mesh_to_ase_test object/flat_mesh_to_ase_test.cpp)
target_link_libraries(flat_mesh_to_ase_test PRIVATE opennova_object opennova_ase opennova_threedi)
target_include_directories(flat_mesh_to_ase_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME flat_mesh_to_ase COMMAND flat_mesh_to_ase_test)

add_executable(material_list_to_ase_test object/material_list_to_ase_test.cpp)
target_link_libraries(material_list_to_ase_test PRIVATE opennova_object opennova_ase opennova_threedi)
target_include_directories(material_list_to_ase_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME material_list_to_ase COMMAND material_list_to_ase_test)

add_executable(lights_to_ase_test object/lights_to_ase_test.cpp)
target_link_libraries(lights_to_ase_test PRIVATE opennova_object opennova_ase opennova_threedi)
target_include_directories(lights_to_ase_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME lights_to_ase COMMAND lights_to_ase_test)

add_executable(write_ase_files_from_flat_meshes_test object/write_ase_files_from_flat_meshes_test.cpp)
target_link_libraries(write_ase_files_from_flat_meshes_test PRIVATE opennova_object opennova_ase opennova_threedi)
target_include_directories(write_ase_files_from_flat_meshes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME write_ase_files_from_flat_meshes COMMAND write_ase_files_from_flat_meshes_test)

add_executable(bones_to_ase_with_bad_test object/bones_to_ase_with_bad_test.cpp)
target_link_libraries(bones_to_ase_with_bad_test PRIVATE opennova_object opennova_ase opennova_bad opennova_threedi)
target_include_directories(bones_to_ase_with_bad_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bones_to_ase_with_bad COMMAND bones_to_ase_with_bad_test)

add_executable(bake_project_export_test object/bake_project_export_test.cpp)
target_link_libraries(bake_project_export_test PRIVATE opennova_object opennova_threedi)
target_include_directories(bake_project_export_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    # The native project bake path parses ASE and builds the full object IR in a
    # single process; keep the test harness stack reserve aligned with Python and
    # OED-style runners that already have a larger stack.
    target_link_options(bake_project_export_test PRIVATE /STACK:8388608)
endif()
add_test(NAME bake_project_export COMMAND bake_project_export_test)

add_executable(three_di_policy_test object/three_di_policy_test.cpp)
target_link_libraries(three_di_policy_test PRIVATE opennova_object opennova_threedi)
target_include_directories(three_di_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME three_di_policy COMMAND three_di_policy_test)

# Terrain format library tests
add_executable(pcx_roundtrip_test pcx/pcx_roundtrip_test.cpp)
target_link_libraries(pcx_roundtrip_test PRIVATE opennova_pcx)
target_include_directories(pcx_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pcx_roundtrip COMMAND pcx_roundtrip_test)

add_executable(til_roundtrip_test til/til_roundtrip_test.cpp)
target_link_libraries(til_roundtrip_test PRIVATE opennova_til)
target_include_directories(til_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_roundtrip COMMAND til_roundtrip_test)

add_executable(til_tile_atlas_test til/til_tile_atlas_test.cpp)
target_link_libraries(til_tile_atlas_test PRIVATE opennova_til)
target_include_directories(til_tile_atlas_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_tile_atlas COMMAND til_tile_atlas_test)

add_executable(til_render_uv_test til/til_render_uv_test.cpp)
target_link_libraries(til_render_uv_test PRIVATE opennova_til)
target_include_directories(til_render_uv_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_render_uv COMMAND til_render_uv_test)

add_executable(til_overlay_bake_test til/til_overlay_bake_test.cpp)
target_link_libraries(til_overlay_bake_test PRIVATE opennova_til)
target_include_directories(til_overlay_bake_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_overlay_bake COMMAND til_overlay_bake_test)

add_executable(foliage_map_test foliage/foliage_map_test.cpp)
target_link_libraries(foliage_map_test PRIVATE opennova_foliage)
target_include_directories(foliage_map_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME foliage_map COMMAND foliage_map_test)

# Foliage placement + dispatcher parity tests — exercise sub_5C0240 / sub_5C1940
# invariants (deterministic PRNG, 21-cap, 4-quadrant walk, 128-LRU, 8-frame
# stagger, 38.0 near-Z reject). See libs/foliage/include/foliage/placement.h,
# docs/engine_spec_foliage.md §4–§5, RE/kong/kong_output_jodemo/terrain.c:15026+.
foreach(name placement_determinism placement_cap placement_midpoints dispatcher_quadrants dispatcher_lru dispatcher_stagger dispatcher_near_z dispatcher_frame_progression)
    add_executable(foliage_${name}_test foliage/foliage_${name}_test.cpp)
    target_link_libraries(foliage_${name}_test PRIVATE opennova_foliage)
    target_include_directories(foliage_${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME foliage_${name} COMMAND foliage_${name}_test)
endforeach()

add_executable(tpj_roundtrip_test tpj/tpj_roundtrip_test.cpp)
target_link_libraries(tpj_roundtrip_test PRIVATE opennova_tpj)
target_include_directories(tpj_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tpj_roundtrip COMMAND tpj_roundtrip_test)

add_executable(trn_roundtrip_test trn/trn_roundtrip_test.cpp)
target_link_libraries(trn_roundtrip_test PRIVATE opennova_trn)
target_include_directories(trn_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME trn_roundtrip COMMAND trn_roundtrip_test)

add_executable(trn_optional_polydata_test trn/trn_optional_polydata_test.cpp)
target_link_libraries(trn_optional_polydata_test PRIVATE opennova_trn)
target_include_directories(trn_optional_polydata_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME trn_optional_polydata COMMAND trn_optional_polydata_test)

add_executable(cpt_roundtrip_test cpt/cpt_roundtrip_test.cpp)
target_link_libraries(cpt_roundtrip_test PRIVATE opennova_cpt)
target_include_directories(cpt_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cpt_roundtrip COMMAND cpt_roundtrip_test)

# ENV environment/time-of-day format tests
add_executable(env_unit_test env/env_unit_test.cpp)
target_link_libraries(env_unit_test PRIVATE opennova_env)
target_include_directories(env_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(env_unit_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME env_unit COMMAND env_unit_test)

# Terrain build foundation tests ported from the canonical terrain reference.
#   crypto_roundtrip   — rotate_encrypt/decrypt round-trip
#   depthmap_smooth    — 8-bit -> 16-bit smoothing formula parity
#   tpm1_roundtrip     — .tml MeshData read/write
#   cdep_read          — CDEP header parity against a known Dvxi5.cpt capture
#   cdep_roundtrip     — CDEP encode/decode round-trip independent of the bake
foreach(name depthmap_smooth tpm1_roundtrip)
    add_executable(${name}_test trngen/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE opennova_terrain)
    target_include_directories(${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

add_executable(terrain_lighting_test trngen/terrain_lighting_test.cpp)
target_link_libraries(terrain_lighting_test PRIVATE opennova_terrain)
target_include_directories(terrain_lighting_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_lighting COMMAND terrain_lighting_test)

foreach(name crypto_roundtrip cdep_read cdep_roundtrip)
    add_executable(${name}_test trngen/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE opennova_cpt)
    target_include_directories(${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

add_executable(trn_config_roundtrip_test trngen/trn_config_roundtrip_test.cpp)
target_link_libraries(trn_config_roundtrip_test PRIVATE opennova_trn)
target_include_directories(trn_config_roundtrip_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libs/terrain/include
)
add_test(NAME trn_config_roundtrip COMMAND trn_config_roundtrip_test)

# DVD4 parity test: drives opennova::build_terrain() on the captured BHD-era heightmap.
# Regression test for the LoadSectionDedup crash that hit older terrains where
# the root merge ingests face indices past level.vertex_count.
add_executable(dvd4_parity_test trngen/dvd4_parity_test.cpp)
target_link_libraries(dvd4_parity_test PRIVATE opennova_terrain)
target_include_directories(dvd4_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dvd4_parity COMMAND dvd4_parity_test)

# CDEP overflow guard: bake must clamp rather than silently corrupt the .cpt
# when a 256-pixel block exceeds the 4-bit bits_per_delta field's 15-bit limit.
add_executable(cdep_overflow_guard_test trngen/cdep_overflow_guard_test.cpp)
target_link_libraries(cdep_overflow_guard_test PRIVATE opennova_terrain opennova_cpt)
target_include_directories(cdep_overflow_guard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cdep_overflow_guard COMMAND cdep_overflow_guard_test)

# Dvxi5 repro: exercises a post-clamp heightmap with block ranges at the
# CDEP ceiling. Kept as a regression anchor for the fixed bake pipeline.
add_executable(dvxi5_repro_test trngen/dvxi5_repro_test.cpp)
target_link_libraries(dvxi5_repro_test PRIVATE opennova_terrain)
target_include_directories(dvxi5_repro_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    # /EHa so the SEH translator in main() can convert access violations
    # into std::runtime_error instead of bypassing C++ catch blocks.
    target_compile_options(dvxi5_repro_test PRIVATE /EHa)
endif()
add_test(NAME dvxi5_repro COMMAND dvxi5_repro_test)
