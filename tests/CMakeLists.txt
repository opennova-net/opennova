# Machine-local retail data gates (tests/common/retail_paths.h, the two
# OPENNOVA_* roots in docs/asset-gated-tests.md): a gated test prints
# "SKIP: needs <what>" and exits 77 when its data is absent, so ctest reports it
# Skipped, never Passed. A test whose synthetic legs ran prints "SKIP-LEG:
# needs <what>" for its missing retail leg and exits 0 (a plain add_test).
function(opennova_add_gated_test name)
    add_test(NAME ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 LABELS retail)
endfunction()

# Mixed binaries expose synthetic cases without resolving either retail root.
# The separately labeled compatibility entry also runs their retail assertions.
# Serialize each pair because some legacy format tests share scratch filenames.
function(opennova_add_mixed_test name)
    cmake_parse_arguments(TEST "" "WORKING_DIRECTORY" "COMMAND" ${ARGN})
    add_test(NAME ${name} COMMAND ${TEST_COMMAND})
    add_test(NAME ${name}_retail COMMAND ${TEST_COMMAND} --retail)
    set_tests_properties(${name}_retail PROPERTIES LABELS retail)
    set_tests_properties(${name} ${name}_retail PROPERTIES RESOURCE_LOCK mixed_${name})
    if(TEST_WORKING_DIRECTORY)
        set_tests_properties(${name} ${name}_retail PROPERTIES
                             WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}")
    endif()
endfunction()

