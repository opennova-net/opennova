add_library(opennova_scr STATIC
    src/scr.cpp
)

target_include_directories(opennova_scr
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_scr PUBLIC cxx_std_11)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_scr PUBLIC opennova_io)
