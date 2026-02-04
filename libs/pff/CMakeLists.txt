add_library(opennova_pff STATIC
    src/pff.c
)

target_include_directories(opennova_pff
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(opennova_pff PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
