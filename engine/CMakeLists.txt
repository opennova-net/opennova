# engine/ — the portable, Godot-free C++ core (ADR 0028), built as the Shape A
# group targets: one STATIC library per group directory plus the header-only
# INTERFACE leaf. Directories, namespaces, and the flat C ABI are unchanged —
# only the CMake target granularity collapsed (2026-08 Shape A decision).
#
#   opennova_io                 INTERFACE  base/io (byte/bit IO, fixed-point, strutil)
#   opennova_formats            STATIC     formats/* + the mission_format half of
#                                          runtime/mission (see engine/formats/CMakeLists.txt)
#   opennova_base               STATIC     base/{vfs,resource_index,gameprofile,pcapio}
#   opennova_runtime            STATIC     runtime/* (minus the mission_format sources)
#   opennova_net                STATIC     net/* + the novaworld session/gate legs
#   opennova_novaworld_service  STATIC     the novaworld service leg — the ONLY
#                                          target that links opennova_sqlite
#
# PUBLIC link chain: formats -> io; base -> formats; runtime -> base;
# net -> runtime; novaworld_service -> net (+ sqlite PRIVATE). base sits ABOVE
# formats deliberately: vfs consumes pff/scr/bfc1; only io is at the bottom.
#
# Both CMake roots (the repo root and godot/src) add this directory; the
# including root must have defined opennova_sqlite (third_party/sqlite) first —
# the service target links it.

add_subdirectory(base/io)
add_subdirectory(formats)
add_subdirectory(base)
add_subdirectory(runtime)
add_subdirectory(net)
