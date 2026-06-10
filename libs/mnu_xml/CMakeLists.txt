add_library(opennova_mnu_xml STATIC
    src/mnu_xml.cpp
)

target_include_directories(opennova_mnu_xml
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_mnu_xml PUBLIC cxx_std_17)
