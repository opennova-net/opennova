# Shared io infrastructure unit tests
add_executable(io_test io/io_test.cpp)
target_link_libraries(io_test PRIVATE opennova_io)
target_compile_features(io_test PRIVATE cxx_std_17)
target_include_directories(io_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME io COMMAND io_test)

# Roundtrip test
add_executable(threedi_roundtrip_test threedi/roundtrip_test.cpp)
target_link_libraries(threedi_roundtrip_test PRIVATE opennova_threedi)
target_include_directories(threedi_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_roundtrip COMMAND threedi_roundtrip_test)

add_executable(threedi_compare_test threedi/compare_test.cpp)
target_link_libraries(threedi_compare_test PRIVATE opennova_threedi)
target_include_directories(threedi_compare_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_compare COMMAND threedi_compare_test)

# GP format parsing test
add_executable(threedi_gp_parse_test threedi/gp_parse_test.cpp)
target_link_libraries(threedi_gp_parse_test PRIVATE opennova_threedi)
target_include_directories(threedi_gp_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_gp_parse COMMAND threedi_gp_parse_test)

# GP format roundtrip test
add_executable(threedi_gp_roundtrip_test threedi/gp_roundtrip_test.cpp)
target_link_libraries(threedi_gp_roundtrip_test PRIVATE opennova_threedi)
target_include_directories(threedi_gp_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_gp_roundtrip COMMAND threedi_gp_roundtrip_test)

# Retail global control-register catalog.
add_executable(threedi_ctrl_catalog_test threedi/ctrl_catalog_test.cpp)
target_link_libraries(threedi_ctrl_catalog_test PRIVATE opennova_threedi)
target_include_directories(threedi_ctrl_catalog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_ctrl_catalog COMMAND threedi_ctrl_catalog_test)

# PANM unit tests
foreach(test ctrl decode runtime wave)
    add_executable(threedi_panm_${test}_test threedi/panm_${test}_test.cpp)
    target_link_libraries(threedi_panm_${test}_test PRIVATE opennova_threedi)
    target_include_directories(threedi_panm_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME threedi_panm_${test} COMMAND threedi_panm_${test}_test)
endforeach()

# Placement ground-anchor resolution (ground userpoint, else part-0 bounding center)
add_executable(threedi_ground_anchor_test threedi/ground_anchor_test.cpp)
target_link_libraries(threedi_ground_anchor_test PRIVATE opennova_threedi)
target_include_directories(threedi_ground_anchor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_ground_anchor COMMAND threedi_ground_anchor_test)

add_executable(threedi_userpoint_ir_test threedi/userpoint_ir_test.cpp)
target_link_libraries(threedi_userpoint_ir_test PRIVATE opennova_threedi)
target_include_directories(threedi_userpoint_ir_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_userpoint_ir COMMAND threedi_userpoint_ir_test)

add_executable(threedi_collision_ir_test threedi/collision_ir_test.cpp)
target_link_libraries(threedi_collision_ir_test PRIVATE opennova_threedi)
target_include_directories(threedi_collision_ir_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_collision_ir COMMAND threedi_collision_ir_test)

# ADM tests
foreach(test comment trim_value parse write variants)
    add_executable(adm_${test}_test anim/adm_${test}_test.cpp)
    target_link_libraries(adm_${test}_test PRIVATE opennova_anim)
    target_include_directories(adm_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME adm_${test} COMMAND adm_${test}_test)
endforeach()

# BAD tests
add_executable(bad_parse_test bad/bad_parse_test.cpp)
target_link_libraries(bad_parse_test PRIVATE opennova_bad)
target_include_directories(bad_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bad_parse COMMAND bad_parse_test)

add_executable(bad_roundtrip_test bad/bad_roundtrip_test.cpp)
target_link_libraries(bad_roundtrip_test PRIVATE opennova_bad)
target_include_directories(bad_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bad_roundtrip COMMAND bad_roundtrip_test)

