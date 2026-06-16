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

# ADM tests
foreach(test comment trim_value parse write)
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

# Mission (BMS) tests
add_executable(mission_authoring_test mission/mission_authoring_test.cpp)
target_link_libraries(mission_authoring_test PRIVATE opennova_mission)
target_include_directories(mission_authoring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_authoring COMMAND mission_authoring_test)

add_executable(mission_bms_test mission/mission_bms_test.cpp)
target_link_libraries(mission_bms_test PRIVATE opennova_mission)
target_include_directories(mission_bms_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_bms COMMAND mission_bms_test)

add_executable(mission_c_abi_test mission/mission_c_abi_test.cpp)
target_link_libraries(mission_c_abi_test PRIVATE opennova_mission)
target_include_directories(mission_c_abi_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_c_abi COMMAND mission_c_abi_test)

add_executable(mission_schema_test mission/mission_schema_test.cpp)
target_link_libraries(mission_schema_test PRIVATE opennova_mission)
target_include_directories(mission_schema_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mission_schema COMMAND mission_schema_test)

# Corpus round-trip: parse->write byte-exact over a directory of real shipped .bms missions.
# Gated on OPENNOVA_MISSION_CORPUS (copyrighted assets, not committed); skips + passes when unset.
add_executable(mission_corpus_test mission/mission_corpus_test.cpp)
target_link_libraries(mission_corpus_test PRIVATE opennova_mission)
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
foreach(test light_runtime material_classify material_eval)
    add_executable(renderer_${test}_test renderer/${test}_test.cpp)
    target_link_libraries(renderer_${test}_test PRIVATE opennova_renderer opennova_threedi)
    target_include_directories(renderer_${test}_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/libs/oed/include)
    target_compile_features(renderer_${test}_test PRIVATE cxx_std_17)
    add_test(NAME renderer_${test} COMMAND renderer_${test}_test)
endforeach()

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

