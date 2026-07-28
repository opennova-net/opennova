add_library(opennova_tpj STATIC
    src/tpj_io.cpp
)

target_include_directories(opennova_tpj
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_tpj
    PUBLIC opennova_io
        opennova_trn
)

target_compile_features(opennova_tpj PUBLIC cxx_std_17)

