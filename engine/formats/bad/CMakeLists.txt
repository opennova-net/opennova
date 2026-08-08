add_library(opennova_bad STATIC
    src/bad.cpp
    src/bad_write.cpp
)

target_include_directories(opennova_bad
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_bad PUBLIC cxx_std_11)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_bad PUBLIC opennova_io)
