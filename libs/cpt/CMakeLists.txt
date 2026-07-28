add_library(opennova_cpt STATIC
    src/cpt_crypto.cpp
    src/cpt_io.cpp
)

target_include_directories(opennova_cpt
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_cpt PUBLIC cxx_std_17)
target_link_libraries(opennova_cpt PRIVATE opennova_io)

