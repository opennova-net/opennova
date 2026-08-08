add_library(opennova_trn STATIC
    src/trn_io.cpp
)

target_include_directories(opennova_trn
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_trn
    PUBLIC
        opennova_foliage
)

target_compile_features(opennova_trn PUBLIC cxx_std_17)
