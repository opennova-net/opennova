add_library(opennova_cbin STATIC
    src/cbin.cpp
)

target_include_directories(opennova_cbin
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_cbin PUBLIC cxx_std_17)
