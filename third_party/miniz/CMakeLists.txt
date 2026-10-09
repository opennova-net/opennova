# miniz 2.1.0 (public domain / unlicense, see the end of miniz.c), Rich
# Geldreich's single-file zlib subset. Vendored as is: its consumers are the BFC1
# container decoder (engine/formats/bfc1, the inflate) and the PNG reader and
# writer (engine/formats/png, the inflate, the deflate and the CRC), all inside
# opennova_formats, which PRIVATE-links this target; nothing else in the tree
# includes miniz.h.
# The engine itself carries no C sources (ADR 0043 d14), so the C toolchain
# exists for this leaf, bcrypt and sqlite alone. Bump by replacing miniz.c and
# miniz.h together.

add_library(opennova_miniz STATIC
    miniz.c
)

target_include_directories(opennova_miniz
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(opennova_miniz PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# Suppress the noise that comes with vendored single-file sources.
if(MSVC)
    target_compile_options(opennova_miniz PRIVATE /W0)
else()
    target_compile_options(opennova_miniz PRIVATE -w)
endif()
