add_library(opennova_bms STATIC
    src/parser.cpp
)

target_include_directories(opennova_bms
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_bms PUBLIC cxx_std_17)
