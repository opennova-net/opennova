# opennova-project — the OpenNova Editor's project core on the command line
# (ADR 0046 d4): create, inspect and validate a project without the editor.
#
# The commands live in a small STATIC library so the executable and the
# project_cli ctest drive the very same code.
add_library(opennova_project_cli STATIC commands.cpp)
target_include_directories(opennova_project_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(opennova_project_cli PUBLIC opennova_editor)
target_compile_features(opennova_project_cli PUBLIC cxx_std_17)

add_executable(opennova_project main.cpp)
set_target_properties(opennova_project PROPERTIES OUTPUT_NAME opennova-project)
target_link_libraries(opennova_project PRIVATE opennova_project_cli)
target_compile_features(opennova_project PRIVATE cxx_std_17)
