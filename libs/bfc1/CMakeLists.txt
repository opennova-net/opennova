add_library(opennova_bfc1 STATIC
    src/bfc1.c
    src/miniz.c
)

target_include_directories(opennova_bfc1
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(opennova_bfc1 PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
