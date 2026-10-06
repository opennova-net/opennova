# opennova-3di — build a .3di from an authored .o3d scene, write one back out
# as a scene, inspect or compare models; the same for a rig's .bad/.adm clip
# set through the .o3a text (`anim build|scene|info|compare`); `texture`, an
# image written as a model's .dds, .tga or .mdt; and `catalog`, the engine
# tables a front end offers.
#
# The native half of the Blender add-on (tools/blender/opennova_3di): the
# add-on writes and reads the scene text, this tool reads and writes every
# model and clip through the engine's own construction API and parity writers
# (formats/threedi, formats/bad, formats/adm), so there is one encoder and one
# decoder. Ships inside the add-on's bin/ directory.

# The command handlers, one translation unit per command: the exe dispatches
# them and the command tests (tests/threedi, tests/anim) call them directly.
# Weapon timing also links the runtime: its preview runs the real weapon FSM,
# and its merge reads the result back through the def parser. `texture` writes
# through the runtime's image import (renderer/texture_authoring: the source
# read, the halving, the DXT encoder), the pieces the editor's import runs too.
add_library(opennova_3di_commands STATIC build.cpp scene.cpp info.cpp compare.cpp
    anim_load.cpp anim_build.cpp anim_scene.cpp anim_info.cpp anim_compare.cpp weapon_timing.cpp
    weapon_merge.cpp texture.cpp)
target_link_libraries(opennova_3di_commands PUBLIC opennova_runtime)
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
