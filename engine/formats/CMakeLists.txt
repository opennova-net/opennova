# opennova_formats — every NovaLogic format parser/writer (one directory per
# format, ADR 0024), built as one STATIC group target (Shape A).
#
# The mission FORMAT half — .mis/.bms parse+write, document edits, and the C
# ABI — belongs to this group. Event evaluation, promotion, and boot stay in
# engine/runtime/mission.

add_library(opennova_formats STATIC
    # adm — .adm animation definition map (anim slots -> .bad clip variant rings)
    ${CMAKE_CURRENT_SOURCE_DIR}/adm/adm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/adm/adm_write.cpp
    # aip — AI profile text (PARTIAL PORT: the two witnessed speed keys; the
    # remainder is the ledgered D-AI-11(h) gap and lands here when witnessed)
    ${CMAKE_CURRENT_SOURCE_DIR}/aip/aip.cpp
    # avatars — Avatars.def
    ${CMAKE_CURRENT_SOURCE_DIR}/avatars/avatars.cpp
    # bad — skeletal animation (the clip construction seam and the writer,
    # ADR 0047: the add-on authors clips through them)
    ${CMAKE_CURRENT_SOURCE_DIR}/bad/bad.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bad/bad_build.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bad/bad_o3a_read.cpp    # the .o3a clip-set text -> the seam
    ${CMAKE_CURRENT_SOURCE_DIR}/bad/bad_write.cpp
    # bink — native BIKi menu-video decode
    ${CMAKE_CURRENT_SOURCE_DIR}/bink/bink.cpp
    # bfc1 — compressed container (inflates through the vendored miniz,
    # third_party/miniz, linked below)
    ${CMAKE_CURRENT_SOURCE_DIR}/bfc1/bfc1.cpp
    # cbin — credits binary
    ${CMAKE_CURRENT_SOURCE_DIR}/cbin/cbin.cpp
    # cpt — terrain height/CDEP container
    ${CMAKE_CURRENT_SOURCE_DIR}/cpt/cpt_crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cpt/cpt_io.cpp
    # dbf — dialog bank
    ${CMAKE_CURRENT_SOURCE_DIR}/dbf/dbf.cpp
    # dds — the DirectDraw surface the game hands to D3DX (writer only: A8R8G8B8)
    ${CMAKE_CURRENT_SOURCE_DIR}/dds/dds.cpp
    # def — the .def text tables
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def.cpp          # the .def umbrella + loadout weight/encumbrance
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_ammo.cpp     # AMMO.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_authored.cpp  # a member in the units its line writes
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_hudpos.cpp   # HUDPOS.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_items.cpp    # ITEMS.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_scan.cpp     # the shared .def text scanner
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_weapons.cpp  # WEAPONS.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_schema.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_schema_records.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_schema_properties.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_write.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_write_record.cpp

    # env — environment/time-of-day
    ${CMAKE_CURRENT_SOURCE_DIR}/env/env.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/env/env_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/env/env_weather_core.cpp
    # fnt — bitmap fonts
    ${CMAKE_CURRENT_SOURCE_DIR}/fnt/fnt.cpp
    # foliage — foliage defs + placement map
    ${CMAKE_CURRENT_SOURCE_DIR}/foliage/foliage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/foliage/runtime.cpp
    # grm — facial texture mesh and gesture offsets
    ${CMAKE_CURRENT_SOURCE_DIR}/grm/grm.cpp
    # lwf — sound profiles (+ the .wav payload decode the profiles reference)
    ${CMAKE_CURRENT_SOURCE_DIR}/lwf/lwf.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/lwf/wav_pcm.cpp
    # mission — .bms/.mis missions: document model, parsers/writers and edit
    # helpers. Standalone (no world/runtime dependency);
    # the runtime half — event runtime, promotion, boot — lives in
    # engine/runtime/mission; the group in the include path keeps the two apart
    # (<formats/mission/...> vs <runtime/mission/...>, ADR 0040; the ADR 0029
    # acyclicity fold; directory home since ADR 0030).
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/authoring.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/bms.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/bms_edit.cpp         # the document's edit operations + typed views over bms::File
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_mis_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_mis_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_names.cpp    # BMS event-logic enum display names
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_records.cpp  # bms:: structs <-> typed mission records
    # mns — menu stylesheet
    ${CMAKE_CURRENT_SOURCE_DIR}/mns/mns.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mns/mns_document.cpp
    # mnu — menu UI
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/mnu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/mnu_layout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/mnu_schema.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mnu/mnu_xml.cpp
    # mus — music script bytecode
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_parse.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_decompile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_compile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mus/mus_vm.cpp
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
    # score — score.ini scoring config (ordered GAMETYPE rows: FIELD/VAR values)
    ${CMAKE_CURRENT_SOURCE_DIR}/score/score.cpp
    # scr — obfuscated text container
    ${CMAKE_CURRENT_SOURCE_DIR}/scr/scr.cpp
    # sph — the /PROFILE server-log FOURCC chunk container (payload records:
    # engine/net/npwire serverlog_decode)
    ${CMAKE_CURRENT_SOURCE_DIR}/sph/sph.cpp
    # tga — the true-colour image the game's texture loaders read (writer only: 32-bit)
    ${CMAKE_CURRENT_SOURCE_DIR}/tga/tga.cpp
    # threedi — the .3di model format (3DI3 first-class, ADR 0027)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_3di3_read.cpp   # 3DI3 -> Threedi3di3
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_3di3_write.cpp  # Threedi3di3 -> 3DI3 (parity writer, ADR 0003)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_build.cpp       # the construction API over the writer (recipe -> Threedi3di3)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_o3d_read.cpp    # the .o3d scene text -> the construction API
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_schema.cpp      # the property table an editor sets fields through
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/scene_text.cpp          # the tokenizer the .o3d and .o3a texts share
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_3di3_ground_anchor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_ctrl_catalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm_runtime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm_wave.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm_matrices.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_panm_pose.cpp
    # til — terrain tiles
    ${CMAKE_CURRENT_SOURCE_DIR}/til/til_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/til/til_foliage_blocker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/til/til_tsd.cpp
    # trn — terrain config
    ${CMAKE_CURRENT_SOURCE_DIR}/trn/trn_io.cpp
    # wac — the WAC scripting language's command table (names + param types),
    # bytecode encoding and compiled Program model. The single-pass compiler
    # (binds names/vars against the live world) and the VM are engine/runtime/wac.
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/command_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/command_table_data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/wac/help.cpp
)

# engine/ is the one public include root (ADR 0040): <formats/<domain>/file.h>.
# wac's format half lives here as <formats/wac/...>; the compiler/VM are
# <runtime/wac/...> — the group in the path keeps the two apart.
target_include_directories(opennova_formats
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(opennova_formats PUBLIC opennova_io opennova_crt)
# The BFC1 decoder inflates through miniz (the editor's PNG reader is the other
# consumer); the vendored leaf stays private
# to this group (a STATIC library's PRIVATE deps still ride every downstream
# link line, so nothing else needs to name it).
target_link_libraries(opennova_formats PRIVATE opennova_miniz)

# Link the math library (sin/cos/floorf on the non-MSVC toolchains; not needed
# on Windows/MSVC).
if(NOT WIN32 OR MINGW)
    target_link_libraries(opennova_formats PUBLIC m)
endif()

# C++17 is the group standard (the per-format C++11 members are C++17-clean).
target_compile_features(opennova_formats PUBLIC cxx_std_17)

set_target_properties(opennova_formats PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
