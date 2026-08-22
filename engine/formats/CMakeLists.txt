# opennova_formats — every NovaLogic format parser/writer (one directory per
# format, ADR 0024), built as one STATIC group target (Shape A).
#
# GUARDRAIL (non-negotiable): the mission FORMAT half — .mis/.bms parse+write,
# schema, authoring, the C ABI — belongs to THIS group. Without that
# assignment there is no acyclic 4-group partition: base/refs -> the mission
# format while runtime/simassets -> base/resource_index would cycle base and
# runtime. Since ADR 0030 the directory matches the fold
# (engine/formats/mission); the runtime half stays engine/runtime/mission.

add_library(opennova_formats STATIC
    # adm — .adm animation definition map (anim slots -> .bad clip variant rings)
    ${CMAKE_CURRENT_SOURCE_DIR}/adm/adm.cpp
    # aip — AI profile text (PARTIAL PORT: the two witnessed speed keys; the
    # remainder is the ledgered D-AI-11(h) gap and lands here when witnessed)
    ${CMAKE_CURRENT_SOURCE_DIR}/aip/aip.cpp
    # ase — 3ds Max ASCII scene export (the OED model source format)
    ${CMAKE_CURRENT_SOURCE_DIR}/ase/ase_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ase/ase_writer.cpp
    # avatars — Avatars.def
    ${CMAKE_CURRENT_SOURCE_DIR}/avatars/avatars.cpp
    # bad — skeletal animation
    ${CMAKE_CURRENT_SOURCE_DIR}/bad/bad.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bad/bad_write.cpp
    # bink — native BIKi menu-video decode
    ${CMAKE_CURRENT_SOURCE_DIR}/bink/bink.cpp
    # bfc1 — compressed container (vendored miniz stays in-tree here)
    ${CMAKE_CURRENT_SOURCE_DIR}/bfc1/bfc1.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bfc1/miniz.c
    # cbin — credits binary
    ${CMAKE_CURRENT_SOURCE_DIR}/cbin/cbin.cpp
    # cpt — terrain height/CDEP container
    ${CMAKE_CURRENT_SOURCE_DIR}/cpt/cpt_crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cpt/cpt_io.cpp
    # dbf — dialog bank
    ${CMAKE_CURRENT_SOURCE_DIR}/dbf/dbf.cpp
    # def — the .def text tables
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def.cpp          # the .def umbrella + loadout weight/encumbrance
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_ammo.cpp     # AMMO.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_hudpos.cpp   # HUDPOS.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_items.cpp    # ITEMS.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_scan.cpp     # the shared .def text scanner
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_weapons.cpp  # WEAPONS.DEF
    # dep — the raw 1024x1024 u16 depth-buffer bake intermediate (Output.dep)
    ${CMAKE_CURRENT_SOURCE_DIR}/dep/dep.cpp
    # env — environment/time-of-day
    ${CMAKE_CURRENT_SOURCE_DIR}/env/env.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/env/env_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/env/env_weather_core.cpp
    # fnt — bitmap fonts
    ${CMAKE_CURRENT_SOURCE_DIR}/fnt/fnt.c
    # foliage — foliage defs + placement map
    ${CMAKE_CURRENT_SOURCE_DIR}/foliage/foliage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/foliage/runtime.cpp
    # lwf — sound profiles (+ the .wav payload decode the profiles reference)
    ${CMAKE_CURRENT_SOURCE_DIR}/lwf/lwf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lwf/wav_pcm.cpp
    # mission — .bms/.mis missions: document model, schema reflection,
    # authoring edits, and the flat C ABI. Standalone (no world/runtime
    # dependency); the runtime half — event runtime, promotion, boot — lives
    # in engine/runtime/mission, both exposing <mission/...> with disjoint
    # header sets (the ADR 0029 acyclicity fold; directory home since
    # ADR 0030).
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/authoring.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/bms.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission.cpp          # MissionDocument — the document and its edit operations
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_capi.cpp     # the flat C ABI over it
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_mis_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_mis_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_names.cpp    # BMS event-logic enum display names
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_records.cpp  # bms:: structs <-> typed editor records
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_schema.cpp
    # mns — menu stylesheet
    ${CMAKE_CURRENT_SOURCE_DIR}/mns/mns.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mns/mns_document.cpp
    # mnu — menu UI
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/mnu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/mnu_layout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/mnu_xml.cpp
    # mus — music script bytecode
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_parse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_decompile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_compile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_vm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_ast.cpp
    # oed — the OED project/convert/export pipeline
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/oed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/convert_internal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/export_3di.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/rdta.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/material_utils.cpp
    # pcx — image
    ${CMAKE_CURRENT_SOURCE_DIR}/pcx/pcx_io.cpp
    # pff — the archive format
    ${CMAKE_CURRENT_SOURCE_DIR}/pff/pff.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pff/pff_writer.cpp
    # playersav — weapon.sav player profile
    ${CMAKE_CURRENT_SOURCE_DIR}/playersav/weapon_sav.cpp
    # ptl — particle effects (.ptl): model + parser + writer + enum/field tables;
    # historical include prefix particle/ (the emitter/effect-scene sim stays in
    # engine/runtime/particle — disjoint header sets, ADR 0030 decision 2)
    ${CMAKE_CURRENT_SOURCE_DIR}/particle/parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/particle/particle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/particle/writer.cpp
    # rtxt — localized string tables
    ${CMAKE_CURRENT_SOURCE_DIR}/rtxt/rtxt.cpp
    # sbf — sound banks
    ${CMAKE_CURRENT_SOURCE_DIR}/sbf/sbf.cpp
    # scr — obfuscated text container
    ${CMAKE_CURRENT_SOURCE_DIR}/scr/scr.cpp
    # sph — the /PROFILE server-log FOURCC chunk container (payload records:
    # engine/net/npwire serverlog_decode)
    ${CMAKE_CURRENT_SOURCE_DIR}/sph/sph.cpp
    # tdp — OED project structs
    ${CMAKE_CURRENT_SOURCE_DIR}/tdp/tdp.cpp
    # threedi — the .3di model format (3DI3 first-class, ADR 0027)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_3di3_read.cpp   # 3DI3 -> Threedi3di3
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_3di3_write.cpp  # Threedi3di3 -> 3DI3 (parity writer, ADR 0003)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_3di3_ground_anchor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_compare.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_ctrl_catalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm_wave.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm_matrices.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm_pose.cpp
    # til — terrain tiles
    ${CMAKE_CURRENT_SOURCE_DIR}/til/til_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/til/til_foliage_blocker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/til/til_overlay_bake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/til/til_tsd.cpp
    # tpj — terrain project
    ${CMAKE_CURRENT_SOURCE_DIR}/tpj/tpj_io.cpp
    # tpm — TPM1 terrain tile mesh (.tml/.tms; magic-shaped name, precedent
    # bfc1). packing.{h,cpp} are the on-disk index codecs, src-private.
    ${CMAKE_CURRENT_SOURCE_DIR}/tpm/mesh_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tpm/packing.cpp
    # trn — terrain config
    ${CMAKE_CURRENT_SOURCE_DIR}/trn/trn_io.cpp
    # wac — the WAC scripting language front end: lexer, parser/AST, the command
    # table (names + param types), and the compiled Program model. The compiler
    # (binds names/vars against the live world) and the VM are engine/runtime/wac.
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/command_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/command_table.gen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/lexer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/parser.cpp
)

# Every format's public include/ keeps its historical <domain>/... include shape.
target_include_directories(opennova_formats
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        # wac's front-end headers; the compiler/VM headers stay in
        # engine/runtime/wac's include/wac/ (disjoint header sets, one prefix).
    PRIVATE
)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_formats PUBLIC opennova_io)

# Link math library (needed for sin/cos/floorf in pure C builds; not needed on
# Windows/MSVC) — carried over from the threedi lib.
if(NOT WIN32 OR MINGW)
    target_link_libraries(opennova_formats PUBLIC m)
endif()

# C++17 is the group standard (the per-format C++11 members are C++17-clean).
target_compile_features(opennova_formats PUBLIC cxx_std_17)

# miniz.c stays as C; fnt.c is C99.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/bfc1/miniz.c
    PROPERTIES LANGUAGE C)
set_target_properties(opennova_formats PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
