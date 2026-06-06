# Shared undo/redo core for the OpenNova editor (ONED) workspaces.
# Header-only: the whole mechanic is a template, so this is an INTERFACE target
# that only carries the include path + the C++17 requirement. Godot-agnostic and
# dependency-free, so the GDExtension engine layer and the libs/ FFI both consume
# it without pulling anything new in.
add_library(opennova_oned_edit INTERFACE)

target_include_directories(opennova_oned_edit
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_oned_edit INTERFACE cxx_std_17)
