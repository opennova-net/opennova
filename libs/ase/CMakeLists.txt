add_library(opennova_ase STATIC
    src/ase_parser.cpp
    src/ase_writer.cpp
)

target_include_directories(opennova_ase
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_ase PUBLIC cxx_std_17)
