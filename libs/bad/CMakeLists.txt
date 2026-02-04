add_library(oed_bad STATIC
    src/bad.c
)

target_include_directories(oed_bad
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(oed_bad PUBLIC c_std_99)
