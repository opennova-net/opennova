# opennova_formats — every NovaLogic format parser/writer (one directory per
# format, ADR 0024), built as one STATIC group target (Shape A).
#
# GUARDRAIL (non-negotiable): the opennova_mission_format sources — the
# .mis/.bms parse+write half of engine/runtime/mission — belong to THIS group.
# Without that assignment there is no acyclic 4-group partition:
# base/refs -> mission_format while runtime/simassets -> base/resource_index
# would cycle base and runtime. The files stay under engine/runtime/mission/
# (sources do not move); only their target home is here.

add_library(opennova_formats STATIC
    # adm — .adm animation definition map (anim slots -> .bad clip variant rings)
    ${CMAKE_CURRENT_SOURCE_DIR}/adm/src/adm.cpp
    # ase — 3ds Max ASCII scene export (the OED model source format)
    ${CMAKE_CURRENT_SOURCE_DIR}/ase/src/ase_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ase/src/ase_writer.cpp
    # avatars — Avatars.def
    ${CMAKE_CURRENT_SOURCE_DIR}/avatars/src/avatars.cpp
    # bad — skeletal animation
    ${CMAKE_CURRENT_SOURCE_DIR}/bad/src/bad.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bad/src/bad_write.cpp
    # bfc1 — compressed container (vendored miniz stays in-tree here)
    ${CMAKE_CURRENT_SOURCE_DIR}/bfc1/src/bfc1.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bfc1/src/miniz.c
    # cbin — credits binary
    ${CMAKE_CURRENT_SOURCE_DIR}/cbin/src/cbin.cpp
    # cpt — terrain height/CDEP container
    ${CMAKE_CURRENT_SOURCE_DIR}/cpt/src/cpt_crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cpt/src/cpt_io.cpp
    # dbf — dialog bank
    ${CMAKE_CURRENT_SOURCE_DIR}/dbf/src/dbf.cpp
    # def — the .def text tables
    ${CMAKE_CURRENT_SOURCE_DIR}/def/src/def.cpp          # the .def umbrella + loadout weight/encumbrance
    ${CMAKE_CURRENT_SOURCE_DIR}/def/src/def_ammo.cpp     # AMMO.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/src/def_hudpos.cpp   # HUDPOS.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/src/def_items.cpp    # ITEMS.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/src/def_scan.cpp     # the shared .def text scanner
    ${CMAKE_CURRENT_SOURCE_DIR}/def/src/def_weapons.cpp  # WEAPONS.DEF
    # env — environment/time-of-day
    ${CMAKE_CURRENT_SOURCE_DIR}/env/src/env.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/env/src/env_render.cpp
    # fnt — bitmap fonts
    ${CMAKE_CURRENT_SOURCE_DIR}/fnt/src/fnt.c
    # foliage — foliage defs + placement map
    ${CMAKE_CURRENT_SOURCE_DIR}/foliage/src/foliage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/foliage/src/runtime.cpp
    # lwf — sound profiles
    ${CMAKE_CURRENT_SOURCE_DIR}/lwf/src/lwf.cpp
    # mns — menu stylesheet
    ${CMAKE_CURRENT_SOURCE_DIR}/mns/src/mns.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mns/src/mns_document.cpp
    # mnu — menu UI
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/src/mnu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/src/mnu_layout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/src/mnu_xml.cpp
    # mus — music script bytecode
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/src/mus_parse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/src/mus_decompile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/src/mus_compile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/src/mus_vm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/src/mus_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/src/mus_ast.cpp
    # oed — the OED project/convert/export pipeline
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/src/oed.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/src/convert_internal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/src/export_3di.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/src/rdta.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/oed/src/material_utils.cpp
    # pcx — image
    ${CMAKE_CURRENT_SOURCE_DIR}/pcx/src/pcx_io.cpp
    # pff — the archive format
    ${CMAKE_CURRENT_SOURCE_DIR}/pff/src/pff.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pff/src/pff_writer.cpp
    # playersav — weapon.sav player profile
    ${CMAKE_CURRENT_SOURCE_DIR}/playersav/src/weapon_sav.cpp
    # rtxt — localized string tables
    ${CMAKE_CURRENT_SOURCE_DIR}/rtxt/src/rtxt.cpp
    # sbf — sound banks
    ${CMAKE_CURRENT_SOURCE_DIR}/sbf/src/sbf.cpp
    # scr — obfuscated text container
    ${CMAKE_CURRENT_SOURCE_DIR}/scr/src/scr.cpp
    # tdp — OED project structs
    ${CMAKE_CURRENT_SOURCE_DIR}/tdp/src/tdp.cpp
    # threedi — the .3di model format (3DI3 first-class, ADR 0027)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_3di3_read.cpp   # 3DI3 -> Threedi3di3
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_3di3_write.cpp  # Threedi3di3 -> 3DI3 (parity writer, ADR 0003)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_3di3_ground_anchor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_compare.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_ctrl_catalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_panm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_panm_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_panm_wave.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_panm_matrices.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/src/threedi_panm_pose.cpp
    # til — terrain tiles
    ${CMAKE_CURRENT_SOURCE_DIR}/til/src/til_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/til/src/til_foliage_blocker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/til/src/til_overlay_bake.cpp
    # tpj — terrain project
    ${CMAKE_CURRENT_SOURCE_DIR}/tpj/src/tpj_io.cpp
    # trn — terrain config
    ${CMAKE_CURRENT_SOURCE_DIR}/trn/src/trn_io.cpp
    # mission_format (engine/runtime/mission, format half — see GUARDRAIL above):
    # .mis/.bms parse+write, schema reflection, authoring edits. Standalone —
    # no world/runtime dependency; the flat C ABI (mission.h) and parse-only
    # consumers (refs, npruntime's bms use) build against this.
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/src/authoring.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/src/bms.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/src/mission.cpp          # MissionDocument — the document and its edit operations
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/src/mission_capi.cpp     # the flat C ABI over it
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/src/mission_mis_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/src/mission_mis_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/src/mission_names.cpp    # BMS event-logic enum display names
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/src/mission_records.cpp  # bms:: structs <-> typed editor records
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/src/mission_schema.cpp
)

