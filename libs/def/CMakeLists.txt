add_library(opennova_def STATIC
    src/def.cpp
)

target_include_directories(opennova_def
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_def PUBLIC cxx_std_11)
