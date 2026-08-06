add_library(opennova_wac STATIC
    src/command_table.gen.cpp
    src/command_table.cpp
    src/lexer.cpp
    src/parser.cpp
    src/compiler.cpp
    src/vm.cpp
)

target_include_directories(opennova_wac
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_wac PUBLIC opennova_io opennova_world)

target_compile_features(opennova_wac PUBLIC cxx_std_17)

set_target_properties(opennova_wac PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