# ANIM skeletal sampler (libs/anim): coordinate conventions, quaternion helpers, and
# world<->local self-consistency on the real BINOC.bad fixture.
add_executable(anim_sample_test anim/anim_sample_test.cpp)
target_link_libraries(anim_sample_test PRIVATE opennova_anim opennova_bad)
target_include_directories(anim_sample_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(anim_sample_test PRIVATE cxx_std_17)
add_test(NAME anim_sample COMMAND anim_sample_test)

# ANIM aim overlay (the torso bend): bone-class map, BAM blend branches, compose
# invariants. [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md s14]
add_executable(aim_overlay_test anim/aim_overlay_test.cpp)
target_link_libraries(aim_overlay_test PRIVATE opennova_anim)
target_include_directories(aim_overlay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(aim_overlay_test PRIVATE cxx_std_17)
add_test(NAME aim_overlay COMMAND aim_overlay_test)

# DEF tests
add_executable(def_comment_strip_test def/def_comment_strip_test.cpp)
target_link_libraries(def_comment_strip_test PRIVATE opennova_def)
target_include_directories(def_comment_strip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME def_comment_strip COMMAND def_comment_strip_test)

add_executable(def_flag_parity_test def/def_flag_parity_test.cpp)
target_link_libraries(def_flag_parity_test PRIVATE opennova_def)
add_test(NAME def_flag_parity COMMAND def_flag_parity_test)

foreach(test items weapons ammo hudpos item_attrib item_type)
    add_executable(def_parse_${test}_test def/def_parse_${test}_test.cpp)
    target_link_libraries(def_parse_${test}_test PRIVATE opennova_def)
    target_include_directories(def_parse_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME def_parse_${test} COMMAND def_parse_${test}_test)
endforeach()

# PLAYER_INFO loadout weight + encumbrance math (D-PLAYERINFO-11 weight readout).
add_executable(def_loadout_weight_test def/def_loadout_weight_test.cpp)
target_link_libraries(def_loadout_weight_test PRIVATE opennova_def)
add_test(NAME def_loadout_weight COMMAND def_loadout_weight_test)

# AVATARS (Avatars.def) tests: parse against the retail fixture + from-scratch
# writer round-trip (docs/playerinfo/avatars-re.md, docs/adr/0021).
foreach(test parse roundtrip)
    add_executable(avatars_${test}_test avatars/avatars_${test}_test.cpp)
    target_link_libraries(avatars_${test}_test PRIVATE opennova_avatars)
    target_include_directories(avatars_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(avatars_${test}_test PRIVATE cxx_std_17)
    add_test(NAME avatars_${test} COMMAND avatars_${test}_test)
endforeach()

# Mission (BMS) tests
add_executable(mission_authoring_test mission/mission_authoring_test.cpp)
target_link_libraries(mission_authoring_test PRIVATE opennova_mission_format opennova_def)
target_include_directories(mission_authoring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_authoring COMMAND mission_authoring_test)

add_executable(mission_bms_test mission/mission_bms_test.cpp)
target_link_libraries(mission_bms_test PRIVATE opennova_mission_format)
target_include_directories(mission_bms_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_bms COMMAND mission_bms_test)

add_executable(mission_mis_test mission/mission_mis_test.cpp)
target_link_libraries(mission_mis_test PRIVATE opennova_mission_format)
target_include_directories(mission_mis_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_mis_test PRIVATE cxx_std_17)
add_test(NAME mission_mis COMMAND mission_mis_test)

# Retail-fixture .mis idempotency: .bms -> .mis (gen1) -> parse -> .mis (gen2), byte-equal.
add_executable(mission_mis_idempotency_test mission/mission_mis_idempotency_test.cpp)
target_link_libraries(mission_mis_idempotency_test PRIVATE opennova_mission_format)
target_include_directories(mission_mis_idempotency_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_mis_idempotency_test PRIVATE cxx_std_17)
add_test(NAME mission_mis_idempotency COMMAND mission_mis_idempotency_test)

add_executable(mission_c_abi_test mission/mission_c_abi_test.cpp)
target_link_libraries(mission_c_abi_test PRIVATE opennova_mission_format)
target_include_directories(mission_c_abi_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_c_abi COMMAND mission_c_abi_test)

add_executable(mission_schema_test mission/mission_schema_test.cpp)
target_link_libraries(mission_schema_test PRIVATE opennova_mission_format)
target_include_directories(mission_schema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_schema COMMAND mission_schema_test)

# Corpus round-trip: parse->write byte-exact over a directory of real shipped .bms missions.
# Gated on OPENNOVA_MISSION_CORPUS (copyrighted assets, not committed); skips + passes when unset.
add_executable(mission_corpus_test mission/mission_corpus_test.cpp)
target_link_libraries(mission_corpus_test PRIVATE opennova_mission_format)
target_include_directories(mission_corpus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mission_corpus_test PRIVATE cxx_std_17)
add_test(NAME mission_corpus COMMAND mission_corpus_test)

# LWF sound-profile tests: byte-exact parse->encode round-trip + mutate-reread
# over real JO fixtures (fixtures/lwf/00TRa.LWF + gamelocl.LWF).
add_executable(lwf_roundtrip_test lwf/lwf_roundtrip_test.cpp)
target_link_libraries(lwf_roundtrip_test PRIVATE opennova_lwf)
target_include_directories(lwf_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME lwf_roundtrip COMMAND lwf_roundtrip_test)

# The menu sound profile (fixtures/menu_sound/menu.LWF): trigger sets resolve to
# loose .wav members (drives MNU hover/click via NovaMnuMenu).
add_executable(menu_lwf_test lwf/menu_lwf_test.cpp)
target_link_libraries(menu_lwf_test PRIVATE opennova_lwf)
target_include_directories(menu_lwf_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME menu_lwf COMMAND menu_lwf_test)

# DBF dialog-bank tests: byte-exact round-trip + dlg-id -> def-id mapping over a
# real JO fixture (fixtures/dbf/00TRg.DBF).
add_executable(dbf_roundtrip_test dbf/dbf_roundtrip_test.cpp)
target_link_libraries(dbf_roundtrip_test PRIVATE opennova_dbf)
target_include_directories(dbf_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dbf_roundtrip COMMAND dbf_roundtrip_test)

# Audio sound-set member selection state machine (FIRST/RANDOM/SEQUENTIAL/RANDOM_SEQ),
# pushed down from nova_sound_bank.gd into libs/audio.
add_executable(sound_selector_test audio/sound_selector_test.cpp)
target_link_libraries(sound_selector_test PRIVATE opennova_audio)
target_include_directories(sound_selector_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sound_selector_test PRIVATE cxx_std_17)
add_test(NAME sound_selector COMMAND sound_selector_test)

# Ambient marker emitter system: the staggered tick&7 registration cadence, tick-unit
# slot keep-alives, and the per-frame live-slot mix, pushed down from
# nova_mission_audio.gd (docs/audio/lwf-dbf-sound-re.md §driver cadence, D-SND-16).
add_executable(ambient_mixer_test audio/ambient_mixer_test.cpp)
target_link_libraries(ambient_mixer_test PRIVATE opennova_audio)
target_include_directories(ambient_mixer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ambient_mixer_test PRIVATE cxx_std_17)
add_test(NAME ambient_mixer COMMAND ambient_mixer_test)

# SndProf.def sound-profile parse + lookup (asset-gated retail spot check via JO_ASSETS).
add_executable(sound_profile_test audio/sound_profile_test.cpp)
target_link_libraries(sound_profile_test PRIVATE opennova_audio)
target_include_directories(sound_profile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sound_profile_test PRIVATE cxx_std_17)
add_test(NAME sound_profile COMMAND sound_profile_test)

# PFF tests
add_executable(pff_unit_test pff/pff_unit_test.cpp)
target_link_libraries(pff_unit_test PRIVATE opennova_pff)
target_include_directories(pff_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pff_unit COMMAND pff_unit_test)

# Game profile table tests
add_executable(gameprofile_test gameprofile/gameprofile_test.c)
target_link_libraries(gameprofile_test PRIVATE opennova_gameprofile)
target_include_directories(gameprofile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gameprofile COMMAND gameprofile_test)

add_executable(required_resources_test gameprofile/required_resources_test.c)
target_link_libraries(required_resources_test PRIVATE opennova_gameprofile)
add_test(NAME required_resources COMMAND required_resources_test)

# SBF tests
add_executable(sbf_header_test sbf/sbf_header_test.cpp)
target_link_libraries(sbf_header_test PRIVATE opennova_sbf)
target_include_directories(sbf_header_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sbf_header COMMAND sbf_header_test)

add_executable(sbf_parse_test sbf/sbf_parse_test.cpp)
target_link_libraries(sbf_parse_test PRIVATE opennova_sbf)
target_include_directories(sbf_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sbf_parse_test PRIVATE
    SBF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/sbf")
add_test(NAME sbf_parse COMMAND sbf_parse_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(sbf_decode_test sbf/sbf_decode_test.cpp)
target_link_libraries(sbf_decode_test PRIVATE opennova_sbf)
target_include_directories(sbf_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sbf_decode_test PRIVATE
    SBF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/sbf")
add_test(NAME sbf_decode COMMAND sbf_decode_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(sbf_compat_test sbf/sbf_compat_test.cpp)
target_link_libraries(sbf_compat_test PRIVATE opennova_sbf)
target_include_directories(sbf_compat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sbf_compat COMMAND sbf_compat_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(sbf_encode_test sbf/sbf_encode_test.cpp)
target_link_libraries(sbf_encode_test PRIVATE opennova_sbf)
target_include_directories(sbf_encode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sbf_encode_test PRIVATE
    SBF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/sbf")
if(NOT MSVC)
    target_link_libraries(sbf_encode_test PRIVATE m)
endif()
add_test(NAME sbf_encode COMMAND sbf_encode_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(sbf_roundtrip_test sbf/sbf_roundtrip_test.cpp)
target_link_libraries(sbf_roundtrip_test PRIVATE opennova_sbf)
target_include_directories(sbf_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sbf_roundtrip_test PRIVATE
    SBF_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/sbf")
if(NOT MSVC)
    target_link_libraries(sbf_roundtrip_test PRIVATE m)
endif()
add_test(NAME sbf_roundtrip COMMAND sbf_roundtrip_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# MUS tests
add_executable(mus_header_test mus/mus_header_test.cpp)
target_link_libraries(mus_header_test PRIVATE opennova_mus)
target_include_directories(mus_header_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mus_header COMMAND mus_header_test)

add_executable(mus_parse_test mus/mus_parse_test.cpp)
target_link_libraries(mus_parse_test PRIVATE opennova_mus)
target_include_directories(mus_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_parse_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_parse COMMAND mus_parse_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_compat_test mus/mus_compat_test.cpp)
target_link_libraries(mus_compat_test PRIVATE opennova_mus)
target_include_directories(mus_compat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_compat_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_compat COMMAND mus_compat_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_decompile_test mus/mus_decompile_test.cpp)
target_link_libraries(mus_decompile_test PRIVATE opennova_mus)
target_include_directories(mus_decompile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_decompile_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_decompile COMMAND mus_decompile_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_compile_test mus/mus_compile_test.cpp)
target_link_libraries(mus_compile_test PRIVATE opennova_mus)
target_include_directories(mus_compile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mus_compile COMMAND mus_compile_test)

add_executable(mus_roundtrip_test mus/mus_roundtrip_test.cpp)
target_link_libraries(mus_roundtrip_test PRIVATE opennova_mus)
target_include_directories(mus_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_roundtrip_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_roundtrip COMMAND mus_roundtrip_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_names_roundtrip_test mus/mus_names_roundtrip_test.cpp)
target_link_libraries(mus_names_roundtrip_test PRIVATE opennova_mus)
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
target_link_libraries(mus_entry_roundtrip_test PRIVATE opennova_mus)
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
target_link_libraries(mus_authored_behavioral_test PRIVATE opennova_mus)
target_include_directories(mus_authored_behavioral_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mus_authored_behavioral_test PRIVATE cxx_std_17)
add_test(NAME mus_authored_behavioral COMMAND mus_authored_behavioral_test)

add_executable(mus_ast_test mus/mus_ast_test.cpp)
target_link_libraries(mus_ast_test PRIVATE opennova_mus)
target_include_directories(mus_ast_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mus_ast_test PRIVATE cxx_std_17)
target_compile_definitions(mus_ast_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_ast COMMAND mus_ast_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_expr_tree_test mus/mus_expr_tree_test.cpp)
target_link_libraries(mus_expr_tree_test PRIVATE opennova_mus)
target_include_directories(mus_expr_tree_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mus_expr_tree_test PRIVATE cxx_std_17)
target_compile_definitions(mus_expr_tree_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_expr_tree COMMAND mus_expr_tree_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_vm_test mus/mus_vm_test.cpp)
target_link_libraries(mus_vm_test PRIVATE opennova_mus)
target_include_directories(mus_vm_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_vm_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_vm COMMAND mus_vm_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mus_vm_fixes_test mus/mus_vm_fixes_test.cpp)
target_link_libraries(mus_vm_fixes_test PRIVATE opennova_mus)
target_include_directories(mus_vm_fixes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mus_vm_fixes COMMAND mus_vm_fixes_test)

add_executable(mus_model_test mus/mus_model_test.cpp)
target_link_libraries(mus_model_test PRIVATE opennova_mus)
target_include_directories(mus_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_model_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_model COMMAND mus_model_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Write-path parity: the canonical re-encoded form is a byte-stable fixed point
# (byte-identity to the ORIGINAL .bin is impossible -- editor debug info is not
# reproduced). Gates the structured-edit write path; see the test file header.
add_executable(mus_encode_idempotence_test mus/mus_encode_idempotence_test.cpp)
target_link_libraries(mus_encode_idempotence_test PRIVATE opennova_mus)
target_include_directories(mus_encode_idempotence_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mus_encode_idempotence_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_encode_idempotence COMMAND mus_encode_idempotence_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Structured play-edit parity: splicing a play into a section's decompiled text
# adds exactly one model play with unchanged edges, and a no-op insert/remove is
# byte-identical. Guards the editor's drag-to-add-play write path.
add_executable(mus_structured_play_edit_test mus/mus_structured_play_edit_test.cpp)
target_link_libraries(mus_structured_play_edit_test PRIVATE opennova_mus)
target_include_directories(mus_structured_play_edit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mus_structured_play_edit_test PRIVATE cxx_std_17)
target_compile_definitions(mus_structured_play_edit_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_structured_play_edit COMMAND mus_structured_play_edit_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Structured statement + section edit parity (Phase 2 authoring): top-level
# statements are located by line span (mus_ast_emit_text_spans, the same emit the
# editor's get_annotated_decompile bridge uses) then spliced/deleted/replaced/
# reordered; section rename is bytecode-identical, referenced-delete is refused.
add_executable(mus_structured_section_edit_test mus/mus_structured_section_edit_test.cpp)
target_link_libraries(mus_structured_section_edit_test PRIVATE opennova_mus)
target_include_directories(mus_structured_section_edit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mus_structured_section_edit_test PRIVATE cxx_std_17)
target_compile_definitions(mus_structured_section_edit_test PRIVATE
    MUS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/mus")
add_test(NAME mus_structured_section_edit COMMAND mus_structured_section_edit_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# VFS tests (mount precedence, mount_game expansion override, SCR decode-on-read)
add_executable(vfs_test vfs/vfs_test.cpp)
target_link_libraries(vfs_test PRIVATE opennova_vfs opennova_pff)
target_include_directories(vfs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vfs_test PRIVATE cxx_std_17)
add_test(NAME vfs COMMAND vfs_test)

add_executable(vfs_mus_decode_test vfs/vfs_mus_decode_test.cpp)
target_link_libraries(vfs_mus_decode_test PRIVATE opennova_vfs)
target_include_directories(vfs_mus_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vfs_mus_decode_test PRIVATE cxx_std_17)
add_test(NAME vfs_mus_decode COMMAND vfs_mus_decode_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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

# RTXT localized string table tests
foreach(test roundtrip byte_equal lookup strip_hotkey empty section_lookup)
    add_executable(rtxt_${test}_test rtxt/${test}_test.cpp)
    target_link_libraries(rtxt_${test}_test PRIVATE opennova_rtxt)
    target_include_directories(rtxt_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rtxt)
    add_test(NAME rtxt_${test} COMMAND rtxt_${test}_test)
endforeach()

# Minimal authored fixture set — the fatal-set RTXT string tables
# (fixtures/minimal/). Regenerates each in memory and guards it byte-matches
# the committed file + round-trips. See fixtures/minimal/README.md.
add_executable(minimal_rtxt_gen_test fixtures/minimal_rtxt_gen.cpp)
target_link_libraries(minimal_rtxt_gen_test PRIVATE opennova_rtxt)
target_compile_definitions(minimal_rtxt_gen_test PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/minimal")
add_test(NAME minimal_rtxt_gen COMMAND minimal_rtxt_gen_test)

# Minimal set's text .def files parse through our loaders (items/weapon/ammo).
add_executable(minimal_def_validate_test fixtures/minimal_def_validate.cpp)
target_link_libraries(minimal_def_validate_test PRIVATE opennova_def)
target_compile_definitions(minimal_def_validate_test PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/minimal")
add_test(NAME minimal_def_validate COMMAND minimal_def_validate_test)

# Minimal set's menus parse (main Startup node + mp host/join screen).
add_executable(minimal_mnu_validate_test fixtures/minimal_mnu_validate.cpp)
target_link_libraries(minimal_mnu_validate_test PRIVATE opennova_mnu)
target_compile_definitions(minimal_mnu_validate_test PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/minimal")
add_test(NAME minimal_mnu_validate COMMAND minimal_mnu_validate_test)

# Minimal set's custom map — the .env + .bms legs (the .trn rides its own slice).
add_executable(minimal_map_gen_test fixtures/minimal_map_gen.cpp)
target_link_libraries(minimal_map_gen_test PRIVATE opennova_env opennova_mission_format)
target_compile_definitions(minimal_map_gen_test PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/minimal")
add_test(NAME minimal_map_gen COMMAND minimal_map_gen_test)

# Minimal map's terrain config (.trn) — round-trip guarded; the heavy .cpt/tiles
# are generate-at-package (not committed).
add_executable(minimal_trn_gen_test fixtures/minimal_trn_gen.cpp)
target_link_libraries(minimal_trn_gen_test PRIVATE opennova_trn)
target_compile_definitions(minimal_trn_gen_test PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/minimal")
add_test(NAME minimal_trn_gen COMMAND minimal_trn_gen_test)

# Minimal map's source-art images (colormap/detail/tilestrip TGAs, charmap/
# foliagemap PCX) the .trn references — authored + validated.
add_executable(minimal_art_gen_test fixtures/minimal_art_gen.cpp)
target_link_libraries(minimal_art_gen_test PRIVATE opennova_pcx)
target_compile_definitions(minimal_art_gen_test PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/minimal")
add_test(NAME minimal_art_gen COMMAND minimal_art_gen_test)

# Minimal set's generated boot font (minimal_fnt_builder.h): validity +
# byte-stable roundtrip + label glyphs drawable. Always-on; the font itself is
# emitted at package time, never committed.
add_executable(minimal_fnt_gen_test fixtures/minimal_fnt_gen.cpp)
target_link_libraries(minimal_fnt_gen_test PRIVATE opennova_fnt)
add_test(NAME minimal_fnt_gen COMMAND minimal_fnt_gen_test)

# Minimal mission's dialog companions (mnml.dbf + mnml.lwf): empty-but-valid
# via the libs/dbf + libs/lwf writers, byte-stable vs the committed files.
add_executable(minimal_companion_gen_test fixtures/minimal_companion_gen.cpp)
target_link_libraries(minimal_companion_gen_test PRIVATE opennova_dbf opennova_lwf)
target_compile_definitions(minimal_companion_gen_test PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/minimal")
add_test(NAME minimal_companion_gen COMMAND minimal_companion_gen_test)

# Minimal set's generated boot music (minimal_mus_builder.h): the silent SBF
# bank + the minimal MENUMUS/GAMEMUS scripts parse back valid. Always-on; the
# files are emitted at package time, never committed.
add_executable(minimal_mus_gen_test fixtures/minimal_mus_gen.cpp)
target_link_libraries(minimal_mus_gen_test PRIVATE opennova_mus opennova_sbf)
add_test(NAME minimal_mus_gen COMMAND minimal_mus_gen_test)

# Minimal set's packaging tool: build_terrain + generate the boot fonts +
# boot music + PFF-bundle the whole set. Skips clean unless
# OPENNOVA_BUILD_MINIMAL_PFF=1 (generate-at-package: ~35 s, ~10 MB).
add_executable(minimal_pff_package_test fixtures/minimal_pff_package.cpp)
target_link_libraries(minimal_pff_package_test PRIVATE opennova_terrain opennova_pff opennova_fnt
                                                       opennova_mus opennova_sbf)
target_compile_definitions(minimal_pff_package_test PRIVATE
    MINIMAL_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/minimal")
add_test(NAME minimal_pff_package COMMAND minimal_pff_package_test)

# RTXT parity against committed retail string tables (format invariants + byte roundtrip)
add_executable(rtxt_real_parity_test rtxt/real_parity_test.cpp)
target_link_libraries(rtxt_real_parity_test PRIVATE opennova_rtxt)
target_include_directories(rtxt_real_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rtxt)
target_compile_definitions(rtxt_real_parity_test PRIVATE
    RTXT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/rtxt")
add_test(NAME rtxt_real_parity COMMAND rtxt_real_parity_test)

# Full-install RTXT sweep through the engine-faithful VFS (skips unless OPENNOVA_JO_DIR set)
add_executable(rtxt_jo_install_sweep_test rtxt/jo_install_sweep_test.cpp)
target_link_libraries(rtxt_jo_install_sweep_test PRIVATE opennova_rtxt opennova_vfs opennova_pff)
target_include_directories(rtxt_jo_install_sweep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rtxt)
add_test(NAME rtxt_jo_install_sweep COMMAND rtxt_jo_install_sweep_test)

# Resource directory index tests
add_executable(resource_index_test resource_index/resource_index_test.cpp)
target_link_libraries(resource_index_test PRIVATE opennova_resource_index)
target_include_directories(resource_index_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME resource_index COMMAND resource_index_test)

add_executable(resource_index_mus_test resource_index/resource_index_mus_test.cpp)
target_link_libraries(resource_index_mus_test PRIVATE opennova_resource_index)
target_include_directories(resource_index_mus_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME resource_index_mus COMMAND resource_index_mus_test)

# OED object workspace/export tests
add_executable(oed_build_model_test oed/build_model_test.cpp)
target_link_libraries(oed_build_model_test PRIVATE opennova_oed opennova_threedi opennova_tdp opennova_ase)
target_include_directories(oed_build_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME oed_build_model COMMAND oed_build_model_test)

add_executable(oed_export_3di_test oed/export_3di_test.cpp)
target_link_libraries(oed_export_3di_test PRIVATE opennova_oed opennova_threedi opennova_tdp opennova_ase)
target_include_directories(oed_export_3di_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME oed_export_3di COMMAND oed_export_3di_test)

add_executable(oed_error_test oed/error_test.cpp)
target_link_libraries(oed_error_test PRIVATE opennova_oed opennova_threedi opennova_tdp opennova_ase)
target_include_directories(oed_error_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME oed_error COMMAND oed_error_test)

# Object renderer/shader tests
foreach(test light_runtime material_classify material_eval
        particle_atlas_contract particle_frame_compiler_contract render_order state_vectors)
    add_executable(renderer_${test}_test renderer/${test}_test.cpp)
    target_link_libraries(renderer_${test}_test PRIVATE opennova_renderer opennova_threedi)
    target_include_directories(renderer_${test}_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/libs/oed/include)
    target_compile_features(renderer_${test}_test PRIVATE cxx_std_17)
    add_test(NAME renderer_${test} COMMAND renderer_${test}_test)
endforeach()

# The REN-1 T1 parity instrument reads its committed golden from the source
# tree (dump mode rewrites it there — ADR 0023).
target_compile_definitions(renderer_state_vectors_test PRIVATE
    RENDER_STATE_VECTORS_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/renderer/render_state_vectors.golden")

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

add_executable(tdp_from_3di_test tdp/tdp_from_3di_test.cpp)
target_link_libraries(tdp_from_3di_test PRIVATE opennova_tdp opennova_threedi)
target_include_directories(tdp_from_3di_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tdp_from_3di COMMAND tdp_from_3di_test)

# Terrain format library tests
add_executable(fnt_roundtrip_test fnt/fnt_roundtrip_test.cpp)
target_link_libraries(fnt_roundtrip_test PRIVATE opennova_fnt)
target_include_directories(fnt_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fnt_roundtrip_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME fnt_roundtrip COMMAND fnt_roundtrip_test)

add_executable(fnt_pack_test fnt/fnt_pack_test.c)
target_link_libraries(fnt_pack_test PRIVATE opennova_fnt)
add_test(NAME fnt_pack COMMAND fnt_pack_test)

add_executable(pcx_roundtrip_test pcx/pcx_roundtrip_test.cpp)
target_link_libraries(pcx_roundtrip_test PRIVATE opennova_pcx)
target_include_directories(pcx_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pcx_roundtrip COMMAND pcx_roundtrip_test)

add_executable(til_roundtrip_test til/til_roundtrip_test.cpp)
target_link_libraries(til_roundtrip_test PRIVATE opennova_til)
target_include_directories(til_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_roundtrip COMMAND til_roundtrip_test)

add_executable(til_foliage_blocker_test til/til_foliage_blocker_test.cpp)
target_link_libraries(til_foliage_blocker_test PRIVATE opennova_til)
target_include_directories(til_foliage_blocker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME til_foliage_blocker COMMAND til_foliage_blocker_test)

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

add_executable(foliage_map_remap_indices_test foliage/foliage_map_remap_indices_test.cpp)
target_link_libraries(foliage_map_remap_indices_test PRIVATE opennova_foliage)
target_include_directories(foliage_map_remap_indices_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME foliage_map_remap_indices COMMAND foliage_map_remap_indices_test)

add_executable(foliage_runtime_vectors_test foliage/foliage_runtime_vectors_test.cpp)
target_link_libraries(foliage_runtime_vectors_test PRIVATE opennova_foliage)
target_include_directories(foliage_runtime_vectors_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(foliage_runtime_vectors_test PRIVATE cxx_std_17)
add_test(NAME foliage_runtime_vectors COMMAND foliage_runtime_vectors_test)

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

add_executable(env_render_unit_test env/env_render_unit_test.cpp)
target_link_libraries(env_render_unit_test PRIVATE opennova_env)
target_include_directories(env_render_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME env_render_unit COMMAND env_render_unit_test)

add_executable(jo_env_sweep_test env/jo_env_sweep_test.cpp)
target_link_libraries(jo_env_sweep_test PRIVATE opennova_env opennova_vfs)
target_include_directories(jo_env_sweep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME env_jo_install COMMAND jo_env_sweep_test)

# Terrain build foundation + parity tests ported from the canonical terrain reference.
#   parametric_parity — byte-identical CPT output across Sample/Gradient/Checker64/Perlin fixtures
#   crypto_roundtrip   — rotate_encrypt/decrypt round-trip
#   depthmap_smooth    — 8-bit -> 16-bit smoothing formula parity
#   tpm1_roundtrip     — .tml MeshData read/write
#   cdep_read          — CDEP header parity against a known Dvxi5.cpt capture
#   cdep_roundtrip     — CDEP encode/decode round-trip independent of the bake
foreach(name parametric_parity depthmap_smooth tpm1_roundtrip)
    add_executable(${name}_test terrain/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE opennova_terrain)
    target_include_directories(${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

add_executable(terrain_lighting_test terrain/terrain_lighting_test.cpp)
target_link_libraries(terrain_lighting_test PRIVATE opennova_terrain)
target_include_directories(terrain_lighting_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_lighting COMMAND terrain_lighting_test)

add_executable(terrain_texture_preprocess_test terrain/terrain_texture_preprocess_test.cpp)
target_link_libraries(terrain_texture_preprocess_test PRIVATE opennova_terrain)
target_include_directories(terrain_texture_preprocess_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_texture_preprocess COMMAND terrain_texture_preprocess_test)

add_executable(terrain_lod_family_test terrain/terrain_lod_family_test.cpp)
target_link_libraries(terrain_lod_family_test PRIVATE opennova_terrain)
target_include_directories(terrain_lod_family_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_lod_family COMMAND terrain_lod_family_test)

add_executable(terrain_foliage_detail_collector_test terrain/terrain_foliage_detail_collector_test.cpp)
target_link_libraries(terrain_foliage_detail_collector_test PRIVATE opennova_terrain)
target_include_directories(terrain_foliage_detail_collector_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_foliage_detail_collector COMMAND terrain_foliage_detail_collector_test)

# parametric_parity is a long byte-identical fixture parity run and remains
# disabled until its CI stability/performance cost is addressed.
set_tests_properties(parametric_parity PROPERTIES DISABLED TRUE)

foreach(name crypto_roundtrip cdep_read cdep_roundtrip)
    add_executable(${name}_test terrain/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE opennova_cpt)
    target_include_directories(${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

add_executable(trn_config_roundtrip_test terrain/trn_config_roundtrip_test.cpp)
target_link_libraries(trn_config_roundtrip_test PRIVATE opennova_trn)
target_include_directories(trn_config_roundtrip_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libs/terrain/include
)
add_test(NAME trn_config_roundtrip COMMAND trn_config_roundtrip_test)

# DVD4 parity test: drives opennova::build_terrain() on the captured BHD-era heightmap.
# Regression test for the LoadSectionDedup crash that hit older terrains where
# the root merge ingests face indices past level.vertex_count.
add_executable(dvd4_parity_test terrain/dvd4_parity_test.cpp)
target_link_libraries(dvd4_parity_test PRIVATE opennova_terrain)
target_include_directories(dvd4_parity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME dvd4_parity COMMAND dvd4_parity_test)

# CDEP overflow guard: bake must clamp rather than silently corrupt the .cpt
# when a 256-pixel block exceeds the 4-bit bits_per_delta field's 15-bit limit.
add_executable(cdep_overflow_guard_test terrain/cdep_overflow_guard_test.cpp)
target_link_libraries(cdep_overflow_guard_test PRIVATE opennova_terrain opennova_cpt)
target_include_directories(cdep_overflow_guard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cdep_overflow_guard COMMAND cdep_overflow_guard_test)

# CDEP per-block range constraint: the editor-side raw16 clamp/count kernel,
# ported from terrain_editor_cdep_constraint.gd into libs/terrain.
add_executable(cdep_constraint_test terrain/cdep_constraint_test.cpp)
target_link_libraries(cdep_constraint_test PRIVATE opennova_terrain)
target_include_directories(cdep_constraint_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cdep_constraint COMMAND cdep_constraint_test)

# Terrain world->source coordinate transform kernel (libs/terrain/coords).
# Characterizes the runtime resolve_world_sample formula plus the editor-mode
# guard divergences (bounds-reject, id-clamp, local-clamp) ported from
# EditorTerrainMesh.world_to_source_coords / world_to_cell_source_coords.
add_executable(coords_test terrain/coords_test.cpp)
target_link_libraries(coords_test PRIVATE opennova_terrain)
target_include_directories(coords_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME coords COMMAND coords_test)

# Per-quadrant neighbour-tap locks (the .trn lock_* pairs) in the shared tap
# kernel: the masking against a verbatim copy of the original, the lock-table
# index, the sampler at a sector seam locked vs unlocked, and the all-zero
# default that keeps unlocked terrains rendering exactly as before.
add_executable(quadrant_lock_test terrain/quadrant_lock_test.cpp)
target_link_libraries(quadrant_lock_test PRIVATE opennova_terrain_query)
target_include_directories(quadrant_lock_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME quadrant_lock COMMAND quadrant_lock_test)

# The retail terrain segment raycast (libs/terrain_query/terrain_raycast):
# the Terrain_RaycastHeightmapLoRes march + HiRes_0 refine structural
# translation over a synthetic sampler — step normalization/budget, ray-z hit
# semantics, the column-shortcut crossing asymmetry, the empty-cell height-0
# floor, the out-of-extent editor guard, and the odd skip-refine guard.
add_executable(terrain_raycast_test terrain/terrain_raycast_test.cpp)
target_link_libraries(terrain_raycast_test PRIVATE opennova_terrain_query)
target_include_directories(terrain_raycast_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_raycast COMMAND terrain_raycast_test)

# Height brush kernels (libs/terrain/brush): raise/lower, smooth (two-pass),
# flatten + the shared smoothstep falloff, ported from terrain_editor_brushes.gd.
add_executable(brush_height_test terrain/brush_height_test.cpp)
target_link_libraries(brush_height_test PRIVATE opennova_terrain)
target_include_directories(brush_height_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME brush_height COMMAND brush_height_test)

# Colour/blend brush kernels (libs/terrain/brush) on RGBA8 buffers. Byte-parity
# critical: pins Godot's FORMAT_RGBA8 quantization + the blend/lerp/clone math.
add_executable(brush_color_test terrain/brush_color_test.cpp)
target_link_libraries(brush_color_test PRIVATE opennova_terrain)
target_include_directories(brush_color_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME brush_color COMMAND brush_color_test)

# Dvxi5 repro: exercises a post-clamp heightmap with block ranges at the
# CDEP ceiling. Kept as a regression anchor for the fixed bake pipeline.
add_executable(dvxi5_repro_test terrain/dvxi5_repro_test.cpp)
target_link_libraries(dvxi5_repro_test PRIVATE opennova_terrain)
target_include_directories(dvxi5_repro_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    # /EHa so the SEH translator in main() can convert access violations
    # into std::runtime_error instead of bypassing C++ catch blocks.
    target_compile_options(dvxi5_repro_test PRIVATE /EHa)
endif()
add_test(NAME dvxi5_repro COMMAND dvxi5_repro_test)

# Particle (.ptl) text-format parser/simulator tests. See libs/particle and the
# witness matrix in docs/particles/ptl-format-re.md
# (CParticleDef_ParseFromConfigMap @ 0x5ed210, Jointops.exe IDB).
foreach(name smallest minimal_effect multi_section multi_layer table_handles
        smoke_all_fixtures enum_parse writer_roundtrip emitter curve_lut
        emit_shape translate atlas lenient_lines effect_scene_contract)
    add_executable(particle_${name}_test particle/particle_${name}_test.cpp)
    target_link_libraries(particle_${name}_test PRIVATE opennova_particle)
    target_include_directories(particle_${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(particle_${name}_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME particle_${name} COMMAND particle_${name}_test)
endforeach()

# Smoke test stays disabled until the full 77-file corpus is mirrored into
# fixtures/particle/ (mirrors the parametric_parity DISABLED pattern above).
set_tests_properties(particle_smoke_all_fixtures PROPERTIES DISABLED TRUE)

# CBIN tests
add_executable(cbin_unit_test cbin/cbin_unit_test.cpp)
target_link_libraries(cbin_unit_test PRIVATE opennova_cbin)
target_include_directories(cbin_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cbin_unit COMMAND cbin_unit_test)

add_executable(cbin_roundtrip_test cbin/cbin_roundtrip_test.cpp)
target_link_libraries(cbin_roundtrip_test PRIVATE opennova_cbin)
target_include_directories(cbin_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cbin_roundtrip_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME cbin_roundtrip COMMAND cbin_roundtrip_test)

# MNU menu UI tests (forgiving XML parser, MNU AST + serialize roundtrip, MNS stylesheet)
add_executable(mnu_xml_unit_test mnu/mnu_xml_unit_test.cpp)
target_link_libraries(mnu_xml_unit_test PRIVATE opennova_mnu)
target_include_directories(mnu_xml_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mnu_xml_unit COMMAND mnu_xml_unit_test)

# controls: the JO input-binding catalog + Options->Controls (CONTROL_MAPPING) model.
add_executable(controls_test controls/controls_test.cpp)
target_link_libraries(controls_test PRIVATE opennova_controls)
target_include_directories(controls_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME controls_unit COMMAND controls_test)

add_executable(mnu_unit_test mnu/mnu_unit_test.cpp)
target_link_libraries(mnu_unit_test PRIVATE opennova_mnu)
target_include_directories(mnu_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mnu_unit COMMAND mnu_unit_test)

# Compat sweep over real shipped .mnu menus (committed fixtures required-pass,
# developer Desktop set skip-if-absent). Reads fixtures/mnu/ so it runs from the
# repo root, like sbf_compat.
add_executable(mnu_compat_test mnu/mnu_compat_test.cpp)
target_link_libraries(mnu_compat_test PRIVATE opennova_mnu)
target_include_directories(mnu_compat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mnu_compat COMMAND mnu_compat_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Round-trip key-coverage: catches attributes/tags dropped at parse time, which
# the idempotence sweep above cannot see (see ADR 0002). Reads fixtures/mnu/.
add_executable(mnu_coverage_test mnu/mnu_coverage_test.cpp)
target_link_libraries(mnu_coverage_test PRIVATE opennova_mnu)
target_include_directories(mnu_coverage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mnu_coverage COMMAND mnu_coverage_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(mns_parse_test mns/mns_parse_test.cpp)
target_link_libraries(mns_parse_test PRIVATE opennova_mns)
target_include_directories(mns_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mns_parse COMMAND mns_parse_test)

# Lossless MNS document model (ADR 0009): byte round-trip on the real shipped
# menu_style.mns, minimal-delta edit stability, diagnostics. Reads
# fixtures/mns/ so it runs from the repo root, like mnu_compat.
add_executable(mns_document_test mns/mns_document_test.cpp)
target_link_libraries(mns_document_test PRIVATE opennova_mns)
target_include_directories(mns_document_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mns_document COMMAND mns_document_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Shared editor undo/redo core (header-only opennova::edit::EditHistory)
add_executable(oned_edit_edit_history_test oned_edit/edit_history_test.cpp)
target_link_libraries(oned_edit_edit_history_test PRIVATE opennova_oned_edit)
target_include_directories(oned_edit_edit_history_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME edit_history COMMAND oned_edit_edit_history_test)

# World runtime substrate: addressable entities (faithful find_by_net_id), shared
# var store, entity-command primitives, the logic-tick clock, snapshot/restore.
add_executable(world_test world/world_test.cpp)
target_link_libraries(world_test PRIVATE opennova_world)
target_include_directories(world_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(world_test PRIVATE cxx_std_17)
add_test(NAME world COMMAND world_test)

# The player waypoint track (HUD waypoint label backing state).
add_executable(waypoint_track_test world/waypoint_track_test.cpp)
target_link_libraries(waypoint_track_test PRIVATE opennova_world)
target_include_directories(waypoint_track_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(waypoint_track_test PRIVATE cxx_std_17)
add_test(NAME waypoint_track COMMAND waypoint_track_test)

# AI subsystem (byte-exact foundation): state enum + struct layout, AIEvent ring,
# budget gate (AI_BeginUpdate), the infantry state-machine dispatcher + transitions,
# and the trivial enter/exit/event handler ports.
add_executable(ai_test world/ai_test.cpp)
target_link_libraries(ai_test PRIVATE opennova_world)
target_include_directories(ai_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ai_test PRIVATE cxx_std_17)
add_test(NAME ai COMMAND ai_test)

# Item destruction: the explosion queue + AoE gates, the bullet armor gates, the
# destructible death chain (husk flags + events + the kz chain blast), the
# death-piece pool, and the dead-item settle (world/destruction.h;
# docs/world/world-wac-ai-re.md §24).
add_executable(destruction_test world/destruction_test.cpp)
target_link_libraries(destruction_test PRIVATE opennova_world)
target_include_directories(destruction_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(destruction_test PRIVATE cxx_std_17)
add_test(NAME destruction COMMAND destruction_test)

# Throwables (world/throwables.h; world-wac-ai-re §27): PowerThrow charge,
# grenade bounce/fuse, satchel place/detonator, claymore cone + fan, AV-mine
# data-dead proximity, owner-death cleanup.
add_executable(throwables_test world/throwables_test.cpp)
target_link_libraries(throwables_test PRIVATE opennova_world)
target_include_directories(throwables_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(throwables_test PRIVATE cxx_std_17)
add_test(NAME throwables COMMAND throwables_test)

# Terrain-grounded movement: the shared height_field samplers, the 5-tap
# calc_average_ground_height port (Entity_CalcAverageGroundHeight @0x457230) by hand
# math, and the AiSystem ground clamp over a synthetic ramp atlas.
add_executable(ground_height_test world/ground_height_test.cpp)
target_link_libraries(ground_height_test PRIVATE opennova_world)
target_include_directories(ground_height_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ground_height_test PRIVATE cxx_std_17)
add_test(NAME ground_height COMMAND ground_height_test)

# Infantry motor (Entity_UpdateInfantryAI @0x4b9910): body-turn clamp, turn-in-place
# gates, alert/wounded gait selection + availability fallbacks, one-shot route
# lifecycle, flag-table commit rules, gravity/landing/fall damage, slope slide —
# each against the witnessed constants, over synthetic clips + height fields.
add_executable(infantry_test world/infantry_test.cpp)
target_link_libraries(infantry_test PRIVATE opennova_world)
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
target_link_libraries(slot_sound_test PRIVATE opennova_world)
target_include_directories(slot_sound_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(slot_sound_test PRIVATE cxx_std_17)
add_test(NAME slot_sound COMMAND slot_sound_test)

# World-object collision (Entity_TestCollisionSections @0x4aef90 / _RaycastCollisionModel
# @0x413060 / _ComputeBoneCollisionForce @0x4ae150 / _ProcessCollisionAndPlatformPhysics
# @0x4b2bd0): matrix roundtrip, blink type-8 query + flags^6/indoors, type-1 segment
# clip, the ground probe through candidate models, wall push-out with prev-position
# gating, hurt/ladder volumes, and the idle skip throttle — over hand-built volumes.
add_executable(collision_test world/collision_test.cpp)
target_link_libraries(collision_test PRIVATE opennova_world)
target_include_directories(collision_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(collision_test PRIVATE cxx_std_17)
add_test(NAME collision COMMAND collision_test)

# Authoritative projectile consequences through RoundSim's CollisionWorld seam:
# pre-arm dud substitution, kinetic damage, NoDie, and physical zero-damage blockers.
add_executable(projectile_combat_test world/projectile_combat_test.cpp)
target_link_libraries(projectile_combat_test PRIVATE opennova_world)
target_include_directories(projectile_combat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(projectile_combat_test PRIVATE cxx_std_17)
add_test(NAME projectile_combat COMMAND projectile_combat_test)

# Rendering occlusion (Terrain_InitBuildingPortals @0x5c7480 register/weld/flags,
# build_sector_visibility_masks @0x5c8610 + render_visibility_portal_traversal
# @0x5c4ae0, test_sector_entity_occlusion @0x5c4610, the entity-collector gates +
# terrain_occlusion_check_three_rays @0x610ed0, Entity_QueryBlinkBoxesAtPoint
# @0x4af350): render-float math, the weld pass, outdoor/indoor mask rules,
# outside-in viewthru banking, TOC shadow-wedge culling, the blink-hits render
# gate + three-ray latch — over hand-built portal/collision models.
add_executable(occlusion_test world/occlusion_test.cpp)
target_link_libraries(occlusion_test PRIVATE opennova_world)
target_include_directories(occlusion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(occlusion_test PRIVATE cxx_std_17)
add_test(NAME occlusion COMMAND occlusion_test)

# Real-asset occlusion probe: retail Armry01.3di (the 00TRa armory) through the
# host's IR->runtime conversions, camera walked room to room, per-room section
# masks asserted (window/portal/open-slot legs over authored OFAC topology).
# Uses the committed Armry01 fixture in CI; OPENNOVA_JO_ASSETS can override it
# for a retail-install parity run.
add_executable(occlusion_armry_test world/occlusion_armry_test.cpp)
target_link_libraries(occlusion_armry_test PRIVATE opennova_world opennova_threedi)
target_include_directories(occlusion_armry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(occlusion_armry_test PRIVATE cxx_std_17)
target_compile_definitions(occlusion_armry_test PRIVATE
    OPENNOVA_ARMRY_FIXTURE="${CMAKE_SOURCE_DIR}/fixtures/3dp/armry01/Armry01.3di")
add_test(NAME occlusion_armry COMMAND occlusion_armry_test)

# The FP weapon action FSM (WeaponAction_ProcessFrame @0x540e60 + Anim_InitActions
# @0x541fa0, net-re §5.62): the ACTION-row bake (auto delays from clip ticks), the
# pump phase protocol, and the witnessed handler decisions — the fire->recoil chain,
# the recoil arbiter (refire/auto-reload/emptyidle), reload scope stash/rescope,
# 3-round burst, dry-fire.
add_executable(weapon_fsm_test world/weapon_fsm_test.cpp)
target_link_libraries(weapon_fsm_test PRIVATE opennova_world)
target_include_directories(weapon_fsm_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(weapon_fsm_test PRIVATE cxx_std_17)
add_test(NAME weapon_fsm COMMAND weapon_fsm_test)

# The per-player weapon slot pool, spawn-kit chain, and map availability rules
# (the 2026-07-18 loadout grill): the .bms kit filter + knife fallback, display
# expansion, the 780-slot fill, pool seeding (classrounds overrides) + clip
# recalc, and the three switching walks (select / category-key / cycle) with
# their witnessed gates.
add_executable(weapon_inventory_test world/weapon_inventory_test.cpp)
target_link_libraries(weapon_inventory_test PRIVATE opennova_world)
target_include_directories(weapon_inventory_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(weapon_inventory_test PRIVATE cxx_std_17)
add_test(NAME weapon_inventory COMMAND weapon_inventory_test)

# Local player view state (the ADS scope ease, the 3P anchor chase, the fov
# policy) at the fixed 62.5 Hz tick — render-cadence invariance — plus the
# input-dispatch gates in front of the FSM requests.
add_executable(player_view_test world/player_view_test.cpp)
target_link_libraries(player_view_test PRIVATE opennova_world)
target_include_directories(player_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(player_view_test PRIVATE cxx_std_17)
add_test(NAME player_view COMMAND player_view_test)

# The mouse-look pipeline (pixel deltas -> yaw/pitch BAM): the witnessed integer
# math — sens << 11, the +0x8000 rounding, the scoped zoom divide, flipmouse,
# and the pitch clamps with the prone +40-degree up-limit.
add_executable(player_look_test world/player_look_test.cpp)
target_link_libraries(player_look_test PRIVATE opennova_world)
target_include_directories(player_look_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(player_look_test PRIVATE cxx_std_17)
add_test(NAME player_look COMMAND player_look_test)

# Spawn-state init (Entity_ResetToSpawnState @0x4B9610, net-re §5.2b): the entity+36
# bit-1 movement-gate clear + Position backup — the field-init half of the host
# player-spawn machine that the Phase-2 listen-server player spawn reuses.
add_executable(entity_spawn_test world/entity_spawn_test.cpp)
target_link_libraries(entity_spawn_test PRIVATE opennova_world)
target_include_directories(entity_spawn_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(entity_spawn_test PRIVATE cxx_std_17)
add_test(NAME entity_spawn COMMAND entity_spawn_test)

# The host's own-player keystone (net-re §5.2b/§5.38): spawn_player (pool-0 player-infantry
# 0x14B9 + gate clear), PlayerBodyInput (the 8-way Player_PackInputStateToEntity port),
# and the infantry motor's local-player branch driving + mirroring the pose to the registry
# Entity that the S2C 0x0A snapshot reads.
add_executable(player_spawn_test world/player_spawn_test.cpp)
target_link_libraries(player_spawn_test PRIVATE opennova_world)
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
target_link_libraries(spawn_select_test PRIVATE opennova_world)
target_include_directories(spawn_select_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(spawn_select_test PRIVATE cxx_std_17)
add_test(NAME spawn_select COMMAND spawn_select_test)

# Advance & Secure zone-slot chain (net-re §5.61; ZoneSlotChain_* @0x4A2350..0x4A2DE0,
# ex-"CWeaponSlotManager"): masks/frontier/adjacency, the 0x0F owned-zone mask, the control
# latch, the 0xFFFE auto-deploy pick, and the 0x0E pick resolve — pinned on the ASH_I5A shape
# (zone 1 team 1 / zone 2 x2 neutral / zone 3 team 2, golden uniformMask 0x8).
add_executable(zone_chain_test world/zone_chain_test.cpp)
target_link_libraries(zone_chain_test PRIVATE opennova_world)
target_include_directories(zone_chain_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(zone_chain_test PRIVATE cxx_std_17)
add_test(NAME zone_chain COMMAND zone_chain_test)

# Host-authoritative ground-vehicle motor (net-re §5.13 drive-authority witness): the
# items.def physics-block scaling (ItemDef_ParsePhysicsProperty @0x49d870), the drive
# input mapping, steering chase, and speed pipeline of Entity_UpdateVehiclePhysics
# @0x48af00 — a mounted ctrl/drvr player's replicated input moves the vehicle.
add_executable(vehicle_motor_test world/vehicle_motor_test.cpp)
target_link_libraries(vehicle_motor_test PRIVATE opennova_world opennova_def)
target_include_directories(vehicle_motor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vehicle_motor_test PRIVATE cxx_std_17)
add_test(NAME vehicle_motor COMMAND vehicle_motor_test)

# The USE-ITEM mount chain (Entity_ToggleVehicleMount @0x436950 + the nearest-seat scan
# @0x435d50 + the deck best-seat @0x4351f0), the BMS Player mount triggers (subs 38-41),
# the vehicle AI-driver/parked leg (@0x48bc12-0x48c034), and the player deploy group
# stamp (@0x519fd0).
add_executable(vehicle_mount_test world/vehicle_mount_test.cpp)
target_link_libraries(vehicle_mount_test PRIVATE opennova_world)
target_include_directories(vehicle_mount_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(vehicle_mount_test PRIVATE cxx_std_17)
add_test(NAME vehicle_mount COMMAND vehicle_mount_test)

# Root-motion track grill vs real JO clips (AnimMap_UpdateEntity @0x40b5f0): the .bad
# events array is the per-frame root record; rec[3] = cumulative forward distance whose
# 16.16 delta drives the infantry motor. Asset-gated: absent files skip (CI-safe).
add_executable(root_motion_test anim/root_motion_test.cpp)
target_link_libraries(root_motion_test PRIVATE opennova_bad)
target_include_directories(root_motion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(root_motion_test PRIVATE cxx_std_17)
add_test(NAME root_motion COMMAND root_motion_test)

# WAC scripting: registry oracle (165-command ISA + 28 param types from the binary),
# VM behavior (var math, entity mutation, temporal/edge firing, env, authority),
# and a zero-crash parse over the real shipped .wac corpus (dirs via argv, else the
# dev defaults; absent dirs skip).
foreach(test registry_oracle behavior corpus)
    add_executable(wac_${test}_test wac/wac_${test}_test.cpp)
    target_link_libraries(wac_${test}_test PRIVATE opennova_wac)
    target_include_directories(wac_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(wac_${test}_test PRIVATE cxx_std_17)
    add_test(NAME wac_${test} COMMAND wac_${test}_test)
endforeach()

# Cross-system: BMS event runtime + WAC share one world/var store (libs/mission
# event_runtime + opennova_wac). MisvarChange(V#) and WAC eq(V#) see the same var.
add_executable(event_runtime_test mission/event_runtime_test.cpp)
target_link_libraries(event_runtime_test PRIVATE opennova_mission opennova_wac opennova_world)
target_include_directories(event_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(event_runtime_test PRIVATE cxx_std_17)
add_test(NAME event_runtime_bms COMMAND event_runtime_test)

# Mission -> world promotion: spawn a parsed mission's entities into the world pools,
# allocate AI brains per organic, build the waypoint nav table, then tick the AI to prove
# entities patrol their authored routes (the connective tissue for the live sim).
add_executable(promote_test mission/promote_test.cpp)
target_link_libraries(promote_test PRIVATE opennova_mission opennova_world)
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

# Emplacement mount: the seat model + EntityCommands mount/dismount/find_best_seat
# primitives + the BMS AttachToEmplaced path (event -> proximity mount) + the
# snapshot/restore rewind of a mount.
add_executable(mount_test mission/mount_test.cpp)
target_link_libraries(mount_test PRIVATE opennova_mission opennova_world)
target_include_directories(mount_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(mount_test PRIVATE cxx_std_17)
add_test(NAME mission_mount COMMAND mount_test)

# Cross-resource reference extraction: env/credits/items.def extractors, the
# extension+magic dispatch, edge dedup, and fail-soft on malformed input.
add_executable(refs_extract_test refs/refs_extract_test.cpp)
target_link_libraries(refs_extract_test PRIVATE opennova_refs opennova_env opennova_cbin opennova_def)
target_include_directories(refs_extract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(refs_extract_test PRIVATE cxx_std_17)
add_test(NAME refs_extract COMMAND refs_extract_test)

# refs: .3di material-texture, .bms header/item, and .mnu menu-asset extractors
# against the committed retail fixtures (self-consistent vs each format's own
# parser).
foreach(test threedi mission mnu)
    add_executable(refs_${test}_test refs/refs_${test}_test.cpp)
    target_link_libraries(refs_${test}_test PRIVATE opennova_refs opennova_threedi opennova_mission_format opennova_mnu)
    target_include_directories(refs_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(refs_${test}_test PRIVATE cxx_std_17)
    add_test(NAME refs_${test} COMMAND refs_${test}_test)
endforeach()

# refs: the whole-root RefGraph (referrers inverse, (size,mtime) memo, fail-soft
# errors, broken-reference validation) over an in-memory file map.
add_executable(ref_graph_test refs/ref_graph_test.cpp)
target_link_libraries(ref_graph_test PRIVATE opennova_refs opennova_env opennova_cbin)
target_include_directories(ref_graph_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ref_graph_test PRIVATE cxx_std_17)
add_test(NAME ref_graph COMMAND ref_graph_test)

# ----------------------------------------------------------------------------
# Net stack tests (relanded from PR #37)
# ----------------------------------------------------------------------------

# novacrypto tests
add_executable(url_cipher_test novacrypto/url_cipher_test.cpp)
target_link_libraries(url_cipher_test PRIVATE opennova_novacrypto)
target_include_directories(url_cipher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME url_cipher COMMAND url_cipher_test)

add_executable(crc32_test novacrypto/crc32_test.cpp)
target_link_libraries(crc32_test PRIVATE opennova_novacrypto)
target_include_directories(crc32_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME crc32 COMMAND crc32_test)

add_executable(nwu_test novacrypto/nwu_test.cpp)
target_link_libraries(nwu_test PRIVATE opennova_novacrypto)
target_include_directories(nwu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nwu COMMAND nwu_test)

add_executable(pubcrypto_test novacrypto/pubcrypto_test.cpp)
target_link_libraries(pubcrypto_test PRIVATE opennova_novacrypto)
target_include_directories(pubcrypto_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pubcrypto COMMAND pubcrypto_test)

add_executable(epask_test novacrypto/epask_test.cpp)
target_link_libraries(epask_test PRIVATE opennova_novacrypto)
target_include_directories(epask_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME epask COMMAND epask_test)

# napi tests
add_executable(envelope_test napi/envelope_test.cpp)
target_link_libraries(envelope_test PRIVATE opennova_napi opennova_novacrypto)
target_include_directories(envelope_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME envelope COMMAND envelope_test)

add_executable(tlv_test napi/tlv_test.cpp)
target_link_libraries(tlv_test PRIVATE opennova_napi)
target_include_directories(tlv_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME tlv COMMAND tlv_test)

add_executable(session_test napi/session_test.cpp)
target_link_libraries(session_test PRIVATE opennova_napi)
target_include_directories(session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME session COMMAND session_test)

# novaworld tests
add_executable(registration_url_test novaworld/registration_url_test.cpp)
target_link_libraries(registration_url_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(registration_url_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME registration_url COMMAND registration_url_test)

add_executable(gate_response_test novaworld/gate_response_test.cpp)
target_link_libraries(gate_response_test PRIVATE opennova_novaworld)
target_include_directories(gate_response_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gate_response COMMAND gate_response_test)

add_executable(gate_probe_test novaworld/gate_probe_test.cpp)
target_link_libraries(gate_probe_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(gate_probe_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gate_probe COMMAND gate_probe_test)

add_executable(lobby_update_test novaworld/lobby_update_test.cpp)
target_link_libraries(lobby_update_test PRIVATE opennova_novaworld)
target_include_directories(lobby_update_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME lobby_update COMMAND lobby_update_test)

add_executable(gate_server_loopback_test novaworld/gate_server_loopback_test.cpp)
target_link_libraries(gate_server_loopback_test PRIVATE opennova_novaworld opennova_novacrypto opennova_napi opennova_app_common)
target_include_directories(gate_server_loopback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gate_server_loopback COMMAND gate_server_loopback_test)

add_executable(gsb_roundtrip_test novaworld/gsb_roundtrip_test.cpp)
target_link_libraries(gsb_roundtrip_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(gsb_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gsb_roundtrip COMMAND gsb_roundtrip_test)

# ADR 0010 Phase 2 — gsb_parse_response is the exact client inverse of the builder.
add_executable(gsb_parse_roundtrip_test novaworld/gsb_parse_roundtrip_test.cpp)
target_link_libraries(gsb_parse_roundtrip_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(gsb_parse_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gsb_parse_roundtrip COMMAND gsb_parse_roundtrip_test)

# Retail dispatcher semantics (D-NET-190..193): SVRS accumulation, the gated
# "GSB " reset, undersized-record skips, and the XXXX-terminator hardening.
add_executable(gsb_retail_semantics_test novaworld/gsb_retail_semantics_test.cpp)
target_link_libraries(gsb_retail_semantics_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(gsb_retail_semantics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME gsb_retail_semantics COMMAND gsb_retail_semantics_test)

# Byte-level oracle: decode the genuine .204 server-browser blob
# (fixtures/novaworld/nw204_jop_2.gsb) through our retail-faithful parser.
add_executable(gsb_real204_decode_test novaworld/gsb_real204_decode_test.cpp)
target_link_libraries(gsb_real204_decode_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(gsb_real204_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gsb_real204_decode_test PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld")
add_test(NAME gsb_real204_decode COMMAND gsb_real204_decode_test)

# ADR 0010 / P7 Part 2 — the lobby HTTP flow (login / GSB / NWJoin machines) driven bytes-in/out.
add_executable(http_flow_test novaworld/http_flow_test.cpp)
target_link_libraries(http_flow_test PRIVATE opennova_novaworld opennova_novacrypto)
add_test(NAME http_flow COMMAND http_flow_test)

# ADR 0010 / P7 Part 2 (B2) — the lobby var-builders (host request/update + identity + host:port)
# proven byte-identical to the bindings' hand-assembly.
add_executable(lobby_vars_test novaworld/lobby_vars_test.cpp)
target_link_libraries(lobby_vars_test PRIVATE opennova_novaworld opennova_napi)
add_test(NAME lobby_vars COMMAND lobby_vars_test)

add_executable(peer_addr_test npwire/peer_addr_test.cpp)
target_link_libraries(peer_addr_test PRIVATE opennova_npwire)
target_include_directories(peer_addr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME peer_addr COMMAND peer_addr_test)

add_executable(protocol_message_test novaworld/protocol_message_test.cpp)
target_link_libraries(protocol_message_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(protocol_message_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME protocol_message COMMAND protocol_message_test)

add_executable(session_protocol_test novaworld/session_protocol_test.cpp)
target_link_libraries(session_protocol_test PRIVATE opennova_novaworld)
target_include_directories(session_protocol_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME session_protocol COMMAND session_protocol_test)

# NOTE (P8): the old joiner_session_test (the legacy novaworld::JoinerSession driven against the
# retired HostSessionAccept) was deleted — np::JoinerConnection is the promoted in-match client and is
# covered by tests/npruntime/joiner_connection_test.cpp against the real np server legs.

# Connection registry / manager tests
add_executable(connection_registry_test novaworld/connection_registry_test.cpp)
target_link_libraries(connection_registry_test PRIVATE opennova_novaworld)
target_include_directories(connection_registry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME connection_registry COMMAND connection_registry_test)

add_executable(connection_manager_test novaworld/connection_manager_test.cpp)
target_link_libraries(connection_manager_test PRIVATE opennova_novaworld)
target_include_directories(connection_manager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME connection_manager COMMAND connection_manager_test)

# SQLite wrapper + migration runner tests
add_executable(db_sqlite_test novaworld/db_sqlite_test.cpp)
target_link_libraries(db_sqlite_test PRIVATE opennova_novaworld)
target_include_directories(db_sqlite_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME db_sqlite COMMAND db_sqlite_test)

# Phase A — ClientHello/ClientAuth serializer roundtrip
add_executable(session_hello_roundtrip_test novaworld/session_hello_roundtrip_test.cpp)
target_link_libraries(session_hello_roundtrip_test PRIVATE opennova_novaworld)
target_include_directories(session_hello_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME session_hello_roundtrip COMMAND session_hello_roundtrip_test)

# Phase D — LobbySession dispatch tests
add_executable(lobby_session_test novaworld/lobby_session_test.cpp)
target_link_libraries(lobby_session_test PRIVATE opennova_novaworld opennova_napi)
target_include_directories(lobby_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME lobby_session COMMAND lobby_session_test)

# ADR 0010 Phase 1 — ClientSession handshake (hk echo + 0x83 verify) driven
# in-process against the real server-side parsers/builders + LobbySession.
add_executable(client_session_loopback_test novaworld/client_session_loopback_test.cpp)
target_link_libraries(client_session_loopback_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto)
target_include_directories(client_session_loopback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME client_session_loopback COMMAND client_session_loopback_test)

# NW-S3 closure: the 0x42 join carries the retail CU var set (make_novaworld_join_cu
# builder + a ClientSession actually emitting the chunks in its ClientAuth).
add_executable(client_session_cu_test novaworld/client_session_cu_test.cpp)
target_link_libraries(client_session_cu_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto)
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
target_link_libraries(nw_host_register_e2e_test PRIVATE
    opennova_novaworld opennova_npruntime opennova_napi opennova_novacrypto opennova_app_common)
target_include_directories(nw_host_register_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server
    ${CMAKE_SOURCE_DIR}/apps/common)
if(WIN32)
    target_link_libraries(nw_host_register_e2e_test PRIVATE ws2_32)
endif()
add_test(NAME nw_host_register_e2e COMMAND nw_host_register_e2e_test)

# ADR 0010 Phase 3 — EPASK account-login wire format. Builds the NWLogin.dll
# credential POST body with the login helpers and decodes it through a copy of
# the server's parse_form_body + EPASK path, then checks the cookie jar carries
# NWHANDLE/PCID onto the later GSB fetch (grill NW-S5/B3).
add_executable(http_login_test novaworld/http_login_test.cpp)
target_link_libraries(http_login_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(http_login_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME http_login COMMAND http_login_test)

# ADR 0010 Phase 5 — the NWJoin .joi connection-string parser (host address +
# tokens) and url_cipher NK round-trip.
add_executable(joi_parse_test novaworld/joi_parse_test.cpp)
target_link_libraries(joi_parse_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(joi_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME joi_parse COMMAND joi_parse_test)

add_executable(join_identity_test novaworld/join_identity_test.cpp)
target_link_libraries(join_identity_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(join_identity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME join_identity COMMAND join_identity_test)

# ADR 0010 Phase 5 — the JointOperations ClientHello (proto switch): PN flips to
# JointOperations with a distinct PG; the NOVAWORLDUDP path is unchanged.
add_executable(jointops_hello_test novaworld/jointops_hello_test.cpp)
target_link_libraries(jointops_hello_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto)
target_include_directories(jointops_hello_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME jointops_hello COMMAND jointops_hello_test)

# ADR 0010 real-NW parity — decodes the genuine .204 lobby capture (the oracle):
# envelope + outer NWU + per-session SCRK, dumps every container so our builders
# can be matched field for field. fixtures/novaworld/nw204_lobby.hexcap.
add_executable(nw204_lobby_decode_test novaworld/nw204_lobby_decode_test.cpp)
target_link_libraries(nw204_lobby_decode_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto)
target_include_directories(nw204_lobby_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw204_lobby_decode_test PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld")
add_test(NAME nw204_lobby_decode COMMAND nw204_lobby_decode_test)

# In-game JOINTOPERATIONS surveyor — decodes a loopback host+join+play capture
# and histograms the in-match NAPI messages per (direction, msg_id) so the
# high-volume tags can be RE'd against §4. Asset-gated on env NW_INGAME_HEXCAP
# (skips cleanly when unset); same decode pipeline as nw204_lobby_decode.
add_executable(nw_ingame_histogram_test novaworld/nw_ingame_histogram_test.cpp)
target_link_libraries(nw_ingame_histogram_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto)
target_include_directories(nw_ingame_histogram_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_histogram COMMAND nw_ingame_histogram_test)

# Pool-records byte-witness — walks every S2C 0x0D and 0x20 payload in the
# same hexcap and asserts the §5.11/§5.12 field maps consume the wire exactly.
# Asset-gated on NW_INGAME_HEXCAP, identical pipeline to the histogram test.
add_executable(nw_ingame_pool_records_test novaworld/nw_ingame_pool_records_test.cpp)
target_link_libraries(nw_ingame_pool_records_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto)
target_include_directories(nw_ingame_pool_records_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_pool_records COMMAND nw_ingame_pool_records_test)

# /PROFILE .sph server-log decoder — §5.22. Witnessed against the controlled
# dvxi5 probe recordings (host.sph / client.sph), the engine's own decoded view
# of the same session as the pool-records capture. Skips unless
# NW_PROFILE_SPH_DIR points at the folder holding the two .sph files.
add_executable(nw_serverlog_decode_test novaworld/nw_serverlog_decode_test.cpp)
target_link_libraries(nw_serverlog_decode_test PRIVATE opennova_novaworld)
target_include_directories(nw_serverlog_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_serverlog_decode COMMAND nw_serverlog_decode_test)

# Authored-mission cross-validation (D-NET-62) — decode the dvxi5 probe capture
# and assert every authored vehicle/marker reproduces on the wire (type_id +
# position + team + heading), not just that the body was consumed. Reads the
# real pcap directly via the shared apps/common reader; skips unless the capture
# is present (NW_DVXI5_PCAP, else the in-tree .scratch path).
add_executable(nw_pool_groundtruth_test novaworld/nw_pool_groundtruth_test.cpp)
target_link_libraries(nw_pool_groundtruth_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_pool_groundtruth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_pool_groundtruth_test PRIVATE
    FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld"
    DEFAULT_DVXI5_PCAP="${CMAKE_SOURCE_DIR}/.scratch/host_and_join_game_on_opennovaworld_loopback_mission_probe.pcapng")
add_test(NAME nw_pool_groundtruth COMMAND nw_pool_groundtruth_test)

# dvxi3 probe (probe 2, Team Deathmatch) groundtruth — successor to the dvxi5
# test, additionally cross-validating the S2C 0x10 pool-2 static-entity decoder
# (armory, oil pump, oil-field) against the authored statics. Skips unless the
# capture is present (NW_DVXI3_PCAP, else the in-tree .scratch path).
add_executable(nw_dvxi3_groundtruth_test novaworld/nw_dvxi3_groundtruth_test.cpp)
target_link_libraries(nw_dvxi3_groundtruth_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_dvxi3_groundtruth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_dvxi3_groundtruth_test PRIVATE
    FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld"
    DEFAULT_DVXI3_PCAP="${CMAKE_SOURCE_DIR}/.scratch/probe2.pcapng")
add_test(NAME nw_dvxi3_groundtruth COMMAND nw_dvxi3_groundtruth_test)

# Co-op probe 3 (dvxc1): re-validates the spawn batches AND witnesses the deferred
# Co-op bodies (0x0F waypoint records, 0x0A objective sub-block 3, 0x60 multi-chunk
# + C2S 0x33, 0x7B gameName + g_GameType). Gated on NW_DVXC1_PCAP / the .scratch
# capture; skips clean when absent.
add_executable(nw_dvxc1_groundtruth_test novaworld/nw_dvxc1_groundtruth_test.cpp)
target_link_libraries(nw_dvxc1_groundtruth_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_dvxc1_groundtruth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_dvxc1_groundtruth_test PRIVATE
    FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld"
    DEFAULT_DVXC1_PCAP="${CMAKE_SOURCE_DIR}/.scratch/probe3.pcapng")
add_test(NAME nw_dvxc1_groundtruth COMMAND nw_dvxc1_groundtruth_test)

# probe3_again: the richer dvxc1 re-run (3 players, 15 weapons, more movement).
# Asserts the NEW dimensions — RTT/trio transport volume, multi-client identity +
# death/leave lifecycle (0x26+0x13 deaths, 0x5D despawn cross-checked vs the host
# .sph oracle), weapon variety, 0x59 deployed-item, and the confirmed negatives
# (no guided 0x0C sub_ops, AI-static 0x20, Co-op roster teams==0). Gated on
# NW_PROBE3AGAIN_PCAP / the .scratch capture; skips clean when absent.
add_executable(nw_probe3again_lifecycle_test novaworld/nw_probe3again_lifecycle_test.cpp)
target_link_libraries(nw_probe3again_lifecycle_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_probe3again_lifecycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_probe3again_lifecycle_test PRIVATE
    DEFAULT_PROBE3AGAIN_PCAP="${CMAKE_SOURCE_DIR}/.scratch/probe3_again.pcapng"
    DEFAULT_PROBE3AGAIN_HOST_SPH="${CMAKE_SOURCE_DIR}/.scratch/hostprof_probe3again.sph")
add_test(NAME nw_probe3again_lifecycle COMMAND nw_probe3again_lifecycle_test)

# operation_whitenoise: the FIRST stock retail Co-op capture (not an authored
# probe). Wire-only coverage witness — asserts every catalogued Decoded tag the
# capture carries is consumed to the byte across all occurrences, plus the S2C
# 0x20 stock load-batch spot-asserts (paged, every optional flag exercised, load-
# only) and the confirmed negatives (C2S 0x0C all sub_op 0x0A, no 0x44 / 0x59).
# Gated on NW_WHITENOISE_PCAP / the .scratch capture; skips clean when absent.
add_executable(nw_whitenoise_coverage_test novaworld/nw_whitenoise_coverage_test.cpp)
target_link_libraries(nw_whitenoise_coverage_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_whitenoise_coverage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_whitenoise_coverage_test PRIVATE
    DEFAULT_WHITENOISE_PCAP="${CMAKE_SOURCE_DIR}/.scratch/operation_whitenoise.pcapng")
add_test(NAME nw_whitenoise_coverage COMMAND nw_whitenoise_coverage_test)

# Inline-pcap decoder regression — crafts tiny in-memory pcaps and round-trips
# the 0x0D / 0x20 pool decoders through the full S2C stack (encode -> frame ->
# NWU -> envelope -> build_pcap_udp -> read_pcap_udp -> decode). No capture
# fixtures, so it runs unconditionally in CI.
add_executable(nw_pool_decode_unit_test novaworld/nw_pool_decode_unit_test.cpp)
target_link_libraries(nw_pool_decode_unit_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_pool_decode_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_pool_decode_unit COMMAND nw_pool_decode_unit_test)

# Replay-timeline assembler regression — crafts an inline capture (auth pair +
# spawn + C2S 0x0C uplinks), drives the shared outer-decode pipeline
# (decode_capture_to_messages) into build_replay_timeline, and asserts the
# assembled entities + tracks. No capture fixtures, so it runs in CI.
add_executable(nw_replay_timeline_test novaworld/nw_replay_timeline_test.cpp)
target_link_libraries(nw_replay_timeline_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_replay_timeline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_replay_timeline COMMAND nw_replay_timeline_test)

# CaptureDecoder streaming-equivalence — pins that a sequence of push() calls
# yields byte-identical InGameMessages to one batch decode_capture_to_messages()
# (the contract the in-engine live net client relies on). Runs on an inline
# two-session capture unconditionally; also checks the real 3-player probe3_again
# capture when present (NW_PROBE3AGAIN_PCAP / .scratch), skipping clean otherwise.
add_executable(nw_capture_decoder_test novaworld/nw_capture_decoder_test.cpp)
target_link_libraries(nw_capture_decoder_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_capture_decoder_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_capture_decoder_test PRIVATE
    DEFAULT_PROBE3AGAIN_PCAP="${CMAKE_SOURCE_DIR}/.scratch/probe3_again.pcapng")
add_test(NAME nw_capture_decoder COMMAND nw_capture_decoder_test)

# Golden coverage diff — compare OUR freshly captured retail-join session
# (NW_GOLDEN_OURS, produced live by scripts/net/diff_vs_golden.ps1's capture leg)
# against a golden retail<->retail capture, by message coverage per (dir,tag). A
# missing or spurious S2C tag fails the build; the CI-gated counterpart to the
# diff_vs_golden.ps1 harness. Skips clean when NW_GOLDEN_OURS is unset (no
# committed "ours" oracle). Golden defaults to the gitignored .scratch/golden.
add_executable(nw_golden_diff_test novaworld/nw_golden_diff_test.cpp)
target_link_libraries(nw_golden_diff_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_golden_diff_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_golden_diff_test PRIVATE
    DEFAULT_GAMEPLAY_PCAP="${CMAKE_SOURCE_DIR}/.scratch/golden/retail-gameplay-session.pcapng")
add_test(NAME nw_golden_diff COMMAND nw_golden_diff_test)

# nw_replay role partition — pins that the replay tool splits a session into host
# + client roles from the NAPI session opcode + UDP ports, ignores gate/SSDP
# noise, and preserves each role's handshake. Compiles the partition unit
# (apps/nw_replay/nw_replay_partition.cpp) directly. No fixtures, runs in CI.
add_executable(nw_replay_partition_test
    novaworld/nw_replay_partition_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/nw_replay/nw_replay_partition.cpp)
target_link_libraries(nw_replay_partition_test PRIVATE opennova_novaworld opennova_napi opennova_novacrypto opennova_app_common opennova_pcapio)
target_include_directories(nw_replay_partition_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/apps/nw_replay)
add_test(NAME nw_replay_partition COMMAND nw_replay_partition_test)

# nw_replay CLI + playback core — every numeric option must convert whole (no
# NaN/inf/partial values reaching the pacing math) and a playback pass must
# report capture-read and UDP-send failures instead of printing "complete".
# Compiles apps/nw_replay/nw_replay_cli.cpp directly. No fixtures, runs in CI.
add_executable(nw_replay_cli_test
    novaworld/nw_replay_cli_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/nw_replay/nw_replay_cli.cpp)
target_link_libraries(nw_replay_cli_test PRIVATE opennova_pcapio)
target_include_directories(nw_replay_cli_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/apps/nw_replay)
add_test(NAME nw_replay_cli COMMAND nw_replay_cli_test)

# Streaming pcap reader — stream_pcap_udp_file must match the whole-file
# read_pcap_udp byte-for-byte and honor early-exit (the basis for nw_replay
# handling multi-GB captures without loading them into RAM). No fixtures, CI.
add_executable(nw_pcap_stream_test novaworld/nw_pcap_stream_test.cpp)
target_link_libraries(nw_pcap_stream_test PRIVATE opennova_app_common opennova_pcapio)
target_include_directories(nw_pcap_stream_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_pcap_stream COMMAND nw_pcap_stream_test)

# Per-entity compact-record decoders — §5.10 player / §5.13 vehicle / §5.14
# infantry. Hand-built input vectors mirror the wire layout; no capture
# dependency, so the test runs unconditionally in CI.
add_executable(nw_ingame_compact_records_test novaworld/nw_ingame_compact_records_test.cpp)
target_link_libraries(nw_ingame_compact_records_test PRIVATE opennova_novaworld)
target_include_directories(nw_ingame_compact_records_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_compact_records COMMAND nw_ingame_compact_records_test)

# C2S 0x0C extended (type-10) uplink decoder — §5.10. Byte vectors embedded
# verbatim from the 3-player loopback pcap (host_and_join…_threeplayers…).
add_executable(nw_ingame_c2s_uplink_test novaworld/nw_ingame_c2s_uplink_test.cpp)
target_link_libraries(nw_ingame_c2s_uplink_test PRIVATE opennova_novaworld)
target_include_directories(nw_ingame_c2s_uplink_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_c2s_uplink COMMAND nw_ingame_c2s_uplink_test)

# In-match S2C encoders — loopback identity (encode -> decode field-identical),
# ADR 0011 §4. encode side ports serialize_entity_pool_to_packet @0x503460; the
# decode side is verified vs NapiNPClientMsg_0x020 @0x425C00. No capture dep.
add_executable(nw_ingame_encode_test novaworld/nw_ingame_encode_test.cpp)
target_link_libraries(nw_ingame_encode_test PRIVATE opennova_novaworld)
target_include_directories(nw_ingame_encode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_encode COMMAND nw_ingame_encode_test)

# NET-0 tier-1 byte-stability gate (docs/maturity-program.md): FNV-1a64 vectors
# over a deterministic synthetic corpus through every in-match encoder. Catches
# SYMMETRIC codec changes the encode<->decode round-trip cannot see; vectors
# update only with a wire witness. Always-run, no capture dep.
add_executable(nw_codec_identity_test novaworld/nw_codec_identity_test.cpp)
target_link_libraries(nw_codec_identity_test PRIVATE opennova_novaworld)
target_include_directories(nw_codec_identity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_codec_identity COMMAND nw_codec_identity_test)

# NET-0 tier-1 self-capture gate — nw_golden_diff's SELF mode: a deterministic
# in-process opennova host + opennova joiner session (the host_session owner loop
# + ClientRuntime over an in-memory IDatagramSocket) coverage-diffed per (dir,tag)
# against the COMMITTED opennova-produced fixture (LFS, zero retail bytes) with
# EXACT set equality — no noise floor, both sides are ours. Cannot skip; a missing
# fixture fails. Regen: OPENNOVA_WRITE_SELF_FIXTURE=1 (justify the delta in-commit).
add_executable(nw_self_capture_test novaworld/nw_self_capture_test.cpp)
target_link_libraries(nw_self_capture_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world opennova_novaworld
            opennova_mission opennova_app_common opennova_pcapio)
target_include_directories(nw_self_capture_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nw_self_capture_test PRIVATE
    SELF_CAPTURE_FIXTURE="${CMAKE_SOURCE_DIR}/fixtures/novaworld/self-capture-session.pcap")
add_test(NAME nw_self_capture COMMAND nw_self_capture_test)

# In-game message catalog coverage gate — asserts every Decoded tag's decoder
# consumes a representative body to the byte and the catalog stays consistent
# (the net analog of ADR 0002's MNU key-coverage). Self-contained, always-run.
add_executable(nw_message_coverage_test novaworld/nw_message_coverage_test.cpp)
target_link_libraries(nw_message_coverage_test PRIVATE opennova_novaworld)
target_include_directories(nw_message_coverage_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_message_coverage COMMAND nw_message_coverage_test)

# §5.15 guided weapon record — per-(mode, field-group) encode<->decode round-trip
# pinning the structural port of Entity_SerializeGuidedMissileState. Self-contained.
add_executable(nw_ingame_guided_test novaworld/nw_ingame_guided_test.cpp)
target_link_libraries(nw_ingame_guided_test PRIVATE opennova_novaworld)
target_include_directories(nw_ingame_guided_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME nw_ingame_guided COMMAND nw_ingame_guided_test)

# Backend schema regression — applies the actual backend/migrations/* and
# backend/seed/* the standalone server uses, so SQL syntax errors and seed
# regressions break CI before they break the running server.
# Retail-capture replay — pins the ProtocolMessage codec and the RE-doc
# §5.4/§5.5 BMS-bundle facts against the 2026-04-26 live JO captures.
add_executable(nwmsg_replay_test novaworld/nwmsg_replay_test.cpp)
target_link_libraries(nwmsg_replay_test PRIVATE opennova_novaworld)
target_include_directories(nwmsg_replay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(nwmsg_replay_test PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld")
add_test(NAME nwmsg_replay COMMAND nwmsg_replay_test)

# Host-row staleness backstop sweep (prune_stale_hosts).
add_executable(host_prune_test novaworld/host_prune_test.cpp)
target_link_libraries(host_prune_test PRIVATE opennova_novaworld)
target_include_directories(host_prune_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(host_prune_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME host_prune COMMAND host_prune_test)

# End-to-end host register -> read-back (the path /api/lobbies + /jop_2.gsb use).
add_executable(host_register_list_test novaworld/host_register_list_test.cpp)
target_link_libraries(host_register_list_test PRIVATE opennova_novaworld opennova_napi)
target_include_directories(host_register_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(host_register_list_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME host_register_list COMMAND host_register_list_test)

add_executable(backend_schema_test novaworld/backend_schema_test.cpp)
target_link_libraries(backend_schema_test PRIVATE opennova_novaworld)
target_include_directories(backend_schema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(backend_schema_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME backend_schema COMMAND backend_schema_test)

# Expansion-release / publish repository writes (the SQL ported from onnet).
# Compiles catalog_repository.cpp straight in — no Crow/libcurl needed, the
# write layer depends only on the sqlite wrapper in opennova_novaworld.
add_executable(expansion_publish_test
    novaworld/expansion_publish_test.cpp
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server/catalog_repository.cpp)
target_link_libraries(expansion_publish_test PRIVATE opennova_novaworld)
target_include_directories(expansion_publish_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/novaworld_server)
target_compile_definitions(expansion_publish_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME expansion_publish COMMAND expansion_publish_test)

# Unknown-message tracker: record/dedup, sample cap, flush/upsert against the
# real 0004 migration (OPENNOVA_SOURCE_DIR for the SQL on disk), snapshot.
add_executable(unknown_tracker_test novaworld/unknown_tracker_test.cpp)
target_link_libraries(unknown_tracker_test PRIVATE opennova_novaworld)
target_include_directories(unknown_tracker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(unknown_tracker_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME unknown_tracker COMMAND unknown_tracker_test)

# netsim tests — the in-match net seam (ADR 0009/0011).
# The in-process loopback transport.
add_executable(netsim_loopback_channel_test netsim/loopback_channel_test.cpp)
target_link_libraries(netsim_loopback_channel_test PRIVATE opennova_netsim)
add_test(NAME netsim_loopback_channel COMMAND netsim_loopback_channel_test)

# The ADR 0011 §4 loopback identity guard: World entity -> encode -> loopback ->
# decode -> ClientState, field-identical to what the host's encoder + codec produced.
add_executable(netsim_loopback_identity_test netsim/loopback_identity_test.cpp)
target_link_libraries(netsim_loopback_identity_test PRIVATE opennova_netsim)
add_test(NAME netsim_loopback_identity COMMAND netsim_loopback_identity_test)

# The co-op-LAN fan-out guard: one World snapshot fanned to a connection table (loopback
# self + a socket-free UdpSessionTransport joiner), per-connection anchoring, and the
# host-side C2S 0x0C drain/SNAP — no Godot, no OS sockets (Increment A).
add_executable(netsim_two_peer_fanout_test netsim/two_peer_fanout_test.cpp)
target_link_libraries(netsim_two_peer_fanout_test PRIVATE opennova_netsim)
add_test(NAME netsim_two_peer_fanout COMMAND netsim_two_peer_fanout_test)

add_executable(netsim_build_player_uplink_test netsim/build_player_uplink_test.cpp)
target_link_libraries(netsim_build_player_uplink_test PRIVATE opennova_netsim)
add_test(NAME netsim_build_player_uplink COMMAND netsim_build_player_uplink_test)

# The §5.10b decode-dispatch guard: the client view's items.def class resolver sizes
# 0x0A tag-1 records (header-only no-callback vs vehicle/infantry compacts) and
# outranks the 0x0D pool-blanket learner — a mis-sized record desyncs the frame and
# scatters every later record around the recipient anchor.
add_executable(netsim_client_view_arms_dip_test netsim/client_view_arms_dip_test.cpp)
target_link_libraries(netsim_client_view_arms_dip_test PRIVATE opennova_netsim)
add_test(NAME netsim_client_view_arms_dip COMMAND netsim_client_view_arms_dip_test)

add_executable(netsim_client_view_recoil_test netsim/client_view_recoil_test.cpp)
target_link_libraries(netsim_client_view_recoil_test PRIVATE opennova_netsim)
add_test(NAME netsim_client_view_recoil COMMAND netsim_client_view_recoil_test)

add_executable(netsim_client_view_class_resolver_test netsim/client_view_class_resolver_test.cpp)
target_link_libraries(netsim_client_view_class_resolver_test PRIVATE opennova_netsim)
add_test(NAME netsim_client_view_class_resolver COMMAND netsim_client_view_class_resolver_test)

add_executable(netsim_client_view_anim_pulse_test netsim/client_view_anim_pulse_test.cpp)
target_link_libraries(netsim_client_view_anim_pulse_test PRIVATE opennova_netsim)
add_test(NAME netsim_client_view_anim_pulse COMMAND netsim_client_view_anim_pulse_test)

add_executable(netsim_world_stream_extractors_test netsim/world_stream_extractors_test.cpp)
target_link_libraries(netsim_world_stream_extractors_test PRIVATE opennova_netsim)
add_test(NAME netsim_world_stream_extractors COMMAND netsim_world_stream_extractors_test)

# The non-COOP follower repro: replays the retail vehicle-session golden through
# NetClientView and pins every organic-parented pool-1 spawn (§5.11 spawnFlags
# 0x0100) to its own §5.10b compact wire positions. Env-gated: needs the
# gitignored golden (NW_GOLDEN_VEHICLE_SESSION, DEFAULT below) and an items.def
# class table (NW_ITEMS_DEF); skips clean when either is absent.
add_executable(netsim_client_view_capture_parent_follow_test
    netsim/client_view_capture_parent_follow_test.cpp)
target_link_libraries(netsim_client_view_capture_parent_follow_test
    PRIVATE opennova_netsim opennova_npwire opennova_def opennova_scr
    opennova_app_common opennova_pcapio)
target_compile_definitions(netsim_client_view_capture_parent_follow_test PRIVATE
    DEFAULT_VEHICLE_SESSION_PCAP="${CMAKE_SOURCE_DIR}/.scratch/golden/retail-vehicle-session.pcapng")
add_test(NAME netsim_client_view_capture_parent_follow
    COMMAND netsim_client_view_capture_parent_follow_test)

# npruntime tests — the IDA-faithful in-match NAPI NP runtime (the named server/client
# state machines + frame loop). Server-first (the plan's P0..P8).
# P0/P1: the in-process listen-server bring-up state writes (§5.0).
add_executable(npruntime_server_session_test npruntime/server_session_test.cpp)
target_link_libraries(npruntime_server_session_test PRIVATE opennova_npruntime)
add_test(NAME npruntime_server_session COMMAND npruntime_server_session_test)

# P3: the World-driven host-side spawn pipeline (Server_InitNewRoundState ->
# ProcessPendingPlayerSpawns -> Server_BuildPlayerInfoAndAdd) — spawns the pool-0 player in World,
# stamping entity+0x78 ownerConnectionId; then the §1 dcb wiring surfaces it in the 0x0C batch.
add_executable(npruntime_server_spawn_test npruntime/server_spawn_test.cpp)
target_link_libraries(npruntime_server_spawn_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world opennova_novaworld)
add_test(NAME npruntime_server_spawn COMMAND npruntime_server_spawn_test)

# P3: the §5.2a two-track initial-state burst machine (Server_SendInitialGameStateToPlayer) over a
# real World + bms::File — emitted tag order + per-tag body round-trip + the entity+0x78 dcb wiring.
add_executable(npruntime_initial_state_burst_test npruntime/initial_state_burst_test.cpp)
target_link_libraries(npruntime_initial_state_burst_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world opennova_novaworld opennova_mission)
add_test(NAME npruntime_initial_state_burst COMMAND npruntime_initial_state_burst_test)

# ADR 0013: the shared byte-budget spawn/world-stream chunker (np::slice_batch_pages) the §5.2a burst
# pages every world pool through — retail pool margins, exact tile pages, oversized record, cursor resume.
add_executable(npruntime_batch_chunker_test npruntime/batch_chunker_test.cpp)
target_link_libraries(npruntime_batch_chunker_test PRIVATE opennova_npruntime)
add_test(NAME npruntime_batch_chunker COMMAND npruntime_batch_chunker_test)

# D-NET-141: the weapon.def armory table — witnessed index allocation (null@0 + file order),
# ammo-clip resolution, and charfilter/teamfilter masks, against the committed fixture; §5.57.
add_executable(npruntime_weapon_table_test npruntime/weapon_table_test.cpp)
target_link_libraries(npruntime_weapon_table_test
    PRIVATE opennova_npruntime opennova_world opennova_def)
target_include_directories(npruntime_weapon_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_weapon_table COMMAND npruntime_weapon_table_test)

# D-NET-168 / D-NET-176 / D-NET-177: the S2C 0x50 team-assign re-latch + 0x2F re-submit, the
# C2S 0x32 -> S2C 0x5D empty-slot sweep (entity removal) vs the bookkeeping-only 0x46 bit15, and
# the in-match session-loss surface [orig: @0x431910 / @0x51a600 + @0x429730 / @0x4ca4a0 -> @0x4c63d0].
add_executable(npruntime_entity_lifecycle_net_test npruntime/entity_lifecycle_net_test.cpp)
target_link_libraries(npruntime_entity_lifecycle_net_test
    PRIVATE opennova_npruntime opennova_netsim opennova_novaworld opennova_world)
add_test(NAME npruntime_entity_lifecycle_net COMMAND npruntime_entity_lifecycle_net_test)

# D-NET-142: the C2S 0x25 reload-request relay — dispatch broadcasts S2C 0x49 onto every in-match
# connection's transport (requester included) [orig: @0x514DF0 -> SendFiltered @0x4C87E0]; §5.58.
add_executable(npruntime_reload_relay_test npruntime/reload_relay_test.cpp)
target_link_libraries(npruntime_reload_relay_test
    PRIVATE opennova_npruntime opennova_netsim opennova_novaworld opennova_world)
add_test(NAME npruntime_reload_relay COMMAND npruntime_reload_relay_test)

# D-NET-152: the C2S 0x06 client-fired-round pipeline — anti-spoof, clip authority (+ the 0x25
# refill), and the g_round_ring append behind the §5.9.1 tag-2 echo
# [orig: @0x513310 -> Server_ClientFiredRound @0x50baa0 -> RoundData_AddRound @0x4fdb40].
add_executable(npruntime_client_fire_test npruntime/client_fire_test.cpp)
target_link_libraries(npruntime_client_fire_test
    PRIVATE opennova_npruntime opennova_netsim opennova_novaworld opennova_world)
add_test(NAME npruntime_client_fire COMMAND npruntime_client_fire_test)

# §5.62: the authoritative round sim — fire -> synchronous spawn [orig: RoundData_AddRound
# @0x4fdb40 inline RoundData_SpawnRound @0x4ec0d0], flight + hit [orig:
# Weapon_UpdateAllProjectiles @0x4ec020 -> Projectile_UpdatePhysics @0x4e9d70], the kinetic
# damage number [orig: Weapon_CalcImpactDamage @0x4ec920], and the death routing + respawn
# release [orig: Entity_CheckAndProcessDeath @0x51b550 -> S2C 0x13/0x1E].
add_executable(npruntime_round_sim_test npruntime/round_sim_test.cpp)
target_link_libraries(npruntime_round_sim_test
    PRIVATE opennova_npruntime opennova_netsim opennova_novaworld opennova_world)
add_test(NAME npruntime_round_sim COMMAND npruntime_round_sim_test)

# The SP round-outcome loop (world-wac-ai-re §20): kill tallies -> the WAC
# bluekills/greenkills source, the humans count, the SP auto-lose condition
# [orig: Server_CheckWinConditions @0x51ad40], the round-end latch + host effect
# [orig: Server_ProcessRoundEnd @0x5164f0], and the post-round respawn hold.
add_executable(npruntime_round_end_test npruntime/round_end_test.cpp)
target_link_libraries(npruntime_round_end_test
    PRIVATE opennova_npruntime opennova_netsim opennova_novaworld opennova_world)
add_test(NAME npruntime_round_end COMMAND npruntime_round_end_test)

# P2: the per-connection handshake — the server legs over the np free functions + the reactive §5.1
# reply path (handshake_server), and the golden 0x81/0x82 byte-parity. (The old World-less
# joiner_connection_test, which drove the client mirror through the retired game_runtime spawn-gate
# mirror, was removed at P8: the real JoinerConnection join->spawn->C2S->SNAP flow is covered on the
# World path by npruntime_client_runtime + npruntime_two_endpoint_socket.)
add_executable(npruntime_handshake_server_test npruntime/handshake_server_test.cpp)
target_link_libraries(npruntime_handshake_server_test
    PRIVATE opennova_npruntime opennova_netsim opennova_novaworld opennova_world opennova_def)
target_include_directories(npruntime_handshake_server_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_handshake_server COMMAND npruntime_handshake_server_test)

# Golden 0x81/0x82 byte-parity vs the local LAN host/join session capture. Env-gated + skip-if-absent
# (no committed derived oracle); DEFAULT path points at the gitignored .scratch golden.
add_executable(npruntime_golden_lan_join_session_test npruntime/golden_lan_join_session_test.cpp)
target_link_libraries(npruntime_golden_lan_join_session_test
    PRIVATE opennova_npruntime opennova_novaworld opennova_app_common opennova_pcapio)
target_compile_definitions(npruntime_golden_lan_join_session_test PRIVATE
    DEFAULT_LAN_JOIN_SESSION_PCAP="${CMAKE_SOURCE_DIR}/.scratch/golden/retail-lan-host-join-session.pcapng")
add_test(NAME npruntime_golden_lan_join_session COMMAND npruntime_golden_lan_join_session_test)

# P3 golden — validate the §5.2a initial-state burst ORDER against the retail LAN host/join capture
# (handshake + world-load). Env-gated + skip-if-absent (no committed derived oracle).
add_executable(npruntime_golden_lan_join_test npruntime/golden_lan_join_test.cpp)
target_link_libraries(npruntime_golden_lan_join_test
    PRIVATE opennova_novaworld opennova_app_common opennova_pcapio)
target_compile_definitions(npruntime_golden_lan_join_test PRIVATE
    DEFAULT_LAN_JOIN_PCAP="${CMAKE_SOURCE_DIR}/.scratch/golden/retail-lan-host-join.pcapng")
add_test(NAME npruntime_golden_lan_join COMMAND npruntime_golden_lan_join_test)

# P4 golden — np::Server_TickUpdate round-trips a REAL retail C2S 0x0C uplink into an S2C 0x0A frame
# (0x0C-in -> SNAP -> 0x0A-out), sourced from the in-match gameplay capture. Env-gated + skip-if-absent
# (no committed derived oracle); unlike the order-only lan_join golden this drives the npruntime runtime,
# so it links opennova_npruntime + opennova_netsim + opennova_world.
add_executable(npruntime_golden_gameplay_test npruntime/golden_gameplay_test.cpp)
target_link_libraries(npruntime_golden_gameplay_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world opennova_novaworld opennova_app_common opennova_pcapio)
target_compile_definitions(npruntime_golden_gameplay_test PRIVATE
    DEFAULT_GAMEPLAY_PCAP="${CMAKE_SOURCE_DIR}/.scratch/golden/retail-gameplay-session.pcapng")
add_test(NAME npruntime_golden_gameplay COMMAND npruntime_golden_gameplay_test)

# P5 — the headless CLIENT runtime (np::ClientRuntime / Client_ProcessNetworkFrame). Always-on
# in-process round-trip (client_runtime <-> np server legs <-> Server_TickUpdate + apply_in_match_c2s)
# plus the host-as-client D-NET-121/122 anchor/fold path. No capture needed.
add_executable(npruntime_client_runtime_test npruntime/client_runtime_test.cpp)
target_link_libraries(npruntime_client_runtime_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world opennova_novaworld)
add_test(NAME npruntime_client_runtime COMMAND npruntime_client_runtime_test)

# Retail in-match loss recovery: future 0x43/0x83 packets trigger direction-specific 0x84/0x44
# missing-sequence lists, and each endpoint reconstructs retained reliable records under the old
# sequence with its current ACK. [orig: SendMissingSeqList @0x623560; HandleResendList @0x623800]
add_executable(npruntime_session_resend_test npruntime/session_resend_test.cpp)
target_link_libraries(npruntime_session_resend_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world opennova_novaworld)
add_test(NAME npruntime_session_resend COMMAND npruntime_session_resend_test)

# The host-initiated close (the connection-description record, settings flag + tag 3) pinned against
# the 80 captured bytes of a live retail deploy-screen idle punt, plus the S2C 0x30/0x31 anti-cheat
# CRC challenges a stock host streams at every joiner. [orig: Server_LogCRCMismatchPunt @0x517ed0 ->
# NapiNPDataTransfer_SendDescription @0x628c80; CNapiNPConnection_HandleDescriptionPacket @0x621ae0;
# NapiNPClientMsg_HandleChecksumRequest @0x431170; NapiNPClientMsg_0x031 @0x4311e0]
add_executable(npruntime_host_punt_test npruntime/host_punt_test.cpp)
target_link_libraries(npruntime_host_punt_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world)
add_test(NAME npruntime_host_punt COMMAND npruntime_host_punt_test)

# Loss-tolerant pre-session UDP join: dropped 0x41/0x42 datagrams are retried on the retail active
# send interval without changing their wire bytes, and the retried legs complete against the real host.
add_executable(npruntime_joiner_handshake_retry_test npruntime/joiner_handshake_retry_test.cpp)
target_link_libraries(npruntime_joiner_handshake_retry_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world)
target_include_directories(npruntime_joiner_handshake_retry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME npruntime_joiner_handshake_retry COMMAND npruntime_joiner_handshake_retry_test)

# P5 Level-3 golden — drive the REAL client emission path (np::JoinerConnection::frame_c2s_uplink via
# client_runtime) seeded from the gameplay capture and assert the emitted C2S 0x0C datagram is
# byte-identical to the captured client datagram (emitted C2S == client-origin); plus fold the
# capture's S2C 0x0A through NetClientView and cross-check decompressed positions against the
# world-stream absolute spawn positions (an independent in-pcap oracle, not the same decoder). Default
# resolves the gitignored golden under .scratch/golden; env-gated NW_GOLDEN_GAMEPLAY; skip-if-absent.
add_executable(npruntime_golden_client_test npruntime/golden_client_test.cpp)
target_link_libraries(npruntime_golden_client_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world opennova_novaworld opennova_app_common opennova_pcapio)
target_compile_definitions(npruntime_golden_client_test PRIVATE
    DEFAULT_GAMEPLAY_PCAP="${CMAKE_SOURCE_DIR}/.scratch/golden/retail-gameplay-session.pcapng")
add_test(NAME npruntime_golden_client COMMAND npruntime_golden_client_test)

# P6 — two endpoints over REAL UDP sockets: a ClientRuntime joiner and the apps/nw_server host
# owner-loop (npruntime/host_session.h, the SAME loop apps/nw_server runs), each on its own bound UDP
# over the wire (single-thread poll-pump). Asserts mirror npruntime_client_runtime but over real
# sockets, and adds the §5.2a S2C tag-order check (always-on, our host) + the env-gated cross-check vs
# the retail LAN host/join capture (NW_GOLDEN_LAN_JOIN, skip-clean). opennova_mission for the minimal
# World/bms; opennova_app_common for net_sockets + net_datagram_socket + pcap_reader (ws2_32 on WIN32).
add_executable(npruntime_two_endpoint_socket_test npruntime/two_endpoint_socket_test.cpp)
target_link_libraries(npruntime_two_endpoint_socket_test
    PRIVATE opennova_npruntime opennova_netsim opennova_world opennova_novaworld
            opennova_mission opennova_app_common opennova_pcapio)
target_compile_definitions(npruntime_two_endpoint_socket_test PRIVATE
    DEFAULT_LAN_JOIN_PCAP="${CMAKE_SOURCE_DIR}/.scratch/golden/retail-lan-host-join.pcapng")
if(WIN32)
    target_link_libraries(npruntime_two_endpoint_socket_test PRIVATE ws2_32)
endif()
add_test(NAME npruntime_two_endpoint_socket COMMAND npruntime_two_endpoint_socket_test)

# NET-4 (ADR 0019 §5): the C-ABI export-identity guard. The shared library's
# flat export list must match scripts/lint/abi_exports_baseline.txt, and no
# net-lib symbol family may appear at all. Runs wherever BUILD_SHARED_LIB=ON
# builds run ctest (scripts/build.sh, the CI build-and-test job).
if(BUILD_SHARED_LIB)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME abi_export_identity
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/scripts/lint/abi_exports_check.py"
                    --lib "$<TARGET_FILE:opennova_shared>")
    endif()
endif()

# weapon.sav player-profile format tests (libs/playersav).
add_subdirectory(playersav)
