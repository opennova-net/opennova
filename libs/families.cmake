# Family link-interface groups (LIBS-2, ADR 0024).
#
# One-lib-per-format stands: each member keeps its own target, headers, and
# tests. These INTERFACE targets exist so whole-family consumers — the
# GDExtension is the canonical one — can name the family instead of every
# member. They are link conveniences, never physical merges, and never a way
# around scripts/lint/link_graph_check.py's forbidden edges (the check walks
# the transitive closure). Leaf consumers (ctests, apps) keep linking exactly
# the libs they use; nothing under libs/ links a family target.
#
# Include from a CMake root AFTER every member target exists (both roots do:
# the repo root and godot/engine).

add_library(opennova_terrain_family INTERFACE)
target_link_libraries(opennova_terrain_family INTERFACE
    opennova_terrain
    opennova_terrain_query
    opennova_cpt
    opennova_til
    opennova_trn
    opennova_tpj
    opennova_foliage
)

add_library(opennova_audio_family INTERFACE)
target_link_libraries(opennova_audio_family INTERFACE
    opennova_audio
    opennova_sbf
    opennova_mus
    opennova_lwf
    opennova_dbf
)
