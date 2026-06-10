add_library(opennova_mnu STATIC
    src/mnu.cpp
)

target_include_directories(opennova_mnu
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_mnu
    PUBLIC
        opennova_mnu_xml
)

target_compile_features(opennova_mnu PUBLIC cxx_std_17)
