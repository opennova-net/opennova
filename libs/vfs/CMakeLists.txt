add_library(opennova_vfs STATIC
    src/vfs.cpp
    src/vfs_decode.cpp
    src/vfs_capi.cpp
)

target_include_directories(opennova_vfs
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_vfs PUBLIC cxx_std_17)

# pff/scr/bfc1 are implementation details (not exposed in the public headers).
target_link_libraries(opennova_vfs
    PRIVATE opennova_pff opennova_scr opennova_bfc1
)

set_target_properties(opennova_vfs PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_vfs PUBLIC opennova_io)
