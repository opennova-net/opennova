# Machine-local retail data gates (tests/common/retail_paths.h, the four
# OPENNOVA_* roots in docs/asset-gated-tests.md): a gated test prints
# "SKIP: needs <what>" and exits 77 when its data is absent, so ctest reports it
# Skipped, never Passed. A test whose synthetic legs ran prints "SKIP-LEG:
# needs <what>" for its missing retail leg and exits 0 (a plain add_test).
function(opennova_add_gated_test name)
    add_test(NAME ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

# The retail-mission rig the asset-gated world/mission ctests share
# (common/retail_mission_files.h): one real mission booted through the
# engine's own mission::MissionKernel (ADR 0042 d3) and ticked on the SP
# listen frame (inmatch::listen_host) or the kernel's bare no-net path; this
# pair supplies only the retail terrain documents and the TickTarget adapter.
add_library(opennova_test_rig STATIC common/retail_mission_files.cpp)
target_include_directories(opennova_test_rig PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(opennova_test_rig
    PUBLIC opennova_net opennova_runtime opennova_formats opennova_base)

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

add_executable(threedi_compare_test threedi/compare_test.cpp threedi/threedi_compare.cpp)
target_link_libraries(threedi_compare_test PRIVATE opennova_formats)
target_include_directories(threedi_compare_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_compare COMMAND threedi_compare_test)

# Retail global control-register catalog.
add_executable(threedi_ctrl_catalog_test threedi/ctrl_catalog_test.cpp)
target_link_libraries(threedi_ctrl_catalog_test PRIVATE opennova_formats)
target_include_directories(threedi_ctrl_catalog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_ctrl_catalog COMMAND threedi_ctrl_catalog_test)

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
foreach(test comment trim_value parse variants)
    add_executable(adm_${test}_test anim/adm_${test}_test.cpp)
    target_link_libraries(adm_${test}_test PRIVATE opennova_formats)
    target_include_directories(adm_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
foreach(test comment trim_value variants)
    add_test(NAME adm_${test} COMMAND adm_${test}_test)
endforeach()
opennova_add_gated_test(adm_parse COMMAND adm_parse_test)

# BAD tests: the null guard unconditionally, the shipped BINOC.bad pins as the
# reference-set SKIP-LEG leg.
add_executable(bad_parse_test bad/bad_parse_test.cpp)
target_link_libraries(bad_parse_test PRIVATE opennova_formats)
target_include_directories(bad_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bad_parse COMMAND bad_parse_test)

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
add_test(NAME anim_sample COMMAND anim_sample_test)

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
# weapons and ammo run their inline blocks unconditionally and pin the shipped
# tables from the reference fixture set as SKIP-LEG legs; every hudpos leg
# reads the shipped table, so it gates whole on OPENNOVA_JO_ASSETS.
foreach(test items weapons ammo hudpos item_attrib item_type)
    add_executable(def_parse_${test}_test def/def_parse_${test}_test.cpp)
    target_link_libraries(def_parse_${test}_test PRIVATE opennova_formats)
    target_include_directories(def_parse_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
foreach(test items weapons ammo item_attrib item_type)
    add_test(NAME def_parse_${test} COMMAND def_parse_${test}_test)
endforeach()
opennova_add_gated_test(def_parse_hudpos COMMAND def_parse_hudpos_test)

# PLAYER_INFO loadout weight + encumbrance math (D-PLAYERINFO-11 weight readout).
add_executable(def_loadout_weight_test def/def_loadout_weight_test.cpp)
target_link_libraries(def_loadout_weight_test PRIVATE opennova_formats)
add_test(NAME def_loadout_weight COMMAND def_loadout_weight_test)

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
    target_link_libraries(avatars_${test}_test PRIVATE opennova_formats)
    target_include_directories(avatars_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(avatars_${test}_test PRIVATE cxx_std_17)
    add_test(NAME avatars_${test} COMMAND avatars_${test}_test)
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

add_executable(mission_catalog_test mission/mission_catalog_test.cpp)
target_link_libraries(mission_catalog_test PRIVATE opennova_runtime)
target_include_directories(mission_catalog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_catalog_test PRIVATE cxx_std_17)
add_test(NAME mission_catalog COMMAND mission_catalog_test)

# The mission load plan (mission/mission_load_plan.h): the stage order and the
# witnessed loading-screen anchors the shell presents.
add_executable(mission_load_plan_test mission/mission_load_plan_test.cpp)
target_link_libraries(mission_load_plan_test PRIVATE opennova_runtime)
add_test(NAME mission_load_plan COMMAND mission_load_plan_test)

# fixtures/bms/synth_dense.bms: the synthetic dense mission (four populated
# pools, zero-valued optional fields) authored through MissionDocument and
# written by bms::write; mission_bms and mission_mis_idempotency read it.
add_executable(minimal_bms_gen_test fixtures/minimal_bms_gen.cpp)
target_link_libraries(minimal_bms_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_bms_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minimal_bms_gen COMMAND minimal_bms_gen_test)

add_executable(mission_bms_test mission/mission_bms_test.cpp)
target_link_libraries(mission_bms_test PRIVATE opennova_formats)
target_include_directories(mission_bms_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_bms COMMAND mission_bms_test)

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
add_test(NAME mission_mis_idempotency COMMAND mission_mis_idempotency_test)

# Corpus round-trip: parse->write byte-exact over a directory of real shipped .bms missions.
# Gated on OPENNOVA_JO_ASSETS (the tree's loose .bms; copyrighted, not committed); skips + passes when unset.
add_executable(mission_corpus_test mission/mission_corpus_test.cpp)
target_link_libraries(mission_corpus_test PRIVATE opennova_formats)
target_include_directories(mission_corpus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_corpus_test PRIVATE cxx_std_17)
opennova_add_gated_test(mission_corpus COMMAND mission_corpus_test)

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
target_link_libraries(inmatch_session_test PRIVATE opennova_net)
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
add_executable(score_roundtrip_test score/score_roundtrip_test.cpp)
target_link_libraries(score_roundtrip_test PRIVATE opennova_formats)
target_include_directories(score_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME score_roundtrip COMMAND score_roundtrip_test)

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
add_test(NAME dbf_roundtrip COMMAND dbf_roundtrip_test)

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

# S13 (ADR 0028): the envs-marker resolution — the items.def dispatch-tag gate across
# entity pools, the authored soundloop slots passed through unfiltered, walk order,
# and the BMS type-id -> items.def id offset. [orig: @0x82ABD4 -> @0x4a8080]
add_executable(envs_markers_test audio/envs_markers_test.cpp)
target_link_libraries(envs_markers_test PRIVATE opennova_runtime)
add_test(NAME audio_envs_markers COMMAND envs_markers_test)

add_executable(bank_chain_test audio/bank_chain_test.cpp)
target_link_libraries(bank_chain_test PRIVATE opennova_runtime)
add_test(NAME audio_bank_chain COMMAND bank_chain_test)

# SndProf.def sound-profile parse + lookup (asset-gated retail spot check via JO_ASSETS).
add_executable(sound_profile_test audio/sound_profile_test.cpp)
target_link_libraries(sound_profile_test PRIVATE opennova_runtime)
target_include_directories(sound_profile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sound_profile_test PRIVATE cxx_std_17)
add_test(NAME sound_profile COMMAND sound_profile_test)

# PFF tests
add_executable(pff_unit_test pff/pff_unit_test.cpp)
target_link_libraries(pff_unit_test PRIVATE opennova_formats)
target_include_directories(pff_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pff_unit COMMAND pff_unit_test)

# Game profile table tests
add_executable(gameprofile_test gameprofile/gameprofile_test.c)
target_link_libraries(gameprofile_test PRIVATE opennova_base)
target_include_directories(gameprofile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gameprofile COMMAND gameprofile_test)

add_executable(required_resources_test gameprofile/required_resources_test.c)
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
target_compile_definitions(sbf_encode_test PRIVATE
    SBF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/sbf")
if(NOT MSVC)
    target_link_libraries(sbf_encode_test PRIVATE m)
endif()
add_test(NAME sbf_encode COMMAND sbf_encode_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
# mus_encode_file; the shipped jo_gamemus.bin / jo_menumus.bin and their golden
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
add_test(NAME mus_parse COMMAND mus_parse_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_compat_test mus/mus_compat_test.cpp)
target_link_libraries(mus_compat_test PRIVATE opennova_formats)
target_include_directories(mus_compat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_compat_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_compat COMMAND mus_compat_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_decompile_test mus/mus_decompile_test.cpp)
target_link_libraries(mus_decompile_test PRIVATE opennova_formats)
target_include_directories(mus_decompile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_decompile_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_decompile COMMAND mus_decompile_test
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
add_test(NAME mus_roundtrip COMMAND mus_roundtrip_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_names_roundtrip_test mus/mus_names_roundtrip_test.cpp)
target_link_libraries(mus_names_roundtrip_test PRIVATE opennova_formats)
target_include_directories(mus_names_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_names_roundtrip_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_names_roundtrip COMMAND mus_names_roundtrip_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Entry-section round-trip: the compiler hardcodes entry_section_index = 0, so a
# faithful round-trip needs the original entry to be its lowest-offset section
# (true for both stock bins). Pins that the recompiled entry names the same
# section -- a drift the text round-trip and encode-idempotence cannot see.
add_executable(mus_entry_roundtrip_test mus/mus_entry_roundtrip_test.cpp)
target_link_libraries(mus_entry_roundtrip_test PRIVATE opennova_formats)
target_include_directories(mus_entry_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_entry_roundtrip_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_entry_roundtrip COMMAND mus_entry_roundtrip_test
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
add_test(NAME mus_vm COMMAND mus_vm_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_vm_fixes_test mus/mus_vm_fixes_test.cpp)
target_link_libraries(mus_vm_fixes_test PRIVATE opennova_formats)
target_include_directories(mus_vm_fixes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mus_vm_fixes COMMAND mus_vm_fixes_test)

# Write-path parity: the canonical re-encoded form is a byte-stable fixed point
# (byte-identity to the ORIGINAL .bin is impossible because debug info is not
# reproduced). See the test file header.
add_executable(mus_encode_idempotence_test mus/mus_encode_idempotence_test.cpp)
target_link_libraries(mus_encode_idempotence_test PRIVATE opennova_formats)
target_include_directories(mus_encode_idempotence_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_encode_idempotence_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_encode_idempotence COMMAND mus_encode_idempotence_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# VFS tests (mount precedence, mount_game expansion override, SCR decode-on-read)
add_executable(vfs_test vfs/vfs_test.cpp)
target_link_libraries(vfs_test PRIVATE opennova_base opennova_formats)
target_include_directories(vfs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vfs_test PRIVATE cxx_std_17)
add_test(NAME vfs COMMAND vfs_test)

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

# Minimal authored fixture set — the fatal-set RTXT string tables
# (assets/) and the synthetic parity set (fixtures/rtxt). Regenerates each
# in memory and guards it byte-matches the committed file + round-trips.
# See assets/README.md and fixtures/README.md.
add_executable(minimal_rtxt_gen_test fixtures/minimal_rtxt_gen.cpp)
target_link_libraries(minimal_rtxt_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_rtxt_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(minimal_rtxt_gen_test PRIVATE
    GAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
    RTXT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/rtxt")
add_test(NAME minimal_rtxt_gen COMMAND minimal_rtxt_gen_test)

# Minimal set's hand-authored text resources must be CRLF: retail's text
# parsers are not LF-tolerant and fail SILENTLY (a LF-only menu_style.mns
# dead-ends the menu shell before main.mnu; a bare LF stops the .def parser
# after block 0). Witnessed on retail 2026-08-23.
add_executable(minimal_eol_guard_test fixtures/minimal_eol_guard.cpp)
target_compile_definitions(minimal_eol_guard_test PRIVATE
    GAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
add_test(NAME minimal_eol_guard COMMAND minimal_eol_guard_test)

# Minimal set's text .def files parse through our loaders (items/weapon/ammo).
add_executable(minimal_def_validate_test fixtures/minimal_def_validate.cpp)
target_link_libraries(minimal_def_validate_test PRIVATE opennova_formats)
target_compile_definitions(minimal_def_validate_test PRIVATE
    GAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
add_test(NAME minimal_def_validate COMMAND minimal_def_validate_test)

# Minimal set's menus parse (main Startup node + mp host/join screen).
add_executable(minimal_mnu_validate_test fixtures/minimal_mnu_validate.cpp)
target_link_libraries(minimal_mnu_validate_test PRIVATE opennova_formats)
target_compile_definitions(minimal_mnu_validate_test PRIVATE
    GAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
add_test(NAME minimal_mnu_validate COMMAND minimal_mnu_validate_test)

# Minimal set's custom map — the .env + .bms legs (the .trn rides its own slice).
add_executable(minimal_map_validate_test fixtures/minimal_map_validate.cpp)
target_link_libraries(minimal_map_validate_test PRIVATE opennova_formats)
target_compile_definitions(minimal_map_validate_test PRIVATE
    GAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
add_test(NAME minimal_map_validate COMMAND minimal_map_validate_test)

# Minimal map's terrain config (.trn) — round-trip guarded; the heavy .cpt/tiles
# are generate-at-package (not committed).
add_executable(minimal_trn_gen_test fixtures/minimal_trn_gen.cpp)
target_include_directories(minimal_trn_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(minimal_trn_gen_test PRIVATE opennova_formats)
target_compile_definitions(minimal_trn_gen_test PRIVATE
    GAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
add_test(NAME minimal_trn_gen COMMAND minimal_trn_gen_test)

# Minimal map's source-art images (colormap/detail/tilestrip TGAs, charmap/
# foliagemap PCX) the .trn references — authored + validated.
add_executable(minimal_art_validate_test fixtures/minimal_art_validate.cpp)
target_link_libraries(minimal_art_validate_test PRIVATE opennova_formats)
target_compile_definitions(minimal_art_validate_test PRIVATE
    GAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
add_test(NAME minimal_art_validate COMMAND minimal_art_validate_test)

# Minimal set's generated boot font (minimal_fnt_builder.h): validity +
# byte-stable roundtrip + label glyphs drawable. Always-on; the boot font
# itself is emitted at package time, never committed. The same builder mints
# fixtures/fnt/synth_{1,3}page.fnt, byte-compared every run (--write regenerates).
add_executable(minimal_fnt_gen_test fixtures/minimal_fnt_gen.cpp)
target_link_libraries(minimal_fnt_gen_test PRIVATE opennova_formats)
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

# Minimal mission's dialog companions (mnml.dbf + mnml.lwf): empty-but-valid
# via the engine/formats/dbf + engine/formats/lwf writers, byte-stable vs the committed files.
add_executable(minimal_companion_gen_test fixtures/minimal_companion_gen.cpp)
target_link_libraries(minimal_companion_gen_test PRIVATE opennova_formats)
target_include_directories(minimal_companion_gen_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(minimal_companion_gen_test PRIVATE
    GAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
add_test(NAME minimal_companion_gen COMMAND minimal_companion_gen_test)

# Minimal set's generated boot music (minimal_mus_builder.h): the silent SBF
# bank + the minimal MENUMUS/GAMEMUS scripts parse back valid. Always-on; the
# files are emitted at package time, never committed.
add_executable(minimal_mus_gen_test fixtures/minimal_mus_gen.cpp)
target_link_libraries(minimal_mus_gen_test PRIVATE opennova_formats)
add_test(NAME minimal_mus_gen COMMAND minimal_mus_gen_test)

# Minimal set's packaging tool: PFF-bundle the committed assets/ tree or
# assemble a loose install. A TOOL, not a test: it only acts on
# `minimal_pff_package_test --write-pff` / `--install <dir>` (assets/README.md),
# so it is built here beside the generators but never registered with ctest.
add_executable(minimal_pff_package_test fixtures/minimal_pff_package.cpp)
target_include_directories(minimal_pff_package_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(minimal_pff_package_test PRIVATE opennova_runtime opennova_formats)
target_compile_definitions(minimal_pff_package_test PRIVATE
    GAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

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

add_executable(resource_index_mus_test resource_index/resource_index_mus_test.cpp)
target_link_libraries(resource_index_mus_test PRIVATE opennova_base)
target_include_directories(resource_index_mus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME resource_index_mus COMMAND resource_index_mus_test)

# simassets: the sim-side .3di derivations (ADR 0028)
add_executable(simassets_model_builders_test simassets/model_builders_test.cpp)
target_link_libraries(simassets_model_builders_test PRIVATE opennova_runtime)
target_include_directories(simassets_model_builders_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME simassets_model_builders COMMAND simassets_model_builders_test)

add_executable(simassets_fp_viewmodel_spec_test simassets/fp_viewmodel_spec_test.cpp)
target_link_libraries(simassets_fp_viewmodel_spec_test PRIVATE opennova_runtime)
target_include_directories(simassets_fp_viewmodel_spec_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME simassets_fp_viewmodel_spec COMMAND simassets_fp_viewmodel_spec_test)

add_executable(simassets_adm_root_motion_test simassets/adm_root_motion_test.cpp)
target_link_libraries(simassets_adm_root_motion_test PRIVATE opennova_runtime)
target_include_directories(simassets_adm_root_motion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME simassets_adm_root_motion COMMAND simassets_adm_root_motion_test)

add_executable(simassets_adm_clip_index_test simassets/adm_clip_index_test.cpp)
target_link_libraries(simassets_adm_clip_index_test PRIVATE opennova_runtime)
target_include_directories(simassets_adm_clip_index_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME simassets_adm_clip_index COMMAND simassets_adm_clip_index_test)

# The SECONDARY (weapon) channel composition over the shipped BINOC.bad rig
# plus its BINOC_twist.bad (the two-bone twist minted once from the retired
# writer; the test asserts that provenance before probing). Both come from the
# reference fixture set; gated on OPENNOVA_JO_ASSETS.
add_executable(simassets_adm_skeletal_clips_weapon_channel_test simassets/adm_skeletal_clips_weapon_channel_test.cpp)
target_link_libraries(simassets_adm_skeletal_clips_weapon_channel_test PRIVATE opennova_runtime)
target_include_directories(simassets_adm_skeletal_clips_weapon_channel_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
opennova_add_gated_test(simassets_adm_skeletal_clips_weapon_channel COMMAND simassets_adm_skeletal_clips_weapon_channel_test)

add_executable(simassets_item_traits_test simassets/item_traits_test.cpp)
target_link_libraries(simassets_item_traits_test PRIVATE opennova_runtime)
target_include_directories(simassets_item_traits_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME simassets_item_traits COMMAND simassets_item_traits_test)

add_executable(simassets_seat_spec_extract_test simassets/seat_spec_extract_test.cpp)
target_link_libraries(simassets_seat_spec_extract_test PRIVATE opennova_runtime)
target_include_directories(simassets_seat_spec_extract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME simassets_seat_spec_extract COMMAND simassets_seat_spec_extract_test)

# Object renderer/shader tests
foreach(test device_fog environment_cube foliage_frame_wind light_runtime light_scene light_terrain_pass material_classify material_eval
        particle_atlas_contract particle_frame_compiler_contract render_order
        q3_frame render_slot_shadow
        state_vectors tracer_frame precipitation_frame)
    add_executable(renderer_${test}_test renderer/${test}_test.cpp)
    target_link_libraries(renderer_${test}_test PRIVATE opennova_runtime opennova_formats)
    target_include_directories(renderer_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(renderer_${test}_test PRIVATE cxx_std_17)
    add_test(NAME renderer_${test} COMMAND renderer_${test}_test)
endforeach()

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

add_executable(fnt_pack_test fnt/fnt_pack_test.c)
target_link_libraries(fnt_pack_test PRIVATE opennova_formats)
add_test(NAME fnt_pack COMMAND fnt_pack_test)

add_executable(pcx_roundtrip_test pcx/pcx_roundtrip_test.cpp)
target_link_libraries(pcx_roundtrip_test PRIVATE opennova_formats)
target_include_directories(pcx_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pcx_roundtrip COMMAND pcx_roundtrip_test)

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

add_executable(til_overlay_bake_test til/til_overlay_bake_test.cpp)
target_link_libraries(til_overlay_bake_test PRIVATE opennova_formats)
target_include_directories(til_overlay_bake_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_overlay_bake COMMAND til_overlay_bake_test)

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

add_executable(trn_optional_polydata_test trn/trn_optional_polydata_test.cpp)
target_link_libraries(trn_optional_polydata_test PRIVATE opennova_formats)
target_include_directories(trn_optional_polydata_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME trn_optional_polydata COMMAND trn_optional_polydata_test)

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
add_executable(weather_state_test world/weather_state_test.cpp)
target_link_libraries(weather_state_test PRIVATE opennova_runtime)
target_include_directories(weather_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME weather_state COMMAND weather_state_test)

# The precipitation pool (runtime/environment/precipitation): seed, fall, wrap,
# floor, and the drawer compile.
add_executable(precipitation_test environment/precipitation_test.cpp)
target_link_libraries(precipitation_test PRIVATE opennova_runtime)
target_include_directories(precipitation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME precipitation COMMAND precipitation_test)

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
add_test(NAME terrain_tile_composer COMMAND terrain_tile_composer_test)

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

# The character registry (npruntime/character_registry.h): the packed-id
# decode, the per-side default walk, and the joiner profile projection
# (npruntime/join_character_profile.h).
add_executable(character_registry_test npruntime/character_registry_test.cpp)
target_link_libraries(character_registry_test PRIVATE opennova_net)
add_test(NAME character_registry COMMAND character_registry_test)

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

add_executable(hud_frame_compiler_test hud/hud_frame_compiler_test.cpp)
target_link_libraries(hud_frame_compiler_test PRIVATE opennova_runtime)
target_include_directories(hud_frame_compiler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME hud_frame_compiler COMMAND hud_frame_compiler_test)

add_executable(client_minimap_overlay_test netsim/client_minimap_overlay_test.cpp)
target_link_libraries(client_minimap_overlay_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_minimap_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_minimap_overlay COMMAND client_minimap_overlay_test)

# The Tab board's folded lanes (the 0x16 scoreboard + the 0x46 roster join —
# empty lists CLEAR and removals WIPE, the witnessed parser shape) plus the
# draw-time projection (netsim::project_scoreboard).
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

# The menu draw-list compiler (engine/runtime/menu, ADR 0033 R2): the
# witnessed .mnu widget draw walk — order, state-driven appearance/color
# selection, text placement/truncation, the edit caret, frame pieces, and the
# per-element int truncation (docs/mnu/menu-re.md "Widget render dispatch").
add_executable(menu_frame_compiler_test menu/menu_frame_compiler_test.cpp)
target_link_libraries(menu_frame_compiler_test PRIVATE opennova_runtime)
target_include_directories(menu_frame_compiler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_frame_compiler COMMAND menu_frame_compiler_test)

add_executable(menu_table_sort_test menu/menu_table_sort_test.cpp)
target_link_libraries(menu_table_sort_test PRIVATE opennova_runtime)
target_include_directories(menu_table_sort_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_table_sort COMMAND menu_table_sort_test)

add_executable(group_render_gate_test particle/group_render_gate_test.cpp)
target_link_libraries(group_render_gate_test PRIVATE opennova_runtime)
target_include_directories(group_render_gate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME group_render_gate COMMAND group_render_gate_test)

add_executable(vehicle_collision_damage_test world/vehicle_collision_damage_test.cpp)
target_link_libraries(vehicle_collision_damage_test PRIVATE opennova_runtime)
target_include_directories(vehicle_collision_damage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME vehicle_collision_damage COMMAND vehicle_collision_damage_test)

# The menu backdrop movie policy (engine/runtime/menu/menu_video): the
# witnessed three-slot table, the STARTUP/strips draw gate, and the
# expansion-first source resolution (docs/mnu/menu-re.md "The menu backdrop").
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
add_test(NAME trn_config_roundtrip COMMAND trn_config_roundtrip_test)

# Terrain world->source coordinate transform kernel (engine/runtime/terrain/coords).
# Characterizes the runtime resolve_world_sample formula plus the editor-mode
# guard divergences (bounds-reject, id-clamp, local-clamp) ported from
# Shared runtime and bounds-checked terrain coordinate transforms.
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

# Particle (.ptl) tests. Format half (engine/formats/ptl: model/parser/writer/
# tables) links opennova_formats; sim half (engine/runtime/particle: emitter +
# effect scene) links opennova_runtime. Witness matrix:
# docs/particles/ptl-format-re.md (CParticleDef_ParseFromConfigMap @ 0x5ed210,
# Jointops.exe IDB).
foreach(name smallest minimal_effect multi_section multi_layer table_handles
        smoke_all_fixtures enum_parse writer_roundtrip curve_lut atlas
        lenient_lines)
    add_executable(particle_${name}_test particle/particle_${name}_test.cpp)
    target_link_libraries(particle_${name}_test PRIVATE opennova_formats)
    target_include_directories(particle_${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(particle_${name}_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME particle_${name} COMMAND particle_${name}_test)
endforeach()
foreach(name emitter emit_shape translate effect_scene_contract)
    add_executable(particle_${name}_test particle/particle_${name}_test.cpp)
    target_link_libraries(particle_${name}_test PRIVATE opennova_runtime)
    target_include_directories(particle_${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(particle_${name}_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME particle_${name} COMMAND particle_${name}_test)
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
add_test(NAME cbin_roundtrip COMMAND cbin_roundtrip_test)

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
add_test(NAME mns_document COMMAND mns_document_test)

# World runtime substrate: addressable entities (faithful find_by_net_id), shared
# var store, entity-command primitives, the logic-tick clock, snapshot/restore.
add_executable(world_test world/world_test.cpp)
target_link_libraries(world_test PRIVATE opennova_runtime)
target_include_directories(world_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(world_test PRIVATE cxx_std_17)
add_test(NAME world COMMAND world_test)

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

# Entity_ClassifyForMinimap: definition/entity marker classification shared by
# every server producer before packet cadence/recipient policy.
add_executable(minimap_overlay_test world/minimap_overlay_test.cpp)
target_link_libraries(minimap_overlay_test PRIVATE opennova_runtime opennova_test_rig)
target_include_directories(minimap_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(minimap_overlay_test PRIVATE cxx_std_17)
add_test(NAME minimap_overlay COMMAND minimap_overlay_test)

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
target_link_libraries(feed_format_test PRIVATE opennova_runtime opennova_net)
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

# The player-chat lane (S2C 0x14) fold + the HUD channel table.
add_executable(client_replica_chat_test netsim/client_replica_chat_test.cpp)
target_link_libraries(client_replica_chat_test PRIVATE opennova_net opennova_runtime)
target_include_directories(client_replica_chat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_replica_chat COMMAND client_replica_chat_test)

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
add_test(NAME ground_conform COMMAND ground_conform_test)

# The spring leg's WIRING (vehicle_suspension.cpp): def keys, the role-picked
# parked latch + Flags 0x10 replication, the spring dt, the per-wheel step.
add_executable(vehicle_suspension_test world/vehicle_suspension_test.cpp)
target_link_libraries(vehicle_suspension_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(vehicle_suspension_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME vehicle_suspension COMMAND vehicle_suspension_test)

# AI subsystem (byte-exact foundation): state enum + struct layout, AIEvent ring,
# budget gate (AI_BeginUpdate), the infantry state-machine dispatcher + transitions,
# and the trivial enter/exit/event handler ports.
add_executable(ai_test world/ai_test.cpp)
target_link_libraries(ai_test PRIVATE opennova_runtime)
target_include_directories(ai_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ai_test PRIVATE cxx_std_17)
add_test(NAME ai COMMAND ai_test)

# Item destruction: the explosion queue + AoE gates, the bullet armor gates, the
# destructible death chain (husk flags + events + the kz chain blast), the
# death-piece pool, and the dead-item settle (world/destruction.h;
# docs/world/world-wac-ai-re.md §24).
add_executable(destruction_test world/destruction_test.cpp)
target_link_libraries(destruction_test PRIVATE opennova_runtime)
target_include_directories(destruction_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(destruction_test PRIVATE cxx_std_17)
add_test(NAME destruction COMMAND destruction_test)

# Throwables (world/throwables.h; world-wac-ai-re §27): PowerThrow charge,
# grenade bounce/fuse, satchel place/detonator, claymore cone + fan, AV-mine
# data-dead proximity, owner-death cleanup.
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
if(MSVC)
    target_link_options(infantry_test PRIVATE "/STACK:8388608")
endif()
add_test(NAME infantry COMMAND infantry_test)

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

# World-object collision (Entity_TestCollisionSections @0x4aef90 / _RaycastCollisionModel
# @0x413060 / _ComputeBoneCollisionForce @0x4ae150 / _ProcessCollisionAndPlatformPhysics
# @0x4b2bd0): matrix roundtrip, blink type-8 query + flags^6/indoors, type-1 segment
# clip, the ground probe through candidate models, wall push-out with prev-position
# gating, hurt/ladder volumes, and the idle skip throttle — over hand-built volumes.
add_executable(collision_test world/collision_test.cpp)
target_link_libraries(collision_test PRIVATE opennova_runtime)
target_include_directories(collision_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(collision_test PRIVATE cxx_std_17)
add_test(NAME collision COMMAND collision_test)

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
# build_sector_visibility_masks @0x5c8610 + render_visibility_portal_traversal
# @0x5c4ae0, test_sector_entity_occlusion @0x5c4610, the entity-collector gates +
# terrain_occlusion_check_three_rays @0x610ed0, Entity_QueryBlinkBoxesAtPoint
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
add_test(NAME occlusion_armry COMMAND occlusion_armry_test)

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

# The mode-4 death lerp camera (world/death_camera.h): FROM/TO, probe, lerp.
add_executable(death_camera_test world/death_camera_test.cpp)
target_link_libraries(death_camera_test PRIVATE opennova_runtime)
target_include_directories(death_camera_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(death_camera_test PRIVATE cxx_std_17)
add_test(NAME death_camera COMMAND death_camera_test)

# The local player's view cluster, orchestrated (world/local_player_view.h):
# the scope/binocular/NVG request gates, the tick's arbiter feed + death-camera
# entry edge, the composed frame read.
add_executable(local_player_view_test world/local_player_view_test.cpp)
target_link_libraries(local_player_view_test PRIVATE opennova_runtime)
target_include_directories(local_player_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(local_player_view_test PRIVATE cxx_std_17)
add_test(NAME local_player_view COMMAND local_player_view_test)

# The attach-command seat-selection mirror (world/vehicle_attach.h
# predict_seat_selection) and the shared weight table.
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

# The authoritative multiplayer rules/stats domain shared by TDM and A&S, with
# Co-op pinned to the existing WAC/BMS outcome path. [orig:
# GameEvent_ProcessScoring @0x52F550; Server_CheckWinConditions @0x51AD40]
add_executable(match_test world/match_test.cpp)
target_link_libraries(match_test PRIVATE opennova_runtime)
target_include_directories(match_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(match_test PRIVATE cxx_std_17)
add_test(NAME match COMMAND match_test)

# Host-authoritative ground-vehicle motor (net-re §5.13 drive-authority witness): the
# items.def physics-block scaling (ItemDef_ParsePhysicsProperty @0x49d870), the drive
# input mapping, steering chase, and speed pipeline of Entity_UpdateVehiclePhysics
# @0x48af00 — a mounted ctrl/drvr player's replicated input moves the vehicle.
add_executable(turret_window_test world/turret_window_test.cpp)
target_link_libraries(turret_window_test PRIVATE opennova_runtime)
target_compile_features(turret_window_test PRIVATE cxx_std_17)
add_test(NAME turret_window COMMAND turret_window_test)

add_executable(vehicle_motor_test world/vehicle_motor_test.cpp)
target_link_libraries(vehicle_motor_test PRIVATE opennova_runtime opennova_formats)
target_include_directories(vehicle_motor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vehicle_motor_test PRIVATE cxx_std_17)
add_test(NAME vehicle_motor COMMAND vehicle_motor_test)

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

# Root-motion track grill vs real JO clips (AnimMap_UpdateEntity @0x40b5f0): the .bad
# events array is the per-frame root record; rec[3] = cumulative forward distance whose
# 16.16 delta drives the infantry motor. Asset-gated: absent files skip (CI-safe).
add_executable(root_motion_test anim/root_motion_test.cpp)
target_link_libraries(root_motion_test PRIVATE opennova_formats)
target_include_directories(root_motion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(root_motion_test PRIVATE cxx_std_17)
opennova_add_gated_test(root_motion COMMAND root_motion_test)

# WAC scripting. The registry oracle (165-command ISA + 28 param types from the
# binary) exercises only the language front end (engine/formats/wac); VM behavior
# (var math, entity mutation, temporal/edge firing, env, authority) and the
# zero-crash parse over the real shipped .wac corpus compile against the world
# (dirs via argv, else the dev defaults; absent dirs skip).
add_executable(wac_registry_oracle_test wac/wac_registry_oracle_test.cpp)
target_link_libraries(wac_registry_oracle_test PRIVATE opennova_formats)
target_include_directories(wac_registry_oracle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(wac_registry_oracle_test PRIVATE cxx_std_17)
add_test(NAME wac_registry_oracle COMMAND wac_registry_oracle_test)

foreach(test behavior corpus)
    add_executable(wac_${test}_test wac/wac_${test}_test.cpp)
    target_link_libraries(wac_${test}_test PRIVATE opennova_runtime)
    target_include_directories(wac_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(wac_${test}_test PRIVATE cxx_std_17)
endforeach()
add_test(NAME wac_behavior COMMAND wac_behavior_test)
# The corpus sweep walks OPENNOVA_JO_ASSETS (argv dirs override); it reports
# Skipped without it.
opennova_add_gated_test(wac_corpus COMMAND wac_corpus_test)

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

# S9 (ADR 0028): the mission boot policy — the first ctest lock on the boot ORDER:
# run_mission_boot's exact step sequence + gates (role, missing inputs, the load abort,
# the ammo-gated AI-weapon seed), the mission-text fallback, and the .aip speed resolve.
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

# The kernel's lifecycle contract over the same synthetic mission, ungated: the
# ordering guards, the sealed play-start baseline (capture/restore rewinds the
# registry, the player, the clock and the event latches), the strict-vs-lenient
# WAC diagnostic policy, and the no-terrain path.
add_executable(mission_kernel_lifecycle_test mission/mission_kernel_lifecycle_test.cpp)
target_link_libraries(mission_kernel_lifecycle_test PRIVATE opennova_runtime)
target_include_directories(mission_kernel_lifecycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_kernel_lifecycle_test PRIVATE cxx_std_17)
add_test(NAME mission_kernel_lifecycle COMMAND mission_kernel_lifecycle_test)

# inmatch::listen_host over the kernel (ADR 0042 d3), ungated: the SP listen
# bring-up and its per-tick frame over a null socket, the local C2S gameplay
# drain, and the dedicated (HostOnly) bring-up.
add_executable(listen_host_test mission/listen_host_test.cpp)
target_link_libraries(listen_host_test PRIVATE opennova_net opennova_runtime)
target_include_directories(listen_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(listen_host_test PRIVATE cxx_std_17)
add_test(NAME listen_host COMMAND listen_host_test)

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

add_executable(gate_probe_test novaworld/gate_probe_test.cpp)
target_link_libraries(gate_probe_test PRIVATE opennova_net)
target_include_directories(gate_probe_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gate_probe COMMAND gate_probe_test)

add_executable(lobby_update_test novaworld/lobby_update_test.cpp)
target_link_libraries(lobby_update_test PRIVATE opennova_novaworld_service)
target_include_directories(lobby_update_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME lobby_update COMMAND lobby_update_test)

add_executable(gate_server_loopback_test novaworld/gate_server_loopback_test.cpp)
target_link_libraries(gate_server_loopback_test PRIVATE opennova_net opennova_app_common)
target_include_directories(gate_server_loopback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gate_server_loopback COMMAND gate_server_loopback_test)

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

# Retail dispatcher semantics (D-NET-190..193): SVRS accumulation, the gated
# "GSB " reset, undersized-record skips, and the XXXX-terminator hardening.
add_executable(gsb_retail_semantics_test novaworld/gsb_retail_semantics_test.cpp)
target_link_libraries(gsb_retail_semantics_test PRIVATE opennova_net)
target_include_directories(gsb_retail_semantics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gsb_retail_semantics COMMAND gsb_retail_semantics_test)

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

add_executable(net_ports_test npwire/net_ports_test.cpp)
target_link_libraries(net_ports_test PRIVATE opennova_net)
target_include_directories(net_ports_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME net_ports COMMAND net_ports_test)

add_executable(game_type_policy_test npwire/game_type_policy_test.cpp)
target_link_libraries(game_type_policy_test PRIVATE opennova_net)
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
add_executable(nw_host_register_e2e_test
    novaworld/nw_host_register_e2e_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/nw_udp_listener.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/server_config.cpp)
target_link_libraries(nw_host_register_e2e_test
    PRIVATE opennova_novaworld_service opennova_net opennova_app_common)
target_include_directories(nw_host_register_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
if(WIN32)
    target_link_libraries(nw_host_register_e2e_test PRIVATE ws2_32)
endif()
add_test(NAME nw_host_register_e2e COMMAND nw_host_register_e2e_test)
# This is the only test that drives the complete listener lifecycle through
# real UDP sockets. Its one-second negative/retry receive windows are protocol
# assertions, not load-test budgets, so CPU-saturated parallel ctest runs can
# manufacture packet timeouts that never reproduce in isolation.
set_tests_properties(nw_host_register_e2e PROPERTIES RUN_SERIAL TRUE)

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
    PRIVATE opennova_net opennova_app_common opennova_base)
target_include_directories(nw_pool_decode_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_pool_decode_unit COMMAND nw_pool_decode_unit_test)

# CaptureDecoder streaming-equivalence — pins that a sequence of push() calls
# yields byte-identical InGameMessages to one batch decode_capture_to_messages()
# (the contract the in-engine live net client relies on) over an inline
# two-session capture.
add_executable(nw_capture_decoder_test novaworld/nw_capture_decoder_test.cpp)
target_link_libraries(nw_capture_decoder_test
    PRIVATE opennova_net opennova_app_common opennova_base)
target_include_directories(nw_capture_decoder_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_capture_decoder COMMAND nw_capture_decoder_test)

# Streaming pcap reader — stream_pcap_udp_file must match the whole-file
# read_pcap_udp byte-for-byte and honor early-exit (so capture consumers can
# stream multi-GB captures without loading them into RAM). No fixtures, CI.
add_executable(nw_pcap_stream_test novaworld/nw_pcap_stream_test.cpp)
target_link_libraries(nw_pcap_stream_test PRIVATE opennova_app_common opennova_base)
target_include_directories(nw_pcap_stream_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_pcap_stream COMMAND nw_pcap_stream_test)

add_executable(nw_pcap_writer_test novaworld/nw_pcap_writer_test.cpp)
target_link_libraries(nw_pcap_writer_test PRIVATE opennova_app_common opennova_base)
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
# ADR 0011 §4. encode side ports serialize_entity_pool_to_packet @0x503460; the
# decode side is verified vs NapiNPClientMsg_0x020 @0x425C00. No capture dep.
add_executable(nw_ingame_encode_test novaworld/nw_ingame_encode_test.cpp)
target_link_libraries(nw_ingame_encode_test PRIVATE opennova_net)
target_include_directories(nw_ingame_encode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_encode COMMAND nw_ingame_encode_test)

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
    PRIVATE opennova_net opennova_runtime opennova_app_common opennova_base)
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

# Expansion-release / publish repository writes (the SQL ported from onnet).
# Compiles catalog_repository.cpp straight in — no Crow/libcurl needed, the
# write layer depends only on the sqlite wrapper in opennova_novaworld_service.
add_executable(expansion_publish_test
    novaworld/expansion_publish_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/catalog_repository.cpp)
target_link_libraries(expansion_publish_test PRIVATE opennova_novaworld_service)
target_include_directories(expansion_publish_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(expansion_publish_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME expansion_publish COMMAND expansion_publish_test)

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
target_link_libraries(netsim_loopback_channel_test PRIVATE opennova_net)
add_test(NAME netsim_loopback_channel COMMAND netsim_loopback_channel_test)

add_executable(netsim_item_replication_catalog_test
    netsim/item_replication_catalog_test.cpp)
target_link_libraries(netsim_item_replication_catalog_test PRIVATE opennova_net opennova_formats)
add_test(NAME netsim_item_replication_catalog
    COMMAND netsim_item_replication_catalog_test)

# The ADR 0011 §4 loopback identity guard: World entity -> encode -> loopback ->
# decode -> ClientState, field-identical to what the host's encoder + codec produced.
add_executable(netsim_loopback_identity_test netsim/loopback_identity_test.cpp)
target_link_libraries(netsim_loopback_identity_test PRIVATE opennova_net)
add_test(NAME netsim_loopback_identity COMMAND netsim_loopback_identity_test)

# A header-only retail join has no local BMS body to promote. The decoded
# 0x10/0x0D/0x20 stream must create exact pool/slot world rows for native
# collision, deploy, and vehicle-seat consumers.
add_executable(netsim_client_world_materializer_test
    netsim/client_world_materializer_test.cpp)
target_link_libraries(netsim_client_world_materializer_test PRIVATE opennova_net)
add_test(NAME netsim_client_world_materializer
    COMMAND netsim_client_world_materializer_test)

# The D-NET-196 between-update mover contract: the joiner-side per-class chase
# (net-re §5.38e) keeps remote entities gliding across the host's subrate 0x0A
# cadence — smoothness legs, the witnessed snap/deadband/respawn edges, exact
# bucket-ladder pins, and the honest chase-only fast-vehicle sustain limit
# (the family prediction legs live world-side; watercraft_client_motor_test).
add_executable(netsim_remote_motion_smoothness_test netsim/remote_motion_smoothness_test.cpp)
target_link_libraries(netsim_remote_motion_smoothness_test PRIVATE opennova_net)
add_test(NAME netsim_remote_motion_smoothness COMMAND netsim_remote_motion_smoothness_test)

# The co-op-LAN fan-out guard: one World snapshot fanned to a connection table (loopback
# self + a socket-free UdpSessionTransport joiner), per-connection anchoring, and the
# host-side C2S 0x0C drain/SNAP — no Godot, no OS sockets (Increment A).
add_executable(netsim_two_peer_fanout_test netsim/two_peer_fanout_test.cpp)
target_link_libraries(netsim_two_peer_fanout_test PRIVATE opennova_net)
add_test(NAME netsim_two_peer_fanout COMMAND netsim_two_peer_fanout_test)

add_executable(netsim_build_player_uplink_test netsim/build_player_uplink_test.cpp)
target_link_libraries(netsim_build_player_uplink_test PRIVATE opennova_net)
add_test(NAME netsim_build_player_uplink COMMAND netsim_build_player_uplink_test)

# The §5.10b decode-dispatch guard: the replica pipeline's items.def class resolver sizes
# 0x0A tag-1 records (header-only no-callback vs vehicle/infantry compacts) and
# outranks the 0x0D pool-blanket learner — a mis-sized record desyncs the frame and
# scatters every later record around the recipient anchor.
add_executable(netsim_client_replica_pipeline_arms_dip_test
    netsim/client_replica_pipeline_arms_dip_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_arms_dip_test PRIVATE opennova_net)
add_test(NAME netsim_client_replica_pipeline_arms_dip
    COMMAND netsim_client_replica_pipeline_arms_dip_test)

add_executable(netsim_client_replica_pipeline_world_page_atomicity_test
    netsim/client_replica_pipeline_world_page_atomicity_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_world_page_atomicity_test PRIVATE opennova_net)
add_test(NAME netsim_client_replica_pipeline_world_page_atomicity
    COMMAND netsim_client_replica_pipeline_world_page_atomicity_test)

add_executable(netsim_client_replica_pipeline_recoil_test
    netsim/client_replica_pipeline_recoil_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_recoil_test PRIVATE opennova_net)
add_test(NAME netsim_client_replica_pipeline_recoil
    COMMAND netsim_client_replica_pipeline_recoil_test)

add_executable(netsim_client_replica_pipeline_occupant_backref_test
    netsim/client_replica_pipeline_occupant_backref_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_occupant_backref_test PRIVATE opennova_net)
add_test(NAME netsim_client_replica_pipeline_occupant_backref
    COMMAND netsim_client_replica_pipeline_occupant_backref_test)

add_executable(netsim_client_replica_pipeline_target_carrier_follow_test
    netsim/client_replica_pipeline_target_carrier_follow_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_target_carrier_follow_test
    PRIVATE opennova_net)
add_executable(netsim_client_replica_pipeline_contact_resolver_test
    netsim/client_replica_pipeline_contact_resolver_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_contact_resolver_test PRIVATE opennova_net)
add_test(NAME netsim_client_replica_pipeline_contact_resolver
    COMMAND netsim_client_replica_pipeline_contact_resolver_test)
add_test(NAME netsim_client_replica_pipeline_target_carrier_follow
    COMMAND netsim_client_replica_pipeline_target_carrier_follow_test)

add_executable(netsim_client_replica_pipeline_class_resolver_test
    netsim/client_replica_pipeline_class_resolver_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_class_resolver_test PRIVATE opennova_net)
add_test(NAME netsim_client_replica_pipeline_class_resolver
    COMMAND netsim_client_replica_pipeline_class_resolver_test)

add_executable(netsim_client_replica_pipeline_anim_pulse_test
    netsim/client_replica_pipeline_anim_pulse_test.cpp)
target_link_libraries(netsim_client_replica_pipeline_anim_pulse_test PRIVATE opennova_net)
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
target_link_libraries(netsim_world_stream_extractors_test PRIVATE opennova_net)
add_test(NAME netsim_world_stream_extractors COMMAND netsim_world_stream_extractors_test)

# npruntime tests — the IDA-faithful in-match NAPI NP runtime (the named server/client
# state machines + frame loop). Server-first (the plan's P0..P8).
# P0/P1: the in-process listen-server bring-up state writes (§5.0).
add_executable(npruntime_server_session_test npruntime/server_session_test.cpp)
target_link_libraries(npruntime_server_session_test PRIVATE opennova_net)
add_test(NAME npruntime_server_session COMMAND npruntime_server_session_test)

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
add_executable(npruntime_joiner_world_bridge_test npruntime/joiner_world_bridge_test.cpp)
target_link_libraries(npruntime_joiner_world_bridge_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_joiner_world_bridge COMMAND npruntime_joiner_world_bridge_test)

# The joiner session-drive policy (S10b): the two 0xEA60 reachable-analog windows, the S2C 0x7B
# promote validation, the admission/deploy/loss edge machine (frame ORDERING locked: loss > abort >
# settle > window > edges), and the D-NET-178 expansion reconcile decision.
add_executable(npruntime_join_session_policy_test npruntime/join_session_policy_test.cpp)
target_link_libraries(npruntime_join_session_policy_test PRIVATE opennova_net)
add_test(NAME npruntime_join_session_policy COMMAND npruntime_join_session_policy_test)

# ADR 0013: the shared byte-budget spawn/world-stream chunker (np::slice_batch_pages) the §5.2a burst
# pages every world pool through — retail pool margins, exact tile pages, oversized record, cursor resume.
add_executable(npruntime_batch_chunker_test npruntime/batch_chunker_test.cpp)
target_link_libraries(npruntime_batch_chunker_test PRIVATE opennova_net)
add_test(NAME npruntime_batch_chunker COMMAND npruntime_batch_chunker_test)

# D-NET-141: the weapon.def armory table — witnessed index allocation (null@0 + file order),
# ammo-clip resolution, and charfilter/teamfilter masks, against the committed fixture; §5.57.
add_executable(npruntime_weapon_table_test npruntime/weapon_table_test.cpp)
target_link_libraries(npruntime_weapon_table_test
    PRIVATE opennova_net opennova_runtime opennova_formats opennova_base)
target_include_directories(npruntime_weapon_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_weapon_table COMMAND npruntime_weapon_table_test)

# D-NET-168 / D-NET-176 / D-NET-177: the S2C 0x50 team-assign re-latch + 0x2F re-submit, the
# C2S 0x32 -> S2C 0x5D empty-slot sweep (entity removal) vs the bookkeeping-only 0x46 bit15, and
# the in-match session-loss surface [orig: @0x431910 / @0x51a600 + @0x429730 / @0x4ca4a0 -> @0x4c63d0].
add_executable(npruntime_entity_lifecycle_net_test npruntime/entity_lifecycle_net_test.cpp)
target_link_libraries(npruntime_entity_lifecycle_net_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_entity_lifecycle_net COMMAND npruntime_entity_lifecycle_net_test)

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
# refill), and the g_round_ring append behind the §5.9.1 tag-2 echo
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
        world/truck_rest_00tra world/rock_collision_00trg world/ai_corpse world/ai_threat
        world/ai_muzzle_pose
        world/ladder_00tra mission/defense_00trg mission/vehicle_ride_00tra mission/soak_00trg
        mission/native_assets_00trg mission/lose_flow_04tr)
    get_filename_component(port_name ${port} NAME)
    add_executable(${port_name}_test ${port}_test.cpp)
    target_link_libraries(${port_name}_test PRIVATE opennova_test_rig)
    opennova_add_gated_test(${port_name} COMMAND ${port_name}_test)
endforeach()

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
add_test(NAME npruntime_handshake_server COMMAND npruntime_handshake_server_test)

# App-layer readiness gate used by the automated retail interop harness: send
# the shared retail 0x41 probe over a real loopback socket, accept only a valid
# game-server 0x81, and retain a bounded no-responder timeout.
add_executable(npruntime_lan_probe_client_test npruntime/lan_probe_client_test.cpp)
target_link_libraries(npruntime_lan_probe_client_test
    PRIVATE opennova_lan_probe_client opennova_net opennova_app_common)
add_test(NAME npruntime_lan_probe_client COMMAND npruntime_lan_probe_client_test)

# P5 — the headless CLIENT runtime (np::ClientRuntime / Client_ProcessNetworkFrame). Always-on
# in-process round-trip (client_runtime <-> np server legs <-> Server_TickUpdate + apply_in_match_c2s)
# plus the host-as-client D-NET-121/122 anchor/fold path. No capture needed.
add_executable(npruntime_client_runtime_test npruntime/client_runtime_test.cpp)
target_link_libraries(npruntime_client_runtime_test PRIVATE opennova_net opennova_runtime)
target_include_directories(npruntime_client_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}) # common/null_datagram_socket.h
add_test(NAME npruntime_client_runtime COMMAND npruntime_client_runtime_test)

# D-NET-194: a joiner retains the host's exact S2C 0x0B BMS header and
# reassembles the paged S2C 0x45 terrain-tile stream for wire-driven loading.
add_executable(npruntime_join_wire_assets_test npruntime/join_wire_assets_test.cpp)
target_link_libraries(npruntime_join_wire_assets_test PRIVATE opennova_net)
add_test(NAME npruntime_join_wire_assets COMMAND npruntime_join_wire_assets_test)

# Retail in-match loss recovery: future 0x43/0x83 packets trigger direction-specific 0x84/0x44
# missing-sequence lists, and each endpoint reconstructs retained reliable records under the old
# sequence with its current ACK. [orig: SendMissingSeqList @0x623560; HandleResendList @0x623800]
add_executable(npruntime_session_resend_test npruntime/session_resend_test.cpp)
target_link_libraries(npruntime_session_resend_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_session_resend COMMAND npruntime_session_resend_test)

# The host-initiated close (the connection-description record, settings flag + tag 3) pinned against
# the 80 captured bytes of a live retail deploy-screen idle punt, plus the S2C 0x30/0x31 anti-cheat
# CRC challenges a stock host streams at every joiner. [orig: Server_LogCRCMismatchPunt @0x517ed0 ->
# NapiNPDataTransfer_SendDescription @0x628c80; CNapiNPConnection_HandleDescriptionPacket @0x621ae0;
# NapiNPClientMsg_HandleChecksumRequest @0x431170; NapiNPClientMsg_0x031 @0x4311e0]
add_executable(npruntime_host_punt_test npruntime/host_punt_test.cpp)
target_link_libraries(npruntime_host_punt_test PRIVATE opennova_net opennova_runtime)
add_test(NAME npruntime_host_punt COMMAND npruntime_host_punt_test)

# Loss-tolerant pre-session UDP join: dropped 0x41/0x42 datagrams are retried on the retail active
# send interval without changing their wire bytes, and the retried legs complete against the real host.
add_executable(npruntime_joiner_handshake_retry_test npruntime/joiner_handshake_retry_test.cpp)
target_link_libraries(npruntime_joiner_handshake_retry_test PRIVATE opennova_net opennova_runtime)
target_include_directories(npruntime_joiner_handshake_retry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_joiner_handshake_retry COMMAND npruntime_joiner_handshake_retry_test)

# P6 — two endpoints over REAL UDP sockets: a ClientRuntime joiner and the apps/nw_server host
# owner-loop (npruntime/host_session.h, the SAME loop apps/nw_server runs), each on its own bound UDP
# over the wire (single-thread poll-pump). Asserts mirror npruntime_client_runtime but over real
# sockets, and adds the §5.2a S2C tag-order check (always-on, our host). opennova_runtime for the
# minimal World/bms; opennova_app_common for net_sockets + net_datagram_socket (ws2_32 on WIN32).
add_executable(npruntime_two_endpoint_socket_test npruntime/two_endpoint_socket_test.cpp)
target_include_directories(npruntime_two_endpoint_socket_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(npruntime_two_endpoint_socket_test
    PRIVATE opennova_net opennova_runtime opennova_app_common opennova_base)
if(WIN32)
    target_link_libraries(npruntime_two_endpoint_socket_test PRIVATE ws2_32)
endif()
add_test(NAME npruntime_two_endpoint_socket COMMAND npruntime_two_endpoint_socket_test)

# weapon.sav player-profile format tests (engine/formats/playersav).
add_subdirectory(playersav)

# The in-engine dev tools (ADR 0039, engine/runtime/devtools). The frame-stats
# board, the ImGui pass and ONED's surface are unconditional; the game-window
# test needs them compiled in (OPENNOVA_DEVTOOLS, the root default). The ImGui
# tests drive a null backend.
add_executable(frame_stats_board_test devtools/frame_stats_board_test.cpp)
target_link_libraries(frame_stats_board_test PRIVATE opennova_runtime)
add_test(NAME frame_stats_board COMMAND frame_stats_board_test)
add_executable(oned_ui_test devtools/oned_ui_test.cpp)
target_link_libraries(oned_ui_test PRIVATE opennova_runtime opennova_imgui)
add_test(NAME oned_ui COMMAND oned_ui_test)
if(OPENNOVA_DEVTOOLS)
    add_executable(devtools_test devtools/devtools_test.cpp)
    target_link_libraries(devtools_test PRIVATE opennova_runtime opennova_imgui)
    add_test(NAME devtools COMMAND devtools_test)
endif()
