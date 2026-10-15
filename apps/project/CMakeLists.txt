# opennova-project: the OpenNova Editor's project session on the command line (ADR 0046 d4,
# S13 A7): create, inspect, validate, import into and build a project without the editor, each
# verb a headless ProjectSession driven by its requests and read by its query.
#
# The verbs live in a small STATIC library so the executable and the project_cli ctests drive
# the very same code.
add_library(opennova_project_cli STATIC cli_verbs.cpp)
target_include_directories(opennova_project_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(opennova_project_cli PUBLIC opennova_editor)
target_compile_features(opennova_project_cli PUBLIC cxx_std_17)

add_executable(opennova_project main.cpp)
set_target_properties(opennova_project PROPERTIES OUTPUT_NAME opennova-project)
target_link_libraries(opennova_project PRIVATE opennova_project_cli)
if(WIN32)
  # main.cpp reads the arguments wide (CommandLineToArgvW) and hands the verbs UTF-8.
  target_link_libraries(opennova_project PRIVATE shell32)
endif()
target_compile_features(opennova_project PRIVATE cxx_std_17)