# Every format's public include/ keeps its historical <domain>/... include shape.
target_include_directories(opennova_formats
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/adm/include
        ${CMAKE_CURRENT_SOURCE_DIR}/ase/include
        ${CMAKE_CURRENT_SOURCE_DIR}/avatars/include
        ${CMAKE_CURRENT_SOURCE_DIR}/bad/include
        ${CMAKE_CURRENT_SOURCE_DIR}/bfc1/include
        ${CMAKE_CURRENT_SOURCE_DIR}/cbin/include
        ${CMAKE_CURRENT_SOURCE_DIR}/cpt/include
        ${CMAKE_CURRENT_SOURCE_DIR}/dbf/include
        ${CMAKE_CURRENT_SOURCE_DIR}/def/include
        ${CMAKE_CURRENT_SOURCE_DIR}/env/include
        ${CMAKE_CURRENT_SOURCE_DIR}/fnt/include
        ${CMAKE_CURRENT_SOURCE_DIR}/foliage/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lwf/include
        ${CMAKE_CURRENT_SOURCE_DIR}/mns/include
        ${CMAKE_CURRENT_SOURCE_DIR}/mnu/include
        ${CMAKE_CURRENT_SOURCE_DIR}/mus/include
        ${CMAKE_CURRENT_SOURCE_DIR}/oed/include
        ${CMAKE_CURRENT_SOURCE_DIR}/pcx/include
        ${CMAKE_CURRENT_SOURCE_DIR}/pff/include
        ${CMAKE_CURRENT_SOURCE_DIR}/playersav/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rtxt/include
        ${CMAKE_CURRENT_SOURCE_DIR}/sbf/include
        ${CMAKE_CURRENT_SOURCE_DIR}/scr/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tdp/include
        ${CMAKE_CURRENT_SOURCE_DIR}/threedi/include
        ${CMAKE_CURRENT_SOURCE_DIR}/til/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tpj/include
        ${CMAKE_CURRENT_SOURCE_DIR}/trn/include
        # mission_format's public headers (and the runtime mission half's — one
        # include/ per directory; the runtime group reuses this propagated dir).
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/mission/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/bfc1/src   # miniz.h (vendored, private)
        ${CMAKE_CURRENT_SOURCE_DIR}/oed/src    # threedi_model.h (private)
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
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/bfc1/src/miniz.c
    PROPERTIES LANGUAGE C)
set_target_properties(opennova_formats PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
