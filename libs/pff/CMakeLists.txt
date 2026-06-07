add_library(opennova_pff STATIC
    src/pff.cpp
    src/pff_writer.cpp
)

target_include_directories(opennova_pff
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(opennova_pff PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
)
