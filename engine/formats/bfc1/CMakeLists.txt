add_library(opennova_bfc1 STATIC
    src/bfc1.cpp
    src/miniz.c
)

target_include_directories(opennova_bfc1
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(opennova_bfc1 PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
)

# miniz.c stays as C — set C99 for that file only
set_source_files_properties(src/miniz.c PROPERTIES LANGUAGE C)
set_target_properties(opennova_bfc1 PROPERTIES C_STANDARD 99)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_bfc1 PUBLIC opennova_io)
