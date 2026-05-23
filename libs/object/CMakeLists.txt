add_library(opennova_object STATIC
    src/flat_mesh.cpp
    src/ir_to_flat_mesh.cpp
    src/coords.cpp
    src/smoothing_groups.cpp
    src/three_di_policy.cpp
    src/nlascexp_quirks.cpp
    src/ase_vec.cpp
    src/flat_mesh_to_ase.cpp
    src/material_list_to_ase.cpp
    src/markers_to_ase.cpp
    src/bones_to_ase.cpp
    src/lights_to_ase.cpp
    src/polyhedron.cpp
    src/collision_to_ase.cpp
    src/occlusion_to_ase.cpp
    src/write_ase_files.cpp
    src/write_ase_files_from_flat_meshes.cpp
)

target_include_directories(opennova_object
    PUBLIC  include
    PRIVATE src   # for threedi_model.h (private)
)

# Dependencies: ASE parser, TDP project structs.
# threedi write/free symbols resolve at final link time (via --whole-archive
# in the shared lib, or direct linking in executables). Linking opennova_threedi
# here would cause CMake to de-duplicate it out of the --whole-archive block.
target_link_libraries(opennova_object
    PUBLIC opennova_ase opennova_tdp opennova_bad
)

# Need threedi include path for threedi_panm.h (header-only) and threedi.h.
# Sibling-relative path so this works from both root cmake builds and Godot
# extension builds (which invoke cmake from godot/engine).
target_include_directories(opennova_object
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../threedi/include
)

target_compile_features(opennova_object PRIVATE cxx_std_17)
