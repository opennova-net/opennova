add_library(opennova_mus STATIC
    src/mus_parse.cpp
    src/mus_decompile.cpp
    src/mus_compile.cpp
    src/mus_vm.cpp
    src/mus_model.cpp
    src/mus_ast.cpp
)

target_include_directories(opennova_mus
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(opennova_mus PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
