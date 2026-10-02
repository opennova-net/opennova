# engine/ — the portable, Godot-free C++ core (ADR 0028), built as the Shape A
# group targets: one STATIC library per group directory plus the header-only
# INTERFACE leaf. Directories and namespaces are unchanged — only the CMake
# target granularity collapsed (2026-08 Shape A decision).
#
#   opennova_io                 INTERFACE  base/io (byte/bit IO, fixed-point, strutil)
#   opennova_crt                STATIC     base/crt (retail-parity MSVC CRT rand stream)
#   opennova_formats            STATIC     formats/*, including formats/mission (the
#                                          mission document model; its runtime half
#                                          is runtime/mission)
#   opennova_base               STATIC     base/{vfs,resource_index,gameprofile,pcapio,
#                                          os_random}
#   opennova_net                STATIC     net/* — the wire: novacrypto, napi, npwire,
#                                          the novaworld session/gate legs
#   opennova_runtime            STATIC     runtime/*, including the in-match session
#                                          and replication (ADR 0043 d4)
#   opennova_novaworld_service  STATIC     the novaworld service leg — the ONLY
#                                          target that links opennova_sqlite
#   opennova_editor             STATIC     editor/* — the OpenNova Editor's portable
#                                          core (ADR 0046 d3): project, assets,
#                                          requirements, and the later slices
#
# PUBLIC link chain: formats -> io; base -> formats; net -> base;
# runtime -> net; novaworld_service -> net (+ sqlite PRIVATE); editor -> runtime.
# base sits ABOVE formats deliberately: vfs consumes pff/scr/bfc1; only io is at
# the bottom. net is wire only and never links runtime (ADR 0043 d4). Nothing
# below editor links or includes it: the game and the Play child never carry
# editor code (ADR 0046 d2/d3).
#
# Both CMake roots (the repo root and godot/src) add this directory; the
# including root must have defined opennova_sqlite (third_party/sqlite) first —
# the service target links it.

# The Visual Studio generator compiles one project's sources one at a time
# unless cl gets /MP; opennova_runtime alone is ~400 sources on the critical
# path of both roots. Ninja schedules sources itself (and CI's sccache wants
# one source per cl), so the flag is for the VS generator only.
if(MSVC AND CMAKE_GENERATOR MATCHES "^Visual Studio")
    add_compile_options(/MP)
endif()

add_subdirectory(base/io)
add_subdirectory(base/crt)
add_subdirectory(formats)
add_subdirectory(base)
add_subdirectory(net)
add_subdirectory(runtime)
add_subdirectory(editor)
