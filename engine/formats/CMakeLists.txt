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
    # admincfg — admin.cfg, the remote-admin console's users and whitelist (read-only:
    # retail never writes it)
    ${CMAKE_CURRENT_SOURCE_DIR}/admincfg/admin_cfg.cpp
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
    # banlist — the two ban lists: banlist.txt (PCIDs, a NovaWorld host's) and
    # banned.txt (addresses and names, every authority)
    ${CMAKE_CURRENT_SOURCE_DIR}/banlist/address_ban_list.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/banlist/pcid_ban_list.cpp
    # bink — native BIKi menu-video decode
    ${CMAKE_CURRENT_SOURCE_DIR}/bink/bink.cpp
    # bfc1 — compressed container (inflates through the vendored miniz,
    # third_party/miniz, linked below)
    ${CMAKE_CURRENT_SOURCE_DIR}/bfc1/bfc1.cpp
    # cbin — credits binary
    ${CMAKE_CURRENT_SOURCE_DIR}/cbin/binary_config.cpp # the CBIN form read and written exactly
    ${CMAKE_CURRENT_SOURCE_DIR}/cbin/cbin.cpp
    # charattr — charattr.def, the character classes' attributes (read and written)
    ${CMAKE_CURRENT_SOURCE_DIR}/charattr/charattr.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/charattr/charattr_write.cpp
    # configfile — the ConfigFile's text form and its accessors (charattr.def, a marquee's credits)
    ${CMAKE_CURRENT_SOURCE_DIR}/configfile/config_file.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_compose.cpp  # a file generated over its modeled layout
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_hudpos.cpp   # HUDPOS.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_hudpos_write.cpp # HUDPOS.DEF's writer
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_items.cpp    # ITEMS.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_layout.cpp   # the order and indentation a file was read with
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_notes.cpp    # what a save needs to put a file back as it was read
    ${CMAKE_CURRENT_SOURCE_DIR}/def/def_powerup.cpp  # POWERUP.DEF
    ${CMAKE_CURRENT_SOURCE_DIR}/def/reserved_items.cpp # the ids and rows the engine fixes
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
    # gamecfg — game.cfg, the persisted settings (the key model, reader, writer)
    ${CMAKE_CURRENT_SOURCE_DIR}/gamecfg/game_cfg.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gamecfg/game_cfg_write.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_chains.cpp   # an event's triggers and actions as the records it owns
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_field.cpp    # every mission record's fields as rows (the setters by name, the editor's table)
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_mis_parser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_mis_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_names.cpp    # BMS event-logic enum display names
    ${CMAKE_CURRENT_SOURCE_DIR}/mission/mission_params.cpp   # what each trigger's and action's parameters are, as rows
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
    ${CMAKE_CURRENT_SOURCE_DIR}/pff/pff_stream_writer.cpp
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
    # tga — the game's TGA reader (the decode every texture loader runs on a .tga)
    # and the true-colour image writer (32-bit) the editor's textures go through
    ${CMAKE_CURRENT_SOURCE_DIR}/tga/tga_read.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tga/tga.cpp
    # threedi — the .3di model format (3DI3 first-class, ADR 0027)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_3di3_read.cpp   # 3DI3 -> Threedi3di3
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_3di3_write.cpp  # Threedi3di3 -> 3DI3 (parity writer, ADR 0003)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_build.cpp       # the construction API over the writer (recipe -> Threedi3di3)
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_o3d_read.cpp    # the .o3d scene text -> its parse
    ${CMAKE_CURRENT_SOURCE_DIR}/threedi/threedi_o3d_lower.cpp   # the parse -> the construction API, for a target
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