# The retail-mission rig the asset-gated world/mission ctests share
# (common/retail_mission_files.h): one real mission booted through the
# engine's own mission::MissionKernel (ADR 0042 d3) and ticked on the SP
# listen frame (inmatch::HostRole) or the local role's bare no-net tick; this
# pair supplies only the retail terrain documents and the tick roles.
add_library(opennova_test_rig STATIC common/retail_mission_files.cpp)
target_include_directories(opennova_test_rig PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(opennova_test_rig
    PUBLIC opennova_net opennova_runtime opennova_formats opennova_base)

# The retail-root seam the gated tests share: a root resolves only to an
# existing directory, so a root naming a missing folder skips like an unset one.
add_executable(retail_paths_test common/retail_paths_test.cpp)
target_compile_features(retail_paths_test PRIVATE cxx_std_17)
target_include_directories(retail_paths_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME retail_paths COMMAND retail_paths_test)

# Shared io infrastructure unit tests
add_executable(io_test io/io_test.cpp)
target_link_libraries(io_test PRIVATE opennova_io)
target_compile_features(io_test PRIVATE cxx_std_17)
target_include_directories(io_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME io COMMAND io_test)

# Roundtrip test
add_executable(threedi_roundtrip_test threedi/roundtrip_test.cpp)
target_link_libraries(threedi_roundtrip_test PRIVATE opennova_formats)
target_include_directories(threedi_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_roundtrip COMMAND threedi_roundtrip_test)

# The property table an editor sets a model's fields through: every field set to
# its own value leaves the bytes (synthetic models; retail models as a SKIP-LEG
# leg), derived words follow their fields.

# Every retail model through the reader and the writer the editor saves by:
# byte-identical and a fixed point (OPENNOVA_JO_ASSETS).
add_executable(threedi_retail_rewrite_test threedi/retail_rewrite_test.cpp)
target_link_libraries(threedi_retail_rewrite_test PRIVATE opennova_formats)
target_include_directories(threedi_retail_rewrite_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(threedi_retail_rewrite COMMAND threedi_retail_rewrite_test)

# The BHD-era GP reader and its migration to 3DI3: synthetic GP models built in the test,
# and every GP model BHD ships (OPENNOVA_JO_ASSETS/fixtures/bhd/3di) migrated and read back.
add_executable(threedi_gp_test threedi/threedi_gp_test.cpp)
target_link_libraries(threedi_gp_test PRIVATE opennova_formats)
target_include_directories(threedi_gp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(threedi_gp COMMAND threedi_gp_test)

# Every retail .bad and .adm through the reader and the writer the editor
# saves by: field-equal and a fixed point (OPENNOVA_JO_ASSETS).
add_executable(anim_retail_rewrite_test anim/retail_rewrite_test.cpp)
target_link_libraries(anim_retail_rewrite_test PRIVATE opennova_formats)
target_include_directories(anim_retail_rewrite_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(anim_retail_rewrite COMMAND anim_retail_rewrite_test)

add_executable(threedi_compare_test threedi/compare_test.cpp threedi/threedi_compare.cpp)
target_link_libraries(threedi_compare_test PRIVATE opennova_formats)
target_include_directories(threedi_compare_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_compare COMMAND threedi_compare_test)

# Retail global control-register catalog.
add_executable(threedi_ctrl_catalog_test threedi/ctrl_catalog_test.cpp)
target_link_libraries(threedi_ctrl_catalog_test PRIVATE opennova_formats)
target_include_directories(threedi_ctrl_catalog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_ctrl_catalog COMMAND threedi_ctrl_catalog_test)

# opennova-3di (the Blender add-on's CLI): build the fixture scenes, read the
# minted models back and check the conversions the CLI owns.
add_test(NAME opennova_3di_build
         COMMAND opennova_3di build ${CMAKE_SOURCE_DIR}/fixtures/threedi/o3d/spinner.o3d
                 -o ${CMAKE_CURRENT_BINARY_DIR}/spinner.3di)
set_tests_properties(opennova_3di_build PROPERTIES FIXTURES_SETUP opennova_3di_spinner)
add_executable(threedi_o3d_cli_test threedi/o3d_cli_test.cpp)
target_link_libraries(threedi_o3d_cli_test PRIVATE opennova_formats)
add_test(NAME threedi_o3d_cli COMMAND threedi_o3d_cli_test ${CMAKE_CURRENT_BINARY_DIR}/spinner.3di)
set_tests_properties(threedi_o3d_cli PROPERTIES FIXTURES_REQUIRED opennova_3di_spinner)
add_test(NAME opennova_3di_build_skinned
         COMMAND opennova_3di build ${CMAKE_SOURCE_DIR}/fixtures/threedi/o3d/skinned.o3d
                 -o ${CMAKE_CURRENT_BINARY_DIR}/skinned.3di)
set_tests_properties(opennova_3di_build_skinned PROPERTIES FIXTURES_SETUP opennova_3di_skinned)
add_test(NAME threedi_o3d_skinned COMMAND threedi_o3d_cli_test --skinned ${CMAKE_CURRENT_BINARY_DIR}/skinned.3di)
set_tests_properties(threedi_o3d_skinned PROPERTIES FIXTURES_REQUIRED opennova_3di_skinned)
add_test(NAME opennova_3di_build_building
         COMMAND opennova_3di build ${CMAKE_SOURCE_DIR}/fixtures/threedi/o3d/building.o3d
                 -o ${CMAKE_CURRENT_BINARY_DIR}/building.3di)
set_tests_properties(opennova_3di_build_building PROPERTIES FIXTURES_SETUP opennova_3di_building)
add_test(NAME threedi_o3d_building COMMAND threedi_o3d_cli_test --building ${CMAKE_CURRENT_BINARY_DIR}/building.3di)
set_tests_properties(threedi_o3d_building PROPERTIES FIXTURES_REQUIRED opennova_3di_building)
# `scene` is build's inverse: build -> scene -> build re-mints each fixture
# byte for byte.
foreach(fixture spinner skinned building)
    add_test(NAME opennova_3di_scene_${fixture}
             COMMAND opennova_3di scene ${CMAKE_CURRENT_BINARY_DIR}/${fixture}.3di
                     -o ${CMAKE_CURRENT_BINARY_DIR}/${fixture}.rt.o3d)
    add_test(NAME opennova_3di_rebuild_${fixture}
             COMMAND opennova_3di build ${CMAKE_CURRENT_BINARY_DIR}/${fixture}.rt.o3d
                     -o ${CMAKE_CURRENT_BINARY_DIR}/${fixture}.rt.3di)
    add_test(NAME opennova_3di_roundtrip_${fixture}
             COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/${fixture}.3di
                     ${CMAKE_CURRENT_BINARY_DIR}/${fixture}.rt.3di)
    set_tests_properties(opennova_3di_scene_${fixture} PROPERTIES
                         FIXTURES_REQUIRED opennova_3di_${fixture} FIXTURES_SETUP threedi_scene_${fixture})
    set_tests_properties(opennova_3di_rebuild_${fixture} PROPERTIES
                         FIXTURES_REQUIRED threedi_scene_${fixture} FIXTURES_SETUP threedi_rebuild_${fixture})
    set_tests_properties(opennova_3di_roundtrip_${fixture} PROPERTIES FIXTURES_REQUIRED threedi_rebuild_${fixture})
endforeach()
# The anim commands over the same handlers the CLI dispatches: the clip-set
# round trip, what compare calls the same animation, and the scenes build refuses.
add_executable(anim_o3a_commands_test anim/o3a_commands_test.cpp)
target_link_libraries(anim_o3a_commands_test PRIVATE opennova_3di_commands)
add_test(NAME anim_o3a_commands COMMAND anim_o3a_commands_test ${CMAKE_CURRENT_BINARY_DIR})
add_executable(weapon_timing_test anim/weapon_timing_test.cpp)
target_link_libraries(weapon_timing_test PRIVATE opennova_3di_commands)
target_include_directories(weapon_timing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(weapon_timing COMMAND weapon_timing_test ${CMAKE_CURRENT_BINARY_DIR})
# The Blender add-on's tests, headless, when Blender is found (set
# BLENDER_EXECUTABLE when it is not on PATH): every tests/blender/*_test.py
# loads the source tree's add-on through tests/blender/addon_harness.py and
# runs the opennova-3di built here, authoring what it exports from scratch.
# Mixed rows: the `_retail` entry passes --retail, which selects the legs that
# read OPENNOVA_JO_ASSETS (addon_harness.retail_asset).
find_program(BLENDER_EXECUTABLE NAMES blender)
if(BLENDER_EXECUTABLE)
    file(GLOB OPENNOVA_BLENDER_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/blender/*_test.py)
    foreach(test ${OPENNOVA_BLENDER_TESTS})
        get_filename_component(name ${test} NAME_WE)
        opennova_add_mixed_test(blender_${name} COMMAND ${BLENDER_EXECUTABLE}
            --background --factory-startup --python-exit-code 1 --python ${test} -- $<TARGET_FILE:opennova_3di>)
    endforeach()
endif()
# `catalog` prints the 252 anim slot keys, the weapon actions an author times
# with their slot keys, and the event trigger bits a front end offers (the
# Blender add-on parses them), and `info` refuses a flag it does not take.
add_test(NAME opennova_3di_catalog_anim COMMAND opennova_3di catalog)
set_tests_properties(opennova_3di_catalog_anim PROPERTIES PASS_REGULAR_EXPRESSION
    "animslot 0 anim_reset[\r\n]+.*animslot 140 anim_guard[\r\n]+.*animslot 239 anim_death_bullet_leftfoot_left[\r\n]+.*animslot 251 anim_wpn_scopedown[\r\n]+weaponaction idle 241 anim_wpn_idle[\r\n]+weaponaction emptyidle 242 anim_wpn_empty_idle[\r\n]+.*weaponaction scopedown 251 anim_wpn_scopedown[\r\n]+trigger 0x1 FOOT_LEFT.*trigger 0x10 FIRE_MARKER3.*trigger 0x400 FOLEY_6")
add_test(NAME opennova_3di_anim_info_flag COMMAND opennova_3di anim info unread.adm --bogus)
set_tests_properties(opennova_3di_anim_info_flag PROPERTIES PASS_REGULAR_EXPRESSION
    "anim info takes --verbose or --keys")
add_test(NAME opennova_3di_info_flag COMMAND opennova_3di info unread.3di --bogus)
set_tests_properties(opennova_3di_info_flag PROPERTIES PASS_REGULAR_EXPRESSION
    "info takes --verbose, --planes or --verts")

# Command regressions, including invalid inputs and semantic differences.
add_executable(threedi_o3d_commands_test threedi/o3d_commands_test.cpp)
target_link_libraries(threedi_o3d_commands_test PRIVATE opennova_3di_commands)
add_test(NAME threedi_o3d_commands COMMAND threedi_o3d_commands_test ${CMAKE_CURRENT_BINARY_DIR})
# build -> scene -> build over small scenes, and the records build refuses
# (the executable too, where a refusal must say why).
add_executable(threedi_o3d_build_test threedi/o3d_build_test.cpp)
target_link_libraries(threedi_o3d_build_test PRIVATE opennova_3di_commands)
target_include_directories(threedi_o3d_build_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_o3d_build
         COMMAND threedi_o3d_build_test ${CMAKE_CURRENT_BINARY_DIR} $<TARGET_FILE:opennova_3di>)
# The .o3d lowering to the retail target: strip and palette splits, influences
# past four, authored tangent frames, and every limit at its line.
add_executable(threedi_o3d_lower_test threedi/o3d_lower_test.cpp)
target_link_libraries(threedi_o3d_lower_test PRIVATE opennova_runtime)
target_compile_features(threedi_o3d_lower_test PRIVATE cxx_std_17)
add_test(NAME threedi_o3d_lower COMMAND threedi_o3d_lower_test)
# opennova-3di over paths the ANSI code page cannot hold (the exe's UTF-8
# manifest); the test embeds the manifest so its own command line is UTF-8.
add_executable(threedi_o3d_unicode_test threedi/o3d_unicode_test.cpp)
target_compile_features(threedi_o3d_unicode_test PRIVATE cxx_std_17)
if(MSVC)
    target_sources(threedi_o3d_unicode_test PRIVATE ${CMAKE_SOURCE_DIR}/apps/3di/utf8.manifest)
endif()
add_test(NAME threedi_o3d_unicode
         COMMAND threedi_o3d_unicode_test $<TARGET_FILE:opennova_3di> ${CMAKE_CURRENT_BINARY_DIR})

# Retail clip sets through anim scene -> anim build -> anim compare (OPENNOVA_JO_ASSETS).
add_executable(anim_o3a_retail_roundtrip_test anim/o3a_retail_roundtrip_test.cpp)
target_include_directories(anim_o3a_retail_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(anim_o3a_retail_roundtrip_test PRIVATE cxx_std_17)
opennova_add_gated_test(anim_o3a_retail_roundtrip
    COMMAND anim_o3a_retail_roundtrip_test $<TARGET_FILE:opennova_3di> ${CMAKE_CURRENT_BINARY_DIR})

# The runtime plays a rebuilt clip set the way it plays the shipped one
# (OPENNOVA_JO_ASSETS).
add_executable(anim_o3a_runtime_playback_test anim/o3a_runtime_playback_test.cpp)
target_link_libraries(anim_o3a_runtime_playback_test PRIVATE opennova_runtime)
target_include_directories(anim_o3a_runtime_playback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(anim_o3a_runtime_playback_test PRIVATE cxx_std_17)
opennova_add_gated_test(anim_o3a_runtime_playback
    COMMAND anim_o3a_runtime_playback_test $<TARGET_FILE:opennova_3di> ${CMAKE_CURRENT_BINARY_DIR})

# Retail models through scene -> build -> compare (OPENNOVA_JO_ASSETS).
add_executable(threedi_o3d_retail_roundtrip_test threedi/o3d_retail_roundtrip_test.cpp)
target_include_directories(threedi_o3d_retail_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(threedi_o3d_retail_roundtrip_test PRIVATE opennova_formats)
target_compile_features(threedi_o3d_retail_roundtrip_test PRIVATE cxx_std_17)
opennova_add_gated_test(threedi_o3d_retail_roundtrip
    COMMAND threedi_o3d_retail_roundtrip_test $<TARGET_FILE:opennova_3di> ${CMAKE_CURRENT_BINARY_DIR})

# PANM unit tests
foreach(test ctrl decode pose runtime wave)
    add_executable(threedi_panm_${test}_test threedi/panm_${test}_test.cpp)
    target_link_libraries(threedi_panm_${test}_test PRIVATE opennova_formats)
    target_include_directories(threedi_panm_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME threedi_panm_${test} COMMAND threedi_panm_${test}_test)
endforeach()

# Placement ground-anchor resolution (ground userpoint, else the model origin)
add_executable(threedi_ground_anchor_test threedi/ground_anchor_test.cpp)
target_link_libraries(threedi_ground_anchor_test PRIVATE opennova_formats)
target_include_directories(threedi_ground_anchor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_ground_anchor COMMAND threedi_ground_anchor_test)

add_executable(threedi_userpoint_test threedi/userpoint_test.cpp)
target_link_libraries(threedi_userpoint_test PRIVATE opennova_formats)
target_include_directories(threedi_userpoint_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_userpoint COMMAND threedi_userpoint_test)

# Asset-gated (OPENNOVA_JO_DIR): the retail MTRL facts (Scrate1's constant
# AlphaGen on an opaque, non-alpha-tested FF_ST_OP row) the projected-shadow
# GUT witness keys on, read through the engine's 3DI3 parser.
add_executable(threedi_retail_material_facts_test threedi/retail_material_facts_test.cpp)
target_link_libraries(threedi_retail_material_facts_test PRIVATE opennova_base opennova_formats)
target_include_directories(threedi_retail_material_facts_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(threedi_retail_material_facts_test PRIVATE cxx_std_17)
opennova_add_gated_test(threedi_retail_material_facts COMMAND threedi_retail_material_facts_test)

add_executable(threedi_collision_3di_test threedi/collision_3di_test.cpp)
target_link_libraries(threedi_collision_3di_test PRIVATE opennova_formats)
target_include_directories(threedi_collision_3di_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_collision_3di COMMAND threedi_collision_3di_test)

# The synthetic 3DI model set fixtures/threedi/synth (eleven base models plus
# the one-edit variants the GUT suite loads), minted through the parity
# writer from tests/fixtures/minimal_3di_builder.h and byte-compared every run
# (--write regenerates). Every 3DI consumer below reads from that directory.
add_executable(minimal_3di_gen_test fixtures/minimal_3di_gen.cpp)
target_link_libraries(minimal_3di_gen_test PRIVATE opennova_formats)
target_compile_features(minimal_3di_gen_test PRIVATE cxx_std_17)
target_include_directories(minimal_3di_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_3di_gen COMMAND minimal_3di_gen_test)

# ADM tests. adm_parse pins the shipped mp5_1st.adm from the reference fixture
# set (gated on OPENNOVA_JO_ASSETS); the rest author their maps in memory.
foreach(test comment trim_value parse variants write)
    add_executable(adm_${test}_test anim/adm_${test}_test.cpp)
    target_link_libraries(adm_${test}_test PRIVATE opennova_formats)
    target_include_directories(adm_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
foreach(test comment trim_value variants write)
    add_test(NAME adm_${test} COMMAND adm_${test}_test)
endforeach()
opennova_add_gated_test(adm_parse COMMAND adm_parse_test)

# BAD tests: the null guard unconditionally, the shipped BINOC.bad pins as the
# reference-set SKIP-LEG leg, and 357_RST.bad's translation rows 0..frame_count
# (OPENNOVA_JO_ASSETS, SKIP-LEG).
add_executable(bad_parse_test bad/bad_parse_test.cpp)
target_link_libraries(bad_parse_test PRIVATE opennova_formats)
target_include_directories(bad_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(bad_parse COMMAND bad_parse_test)
# bad_roundtrip: the writer and the construction seam against the reader -- our
# fixtures byte-exact, a from-scratch clip field-equal and re-write-stable, the
# builder's derived bone table pinned, BINOC.bad as the SKIP-LEG leg.
add_executable(bad_roundtrip_test bad/bad_roundtrip_test.cpp)
target_link_libraries(bad_roundtrip_test PRIVATE opennova_formats)
target_include_directories(bad_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(bad_roundtrip COMMAND bad_roundtrip_test)
# bad_build: the construction seam -- the frame maps, the derived bind and
# position, the events' bottom and top written as stated, the refusals, and the
# canonical table, with SKIP-LEG retail legs over both derivations (BINOC.bad's
# bind; DT1PRONE.BAD's positions through DT1RST.BAD's bind).
add_executable(bad_build_test bad/bad_build_test.cpp)
target_link_libraries(bad_build_test PRIVATE opennova_formats)
target_include_directories(bad_build_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(bad_build COMMAND bad_build_test)

# Native BIKi menu-video decoder: a deterministic generated bitstream plus an
# OPENNOVA_JO_DIR-gated retail smoke test.
add_executable(bink_test bink/bink_test.cpp)
target_link_libraries(bink_test PRIVATE opennova_formats)
add_test(NAME bink COMMAND bink_test)

add_executable(bink_retail_test bink/bink_retail_test.cpp)
target_include_directories(bink_retail_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bink_retail_test PRIVATE opennova_formats)
opennova_add_gated_test(bink_retail COMMAND bink_retail_test)

# ANIM skeletal sampler (engine/runtime/anim): coordinate conventions, quaternion helpers, and
# world<->local self-consistency on the shipped BINOC.bad (a reference-set SKIP-LEG leg).
add_executable(anim_sample_test anim/anim_sample_test.cpp)
target_link_libraries(anim_sample_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(anim_sample_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(anim_sample_test PRIVATE cxx_std_17)
opennova_add_mixed_test(anim_sample COMMAND anim_sample_test)

# ANIM positions_from_model over the retail viewmodel corpus (the retail half of
# the former tests/test_bad_pos_derivation.py; synthetic exactness stays in
# anim_sample): healthy M16/M24/M21/Frag rigs reconstruct BadBone.position within
# 5e-4 on >=30 norm-consistent bones each; ak47_1st's X-triplicated rows (>=20)
# come back model-consistent. Asset-gated on OPENNOVA_JO_ASSETS; reports Skipped
# when unset.
add_executable(anim_positions_from_model_corpus_test anim/anim_positions_from_model_corpus_test.cpp)
target_link_libraries(anim_positions_from_model_corpus_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(anim_positions_from_model_corpus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(anim_positions_from_model_corpus_test PRIVATE cxx_std_17)
opennova_add_gated_test(anim_positions_from_model_corpus COMMAND anim_positions_from_model_corpus_test)

# ANIM aim overlay (the torso bend): bone-class map, BAM blend branches, compose
# invariants. [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md s14]
add_executable(aim_overlay_test anim/aim_overlay_test.cpp)
target_link_libraries(aim_overlay_test PRIVATE opennova_runtime)
target_include_directories(aim_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(aim_overlay_test PRIVATE cxx_std_17)
add_test(NAME aim_overlay COMMAND aim_overlay_test)

# ANIM skeletal pose (S3, ADR 0028): the time-domain window walk, blend
# endpoints, BN## tag parse, and the .bad bind-rest transpose the collision
# pose provider composes with.
add_executable(anim_skeletal_pose_test anim/skeletal_pose_test.cpp)
target_link_libraries(anim_skeletal_pose_test PRIVATE opennova_runtime)
target_include_directories(anim_skeletal_pose_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(anim_skeletal_pose_test PRIVATE cxx_std_17)
add_test(NAME anim_skeletal_pose COMMAND anim_skeletal_pose_test)

# DEF tests
add_executable(def_comment_strip_test def/def_comment_strip_test.cpp)
target_link_libraries(def_comment_strip_test PRIVATE opennova_formats)
target_include_directories(def_comment_strip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME def_comment_strip COMMAND def_comment_strip_test)

add_executable(def_flag_parity_test def/def_flag_parity_test.cpp)
target_link_libraries(def_flag_parity_test PRIVATE opennova_formats)
add_test(NAME def_flag_parity COMMAND def_flag_parity_test)

# The def parsers. items / item_attrib / item_type read the authored tables;
# weapons, ammo, powerup and hudpos run their inline blocks unconditionally and
# pin the shipped tables from the reference fixture set as SKIP-LEG legs.
foreach(test items weapons ammo powerup hudpos item_attrib item_part_bytes item_type)
    add_executable(def_parse_${test}_test def/def_parse_${test}_test.cpp)
    target_link_libraries(def_parse_${test}_test PRIVATE opennova_formats)
    target_include_directories(def_parse_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
foreach(test items weapons ammo powerup hudpos item_attrib item_part_bytes item_type)
    if(test MATCHES "^(items|weapons|ammo|powerup|hudpos)$")
        opennova_add_mixed_test(def_parse_${test} COMMAND def_parse_${test}_test)
    else()
        add_test(NAME def_parse_${test} COMMAND def_parse_${test}_test)
    endif()
endforeach()
# The powerup retail leg reads powerup.def off the packed install through the Vfs.
target_link_libraries(def_parse_powerup_test PRIVATE opennova_base)

# hudfx.def (docs/interface/hud-re.md "hudfx.def"): the tagged lines, the one the game reads, the slots'
# names, the writer's form and an authored file written again over its layout. JO ships none.
add_executable(def_hudfx_test def/def_hudfx_test.cpp)
target_link_libraries(def_hudfx_test PRIVATE opennova_formats)
target_include_directories(def_hudfx_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME def_hudfx COMMAND def_hudfx_test)

# PLAYER_INFO loadout weight + encumbrance math (D-PLAYERINFO-11 weight readout).
add_executable(def_loadout_weight_test def/def_loadout_weight_test.cpp)
target_link_libraries(def_loadout_weight_test PRIVATE opennova_formats)
add_test(NAME def_loadout_weight COMMAND def_loadout_weight_test)

# The items.def ids and rows the engine fixes (docs/world/itemdef-re.md, "The ids and rows the
# engine fixes"): the table, its lookups and rules, and the runtime's constants for the same ids.
add_executable(def_reserved_items_test def/def_reserved_items_test.cpp)
target_link_libraries(def_reserved_items_test PRIVATE opennova_runtime)
target_include_directories(def_reserved_items_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME def_reserved_items COMMAND def_reserved_items_test)

# AVATARS (Avatars.def) tests: parse against the retail fixture + from-scratch
# writer round-trip (docs/playerinfo/avatars-re.md, docs/adr/0021).
# fixtures/avatars/synth_avatars.def: the synthetic avatar table through avatars_write.
add_executable(minimal_avatars_gen_test fixtures/minimal_avatars_gen.cpp)
target_link_libraries(minimal_avatars_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_avatars_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_avatars_gen COMMAND minimal_avatars_gen_test)

foreach(test parse roundtrip)
    # The minted fixtures/avatars/synth_avatars.def carries the shape; the shipped
    # table is each test's OPENNOVA_JO_ASSETS reference leg.
    add_executable(avatars_${test}_test avatars/avatars_${test}_test.cpp)
    target_link_libraries(avatars_${test}_test PRIVATE opennova_base opennova_formats)
    target_include_directories(avatars_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(avatars_${test}_test PRIVATE cxx_std_17)
    opennova_add_mixed_test(avatars_${test} COMMAND avatars_${test}_test)
endforeach()

# Mission (BMS) tests
add_executable(mission_authoring_test mission/mission_authoring_test.cpp)
target_link_libraries(mission_authoring_test PRIVATE opennova_formats)
target_include_directories(mission_authoring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_authoring COMMAND mission_authoring_test)

add_executable(mission_placement_traits_test mission/placement_traits_test.cpp)
target_link_libraries(mission_placement_traits_test PRIVATE opennova_runtime)
target_include_directories(mission_placement_traits_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_placement_traits_test PRIVATE cxx_std_17)
add_test(NAME mission_placement_traits COMMAND mission_placement_traits_test)

add_executable(mission_static_sources_test mission/static_sources_test.cpp)
target_link_libraries(mission_static_sources_test PRIVATE opennova_runtime)
target_include_directories(mission_static_sources_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_static_sources COMMAND mission_static_sources_test)

add_executable(mission_catalog_test mission/mission_catalog_test.cpp)
target_link_libraries(mission_catalog_test PRIVATE opennova_runtime)
target_include_directories(mission_catalog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_catalog_test PRIVATE cxx_std_17)
add_test(NAME mission_catalog COMMAND mission_catalog_test)

# The files the game finds by a mission's name (mission/mission_sidecars.h): the
# six witnessed rows and the names they build.
add_executable(mission_sidecars_test mission/mission_sidecars_test.cpp)
target_link_libraries(mission_sidecars_test PRIVATE opennova_runtime)
target_include_directories(mission_sidecars_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_sidecars_test PRIVATE cxx_std_17)
add_test(NAME mission_sidecars COMMAND mission_sidecars_test)

# The mission load plan (mission/mission_load_plan.h): the stage order and the
# witnessed loading-screen anchors the shell presents.
add_executable(mission_load_plan_test mission/mission_load_plan_test.cpp)
target_link_libraries(mission_load_plan_test PRIVATE opennova_runtime)
add_test(NAME mission_load_plan COMMAND mission_load_plan_test)

# fixtures/bms/synth_dense.bms: the synthetic dense mission (four populated
# pools, zero-valued optional fields) authored through bms_edit and
# written by bms::write; mission_bms and mission_mis_idempotency read it.
add_executable(minimal_bms_gen_test fixtures/minimal_bms_gen.cpp)
target_link_libraries(minimal_bms_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_bms_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_bms_gen COMMAND minimal_bms_gen_test)

# fixtures/bms/synth_logic.bms + synth_logic.bin: the small mission with everything the
# mission document and its picture read (every row kind, a path, two events naming one
# another, the text keys its records form) and its string table, written by bms::write and
# rtxt::write; editor_mission_document and editor_document_contract read them.
add_executable(mission_set_gen_test fixtures/mission_set_gen.cpp)
target_link_libraries(mission_set_gen_test PRIVATE opennova_formats)
target_include_directories(mission_set_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_set_gen COMMAND mission_set_gen_test)

add_executable(mission_bms_test mission/mission_bms_test.cpp)
target_link_libraries(mission_bms_test PRIVATE opennova_formats)
target_include_directories(mission_bms_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_bms COMMAND mission_bms_test)

# An event's runs split out of the file's tables and joined back
# (formats/mission/mission_chains.h): the identity on a canonical file, a
# reordered one joined canonical, the refusals, an empty run's running offset.
add_executable(mission_chains_test mission/mission_chains_test.cpp)
target_link_libraries(mission_chains_test PRIVATE opennova_formats)
target_include_directories(mission_chains_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_chains COMMAND mission_chains_test)

# What each trigger's and action's parameters are, as rows
# (formats/mission/mission_params.h): a row per sub-type and action type, the
# references they make, the sub-type names the typed views give.
add_executable(mission_params_test mission/mission_params_test.cpp)
target_link_libraries(mission_params_test PRIVATE opennova_formats)
target_include_directories(mission_params_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_params COMMAND mission_params_test)

add_executable(mission_mis_test mission/mission_mis_test.cpp)
target_link_libraries(mission_mis_test PRIVATE opennova_formats)
target_include_directories(mission_mis_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_mis_test PRIVATE cxx_std_17)
add_test(NAME mission_mis COMMAND mission_mis_test)

# .mis idempotency: .bms -> .mis (gen1) -> parse -> .mis (gen2), byte-equal, over the
# minted dense mission, plus the shipped ash_i5b as an OPENNOVA_JO_ASSETS reference leg.
add_executable(mission_mis_idempotency_test mission/mission_mis_idempotency_test.cpp)
target_link_libraries(mission_mis_idempotency_test PRIVATE opennova_formats)
target_include_directories(mission_mis_idempotency_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_mis_idempotency_test PRIVATE cxx_std_17)
opennova_add_mixed_test(mission_mis_idempotency COMMAND mission_mis_idempotency_test)

# The mission writer against the files it must write: parse->write compared with the
# bytes read, section by section, then the canonical fixed point. The ordinary row
# reads the minted synth_dense.bms; the retail leg every mission the install ships,
# out of its archives through the VFS (OPENNOVA_JO_DIR), each rewritten to its own
# bytes but the named ones whose loadout chunk is damaged (D-EVT-7).
add_executable(mission_corpus_test mission/mission_corpus_test.cpp)
target_link_libraries(mission_corpus_test PRIVATE opennova_base opennova_formats)
target_include_directories(mission_corpus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_corpus_test PRIVATE cxx_std_17)
opennova_add_mixed_test(mission_corpus COMMAND mission_corpus_test)

# LWF sound-profile tests: byte-exact parse->encode round-trip + mutate-reread
# over the synthetic fixtures/lwf/menu.lwf; the retail profiles are swept
# behind OPENNOVA_JO_ASSETS.
add_executable(lwf_roundtrip_test lwf/lwf_roundtrip_test.cpp)
target_link_libraries(lwf_roundtrip_test PRIVATE opennova_formats)
target_include_directories(lwf_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME lwf_roundtrip COMMAND lwf_roundtrip_test)

# fixtures/lwf/menu.lwf + tone.wav are minted by our own writers and
# byte-compared every run (--write regenerates).
add_executable(minimal_lwf_gen_test fixtures/minimal_lwf_gen.cpp)
target_link_libraries(minimal_lwf_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_lwf_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_lwf_gen COMMAND minimal_lwf_gen_test)

add_executable(lwf_jo_assets_sweep_test lwf/lwf_jo_assets_sweep_test.cpp)
target_link_libraries(lwf_jo_assets_sweep_test PRIVATE opennova_formats)
target_include_directories(lwf_jo_assets_sweep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(lwf_jo_assets_sweep COMMAND lwf_jo_assets_sweep_test)

add_executable(wav_pcm_test lwf/wav_pcm_test.cpp)
target_link_libraries(wav_pcm_test PRIVATE opennova_formats)
add_test(NAME wav_pcm COMMAND wav_pcm_test)

add_executable(inmatch_session_test frame/inmatch_session_test.cpp)
target_link_libraries(inmatch_session_test PRIVATE opennova_runtime)
add_test(NAME inmatch_session COMMAND inmatch_session_test)

# The menu sound profile (the synthetic fixtures/lwf/menu.lwf): trigger sets
# resolve to loose .wav members (drives MNU hover/click via MnuMenu).
add_executable(menu_lwf_test lwf/menu_lwf_test.cpp)
target_link_libraries(menu_lwf_test PRIVATE opennova_formats)
target_include_directories(menu_lwf_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_lwf COMMAND menu_lwf_test)

# score — score.ini scoring config: parse, canonical write, and the witnessed
# row-index ladder. Committed fixture is AUTHORED (fixtures/score/score_sample.ini),
# not retail data; <OPENNOVA_JO_ASSETS>/score.ini adds the retail sweep leg.
add_executable(facial_animation_test world/facial_animation_test.cpp)
target_link_libraries(facial_animation_test PRIVATE opennova_runtime)
target_include_directories(facial_animation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME facial_animation COMMAND facial_animation_test)

add_executable(teammate_spawn_test mission/teammate_spawn_test.cpp)
target_link_libraries(teammate_spawn_test PRIVATE opennova_test_rig)
target_include_directories(teammate_spawn_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME teammate_spawn COMMAND teammate_spawn_test)
set_tests_properties(teammate_spawn PROPERTIES SKIP_RETURN_CODE 77 LABELS retail)

add_executable(infantry_spawn_test world/infantry_spawn_test.cpp)
target_link_libraries(infantry_spawn_test PRIVATE opennova_runtime)
target_include_directories(infantry_spawn_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(infantry_spawn COMMAND infantry_spawn_test)

add_executable(teammate_operations_test world/teammate_operations_test.cpp)
target_link_libraries(teammate_operations_test PRIVATE opennova_runtime)
target_include_directories(teammate_operations_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME teammate_operations COMMAND teammate_operations_test)

add_executable(infantry_escort_test world/infantry_escort_test.cpp)
target_link_libraries(infantry_escort_test PRIVATE opennova_runtime)
target_include_directories(infantry_escort_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME infantry_escort COMMAND infantry_escort_test)

add_executable(grm_roundtrip_test grm/grm_roundtrip_test.cpp)
target_link_libraries(grm_roundtrip_test PRIVATE opennova_formats)
target_include_directories(grm_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(grm_roundtrip COMMAND grm_roundtrip_test)

add_executable(score_roundtrip_test score/score_roundtrip_test.cpp)
target_link_libraries(score_roundtrip_test PRIVATE opennova_formats)
target_include_directories(score_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(score_roundtrip COMMAND score_roundtrip_test)

# DBF dialog bank: byte-exact round-trip + dlg-id -> def-id mapping over the
# minted fixtures/dbf/synth_bank.dbf, plus the shipped 00TRg.DBF as an
# OPENNOVA_JO_ASSETS reference-fixture leg.
add_executable(minimal_dbf_gen_test fixtures/minimal_dbf_gen.cpp)
target_link_libraries(minimal_dbf_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_dbf_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_dbf_gen COMMAND minimal_dbf_gen_test)

add_executable(dbf_roundtrip_test dbf/dbf_roundtrip_test.cpp)
target_link_libraries(dbf_roundtrip_test PRIVATE opennova_formats)
target_include_directories(dbf_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(dbf_roundtrip COMMAND dbf_roundtrip_test)

# Audio sound-set member selection state machine (FIRST/RANDOM/SEQUENTIAL/RANDOM_SEQ),
# pushed down from sound_bank.gd into engine/runtime/audio.
add_executable(sound_selector_test audio/sound_selector_test.cpp)
target_link_libraries(sound_selector_test PRIVATE opennova_runtime)
target_include_directories(sound_selector_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sound_selector_test PRIVATE cxx_std_17)
add_test(NAME sound_selector COMMAND sound_selector_test)

# The footstep slot pick's witnessed test order (water > on-entity > snow >
# ground), shared by the authority and wire-fed body channels.
add_executable(footstep_slot_test audio/footstep_slot_test.cpp)
target_link_libraries(footstep_slot_test PRIVATE opennova_runtime)
target_include_directories(footstep_slot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(footstep_slot_test PRIVATE cxx_std_17)
add_test(NAME footstep_slot COMMAND footstep_slot_test)

# Ambient marker emitter system: the staggered tick&7 registration cadence, tick-unit
# slot keep-alives, and the per-frame live-slot mix, pushed down from
# mission_audio.gd (docs/audio/lwf-dbf-sound-re.md §driver cadence, D-SND-16).
add_executable(ambient_mixer_test audio/ambient_mixer_test.cpp)
target_link_libraries(ambient_mixer_test PRIVATE opennova_runtime)
target_include_directories(ambient_mixer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ambient_mixer_test PRIVATE cxx_std_17)
add_test(NAME ambient_mixer COMMAND ambient_mixer_test)

# The ambient channel pool (runtime/audio/ambient_channel_pool.h): rank-order
# binds up to the budget, incumbents keep their channel, dropouts release,
# unresolvable entrants cached out, a pinned pool never grows.
add_executable(ambient_channel_pool_test audio/ambient_channel_pool_test.cpp)
target_link_libraries(ambient_channel_pool_test PRIVATE opennova_runtime)
target_compile_features(ambient_channel_pool_test PRIVATE cxx_std_17)
add_test(NAME ambient_channel_pool COMMAND ambient_channel_pool_test)

# S13 (ADR 0028): the envs-marker resolution — the items.def dispatch-tag gate across
# entity pools, the authored soundloop slots passed through unfiltered, walk order,
# and the BMS type-id -> items.def id offset. [orig: @0x82ABD4 -> @0x4a8080]
add_executable(envs_markers_test audio/envs_markers_test.cpp)
target_link_libraries(envs_markers_test PRIVATE opennova_runtime)
add_test(NAME audio_envs_markers COMMAND envs_markers_test)

add_executable(bank_chain_test audio/bank_chain_test.cpp)
target_link_libraries(bank_chain_test PRIVATE opennova_runtime)
add_test(NAME audio_bank_chain COMMAND bank_chain_test)

# The loaded-bank set index (case-insensitive, first bank wins) and the 3D
# one-shot fire decision (set-range cull, occlusion inflate + recheck, the
# per-layer member pick and fire-time volume), pushed down from the Godot
# sound bank (godot/src/audio/sound_bank; ADR 0043 slice G7).
add_executable(oneshot_play_test audio/oneshot_play_test.cpp)
target_link_libraries(oneshot_play_test PRIVATE opennova_runtime)
target_include_directories(oneshot_play_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(oneshot_play_test PRIVATE cxx_std_17)
opennova_add_mixed_test(audio_oneshot_play COMMAND oneshot_play_test)

# The presenting-peer gate inside the fire-sound plays and the own-channel key
# the ready rows carry (world/fire_sound.h; sibling of world/fire_sound_test).
add_executable(fire_sound_peer_gate_test audio/fire_sound_peer_gate_test.cpp)
target_link_libraries(fire_sound_peer_gate_test PRIVATE opennova_runtime)
target_include_directories(fire_sound_peer_gate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fire_sound_peer_gate_test PRIVATE cxx_std_17)
add_test(NAME audio_fire_sound_peer_gate COMMAND fire_sound_peer_gate_test)

# The serialized dialog queue, the PlayWavList dialog-id resolution through
# the co-named .DBF and the WAC scripted-voice channel reset, pushed down from
# the Godot mission audio (godot/src/audio/mission_audio; ADR 0043 slice G7).
add_executable(dialog_queue_test audio/dialog_queue_test.cpp)
target_link_libraries(dialog_queue_test PRIVATE opennova_runtime)
target_include_directories(dialog_queue_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(dialog_queue_test PRIVATE cxx_std_17)
add_test(NAME audio_dialog_queue COMMAND dialog_queue_test)

# SndProf.def sound-profile parse + lookup (asset-gated retail spot check via JO_ASSETS).
add_executable(sound_profile_test audio/sound_profile_test.cpp)
target_link_libraries(sound_profile_test PRIVATE opennova_runtime)
target_include_directories(sound_profile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sound_profile_test PRIVATE cxx_std_17)
opennova_add_mixed_test(sound_profile COMMAND sound_profile_test)

add_executable(infantry_stance_sound_test world/infantry_stance_sound_test.cpp)
target_link_libraries(infantry_stance_sound_test PRIVATE opennova_runtime)
add_test(NAME infantry_stance_sound COMMAND infantry_stance_sound_test)

# PFF tests
add_executable(pff_unit_test pff/pff_unit_test.cpp)
target_link_libraries(pff_unit_test PRIVATE opennova_formats)
target_include_directories(pff_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pff_unit COMMAND pff_unit_test)

# The resumable PFF writer (S13 A1): byte for byte the pre-stream writer's archives at write
# budgets of 1, 7 and the whole, through the single-call forms too.
add_executable(pff_stream_writer_test pff/pff_stream_writer_test.cpp)
target_link_libraries(pff_stream_writer_test PRIVATE opennova_formats)
target_include_directories(pff_stream_writer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pff_stream_writer COMMAND pff_stream_writer_test)

# Game profile table tests
add_executable(gameprofile_test gameprofile/gameprofile_test.cpp)
target_link_libraries(gameprofile_test PRIVATE opennova_base)
target_include_directories(gameprofile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gameprofile COMMAND gameprofile_test)

add_executable(required_resources_test gameprofile/required_resources_test.cpp)
target_link_libraries(required_resources_test PRIVATE opennova_base)
add_test(NAME required_resources COMMAND required_resources_test)

# SBF tests
add_executable(sbf_header_test sbf/sbf_header_test.cpp)
target_link_libraries(sbf_header_test PRIVATE opennova_formats)
target_include_directories(sbf_header_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sbf_header COMMAND sbf_header_test)

add_executable(sbf_parse_test sbf/sbf_parse_test.cpp)
target_link_libraries(sbf_parse_test PRIVATE opennova_formats)
target_include_directories(sbf_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sbf_parse_test PRIVATE
    SBF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/sbf")
add_test(NAME sbf_parse COMMAND sbf_parse_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(sbf_decode_test sbf/sbf_decode_test.cpp)
target_link_libraries(sbf_decode_test PRIVATE opennova_formats)
target_include_directories(sbf_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sbf_decode_test PRIVATE
    SBF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/sbf")
add_test(NAME sbf_decode COMMAND sbf_decode_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(sbf_compat_test sbf/sbf_compat_test.cpp)
target_link_libraries(sbf_compat_test PRIVATE opennova_formats)
target_include_directories(sbf_compat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sbf_compat COMMAND sbf_compat_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(sbf_encode_test sbf/sbf_encode_test.cpp)
target_link_libraries(sbf_encode_test PRIVATE opennova_formats)
target_include_directories(sbf_encode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT MSVC)
    target_link_libraries(sbf_encode_test PRIVATE m)
endif()
add_test(NAME sbf_encode COMMAND sbf_encode_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(sbf_bank_test sbf/sbf_bank_test.cpp)
target_link_libraries(sbf_bank_test PRIVATE opennova_formats)
target_include_directories(sbf_bank_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sbf_bank_test PRIVATE
    SBF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/sbf")
add_test(NAME sbf_bank COMMAND sbf_bank_test)

add_executable(sbf_roundtrip_test sbf/sbf_roundtrip_test.cpp)
target_link_libraries(sbf_roundtrip_test PRIVATE opennova_formats)
target_include_directories(sbf_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sbf_roundtrip_test PRIVATE
    SBF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/sbf")
if(NOT MSVC)
    target_link_libraries(sbf_roundtrip_test PRIVATE m)
endif()
add_test(NAME sbf_roundtrip COMMAND sbf_roundtrip_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The synthetic bank the SBF tests read (fixtures/sbf/synth_gamemus.sbf) is
# minted by our own encoder and byte-compared every run (--write regenerates).
add_executable(minimal_sbf_gen_test fixtures/minimal_sbf_gen.cpp)
target_link_libraries(minimal_sbf_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_sbf_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_sbf_gen COMMAND minimal_sbf_gen_test)

# The retail banks (gamemus.sbf + menumus.sbf) are swept behind OPENNOVA_JO_DIR.
add_executable(sbf_jo_install_sweep_test sbf/sbf_jo_install_sweep_test.cpp)
target_link_libraries(sbf_jo_install_sweep_test PRIVATE opennova_formats)
target_include_directories(sbf_jo_install_sweep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(sbf_jo_install_sweep COMMAND sbf_jo_install_sweep_test)

# MUS tests. fixtures/mus/synth_{gamemus,menumus}.bin and the decompile golden
# golden_synth_gamemus.mus.txt: two authored programs through mus_compile +
# mus_encode_file; the shipped jo_gamemus.bin / jo_menumus.bin (their text pinned by
# mus_decompile's retail leg and compiled back byte for byte, no golden of them)
# are each fixture test's OPENNOVA_JO_ASSETS reference leg.
add_executable(synth_mus_gen_test fixtures/synth_mus_gen.cpp)
target_link_libraries(synth_mus_gen_test PRIVATE opennova_formats)
target_include_directories(synth_mus_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME synth_mus_gen COMMAND synth_mus_gen_test)

add_executable(mus_header_test mus/mus_header_test.cpp)
target_link_libraries(mus_header_test PRIVATE opennova_formats)
target_include_directories(mus_header_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mus_header COMMAND mus_header_test)

add_executable(mus_parse_test mus/mus_parse_test.cpp)
target_link_libraries(mus_parse_test PRIVATE opennova_formats)
target_include_directories(mus_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_parse_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
opennova_add_mixed_test(mus_parse COMMAND mus_parse_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_compat_test mus/mus_compat_test.cpp)
target_link_libraries(mus_compat_test PRIVATE opennova_formats)
target_include_directories(mus_compat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_compat_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
opennova_add_mixed_test(mus_compat COMMAND mus_compat_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_decompile_test mus/mus_decompile_test.cpp)
target_link_libraries(mus_decompile_test PRIVATE opennova_formats)
target_include_directories(mus_decompile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_decompile_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
opennova_add_mixed_test(mus_decompile COMMAND mus_decompile_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_compile_test mus/mus_compile_test.cpp)
target_link_libraries(mus_compile_test PRIVATE opennova_formats)
target_include_directories(mus_compile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mus_compile COMMAND mus_compile_test)

add_executable(mus_roundtrip_test mus/mus_roundtrip_test.cpp)
target_link_libraries(mus_roundtrip_test PRIVATE opennova_formats)
target_include_directories(mus_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_roundtrip_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
opennova_add_mixed_test(mus_roundtrip COMMAND mus_roundtrip_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_names_roundtrip_test mus/mus_names_roundtrip_test.cpp)
target_link_libraries(mus_names_roundtrip_test PRIVATE opennova_formats)
target_include_directories(mus_names_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_names_roundtrip_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
opennova_add_mixed_test(mus_names_roundtrip COMMAND mus_names_roundtrip_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Entry-section round-trip: the compiler hardcodes entry_section_index = 0, so a
# faithful round-trip needs the original entry to be its lowest-offset section
# (true for both stock bins). Pins that the recompiled entry names the same
# section -- a drift the text round-trip and encode-idempotence cannot see.
add_executable(mus_entry_roundtrip_test mus/mus_entry_roundtrip_test.cpp)
target_link_libraries(mus_entry_roundtrip_test PRIVATE opennova_base opennova_formats)
target_include_directories(mus_entry_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_entry_roundtrip_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
opennova_add_mixed_test(mus_entry_roundtrip COMMAND mus_entry_roundtrip_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Authored-program behavior: compile the canonical .mus TEXT the visual editor
# emits (play, if/else, on-switch + default, enter-transition, assign, inc/dec,
# method/volume) and assert the observable event stream on the VM. The companion
# mus_diff_jointops.py (local RE tooling; docs/audio/mus-sbf-re.md) runs the SAME
# corpus through the original Jointops handlers; this committed test locks the
# behavior without that binary.
add_executable(mus_authored_behavioral_test mus/mus_authored_behavioral_test.cpp)
target_link_libraries(mus_authored_behavioral_test PRIVATE opennova_formats)
target_include_directories(mus_authored_behavioral_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mus_authored_behavioral_test PRIVATE cxx_std_17)
add_test(NAME mus_authored_behavioral COMMAND mus_authored_behavioral_test)

add_executable(mus_vm_test mus/mus_vm_test.cpp)
target_link_libraries(mus_vm_test PRIVATE opennova_formats)
target_include_directories(mus_vm_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_vm_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
opennova_add_mixed_test(mus_vm COMMAND mus_vm_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_vm_fixes_test mus/mus_vm_fixes_test.cpp)
target_link_libraries(mus_vm_fixes_test PRIVATE opennova_formats)
target_include_directories(mus_vm_fixes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mus_vm_fixes COMMAND mus_vm_fixes_test)

# Write-path parity: a program decompiled to its MUS text, compiled and encoded again is its own bytes (the
# compiler writes MDEdit's layout): the minted pair in core; the reference pair and every music script the
# install's archives hold (base and each expansion) as the retail leg. See the test file header.
add_executable(mus_encode_idempotence_test mus/mus_encode_idempotence_test.cpp)
target_link_libraries(mus_encode_idempotence_test PRIVATE opennova_base opennova_formats)
target_include_directories(mus_encode_idempotence_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_encode_idempotence_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
opennova_add_mixed_test(mus_encode_idempotence COMMAND mus_encode_idempotence_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# VFS tests (mount precedence, mount_game expansion override, SCR decode-on-read)
add_executable(vfs_test vfs/vfs_test.cpp)
target_link_libraries(vfs_test PRIVATE opennova_base opennova_formats)
target_include_directories(vfs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vfs_test PRIVATE cxx_std_17)
add_test(NAME vfs COMMAND vfs_test)

# An install root deeper than MAX_PATH under a non-ASCII name: the VFS, the resource
# index and the session's _NSTMOUT.TXT read through base/io/os_path.h.
add_executable(vfs_long_path_test vfs/vfs_long_path_test.cpp)
target_link_libraries(vfs_long_path_test PRIVATE opennova_base opennova_formats)
target_include_directories(vfs_long_path_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vfs_long_path_test PRIVATE cxx_std_17)
add_test(NAME vfs_long_path COMMAND vfs_long_path_test)

add_executable(vfs_mus_decode_test vfs/vfs_mus_decode_test.cpp)
target_link_libraries(vfs_mus_decode_test PRIVATE opennova_base)
target_include_directories(vfs_mus_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vfs_mus_decode_test PRIVATE cxx_std_17)
add_test(NAME vfs_mus_decode COMMAND vfs_mus_decode_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# BFC1 tests
add_executable(bfc1_unit_test bfc1/bfc1_unit_test.cpp)
target_link_libraries(bfc1_unit_test PRIVATE opennova_formats)
target_include_directories(bfc1_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bfc1_unit COMMAND bfc1_unit_test)

# SCR tests
add_executable(scr_unit_test scr/scr_unit_test.cpp)
target_link_libraries(scr_unit_test PRIVATE opennova_formats)
target_include_directories(scr_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME scr_unit COMMAND scr_unit_test)

# RTXT localized string table tests
foreach(test roundtrip byte_equal lookup strip_hotkey empty section_lookup)
    add_executable(rtxt_${test}_test rtxt/${test}_test.cpp)
    target_link_libraries(rtxt_${test}_test PRIVATE opennova_formats)
    target_include_directories(rtxt_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rtxt)
    add_test(NAME rtxt_${test} COMMAND rtxt_${test}_test)
endforeach()

# Synthetic RTXT parity fixtures: byte-exact generation and round trips.
add_executable(minimal_rtxt_gen_test fixtures/minimal_rtxt_gen.cpp)
target_link_libraries(minimal_rtxt_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_rtxt_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(minimal_rtxt_gen_test PRIVATE
    RTXT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/rtxt")
add_test(NAME minimal_rtxt_gen COMMAND minimal_rtxt_gen_test)

# Synthetic fonts: validity, byte-stable round trips and drawable glyphs.
# The builder mints
# assets/opennova.fnt and fixtures/fnt/synth_{1,3}page.fnt, byte-compared every
# run (--write regenerates).
add_executable(minimal_fnt_gen_test fixtures/minimal_fnt_gen.cpp)
target_link_libraries(minimal_fnt_gen_test PRIVATE opennova_editor) # the art lives with the editor's blank factories
target_include_directories(minimal_fnt_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_fnt_gen COMMAND minimal_fnt_gen_test)

# fixtures/cbin: the synthetic credits lists (synth_nlist*.kda through
# cbin::encode; the shipped trio is cbin_roundtrip's reference-tree leg),
# credits_image.png (the 8x8 PNG the credits tests stage under the image name
# the minted .kda references; a self-contained PNG writer) and particle_dot.tga
# (the effect-world test's particle sprite).
add_executable(minimal_cbin_gen_test fixtures/minimal_cbin_gen.cpp)
target_link_libraries(minimal_cbin_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_cbin_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_cbin_gen COMMAND minimal_cbin_gen_test)

# fixtures/env/synth_full.env + the sky maps it names (cloud01.pcx, cloud01b.pcx):
# the synthetic environment via save_env and the PCX pair
# minted by encode_pcx_indexed, all from integer data.
add_executable(minimal_env_gen_test fixtures/minimal_env_gen.cpp)
target_link_libraries(minimal_env_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_env_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_env_gen COMMAND minimal_env_gen_test)

# RTXT parity over the minted synthetic string tables (format invariants + byte
# roundtrip); the same checks over the retail bins are the gated install sweep.
add_executable(rtxt_synth_parity_test rtxt/synth_parity_test.cpp)
target_link_libraries(rtxt_synth_parity_test PRIVATE opennova_formats)
target_include_directories(rtxt_synth_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rtxt)
target_compile_definitions(rtxt_synth_parity_test PRIVATE
    RTXT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/rtxt")
add_test(NAME rtxt_synth_parity COMMAND rtxt_synth_parity_test)

# Full-install RTXT sweep through the engine-faithful VFS (skips unless OPENNOVA_JO_DIR set)
add_executable(rtxt_jo_install_sweep_test rtxt/jo_install_sweep_test.cpp)
target_include_directories(rtxt_jo_install_sweep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rtxt_jo_install_sweep_test PRIVATE opennova_formats opennova_base)
target_include_directories(rtxt_jo_install_sweep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rtxt)
opennova_add_gated_test(rtxt_jo_install_sweep COMMAND rtxt_jo_install_sweep_test)

# Resource directory index tests
add_executable(resource_index_test resource_index/resource_index_test.cpp)
target_link_libraries(resource_index_test PRIVATE opennova_base)
target_include_directories(resource_index_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME resource_index COMMAND resource_index_test)

# Launch flags + the boot resource-dir ladder (base/resource_index/boot_policy.h).
add_executable(boot_policy_test resource_index/boot_policy_test.cpp)
target_link_libraries(boot_policy_test PRIVATE opennova_base)
target_include_directories(boot_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(boot_policy_test PRIVATE cxx_std_17)
add_test(NAME boot_policy COMMAND boot_policy_test)

# opennova-extract mounts an install as a launch with its flags does (apps/extract).
add_executable(extract_cli_test resource_index/extract_cli_test.cpp)
target_link_libraries(extract_cli_test PRIVATE opennova_formats)
target_include_directories(extract_cli_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(extract_cli_test PRIVATE cxx_std_17)
add_dependencies(extract_cli_test opennova_extract)
add_test(NAME extract_cli COMMAND extract_cli_test $<TARGET_FILE:opennova_extract> ${CMAKE_CURRENT_BINARY_DIR})

# The texture candidate order the Godot resolvers and opennova-3di share
# (base/resource_index/texture_candidates.h).
add_executable(texture_candidates_test resource_index/texture_candidates_test.cpp)
target_link_libraries(texture_candidates_test PRIVATE opennova_base)
target_include_directories(texture_candidates_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(texture_candidates_test PRIVATE cxx_std_17)
add_test(NAME texture_candidates COMMAND texture_candidates_test)

add_executable(resource_index_mus_test resource_index/resource_index_mus_test.cpp)
target_link_libraries(resource_index_mus_test PRIVATE opennova_base)
target_include_directories(resource_index_mus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME resource_index_mus COMMAND resource_index_mus_test)

# ResourceIndex over an embedder's own file set (mount_source, ADR 0046 S14: the editor's project
# files in place of an install): what it has and reads, what it does not index, its revision, and an
# AssetStore parsing a model through it.
add_executable(resource_index_source_test resource_index/resource_index_source_test.cpp)
target_link_libraries(resource_index_source_test PRIVATE opennova_runtime)
target_include_directories(resource_index_source_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME resource_index_source COMMAND resource_index_source_test)

# The presented-model lookup (world/entity_index.h): the BMS-id and origin
# candidates, groups and area-trigger zones an embedder resolves its presented
# models by.
add_executable(world_entity_index_test world/entity_index_test.cpp)
target_link_libraries(world_entity_index_test PRIVATE opennova_runtime)
target_include_directories(world_entity_index_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME world_entity_index COMMAND world_entity_index_test)

# native world: the sim-side .3di derivations (ADR 0028)
add_executable(world_model_geometry_test world/model_geometry_test.cpp)
target_link_libraries(world_model_geometry_test PRIVATE opennova_runtime)
target_include_directories(world_model_geometry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME world_model_geometry COMMAND world_model_geometry_test)

add_executable(renderer_model_controls_test renderer/model_controls_test.cpp)
target_link_libraries(renderer_model_controls_test PRIVATE opennova_runtime)
target_include_directories(renderer_model_controls_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME renderer_model_controls COMMAND renderer_model_controls_test)

add_executable(renderer_model_panm_cache_test renderer/model_panm_cache_test.cpp)
target_link_libraries(renderer_model_panm_cache_test PRIVATE opennova_runtime)
target_include_directories(renderer_model_panm_cache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME renderer_model_panm_cache COMMAND renderer_model_panm_cache_test)

add_executable(renderer_model_mesh_prepare_test renderer/model_mesh_prepare_test.cpp)
target_link_libraries(renderer_model_mesh_prepare_test PRIVATE opennova_runtime)
target_include_directories(renderer_model_mesh_prepare_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME renderer_model_mesh_prepare COMMAND renderer_model_mesh_prepare_test)

add_executable(renderer_fp_viewmodel_spec_test renderer/fp_viewmodel_spec_test.cpp)
target_link_libraries(renderer_fp_viewmodel_spec_test PRIVATE opennova_runtime)
target_include_directories(renderer_fp_viewmodel_spec_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME renderer_fp_viewmodel_spec COMMAND renderer_fp_viewmodel_spec_test)

add_executable(anim_remote_body_state_test anim/remote_body_state_test.cpp)
target_link_libraries(anim_remote_body_state_test PRIVATE opennova_runtime)
target_include_directories(anim_remote_body_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME anim_remote_body_state COMMAND anim_remote_body_state_test)

add_executable(anim_adm_root_motion_test anim/adm_root_motion_test.cpp)
target_link_libraries(anim_adm_root_motion_test PRIVATE opennova_runtime)
target_include_directories(anim_adm_root_motion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME anim_adm_root_motion COMMAND anim_adm_root_motion_test)

add_executable(anim_adm_playback_test anim/adm_playback_test.cpp)
target_link_libraries(anim_adm_playback_test PRIVATE opennova_runtime)
target_include_directories(anim_adm_playback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME anim_adm_playback COMMAND anim_adm_playback_test)

add_executable(anim_adm_clip_index_test anim/adm_clip_index_test.cpp)
target_link_libraries(anim_adm_clip_index_test PRIVATE opennova_runtime)
target_include_directories(anim_adm_clip_index_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME anim_adm_clip_index COMMAND anim_adm_clip_index_test)

# The first-person weapon rings: one table per .adm shared by every weapon
# naming it, last token first; the weapon table's load sequence; the FP
# channel's loop-wrap fade.
add_executable(anim_adm_ring_table_test anim/adm_ring_table_test.cpp)
target_link_libraries(anim_adm_ring_table_test PRIVATE opennova_runtime)
target_include_directories(anim_adm_ring_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME anim_adm_ring_table COMMAND anim_adm_ring_table_test)

# The SECONDARY (weapon) channel composition over the shipped BINOC.bad rig
# plus its BINOC_twist.bad (the two-bone twist minted once from the retired
# writer; the test asserts that provenance before probing). Both come from the
# reference fixture set; gated on OPENNOVA_JO_ASSETS.
add_executable(anim_skeletal_clips_weapon_channel_test anim/skeletal_clips_weapon_channel_test.cpp)
target_link_libraries(anim_skeletal_clips_weapon_channel_test PRIVATE opennova_runtime)
target_include_directories(anim_skeletal_clips_weapon_channel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(anim_skeletal_clips_weapon_channel COMMAND anim_skeletal_clips_weapon_channel_test)

add_executable(mission_item_traits_test mission/item_traits_test.cpp)
target_link_libraries(mission_item_traits_test PRIVATE opennova_runtime)
target_include_directories(mission_item_traits_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_item_traits COMMAND mission_item_traits_test)

add_executable(mission_seat_spec_extract_test mission/seat_spec_extract_test.cpp)
target_link_libraries(mission_seat_spec_extract_test PRIVATE opennova_runtime)
target_include_directories(mission_seat_spec_extract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_seat_spec_extract COMMAND mission_seat_spec_extract_test)

# Object renderer/shader tests
foreach(test device_fog environment_cube foliage_frame_compile foliage_frame_wind light_runtime light_scene light_terrain_pass material_classify material_eval material_texture
        particle_atlas_contract particle_frame_compiler_contract render_order
        frame_fx_effects nvg_scope_lens q3_frame render_slot_shadow
        scene_overlay state_vectors tracer_frame precipitation_frame texture_dxt texture_load_rules
        d3dx_image_codecs device_texture)
    add_executable(renderer_${test}_test renderer/${test}_test.cpp)
    target_link_libraries(renderer_${test}_test PRIVATE opennova_runtime opennova_formats)
    target_include_directories(renderer_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(renderer_${test}_test PRIVATE cxx_std_17)
    add_test(NAME renderer_${test} COMMAND renderer_${test}_test)
endforeach()

# The NORMAL-pass coverage table checked against the object pipeline manifest's coverage_source.
target_compile_definitions(renderer_material_classify_test PRIVATE
    OBJECT_PIPELINE_MANIFEST="${CMAKE_SOURCE_DIR}/godot/shaders/object/pipeline_manifest.json")

# The REN-1 T1 parity instrument reads its committed golden from the source
# tree (dump mode rewrites it there — ADR 0023).
target_compile_definitions(renderer_state_vectors_test PRIVATE
    RENDER_STATE_VECTORS_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/renderer/render_state_vectors.golden")

# Terrain format library tests
add_executable(fnt_roundtrip_test fnt/fnt_roundtrip_test.cpp)
target_link_libraries(fnt_roundtrip_test PRIVATE opennova_formats)
target_include_directories(fnt_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fnt_roundtrip_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME fnt_roundtrip COMMAND fnt_roundtrip_test)

add_executable(fnt_pack_test fnt/fnt_pack_test.cpp)
target_link_libraries(fnt_pack_test PRIVATE opennova_formats)
add_test(NAME fnt_pack COMMAND fnt_pack_test)

add_executable(pcx_roundtrip_test pcx/pcx_roundtrip_test.cpp)
target_link_libraries(pcx_roundtrip_test PRIVATE opennova_formats)
target_include_directories(pcx_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pcx_roundtrip COMMAND pcx_roundtrip_test)

# The game's TGA reader (formats/tga/tga_read.h): the unconditional flip, the
# pixel start, the forms it reads and the ones it zero-fills.
add_executable(tga_read_test tga/tga_read_test.cpp)
target_link_libraries(tga_read_test PRIVATE opennova_formats)
target_include_directories(tga_read_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tga_read COMMAND tga_read_test)

# The texture writers the editor's placeholder textures go through (S11h): every header
# byte and the pixels' order, each read back as the game's readers read the file; and the
# header size readers the editor measures textures with (S12 A3).
add_executable(tga_write_test tga/tga_write_test.cpp)
target_link_libraries(tga_write_test PRIVATE opennova_formats)
target_include_directories(tga_write_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tga_write COMMAND tga_write_test)

add_executable(dds_write_test dds/dds_write_test.cpp)
target_link_libraries(dds_write_test PRIVATE opennova_formats)
target_include_directories(dds_write_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dds_write COMMAND dds_write_test)

# What the game's readers make of a texture file (ADR 0046 S18, the texture document; the TGA reader's
# is tga_read): D3DX's DDS loader (its pixel-format table, the levels, what it refuses) and the PCX
# reader's indices and palette, each over files minted by our writers or here.
add_executable(dds_read_test dds/dds_read_test.cpp)
target_link_libraries(dds_read_test PRIVATE opennova_formats)
target_include_directories(dds_read_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dds_read COMMAND dds_read_test)

add_executable(pcx_game_decode_test pcx/pcx_game_decode_test.cpp)
target_link_libraries(pcx_game_decode_test PRIVATE opennova_formats)
target_include_directories(pcx_game_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pcx_game_decode COMMAND pcx_game_decode_test)

add_executable(til_roundtrip_test til/til_roundtrip_test.cpp)
target_link_libraries(til_roundtrip_test PRIVATE opennova_formats)
target_include_directories(til_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_roundtrip COMMAND til_roundtrip_test)

add_executable(til_foliage_blocker_test til/til_foliage_blocker_test.cpp)
target_link_libraries(til_foliage_blocker_test PRIVATE opennova_formats)
target_include_directories(til_foliage_blocker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_foliage_blocker COMMAND til_foliage_blocker_test)

add_executable(til_tsd_test til/til_tsd_test.cpp)
target_link_libraries(til_tsd_test PRIVATE opennova_formats)
target_include_directories(til_tsd_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_tsd COMMAND til_tsd_test)

add_executable(til_tile_atlas_test til/til_tile_atlas_test.cpp)
target_link_libraries(til_tile_atlas_test PRIVATE opennova_formats)
target_include_directories(til_tile_atlas_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_tile_atlas COMMAND til_tile_atlas_test)

add_executable(til_render_uv_test til/til_render_uv_test.cpp)
target_link_libraries(til_render_uv_test PRIVATE opennova_formats)
target_include_directories(til_render_uv_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_render_uv COMMAND til_render_uv_test)

add_executable(foliage_map_test foliage/foliage_map_test.cpp)
target_link_libraries(foliage_map_test PRIVATE opennova_formats)
target_include_directories(foliage_map_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME foliage_map COMMAND foliage_map_test)

add_executable(foliage_runtime_vectors_test foliage/foliage_runtime_vectors_test.cpp)
target_link_libraries(foliage_runtime_vectors_test PRIVATE opennova_formats)
target_include_directories(foliage_runtime_vectors_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(foliage_runtime_vectors_test PRIVATE cxx_std_17)
add_test(NAME foliage_runtime_vectors COMMAND foliage_runtime_vectors_test)

add_executable(trn_roundtrip_test trn/trn_roundtrip_test.cpp)
target_link_libraries(trn_roundtrip_test PRIVATE opennova_formats)
target_include_directories(trn_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME trn_roundtrip COMMAND trn_roundtrip_test)

add_executable(trn_admission_gate_test trn/trn_admission_gate_test.cpp)
target_link_libraries(trn_admission_gate_test PRIVATE opennova_formats)
target_include_directories(trn_admission_gate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME trn_admission_gate COMMAND trn_admission_gate_test)

add_executable(cpt_roundtrip_test cpt/cpt_roundtrip_test.cpp)
target_link_libraries(cpt_roundtrip_test PRIVATE opennova_formats)
target_include_directories(cpt_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cpt_roundtrip COMMAND cpt_roundtrip_test)

# ENV environment/time-of-day format tests
add_executable(env_unit_test env/env_unit_test.cpp)
target_link_libraries(env_unit_test PRIVATE opennova_formats)
target_include_directories(env_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(env_unit_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME env_unit COMMAND env_unit_test)

add_executable(env_render_unit_test env/env_render_unit_test.cpp)
target_link_libraries(env_render_unit_test PRIVATE opennova_formats)
target_include_directories(env_render_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME env_render_unit COMMAND env_render_unit_test)

# The environment runtime (engine/runtime/environment): the TOD state owner +
# weather embedding ported from the shell scripts (ADR 0034 d6 de-scripting).
add_executable(environment_state_test environment/environment_state_test.cpp)
target_link_libraries(environment_state_test PRIVATE opennova_runtime)
target_include_directories(environment_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME environment_state COMMAND environment_state_test)

# The mission-start weather seed (runtime/environment/weather_seed): the
# mission-selected ENV/BMS derivation and retail's 255-tick weather settle must
# reach clients through the same real phase-2 projection every host embeds.
# opennova_net supplies that wire projection.
add_executable(weather_seed_test environment/weather_seed_test.cpp)
target_link_libraries(weather_seed_test PRIVATE opennova_runtime opennova_net)
target_include_directories(weather_seed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME weather_seed COMMAND weather_seed_test)

# The weather home (runtime/world/weather_state): the WAC handler math, the
# mission-start initializer, the wire sample apply, and the sim tick against a
# scripted world.
add_executable(reverb_test world/reverb_test.cpp)
target_link_libraries(reverb_test PRIVATE opennova_runtime)
add_test(NAME reverb COMMAND reverb_test)

add_executable(weather_state_test world/weather_state_test.cpp)
target_link_libraries(weather_state_test PRIVATE opennova_runtime)
target_include_directories(weather_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME weather_state COMMAND weather_state_test)

# The per-item ITEMS.DEF particlefx law (runtime/world/item_effects): the
# pool / controller attrib gates, the identity aliases, and the matched-
# userpoint / origin-fallback attach plan over a synthetic model.
add_executable(item_effects_test world/item_effects_test.cpp)
target_link_libraries(item_effects_test PRIVATE opennova_runtime)
target_include_directories(item_effects_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME item_effects COMMAND item_effects_test)

# The present passes' shared law (runtime/world/present_passes): the husk
# RENDER pick (husk first, apart from the piece pick), the destroyed entity's
# presentation identity + key, and the settling graft's tilt retention.
add_executable(present_passes_test world/present_passes_test.cpp)
target_link_libraries(present_passes_test PRIVATE opennova_runtime)
target_include_directories(present_passes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME present_passes COMMAND present_passes_test)

# The precipitation pool (runtime/environment/precipitation): seed, fall, wrap,
# floor, and the drawer compile.
add_executable(precipitation_test environment/precipitation_test.cpp)
target_link_libraries(precipitation_test PRIVATE opennova_runtime)
target_include_directories(precipitation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME precipitation COMMAND precipitation_test)

# The sun glare's occlusion ray sequence (runtime/environment/glare_occlusion.h):
# one coarse unjittered ray gating two jittered fine rays, then the window tick.
add_executable(glare_occlusion_test environment/glare_occlusion_test.cpp)
target_link_libraries(glare_occlusion_test PRIVATE opennova_runtime)
target_include_directories(glare_occlusion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME glare_occlusion COMMAND glare_occlusion_test)

# The HUD view-helper math cluster (engine/runtime/hud, S15): design-space
# scaling, ALPHAFADE decay + quirk, stance Q16 scale/centering, health bands,
# message tick policy, half-bright, ammo fold + nudge, round icons, crosshair
# spread projection/sum/row/gate.
add_executable(hud_math_test hud/hud_math_test.cpp)
target_link_libraries(hud_math_test PRIVATE opennova_runtime)
add_test(NAME hud_math COMMAND hud_math_test)

add_executable(hud_medic_cross_test hud/hud_medic_cross_test.cpp)
target_link_libraries(hud_medic_cross_test PRIVATE opennova_runtime)
target_include_directories(hud_medic_cross_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hud_medic_cross COMMAND hud_medic_cross_test)

add_executable(hud_lfp_panel_test hud/hud_lfp_panel_test.cpp)
target_link_libraries(hud_lfp_panel_test PRIVATE opennova_runtime)
target_include_directories(hud_lfp_panel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hud_lfp_panel COMMAND hud_lfp_panel_test)

# The HUD config-token policy (hud/hud_config_tokens.h): persisted defaults,
# clamps and the hudcolor/huddetail/showhud action cycles.
add_executable(hud_config_tokens_test hud/hud_config_tokens_test.cpp)
target_link_libraries(hud_config_tokens_test PRIVATE opennova_runtime)
add_test(NAME hud_config_tokens COMMAND hud_config_tokens_test)

# The HUD's key-driven toggles (hud/hud_toggles.h): the gated down-edge latch,
# the shared-key cycles, the view actions, the overlay windows and the resets.
add_executable(hud_toggles_test hud/hud_toggles_test.cpp)
target_link_libraries(hud_toggles_test PRIVATE opennova_runtime)
target_compile_features(hud_toggles_test PRIVATE cxx_std_17)
add_test(NAME hud_toggles COMMAND hud_toggles_test)
# The authority's server-status page (hud/hud_server_status.h): the throttle,
# the roster grid and cells, the score list rows and the page compile with the
# quit dialog and the console lines [orig: Server_DrawStatusScreen @0x50a2d0].
add_executable(hud_server_status_test hud/hud_server_status_test.cpp)
target_link_libraries(hud_server_status_test PRIVATE opennova_runtime)
target_include_directories(hud_server_status_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hud_server_status COMMAND hud_server_status_test)

# The tip system (hud/tip_system.h): the event table, the once-counters and
# option gates, the countdown / fade / reset, the text keys and expansion, the
# escape leg, the panel draw, the "$token$" key display and the local player's
# boarding producers.
add_executable(tip_system_test hud/tip_system_test.cpp)
target_link_libraries(tip_system_test PRIVATE opennova_runtime)
target_include_directories(tip_system_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(tip_system_test PRIVATE cxx_std_17)
add_test(NAME tip_system COMMAND tip_system_test)

# The key-toggled overlay windows (hud/hud_overlay_windows.h): the block
# wrapper, the briefing pages, and the briefing / help / map-legend elements.
add_executable(hud_overlay_windows_test hud/hud_overlay_windows_test.cpp)
target_link_libraries(hud_overlay_windows_test PRIVATE opennova_runtime)
target_include_directories(hud_overlay_windows_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hud_overlay_windows COMMAND hud_overlay_windows_test)

# The loading screen spec (hud/loading_screen.h): the sidecar name rule, the
# present-due rule and the composited resource names.
add_executable(hud_loading_screen_test hud/loading_screen_test.cpp)
target_link_libraries(hud_loading_screen_test PRIVATE opennova_runtime)
add_test(NAME hud_loading_screen COMMAND hud_loading_screen_test)

add_executable(jo_env_sweep_test env/jo_env_sweep_test.cpp)
target_link_libraries(jo_env_sweep_test PRIVATE opennova_formats opennova_base)
target_include_directories(jo_env_sweep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(env_jo_install COMMAND jo_env_sweep_test)

add_executable(terrain_lighting_test terrain/terrain_lighting_test.cpp)
target_link_libraries(terrain_lighting_test PRIVATE opennova_runtime)
target_include_directories(terrain_lighting_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_lighting COMMAND terrain_lighting_test)

add_executable(terrain_texture_preprocess_test terrain/terrain_texture_preprocess_test.cpp)
target_link_libraries(terrain_texture_preprocess_test PRIVATE opennova_runtime)
target_include_directories(terrain_texture_preprocess_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_texture_preprocess COMMAND terrain_texture_preprocess_test)

add_executable(terrain_lod_family_test terrain/terrain_lod_family_test.cpp)
target_link_libraries(terrain_lod_family_test PRIVATE opennova_runtime)
target_include_directories(terrain_lod_family_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_lod_family COMMAND terrain_lod_family_test)

add_executable(terrain_view_cull_test terrain/terrain_view_cull_test.cpp)
target_link_libraries(terrain_view_cull_test PRIVATE opennova_runtime)
target_include_directories(terrain_view_cull_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_view_cull COMMAND terrain_view_cull_test)

add_executable(object_lod_test renderer/object_lod_test.cpp)
target_link_libraries(object_lod_test PRIVATE opennova_runtime)
add_test(NAME object_lod COMMAND object_lod_test)

add_executable(authored_occluder_test renderer/authored_occluder_test.cpp)
target_link_libraries(authored_occluder_test PRIVATE opennova_runtime)
add_test(NAME authored_occluder COMMAND authored_occluder_test)

add_executable(terrain_foliage_detail_collector_test terrain/terrain_foliage_detail_collector_test.cpp)
target_link_libraries(terrain_foliage_detail_collector_test PRIVATE opennova_runtime)
target_include_directories(terrain_foliage_detail_collector_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_foliage_detail_collector COMMAND terrain_foliage_detail_collector_test)

add_executable(terrain_frame_compiler_test terrain/terrain_frame_compiler_test.cpp)
target_link_libraries(terrain_frame_compiler_test PRIVATE opennova_runtime)
target_include_directories(terrain_frame_compiler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_frame_compiler COMMAND terrain_frame_compiler_test)

add_executable(terrain_tile_composition_cache_test terrain/terrain_tile_composition_cache_test.cpp)
target_link_libraries(terrain_tile_composition_cache_test PRIVATE opennova_runtime)
target_include_directories(terrain_tile_composition_cache_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_tile_composition_cache COMMAND terrain_tile_composition_cache_test)

add_executable(terrain_tile_composer_test terrain/terrain_tile_composer_test.cpp)
target_link_libraries(terrain_tile_composer_test PRIVATE opennova_runtime)
target_include_directories(terrain_tile_composer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(terrain_tile_composer COMMAND terrain_tile_composer_test)

# The page-composition thread pool (runtime/terrain/terrain_tile_composition_worker.h):
# no sources = no work, a queued job composes one 256x256 page, a rejected
# completion stays queued for a later budget, an epoch bump drops the queue.
add_executable(terrain_tile_composition_worker_test terrain/terrain_tile_composition_worker_test.cpp)
target_link_libraries(terrain_tile_composition_worker_test PRIVATE opennova_runtime)
target_compile_features(terrain_tile_composition_worker_test PRIVATE cxx_std_17)
add_test(NAME terrain_tile_composition_worker COMMAND terrain_tile_composition_worker_test)

add_executable(terrain_scorch_test terrain/terrain_scorch_test.cpp)
target_link_libraries(terrain_scorch_test PRIVATE opennova_runtime)
target_include_directories(terrain_scorch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_scorch COMMAND terrain_scorch_test)

add_executable(terrain_static_shadow_collector_test terrain/terrain_static_shadow_collector_test.cpp)
target_link_libraries(terrain_static_shadow_collector_test PRIVATE opennova_runtime)
target_include_directories(terrain_static_shadow_collector_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_static_shadow_collector COMMAND terrain_static_shadow_collector_test)

add_executable(terrain_static_shadow_planner_test terrain/terrain_static_shadow_planner_test.cpp)
target_link_libraries(terrain_static_shadow_planner_test PRIVATE opennova_runtime)
target_include_directories(terrain_static_shadow_planner_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_static_shadow_planner COMMAND terrain_static_shadow_planner_test)

add_executable(terrain_static_shadow_material_test terrain/terrain_static_shadow_material_test.cpp)
target_link_libraries(terrain_static_shadow_material_test PRIVATE opennova_runtime)
target_include_directories(terrain_static_shadow_material_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_static_shadow_material COMMAND terrain_static_shadow_material_test)

add_executable(row_stripes_test terrain/row_stripes_test.cpp)
target_link_libraries(row_stripes_test PRIVATE opennova_runtime)
target_include_directories(row_stripes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME row_stripes COMMAND row_stripes_test)

add_executable(terrain_static_shadow_raster_test terrain/terrain_static_shadow_raster_test.cpp)
target_link_libraries(terrain_static_shadow_raster_test PRIVATE opennova_runtime)
target_include_directories(terrain_static_shadow_raster_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_static_shadow_raster COMMAND terrain_static_shadow_raster_test)

add_executable(terrain_static_shadow_alpha_test terrain/terrain_static_shadow_alpha_test.cpp)
target_link_libraries(terrain_static_shadow_alpha_test PRIVATE opennova_runtime)
target_include_directories(terrain_static_shadow_alpha_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_static_shadow_alpha COMMAND terrain_static_shadow_alpha_test)

# The end-of-round overlay ladder + column layout (hud/end_round_overlay.h).
add_executable(end_round_overlay_test hud/end_round_overlay_test.cpp)
target_link_libraries(end_round_overlay_test PRIVATE opennova_runtime)
target_include_directories(end_round_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(end_round_overlay_test PRIVATE cxx_std_17)
add_test(NAME end_round_overlay COMMAND end_round_overlay_test)

# The stat.mnu RESULTLIST feed (hud/stat_screen_feed.h).
# The retained minimap marker feed layout (hud/hud_minimap_feed.h) and the
# marker rows a client draws (npruntime/minimap_markers.h).
add_executable(hud_minimap_feed_test hud/hud_minimap_feed_test.cpp)
target_link_libraries(hud_minimap_feed_test PRIVATE opennova_runtime)
target_include_directories(hud_minimap_feed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hud_minimap_feed_test PRIVATE cxx_std_17)
add_test(NAME hud_minimap_feed COMMAND hud_minimap_feed_test)

# The spinmap's corrected and newly ported legs (hud/hud_minimap_view.h):
# the bit16 squaring and width scale, the invisible bit0 mask, the pointer
# race, the ring band, the bank walk, the blip crop/flash gates, and the
# tether / FARP / zone / pool-3 / waypoint / name / tracked / route legs.
add_executable(hud_minimap_legs_test hud/hud_minimap_legs_test.cpp)
target_link_libraries(hud_minimap_legs_test PRIVATE opennova_runtime)
target_include_directories(hud_minimap_legs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hud_minimap_legs_test PRIVATE cxx_std_17)
add_test(NAME hud_minimap_legs COMMAND hud_minimap_legs_test)

# The windowed map views (hud/hud_map_view.h): the DEATH MAP window and the
# CMAP map's pan/zoom state machines, the CMAP mode-4 input, and the DEATH
# pass compile (zone letters, colours, score lines, crosshair).
add_executable(hud_map_view_test hud/hud_map_view_test.cpp)
target_link_libraries(hud_map_view_test PRIVATE opennova_runtime)
target_include_directories(hud_map_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hud_map_view_test PRIVATE cxx_std_17)
add_test(NAME hud_map_view COMMAND hud_map_view_test)

# The LAN browse window (npruntime/lan_discovery.h LanDiscoveryBrowser): the
# probe identity, the 30 s window, the 3 s re-announce cadence, the reply
# filter and the endpoint-keyed rows.
add_executable(lan_discovery_test npruntime/lan_discovery_test.cpp)
target_link_libraries(lan_discovery_test PRIVATE opennova_net)
target_include_directories(lan_discovery_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(lan_discovery_test PRIVATE cxx_std_17)
add_test(NAME lan_discovery COMMAND lan_discovery_test)

add_executable(minimap_markers_test npruntime/minimap_markers_test.cpp)
target_link_libraries(minimap_markers_test PRIVATE opennova_net opennova_runtime)
target_include_directories(minimap_markers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(minimap_markers_test PRIVATE cxx_std_17)
add_test(NAME minimap_markers COMMAND minimap_markers_test)

# The map feed's v5 entity facts and the non-bank legs' gather.
add_executable(minimap_overlays_test npruntime/minimap_overlays_test.cpp)
target_link_libraries(minimap_overlays_test PRIVATE opennova_net opennova_runtime)
target_include_directories(minimap_overlays_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(minimap_overlays_test PRIVATE cxx_std_17)
add_test(NAME minimap_overlays COMMAND minimap_overlays_test)

# The character registry (runtime/inmatch/character_registry.h): the packed-id
# decode, the per-side default walk, and the joiner profile projection
# (runtime/inmatch/join_character_profile.h).
add_executable(character_registry_test npruntime/character_registry_test.cpp)
target_link_libraries(character_registry_test PRIVATE opennova_runtime)
add_test(NAME character_registry COMMAND character_registry_test)

# The role feeds (inmatch/role_feeds.h): the end-round state, overlay input and stat
# rows off a joiner's replica state, the DEATH screen status, the bare-role fallbacks.
add_executable(role_feeds_test netsim/role_feeds_test.cpp)
target_link_libraries(role_feeds_test PRIVATE opennova_runtime opennova_net)
target_compile_features(role_feeds_test PRIVATE cxx_std_17)
add_test(NAME role_feeds COMMAND role_feeds_test)

# The server config flag word a joiner reads and the join entry's step
# (inmatch/server_flags.h): the BuildFlags literals, the undiscovered word,
# dial / enumerate / prompt.
add_executable(join_entry_test netsim/join_entry_test.cpp)
target_link_libraries(join_entry_test PRIVATE opennova_runtime opennova_net)
target_compile_features(join_entry_test PRIVATE cxx_std_17)
add_test(NAME join_entry COMMAND join_entry_test)

add_executable(stat_screen_feed_test hud/stat_screen_feed_test.cpp)
target_link_libraries(stat_screen_feed_test PRIVATE opennova_runtime opennova_net)
target_include_directories(stat_screen_feed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(stat_screen_feed_test PRIVATE cxx_std_17)
add_test(NAME stat_screen_feed COMMAND stat_screen_feed_test)

# The S2C 0x81 hit-confirm tone ladder (hud/score_fanfare.h).
add_executable(score_fanfare_test hud/score_fanfare_test.cpp)
target_link_libraries(score_fanfare_test PRIVATE opennova_runtime)
target_include_directories(score_fanfare_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(score_fanfare_test PRIVATE cxx_std_17)
add_test(NAME score_fanfare COMMAND score_fanfare_test)

# The HUD's game-text compositions (hud/hud_game_text.h) and the feed row's gametext
# resolve (hud/feed_format.h feed_row_line).
add_executable(hud_game_text_test hud/hud_game_text_test.cpp)
target_link_libraries(hud_game_text_test PRIVATE opennova_runtime)
add_test(NAME hud_game_text COMMAND hud_game_text_test)

# The CMAP RULES tab's text (hud/session_rules_text.h) over the S2C 0x58 fold and
# the 0x7E briefing strings (replication client_state.h), the serializer's key 9.
add_executable(session_rules_text_test hud/session_rules_text_test.cpp)
target_link_libraries(session_rules_text_test PRIVATE opennova_runtime)
add_test(NAME session_rules_text COMMAND session_rules_text_test)

# The HUD item flash BMS action 28 sub 37 arms and the declutter level-0 side
# path it takes (hud/hud_declutter.h HudItemFlash / apply_level).
add_executable(hud_item_flash_test hud/hud_item_flash_test.cpp)
target_link_libraries(hud_item_flash_test PRIVATE opennova_runtime)
add_test(NAME hud_item_flash COMMAND hud_item_flash_test)

# The hudpos.def parse applied to the HUD layout globals (hud/hud_layout_from_hudpos.h):
# the corner rects, the positioned records, the packed colours, the stance slots and
# the last-authored static frame; the retail leg reads the shipped hudpos.def.
add_executable(hud_layout_test hud/hud_layout_test.cpp)
target_link_libraries(hud_layout_test PRIVATE opennova_runtime)
target_include_directories(hud_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(hud_layout COMMAND hud_layout_test)

add_executable(hud_frame_compiler_test hud/hud_frame_compiler_test.cpp)
target_link_libraries(hud_frame_compiler_test PRIVATE opennova_runtime)
target_include_directories(hud_frame_compiler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hud_frame_compiler COMMAND hud_frame_compiler_test)

# The flat HUD residual elements (GAMEINFO/ZONEINFO, CLOCK, TEAMID, HUDLS, the
# objectives panel, /NOHUD, the squad colours, the clip-flash key).
add_executable(hud_frame_elements_test hud/hud_frame_elements_test.cpp)
target_link_libraries(hud_frame_elements_test PRIVATE opennova_runtime)
target_include_directories(hud_frame_elements_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hud_frame_elements COMMAND hud_frame_elements_test)

# The spinmap's bit-10 legs: the dmgslice marks and the threat ring.
add_executable(hud_minimap_radar_test hud/hud_minimap_radar_test.cpp)
target_link_libraries(hud_minimap_radar_test PRIVATE opennova_runtime)
add_test(NAME hud_minimap_radar COMMAND hud_minimap_radar_test)

# The SIGHTS card row modes + the dotsize sight-scale cycle (hud/sight_overlay.h).
add_executable(sight_overlay_test hud/sight_overlay_test.cpp)
target_link_libraries(sight_overlay_test PRIVATE opennova_runtime)
add_test(NAME sight_overlay COMMAND sight_overlay_test)

add_executable(client_minimap_overlay_test netsim/client_minimap_overlay_test.cpp)
target_link_libraries(client_minimap_overlay_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_minimap_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_minimap_overlay COMMAND client_minimap_overlay_test)

# The Tab board's folded lanes (the 0x16 scoreboard + the 0x46 roster join —
# empty lists CLEAR and removals WIPE, the witnessed parser shape) plus the
# draw-time projection (replication::project_scoreboard).
add_executable(client_replica_scoreboard_test netsim/client_replica_scoreboard_test.cpp)
target_link_libraries(client_replica_scoreboard_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_replica_scoreboard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(client_replica_scoreboard_test PRIVATE cxx_std_17)
add_test(NAME client_replica_scoreboard COMMAND client_replica_scoreboard_test)

# The joiner's player walk of the friendly-tags pass over ClientState
# (D-HUD-20 residue a): roster slots with an entity, entry bails, gates, downed facts.
add_executable(client_roster_tags_test netsim/client_roster_tags_test.cpp)
target_link_libraries(client_roster_tags_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_roster_tags_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(client_roster_tags_test PRIVATE cxx_std_17)
add_test(NAME client_roster_tags COMMAND client_roster_tags_test)

# The DEATH-screen facts of the replica fold: the 0x0A sub-block-0 slot timers
# and the 0x6E self wave zone (client_replica_death.cpp).
add_executable(client_replica_death_test netsim/client_replica_death_test.cpp)
target_link_libraries(client_replica_death_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_replica_death_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(client_replica_death_test PRIVATE cxx_std_17)
add_test(NAME client_replica_death COMMAND client_replica_death_test)

add_executable(client_replica_endround_test netsim/client_replica_endround_test.cpp)
target_link_libraries(client_replica_endround_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_replica_endround_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_replica_endround COMMAND client_replica_endround_test)

# The S2C 0x23 WAC remote command end to end: the host VM's flags-0x18 arm
# queues the typed record, the codec carries it, and the joiner fold runs the
# shared handler body (wac::run_remote_command).
add_executable(script_remote_command_test netsim/script_remote_command_test.cpp)
target_link_libraries(script_remote_command_test PRIVATE opennova_net opennova_runtime)
target_include_directories(script_remote_command_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME script_remote_command COMMAND script_remote_command_test)

# The joiner's folds of the retail S2C messages the dispatch audit found without
# a faithful consumer (docs/net/retail-message-dispatch-audit.md).
add_executable(client_message_folds_test netsim/client_message_folds_test.cpp)
target_link_libraries(client_message_folds_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_message_folds_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_message_folds COMMAND client_message_folds_test)

# D-NET-285: the co-op dialog line (S2C 0x28), engine side.
add_executable(dialog_line_test netsim/dialog_line_test.cpp)
target_link_libraries(dialog_line_test PRIVATE opennova_net opennova_runtime)
add_test(NAME dialog_line COMMAND dialog_line_test)

# D-NET-282: the death screen's team change, client side.
add_executable(team_change_client_test netsim/team_change_client_test.cpp)
target_link_libraries(team_change_client_test PRIVATE opennova_net opennova_runtime)
add_test(NAME team_change_client COMMAND team_change_client_test)

# The compiled-menu interaction runtime (menu/menu_runtime.h) over a fake frame seam
# and a synthetic document: the id tree, navigation, the state store and its replay,
# ACTION dispatch, radio groups, the spin wrap, the combo popup, the input machine.
add_executable(menu_runtime_test menu/menu_runtime_test.cpp)
target_link_libraries(menu_runtime_test PRIVATE opennova_runtime)
target_compile_features(menu_runtime_test PRIVATE cxx_std_17)
add_test(NAME menu_runtime COMMAND menu_runtime_test)

# The loadout screens' shared text compositions (menu/loadout_labels.h): the WepDes
# weapon label, the ammo row, the armory row order and the weight readout.
add_executable(loadout_labels_test menu/loadout_labels_test.cpp)
target_link_libraries(loadout_labels_test PRIVATE opennova_runtime)
target_compile_features(loadout_labels_test PRIVATE cxx_std_17)
add_test(NAME loadout_labels COMMAND loadout_labels_test)

add_executable(loadout_screen_test menu/loadout_screen_test.cpp)
target_link_libraries(loadout_screen_test PRIVATE opennova_runtime)
add_test(NAME loadout_screen COMMAND loadout_screen_test)

add_executable(player_info_kit_test menu/player_info_kit_test.cpp)
target_link_libraries(player_info_kit_test PRIVATE opennova_runtime)
target_compile_features(player_info_kit_test PRIVATE cxx_std_17)
add_test(NAME player_info_kit COMMAND player_info_kit_test)

add_executable(player_info_avatars_test menu/player_info_avatars_test.cpp)
target_link_libraries(player_info_avatars_test PRIVATE opennova_runtime)
add_test(NAME player_info_avatars COMMAND player_info_avatars_test)

# The menu draw-list compiler (engine/runtime/menu, ADR 0033 R2): the
# witnessed .mnu widget draw walk — order, state-driven appearance/color
# selection, text placement/truncation, the edit caret, frame pieces, and the
# per-element int truncation (docs/mnu/menu-re.md "Widget render dispatch").
add_executable(menu_table_test menu/menu_table_test.cpp)
target_link_libraries(menu_table_test PRIVATE opennova_runtime)
target_include_directories(menu_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_table COMMAND menu_table_test)
add_executable(command_map_screen_test menu/command_map_screen_test.cpp)
target_link_libraries(command_map_screen_test PRIVATE opennova_runtime)
target_compile_features(command_map_screen_test PRIVATE cxx_std_17)
add_test(NAME command_map_screen COMMAND command_map_screen_test)
add_executable(menu_frame_compiler_test menu/menu_frame_compiler_test.cpp)
target_link_libraries(menu_frame_compiler_test PRIVATE opennova_runtime)
target_include_directories(menu_frame_compiler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_frame_compiler COMMAND menu_frame_compiler_test)

add_executable(menu_frame_parity_test menu/menu_frame_parity_test.cpp)
target_link_libraries(menu_frame_parity_test PRIVATE opennova_runtime)
target_include_directories(menu_frame_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_frame_parity COMMAND menu_frame_parity_test)

# The compiler's notes (ADR 0046 S9j2): one screen per code, the loader's notes, the notes
# only observe (the same draw list before and after them), solve_local_rect.
add_executable(menu_frame_notes_test menu/menu_frame_notes_test.cpp)
target_link_libraries(menu_frame_notes_test PRIVATE opennova_runtime)
target_include_directories(menu_frame_notes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_frame_notes COMMAND menu_frame_notes_test)

# A marquee's credits over both ConfigFile forms (runtime/menu/menu_credits.h): a
# CBIN file read as its text form reads, the minted synth_nlist trio, and the
# shipped lists from the reference fixture set (the _retail row).
add_executable(menu_credits_test menu/menu_credits_test.cpp)
target_link_libraries(menu_credits_test PRIVATE opennova_runtime)
target_include_directories(menu_credits_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(menu_credits COMMAND menu_credits_test)

# Every shipped screen through the frame compiler: the reference fixture menus
# (gated) and every .mnu the packed install serves, with its assets (SKIP-LEG).
add_executable(menu_render_corpus_test menu/menu_render_corpus_test.cpp)
target_link_libraries(menu_render_corpus_test PRIVATE opennova_runtime)
target_include_directories(menu_render_corpus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(menu_render_corpus COMMAND menu_render_corpus_test)

# The runtime's keyboard over the grill's census corpus (the .mnu at the
# OPENNOVA_JO_ASSETS root with menutxt.bin): the shipped hotkey cases (gated).
add_executable(menu_runtime_corpus_test menu/menu_runtime_corpus_test.cpp)
target_link_libraries(menu_runtime_corpus_test PRIVATE opennova_runtime)
target_include_directories(menu_runtime_corpus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(menu_runtime_corpus COMMAND menu_runtime_corpus_test)

add_executable(vehicle_collision_damage_test world/vehicle_collision_damage_test.cpp)
target_link_libraries(vehicle_collision_damage_test PRIVATE opennova_runtime)
target_include_directories(vehicle_collision_damage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME vehicle_collision_damage COMMAND vehicle_collision_damage_test)

# The menu backdrop movie policy (engine/runtime/menu/menu_video): the
# witnessed three-slot table, the STARTUP/strips draw gate, and the
# expansion-first source resolution (docs/mnu/menu-re.md "The menu backdrop").
# The shell's stylesheets: menu_style.mns then brand.mns into one list (menu/menu_style.h).
add_executable(menu_style_test menu/menu_style_test.cpp)
target_link_libraries(menu_style_test PRIVATE opennova_runtime)
target_include_directories(menu_style_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_style COMMAND menu_style_test)

# A screen's string tables and the shell's style through a file source, kept by (name,
# stamp) (menu/menu_screen_inputs.h).
add_executable(menu_screen_inputs_test menu/menu_screen_inputs_test.cpp)
target_link_libraries(menu_screen_inputs_test PRIVATE opennova_runtime)
target_include_directories(menu_screen_inputs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_screen_inputs COMMAND menu_screen_inputs_test)

# A screen's fonts and textures through a file source, kept by (file, stamp), the
# two-configure sweep (menu/menu_frame_assets.h).
add_executable(menu_frame_assets_test menu/menu_frame_assets_test.cpp)
target_link_libraries(menu_frame_assets_test PRIVATE opennova_runtime)
target_include_directories(menu_frame_assets_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_frame_assets COMMAND menu_frame_assets_test)

add_executable(menu_video_test menu/menu_video_test.cpp)
target_link_libraries(menu_video_test PRIVATE opennova_runtime)
target_include_directories(menu_video_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_video COMMAND menu_video_test)

foreach(name crypto_roundtrip cdep_read cdep_roundtrip)
    add_executable(${name}_test terrain/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE opennova_formats)
    target_include_directories(${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

# The synthetic terrain map fixtures/terrain/tmap (Tmap.trn/.cpt/_f.pcx):
# minted by our own writers, byte-compared every run (--write regenerates).
# The retail .cpt polydata is swept behind OPENNOVA_JO_ASSETS.
add_executable(minimal_terrain_gen_test fixtures/minimal_terrain_gen.cpp)
target_link_libraries(minimal_terrain_gen_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(minimal_terrain_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_terrain_gen COMMAND minimal_terrain_gen_test)

add_executable(cpt_jo_assets_sweep_test terrain/cpt_jo_assets_sweep_test.cpp)
target_link_libraries(cpt_jo_assets_sweep_test PRIVATE opennova_formats)
target_include_directories(cpt_jo_assets_sweep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(cpt_jo_assets_sweep COMMAND cpt_jo_assets_sweep_test)

add_executable(trn_config_roundtrip_test terrain/trn_config_roundtrip_test.cpp)
target_link_libraries(trn_config_roundtrip_test PRIVATE opennova_formats)
target_include_directories(trn_config_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(trn_config_roundtrip COMMAND trn_config_roundtrip_test)

# Terrain world->source coordinate transform kernel
# (engine/runtime/terrain_query/coords.h): the shared runtime and bounds-checked
# terrain coordinate transforms.
add_executable(coords_test terrain/coords_test.cpp)
target_link_libraries(coords_test PRIVATE opennova_runtime)
target_include_directories(coords_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME coords COMMAND coords_test)

# Per-quadrant neighbour-tap locks (the .trn lock_* pairs) in the shared tap
# kernel: the masking against a verbatim copy of the original, the lock-table
# index, the sampler at a sector seam locked vs unlocked, and the all-zero
# default that keeps unlocked terrains rendering exactly as before.
add_executable(quadrant_lock_test terrain/quadrant_lock_test.cpp)
target_link_libraries(quadrant_lock_test PRIVATE opennova_runtime)
target_include_directories(quadrant_lock_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME quadrant_lock COMMAND quadrant_lock_test)

# The retail terrain segment raycast (engine/runtime/terrain_query/terrain_raycast):
# the Terrain_RaycastHeightmapLoRes march + HiRes_0 refine structural
# translation over a synthetic sampler — step normalization/budget, ray-z hit
# semantics, the column-shortcut crossing asymmetry, the empty-cell height-0
# floor, the out-of-extent editor guard, and the odd skip-refine guard.
add_executable(terrain_raycast_test terrain/terrain_raycast_test.cpp)
target_link_libraries(terrain_raycast_test PRIVATE opennova_runtime)
target_include_directories(terrain_raycast_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_raycast COMMAND terrain_raycast_test)

# The ONE owning cpt/trn(+charmap) -> TerrainHeightField + SurfaceTypeMap
# builder (engine/runtime/terrain_query/terrain_field_store, ADR 0042 d4):
# dims/origins/locks land in the views, a height sample and a surface index
# round-trip through the store's own copies, and the copies outlive the
# source documents.
add_executable(terrain_field_store_test terrain_query/terrain_field_store_test.cpp)
target_link_libraries(terrain_field_store_test PRIVATE opennova_runtime)
target_include_directories(terrain_field_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_field_store COMMAND terrain_field_store_test)

# fixtures/particle/synth_*.ptl: the five synthetic particle files the focused
# tests read, written by save_particles from integer data (the shipped corpus
# is particle_smoke_all_fixtures' OPENNOVA_JO_ASSETS leg).
add_executable(minimal_particle_gen_test fixtures/minimal_particle_gen.cpp)
target_link_libraries(minimal_particle_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_particle_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_particle_gen COMMAND minimal_particle_gen_test)

# Particle (.ptl) tests. Format half (engine/formats/particle: model/parser/writer/
# tables) links opennova_formats; sim half (engine/runtime/particle: emitter +
# effect scene) links opennova_runtime. Witness matrix:
# docs/particles/ptl-format-re.md (CParticleDef_ParseFromConfigMap @ 0x5ed210,
# Jointops.exe IDB).
foreach(name smallest minimal_effect multi_section multi_layer table_handles
        smoke_all_fixtures enum_parse writer_roundtrip curve_lut
        lenient_lines)
    add_executable(particle_${name}_test particle/particle_${name}_test.cpp)
    target_link_libraries(particle_${name}_test PRIVATE opennova_formats)
    target_include_directories(particle_${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(particle_${name}_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    if(name STREQUAL "smoke_all_fixtures" OR name STREQUAL "key_places")
        opennova_add_mixed_test(particle_${name} COMMAND particle_${name}_test)
    else()
        add_test(NAME particle_${name} COMMAND particle_${name}_test)
    endif()
endforeach()
foreach(name emitter emit_shape translate effect_scene_contract)
    add_executable(particle_${name}_test particle/particle_${name}_test.cpp)
    target_link_libraries(particle_${name}_test PRIVATE opennova_runtime)
    target_include_directories(particle_${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(particle_${name}_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    if(name STREQUAL "smoke_all_fixtures")
        opennova_add_mixed_test(particle_${name} COMMAND particle_${name}_test)
    else()
        add_test(NAME particle_${name} COMMAND particle_${name}_test)
    endif()
endforeach()
# particle_smoke_all_fixtures is mixed: the committed fixtures/particle sweep
# always runs; the extracted retail corpus leg rides OPENNOVA_JO_ASSETS
# (SKIP-LEG without it).

# CBIN tests
add_executable(cbin_unit_test cbin/cbin_unit_test.cpp)
target_link_libraries(cbin_unit_test PRIVATE opennova_formats)
target_include_directories(cbin_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cbin_unit COMMAND cbin_unit_test)

add_executable(cbin_roundtrip_test cbin/cbin_roundtrip_test.cpp)
target_link_libraries(cbin_roundtrip_test PRIVATE opennova_formats)
target_include_directories(cbin_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cbin_roundtrip_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
opennova_add_mixed_test(cbin_roundtrip COMMAND cbin_roundtrip_test)

add_executable(cbin_display_view_test cbin/cbin_display_view_test.cpp)
target_link_libraries(cbin_display_view_test PRIVATE opennova_formats)
target_include_directories(cbin_display_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cbin_display_view_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME cbin_display_view COMMAND cbin_display_view_test)

# MNU menu UI tests (forgiving XML parser, MNU AST + serialize roundtrip, MNS stylesheet)
add_executable(mnu_xml_unit_test mnu/mnu_xml_unit_test.cpp)
target_link_libraries(mnu_xml_unit_test PRIVATE opennova_formats)
target_include_directories(mnu_xml_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mnu_xml_unit COMMAND mnu_xml_unit_test)

add_executable(mnu_layout_test mnu/mnu_layout_test.cpp)
target_link_libraries(mnu_layout_test PRIVATE opennova_formats)
target_include_directories(mnu_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mnu_layout COMMAND mnu_layout_test)

# controls: the JO input-binding catalog + Options->Controls (CONTROL_MAPPING) model.
add_executable(controls_test controls/controls_test.cpp)
target_link_libraries(controls_test PRIVATE opennova_runtime)
target_include_directories(controls_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME controls_unit COMMAND controls_test)

add_executable(player_actions_test controls/player_actions_test.cpp)
target_link_libraries(player_actions_test PRIVATE opennova_runtime)
add_test(NAME player_actions COMMAND player_actions_test)

# The F1 key-binding help pages (controls/help_screen.h).
add_executable(help_screen_test controls/help_screen_test.cpp)
target_link_libraries(help_screen_test PRIVATE opennova_runtime)
add_test(NAME help_screen COMMAND help_screen_test)

add_executable(mnu_unit_test mnu/mnu_unit_test.cpp)
target_link_libraries(mnu_unit_test PRIVATE opennova_formats)
target_include_directories(mnu_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mnu_unit COMMAND mnu_unit_test)

# Compat sweep over real shipped .mnu menus: the fifteen revx02 menus from the
# reference fixture set (gated on OPENNOVA_JO_ASSETS) required-pass, then every
# .mnu the packed install serves (OPENNOVA_JO_DIR, a SKIP-LEG retail leg).
add_executable(mnu_compat_test mnu/mnu_compat_test.cpp)
target_link_libraries(mnu_compat_test PRIVATE opennova_base opennova_formats)
target_include_directories(mnu_compat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(mnu_compat COMMAND mnu_compat_test)

# Round-trip key-coverage: catches attributes/tags dropped at parse time, which
# the idempotence sweep above cannot see (see ADR 0002). The same fifteen
# reference menus; gated on OPENNOVA_JO_ASSETS.
add_executable(mnu_coverage_test mnu/mnu_coverage_test.cpp)
target_link_libraries(mnu_coverage_test PRIVATE opennova_formats)
target_include_directories(mnu_coverage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(mnu_coverage COMMAND mnu_coverage_test)

# The modeled text layout (formats/mnu/mnu_text_layout, D-MNU-22): a menu read and
# written again comes back byte-identical; a value edit changes only its own token; an
# added or moved record goes beside the records of its own list; seeded rounds of list
# edits read back as edited; a document made in code writes the canonical bytes. The
# retail twin sweeps every .mnu the packed install serves (OPENNOVA_JO_DIR) and the
# reference set's shipped menus (OPENNOVA_JO_ASSETS): 47 menus, 24 distinct files, each
# byte-identical, the list rounds and the one-line check of one-token edits over each
# distinct file.
add_executable(mnu_text_layout_test mnu/mnu_text_layout_test.cpp)
target_link_libraries(mnu_text_layout_test PRIVATE opennova_base opennova_formats)
target_include_directories(mnu_text_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(mnu_text_layout COMMAND mnu_text_layout_test)

# The format's rules a property table reads (formats/mnu/mnu_schema, ADR 0046 S9h): which
# window type reads what, what an ACTION's verb reads, the material flag tokens. The table
# itself is the editor's (editor_menu_table, ADR 0046 S13 D10).
add_executable(mnu_schema_test mnu/mnu_schema_test.cpp)
target_link_libraries(mnu_schema_test PRIVATE opennova_formats)
target_include_directories(mnu_schema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mnu_schema COMMAND mnu_schema_test)

add_executable(mns_parse_test mns/mns_parse_test.cpp)
target_link_libraries(mns_parse_test PRIVATE opennova_formats)
target_include_directories(mns_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mns_parse COMMAND mns_parse_test)

# Lossless MNS document model (ADR 0009): synthetic byte round-trips, edit
# stability and diagnostics unconditionally; the shipped menu_style.mns legs
# read the reference fixture set (SKIP-LEG without OPENNOVA_JO_ASSETS).
add_executable(mns_document_test mns/mns_document_test.cpp)
target_link_libraries(mns_document_test PRIVATE opennova_formats)
target_include_directories(mns_document_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(mns_document COMMAND mns_document_test)

# World runtime substrate: addressable entities (faithful find_by_net_id), shared
# var store, entity-command primitives, the logic-tick clock, snapshot/restore.
add_executable(world_test world/world_test.cpp)
target_link_libraries(world_test PRIVATE opennova_runtime)
target_include_directories(world_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(world_test PRIVATE cxx_std_17)
add_test(NAME world COMMAND world_test)

# WAC command bodies and the EntityCommands primitives they reach, pinned against
# the retail handlers (script kills, hide/hold/disable, HP writers, predicates,
# return values).
add_executable(script_command_parity_test world/script_command_parity_test.cpp)
target_link_libraries(script_command_parity_test PRIVATE opennova_runtime)
target_include_directories(script_command_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(script_command_parity_test PRIVATE cxx_std_17)
add_test(NAME script_command_parity COMMAND script_command_parity_test)

# world::inspect (ADR 0042 d5): the entity directory join, the debug-card
# halves, and the EntityCommands set_entity_health/position both-store writes.
add_executable(inspect_test world/inspect_test.cpp)
target_link_libraries(inspect_test PRIVATE opennova_runtime)
target_include_directories(inspect_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(inspect_test PRIVATE cxx_std_17)
add_test(NAME inspect COMMAND inspect_test)

add_executable(tick_accumulator_test world/tick_accumulator_test.cpp)
target_link_libraries(tick_accumulator_test PRIVATE opennova_runtime)
target_include_directories(tick_accumulator_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(tick_accumulator_test PRIVATE cxx_std_17)
add_test(NAME tick_accumulator COMMAND tick_accumulator_test)

# The player waypoint track (HUD waypoint label backing state).
add_executable(waypoint_track_test world/waypoint_track_test.cpp)
target_link_libraries(waypoint_track_test PRIVATE opennova_runtime)
target_include_directories(waypoint_track_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(waypoint_track_test PRIVATE cxx_std_17)
add_test(NAME waypoint_track COMMAND waypoint_track_test)

# The death screen's free-fly motor and the chase orbit state it drives.
add_executable(spectator_motor_test world/spectator_motor_test.cpp)
target_link_libraries(spectator_motor_test PRIVATE opennova_runtime)
target_include_directories(spectator_motor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(spectator_motor_test PRIVATE cxx_std_17)
add_test(NAME spectator_motor COMMAND spectator_motor_test)

# Entity_ClassifyForMinimap: definition/entity marker classification shared by
# every server producer before packet cadence/recipient policy.
add_executable(minimap_overlay_test world/minimap_overlay_test.cpp)
target_link_libraries(minimap_overlay_test PRIVATE opennova_runtime opennova_test_rig)
target_include_directories(minimap_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(minimap_overlay_test PRIVATE cxx_std_17)
opennova_add_mixed_test(minimap_overlay COMMAND minimap_overlay_test)

# The guided-missile flight integrator (D-NET-64): ignition hold, boost ramp,
# BAM turn clamp, the three termination tests and their ordering.
add_executable(guided_missile_flight_test world/guided_missile_flight_test.cpp)
target_link_libraries(guided_missile_flight_test PRIVATE opennova_runtime)
target_include_directories(guided_missile_flight_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(guided_missile_flight_test PRIVATE cxx_std_17)
add_test(NAME guided_missile_flight COMMAND guided_missile_flight_test)

# The message-feed line policy (0x1E -> canned sentence + color) and the
# witnessed game-event classification corrections.
add_executable(feed_format_test hud/feed_format_test.cpp)
target_link_libraries(feed_format_test PRIVATE opennova_runtime)
target_include_directories(feed_format_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(feed_format_test PRIVATE cxx_std_17)
add_test(NAME feed_format COMMAND feed_format_test)

# The Tab player list's row policy: the witnessed glyph append order, the
# team/non-team format + column + color split, and the layout constants.
add_executable(scoreboard_format_test hud/scoreboard_format_test.cpp)
target_link_libraries(scoreboard_format_test PRIVATE opennova_runtime)
target_include_directories(scoreboard_format_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(scoreboard_format_test PRIVATE cxx_std_17)
add_test(NAME scoreboard_format COMMAND scoreboard_format_test)

# The talk keys and the captured chat line (hud/hud_chat_entry.h): the dispatch
# gates and debounce, the line editor, the '\' cycle, the channel bytes.
add_executable(hud_chat_entry_test hud/hud_chat_entry_test.cpp)
target_link_libraries(hud_chat_entry_test PRIVATE opennova_runtime)
target_include_directories(hud_chat_entry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hud_chat_entry_test PRIVATE cxx_std_17)
add_test(NAME hud_chat_entry COMMAND hud_chat_entry_test)

# The connection indicators (D-HUD-38): g_NetQuality's frame step and the three icons' draw.
add_executable(net_quality_indicators_test hud/net_quality_indicators_test.cpp)
target_link_libraries(net_quality_indicators_test PRIVATE opennova_runtime)
target_include_directories(net_quality_indicators_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(net_quality_indicators_test PRIVATE cxx_std_17)
add_test(NAME net_quality_indicators COMMAND net_quality_indicators_test)

add_executable(hud_vehicle_panel_test hud/hud_vehicle_panel_test.cpp)
target_link_libraries(hud_vehicle_panel_test PRIVATE opennova_runtime)
target_include_directories(hud_vehicle_panel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hud_vehicle_panel COMMAND hud_vehicle_panel_test)

add_executable(hud_message_log_test hud/hud_message_log_test.cpp)
target_link_libraries(hud_message_log_test PRIVATE opennova_runtime)
target_include_directories(hud_message_log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hud_message_log COMMAND hud_message_log_test)

# The mounted-vehicle panel feed (world/vehicle_panel_feed): the re-rooted
# vehicle, retail's slot-list order, and the per-marker rows.
add_executable(vehicle_panel_feed_test world/vehicle_panel_feed_test.cpp)
target_link_libraries(vehicle_panel_feed_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(vehicle_panel_feed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vehicle_panel_feed_test PRIVATE cxx_std_17)
add_test(NAME vehicle_panel_feed COMMAND vehicle_panel_feed_test)

# The AAS zone status panel feed (world/lfp_feed): list order, the timer join,
# the viewer's distance/cylinder tests, the contest counts.
add_executable(lfp_feed_test world/lfp_feed_test.cpp)
target_link_libraries(lfp_feed_test PRIVATE opennova_runtime)
target_include_directories(lfp_feed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(lfp_feed_test PRIVATE cxx_std_17)
add_test(NAME lfp_feed COMMAND lfp_feed_test)

# charattr.def ATTRIBUTES -> World::class_attribute_flags -> the friendly-tag
# medic plate: the whole per-class word, the first-missing-section stop, the
# first-section-wins duplicate rule, the 16-slot wrap and the S2C 0x41 clear
# [orig: CharAttr_LoadFromDef @0x412140; AnimMap_IsSlotActive @0x4125e0].
add_executable(charattr_flags_test world/charattr_flags_test.cpp)
target_link_libraries(charattr_flags_test PRIVATE opennova_net opennova_runtime)
target_include_directories(charattr_flags_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(charattr_flags_test PRIVATE cxx_std_17)
add_test(NAME charattr_flags COMMAND charattr_flags_test)

# The friendly-tag radio-request feeds: the per-tag +885 latch folded with the
# Entity_FindChildByDefType carrier walk, and the local player's viewer gate
# [orig: HUD_DrawEntityLabel @0x5a3bba..0x5a3c1a; Entity_FindChildByDefType @0x43bea0].
add_executable(friendly_tags_test world/friendly_tags_test.cpp)
target_link_libraries(friendly_tags_test PRIVATE opennova_net opennova_runtime)
target_include_directories(friendly_tags_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(friendly_tags_test PRIVATE cxx_std_17)
add_test(NAME friendly_tags COMMAND friendly_tags_test)

# The player-chat lane (S2C 0x14) fold + the HUD channel table.
add_executable(client_replica_chat_test netsim/client_replica_chat_test.cpp)
target_link_libraries(client_replica_chat_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_replica_chat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_replica_chat COMMAND client_replica_chat_test)
add_executable(client_replica_spectate_test netsim/client_replica_spectate_test.cpp)
target_link_libraries(client_replica_spectate_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_replica_spectate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_replica_spectate COMMAND client_replica_spectate_test)

add_executable(impact_scar_test world/impact_scar_test.cpp)
target_link_libraries(impact_scar_test PRIVATE opennova_runtime)
target_include_directories(impact_scar_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(impact_scar_test PRIVATE cxx_std_17)
add_test(NAME impact_scar COMMAND impact_scar_test)

add_executable(renderer_scar_draw_list_test renderer/scar_draw_list_test.cpp)
target_link_libraries(renderer_scar_draw_list_test PRIVATE opennova_runtime)
target_include_directories(renderer_scar_draw_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(renderer_scar_draw_list_test PRIVATE cxx_std_17)
add_test(NAME renderer_scar_draw_list COMMAND renderer_scar_draw_list_test)

add_executable(ground_conform_test world/ground_conform_test.cpp)
target_link_libraries(ground_conform_test PRIVATE opennova_runtime opennova_test_rig)
target_include_directories(ground_conform_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(ground_conform COMMAND ground_conform_test)

# The spring leg's WIRING (vehicle_suspension.cpp): def keys, the role-picked
# parked latch + Flags 0x10 replication, the spring dt, the per-wheel step.
add_executable(vehicle_suspension_test world/vehicle_suspension_test.cpp)
target_link_libraries(vehicle_suspension_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(vehicle_suspension_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME vehicle_suspension COMMAND vehicle_suspension_test)

# AI subsystem (byte-exact foundation): state enum + struct layout, AIEvent ring,
# per-entity movement phase (AI_BeginUpdate), the state-machine dispatcher + transitions,
# and the trivial enter/exit/event handler ports.
add_executable(ai_test world/ai_test.cpp)
target_link_libraries(ai_test PRIVATE opennova_runtime)
target_include_directories(ai_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ai_test PRIVATE cxx_std_17)
add_test(NAME ai COMMAND ai_test)

# The vehicle brain's combat ticks through their dispatch rows: the state-17
# GROUND_COMBAT tick (AIEntity_ProcessWeaponFire @0x472E00) leg by leg, the
# processed/continuation LOS split, the rescan clear, turret staging, the
# no-ammo latch, RC_FIRE, the ATEAM sweep, the ATEAM_LOCK replay, the chase.
add_executable(ai_vehicle_combat_test world/ai_vehicle_combat_test.cpp)
target_link_libraries(ai_vehicle_combat_test PRIVATE opennova_runtime)
target_include_directories(ai_vehicle_combat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ai_vehicle_combat_test PRIVATE cxx_std_17)
add_test(NAME ai_vehicle_combat COMMAND ai_vehicle_combat_test)

# The SM target feed (AI_FindBestTargetB @0x466F60) and the engage bookkeeping
# against their retail witnesses.
add_executable(ai_targeting_test world/ai_targeting_test.cpp)
target_link_libraries(ai_targeting_test PRIVATE opennova_runtime)
target_include_directories(ai_targeting_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ai_targeting_test PRIVATE cxx_std_17)
add_test(NAME ai_targeting COMMAND ai_targeting_test)

# The AI line of sight's exclusion set and indoors skip
# (Physics_RaycastTerrainAndSectors @0x539910) and the script LOS split.
add_executable(ai_los_test world/ai_los_test.cpp)
target_link_libraries(ai_los_test PRIVATE opennova_runtime)
target_include_directories(ai_los_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ai_los_test PRIVATE cxx_std_17)
add_test(NAME ai_los COMMAND ai_los_test)

# The global hit record and the script kills that read it: the BMS kills keep
# the last recorded round, WAC killSSN zeroes it, items read its section and
# a brained item's machine its owner word.
add_executable(hit_record_test world/hit_record_test.cpp)
target_link_libraries(hit_record_test PRIVATE opennova_runtime)
target_include_directories(hit_record_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hit_record_test PRIVATE cxx_std_17)
add_test(NAME hit_record COMMAND hit_record_test)

add_executable(movement_brain_parity_test world/movement_brain_parity_test.cpp)
target_link_libraries(movement_brain_parity_test PRIVATE opennova_runtime)
target_compile_features(movement_brain_parity_test PRIVATE cxx_std_17)
add_test(NAME movement_brain_parity COMMAND movement_brain_parity_test)

# Entity_ComputeRelativePositionMetrics @0x545710 against original-x86 vectors
# (scripts/oracles/aim_metrics_parity.py).
add_executable(aim_metrics_parity_test world/aim_metrics_parity_test.cpp)
target_link_libraries(aim_metrics_parity_test PRIVATE opennova_runtime)
target_compile_features(aim_metrics_parity_test PRIVATE cxx_std_17)
add_test(NAME aim_metrics_parity COMMAND aim_metrics_parity_test)

# The vehicle-brain state rows against their retail witnesses: the live hull
# words each row reads, the slot and brain words each enter writes.
add_executable(ai_brain_rows_test world/ai_brain_rows_test.cpp)
target_link_libraries(ai_brain_rows_test PRIVATE opennova_runtime)
target_compile_features(ai_brain_rows_test PRIVATE cxx_std_17)
add_test(NAME ai_brain_rows COMMAND ai_brain_rows_test)

add_executable(npc_weapons_test world/npc_weapons_test.cpp)
target_link_libraries(npc_weapons_test PRIVATE opennova_runtime)
target_include_directories(npc_weapons_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npc_weapons COMMAND npc_weapons_test)

# Item destruction: the explosion queue + AoE gates, the bullet armor gates, the
# destructible death chain (husk flags + events + the kz chain blast), the
# death-piece pool, and the dead-item settle (world/destruction.h;
# docs/world/world-wac-ai-re.md §24).
add_executable(destruction_test world/destruction_test.cpp)
target_link_libraries(destruction_test PRIVATE opennova_runtime)
target_include_directories(destruction_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(destruction_test PRIVATE cxx_std_17)
add_test(NAME destruction COMMAND destruction_test)

add_executable(death_piece_draw_test world/death_piece_draw_test.cpp)
target_link_libraries(death_piece_draw_test PRIVATE opennova_runtime)
target_compile_features(death_piece_draw_test PRIVATE cxx_std_17)
add_test(NAME death_piece_draw COMMAND death_piece_draw_test)

add_executable(nvg_laser_test world/nvg_laser_test.cpp)
target_link_libraries(nvg_laser_test PRIVATE opennova_runtime)
target_compile_features(nvg_laser_test PRIVATE cxx_std_17)
add_test(NAME nvg_laser COMMAND nvg_laser_test)

add_executable(item_events_test world/item_events_test.cpp)
target_link_libraries(item_events_test PRIVATE opennova_runtime)
add_test(NAME item_events COMMAND item_events_test)

# The collision sweep's death-trait mining (mission/collision_resolve ->
# world::ItemDeathTraits): the first husk's KZ / DEAD banks, the huskFinal-only
# and missing-husk gates, and the retail GLASS userpoint table, over
# byte-patched synthetic fixtures in a loose temp root (ADR 0043 d10).
add_executable(item_death_traits_resolve_test world/item_death_traits_resolve_test.cpp)
target_link_libraries(item_death_traits_resolve_test PRIVATE opennova_runtime opennova_base)
target_include_directories(item_death_traits_resolve_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(item_death_traits_resolve_test PRIVATE cxx_std_17)
add_test(NAME item_death_traits_resolve COMMAND item_death_traits_resolve_test)

# The collectors' render model, stamped where the model resolves (placed
# entities) and carried on the decoded wire row's resolved shape.
add_executable(render_model_resolve_test world/render_model_resolve_test.cpp)
target_link_libraries(render_model_resolve_test PRIVATE opennova_runtime opennova_base)
target_include_directories(render_model_resolve_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(render_model_resolve_test PRIVATE cxx_std_17)
add_test(NAME render_model_resolve COMMAND render_model_resolve_test)

add_executable(husk_section_origins_test world/husk_section_origins_test.cpp)
target_link_libraries(husk_section_origins_test PRIVATE opennova_runtime opennova_base)
target_include_directories(husk_section_origins_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(husk_section_origins_test PRIVATE cxx_std_17)
add_test(NAME husk_section_origins COMMAND husk_section_origins_test)

# Asset-gated (OPENNOVA_JO_ASSETS fixtures): the RoundSim debug trail over the
# shipped ammo.def — a grenadehe dies by its fuse, never by an impact.
add_executable(round_debug_trail_test world/round_debug_trail_test.cpp)
target_link_libraries(round_debug_trail_test PRIVATE opennova_runtime)
target_include_directories(round_debug_trail_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(round_debug_trail_test PRIVATE cxx_std_17)
opennova_add_gated_test(round_debug_trail COMMAND round_debug_trail_test)

# Throwables (world/throwables.h; world-wac-ai-re §27): PowerThrow charge,
# grenade bounce/fuse, satchel place/detonator, claymore cone + fan, the AV
# mine's moving-vehicle cone, owner-death cleanup.
add_executable(throwables_test world/throwables_test.cpp)
target_link_libraries(throwables_test PRIVATE opennova_runtime)
target_include_directories(throwables_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(throwables_test PRIVATE cxx_std_17)
add_test(NAME throwables COMMAND throwables_test)

# Terrain-grounded movement: the shared height_field samplers, the 5-tap
# calc_average_ground_height port (Entity_CalcAverageGroundHeight @0x457230) by hand
# math, and the AiSystem ground clamp over a synthetic ramp atlas.
add_executable(ground_height_test world/ground_height_test.cpp)
target_link_libraries(ground_height_test PRIVATE opennova_runtime)
target_include_directories(ground_height_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ground_height_test PRIVATE cxx_std_17)
add_test(NAME ground_height COMMAND ground_height_test)

# Infantry motor (Entity_UpdateInfantryAI @0x4b9910): body-turn clamp, turn-in-place
# gates, alert/wounded gait selection + availability fallbacks, one-shot route
# lifecycle, flag-table commit rules, gravity/landing/fall damage, slope slide —
# each against the witnessed constants, over synthetic clips + height fields.
add_executable(infantry_test world/infantry_test.cpp)
target_link_libraries(infantry_test PRIVATE opennova_runtime opennova_test_rig)
target_include_directories(infantry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(infantry_test PRIVATE cxx_std_17)
# Like player_spawn_test below, MSVC coalesces this file's many scoped World
# fixtures into one Release stack frame. The blend-transition grills add nested
# motor calls on top of that frame, so leave the test runner deliberate headroom.
# World owns its AiSystem by value (ADR 0043 slice E4), which lifted this file's
# largest frame past 8 MB; 16 MB keeps the same headroom.
if(MSVC)
    target_link_options(infantry_test PRIVATE "/STACK:16777216")
endif()
opennova_add_mixed_test(infantry COMMAND infantry_test)

add_executable(collision_force_test world/collision_force_test.cpp)
target_link_libraries(collision_force_test PRIVATE opennova_runtime)
add_test(NAME collision_force COMMAND collision_force_test)

# The three fullscreen damage-feedback words (arms, decays, draw reductions).
add_executable(screen_flash_test world/screen_flash_test.cpp)
target_link_libraries(screen_flash_test PRIVATE opennova_runtime)
add_test(NAME screen_flash COMMAND screen_flash_test)

# The local radar contact state and its producers (spinmap mask bit 10).
add_executable(radar_contacts_test world/radar_contacts_test.cpp)
target_link_libraries(radar_contacts_test PRIVATE opennova_runtime)
add_test(NAME radar_contacts COMMAND radar_contacts_test)

add_executable(infantry_terrain_test world/infantry_terrain_test.cpp)
target_link_libraries(infantry_terrain_test PRIVATE opennova_runtime)
add_test(NAME infantry_terrain COMMAND infantry_terrain_test)

# The org1 (NPC) motor legs against vectors from executing the retail motor
# (Entity_UpdateInfantryAI @0x4B9910): the +0xAC vertical quarter-step tail,
# the even-only climb/ladder/water blocks, and the later org1 parity fixes.
add_executable(infantry_org1_parity_test world/infantry_org1_parity_test.cpp)
target_link_libraries(infantry_org1_parity_test PRIVATE opennova_runtime)
target_compile_features(infantry_org1_parity_test PRIVATE cxx_std_17)
add_test(NAME infantry_org1_parity COMMAND infantry_org1_parity_test)

add_executable(infantry_attention_test world/infantry_attention_test.cpp)
target_link_libraries(infantry_attention_test PRIVATE opennova_runtime)
target_compile_features(infantry_attention_test PRIVATE cxx_std_17)
add_test(NAME infantry_attention COMMAND infantry_attention_test)

# The org1 think block's combat legs (Entity_UpdateInfantryAI @0x4B9910, the
# think @0x4BA970..0x4BE7FD): held/dead targets, the no-target search, the aim
# target and eye, the lead, the hold-timer tail, reload, retaliation LOS.
add_executable(infantry_think_test world/infantry_think_test.cpp)
target_link_libraries(infantry_think_test PRIVATE opennova_runtime)
target_compile_features(infantry_think_test PRIVATE cxx_std_17)
add_test(NAME infantry_think COMMAND infantry_think_test)

# The infantry slot-sound emissions (footsteps/foley/landing/screams).
add_executable(slot_sound_test world/slot_sound_test.cpp)
target_link_libraries(slot_sound_test PRIVATE opennova_runtime)
target_include_directories(slot_sound_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(slot_sound_test PRIVATE cxx_std_17)
add_test(NAME slot_sound COMMAND slot_sound_test)

# The wire-fed remote body's footstep/foley consume (the slot-sound sibling).
add_executable(wire_body_sound_test world/wire_body_sound_test.cpp)
target_link_libraries(wire_body_sound_test PRIVATE opennova_runtime)
target_include_directories(wire_body_sound_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(wire_body_sound_test PRIVATE cxx_std_17)
add_test(NAME wire_body_sound COMMAND wire_body_sound_test)

add_executable(doors_test world/doors_test.cpp)
target_link_libraries(doors_test PRIVATE opennova_runtime)
target_include_directories(doors_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(doors_test PRIVATE cxx_std_17)
if(MSVC)
    target_link_options(doors_test PRIVATE "/STACK:4194304")
endif()
opennova_add_mixed_test(doors COMMAND doors_test)

# World-object collision (Entity_TestCollisionSections @0x4aef90 / _RaycastCollisionModel
# @0x413060 / _ComputeBoneCollisionForce @0x4ae150 / _ProcessCollisionAndPlatformPhysics
# @0x4b2bd0): matrix roundtrip, blink type-8 query + flags^6/indoors, type-1 segment
# clip, the ground probe through candidate models, wall push-out with prev-position
# gating, hurt/ladder volumes, and the idle skip throttle — over hand-built volumes.
add_executable(collision_test world/collision_test.cpp)
target_link_libraries(collision_test PRIVATE opennova_runtime)
target_include_directories(collision_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(collision_test PRIVATE cxx_std_17)
# Like infantry_test above, MSVC coalesces this file's many scoped World
# fixtures into one Release stack frame (999 KB at ADR 0043 slice E6, when
# World gained its vehicle and zone systems); give it the same headroom as
# player_spawn_test and promote_test.
if(MSVC)
    target_link_options(collision_test PRIVATE "/STACK:4194304")
endif()
add_test(NAME collision COMMAND collision_test)

add_executable(collision_vertical_test world/collision_vertical_test.cpp)
target_link_libraries(collision_vertical_test PRIVATE opennova_runtime)
target_compile_features(collision_vertical_test PRIVATE cxx_std_17)
add_test(NAME collision_vertical COMMAND collision_vertical_test)

# Asset-gated (OPENNOVA_JO_DIR): the 00TRg transport-unload repro over the
# real DTruck1 collision; the pipeline preconditions are asserted, the escape
# verdict stays a report while the divergence is open.
add_executable(truck_dismount_test world/truck_dismount_test.cpp)
target_link_libraries(truck_dismount_test PRIVATE opennova_runtime)
target_include_directories(truck_dismount_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(truck_dismount_test PRIVATE cxx_std_17)
opennova_add_gated_test(truck_dismount COMMAND truck_dismount_test)

# Authoritative projectile consequences through RoundSim's CollisionWorld seam:
# pre-arm dud substitution, kinetic damage, NoDie, and physical zero-damage blockers.
add_executable(projectile_combat_test world/projectile_combat_test.cpp)
target_link_libraries(projectile_combat_test PRIVATE opennova_runtime)
target_include_directories(projectile_combat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(projectile_combat_test PRIVATE cxx_std_17)
add_test(NAME projectile_combat COMMAND projectile_combat_test)

add_executable(fire_sound_test world/fire_sound_test.cpp)
target_link_libraries(fire_sound_test PRIVATE opennova_runtime)
target_include_directories(fire_sound_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(fire_sound_test PRIVATE cxx_std_17)
add_test(NAME fire_sound COMMAND fire_sound_test)

add_executable(player_loadout_test world/player_loadout_test.cpp)
target_link_libraries(player_loadout_test PRIVATE opennova_runtime)
target_include_directories(player_loadout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(player_loadout_test PRIVATE cxx_std_17)
add_test(NAME player_loadout COMMAND player_loadout_test)

# Rendering occlusion (Terrain_InitBuildingPortals @0x5c7480 register/weld/flags,
# Terrain_BuildSectorVisibilityMasks @0x5c8610 + Render_VisibilityPortalTraversal
# @0x5c4ae0, Terrain_TestSectorEntityOcclusion @0x5c4610, the entity-collector gates +
# Terrain_OcclusionCheckThreeRays @0x610ed0, Entity_QueryBlinkBoxesAtPoint
# @0x4af350): render-float math, the weld pass, outdoor/indoor mask rules,
# outside-in viewthru banking, TOC shadow-wedge culling, the blink-hits render
# gate + three-ray latch — over hand-built portal/collision models.
add_executable(occlusion_test world/occlusion_test.cpp)
target_link_libraries(occlusion_test PRIVATE opennova_runtime)
target_include_directories(occlusion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(occlusion_test PRIVATE cxx_std_17)
add_test(NAME occlusion COMMAND occlusion_test)

# Real-asset occlusion probe: retail Armry01.3di (the 00TRa armory) through the
# engine's 3DI3 read and occlusion-model build (the parsed struct IS the model,
# ADR 0027), camera walked room to room, per-room section masks asserted
# (window/portal/open-slot legs over authored OFAC topology).
# Uses the committed Armry01 fixture in CI; OPENNOVA_JO_ASSETS can override it
# for a retail-install parity run.
add_executable(occlusion_armry_test world/occlusion_armry_test.cpp)
target_link_libraries(occlusion_armry_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(occlusion_armry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(occlusion_armry_test PRIVATE cxx_std_17)
target_compile_definitions(occlusion_armry_test PRIVATE
    OPENNOVA_ARMRY_FIXTURE="${CMAKE_SOURCE_DIR}/fixtures/threedi/synth/armory.3di")
opennova_add_mixed_test(occlusion_armry COMMAND occlusion_armry_test)

# The FP weapon action FSM (WeaponAction_ProcessFrame @0x540e60 + Anim_InitActions
# @0x541fa0, net-re §5.62): the ACTION-row bake (auto delays from clip ticks), the
# pump phase protocol, and the witnessed handler decisions — the fire->recoil chain,
# the recoil arbiter (refire/auto-reload/emptyidle), reload scope stash/rescope,
# 3-round burst, dry-fire.
add_executable(weapon_fsm_test world/weapon_fsm_test.cpp)
target_link_libraries(weapon_fsm_test PRIVATE opennova_runtime)
target_include_directories(weapon_fsm_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(weapon_fsm_test PRIVATE cxx_std_17)
add_test(NAME weapon_fsm COMMAND weapon_fsm_test)

# The per-player weapon slot pool, spawn-kit chain, and map availability rules
# (the 2026-07-18 loadout grill): the .bms kit filter + knife fallback, display
# expansion, the 780-slot fill, pool seeding (classrounds overrides) + clip
# recalc, and the three switching walks (select / category-key / cycle) with
# their witnessed gates.
add_executable(weapon_inventory_test world/weapon_inventory_test.cpp)
target_link_libraries(weapon_inventory_test PRIVATE opennova_runtime)
target_include_directories(weapon_inventory_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(weapon_inventory_test PRIVATE cxx_std_17)
add_test(NAME weapon_inventory COMMAND weapon_inventory_test)

# Local player view state (the ADS scope ease, the 3P anchor chase, the fov
# policy) at the fixed 62.5 Hz tick — render-cadence invariance — plus the
# input-dispatch gates in front of the FSM requests.
# The DEATH deploy screen's content feed (D-HUD-19 residue b): row loops, sort,
# occupant insertion quirk, status-line kinds, statics gates.
add_executable(deploy_screen_feed_test world/deploy_screen_feed_test.cpp)
target_link_libraries(deploy_screen_feed_test PRIVATE opennova_runtime)
target_include_directories(deploy_screen_feed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(deploy_screen_feed_test PRIVATE cxx_std_17)
add_test(NAME deploy_screen_feed COMMAND deploy_screen_feed_test)

# The gamemus var pump from the local player (world/music_vars.h).
add_executable(music_vars_test world/music_vars_test.cpp)
target_link_libraries(music_vars_test PRIVATE opennova_runtime)
target_compile_features(music_vars_test PRIVATE cxx_std_17)
add_test(NAME music_vars COMMAND music_vars_test)

# The SP objectives panel's row walk (world/objectives_feed.h).
add_executable(objectives_feed_test world/objectives_feed_test.cpp)
target_link_libraries(objectives_feed_test PRIVATE opennova_runtime)
target_compile_features(objectives_feed_test PRIVATE cxx_std_17)
add_test(NAME objectives_feed COMMAND objectives_feed_test)

# The mode-4 death lerp camera (world/death_camera.h): FROM/TO, probe, lerp.
add_executable(death_camera_test world/death_camera_test.cpp)
target_link_libraries(death_camera_test PRIVATE opennova_runtime)
target_include_directories(death_camera_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(death_camera_test PRIVATE cxx_std_17)
add_test(NAME death_camera COMMAND death_camera_test)

# The per-entity death state beside the infantry death edges (world/infantry.h):
# the plyr think cadence + kill-cause bit clear, the edge's attacker fallback,
# and the org0 skin callback's DEATH corpse-fade register.
add_executable(death_state_test world/death_state_test.cpp)
target_link_libraries(death_state_test PRIVATE opennova_runtime)
target_include_directories(death_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(death_state_test PRIVATE cxx_std_17)
add_test(NAME death_state COMMAND death_state_test)

# The local player's view cluster, orchestrated (world/local_player_view.h):
# the scope/binocular/NVG request gates, the tick's arbiter feed + death-camera
# entry edge, the composed frame read.
add_executable(local_player_view_test world/local_player_view_test.cpp)
target_link_libraries(local_player_view_test PRIVATE opennova_runtime)
target_include_directories(local_player_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(local_player_view_test PRIVATE cxx_std_17)
add_test(NAME local_player_view COMMAND local_player_view_test)

add_executable(player_weapon_view_test world/player_weapon_view_test.cpp)
target_link_libraries(player_weapon_view_test PRIVATE opennova_runtime)
target_compile_features(player_weapon_view_test PRIVATE cxx_std_17)
add_test(NAME player_weapon_view COMMAND player_weapon_view_test)

add_executable(present_drains_test world/present_drains_test.cpp)
target_link_libraries(present_drains_test PRIVATE opennova_runtime)
target_compile_features(present_drains_test PRIVATE cxx_std_17)
add_test(NAME present_drains COMMAND present_drains_test)

add_executable(local_player_targeting_test world/local_player_targeting_test.cpp)
target_link_libraries(local_player_targeting_test PRIVATE opennova_runtime)
target_include_directories(local_player_targeting_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(local_player_targeting_test PRIVATE cxx_std_17)
add_test(NAME local_player_targeting COMMAND local_player_targeting_test)

add_executable(special_weapon_parity_test world/special_weapon_parity_test.cpp)
target_link_libraries(special_weapon_parity_test PRIVATE opennova_runtime)
target_include_directories(special_weapon_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(special_weapon_parity_test PRIVATE cxx_std_17)
add_test(NAME special_weapon_parity COMMAND special_weapon_parity_test)

# The attach-command seat-selection mirror (world/vehicle_attach.h
# seat_selection_mode_for_command) and the shared weight table.
add_executable(seat_prediction_test world/seat_prediction_test.cpp)
target_link_libraries(seat_prediction_test PRIVATE opennova_runtime)
target_include_directories(seat_prediction_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(seat_prediction_test PRIVATE cxx_std_17)
add_test(NAME seat_prediction COMMAND seat_prediction_test)

# The mission <-> presentation frame map, the aim ray and the rangefinder clamp
# (world/presentation_frame.h).
add_executable(presentation_frame_test world/presentation_frame_test.cpp)
target_link_libraries(presentation_frame_test PRIVATE opennova_runtime)
target_include_directories(presentation_frame_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(presentation_frame_test PRIVATE cxx_std_17)
add_test(NAME presentation_frame COMMAND presentation_frame_test)

# The occlusion frame camera build (world/occlusion_camera.h): the
# presentation -> mission/render remaps, the five frustum planes, the Q22 rows.
add_executable(occlusion_camera_test world/occlusion_camera_test.cpp)
target_link_libraries(occlusion_camera_test PRIVATE opennova_runtime)
add_test(NAME occlusion_camera COMMAND occlusion_camera_test)

# The local player's presentation law (runtime/world/player_present): the
# camera-mode split, the FP submit AND-composition, the lighting split, the
# local fire-effect gate + gfx pick, the fixed-tick batch order, the per-submit
# CTRL writers, the raw-key latch, the def precedence/memo, the spawn-loadout
# projection.
add_executable(player_present_test world/player_present_test.cpp)
target_link_libraries(player_present_test PRIVATE opennova_runtime)
target_include_directories(player_present_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(player_present_test PRIVATE cxx_std_17)
add_test(NAME player_present COMMAND player_present_test)

add_executable(player_view_test world/player_view_test.cpp)
target_link_libraries(player_view_test PRIVATE opennova_runtime)
target_include_directories(player_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(player_view_test PRIVATE cxx_std_17)
add_test(NAME player_view COMMAND player_view_test)

add_executable(round_move_effect_test world/round_move_effect_test.cpp)
target_link_libraries(round_move_effect_test PRIVATE opennova_runtime)
target_include_directories(round_move_effect_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME round_move_effect COMMAND round_move_effect_test)

# The aircraft attitude integrator pinned through the live client mover (the
# #553 air_attitude.h duplicate was folded into aircraft_client_tick 2026-08-21).
add_executable(aircraft_client_motor_test world/aircraft_client_motor_test.cpp)
target_link_libraries(aircraft_client_motor_test PRIVATE opennova_runtime)
target_include_directories(aircraft_client_motor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME aircraft_client_motor COMMAND aircraft_client_motor_test)

add_executable(vehicle_part_anim_test world/vehicle_part_anim_test.cpp)
target_link_libraries(vehicle_part_anim_test PRIVATE opennova_runtime)
target_include_directories(vehicle_part_anim_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME vehicle_part_anim COMMAND vehicle_part_anim_test)

add_executable(vehicle_attachments_test world/vehicle_attachments_test.cpp)
target_link_libraries(vehicle_attachments_test PRIVATE opennova_runtime)
target_include_directories(vehicle_attachments_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME vehicle_attachments COMMAND vehicle_attachments_test)

add_executable(tp_camera_mount_test world/tp_camera_mount_test.cpp)
target_link_libraries(tp_camera_mount_test PRIVATE opennova_runtime)
target_include_directories(tp_camera_mount_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tp_camera_mount COMMAND tp_camera_mount_test)

# The mouse-look pipeline (pixel deltas -> yaw/pitch BAM): the witnessed integer
# math — sens << 11, the +0x8000 rounding, the scoped zoom divide, flipmouse,
# and the pitch clamps with the prone +40-degree up-limit.
add_executable(player_look_test world/player_look_test.cpp)
target_link_libraries(player_look_test PRIVATE opennova_runtime)
target_include_directories(player_look_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(player_look_test PRIVATE cxx_std_17)
add_test(NAME player_look COMMAND player_look_test)

# Spawn-state init (Entity_ResetToSpawnState @0x4B9610, net-re §5.2b): the entity+36
# bit-1 movement-gate clear + Position backup — the field-init half of the host
# player-spawn machine that the Phase-2 listen-server player spawn reuses.
add_executable(entity_spawn_test world/entity_spawn_test.cpp)
target_link_libraries(entity_spawn_test PRIVATE opennova_runtime)
target_include_directories(entity_spawn_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(entity_spawn_test PRIVATE cxx_std_17)
add_test(NAME entity_spawn COMMAND entity_spawn_test)

# The host's own-player keystone (net-re §5.2b/§5.38): spawn_player (pool-0 player-infantry
# 0x14B9 + gate clear), PlayerBodyInput (the 8-way Player_PackInputStateToEntity port),
# and the infantry motor's local-player branch driving + mirroring the pose to the registry
# Entity that the S2C 0x0A snapshot reads.
add_executable(player_spawn_test world/player_spawn_test.cpp)
target_link_libraries(player_spawn_test PRIVATE opennova_runtime)
target_include_directories(player_spawn_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(player_spawn_test PRIVATE cxx_std_17)
# MSVC coalesces this test's many scoped World fixtures into one Release stack frame.
# World intentionally embeds the fixed projectile ring, so the generated frame is about
# 1.7 MiB even though only one fixture is live at a time; give this test runner room.
if(MSVC)
    target_link_options(player_spawn_test PRIVATE "/STACK:4194304")
endif()
add_test(NAME player_spawn COMMAND player_spawn_test)

# Player spawn-point selection (Server_PositionPlayerForSpawn @0x50cf60 -> Entity_FindBestSpawnPoint
# @0x50ccc0, net-re §5.2c): SP/campaign picks the type-6002 start marker FARTHEST from enemy
# soldiers and never an NPC's position — the faithful fix for "the player spawns on top of NPC 0".
add_executable(spawn_select_test world/spawn_select_test.cpp)
target_link_libraries(spawn_select_test PRIVATE opennova_runtime)
target_include_directories(spawn_select_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(spawn_select_test PRIVATE cxx_std_17)
add_test(NAME spawn_select COMMAND spawn_select_test)

# Advance & Secure zone-slot chain (net-re §5.61; ZoneSlotChain_* @0x4A2350..0x4A2DE0,
# ex-"CWeaponSlotManager"): masks/frontier/adjacency, the 0x0F owned-zone mask, the control
# latch, the 0xFFFE auto-deploy pick, and the 0x0E pick resolve — pinned on the ASH_I5A shape
# (zone 1 team 1 / zone 2 x2 neutral / zone 3 team 2, golden uniformMask 0x8).
add_executable(zone_chain_test world/zone_chain_test.cpp)
target_link_libraries(zone_chain_test PRIVATE opennova_runtime)
target_include_directories(zone_chain_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zone_chain_test PRIVATE cxx_std_17)
add_test(NAME zone_chain COMMAND zone_chain_test)
add_executable(zone_capture_parity_test world/zone_capture_parity_test.cpp)
target_link_libraries(zone_capture_parity_test PRIVATE opennova_runtime)
target_include_directories(zone_capture_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zone_capture_parity_test PRIVATE cxx_std_17)
add_test(NAME zone_capture_parity COMMAND zone_capture_parity_test)

# The authoritative multiplayer rules/stats domain shared by TDM and A&S, with
# Co-op pinned to the existing WAC/BMS outcome path. [orig:
# GameEvent_ProcessScoring @0x52F550; Server_CheckWinConditions @0x51AD40]
add_executable(match_test world/match_test.cpp)
target_link_libraries(match_test PRIVATE opennova_runtime)
target_include_directories(match_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(match_test PRIVATE cxx_std_17)
add_test(NAME match COMMAND match_test)
# The powerup family: the med/ammo pack bind, pickup arms, consume outcomes,
# respawn countdown and the remote-player grant (world/powerup.h;
# docs/world/powerup-re.md).
add_executable(powerup_test world/powerup_test.cpp)
target_link_libraries(powerup_test PRIVATE opennova_runtime)
target_include_directories(powerup_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(powerup_test PRIVATE cxx_std_17)
add_test(NAME powerup COMMAND powerup_test)
add_executable(match_scoring_test world/match_scoring_test.cpp)
target_link_libraries(match_scoring_test PRIVATE opennova_runtime)
target_include_directories(match_scoring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(match_scoring_test PRIVATE cxx_std_17)
add_test(NAME match_scoring COMMAND match_scoring_test)

# Host-authoritative ground-vehicle motor (net-re §5.13 drive-authority witness): the
# items.def physics-block scaling (ItemDef_ParsePhysicsProperty @0x49d870), the drive
# input mapping, steering chase, and speed pipeline of Entity_UpdateVehiclePhysics
# @0x48af00 — a mounted ctrl/drvr player's replicated input moves the vehicle.
add_executable(turret_window_test world/turret_window_test.cpp)
target_link_libraries(turret_window_test PRIVATE opennova_runtime)
target_compile_features(turret_window_test PRIVATE cxx_std_17)
add_test(NAME turret_window COMMAND turret_window_test)

# The emplaced gun channel's stored words (entity+0x322/+0x324): the immediate and
# IsTurret producers of Entity_UpdateChildAttachment @0x4409A0 (the recoil term, the
# 0x92CF34/tick traverse, the local/NPC gunner-yaw tethers) and the window clamp's
# occupant write-back in Entity_UpdateTransformAndTurret @0x440ca0.
add_executable(emplaced_gun_channel_test world/emplaced_gun_channel_test.cpp)
target_link_libraries(emplaced_gun_channel_test PRIVATE opennova_runtime opennova_formats)
target_compile_features(emplaced_gun_channel_test PRIVATE cxx_std_17)
add_test(NAME emplaced_gun_channel COMMAND emplaced_gun_channel_test)

add_executable(vehicle_motor_test world/vehicle_motor_test.cpp)
target_link_libraries(vehicle_motor_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(vehicle_motor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vehicle_motor_test PRIVATE cxx_std_17)
add_test(NAME vehicle_motor COMMAND vehicle_motor_test)

# Unmoved carriers read their placement angles in the spawn form: the motors'
# lazy seeds, the carrier pose, the emplaced gun frame, seat points and the
# attach pre-snap (Entity_SpawnFromBMSRecord @0x40EB42..0x40EBA6).
add_executable(vehicle_placement_angles_test world/vehicle_placement_angles_test.cpp)
target_link_libraries(vehicle_placement_angles_test PRIVATE opennova_runtime)
target_compile_features(vehicle_placement_angles_test PRIVATE cxx_std_17)
add_test(NAME vehicle_placement_angles COMMAND vehicle_placement_angles_test)

add_executable(vehicle_followups_test world/vehicle_followups_test.cpp)
target_link_libraries(vehicle_followups_test PRIVATE opennova_runtime)
add_test(NAME vehicle_followups COMMAND vehicle_followups_test)

# The joiner-side watercraft prediction (D-NET-196 B-facet): fast-boat glide
# beyond the chase-only sustain limit, coast-down, and the steer chain.
add_executable(watercraft_client_motor_test world/watercraft_client_motor_test.cpp)
target_link_libraries(watercraft_client_motor_test PRIVATE opennova_runtime)
add_test(NAME watercraft_client_motor COMMAND watercraft_client_motor_test)

# The USE-ITEM mount chain (Entity_ToggleVehicleMount @0x436950 + the nearest-seat scan
# @0x435d50 + the deck best-seat @0x4351f0), the BMS Player mount triggers (subs 38-41),
# the vehicle AI-driver/parked leg (@0x48bc12-0x48c034), and the player deploy group
# stamp (@0x519fd0).
add_executable(vehicle_mount_test world/vehicle_mount_test.cpp)
target_link_libraries(vehicle_mount_test PRIVATE opennova_runtime)
target_include_directories(vehicle_mount_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vehicle_mount_test PRIVATE cxx_std_17)
add_test(NAME vehicle_mount COMMAND vehicle_mount_test)

# AI route parity: the BMS loader's nav table (Mission_LoadBMSFile @0x40F4E0), the
# aircraft AI leg's state words and the player pilot's PRETTY hold
# (Entity_UpdateAircraftPhysics @0x490310).
add_executable(route_parity_test world/route_parity_test.cpp)
target_link_libraries(route_parity_test PRIVATE opennova_runtime)
target_include_directories(route_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(route_parity_test PRIVATE cxx_std_17)
add_test(NAME route_parity COMMAND route_parity_test)

# Root-motion track grill vs real JO clips (AnimMap_UpdateEntity @0x40b5f0): the .bad
# events array is the per-frame root record; rec[3] = bottom, the hips' height above the
# ground, whose 16.16 change is the vertical root motion. Asset-gated: absent files skip
# (CI-safe).
add_executable(root_motion_test anim/root_motion_test.cpp)
target_link_libraries(root_motion_test PRIVATE opennova_formats)
target_include_directories(root_motion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(root_motion_test PRIVATE cxx_std_17)
opennova_add_gated_test(root_motion COMMAND root_motion_test)

# WAC scripting. The registry oracle (165-command ISA + 28 param types from the
# binary) exercises only the format half (engine/formats/wac); VM behavior (var
# math, entity mutation, temporal/edge firing, env, authority) and the shipped
# .wac corpus, compiled to the original compiler's listings, run against the
# world (dirs via argv, else OPENNOVA_JO_ASSETS; Skipped without it).
add_executable(wac_registry_oracle_test wac/wac_registry_oracle_test.cpp)
target_link_libraries(wac_registry_oracle_test PRIVATE opennova_formats)
target_include_directories(wac_registry_oracle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(wac_registry_oracle_test PRIVATE cxx_std_17)
add_test(NAME wac_registry_oracle COMMAND wac_registry_oracle_test)

foreach(test behavior corpus weapon_input voice actors projectiles effects sounds state players help dispatch_sweep)
    add_executable(wac_${test}_test wac/wac_${test}_test.cpp)
    target_link_libraries(wac_${test}_test PRIVATE opennova_runtime)
    target_include_directories(wac_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(wac_${test}_test PRIVATE cxx_std_17)
endforeach()
add_test(NAME wac_behavior COMMAND wac_behavior_test)
add_test(NAME wac_weapon_input COMMAND wac_weapon_input_test)
add_test(NAME wac_voice COMMAND wac_voice_test)
add_test(NAME wac_actors COMMAND wac_actors_test)
add_test(NAME wac_projectiles COMMAND wac_projectiles_test)
add_test(NAME wac_effects COMMAND wac_effects_test)
add_test(NAME wac_sounds COMMAND wac_sounds_test)
add_test(NAME wac_state COMMAND wac_state_test)
add_test(NAME wac_players COMMAND wac_players_test)
add_test(NAME wac_help COMMAND wac_help_test)
add_test(NAME wac_dispatch_sweep COMMAND wac_dispatch_sweep_test)
# The corpus sweep walks OPENNOVA_JO_ASSETS (argv dirs override); it reports
# Skipped without it.
opennova_add_gated_test(wac_corpus COMMAND wac_corpus_test)

# The original compiler and VM on synthetic programs: the fixtures are the
# pinned executable's own listings and states (scripts/oracles/wac_parity.py).
add_executable(wac_retail_vectors_test wac/wac_retail_vectors_test.cpp)
target_link_libraries(wac_retail_vectors_test PRIVATE opennova_runtime)
target_include_directories(wac_retail_vectors_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(wac_retail_vectors_test PRIVATE cxx_std_17)
add_test(NAME wac_retail_vectors COMMAND wac_retail_vectors_test)

# The WAC program surface the retired WacProgram / WacState bindings served
# (ADR 0043 d10): the compile surface, the layered load, and an installed
# program's 62-tick execution, pause gate, eager start and restart rewind.
add_executable(wac_program_surface_test wac/wac_program_surface_test.cpp)
target_link_libraries(wac_program_surface_test PRIVATE opennova_runtime)
target_include_directories(wac_program_surface_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(wac_program_surface_test PRIVATE cxx_std_17)
add_test(NAME wac_program_surface COMMAND wac_program_surface_test)

# The layered game.wac/server.wac/<mission>.wac load every mission boot walks
# (runtime/wac/wac_layered_load): retail layer order as one program, the
# absent BMS-only case, and the lenient-vs-strict diagnostic policies (strict
# = the dedicated golden host's fatal mode).
add_executable(wac_layered_load_test wac/wac_layered_load_test.cpp)
target_link_libraries(wac_layered_load_test PRIVATE opennova_runtime)
target_include_directories(wac_layered_load_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(wac_layered_load_test PRIVATE cxx_std_17)
add_test(NAME wac_layered_load COMMAND wac_layered_load_test)

# WAC weather actions are authoritative network sources: exercise the compiled
# script through the real per-connection 0x0A phase-2 projection.
add_executable(wac_environment_wire_test wac/wac_environment_wire_test.cpp)
target_link_libraries(wac_environment_wire_test PRIVATE opennova_runtime opennova_net)
target_include_directories(wac_environment_wire_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(wac_environment_wire_test PRIVATE cxx_std_17)
add_test(NAME wac_environment_wire COMMAND wac_environment_wire_test)

# Cross-system: BMS event runtime + WAC share one world/var store (engine/runtime/mission
# event_runtime + opennova_wac). MisvarChange(V#) and WAC eq(V#) see the same var.
add_executable(event_runtime_test mission/event_runtime_test.cpp)
target_link_libraries(event_runtime_test PRIVATE opennova_runtime)
target_include_directories(event_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(event_runtime_test PRIVATE cxx_std_17)
add_test(NAME event_runtime_bms COMMAND event_runtime_test)

# BMS event parity: the retail-witnessed trigger/action semantics (bms-event-runtime-re)
# through the public BmsEventSystem / EntityCommands seams.
add_executable(bms_event_parity_test mission/bms_event_parity_test.cpp)
target_link_libraries(bms_event_parity_test PRIVATE opennova_runtime)
target_include_directories(bms_event_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bms_event_parity_test PRIVATE cxx_std_17)
if(MSVC)
    target_link_options(bms_event_parity_test PRIVATE "/STACK:4194304")
endif()
add_test(NAME bms_event_parity COMMAND bms_event_parity_test)

# The BMS HUD relays: the objective notification (actions 35/36), the
# SubGoalWon/SubGoalLost announcement relay and the host's S2C 0x3F fan.
add_executable(bms_hud_relay_test mission/bms_hud_relay_test.cpp)
target_link_libraries(bms_hud_relay_test PRIVATE opennova_runtime)
target_include_directories(bms_hud_relay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bms_hud_relay_test PRIVATE cxx_std_17)
if(MSVC)
    target_link_options(bms_hud_relay_test PRIVATE "/STACK:4194304")
endif()
add_test(NAME bms_hud_relay COMMAND bms_hud_relay_test)

# The BMS event runtime replayed against the original's instructions: the
# condition/chain/action/entry/pass witnesses scripts/oracles/
# mission_event_parity.py captured in the retail PE (mission/fixtures/
# mission_event_vectors.inc), rebuilt through the raw-record seam.
add_executable(mission_event_vectors_test mission/mission_event_vectors_test.cpp)
target_link_libraries(mission_event_vectors_test PRIVATE opennova_runtime)
target_include_directories(mission_event_vectors_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_event_vectors_test PRIVATE cxx_std_17)
add_test(NAME mission_event_vectors COMMAND mission_event_vectors_test)

# Mission -> world promotion: spawn a parsed mission's entities into the world pools,
# allocate AI brains per organic, build the waypoint nav table, then tick the AI to prove
# entities patrol their authored routes (the connective tissue for the live sim).
add_executable(promote_test mission/promote_test.cpp)
target_link_libraries(promote_test PRIVATE opennova_runtime)
target_include_directories(promote_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(promote_test PRIVATE cxx_std_17)
# This long-running parity harness deliberately keeps many heavyweight World
# fixtures in one main. MSVC reserves their debug/optimized stack slots in a
# single frame; Entity layout growth can therefore exceed Windows' 1 MiB PE
# default before main executes. Keep the larger reserve local to this test.
if(MSVC)
    target_link_options(promote_test PRIVATE "/STACK:4194304")
endif()
add_test(NAME mission_promote COMMAND promote_test)

# The <mission>.bin RTXT harvest (runtime/mission/mission_text.h): the
# briefing pages and the briefing2 -> briefing fallback, the [Locations] /
# [PeopleNames] numeric-key sections, a rejected table.
add_executable(mission_text_test mission/mission_text_test.cpp)
target_link_libraries(mission_text_test PRIVATE opennova_runtime)
target_compile_features(mission_text_test PRIVATE cxx_std_17)
add_test(NAME mission_text COMMAND mission_text_test)

# The F3 debug oracles over the kernel (runtime/mission/debug_oracles.h): the
# hitbox view's stand-in fallback and its exclusions, the pick's zero ray and
# empty-world miss.
add_executable(debug_oracles_test mission/debug_oracles_test.cpp)
target_link_libraries(debug_oracles_test PRIVATE opennova_runtime)
target_compile_features(debug_oracles_test PRIVATE cxx_std_17)
if(MSVC)
    target_link_options(debug_oracles_test PRIVATE "/STACK:4194304")
endif()
add_test(NAME mission_debug_oracles COMMAND debug_oracles_test)

# S9 (ADR 0028): the mission boot's file-resolution rules — the mission-text
# fallback and the .aip resolve (the boot ORDER itself is pinned through
# MissionKernel::boot_trace in mission_kernel_test, ADR 0043 slice E9).
add_executable(mission_runtime_boot_test mission/runtime_boot_test.cpp)
target_link_libraries(mission_runtime_boot_test PRIVATE opennova_runtime)
add_test(NAME mission_runtime_boot COMMAND mission_runtime_boot_test)

# The mission kernel (ADR 0042 d3) over a synthetic mission + in-memory file
# source: open_document, the boot side effects (promotion, WAC install, the
# origin spawn, the baseline), the no-net tick, the teleport/health seams, and
# the CanFire verdict. Ungated — no retail assets.
add_executable(mission_kernel_test mission/mission_kernel_test.cpp)
target_link_libraries(mission_kernel_test PRIVATE opennova_runtime)
target_include_directories(mission_kernel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_kernel_test PRIVATE cxx_std_17)
add_test(NAME mission_kernel COMMAND mission_kernel_test)

# Per-model infantry animation loading with an absent default, using synthetic
# ADM/BAD fixtures: movement, fallback, late spawn, restore and replica rearm.
add_executable(mission_infantry_anim_test mission/mission_infantry_anim_test.cpp)
target_link_libraries(mission_infantry_anim_test PRIVATE opennova_runtime)
target_include_directories(mission_infantry_anim_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_infantry_anim_test PRIVATE cxx_std_17)
add_test(NAME mission_infantry_anim COMMAND mission_infantry_anim_test)

# The kernel's lifecycle contract over the same synthetic mission, ungated: the
# ordering guards, the sealed play-start baseline (capture/restore rewinds the
# registry, the player, the clock and the event latches), the strict-vs-lenient
# WAC diagnostic policy, and the no-terrain path.
add_executable(mission_kernel_lifecycle_test mission/mission_kernel_lifecycle_test.cpp)
target_link_libraries(mission_kernel_lifecycle_test PRIVATE opennova_runtime)
target_include_directories(mission_kernel_lifecycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_kernel_lifecycle_test PRIVATE cxx_std_17)
add_test(NAME mission_kernel_lifecycle COMMAND mission_kernel_lifecycle_test)

# The SP mission lifecycle after the round end: the win epilog's and the lose
# screen's stage machines, the round-over keys, the in-game RESTART, the main
# frame's exit router and the SP restart as a fresh launch (ungated).
add_executable(sp_mission_lifecycle_test mission/sp_mission_lifecycle_test.cpp)
target_link_libraries(sp_mission_lifecycle_test PRIVATE opennova_runtime)
target_include_directories(sp_mission_lifecycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sp_mission_lifecycle_test PRIVATE cxx_std_17)
add_test(NAME sp_mission_lifecycle COMMAND sp_mission_lifecycle_test)

# inmatch::HostRole over the kernel (ADR 0043 d3), ungated: the SP listen
# bring-up and its per-tick frame over a null socket, the local C2S gameplay
# drain, and the dedicated (HostOnly) bring-up.
add_executable(host_role_test mission/host_role_test.cpp)
target_link_libraries(host_role_test PRIVATE opennova_net opennova_runtime)
target_compile_features(host_role_test PRIVATE cxx_std_17)
add_test(NAME host_role COMMAND host_role_test)

# The death screen's spectator over the listen host's role and kernel: the
# free-fly motor's sub-modes, the input filter, the wrap placement and the
# replica track legs.
add_executable(spectator_session_test mission/spectator_session_test.cpp)
target_link_libraries(spectator_session_test PRIVATE opennova_net opennova_runtime)
target_compile_features(spectator_session_test PRIVATE cxx_std_17)
add_test(NAME spectator_session COMMAND spectator_session_test)

# The tick digest (ADR 0043): the per-tick state hash a structural slice must
# leave unchanged. The ctest leg is the synthetic mission (committed chain,
# determinism); `tick_digest_test --mission <name>` is the local retail tool
# behind scripts/parity/tick_digest.py and is never a ctest leg.
add_executable(tick_digest_test mission/tick_digest_test.cpp)
target_link_libraries(tick_digest_test PRIVATE opennova_test_rig)
target_include_directories(tick_digest_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(tick_digest_test PRIVATE cxx_std_17)
add_test(NAME tick_digest COMMAND tick_digest_test)

# Emplacement mount: the seat model + canonical root/child seat selection and attach
# primitives + the BMS AttachToEmplaced path (event -> proximity mount) + the
# snapshot/restore rewind of a mount.
add_executable(mount_test mission/mount_test.cpp)
target_link_libraries(mount_test PRIVATE opennova_runtime)
target_include_directories(mount_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mount_test PRIVATE cxx_std_17)
add_test(NAME mission_mount COMMAND mount_test)

# ----------------------------------------------------------------------------
# Net stack tests (relanded from PR #37)
# ----------------------------------------------------------------------------

# novacrypto tests
add_executable(url_cipher_test novacrypto/url_cipher_test.cpp)
target_link_libraries(url_cipher_test PRIVATE opennova_net)
target_include_directories(url_cipher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME url_cipher COMMAND url_cipher_test)

add_executable(crc32_test novacrypto/crc32_test.cpp)
target_link_libraries(crc32_test PRIVATE opennova_net)
target_include_directories(crc32_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME crc32 COMMAND crc32_test)

add_executable(nwu_test novacrypto/nwu_test.cpp)
target_link_libraries(nwu_test PRIVATE opennova_net)
target_include_directories(nwu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nwu COMMAND nwu_test)

add_executable(pubcrypto_test novacrypto/pubcrypto_test.cpp)
target_link_libraries(pubcrypto_test PRIVATE opennova_net)
target_include_directories(pubcrypto_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pubcrypto COMMAND pubcrypto_test)

add_executable(epask_test novacrypto/epask_test.cpp)
target_link_libraries(epask_test PRIVATE opennova_net)
target_include_directories(epask_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME epask COMMAND epask_test)

# napi tests
add_executable(envelope_test napi/envelope_test.cpp)
target_link_libraries(envelope_test PRIVATE opennova_net)
target_include_directories(envelope_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME envelope COMMAND envelope_test)

add_executable(napi_literal_test napi/literal_test.cpp)
target_link_libraries(napi_literal_test PRIVATE opennova_net)
target_include_directories(napi_literal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME napi_literal COMMAND napi_literal_test)

add_executable(tlv_test napi/tlv_test.cpp)
target_link_libraries(tlv_test PRIVATE opennova_net)
target_include_directories(tlv_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tlv COMMAND tlv_test)

add_executable(session_test napi/session_test.cpp)
target_link_libraries(session_test PRIVATE opennova_net)
target_include_directories(session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME session COMMAND session_test)

# novaworld tests
add_executable(registration_url_test novaworld/registration_url_test.cpp)
target_link_libraries(registration_url_test PRIVATE opennova_net)
target_include_directories(registration_url_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME registration_url COMMAND registration_url_test)

add_executable(gate_response_test novaworld/gate_response_test.cpp)
target_link_libraries(gate_response_test PRIVATE opennova_net)
target_include_directories(gate_response_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gate_response COMMAND gate_response_test)

add_executable(gate_metrics_test novaworld/gate_metrics_test.cpp)
target_link_libraries(gate_metrics_test PRIVATE opennova_net)
add_test(NAME gate_metrics COMMAND gate_metrics_test)

add_executable(gate_probe_test novaworld/gate_probe_test.cpp)
target_link_libraries(gate_probe_test PRIVATE opennova_net)
target_include_directories(gate_probe_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gate_probe COMMAND gate_probe_test)

add_executable(lobby_update_test novaworld/lobby_update_test.cpp)
target_link_libraries(lobby_update_test PRIVATE opennova_net)
target_include_directories(lobby_update_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME lobby_update COMMAND lobby_update_test)

add_executable(gate_server_loopback_test novaworld/gate_server_loopback_test.cpp)
target_link_libraries(gate_server_loopback_test PRIVATE opennova_net opennova_app_common)
target_include_directories(gate_server_loopback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gate_server_loopback COMMAND gate_server_loopback_test)

# The real GateListener on port 0 over loopback UDP: the port start() reports is
# the one that answers a probe and the one the reply advertises as POSTIPPORT,
# and a second listener on that port fails start() (same listener-source recipe
# as the NW UDP e2e tests below).
add_executable(gate_listener_test
    novaworld/gate_listener_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/gate_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(gate_listener_test PRIVATE opennova_novaworld_service opennova_app_common)
target_include_directories(gate_listener_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME gate_listener COMMAND gate_listener_test)

add_executable(gsb_roundtrip_test novaworld/gsb_roundtrip_test.cpp)
target_link_libraries(gsb_roundtrip_test PRIVATE opennova_net)
target_include_directories(gsb_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gsb_roundtrip COMMAND gsb_roundtrip_test)

# ADR 0010 Phase 2 — gsb_parse_response is the exact client inverse of the builder.
add_executable(gsb_parse_roundtrip_test novaworld/gsb_parse_roundtrip_test.cpp)
target_link_libraries(gsb_parse_roundtrip_test PRIVATE opennova_net)
target_include_directories(gsb_parse_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gsb_parse_roundtrip COMMAND gsb_parse_roundtrip_test)

# The browser's per-row ping display fold (-4 -> -3, negative -> -2, 0 -> ms)
# and the witnessed sweep constants.
add_executable(ping_sweep_fold_test novaworld/ping_sweep_fold_test.cpp)
target_link_libraries(ping_sweep_fold_test PRIVATE opennova_net)
target_include_directories(ping_sweep_fold_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ping_sweep_fold COMMAND ping_sweep_fold_test)
# The ping-sweep lifecycle: a refresh's sweep supersedes the running one at once.
find_package(Threads REQUIRED)
add_executable(ping_sweep_runner_test novaworld/ping_sweep_runner_test.cpp)
target_link_libraries(ping_sweep_runner_test PRIVATE opennova_net Threads::Threads)
target_include_directories(ping_sweep_runner_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ping_sweep_runner COMMAND ping_sweep_runner_test)

# Retail dispatcher semantics (D-NET-190..193): SVRS accumulation, the gated
# "GSB " reset, undersized-record skips, and the XXXX-terminator hardening.
add_executable(gsb_retail_semantics_test novaworld/gsb_retail_semantics_test.cpp)
target_link_libraries(gsb_retail_semantics_test PRIVATE opennova_net)
target_include_directories(gsb_retail_semantics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gsb_retail_semantics COMMAND gsb_retail_semantics_test)

# The server browser's table model: cells, ping states, quick filters and
# search, the sort orders (unmeasured pings last either way), details and hover text.
add_executable(server_browser_test novaworld/server_browser_test.cpp)
target_link_libraries(server_browser_test PRIVATE opennova_net)
target_include_directories(server_browser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME server_browser COMMAND server_browser_test)

# Byte-level oracle: decode the genuine .204 server-browser blob
# (fixtures/novaworld/nw204_jop_2.gsb) through our retail-faithful parser.
add_executable(gsb_real204_decode_test novaworld/gsb_real204_decode_test.cpp)
target_link_libraries(gsb_real204_decode_test PRIVATE opennova_net)
target_include_directories(gsb_real204_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gsb_real204_decode_test PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld")
add_test(NAME gsb_real204_decode COMMAND gsb_real204_decode_test)

# ADR 0010 / P7 Part 2 — the lobby HTTP flow (login / GSB / NWJoin machines) driven bytes-in/out.
add_executable(http_flow_test novaworld/http_flow_test.cpp)
target_link_libraries(http_flow_test PRIVATE opennova_net)
add_test(NAME http_flow COMMAND http_flow_test)

# ADR 0010 / P7 Part 2 (B2) — the lobby var-builders (host request/update + identity + host:port)
# proven byte-identical to the bindings' hand-assembly.
add_executable(lobby_vars_test novaworld/lobby_vars_test.cpp)
target_link_libraries(lobby_vars_test PRIVATE opennova_net)
add_test(NAME lobby_vars COMMAND lobby_vars_test)

add_executable(peer_addr_test npwire/peer_addr_test.cpp)
target_link_libraries(peer_addr_test PRIVATE opennova_net)
target_include_directories(peer_addr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME peer_addr COMMAND peer_addr_test)

# The shared BuildOutgoingPackets planner and the CS block it reads (npwire/outgoing_packets.h,
# npwire/cs_config.h): split under the field-13 ceiling, the field-14 budget, both overflow policies.
add_executable(npwire_outgoing_packets_test npwire/outgoing_packets_test.cpp)
target_link_libraries(npwire_outgoing_packets_test PRIVATE opennova_net)
add_test(NAME npwire_outgoing_packets COMMAND npwire_outgoing_packets_test)

add_executable(net_ports_test npwire/net_ports_test.cpp)
target_link_libraries(net_ports_test PRIVATE opennova_net)
target_include_directories(net_ports_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME net_ports COMMAND net_ports_test)

add_executable(game_type_policy_test npwire/game_type_policy_test.cpp)
target_link_libraries(game_type_policy_test PRIVATE opennova_runtime)
target_include_directories(game_type_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME game_type_policy COMMAND game_type_policy_test)

add_executable(protocol_message_test novaworld/protocol_message_test.cpp)
target_link_libraries(protocol_message_test PRIVATE opennova_net)
target_include_directories(protocol_message_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME protocol_message COMMAND protocol_message_test)

add_executable(session_protocol_test novaworld/session_protocol_test.cpp)
target_link_libraries(session_protocol_test PRIVATE opennova_net)
target_include_directories(session_protocol_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME session_protocol COMMAND session_protocol_test)

# Connection registry / manager tests
add_executable(connection_registry_test novaworld/connection_registry_test.cpp)
target_link_libraries(connection_registry_test PRIVATE opennova_novaworld_service)
target_include_directories(connection_registry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME connection_registry COMMAND connection_registry_test)

add_executable(connection_manager_test novaworld/connection_manager_test.cpp)
target_link_libraries(connection_manager_test PRIVATE opennova_novaworld_service)
target_include_directories(connection_manager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME connection_manager COMMAND connection_manager_test)

# SQLite wrapper + migration runner tests
add_executable(db_sqlite_test novaworld/db_sqlite_test.cpp)
target_link_libraries(db_sqlite_test PRIVATE opennova_novaworld_service)
target_include_directories(db_sqlite_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME db_sqlite COMMAND db_sqlite_test)

# One connection per thread under load (issue #981): registrations race host
# writes and roster readers on one WAL file, each thread on its own pooled
# connection. Compiles the server's auth.cpp (create_user) directly: it joins
# opennova_novaworld_server_core only under BUILD_NOVAWORLD_HTTP, and this test
# runs on every build.
add_executable(db_concurrency_test
    novaworld/db_concurrency_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/auth.cpp)
target_link_libraries(db_concurrency_test
    PRIVATE opennova_novaworld_service opennova_bcrypt Threads::Threads)
target_include_directories(db_concurrency_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(db_concurrency_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME db_concurrency COMMAND db_concurrency_test)

# The NovaWorld site's login sessions and roles (web_sessions, players.role):
# the token kept as its SHA-256, the per-account cap, the sliding expiry and
# its race (threads on a WAL file), the refusals, the revocations, the prune and
# the one-shot bootstrap promotion, on a migrated in-memory database. Compiles the
# server's web_session.cpp and auth.cpp directly, as db_concurrency_test does,
# so it runs on every build, not only under BUILD_NOVAWORLD_HTTP.
add_executable(web_session_test
    novaworld/web_session_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/auth.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/web_session.cpp)
target_link_libraries(web_session_test
    PRIVATE opennova_novaworld_service opennova_bcrypt Threads::Threads)
target_include_directories(web_session_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(web_session_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME web_session COMMAND web_session_test)

# The credential routes' brake: token buckets on a hand-stepped clock and the
# trusted-proxy client address. The server's rate_limiter.cpp compiled in
# directly, so it runs on every build.
add_executable(rate_limiter_test
    novaworld/rate_limiter_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/rate_limiter.cpp)
target_link_libraries(rate_limiter_test PRIVATE opennova_io)
target_compile_features(rate_limiter_test PRIVATE cxx_std_17)
target_include_directories(rate_limiter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME rate_limiter COMMAND rate_limiter_test)

# Phase A — ClientHello/ClientAuth serializer roundtrip
add_executable(session_hello_roundtrip_test novaworld/session_hello_roundtrip_test.cpp)
target_link_libraries(session_hello_roundtrip_test PRIVATE opennova_net)
target_include_directories(session_hello_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME session_hello_roundtrip COMMAND session_hello_roundtrip_test)

# Phase D — LobbySession dispatch tests
add_executable(lobby_session_test novaworld/lobby_session_test.cpp)
target_link_libraries(lobby_session_test PRIVATE opennova_novaworld_service opennova_net)
target_include_directories(lobby_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME lobby_session COMMAND lobby_session_test)

# ADR 0010 Phase 1 — ClientSession handshake (hk echo + 0x83 verify) driven
# in-process against the real server-side parsers/builders + LobbySession.
add_executable(client_session_loopback_test novaworld/client_session_loopback_test.cpp)
target_link_libraries(client_session_loopback_test PRIVATE opennova_novaworld_service opennova_net)
target_include_directories(client_session_loopback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_session_loopback COMMAND client_session_loopback_test)

# NW-S3 closure: the 0x42 join carries the retail CU var set (make_novaworld_join_cu
# builder + a ClientSession actually emitting the chunks in its ClientAuth).
add_executable(client_session_cu_test novaworld/client_session_cu_test.cpp)
target_link_libraries(client_session_cu_test PRIVATE opennova_net)
target_include_directories(client_session_cu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_session_cu COMMAND client_session_cu_test)

# Host registration through the REAL apps/novaworld_server NwUdpListener over a
# loopback UDP socket (not the in-process LobbySession mock): drives ClientSession
# to Verified, sends a ClientHostRequest, asserts the host shows in snapshot_hosted
# (the /api/hosts + GSB source). Compiles the listener sources directly — they live
# in the server exe, not a lib. No DB (snapshot reads in-memory lobby state).
# The engine NWU driver + host role (net/novaworld/nwu_lobby_session.h, nwu_host_role.h) over
# real loopback UDP against the real NwUdpListener and an in-test gate: the probe, the
# handshake, a duplicate gate reply ignored, the registration, a roster change, the 1860-tick
# refresh and the stop.
add_executable(nwu_host_session_e2e_test
    novaworld/nwu_host_session_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nwu_host_session_e2e_test
    PRIVATE opennova_novaworld_service opennova_runtime opennova_app_common)
target_include_directories(nwu_host_session_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
add_test(NAME nwu_host_session_e2e COMMAND nwu_host_session_e2e_test)
set_tests_properties(nwu_host_session_e2e PROPERTIES RUN_SERIAL TRUE)

add_executable(nw_host_register_e2e_test
    novaworld/nw_host_register_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nw_host_register_e2e_test
    PRIVATE opennova_novaworld_service opennova_runtime opennova_app_common)
target_include_directories(nw_host_register_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
add_test(NAME nw_host_register_e2e COMMAND nw_host_register_e2e_test)
# This is the only test that drives the complete listener lifecycle through
# real UDP sockets. Its one-second negative/retry receive windows are protocol
# assertions, not load-test budgets, so CPU-saturated parallel ctest runs can
# manufacture packet timeouts that never reproduce in isolation.
set_tests_properties(nw_host_register_e2e PROPERTIES RUN_SERIAL TRUE)

# The lobby's answer to a stock client's 0x44 resend request, through the real
# listener over loopback UDP (same listener-source recipe as above).
add_executable(nw_lobby_resend_e2e_test
    novaworld/nw_lobby_resend_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nw_lobby_resend_e2e_test
    PRIVATE opennova_novaworld_service opennova_runtime opennova_app_common)
target_include_directories(nw_lobby_resend_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
add_test(NAME nw_lobby_resend_e2e COMMAND nw_lobby_resend_e2e_test)
set_tests_properties(nw_lobby_resend_e2e PROPERTIES RUN_SERIAL TRUE)

# The service's ServerCommand / ServerStopHosting pushes to a listed host, through
# the real listener over loopback UDP (same listener-source recipe as above): the
# RID resolves once hosting, the pushes land as the host session's notices, a
# lost push comes back after the send-interval leg's gap and the client's 0x44,
# and the stop takes the row out of the browser. Its one-second recovery window
# is a protocol assertion, so it runs serially.
add_executable(nw_server_command_push_e2e_test
    novaworld/nw_server_command_push_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nw_server_command_push_e2e_test
    PRIVATE opennova_novaworld_service opennova_runtime opennova_app_common)
target_include_directories(nw_server_command_push_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
add_test(NAME nw_server_command_push_e2e COMMAND nw_server_command_push_e2e_test)
set_tests_properties(nw_server_command_push_e2e PROPERTIES RUN_SERIAL TRUE)

# ADR 0010 Phase 3 — EPASK account-login wire format. Builds the NWLogin.dll
# credential POST body with the login helpers and decodes it through a copy of
# the server's parse_form_body + EPASK path, then checks the cookie jar carries
# NWHANDLE/PCID onto the later GSB fetch (grill NW-S5/B3).
add_executable(http_login_test novaworld/http_login_test.cpp)
target_link_libraries(http_login_test PRIVATE opennova_net)
target_include_directories(http_login_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME http_login COMMAND http_login_test)

# ADR 0010 Phase 5 — the NWJoin .joi connection-string parser (host address +
# tokens) and url_cipher NK round-trip.
add_executable(joi_parse_test novaworld/joi_parse_test.cpp)
target_link_libraries(joi_parse_test PRIVATE opennova_net)
target_include_directories(joi_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME joi_parse COMMAND joi_parse_test)

# The service's NWJoin.dll / NWHost.dll relay leg selection: a stale tag cookie
# never hijacks a fresh first call.
add_executable(relay_request_test novaworld/relay_request_test.cpp)
target_link_libraries(relay_request_test PRIVATE opennova_net)
target_include_directories(relay_request_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME relay_request COMMAND relay_request_test)

add_executable(join_identity_test novaworld/join_identity_test.cpp)
target_link_libraries(join_identity_test PRIVATE opennova_net)
target_include_directories(join_identity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME join_identity COMMAND join_identity_test)

# ADR 0010 Phase 5 — the JointOperations ClientHello (proto switch): PN flips to
# JointOperations with a distinct PG; the NOVAWORLDUDP path is unchanged.
add_executable(jointops_hello_test novaworld/jointops_hello_test.cpp)
target_link_libraries(jointops_hello_test PRIVATE opennova_net)
target_include_directories(jointops_hello_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME jointops_hello COMMAND jointops_hello_test)

# ADR 0010 real-NW parity — decodes the genuine .204 lobby capture (the oracle):
# envelope + outer NWU + per-session SCRK, dumps every container so our builders
# can be matched field for field. fixtures/novaworld/nw204_lobby.hexcap.
add_executable(nw204_lobby_decode_test novaworld/nw204_lobby_decode_test.cpp)
target_link_libraries(nw204_lobby_decode_test PRIVATE opennova_net)
target_include_directories(nw204_lobby_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw204_lobby_decode_test PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld")
add_test(NAME nw204_lobby_decode COMMAND nw204_lobby_decode_test)

# Inline-pcap decoder regression — crafts tiny in-memory pcaps and round-trips
# the 0x0D / 0x20 pool decoders through the full S2C stack (encode -> frame ->
# NWU -> envelope -> build_pcap_udp -> read_pcap_udp -> decode). No capture
# fixtures, so it runs unconditionally in CI.
add_executable(nw_pool_decode_unit_test novaworld/nw_pool_decode_unit_test.cpp)
target_link_libraries(nw_pool_decode_unit_test
    PRIVATE opennova_net opennova_base)
target_include_directories(nw_pool_decode_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_pool_decode_unit COMMAND nw_pool_decode_unit_test)

# CaptureDecoder streaming-equivalence — pins that a sequence of push() calls
# yields byte-identical InGameMessages to one batch decode_capture_to_messages()
# (the contract the in-engine live net client relies on) over an inline
# two-session capture.
add_executable(nw_capture_decoder_test novaworld/nw_capture_decoder_test.cpp)
target_link_libraries(nw_capture_decoder_test
    PRIVATE opennova_net opennova_base)
target_include_directories(nw_capture_decoder_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_capture_decoder COMMAND nw_capture_decoder_test)

# Streaming pcap reader — stream_pcap_udp_file must match the whole-file
# read_pcap_udp byte-for-byte and honor early-exit (so capture consumers can
# stream multi-GB captures without loading them into RAM). No fixtures, CI.
add_executable(nw_pcap_stream_test novaworld/nw_pcap_stream_test.cpp)
target_link_libraries(nw_pcap_stream_test PRIVATE opennova_base)
target_include_directories(nw_pcap_stream_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_pcap_stream COMMAND nw_pcap_stream_test)

add_executable(nw_pcap_writer_test novaworld/nw_pcap_writer_test.cpp)
target_link_libraries(nw_pcap_writer_test PRIVATE opennova_base)
target_include_directories(nw_pcap_writer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_pcap_writer COMMAND nw_pcap_writer_test)

# Per-entity compact-record decoders — §5.10 player / §5.13 vehicle / §5.14
# infantry. Hand-built input vectors mirror the wire layout; no capture
# dependency, so the test runs unconditionally in CI.
add_executable(nw_ingame_compact_records_test novaworld/nw_ingame_compact_records_test.cpp)
target_link_libraries(nw_ingame_compact_records_test PRIVATE opennova_net)
target_include_directories(nw_ingame_compact_records_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_compact_records COMMAND nw_ingame_compact_records_test)

# C2S 0x0C extended (type-10) uplink decoder — §5.10. Byte vectors embedded
# verbatim from the 3-player loopback pcap (host_and_join…_threeplayers…).
add_executable(nw_ingame_c2s_uplink_test novaworld/nw_ingame_c2s_uplink_test.cpp)
target_link_libraries(nw_ingame_c2s_uplink_test PRIVATE opennova_net)
target_include_directories(nw_ingame_c2s_uplink_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_c2s_uplink COMMAND nw_ingame_c2s_uplink_test)

# In-match S2C encoders — loopback identity (encode -> decode field-identical),
# ADR 0011 §4. encode side ports NetPacket_SerializeEntityPoolToPacket @0x503460; the
# decode side is verified vs NapiNPClientMsg_0x020 @0x425C00. No capture dep.
add_executable(nw_ingame_encode_test novaworld/nw_ingame_encode_test.cpp)
target_link_libraries(nw_ingame_encode_test PRIVATE opennova_net)
target_include_directories(nw_ingame_encode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_encode COMMAND nw_ingame_encode_test)

# The session-variable list (npwire/session_vars.h): the 0x60 server-info
# stream and the client parse [orig: Game_SerializeMissionInfoToDataStream
# @0x523620; Client_ParseServerSessionVariables @0x5202f0].
add_executable(session_vars_test novaworld/session_vars_test.cpp)
target_link_libraries(session_vars_test PRIVATE opennova_net)
target_include_directories(session_vars_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME session_vars COMMAND session_vars_test)

# NET-0 tier-1 byte-stability gate (docs/maturity-program.md): FNV-1a64 vectors
# over a deterministic synthetic corpus through every in-match encoder. Catches
# SYMMETRIC codec changes the encode<->decode round-trip cannot see; vectors
# update only with a wire witness. Always-run, no capture dep.
add_executable(nw_codec_identity_test novaworld/nw_codec_identity_test.cpp)
target_link_libraries(nw_codec_identity_test PRIVATE opennova_net)
target_include_directories(nw_codec_identity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_codec_identity COMMAND nw_codec_identity_test)

# NET-0 tier-1 self-capture gate — the opennova-vs-opennova coverage diff: a deterministic
# in-process opennova host + opennova joiner session (the host_session owner loop
# + ClientRuntime over an in-memory IDatagramSocket) coverage-diffed per (dir,tag)
# against the COMMITTED opennova-produced fixture (LFS, zero retail bytes) with
# EXACT set equality — no noise floor, both sides are ours. Cannot skip; a missing
# fixture fails. Regen: `nw_self_capture_test --write-fixture` (justify the delta in-commit).
add_executable(nw_self_capture_test novaworld/nw_self_capture_test.cpp)
target_link_libraries(nw_self_capture_test
    PRIVATE opennova_net opennova_runtime opennova_base)
target_include_directories(nw_self_capture_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_self_capture_test PRIVATE
    SELF_CAPTURE_FIXTURE="${CMAKE_SOURCE_DIR}/fixtures/novaworld/self-capture-session.pcap")
add_test(NAME nw_self_capture COMMAND nw_self_capture_test)

# In-game message catalog coverage gate — asserts every Decoded tag's decoder
# consumes a representative body to the byte and the catalog stays consistent
# (the net analog of ADR 0002's MNU key-coverage). Self-contained, always-run.
add_executable(nw_message_coverage_test novaworld/nw_message_coverage_test.cpp)
target_link_libraries(nw_message_coverage_test PRIVATE opennova_net)
target_include_directories(nw_message_coverage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_message_coverage COMMAND nw_message_coverage_test)

# §5.15 guided weapon record — per-(mode, field-group) encode<->decode round-trip
# pinning the structural port of Entity_SerializeGuidedMissileState. Self-contained.
add_executable(nw_ingame_guided_test novaworld/nw_ingame_guided_test.cpp)
target_link_libraries(nw_ingame_guided_test PRIVATE opennova_net)
target_include_directories(nw_ingame_guided_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_guided COMMAND nw_ingame_guided_test)

# Backend schema regression — applies the actual backend/migrations/* and
# backend/seed/* the standalone server uses, so SQL syntax errors and seed
# regressions break CI before they break the running server.
# Retail-capture replay — pins the ProtocolMessage codec and the RE-doc
# §5.4/§5.5 BMS-bundle facts against the 2026-04-26 live JO captures.
add_executable(nwmsg_replay_test novaworld/nwmsg_replay_test.cpp)
target_link_libraries(nwmsg_replay_test PRIVATE opennova_net)
target_include_directories(nwmsg_replay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nwmsg_replay_test PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld")
add_test(NAME nwmsg_replay COMMAND nwmsg_replay_test)
# R1 of the net parity proof ladder: every body in the retail captures decoded
# to the byte and, where a struct encoder exists, re-encoded byte-identical
# (net/npwire/ingame_body_codec.h). The _retail leg proves the authored
# record-class table against the real items.def.
add_executable(nwmsg_codec_parity_test novaworld/nwmsg_codec_parity_test.cpp)
target_link_libraries(nwmsg_codec_parity_test PRIVATE opennova_runtime opennova_formats opennova_net)
target_include_directories(nwmsg_codec_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nwmsg_codec_parity_test PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld")
opennova_add_mixed_test(nwmsg_codec_parity COMMAND nwmsg_codec_parity_test)

# opennova-wire --scenario-events: the decoded event lines the network-parity scenario
# comparator (scripts/net/compare_scenario.py) consumes.
add_executable(opennova_wire_scenario_events_test
    novaworld/wire_scenario_events_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/wire/scenario_events.cpp)
target_link_libraries(opennova_wire_scenario_events_test PRIVATE opennova_net opennova_base)
target_include_directories(opennova_wire_scenario_events_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/wire)
add_test(NAME opennova_wire_scenario_events COMMAND opennova_wire_scenario_events_test)

# Host-row staleness backstop sweep (prune_stale_hosts).
add_executable(host_prune_test novaworld/host_prune_test.cpp)
target_link_libraries(host_prune_test PRIVATE opennova_novaworld_service)
target_include_directories(host_prune_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(host_prune_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME host_prune COMMAND host_prune_test)

# End-to-end host register -> read-back (the path /api/lobbies + /jop_2.gsb use).
add_executable(host_register_list_test novaworld/host_register_list_test.cpp)
target_link_libraries(host_register_list_test PRIVATE opennova_novaworld_service opennova_net)
target_include_directories(host_register_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(host_register_list_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME host_register_list COMMAND host_register_list_test)

add_executable(backend_schema_test novaworld/backend_schema_test.cpp)
target_link_libraries(backend_schema_test PRIVATE opennova_novaworld_service)
target_include_directories(backend_schema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(backend_schema_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME backend_schema COMMAND backend_schema_test)

# The service's HTTP route harness (hosted servers, PR A): the Crow listener from
# opennova_novaworld_server_core on a loopback port over a pooled SQLite file under
# the backend migrations, driven with apps/common's http_exchange. Only with the
# HTTP layer (BUILD_NOVAWORLD_HTTP); the service edge is direct for the
# sqlite-containment lint.
if(BUILD_NOVAWORLD_HTTP)
    add_executable(opennova_novaworld_server_http_routes_test novaworld/http_routes_test.cpp)
    target_link_libraries(opennova_novaworld_server_http_routes_test PRIVATE
        opennova_novaworld_server_core opennova_novaworld_service)
    target_include_directories(opennova_novaworld_server_http_routes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(opennova_novaworld_server_http_routes_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME opennova_novaworld_server_http_routes COMMAND opennova_novaworld_server_http_routes_test)
    # The concurrent case gives each request up to 30 s; this bounds a hang
    # below ctest's 1500 s default.
    set_tests_properties(opennova_novaworld_server_http_routes PROPERTIES TIMEOUT 120)
endif()

# The server binary's signal shutdown: SIGINT and SIGTERM each run main()'s
# orderly stop (exit 0, listeners in order, the WAL closed) with the HTTP layer
# on or off; with it on, past Crow and a held-open request. A last run checks
# that on every port 0 the gate (and the NW UDP SessionInit) advertise the
# ports the sibling listeners bound. POSIX only (fork, kill); TIMEOUT bounds it
# should the child hang, and the child dies with it.
if(UNIX)
    add_executable(opennova_novaworld_server_signal_shutdown_test
        novaworld/server_signal_shutdown_test.cpp)
    target_link_libraries(opennova_novaworld_server_signal_shutdown_test PRIVATE opennova_app_common)
    target_include_directories(opennova_novaworld_server_signal_shutdown_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(opennova_novaworld_server_signal_shutdown_test PRIVATE
        OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        $<$<BOOL:${BUILD_NOVAWORLD_HTTP}>:OPENNOVA_HTTP_ENABLED>)
    add_dependencies(opennova_novaworld_server_signal_shutdown_test opennova_novaworld_server)
    add_test(NAME opennova_novaworld_server_signal_shutdown
        COMMAND opennova_novaworld_server_signal_shutdown_test $<TARGET_FILE:opennova_novaworld_server>)
    set_tests_properties(opennova_novaworld_server_signal_shutdown PROPERTIES TIMEOUT 120)
endif()

# Unknown-message tracker: record/dedup, sample cap, flush/upsert against the
# real 0004 migration (OPENNOVA_SOURCE_DIR for the SQL on disk), snapshot.
add_executable(unknown_tracker_test novaworld/unknown_tracker_test.cpp)
target_link_libraries(unknown_tracker_test PRIVATE opennova_novaworld_service)
target_include_directories(unknown_tracker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(unknown_tracker_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME unknown_tracker COMMAND unknown_tracker_test)

# netsim tests — the in-match net seam (ADR 0009/0011).
# The in-process loopback transport.
add_executable(netsim_loopback_channel_test netsim/loopback_channel_test.cpp)
target_link_libraries(netsim_loopback_channel_test PRIVATE opennova_runtime)
add_test(NAME netsim_loopback_channel COMMAND netsim_loopback_channel_test)

add_executable(netsim_item_replication_catalog_test
    netsim/item_replication_catalog_test.cpp)
target_link_libraries(netsim_item_replication_catalog_test PRIVATE opennova_runtime opennova_formats)
add_test(NAME netsim_item_replication_catalog
    COMMAND netsim_item_replication_catalog_test)

# The ADR 0011 §4 loopback identity guard: World entity -> encode -> loopback ->
# decode -> ClientState, field-identical to what the host's encoder + codec produced.
add_executable(netsim_loopback_identity_test netsim/loopback_identity_test.cpp)
target_link_libraries(netsim_loopback_identity_test PRIVATE opennova_runtime)
add_test(NAME netsim_loopback_identity COMMAND netsim_loopback_identity_test)

# A header-only retail join has no local BMS body to promote. The decoded
# 0x10/0x0D/0x20 stream must create exact pool/slot world rows for native
# collision, deploy, and vehicle-seat consumers.
add_executable(netsim_client_world_materializer_test
    netsim/client_world_materializer_test.cpp)
target_link_libraries(netsim_client_world_materializer_test PRIVATE opennova_runtime)
add_test(NAME netsim_client_world_materializer
    COMMAND netsim_client_world_materializer_test)

# The D-NET-196 between-update mover contract: the joiner-side per-class chase
# (net-re §5.38e) keeps remote entities gliding across the host's subrate 0x0A
# cadence — smoothness legs, the witnessed snap/deadband/respawn edges, exact
# bucket-ladder pins, and the honest chase-only fast-vehicle sustain limit
# (the family prediction legs live world-side; watercraft_client_motor_test).
add_executable(netsim_remote_motion_smoothness_test netsim/remote_motion_smoothness_test.cpp)
target_link_libraries(netsim_remote_motion_smoothness_test PRIVATE opennova_runtime)
add_test(NAME netsim_remote_motion_smoothness COMMAND netsim_remote_motion_smoothness_test)

# The co-op-LAN fan-out guard: one World snapshot fanned to a connection table (loopback
# self + a socket-free UdpSessionTransport joiner), per-connection anchoring, and the
# host-side C2S 0x0C drain/SNAP — no Godot, no OS sockets (Increment A).
add_executable(netsim_two_peer_fanout_test netsim/two_peer_fanout_test.cpp)
target_link_libraries(netsim_two_peer_fanout_test PRIVATE opennova_runtime)
add_test(NAME netsim_two_peer_fanout COMMAND netsim_two_peer_fanout_test)

add_executable(netsim_build_player_uplink_test netsim/build_player_uplink_test.cpp)
target_link_libraries(netsim_build_player_uplink_test PRIVATE opennova_runtime)
add_test(NAME netsim_build_player_uplink COMMAND netsim_build_player_uplink_test)

# The §5.10b decode-dispatch guard: the replica pipeline's items.def class resolver sizes
# 0x0A tag-1 records (header-only no-callback vs vehicle/infantry compacts) and
# outranks the 0x0D pool-blanket learner — a mis-sized record desyncs the frame and
# scatters every later record around the recipient anchor.
add_executable(netsim_client_replica_pipeline_arms_dip_test
    netsim/client_replica_pipeline_arms_dip_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_arms_dip_test PRIVATE opennova_runtime)
add_test(NAME netsim_client_replica_pipeline_arms_dip
    COMMAND netsim_client_replica_pipeline_arms_dip_test)

add_executable(netsim_client_replica_pipeline_world_page_atomicity_test
    netsim/client_replica_pipeline_world_page_atomicity_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_world_page_atomicity_test PRIVATE opennova_runtime)
add_test(NAME netsim_client_replica_pipeline_world_page_atomicity
    COMMAND netsim_client_replica_pipeline_world_page_atomicity_test)

# The engine's PF_* present-row collectors (runtime/inmatch/present_rows.h,
# ADR 0043 G3): the host pool walk and the joiner's decoded-replica walk over a
# bare kernel.
add_executable(netsim_present_rows_test netsim/present_rows_test.cpp)
target_link_libraries(netsim_present_rows_test PRIVATE opennova_runtime)
add_test(NAME netsim_present_rows COMMAND netsim_present_rows_test)

add_executable(netsim_client_replica_pipeline_recoil_test
    netsim/client_replica_pipeline_recoil_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_recoil_test PRIVATE opennova_runtime)
add_test(NAME netsim_client_replica_pipeline_recoil
    COMMAND netsim_client_replica_pipeline_recoil_test)

add_executable(netsim_client_replica_pipeline_occupant_backref_test
    netsim/client_replica_pipeline_occupant_backref_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_occupant_backref_test PRIVATE opennova_runtime)
add_test(NAME netsim_client_replica_pipeline_occupant_backref
    COMMAND netsim_client_replica_pipeline_occupant_backref_test)

# The §5.13 vehicle compact's carrier form (groundEntity handle + carrier-local
# position/heading, composed back on read) and its off-11 vertical-velocity word
# [orig: Entity_SerializeVehicleState @0x460560], host fan and joiner pipeline.
add_executable(netsim_vehicle_compact_carrier_test
    netsim/vehicle_compact_carrier_test.cpp)
target_link_libraries(netsim_vehicle_compact_carrier_test PRIVATE opennova_runtime)
add_test(NAME netsim_vehicle_compact_carrier
    COMMAND netsim_vehicle_compact_carrier_test)

# The joiner's carried-vehicle prediction freeze through the production
# JoinerRole frame: a pool-1 deck rider stays frozen (seat-follow), a vehicle
# whose §5.13 carrier is a pool-2 static predicts from the composed sample.
add_executable(netsim_vehicle_carrier_prediction_test
    netsim/vehicle_carrier_prediction_test.cpp)
target_link_libraries(netsim_vehicle_carrier_prediction_test PRIVATE opennova_net opennova_runtime)
add_test(NAME netsim_vehicle_carrier_prediction
    COMMAND netsim_vehicle_carrier_prediction_test)

# The joiner's replica vehicle rows through the production JoinerRole frame:
# the §5.13 record tail (Flags bits, health word, kill edge), the mover freeze
# and the remote +0x170 claimant.
add_executable(netsim_joiner_vehicle_replica_test
    netsim/joiner_vehicle_replica_test.cpp)
target_link_libraries(netsim_joiner_vehicle_replica_test PRIVATE opennova_net opennova_runtime)
add_test(NAME netsim_joiner_vehicle_replica
    COMMAND netsim_joiner_vehicle_replica_test)

add_executable(netsim_client_replica_pipeline_target_carrier_follow_test
    netsim/client_replica_pipeline_target_carrier_follow_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_target_carrier_follow_test
    PRIVATE opennova_runtime)
add_executable(netsim_client_replica_pipeline_contact_resolver_test
    netsim/client_replica_pipeline_contact_resolver_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_contact_resolver_test PRIVATE opennova_runtime)
add_test(NAME netsim_client_replica_pipeline_contact_resolver
    COMMAND netsim_client_replica_pipeline_contact_resolver_test)
add_test(NAME netsim_client_replica_pipeline_target_carrier_follow
    COMMAND netsim_client_replica_pipeline_target_carrier_follow_test)
add_executable(netsim_client_replica_pipeline_compact_self_heal_test
    netsim/client_replica_pipeline_compact_self_heal_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_compact_self_heal_test PRIVATE opennova_runtime)
add_test(NAME netsim_client_replica_pipeline_compact_self_heal
    COMMAND netsim_client_replica_pipeline_compact_self_heal_test)
add_executable(netsim_net_quality_test netsim/net_quality_test.cpp)
target_link_libraries(netsim_net_quality_test PRIVATE opennova_runtime)
add_test(NAME netsim_net_quality COMMAND netsim_net_quality_test)

add_executable(netsim_client_replica_pipeline_class_resolver_test
    netsim/client_replica_pipeline_class_resolver_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_class_resolver_test PRIVATE opennova_runtime)
add_test(NAME netsim_client_replica_pipeline_class_resolver
    COMMAND netsim_client_replica_pipeline_class_resolver_test)

add_executable(netsim_client_replica_pipeline_anim_pulse_test
    netsim/client_replica_pipeline_anim_pulse_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_anim_pulse_test PRIVATE opennova_runtime)
add_test(NAME netsim_client_replica_pipeline_anim_pulse
    COMMAND netsim_client_replica_pipeline_anim_pulse_test)

# D-NET-209: the per-record receive-side body-state arbitration (@0x4c1153 /
# @0x4c0600) + the channel's deferred promotion and gait->stance transition
# insert (@0x40b5f0) — the tapped-roll lock+queue, same-state pending survival,
# the dead park / respawn edges, and the clip-end promotion cadence.
add_executable(netsim_client_replica_pipeline_body_arbitration_test
    netsim/client_replica_pipeline_body_arbitration_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_body_arbitration_test
    PRIVATE opennova_net opennova_runtime)
add_test(NAME netsim_client_replica_pipeline_body_arbitration
    COMMAND netsim_client_replica_pipeline_body_arbitration_test)

add_executable(netsim_world_stream_extractors_test netsim/world_stream_extractors_test.cpp)
target_link_libraries(netsim_world_stream_extractors_test PRIVATE opennova_runtime)
add_test(NAME netsim_world_stream_extractors COMMAND netsim_world_stream_extractors_test)

# npruntime tests — the IDA-faithful in-match NAPI NP runtime (the named server/client
# state machines + frame loop). Server-first (the plan's P0..P8).
# P0/P1: the in-process listen-server bring-up state writes (§5.0).
add_executable(npruntime_server_session_test npruntime/server_session_test.cpp)
target_link_libraries(npruntime_server_session_test PRIVATE opennova_runtime)
add_test(NAME npruntime_server_session COMMAND npruntime_server_session_test)

add_executable(npruntime_host_settings_test npruntime/host_settings_test.cpp)
target_link_libraries(npruntime_host_settings_test PRIVATE opennova_runtime)
target_include_directories(npruntime_host_settings_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_host_settings COMMAND npruntime_host_settings_test)

# P3: the World-driven host-side spawn pipeline (Server_InitNewRoundState ->
# ProcessPendingPlayerSpawns -> Server_BuildPlayerInfoAndAdd) — spawns the pool-0 player in World,
# stamping entity+0x78 ownerConnectionId; then the §1 dcb wiring surfaces it in the 0x0C batch.
add_executable(npruntime_server_spawn_test npruntime/server_spawn_test.cpp)
target_link_libraries(npruntime_server_spawn_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_server_spawn COMMAND npruntime_server_spawn_test)

# P3: the §5.2a two-track initial-state burst machine (Server_SendInitialGameStateToPlayer) over a
# real World + bms::File — emitted tag order + per-tag body round-trip + the entity+0x78 dcb wiring.
add_executable(npruntime_initial_state_burst_test npruntime/initial_state_burst_test.cpp)
target_link_libraries(npruntime_initial_state_burst_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_initial_state_burst COMMAND npruntime_initial_state_burst_test)

# S10a (ADR 0028): the joiner world<->net bridge — the witnessed client-frame hook order
# [orig: Game_ProcessMainFrame @0x5263f0; Client_ProcessNetworkFrame @0x42c180], the ClientHello
# one-shot, the in-match spawn edge for L + the weapon-pump L-gate, and the per-session resets.
add_executable(inmatch_joiner_role_test npruntime/joiner_role_test.cpp)
target_link_libraries(inmatch_joiner_role_test PRIVATE opennova_net opennova_runtime)
add_test(NAME inmatch_joiner_role COMMAND inmatch_joiner_role_test)

# The joiner session-drive policy (S10b): the two 0xEA60 reachable-analog windows, the S2C 0x7B
# promote validation, the admission/deploy/loss edge machine (frame ORDERING locked: loss > abort >
# settle > window > edges), and the D-NET-178 expansion reconcile decision.
add_executable(npruntime_join_session_policy_test npruntime/join_session_policy_test.cpp)
target_link_libraries(npruntime_join_session_policy_test PRIVATE opennova_runtime)
add_test(NAME npruntime_join_session_policy COMMAND npruntime_join_session_policy_test)

# ADR 0013: the shared byte-budget spawn/world-stream chunker (inmatch::slice_batch_pages) the §5.2a burst
# pages every world pool through — retail pool margins, exact tile pages, oversized record, cursor resume.
add_executable(npruntime_batch_chunker_test npruntime/batch_chunker_test.cpp)
target_link_libraries(npruntime_batch_chunker_test PRIVATE opennova_runtime)
add_test(NAME npruntime_batch_chunker COMMAND npruntime_batch_chunker_test)

# D-NET-141: the weapon.def armory table — witnessed index allocation (null@0 + file order),
# ammo-clip resolution, and charfilter/teamfilter masks, against the committed fixture; §5.57.
add_executable(npruntime_weapon_table_test npruntime/weapon_table_test.cpp)
target_link_libraries(npruntime_weapon_table_test
    PRIVATE opennova_net opennova_runtime opennova_formats opennova_base)
target_include_directories(npruntime_weapon_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(npruntime_weapon_table COMMAND npruntime_weapon_table_test)

# D-NET-168 / D-NET-176 / D-NET-177: the S2C 0x50 team-assign re-latch + 0x2F re-submit, the
# C2S 0x32 -> S2C 0x5D empty-slot sweep (entity removal) vs the bookkeeping-only 0x46 bit15, and
# the in-match session-loss surface [orig: @0x431910 / @0x51a600 + @0x429730 / @0x4ca4a0 -> @0x4c63d0].
add_executable(npruntime_entity_lifecycle_net_test npruntime/entity_lifecycle_net_test.cpp)
target_link_libraries(npruntime_entity_lifecycle_net_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_entity_lifecycle_net COMMAND npruntime_entity_lifecycle_net_test)

add_executable(npruntime_item_state_net_test npruntime/item_state_net_test.cpp)
target_link_libraries(npruntime_item_state_net_test PRIVATE opennova_runtime)
add_test(NAME npruntime_item_state_net COMMAND npruntime_item_state_net_test)

# D-DOOR-1: the host's S2C 0x37 door rows (the command's 0-based sections and
# the 1-based completions) through the server tick and its fan.
add_executable(npruntime_door_relay_test npruntime/door_relay_test.cpp)
target_link_libraries(npruntime_door_relay_test PRIVATE opennova_runtime)
add_test(NAME npruntime_door_relay COMMAND npruntime_door_relay_test)

# D-NET-284: the host's join-window kill page (C2S 0x28 -> S2C 0x4E).
add_executable(npruntime_kill_page_test npruntime/kill_page_test.cpp)
target_link_libraries(npruntime_kill_page_test PRIVATE opennova_runtime)
target_include_directories(npruntime_kill_page_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_kill_page COMMAND npruntime_kill_page_test)

# D-NET-281: the death screen's team change (C2S 0x4D), host side.
add_executable(npruntime_team_change_request_test npruntime/team_change_request_test.cpp)
target_link_libraries(npruntime_team_change_request_test PRIVATE opennova_runtime)
add_test(NAME npruntime_team_change_request COMMAND npruntime_team_change_request_test)

# D-NET-127: the host's S2C 0x46 per-field values (the 0x1000 spectator byte).
add_executable(npruntime_player_sync_fields_test npruntime/player_sync_fields_test.cpp)
target_link_libraries(npruntime_player_sync_fields_test PRIVATE opennova_runtime)
add_test(NAME npruntime_player_sync_fields COMMAND npruntime_player_sync_fields_test)

# D-NET-142: the C2S 0x25 reload-request relay — dispatch broadcasts S2C 0x49 onto every in-match
# connection's transport (requester included) [orig: @0x514DF0 -> SendFiltered @0x4C87E0]; §5.58.
add_executable(npruntime_reload_relay_test npruntime/reload_relay_test.cpp)
target_link_libraries(npruntime_reload_relay_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_reload_relay COMMAND npruntime_reload_relay_test)

# D-THROW-7: placed-device conversion/removal broadcasts exact typed S2C
# 0x59/0x12 records to every in-match remote and skips the host loopback.
add_executable(npruntime_placed_device_relay_test npruntime/placed_device_relay_test.cpp)
target_link_libraries(npruntime_placed_device_relay_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_placed_device_relay COMMAND npruntime_placed_device_relay_test)

# D-NET-152: the C2S 0x06 client-fired-round pipeline — anti-spoof, clip authority (+ the 0x25
# refill), and the g_RoundRing append behind the §5.9.1 tag-2 echo
# [orig: @0x513310 -> Server_ClientFiredRound @0x50baa0 -> RoundData_AddRound @0x4fdb40].
add_executable(npruntime_client_fire_test npruntime/client_fire_test.cpp)
target_link_libraries(npruntime_client_fire_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_client_fire COMMAND npruntime_client_fire_test)

# §5.62: the authoritative round sim — fire -> synchronous spawn [orig: RoundData_AddRound
# @0x4fdb40 inline RoundData_SpawnRound @0x4ec0d0], flight + hit [orig:
# Weapon_UpdateAllProjectiles @0x4ec020 -> Projectile_UpdatePhysics @0x4e9d70], the kinetic
# damage number [orig: Weapon_CalcImpactDamage @0x4ec920], and the death routing + respawn
# release [orig: Entity_CheckAndProcessDeath @0x51b550 -> S2C 0x13/0x1E].
add_executable(npruntime_round_sim_test npruntime/round_sim_test.cpp)
target_link_libraries(npruntime_round_sim_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_round_sim COMMAND npruntime_round_sim_test)

add_executable(npruntime_medic_recipient_test npruntime/medic_recipient_test.cpp)
target_link_libraries(npruntime_medic_recipient_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_medic_recipient COMMAND npruntime_medic_recipient_test)

# The headless AI path conformance report over the kernel-booted mission (the
# fast half of the host-parity loop; --report / --ticks / --bms for diagnosis).
add_executable(ai_path_conformance_test mission/ai_path_conformance_test.cpp)
target_link_libraries(ai_path_conformance_test PRIVATE opennova_test_rig)
opennova_add_gated_test(mission_ai_path_conformance COMMAND ai_path_conformance_test)

# The retail-mission ports of the former in-game probes (ADR 0041: an
# assertion over the portable engine is a ctest, not a probe), all on the
# shared rig (common/retail_mission_files.h). Gated: OPENNOVA_JO_DIR (a packed
# install) or OPENNOVA_JO_ASSETS (an extracted tree), per test header.
foreach(port
        world/truck_rest_00tra world/motorcycle_gravity_06tr
        world/rock_collision_00trg world/ai_corpse world/ai_threat
        world/ai_muzzle_pose
        world/ladder_00tra mission/defense_00trg mission/vehicle_ride_00tra mission/soak_00trg
        mission/native_assets_00trg mission/buggy_01tr mission/fire_hold_01tr
        mission/training_gameplay
        mission/watercraft_02tr mission/parachute_09tr)
    get_filename_component(port_name ${port} NAME)
    add_executable(${port_name}_test ${port}_test.cpp)
    target_link_libraries(${port_name}_test PRIVATE opennova_test_rig)
    opennova_add_gated_test(${port_name} COMMAND ${port_name}_test)
endforeach()

# The indoor-visibility walk (the "occlusion issues in a building" report):
# the local player walked through a retail building's doorway on the shared
# rig, the render-occlusion frame run over the game's camera every tick
# (00TRa's armory door), and CP03's authored spawn (a doorway of HN_Bld1)
# held still and swept. Gated: OPENNOVA_JO_DIR.
add_executable(indoor_visibility_walk_test world/indoor_visibility_walk_test.cpp)
target_link_libraries(indoor_visibility_walk_test PRIVATE opennova_test_rig)
opennova_add_gated_test(indoor_visibility_walk_00tra
    COMMAND indoor_visibility_walk_test --bms 00TRa.bms --bms-id 20 --door 5)
opennova_add_gated_test(indoor_visibility_spawn_cp03
    COMMAND indoor_visibility_walk_test --bms CP03.bms --spawn --idle 120 --yaw-step 45 --retail-truth)
# The blink rules those walks surfaced (synthetic rigs, no data gate): the
# first-pass-only blink letters, and the parent blink-quad copies (the dropped
# object, the ewep class callback and move function, a seated rider).
add_executable(indoor_visibility_fixes_test world/indoor_visibility_fixes_test.cpp)
target_link_libraries(indoor_visibility_fixes_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(indoor_visibility_fixes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME indoor_visibility_fixes COMMAND indoor_visibility_fixes_test)

# The witnessed SP lose flow on the retail data, one executable per training
# mission's WAC: 04TR carries both team legs, 00TRa only `true(bluekills) ->
# Lose(1)` (the first mission's friendly-fire failure, the playthrough gate 8
# of docs/world/npc-mission-completion.md). `--events` prints the mission's
# BMS event table instead (the playthrough's authored-sequence reference).
add_executable(lose_flow_test mission/lose_flow_test.cpp)
target_link_libraries(lose_flow_test PRIVATE opennova_test_rig)
opennova_add_gated_test(lose_flow_04tr COMMAND lose_flow_test --bms 04TR.bms)
opennova_add_gated_test(lose_flow_00tra COMMAND lose_flow_test --bms 00TRa.bms --victim-team 1)
# `--self-kill`: the player's own HE blast tallies nothing (no Player definition
# authors a `score`), so the WAC's bluekills lose never fires on it.
opennova_add_gated_test(lose_flow_04tr_self_kill COMMAND lose_flow_test --bms 04TR.bms --self-kill)
opennova_add_gated_test(lose_flow_05tr_self_kill COMMAND lose_flow_test --bms 05TR.bms --self-kill)
# `--win-event 38`: 01TR's only BlueWin, its five prerequisite events staged with
# their authored actions; the round ends won and the engine's win epilog builds
# its score screen over the mission's own tallies.
opennova_add_gated_test(win_flow_01tr COMMAND lose_flow_test --bms 01TR.bms --win-event 38)
# `--restart-boot`: 00TRa booted fresh and then as the SP restart's boot; the
# restarted world equals the fresh launch at the same world ages (the spawn,
# the fired-event set including event 11's authored activation delay).
opennova_add_gated_test(restart_boot_00tra COMMAND lose_flow_test --bms 00TRa.bms --restart-boot)

# The anim/def/particle/npruntime probes that became gated ctests (ADR 0041):
# each mounts the retail data through ResourceIndex itself and is Skipped
# without OPENNOVA_JO_ASSETS / OPENNOVA_JO_DIR.
foreach(gated
        anim/reload_clips_us01 anim/weapon_action_clips
        npruntime/remote_body_state npruntime/held_weapon_attach npruntime/authored_payload_00trg
        particle/gore_set_catalog)
    get_filename_component(gated_name ${gated} NAME)
    get_filename_component(gated_dir ${gated} DIRECTORY)
    add_executable(${gated_dir}_${gated_name}_test ${gated}_test.cpp)
    target_link_libraries(${gated_dir}_${gated_name}_test PRIVATE opennova_net opennova_runtime)
    target_include_directories(${gated_dir}_${gated_name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(${gated_dir}_${gated_name}_test PRIVATE cxx_std_17)
    opennova_add_gated_test(${gated_dir}_${gated_name} COMMAND ${gated_dir}_${gated_name}_test)
endforeach()

add_executable(npruntime_round_end_test npruntime/round_end_test.cpp)
target_link_libraries(npruntime_round_end_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_round_end COMMAND npruntime_round_end_test)

# P2: the per-connection handshake — the server legs over the np free functions + the reactive §5.1
# reply path (handshake_server), and the golden 0x81/0x82 byte-parity. The real JoinerConnection
# join->spawn->C2S->SNAP flow is covered on the World path by npruntime_client_runtime +
# npruntime_two_endpoint_socket.
add_executable(npruntime_handshake_server_test npruntime/handshake_server_test.cpp)
target_link_libraries(npruntime_handshake_server_test
    PRIVATE opennova_net opennova_runtime opennova_formats)
target_include_directories(npruntime_handshake_server_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(npruntime_handshake_server COMMAND npruntime_handshake_server_test)

add_executable(npruntime_host_producers_test npruntime/host_producers_test.cpp)
target_link_libraries(npruntime_host_producers_test PRIVATE opennova_net opennova_runtime)
target_include_directories(npruntime_host_producers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_host_producers COMMAND npruntime_host_producers_test)

# The status page's roster and host facts off the host context
# (inmatch/server_status_feed.h) and the total-logins count.
add_executable(npruntime_server_status_feed_test npruntime/server_status_feed_test.cpp)
target_link_libraries(npruntime_server_status_feed_test PRIVATE opennova_net opennova_runtime)
target_include_directories(npruntime_server_status_feed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_server_status_feed COMMAND npruntime_server_status_feed_test)
add_executable(npruntime_squad_relay_test npruntime/squad_relay_test.cpp)
target_link_libraries(npruntime_squad_relay_test PRIVATE opennova_net opennova_runtime)
target_include_directories(npruntime_squad_relay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_squad_relay COMMAND npruntime_squad_relay_test)

add_executable(npruntime_squad_client_test npruntime/squad_client_test.cpp)
target_link_libraries(npruntime_squad_client_test PRIVATE opennova_net opennova_runtime)
target_include_directories(npruntime_squad_client_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_squad_client COMMAND npruntime_squad_client_test)

# The `_NSTMOUT.TXT` connection-template override (CNapiNetwork_Init's NEVER / seconds /
# negative legs) and the joiner's 0x82 CS overlay of the host's timeout_ms / msg_out_max:
# the reap runs from the accepted 0x82, -1 disables it, and the pool bound follows the host.
add_executable(npruntime_handshake_timeout_override_test npruntime/handshake_timeout_override_test.cpp)
target_link_libraries(npruntime_handshake_timeout_override_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_handshake_timeout_override COMMAND npruntime_handshake_timeout_override_test)

# App-layer readiness gate used by the automated retail interop harness: send
# the shared retail 0x41 probe over a real loopback socket, accept only a valid
# game-server 0x81, and retain a bounded no-responder timeout.
add_executable(npruntime_lan_probe_client_test npruntime/lan_probe_client_test.cpp)
target_link_libraries(npruntime_lan_probe_client_test
    PRIVATE opennova_lan_probe_client opennova_net opennova_app_common)
add_test(NAME npruntime_lan_probe_client COMMAND npruntime_lan_probe_client_test)

# The remote-admin client codec (net/admin): framing, the challenge, the login the server's own
# decrypt recovers, and the read-only replies in the server's formats.
add_executable(admin_protocol_test admin/admin_protocol_test.cpp)
target_link_libraries(admin_protocol_test PRIVATE opennova_net)
target_include_directories(admin_protocol_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/admin)
add_test(NAME admin_protocol COMMAND admin_protocol_test)

# opennova-nw-lister (apps/nw_lister): the listing JSON + credentials readers; then the app's Lister end to
# end over real loopback UDP against the real NwUdpListener and an in-test gate (the row, a roster
# edit keeping the other slot, the refresh, the stop).
add_executable(novaworld_nw_lister_listing_test novaworld/nw_lister_listing_test.cpp)
target_link_libraries(novaworld_nw_lister_listing_test PRIVATE opennova_nw_lister_core)
add_test(NAME novaworld_nw_lister_listing COMMAND novaworld_nw_lister_listing_test)
add_executable(nw_lister_e2e_test
    novaworld/nw_lister_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nw_lister_e2e_test
    PRIVATE opennova_nw_lister_core opennova_novaworld_service opennova_runtime)
target_include_directories(nw_lister_e2e_test PRIVATE ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
add_test(NAME nw_lister_e2e COMMAND nw_lister_e2e_test)
set_tests_properties(nw_lister_e2e PROPERTIES RUN_SERIAL TRUE)
# The --admin poll thread against an in-test remote-admin server (the login it decrypts, the three
# commands, the QUIT).
add_executable(nw_lister_admin_feed_test novaworld/nw_lister_admin_feed_test.cpp)
target_link_libraries(nw_lister_admin_feed_test PRIVATE opennova_nw_lister_core)
target_include_directories(nw_lister_admin_feed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/admin)
add_test(NAME nw_lister_admin_feed COMMAND nw_lister_admin_feed_test)

# P5 — the headless CLIENT runtime (inmatch::ClientRuntime / Client_ProcessNetworkFrame). Always-on
# in-process round-trip (client_runtime <-> np server legs <-> Server_TickUpdate + apply_in_match_c2s)
# plus the host-as-client D-NET-121/122 anchor/fold path. No capture needed.
# Authenticated connection identity, including duplicate names and slot reuse.
add_executable(npruntime_joiner_identity_test npruntime/joiner_identity_test.cpp)
target_link_libraries(npruntime_joiner_identity_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_joiner_identity COMMAND npruntime_joiner_identity_test)

add_executable(npruntime_client_runtime_test npruntime/client_runtime_test.cpp)
target_link_libraries(npruntime_client_runtime_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_client_runtime COMMAND npruntime_client_runtime_test)

add_executable(npruntime_client_message_dispatch_test npruntime/client_message_dispatch_test.cpp)
target_link_libraries(npruntime_client_message_dispatch_test PRIVATE opennova_runtime)
add_test(NAME npruntime_client_message_dispatch COMMAND npruntime_client_message_dispatch_test)

# D-NET-194: a joiner retains the host's exact S2C 0x0B BMS header and
# reassembles the paged S2C 0x45 terrain-tile stream for wire-driven loading.
add_executable(npruntime_join_wire_assets_test npruntime/join_wire_assets_test.cpp)
target_link_libraries(npruntime_join_wire_assets_test PRIVATE opennova_runtime)
add_test(NAME npruntime_join_wire_assets COMMAND npruntime_join_wire_assets_test)

# Retail in-match loss recovery: future 0x43/0x83 packets trigger direction-specific 0x84/0x44
# missing-sequence lists, and each endpoint reconstructs retained reliable records under the old
# sequence with its current ACK. [orig: SendMissingSeqList @0x623560; HandleResendList @0x623800]
add_executable(npruntime_session_resend_test npruntime/session_resend_test.cpp)
target_link_libraries(npruntime_session_resend_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_session_resend COMMAND npruntime_session_resend_test)

# The NP session layer soaked over an impaired link: ClientRuntime joiners and the host owner
# loop on a virtual clock, each direction through the seeded ImpairedDatagramSocket decorator
# (2 % loss, reordering, duplication, 100 ms delay) under a 12-tick send holdoff; plus a 20 %-loss
# join over a 120-marker world, three joiners admitted at once, and three through the 20 % join
# over a 400-marker world. Asserts every session survives in order, recovers every gap and (at
# 2 %) never stalls past retail's recovery cadence.
add_executable(npruntime_session_soak_test npruntime/session_soak_test.cpp)
target_link_libraries(npruntime_session_soak_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_session_soak COMMAND npruntime_session_soak_test)

# The joiner's admission through retail's loading loops (D-NET-254): each stage's loop calls the
# send pump its own way (per UI frame, a busy spin, or a 50 / 100 ms wall-clock pace), and only
# Client_ProcessNetworkFrame gates it on the holdoff countdown. [orig: SaveFile_SendAndWaitForServerAck
# @0x5204b0; CNapiGameSession_InitRandomSeedOrRequest @0x51e8f0; NapiClient_WaitForDisconnect
# @0x42cb20; NapiClient_WaitForGameStart @0x42cc10]
add_executable(npruntime_client_loading_pump_test npruntime/client_loading_pump_test.cpp)
target_link_libraries(npruntime_client_loading_pump_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_client_loading_pump COMMAND npruntime_client_loading_pump_test)

# The host-initiated close (the connection-description record, settings flag + tag 3) pinned against
# the 80 captured bytes of a live retail deploy-screen idle punt, plus the S2C 0x30/0x31 anti-cheat
# CRC challenges a stock host streams at every joiner. [orig: Server_LogCRCMismatchPunt @0x517ed0 ->
# NapiNPDataTransfer_SendDescription @0x628c80; CNapiNPConnection_HandleDescriptionPacket @0x621ae0;
# NapiNPClientMsg_HandleChecksumRequest @0x431170; NapiNPClientMsg_0x031 @0x4311e0]
add_executable(npruntime_pre_game_menu_test npruntime/pre_game_menu_test.cpp)
target_link_libraries(npruntime_pre_game_menu_test PRIVATE opennova_runtime)
add_test(NAME npruntime_pre_game_menu COMMAND npruntime_pre_game_menu_test)

add_executable(npruntime_disconnect_reason_test npruntime/disconnect_reason_test.cpp)
target_link_libraries(npruntime_disconnect_reason_test PRIVATE opennova_runtime)
add_test(NAME npruntime_disconnect_reason COMMAND npruntime_disconnect_reason_test)

add_executable(npruntime_host_punt_test npruntime/host_punt_test.cpp)
target_link_libraries(npruntime_host_punt_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_host_punt COMMAND npruntime_host_punt_test)

# Server_TickUpdate's maintenance legs from the jo-c cross-check: the 620-tick spawn
# protection and its fire clear, the 1 Hz 0x2F flag refresh, the violation sweep's carry
# limit, the team-mode 0x46 downed resend, the armory-reuse cooldown, the 744-tick
# priority-target sweep, the kit-weight recompute, the spawn-wave eviction and the C2S 0x0E
# C&C / handle-0 admission. [orig: Server_UpdateAllActivePlayerSlots @0x518820; sub_517B20
# @0x517b20; Server_CheckPlayerViolations @0x51abd0; Server_TickUpdate @0x51e2d0;
# NapiNPServerMsg_HandlePlayerLoadout @0x515790; EntityPool_ClearDirtyFlags @0x508e30;
# Server_RecalculateAllPlayerScores @0x5014e0; SpawnWaveList_TryQueuePlayer @0x52a490;
# Server_ProcessClientRequestRespawn @0x519af0]
add_executable(npruntime_server_tick_maintenance_test npruntime/server_tick_maintenance_test.cpp)
target_link_libraries(npruntime_server_tick_maintenance_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_server_tick_maintenance COMMAND npruntime_server_tick_maintenance_test)

# Loss-tolerant pre-session UDP join: dropped 0x41/0x42 datagrams are retried on the retail active
# send interval without changing their wire bytes, and the retried legs complete against the real host.
add_executable(npruntime_joiner_handshake_retry_test npruntime/joiner_handshake_retry_test.cpp)
target_link_libraries(npruntime_joiner_handshake_retry_test PRIVATE opennova_net opennova_runtime)
target_include_directories(npruntime_joiner_handshake_retry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_joiner_handshake_retry COMMAND npruntime_joiner_handshake_retry_test)

# P6 — two endpoints over REAL UDP sockets: a ClientRuntime joiner and a dedicated host's
# owner-loop (npruntime/host_session.h, the SAME loop the game's host runs), each on its own bound UDP
# over the wire (single-thread poll-pump). Asserts mirror npruntime_client_runtime but over real
# sockets, and adds the §5.2a S2C tag-order check (always-on, our host). opennova_runtime for the
# minimal World/bms; opennova_app_common for net_sockets + net_datagram_socket (ws2_32 on WIN32).
add_executable(npruntime_two_endpoint_socket_test npruntime/two_endpoint_socket_test.cpp)
target_include_directories(npruntime_two_endpoint_socket_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(npruntime_two_endpoint_socket_test
    PRIVATE opennova_net opennova_runtime opennova_app_common opennova_base)
add_test(NAME npruntime_two_endpoint_socket COMMAND npruntime_two_endpoint_socket_test)

# weapon.sav player-profile parse/serialize tests (engine/formats/playersav).
# The retail-corpus leg reads the install's weapon.sav (OPENNOVA_JO_DIR) and notes SKIP-LEG without it.
add_executable(playersav_weapon_sav_test playersav/weapon_sav_test.cpp)
target_link_libraries(playersav_weapon_sav_test PRIVATE opennova_formats)
target_include_directories(playersav_weapon_sav_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(playersav_weapon_sav_test PRIVATE cxx_std_17)
opennova_add_mixed_test(playersav_weapon_sav COMMAND playersav_weapon_sav_test)

# The in-engine dev tools (ADR 0039, engine/runtime/devtools). The frame-stats
# board is unconditional; the ImGui pass and game-window test need
# OPENNOVA_DEVTOOLS (the root default). The ImGui
# tests drive a null backend.
add_executable(frame_stats_board_test devtools/frame_stats_board_test.cpp)
target_link_libraries(frame_stats_board_test PRIVATE opennova_runtime)
add_test(NAME frame_stats_board COMMAND frame_stats_board_test)
# The scenario driver's scripted input device: the shared script's schedule
# and the setup pose's retail-bridge conversion (every flavour).
add_executable(scripted_input_test devtools/scripted_input_test.cpp)
target_link_libraries(scripted_input_test PRIVATE opennova_runtime)
add_test(NAME scripted_input COMMAND scripted_input_test)
# The Stats window's row tree against the local role's real tick laps.
add_executable(tick_profile_rows_test devtools/tick_profile_rows_test.cpp)
target_link_libraries(tick_profile_rows_test PRIVATE opennova_test_rig)
target_include_directories(tick_profile_rows_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(tick_profile_rows_test PRIVATE cxx_std_17)
add_test(NAME tick_profile_rows COMMAND tick_profile_rows_test)
# The F3 Script window's engine report over the synthetic mission.
add_executable(script_debug_report_test mission/script_debug_report_test.cpp)
target_link_libraries(script_debug_report_test PRIVATE opennova_test_rig)
target_include_directories(script_debug_report_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(script_debug_report_test PRIVATE cxx_std_17)
add_test(NAME script_debug_report COMMAND script_debug_report_test)
# The F3 Net window's traffic counter over the datagram-socket seam.
add_executable(counting_datagram_socket_test npwire/counting_datagram_socket_test.cpp)
target_link_libraries(counting_datagram_socket_test PRIVATE opennova_net)
add_test(NAME counting_datagram_socket COMMAND counting_datagram_socket_test)
if(OPENNOVA_DEVTOOLS)
    add_executable(devtools_test devtools/devtools_test.cpp)
    target_link_libraries(devtools_test PRIVATE opennova_runtime opennova_imgui)
    add_test(NAME devtools COMMAND devtools_test)
    # The inspection windows added after the ADR 0039 core (Log, Script,
    # Render, Particles, Audio, Player, Net) and the Game-view overlays.
    add_executable(devtools_windows_test devtools/devtools_windows_test.cpp)
    target_link_libraries(devtools_windows_test PRIVATE opennova_runtime opennova_imgui)
    target_include_directories(devtools_windows_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME devtools_windows COMMAND devtools_windows_test)
    add_executable(devtools_overlay_test devtools/devtools_overlay_test.cpp)
    target_link_libraries(devtools_overlay_test PRIVATE opennova_runtime opennova_imgui)
    target_include_directories(devtools_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME devtools_overlay COMMAND devtools_overlay_test)
endif()

# Mission-authored lndm callbacks, independent of thrown AT mines.
add_executable(minefield_test world/minefield_test.cpp)
target_link_libraries(minefield_test PRIVATE opennova_runtime)
add_test(NAME minefield COMMAND minefield_test)

add_executable(minefield_retail_test world/minefield_retail_test.cpp)
target_link_libraries(minefield_retail_test PRIVATE opennova_test_rig)
target_include_directories(minefield_retail_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(minefield_retail COMMAND minefield_retail_test)

target_include_directories(minefield_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_executable(minefield_replica_test netsim/minefield_replica_test.cpp)
target_link_libraries(minefield_replica_test PRIVATE opennova_runtime)
add_test(NAME minefield_replica COMMAND minefield_replica_test)

add_executable(squib_test world/squib_test.cpp)
target_link_libraries(squib_test PRIVATE opennova_runtime)
add_test(NAME squib COMMAND squib_test)

# Shared immutable assets consumed by both simulation and Godot.
add_executable(asset_store_test assets/asset_store_test.cpp)
target_link_libraries(asset_store_test PRIVATE opennova_runtime)
target_include_directories(asset_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME asset_store COMMAND asset_store_test)

# Target cursors, vehicle instruments, Inset geometry, mortar preview, and peer feedback.
add_executable(hud_combat_test hud/hud_combat_test.cpp)
target_link_libraries(hud_combat_test PRIVATE opennova_runtime)
target_compile_features(hud_combat_test PRIVATE cxx_std_17)
add_test(NAME hud_combat COMMAND hud_combat_test)

# The strict JSON reader/writer behind the editor's project files (ADR 0046 d6).
add_executable(json_test io/json_test.cpp)
target_link_libraries(json_test PRIVATE opennova_io)
target_compile_features(json_test PRIVATE cxx_std_17)
target_include_directories(json_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME json COMMAND json_test)

# The OpenNova Editor's core (ADR 0046): the project file, the asset registry, the
# requirements checklist, and the opennova-project commands driven end to end.
add_executable(editor_project_document_test editor/project_document_test.cpp)
target_link_libraries(editor_project_document_test PRIVATE opennova_editor)
target_include_directories(editor_project_document_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_project_document COMMAND editor_project_document_test)

# A project's expansion (ADR 0046 S16): the names the game takes for one, weighed against an install's;
# the files its name forms; the install as the game serves it; a session's expansion project.
add_executable(editor_expansion_test editor/expansion_test.cpp)
target_link_libraries(editor_expansion_test PRIVATE opennova_editor)
target_include_directories(editor_expansion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_expansion COMMAND editor_expansion_test)

add_executable(editor_asset_registry_test editor/asset_registry_test.cpp)
target_link_libraries(editor_asset_registry_test PRIVATE opennova_editor)
target_include_directories(editor_asset_registry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_asset_registry COMMAND editor_asset_registry_test)

# The rule the asset kinds' table makes checkable (S13 D5, ADR 0046 S13): every format the
# runtime's classifier types and every file the required-resources manifest names is a kind the
# editor knows, with a place in the build.
add_executable(editor_asset_kinds_test editor/asset_kinds_test.cpp)
target_link_libraries(editor_asset_kinds_test PRIVATE opennova_editor)
target_include_directories(editor_asset_kinds_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_asset_kinds COMMAND editor_asset_kinds_test)

add_executable(editor_requirements_test editor/requirements_test.cpp)
target_link_libraries(editor_requirements_test PRIVATE opennova_editor)
target_include_directories(editor_requirements_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_requirements COMMAND editor_requirements_test)

add_executable(editor_blank_factory_test editor/blank_factory_test.cpp)
target_link_libraries(editor_blank_factory_test PRIVATE opennova_editor)
target_include_directories(editor_blank_factory_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_blank_factory COMMAND editor_blank_factory_test)

# TrnGen.exe's terrain bake ported (engine/editor/terrain): a depth map to the .cpt the game reads,
# read back through the engine's reader; given the retired TrnGen corpus's directory as its
# argument, the byte parity with TrnGen's own .cpt files (a run by hand).
add_executable(editor_terrain_bake_test editor/terrain_bake_test.cpp)
target_link_libraries(editor_terrain_bake_test PRIVATE opennova_editor)
target_include_directories(editor_terrain_bake_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_terrain_bake COMMAND editor_terrain_bake_test)

# A terrain made from images (S20): the terrain set, the heightmap's depths, the new_terrain request
# over a project (the import, the scan's kinds, the build's packing), the outputs read back and loaded
# by the runtime's terrain field and frame compiler, the import rerun-stable, the blank environment
# and a mission made on the terrain; the surface map read, written and sampled by the runtime (its
# _retail entry reads a shipped char map as the importer does).
add_executable(editor_terrain_import_test editor/terrain_import_test.cpp)
target_link_libraries(editor_terrain_import_test PRIVATE opennova_editor)
target_include_directories(editor_terrain_import_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_terrain_import COMMAND editor_terrain_import_test)

add_executable(editor_create_missing_test editor/create_missing_test.cpp)
target_link_libraries(editor_create_missing_test PRIVATE opennova_editor)
target_include_directories(editor_create_missing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_create_missing COMMAND editor_create_missing_test)

add_executable(editor_project_build_test editor/project_build_test.cpp)
target_link_libraries(editor_project_build_test PRIVATE opennova_editor)
target_include_directories(editor_project_build_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_project_build COMMAND editor_project_build_test)

add_executable(editor_play_session_test editor/play_session_test.cpp)
target_link_libraries(editor_play_session_test PRIVATE opennova_editor)
target_include_directories(editor_play_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_play_session COMMAND editor_play_session_test)

# The neutral editing core at any depth (S9g): a fake document type's walk, placements,
# locators, structural edits and refusals, batches, gestures, the clipboard seam and the
# selection model.
add_executable(editor_document_core_test editor/document_core_test.cpp)
target_link_libraries(editor_document_core_test PRIVATE opennova_editor)
target_include_directories(editor_document_core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_document_core COMMAND editor_document_core_test)

add_executable(editor_session_test editor/editor_session_test.cpp)
target_link_libraries(editor_session_test PRIVATE opennova_editor)
target_include_directories(editor_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_session COMMAND editor_session_test)

# Per-file validation (S13 D4): each file's own findings kept in the validation cache by what
# they were made from, a closed file read and let go; what other files make of what one defines
# (graph/use_checks); the project's rows as the one pass makes them, the trunk's before the
# split pinned over the fixtures; every finding composed carries a finding code's row (S13 A6).
# Retail leg (OPENNOVA_JO_DIR): the install's files a document type opens validated, every
# finding composed a row's, then an edit of one validating it alone.
add_executable(editor_project_validation_test editor/project_validation_test.cpp)
target_link_libraries(editor_project_validation_test PRIVATE opennova_editor)
target_include_directories(editor_project_validation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_project_validation COMMAND editor_project_validation_test)

# Long operations everywhere (S13 A3): the walks stepped by bytes (the scan, the import pass, a
# scan's targeted update), the validation a file at a time and a rename's commit a file at a time,
# each equal to its run in one call; the session's Open, Rescan, import and rename stepped a poll
# at a time (progress only rising, finished once, cancelled leaving nothing, the view a run to the
# end makes), a Save scanning and validating its one file, a stale import refused without writing.
# Retail leg (OPENNOVA_JO_DIR): a project of the JO install's files opened a step at a time at the
# editor's budget, its polls and wall time, and the base layer a dependency mount would build.
add_executable(editor_long_operations_test editor/long_operations_test.cpp)
target_link_libraries(editor_long_operations_test PRIVATE opennova_editor)
target_include_directories(editor_long_operations_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_long_operations COMMAND editor_long_operations_test)

# The document types' project checks (S13 V9): a type's own check across the project's files,
# made by its registry row (the menu type's render check) and kept by whoever validates; a
# stand-in type's check runs once per validation, its findings after the build's gate, and its
# answer that they moved is what composes the Problems rows again.
add_executable(editor_project_checks_test editor/project_checks_test.cpp)
target_link_libraries(editor_project_checks_test PRIVATE opennova_editor)
target_include_directories(editor_project_checks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_project_checks COMMAND editor_project_checks_test)

# The session's operations (S13 A1): the slot stepping a fake operation within a poll's budget,
# cancelled between two steps, finished once; the busy gate's four answers over a real build
# (refused, joined, superseded as the table says, cancelled first); edits during a build.
add_executable(editor_session_operation_test editor/editor_session_operation_test.cpp)
target_link_libraries(editor_session_operation_test PRIVATE opennova_editor)
target_include_directories(editor_session_operation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_session_operation COMMAND editor_session_operation_test)

# The session's parts (S13 A2): the editor's preferences over their store (the memory store's
# round trip, the file store's settings file byte for byte, a session over each), the open
# documents' one position rule and remembered selections, and the unsaved-changes prompt driven
# by the request table's guard column for every request kind.
add_executable(editor_preferences_test editor/editor_preferences_test.cpp)
target_link_libraries(editor_preferences_test PRIVATE opennova_editor)
target_include_directories(editor_preferences_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_preferences COMMAND editor_preferences_test)

# The project, the import and Files as a modder meets them (ADR 0046, the UX round's project lane): a kind
# named in words, the folders each kind lands in, the files a filter lists, a file's card and its requests.
add_executable(editor_project_lane_test editor/project_lane_test.cpp)
target_link_libraries(editor_project_lane_test PRIVATE opennova_editor)
target_include_directories(editor_project_lane_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_project_lane COMMAND editor_project_lane_test)

# What the windows show of their own, held by the session (ADR 0046, the MCP gaps lane): set_workspace and
# the workspace section, each part's wire form, the refusals.
add_executable(editor_workspace_parts_test editor/workspace_parts_test.cpp)
target_link_libraries(editor_workspace_parts_test PRIVATE opennova_editor)
target_include_directories(editor_workspace_parts_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_workspace_parts COMMAND editor_workspace_parts_test)

# The navigation history (CONTEXT.md "Navigation history"): the history alone on a stepped clock, what is
# a step of it in a session, Back and Forward showing a place again, the wire's requests and section.
add_executable(editor_navigation_test editor/navigation_test.cpp)
target_link_libraries(editor_navigation_test PRIVATE opennova_editor)
target_include_directories(editor_navigation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_navigation COMMAND editor_navigation_test)

add_executable(editor_document_set_test editor/document_set_test.cpp)
target_link_libraries(editor_document_set_test PRIVATE opennova_editor)
target_include_directories(editor_document_set_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_document_set COMMAND editor_document_set_test)

# A document type of another kind than records (S13 D6), a test's standing in for the
# stylesheet's: the graph, the validation, the rename and the session over it.
add_executable(editor_non_record_document_test editor/non_record_document_test.cpp)
target_link_libraries(editor_non_record_document_test PRIVATE opennova_editor)
target_include_directories(editor_non_record_document_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_non_record_document COMMAND editor_non_record_document_test)

# The text documents and their types (S13 D9): the model's spans, history and change sets; the
# script's compiler findings and operand references with spans; the graph's span edges, the
# fallback name, Rename everywhere through a span, a Go to's RevealText; the wire's span edits and
# pages of lines; the credits, music script and shader codecs byte for byte; and the install's
# scripts, music scripts, credits and shaders (OPENNOVA_JO_DIR) as the retail leg.
add_executable(editor_text_document_test editor/text_document_test.cpp)
target_link_libraries(editor_text_document_test PRIVATE opennova_editor)
target_include_directories(editor_text_document_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_text_document COMMAND editor_text_document_test)

add_executable(editor_unsaved_guard_test editor/unsaved_guard_test.cpp)
target_link_libraries(editor_unsaved_guard_test PRIVATE opennova_editor)
target_include_directories(editor_unsaved_guard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_unsaved_guard COMMAND editor_unsaved_guard_test)

# The session's wire form (the editor MCP): request tokens both ways, the view, a
# document and a record as JSON over a real session.
add_executable(editor_session_json_test editor/session_json_test.cpp)
target_link_libraries(editor_session_json_test PRIVATE opennova_editor)
target_include_directories(editor_session_json_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_session_json COMMAND editor_session_json_test)

# The finding codes (S13 A6): every token its own across the editor's table and the document
# types' as registered (a stand-in hiding none), each row found by its token, a type's rows its
# group's family, the render check's a row per compiler note and the stylesheet reader's a row
# per reader code, the columns deciding what the codes' spelling decided before (and the
# spelling agreeing), a finding keeping its row, and its subject on the wire under the keys it had.
add_executable(editor_finding_codes_test editor/finding_codes_test.cpp)
target_link_libraries(editor_finding_codes_test PRIVATE opennova_editor)
target_include_directories(editor_finding_codes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_finding_codes COMMAND editor_finding_codes_test)

# The Problems core (S11b): the query the window and the editor MCP share (severities,
# text, scope, only the fixable, grouping), the fixes for each finding and a Fix all
# merged from them, where a finding takes Problems, and the findings by file and record
# (S13 V1: FindingsIndex).
add_executable(editor_problems_test editor/problems_test.cpp)
target_link_libraries(editor_problems_test PRIVATE opennova_editor)
target_include_directories(editor_problems_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_problems COMMAND editor_problems_test)

# The menu clipboard's one rule for the tree and the canvas (S13 V1, documents/mnu_clipboard):
# a window a part holds is copyable from neither, a Paste after the primary's window.
add_executable(editor_mnu_clipboard_test editor/mnu_clipboard_test.cpp)
target_link_libraries(editor_mnu_clipboard_test PRIVATE opennova_editor)
target_include_directories(editor_mnu_clipboard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_mnu_clipboard COMMAND editor_mnu_clipboard_test)

# The viewports' one canvas with no ImGui (S13 V2, V5: preview/canvas_gesture, menu_canvas,
# model_canvas): the one gesture machine and its single EndEdit; the menu's canvas over a
# headless render (a click, a drag of a window and of a handle on the grid, the arrows, Esc,
# several windows, the marquee's one selection, the clipboard's rule, Arrange, a part's window,
# what it draws); the model's canvas over a real session's model viewport (F, a click on a marker,
# a marker's drag ended once, the orbit and the dolly as SetViewports of its camera).
add_executable(editor_canvas_test editor/canvas_test.cpp)
target_link_libraries(editor_canvas_test PRIVATE opennova_editor)
target_include_directories(editor_canvas_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_canvas COMMAND editor_canvas_test)

# The windows' models with no ImGui (S13 V1): what the Problems window lists, folds, proposes
# and asks (ui/problems_list), where the find bar is among a document's hits (ui/find_cursor),
# and what a document's outline lists (ui/outline_model, S13 V3). Built with the windows (their
# sources are ui/'s).
if(OPENNOVA_IMGUI)
    add_executable(editor_problems_list_test editor/problems_list_test.cpp)
    target_link_libraries(editor_problems_list_test PRIVATE opennova_editor)
    target_include_directories(editor_problems_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME editor_problems_list COMMAND editor_problems_list_test)

    add_executable(editor_find_cursor_test editor/find_cursor_test.cpp)
    target_link_libraries(editor_find_cursor_test PRIVATE opennova_editor)
    target_include_directories(editor_find_cursor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME editor_find_cursor COMMAND editor_find_cursor_test)

    # A document's outline (S13 V3, ui/outline_model): its lines as a list, a tree and master and
    # detail, what is open, the filter, the order, and the lines made again only when what they read
    # moves.
    add_executable(editor_outline_model_test editor/outline_model_test.cpp)
    target_link_libraries(editor_outline_model_test PRIVATE opennova_editor)
    target_include_directories(editor_outline_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME editor_outline_model COMMAND editor_outline_model_test)
endif()

# The asset graph (S7): the extractors, the resolver the badges and Problems share,
# the per-file slots an update patches (an incremental update equals a fresh build, S13 D3),
# the base layer, and the rename transaction; its retail legs over the JO install.
add_executable(editor_asset_graph_test editor/asset_graph_test.cpp)
target_link_libraries(editor_asset_graph_test PRIVATE opennova_editor)
target_include_directories(editor_asset_graph_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_asset_graph COMMAND editor_asset_graph_test)

# A document's reference queries (S13 D3, graph/reference_queries): the badge, the missing
# finding, the picker, where Go to leads, and find_definition over the graph's slot or the
# document alone.
add_executable(editor_reference_queries_test editor/reference_queries_test.cpp)
target_link_libraries(editor_reference_queries_test PRIVATE opennova_editor)
target_include_directories(editor_reference_queries_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_reference_queries COMMAND editor_reference_queries_test)

# Find (S12 D8): every document type's fields found as the Inspector shows them (a def's number
# in its written units, a choice by its name, any case unless it must match), and the project's
# files and symbols by name with their usages (editor/model/document_search, AssetGraph::search).
add_executable(editor_document_search_test editor/document_search_test.cpp)
target_link_libraries(editor_document_search_test PRIVATE opennova_editor)
target_include_directories(editor_document_search_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_document_search COMMAND editor_document_search_test)

# Rename everywhere (S12 D9): one test per kind of name (a menu screen used from another menu, a
# style variable brand.mns defines again, a string key two sections define, a weapon's and an
# ammo's name, an item id a mission names, a model's user point), each renamed with exactly the
# uses that reach it, or refused naming the file (graph/rename_transaction, the session's
# PreviewRename and RenameSymbol).
add_executable(editor_symbol_rename_test editor/symbol_rename_test.cpp)
target_link_libraries(editor_symbol_rename_test PRIVATE opennova_editor)
target_include_directories(editor_symbol_rename_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_symbol_rename COMMAND editor_symbol_rename_test)

# The document type contract (S12 Z, ADR 0046 S12 "Adding a document type"): every registered
# type over a file of its kinds loads, serializes stably, labels its fields, keeps field_on's
# read-only, sets a field to its own value with no byte changed, and defines each name as a symbol
# find resolves, every reference kind with its reference_kinds row.
# The texture viewer (ADR 0046 S18): a texture file as the game's readers read it, its facts in words
# (documents/texture_image), the texture document, and its viewport through a session (Files'
# selection previewed from its file, the viewport query and SetViewport, the texture opened, the item
# using it); the retail leg (OPENNOVA_JO_DIR) a sample of the install's textures of each extension.
add_executable(editor_texture_document_test editor/texture_document_test.cpp)
target_link_libraries(editor_texture_document_test PRIVATE opennova_editor)
target_include_directories(editor_texture_document_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_texture_document COMMAND editor_texture_document_test)

# ADR 0046 S18, the texture roles: the catalog (documents/texture_roles: every way the game uses a
# texture, its loader, formats, size rule and witness) and the loaders' name rules and transforms
# (documents/texture_load_rules), against vectors worked by hand; the texture_roles query; the graph
# resolving each texture reference by its loader, a terrain's colour map and blend map refusing a
# build; the retail leg (OPENNOVA_JO_DIR) the install's texture references by role, none refusing one.
add_executable(editor_texture_roles_test editor/texture_roles_test.cpp)
target_link_libraries(editor_texture_roles_test PRIVATE opennova_editor)
target_include_directories(editor_texture_roles_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_texture_roles COMMAND editor_texture_roles_test)

# ADR 0046 S18, the texture thumbnails (preview/texture_thumbnails): a texture as a small picture, the
# PNG writer, the session's cache made by its poll, the texture_thumbnail query and a texture field's
# record and choices naming the file its loader opens.
add_executable(editor_texture_thumbnails_test editor/texture_thumbnails_test.cpp)
target_link_libraries(editor_texture_thumbnails_test PRIVATE opennova_editor)
target_include_directories(editor_texture_thumbnails_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_texture_thumbnails COMMAND editor_texture_thumbnails_test)

# ADR 0046 S18, the texture uses (graph/texture_uses, session/texture_use_index): each texture's uses by
# role over a minted project, a model row by its material, a .tga beside the .dds its loader opens, a
# terrain's, a sky's, a HUD image's, a name the game opens itself; the texture_uses query; a use shown
# where the game draws it (session/texture_show_use: on its model, in its menu, in a mission's view, as used).
add_executable(editor_texture_uses_test editor/texture_uses_test.cpp)
target_link_libraries(editor_texture_uses_test PRIVATE opennova_editor)
target_include_directories(editor_texture_uses_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_texture_uses COMMAND editor_texture_uses_test)

# ADR 0046 S18, the texture checks: a texture file's own findings from its header and its uses' (the
# texture use check, graph/texture_checks) over a minted project, which refuse a build; the retail leg
# (OPENNOVA_JO_DIR) every shipped texture's own findings and every terrain's colour map, none refusing one.
add_executable(editor_texture_checks_test editor/texture_checks_test.cpp)
target_link_libraries(editor_texture_checks_test PRIVATE opennova_editor)
target_include_directories(editor_texture_checks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_texture_checks COMMAND editor_texture_checks_test)

# ADR 0046 S18, the texture budget (documents/texture_budget over renderer/device_texture): what a model
# row's texture costs the game at each object texture detail and as its .dds; over a minted project the
# texture_uses query's budgets, texture.memory and texture.normal_slot_loader with its fix, the
# texture_budget query (the project's textures and their totals); the retail leg (OPENNOVA_JO_DIR) every
# shipped model's texture rows costed, none past the warning.
add_executable(editor_texture_budget_test editor/texture_budget_test.cpp)
target_link_libraries(editor_texture_budget_test PRIVATE opennova_editor)
target_include_directories(editor_texture_budget_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_texture_budget COMMAND editor_texture_budget_test)

# ADR 0046 S18, a texture as the game draws it: the chain the game builds, the object texture detail's halvings,
# a normal map lit, a model row's alpha by its technique; through a session the texture viewport's level, detail,
# normals and light, and a Phong diffuse shown as its use draws it.
add_executable(editor_texture_game_view_test editor/texture_game_view_test.cpp)
target_link_libraries(editor_texture_game_view_test PRIVATE opennova_editor)
target_include_directories(editor_texture_game_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_texture_game_view COMMAND editor_texture_game_view_test)

# ADR 0046 S18, each texture role in its own picture (preview/texture_role_view): a blend map's weights and the
# splat details they weigh, a foliage map's codes and what grows on them, a particle graphic as its atlas page holds
# it; through a session the texture viewport showing each as its use, its legend read again as its terrain moves.
add_executable(editor_texture_role_view_test editor/texture_role_view_test.cpp)
target_link_libraries(editor_texture_role_view_test PRIVATE opennova_editor)
target_include_directories(editor_texture_role_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_texture_role_view COMMAND editor_texture_role_view_test)

# ADR 0046 S18, a texture's import (import/texture_import, graph/texture_import_needs,
# session/texture_import_state): the image importer's option rows, its pieces, every format written and
# read back by the reader the game picks, what a minted project's uses ask of an import, set_import_options
# and the import_options query.
add_executable(editor_texture_import_test editor/texture_import_test.cpp)
target_link_libraries(editor_texture_import_test PRIVATE opennova_editor)
target_include_directories(editor_texture_import_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_texture_import COMMAND editor_texture_import_test)

# ADR 0046 S18, the texture document's edits (documents/texture_operations, texture_operation): each
# whole-image operation one undo step written through the editor's writers, Undo, Redo and Save; an
# import's output and an operation it cannot do refused; the Problems fix of an upside-down TGA's rows.
add_executable(editor_texture_document_edit_test editor/texture_document_edit_test.cpp)
target_link_libraries(editor_texture_document_edit_test PRIVATE opennova_editor)
target_include_directories(editor_texture_document_edit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_texture_document_edit COMMAND editor_texture_document_edit_test)

# ADR 0046 S18, a texture made from an image (import/texture_source, replace_texture) and split
# (plan_split, split_texture): the options that reproduce a texture; a plain texture and an import's output
# replaced, a missing name made, the refusals; a texture split for one referrer; the sources to edit.
add_executable(editor_texture_replace_test editor/texture_replace_test.cpp)
target_link_libraries(editor_texture_replace_test PRIVATE opennova_editor)
target_include_directories(editor_texture_replace_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_texture_replace COMMAND editor_texture_replace_test)

# ADR 0046 S18, the TGA an import source is read by (editor/import/tga_source.h), the format's own
# decode: every origin, depth, colour map and run-length form decoded upright, and what it refuses.
add_executable(editor_tga_source_test editor/tga_source_test.cpp)
target_link_libraries(editor_tga_source_test PRIVATE opennova_editor)
target_include_directories(editor_tga_source_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_tga_source COMMAND editor_tga_source_test)

# ADR 0046 S18, the external program round trip (edit_externally, refresh_changed_sources): a plain TGA
# given a source once and opened, a change saved there imported and its document read again, nothing done
# for nothing changed, an output's own source, a missing name; the Shell's check leaving a refusal's line.
add_executable(editor_texture_external_test editor/texture_external_test.cpp)
target_link_libraries(editor_texture_external_test PRIVATE opennova_editor)
target_include_directories(editor_texture_external_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_texture_external COMMAND editor_texture_external_test)

# ADR 0046 DI-01, outside saves come back (editor/assets/disk_changes.h, editor/session/disk_watch.h): a
# file read once it holds still, the folders' made and gone files, the focus-in sweep; an open catalog's
# outside save read again with its selection kept, a conflict raised at once with Keep my edits.
add_executable(editor_disk_changes_test editor/disk_changes_test.cpp)
target_link_libraries(editor_disk_changes_test PRIVATE opennova_editor)
target_include_directories(editor_disk_changes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_disk_changes COMMAND editor_disk_changes_test)

# A file's rename through the native texts (ADR 0046 S18, graph/native_text_sites): a terrain's, an
# environment's, a particle file's and the HUD layout's names rewritten as text, only the token the game
# reads at each site; a flipbook's frame refused; a file in the SCR form written back as plain text. The
# retail leg (OPENNOVA_JO_DIR): every name the install's native texts write found as its one token.
add_executable(editor_native_rename_test editor/native_rename_test.cpp)
target_link_libraries(editor_native_rename_test PRIVATE opennova_editor)
target_include_directories(editor_native_rename_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_native_rename COMMAND editor_native_rename_test)

add_executable(editor_document_contract_test editor/editor_document_contract_test.cpp)
target_link_libraries(editor_document_contract_test PRIVATE opennova_editor)
target_include_directories(editor_document_contract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_document_contract COMMAND editor_document_contract_test)

# The project's files by logical name, the open documents standing in for theirs
# (editor/assets/project_asset_source.h).
add_executable(editor_project_asset_source_test editor/project_asset_source_test.cpp)
target_link_libraries(editor_project_asset_source_test PRIVATE opennova_editor)
target_include_directories(editor_project_asset_source_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_project_asset_source COMMAND editor_project_asset_source_test)

# The viewport seam (S13 V5, editor/preview/viewport_*, viewports, viewport_device_cache) over
# devices that draw nothing and a real session: the actions a device takes (Keep, Rebuild, Update,
# Clear) from the document's change, the files its picture read and the viewport's options; the
# failure latch; the preview clock; two menus' own options; the device cache's four, its least
# recently used given up and its viewport's camera kept; the envelope and its pages; a SetViewport,
# a drag and a command through the session; one undo step per gesture; S13 V6's builds over frames
# (loading then ready, a newer generation cancelling, an Update folded, a Clear and a failure
# mid-build, the frame's budget). S13 V8: the change sets a viewport follows (a later screen's edit
# and a variable only a later screen names Keep, a screen before the shown one a Rebuild, a user
# point's move an Update over the scene that stands, the menu read again everything), the model's
# scene held until the gesture's end, the menu's caret on the preview clock; its retail leg a
# shipped screen's edit box blinking its caret on the clock (OPENNOVA_JO_DIR).
add_executable(editor_viewport_test editor/viewport_test.cpp)
target_link_libraries(editor_viewport_test PRIVATE opennova_editor)
target_include_directories(editor_viewport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_viewport COMMAND editor_viewport_test)

# The script device's portable half (S13 V10, editor/preview/script_viewport, shown_text,
# text_burst): ViewportKind::Script in the kinds' table, the Main role of every text type; the span
# diff from a control's text, minimal and byte-exact against the TextDocument (an insert, a delete,
# a replace across lines, CR LF kept, a paste of several lines, a lone CR and a NUL kept, a code
# page's character stored, one it has none for refused); the keystroke burst; the gutter marks
# from the findings; the compiler's words highlighted; through a real session, a burst served as one
# undo step, the follow's actions, a Go to's span selected, a document held read only and the busy
# gate, the envelope and a SetViewport; and the install's scripts (OPENNOVA_JO_DIR) as the retail
# leg: their highlights tokens of their lines, their shown text's lines the documents'.
add_executable(editor_script_viewport_test editor/script_viewport_test.cpp)
target_link_libraries(editor_script_viewport_test PRIVATE opennova_editor)
target_include_directories(editor_script_viewport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_script_viewport COMMAND editor_script_viewport_test)

# Edits in a viewport over the wire (S13 V7: edit_in_viewport through a session's JSON requests): its
# refusals, a drag one batch over the selection, a command one request; the wire's gestures (a
# snapped gesture where a canvas's lands, refused and forged tokens, interleaving, what ends one,
# the 10 s lapse); the menu's rounding and its drag to a point; a model's drag by nothing, its frame
# beside any operation; the clock set with no document named; a model read at a canvas's size.
add_executable(editor_viewport_wire_test editor/viewport_wire_test.cpp)
target_link_libraries(editor_viewport_wire_test PRIVATE opennova_editor)
target_include_directories(editor_viewport_wire_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_viewport_wire COMMAND editor_viewport_wire_test)

# What a drag in a mission's viewport writes (editor/preview/mission_handle_edit, ADR 0046 S14) and the
# mission camera (editor/preview/mission_camera), over a fake record document that carries the
# mission's field tokens: a move on the plane and on a fake ground with and without stick, the snap,
# the offsets kept, a sample within a 16.16 step, the lift, the turn's wrap and snap, an area's
# edges; the camera's wire yaw and pitch against presentation_forward_from_angles, a look, a fly,
# the ground point, top, the wire form.
add_executable(editor_mission_handle_edit_test editor/mission_handle_edit_test.cpp)
target_link_libraries(editor_mission_handle_edit_test PRIVATE opennova_editor)
target_include_directories(editor_mission_handle_edit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_mission_handle_edit COMMAND editor_mission_handle_edit_test)

# The mission viewport's portable half that reads no document type (ADR 0046 S14): the scene over
# a source a test holds (editor/preview/mission_scene: read, the patch reading the changed rows
# alone, a reshape reading everything), its marks, picks and boxes, the options' wire form
# (mission_options) and the overlays (mission_overlay: the glyphs, the rings, the primary's handles
# with a yaw-90 entity's east, an area's box, a path's lines, the labels, the marquee, a line clipped
# at the near plane).
add_executable(editor_mission_scene_test editor/mission_scene_test.cpp)
target_link_libraries(editor_mission_scene_test PRIVATE opennova_editor)
target_include_directories(editor_mission_scene_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_mission_scene COMMAND editor_mission_scene_test)

# The mission viewport (editor/preview/mission_viewport, ADR 0046 S14) over a real session, a
# project holding the minted mission (fixtures/bms/synth_logic.bms) and fake devices: the kind's
# row, its status and follow, what each change set comes to (an entity's transform an Update, an
# event's field Keep with no row read, an Add and the header's terrain a Rebuild), a Rebuild held for
# a gesture, a file the device read moving, its camera on the wire, its hits and boxes, its envelope,
# a drop and the ground command; DI-07, the ground under a point in the game's words over a minted
# terrain with a char map (class, footstep slots, effects row, a placed tile, the water). Mixed: the
# retail row opens every shipped mission (OPENNOVA_JO_DIR) in its viewport, hits its marks and moves
# its first item and back.
add_executable(editor_mission_viewport_test editor/mission_viewport_test.cpp)
target_link_libraries(editor_mission_viewport_test PRIVATE opennova_editor)
target_include_directories(editor_mission_viewport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_mission_viewport COMMAND editor_mission_viewport_test)

# The menu's viewport (editor/preview/menu_viewport, S9j, S13 V5) over a headless device: its
# screen, its reason, when its device configures again, the envelope, its drag and arrange.
add_executable(editor_menu_viewport_test editor/menu_viewport_test.cpp)
target_link_libraries(editor_menu_viewport_test PRIVATE opennova_editor)
target_include_directories(editor_menu_viewport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_menu_viewport COMMAND editor_menu_viewport_test)

# The model's viewport (editor/preview/model_viewport, S10p2, S13 V5): what it shows, when it
# builds again (a user point's edit builds nothing), the camera's projection, the level Auto picks,
# the envelope, what a SetViewport sets, the drag of a marker, a clip on its rig.
add_executable(editor_model_viewport_test editor/model_viewport_test.cpp)
target_link_libraries(editor_model_viewport_test PRIVATE opennova_editor)
target_include_directories(editor_model_viewport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_model_viewport COMMAND editor_model_viewport_test)

# DI-04: a clip's events heard in the model preview, on the game's ticks, through the game's rules.
add_executable(editor_clip_sounds_test editor/clip_sounds_test.cpp)
target_link_libraries(editor_clip_sounds_test PRIVATE opennova_editor)
target_include_directories(editor_clip_sounds_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_clip_sounds COMMAND editor_clip_sounds_test)

# DI-10: a model's damage states named in the game's words and played as the game destroys the item (the
# husk swap, the pieces, the death sound, the destroy fade); the retail leg over JO's damageable items.
add_executable(editor_model_damage_test editor/model_damage_test.cpp)
target_link_libraries(editor_model_damage_test PRIVATE opennova_editor)
target_include_directories(editor_model_damage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_model_damage COMMAND editor_model_damage_test)

# What a drag of a menu window's handles writes (editor/preview/menu_layout_edit, S9k1):
# the plan against the runtime's POSITION solve, the grid, the handles, the edits.
add_executable(editor_menu_layout_edit_test editor/menu_layout_edit_test.cpp)
target_link_libraries(editor_menu_layout_edit_test PRIVATE opennova_editor)
target_include_directories(editor_menu_layout_edit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_menu_layout_edit COMMAND editor_menu_layout_edit_test)

# Arranging several menu windows (editor/preview/menu_arrange, S9k2): align, distribute
# and drawing order on synthetic rects, then on a real menu through its compiled screen
# (the hit walk pins the order), and a move of several windows.
add_executable(editor_menu_arrange_test editor/menu_arrange_test.cpp)
target_link_libraries(editor_menu_arrange_test PRIVATE opennova_editor)
target_include_directories(editor_menu_arrange_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_menu_arrange COMMAND editor_menu_arrange_test)

# The query seam (S13 A5): every list a query serves paged in C++ (the pages make the list,
# count the whole, an offset or a limit out of range refused naming the query), the state by
# section and since a revision, the catalog from the tables, the menu reads editor_menu served
# (its tree, its findings, the render check's render) and its batch as edit_record's wire form
# (records by identity or label, a list replaced, one undo step), over a real session.
add_executable(editor_query_test editor/editor_query_test.cpp)
target_link_libraries(editor_query_test PRIVATE opennova_editor)
target_include_directories(editor_query_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_query COMMAND editor_query_test)

# The render check (S9j2): every menu screen compiled headless, its notes as Problems rows
# on their records (never a build's gate, never the graph's findings twice), rendered
# again only when something it read moved; a SKIP-LEG over OPENNOVA_JO_ASSETS' shipped
# menus (every screen renders in the document's order, every interned name a graph edge).
add_executable(editor_menu_render_test editor/menu_screen_render_test.cpp)
target_link_libraries(editor_menu_render_test PRIVATE opennova_editor)
target_include_directories(editor_menu_render_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_menu_render COMMAND editor_menu_render_test)

# The imports (S8): the PNG reader, the quantizer, the sidecars, the import pass, the
# scan and the build over the outputs, and the game install as an import source; the plan
# of an import with the files it needs (S11f, import_plan_test.cpp) and the session's
# import of it, staged and published (S11g, import_apply_test.cpp: its retail leg imports a
# packed install's menu with the files it needs, OPENNOVA_JO_DIR).
add_executable(editor_import_test editor/import_test.cpp editor/import_plan_test.cpp editor/import_apply_test.cpp)
target_link_libraries(editor_import_test PRIVATE opennova_editor)
target_include_directories(editor_import_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_import COMMAND editor_import_test)

# The font importer: a font set's glyph sheet made into a .fnt through our own FNT writer and read
# back by fnt_parse (each advance mode, tracking, the space, the zero records, the header words, the
# pages, the refusals), its run over an ImportContext, over a project (the set an import source, the
# .fnt packed into localres, a changed sheet imported again), and an import of a set from the disk
# bringing its sheet beside it.
add_executable(editor_font_import_test editor/font_import_test.cpp)
target_link_libraries(editor_font_import_test PRIVATE opennova_editor)
target_include_directories(editor_font_import_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_font_import COMMAND editor_font_import_test)

# The editor's ImGui windows over a null backend (ADR 0046 d11): built with the pass. The
# workspace's shell (S11d: its layout, the Document tabs, Files, the menus, the modals) is
# workspace_test.cpp's; the change marks, the reveal, Output and the shortcuts (S11e)
# markers_test.cpp's; the bounds sweep (S11e: nothing runs past what shows of it)
# bounds_test.cpp's; the field controls (S12 D4) field_widgets_test.cpp's; the reference
# picker (S12 D7: each field's own filter, the keys, a Files row dropped on a value, a missing
# value's fixes) reference_picker_test.cpp's; find (S12 D8: the Document window's find bar, Find
# in project) find_test.cpp's; Rename everywhere from the Inspector (S12 D9) rename_test.cpp's; every
# control that raises a request enabled exactly when the busy gate takes it (S13 A1) gate_test.cpp's;
# a clip's timeline and the animation types' Inspector (S17) animation_test.cpp's; the project, the
# import, Files and the layout as a modder meets them (the UX round's project lane) project_test.cpp's;
# Back and Forward (the navigation history: the arrows, Alt+Left and Alt+Right, the mouse's buttons)
# navigation_test.cpp's; all share editor_ui/editor_ui_test_support.h. One ctest row per group (S13 V1: a failure names its
# group, the rows run in parallel), each running `editor_ui_test <group>`; editor_ui_groups holds
# this list and the program's table of groups to one list.
if(OPENNOVA_IMGUI)
    add_executable(editor_ui_test editor_ui/editor_windows_test.cpp editor_ui/workspace_test.cpp
                                  editor_ui/markers_test.cpp editor_ui/bounds_test.cpp
                                  editor_ui/field_widgets_test.cpp editor_ui/reference_picker_test.cpp
                                  editor_ui/find_test.cpp editor_ui/rename_test.cpp editor_ui/gate_test.cpp
                                  editor_ui/logic_test.cpp editor_ui/animation_test.cpp
                                  editor_ui/model_test.cpp editor_ui/project_test.cpp
                                  editor_ui/navigation_test.cpp)
    target_link_libraries(editor_ui_test PRIVATE opennova_editor opennova_imgui)
    target_include_directories(editor_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set(OPENNOVA_EDITOR_UI_GROUPS workspace markers bounds field_widgets reference_picker find rename
                                  menu inspector preview styles problems gate logic animation model
                                  project navigation)
    foreach(group IN LISTS OPENNOVA_EDITOR_UI_GROUPS)
        add_test(NAME editor_ui_${group} COMMAND editor_ui_test ${group})
    endforeach()
    add_test(NAME editor_ui_groups COMMAND editor_ui_test --groups ${OPENNOVA_EDITOR_UI_GROUPS})

    # The document views' contract (S13 V3, ui/document_views): every DocumentTypeId's view over a
    # file of its type on the null backend, its row's role, no request raised by drawing, nothing
    # wider than its tab, a RevealRecord held until it draws; two open documents a view each.
    add_executable(editor_view_contract_test editor_ui/view_contract_test.cpp)
    target_link_libraries(editor_view_contract_test PRIVATE opennova_editor opennova_imgui)
    target_include_directories(editor_view_contract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME editor_view_contract COMMAND editor_view_contract_test)
endif()

add_executable(project_cli_test editor/project_cli_test.cpp)
target_link_libraries(project_cli_test PRIVATE opennova_project_cli)
target_include_directories(project_cli_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME project_cli COMMAND project_cli_test)

# opennova-project as the editor's session, headless (S13 A7): the verb table against the request
# and query tables, each verb's --json the session's answer to the same requests, the request and
# query verbs, --install; its retail leg a project imported from the game install whose Problems
# the query verb reads as the session does.
add_executable(project_cli_session_test editor/project_cli_session_test.cpp)
target_link_libraries(project_cli_session_test PRIVATE opennova_project_cli)
target_include_directories(project_cli_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(project_cli_session COMMAND project_cli_session_test)

add_executable(def_write_test def/def_write_test.cpp)
target_link_libraries(def_write_test PRIVATE opennova_base opennova_formats)
target_include_directories(def_write_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(def_write COMMAND def_write_test)

add_executable(def_noted_write_test def/def_noted_write_test.cpp)
target_link_libraries(def_noted_write_test PRIVATE opennova_base opennova_formats)
target_include_directories(def_noted_write_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(def_noted_write COMMAND def_noted_write_test)

add_executable(editor_catalog_test editor/catalog_document_test.cpp)
target_link_libraries(editor_catalog_test PRIVATE opennova_editor)
target_include_directories(editor_catalog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_catalog COMMAND editor_catalog_test)

# The item ids the engine fixes, as the catalog keeps them (ADR 0046 S19, "Reserved item ids"): the
# ids a new item never takes, the edits refused, the findings and their fixes, the engine's rows added.
add_executable(editor_reserved_items_test editor/reserved_items_test.cpp)
target_link_libraries(editor_reserved_items_test PRIVATE opennova_editor)
target_include_directories(editor_reserved_items_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_reserved_items COMMAND editor_reserved_items_test)
# The sound lane (ADR 0046; DI-02, DI-16): the bank and SndProf.def documents, the sound names in the
# graph by the game's bank order, the preview's picks, the session's play_sound; a SKIP-LEG without
# OPENNOVA_JO_ASSETS (every shipped bank and SndProf.def through the documents).
add_executable(editor_sound_documents_test editor/sound_documents_test.cpp)
target_link_libraries(editor_sound_documents_test PRIVATE opennova_editor)
target_include_directories(editor_sound_documents_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_sound_documents COMMAND editor_sound_documents_test)

add_executable(editor_strings_test editor/strings_document_test.cpp)
target_link_libraries(editor_strings_test PRIVATE opennova_editor)
target_include_directories(editor_strings_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_strings COMMAND editor_strings_test)

# The menu document (S6c), the edits that must not lose authored data (S9d) and the
# retail sweep through it: a SKIP-LEG per root (OPENNOVA_JO_DIR's installed menus,
# OPENNOVA_JO_ASSETS' fifteen revx02 fixtures).
# The menu's table (editor/documents/mnu_table, ADR 0046 S13 D10): every member reached by
# exactly one field path or list, over a synthetic every-element menu and (a SKIP-LEG without
# OPENNOVA_JO_ASSETS) the fifteen reference menus rebuilt through the table.
add_executable(editor_menu_table_test editor/menu_table_test.cpp)
target_link_libraries(editor_menu_table_test PRIVATE opennova_editor)
target_include_directories(editor_menu_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_menu_table COMMAND editor_menu_table_test)

add_executable(editor_menus_test editor/mnu_document_test.cpp)
target_link_libraries(editor_menus_test PRIVATE opennova_editor)
target_include_directories(editor_menus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_menus COMMAND editor_menus_test)

# The model document (S10d): untouched saves, edits, structure and refusals, undo,
# save and reload, the validator (S11h: frames and registers as the runtime reads them);
# a SKIP-LEG per root: every retail model saved untouched (OPENNOVA_JO_ASSETS), every
# model of the game install validated with no error (OPENNOVA_JO_DIR).
# The model's table (editor/documents/model_table.cpp, ADR 0046 S13 D10; the format's
# threedi_schema test it was): a Set of every field of every record to itself changes no
# byte, over the synthetic models and (a SKIP-LEG without OPENNOVA_JO_DIR) every model the
# game install ships; the derived words, the refusals, the lists, the rows.
add_executable(editor_model_table_test editor/model_table_test.cpp)
target_link_libraries(editor_model_table_test PRIVATE opennova_editor)
target_include_directories(editor_model_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_model_table COMMAND editor_model_table_test)

# The mission's table (editor/documents/mission_table.h, ADR 0046 S13 D10, S14): a mission's records
# as rows of the one table shape over the format's field rows, read through the mission document;
# the rules, the editor's ranges, the lists and the chains over the synthetic dense mission, and (a
# SKIP-LEG without OPENNOVA_JO_DIR) every mission the game install ships read and set to itself.
add_executable(editor_mission_table_test editor/mission_table_test.cpp)
target_link_libraries(editor_mission_table_test PRIVATE opennova_editor)
target_include_directories(editor_mission_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_mission_table COMMAND editor_mission_table_test)

# The mission document (editor/documents/mission_document.h, ADR 0046 S14) over the minted
# synth_logic mission: the rows in bands, every reference as a graph edge of its kind, the
# edits and their cascades, the parse's findings, the validator's findings and the mission's
# use check, the picture's reads, an item's TYPE on its symbol; and (a SKIP-LEG without
# OPENNOVA_JO_DIR) every shipped mission opened, its references counted and its findings by
# code.
add_executable(editor_mission_document_test editor/mission_document_test.cpp)
target_link_libraries(editor_mission_document_test PRIVATE opennova_editor)
target_include_directories(editor_mission_document_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_mission_document COMMAND editor_mission_document_test)

# A mission's logic in words (editor/documents/mission_sentence.h, ADR 0046 S15): each trigger and
# action worded from its type and its typed parameters, the chain's fold, an event's flags, delay and
# repeat, a type or a value naming nothing said so; the minted mission's titles; the types "Add
# trigger" and "Add action" offer; and (a SKIP-LEG without OPENNOVA_JO_DIR) every shipped event worded.
add_executable(editor_mission_sentence_test editor/mission_sentence_test.cpp)
target_link_libraries(editor_mission_sentence_test PRIVATE opennova_editor)
target_include_directories(editor_mission_sentence_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_mission_sentence COMMAND editor_mission_sentence_test)

# Adding and editing a mission's logic without its format (editor/documents/mission_logic.h,
# mission_uses.h, ADR 0046 S15): the forms, an add by type, the 20 an event holds refused before the
# edit, a retype keeping what applies, a move, a negation and a join, what names a record, the
# mission_logic and mission_uses queries; and (a SKIP-LEG without OPENNOVA_JO_DIR) every shipped record
# given a form and the defaults' measured values.
add_executable(editor_mission_logic_test editor/mission_logic_test.cpp)
target_link_libraries(editor_mission_logic_test PRIVATE opennova_editor)
target_include_directories(editor_mission_logic_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_mission_logic COMMAND editor_mission_logic_test)

# The display names (ADR 0046 S15, Names: editor/documents/mission_labels.h and
# editor/graph/display_names.h) over the minted synth_logic mission: every record and every value
# that names something in a modder's words, with and without the project's names (an item by its
# catalog's name, a text key by its string), what names nothing said in words, the picker's
# choices worded, the cache, the wire's words through a session; and (a SKIP-LEG without
# OPENNOVA_JO_DIR) every record and reference of the shipped missions worded with the install's
# catalogs and tables, timed.
add_executable(editor_mission_labels_test editor/mission_labels_test.cpp)
target_link_libraries(editor_mission_labels_test PRIVATE opennova_editor)
target_include_directories(editor_mission_labels_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_mission_labels COMMAND editor_mission_labels_test)

# Help in a script's text (editor/session/script_assist.h, ADR 0046 S15): the completions from the WAC
# compiler's tables and the mission of the script's name, what a word is, where it is defined, the
# mission's script, a compiler report in plain words, and the script_assist query.
add_executable(editor_script_assist_test editor/script_assist_test.cpp)
target_link_libraries(editor_script_assist_test PRIVATE opennova_editor)
target_include_directories(editor_script_assist_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME editor_script_assist COMMAND editor_script_assist_test)

# The one table shape (editor/model/table_shape.h, ADR 0046 S13 D10) as the menu's, the def
# catalog's (items, weapons, powerups), the model's and the mission's tables fill it: the value, the
# choice, the labelled field, the record handle and the list ops over a file of each; the cost of a
# keystroke in a def catalog (one field's row resolved); and (a SKIP-LEG without OPENNOVA_JO_DIR)
# the game's powerup.def opened as the catalog's rows, generated back as the file was (its modeled layout).
add_executable(editor_table_shape_test editor/table_shape_test.cpp)
target_link_libraries(editor_table_shape_test PRIVATE opennova_editor)
target_include_directories(editor_table_shape_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_table_shape COMMAND editor_table_shape_test)

add_executable(editor_model_document_test editor/model_document_test.cpp)
target_link_libraries(editor_model_document_test PRIVATE opennova_editor)
target_include_directories(editor_model_document_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_model_document COMMAND editor_model_document_test)

# A model's bullet-face surfaces set on its materials and its records in words (S17): each face's
# material found, a material's surface set on every face made from it in one step, mixed and unified,
# the refusals, the game's words; a SKIP-LEG counting the install's faces and mixed materials.
add_executable(editor_model_surfaces_test editor/model_surfaces_test.cpp)
target_link_libraries(editor_model_surfaces_test PRIVATE opennova_editor)
target_include_directories(editor_model_surfaces_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_model_surfaces COMMAND editor_model_surfaces_test)

# A model's collision shown in its viewport (S17): a box's planes make the box, every synthetic
# model's collision as shapes on the collision LOD's triangles, each record's shape and back, a
# person's hit spheres at the game's radius, a live part carrying its section, picking; a SKIP-LEG
# making every installed model's volumes into solids inside their boxes.
add_executable(editor_model_collision_test editor/model_collision_test.cpp)
target_link_libraries(editor_model_collision_test PRIVATE opennova_editor)
target_include_directories(editor_model_collision_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_model_collision COMMAND editor_model_collision_test)

# The clip and animation map documents (S10e): untouched saves, engine data edits,
# the motion refused, rows and clips, the reset row, the validator and the clip edges;
# a SKIP-LEG saving every retail clip and table untouched.
add_executable(editor_animation_documents_test editor/animation_documents_test.cpp)
target_link_libraries(editor_animation_documents_test PRIVATE opennova_editor)
target_include_directories(editor_animation_documents_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_animation_documents COMMAND editor_animation_documents_test)

# The stylesheet document (S9i): rows per line, edits the game reads as shown, the
# refusals, the validator; a SKIP-LEG for OPENNOVA_JO_ASSETS' shipped menu_style.mns.
add_executable(editor_styles_test editor/mns_document_test.cpp)
target_link_libraries(editor_styles_test PRIVATE opennova_editor)
target_include_directories(editor_styles_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_mixed_test(editor_styles COMMAND editor_styles_test)

# The capture-point labels: the map-overlay slot walk and the marker types 8/9.
add_executable(hud_capture_labels_test hud/hud_capture_labels_test.cpp)
target_link_libraries(hud_capture_labels_test PRIVATE opennova_runtime)
target_compile_features(hud_capture_labels_test PRIVATE cxx_std_17)
add_test(NAME hud_capture_labels COMMAND hud_capture_labels_test)
