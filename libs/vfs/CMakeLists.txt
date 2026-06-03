add_library(opennova_vfs STATIC
    src/vfs.cpp
    src/vfs_decode.cpp
    src/vfs_capi.cpp
)

target_include_directories(opennova_vfs
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# pff/scr/bfc1/mus are implementation details (not exposed in the public headers).
# mus: vfs_decode resolves MUS scripts (incl. the headerless encrypted form) so
# extraction/read_file yield plaintext SCR0.
target_link_libraries(opennova_vfs
    PRIVATE opennova_pff opennova_scr opennova_bfc1 opennova_mus
)

set_target_properties(opennova_vfs PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
