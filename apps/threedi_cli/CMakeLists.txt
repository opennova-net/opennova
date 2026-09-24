# opennova-3di — build a .3di from an authored .o3d scene, or inspect one.
#
# The native half of the Blender exporter (tools/blender/opennova_3di): the
# add-on writes the scene text, this tool mints the model through the
# engine's own construction API and parity writer (formats/threedi), so there
# is one 3DI3 encoder. Ships inside the add-on's bin/ directory.

add_executable(opennova_3di main.cpp build.cpp scene.cpp info.cpp compare.cpp)
set_target_properties(opennova_3di PROPERTIES OUTPUT_NAME opennova-3di)

target_link_libraries(opennova_3di PRIVATE opennova_base)

target_compile_features(opennova_3di PRIVATE cxx_std_17)
