add_library(opennova_pcx STATIC
    src/pcx_io.cpp
)

target_include_directories(opennova_pcx
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_pcx PUBLIC cxx_std_17)

