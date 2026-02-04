add_library(oed_adm STATIC
    src/adm.c
)

target_include_directories(oed_adm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(oed_adm PUBLIC c_std_99)
