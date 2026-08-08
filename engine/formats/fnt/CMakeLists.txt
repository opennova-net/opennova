add_library(opennova_fnt STATIC
    src/fnt.c
)

target_include_directories(opennova_fnt
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(opennova_fnt PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
