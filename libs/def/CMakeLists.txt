add_library(oed_def STATIC
    src/def.cpp
)

target_include_directories(oed_def
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(oed_def PUBLIC cxx_std_11)
