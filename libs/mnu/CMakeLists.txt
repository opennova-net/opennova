add_library(opennova_mnu STATIC
    src/mnu.cpp
    src/mnu_xml.cpp
)

target_include_directories(opennova_mnu
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_mnu PUBLIC cxx_std_17)
target_link_libraries(opennova_mnu PRIVATE opennova_io)