add_executable(foliage_map_remap_indices_test foliage/foliage_map_remap_indices_test.cpp)
target_link_libraries(foliage_map_remap_indices_test PRIVATE opennova_foliage)
target_include_directories(foliage_map_remap_indices_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME foliage_map_remap_indices COMMAND foliage_map_remap_indices_test)

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
    add_executable(${name}_test trngen/${name}_test.cpp)
    target_link_libraries(${name}_test PRIVATE opennova_terrain)
    target_include_directories(${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

add_executable(terrain_lighting_test trngen/terrain_lighting_test.cpp)
target_link_libraries(terrain_lighting_test PRIVATE opennova_terrain)
target_include_directories(terrain_lighting_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME terrain_lighting COMMAND terrain_lighting_test)

# parametric_parity is a long byte-identical fixture parity run and remains
# disabled until its CI stability/performance cost is addressed.
set_tests_properties(parametric_parity PROPERTIES DISABLED TRUE)

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

# CDEP per-block range constraint: the editor-side raw16 clamp/count kernel,
# ported from terrain_editor_cdep_constraint.gd into libs/terrain.
add_executable(cdep_constraint_test trngen/cdep_constraint_test.cpp)
target_link_libraries(cdep_constraint_test PRIVATE opennova_terrain)
target_include_directories(cdep_constraint_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cdep_constraint COMMAND cdep_constraint_test)

# Terrain world->source coordinate transform kernel (libs/terrain/coords).
# Characterizes the runtime resolve_world_sample formula plus the editor-mode
# guard divergences (bounds-reject, id-clamp, local-clamp) ported from
# EditorTerrainMesh.world_to_source_coords / world_to_cell_source_coords.
add_executable(coords_test trngen/coords_test.cpp)
target_link_libraries(coords_test PRIVATE opennova_terrain)
target_include_directories(coords_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME coords COMMAND coords_test)

# Height brush kernels (libs/terrain/brush): raise/lower, smooth (two-pass),
# flatten + the shared smoothstep falloff, ported from terrain_editor_brushes.gd.
add_executable(brush_height_test trngen/brush_height_test.cpp)
target_link_libraries(brush_height_test PRIVATE opennova_terrain)
target_include_directories(brush_height_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME brush_height COMMAND brush_height_test)

# Colour/blend brush kernels (libs/terrain/brush) on RGBA8 buffers. Byte-parity
# critical: pins Godot's FORMAT_RGBA8 quantization + the blend/lerp/clone math.
add_executable(brush_color_test trngen/brush_color_test.cpp)
target_link_libraries(brush_color_test PRIVATE opennova_terrain)
target_include_directories(brush_color_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME brush_color COMMAND brush_color_test)

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
add_executable(mnu_xml_unit_test mnu_xml/mnu_xml_unit_test.cpp)
target_link_libraries(mnu_xml_unit_test PRIVATE opennova_mnu_xml)
target_include_directories(mnu_xml_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME mnu_xml_unit COMMAND mnu_xml_unit_test)

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

# AI subsystem (byte-exact foundation): state enum + struct layout, AIEvent ring,
# budget gate (AI_BeginUpdate), the infantry state-machine dispatcher + transitions,
# and the trivial enter/exit/event handler ports.
add_executable(ai_test world/ai_test.cpp)
target_link_libraries(ai_test PRIVATE opennova_world)
target_include_directories(ai_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ai_test PRIVATE cxx_std_17)
add_test(NAME ai COMMAND ai_test)

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
add_test(NAME infantry COMMAND infantry_test)

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
    target_link_libraries(refs_${test}_test PRIVATE opennova_refs opennova_threedi opennova_mission opennova_mnu)
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

# Byte-level oracle: decode the genuine .204 server-browser blob
# (fixtures/novaworld/nw204_jop_2.gsb) through our retail-faithful parser.
add_executable(gsb_real204_decode_test novaworld/gsb_real204_decode_test.cpp)
target_link_libraries(gsb_real204_decode_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(gsb_real204_decode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gsb_real204_decode_test PRIVATE FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures/novaworld")
add_test(NAME gsb_real204_decode COMMAND gsb_real204_decode_test)

add_executable(protocol_message_test novaworld/protocol_message_test.cpp)
target_link_libraries(protocol_message_test PRIVATE opennova_novaworld opennova_novacrypto)
target_include_directories(protocol_message_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME protocol_message COMMAND protocol_message_test)

add_executable(game_session_test novaworld/game_session_test.cpp)
target_link_libraries(game_session_test PRIVATE opennova_novaworld)
target_include_directories(game_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME game_session COMMAND game_session_test)

add_executable(game_server_runtime_test novaworld/game_server_runtime_test.cpp)
target_link_libraries(game_server_runtime_test PRIVATE opennova_novaworld)
target_include_directories(game_server_runtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME game_server_runtime COMMAND game_server_runtime_test)

add_executable(replication_min_test novaworld/replication_min_test.cpp)
target_link_libraries(replication_min_test PRIVATE opennova_novaworld)
target_include_directories(replication_min_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME replication_min COMMAND replication_min_test)

# bms (mission-file) parser tests
add_executable(bms_spawn_points_test bms/spawn_points_test.cpp)
target_link_libraries(bms_spawn_points_test PRIVATE opennova_bms)
target_include_directories(bms_spawn_points_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME bms_spawn_points COMMAND bms_spawn_points_test)

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

# Unknown-message tracker: record/dedup, sample cap, flush/upsert against the
# real 0004 migration (OPENNOVA_SOURCE_DIR for the SQL on disk), snapshot.
add_executable(unknown_tracker_test novaworld/unknown_tracker_test.cpp)
target_link_libraries(unknown_tracker_test PRIVATE opennova_novaworld)
target_include_directories(unknown_tracker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(unknown_tracker_test PRIVATE OPENNOVA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME unknown_tracker COMMAND unknown_tracker_test)

