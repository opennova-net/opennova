# opennova-3di — build a .3di from an authored .o3d scene, or inspect one.
#
# The native half of the Blender exporter (tools/blender/opennova_3di): the
# add-on writes the scene text, this tool mints the model through the
# engine's own construction API and parity writer (formats/threedi), so there
# is one 3DI3 encoder. Ships inside the add-on's bin/ directory.

# The command handlers, one translation unit per command: the exe dispatches
# them and the command tests (tests/threedi, tests/anim) call them directly.
# The CLI links opennova_base alone, so the runtime headers it reads its tables
# from (runtime/renderer/material_descriptor.h, runtime/world/body_anim.h,
# runtime/anim/anim_event_bits.h) must stay header-only.
add_library(opennova_3di_commands STATIC scene_text.cpp build.cpp scene.cpp info.cpp compare.cpp
    anim_load.cpp anim_build.cpp anim_scene.cpp anim_info.cpp anim_compare.cpp)
target_link_libraries(opennova_3di_commands PUBLIC opennova_base)
target_include_directories(opennova_3di_commands PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(opennova_3di_commands PUBLIC cxx_std_17)

add_executable(opennova_3di main.cpp)
set_target_properties(opennova_3di PROPERTIES OUTPUT_NAME opennova-3di)
target_link_libraries(opennova_3di PRIVATE opennova_3di_commands)

# Windows hands a program its arguments and file names in the ANSI code page
# unless its manifest opts into UTF-8: without it a model under a folder the
# code page cannot spell (Japanese, on a Western system) fails to open. MSVC
# merges a .manifest source into the manifest it embeds (both generators).
if(MSVC)
    target_sources(opennova_3di PRIVATE utf8.manifest)
endif()
