add_library(opennova_gameprofile STATIC
    src/gameprofile.c
    src/required_resources.c
)

target_include_directories(opennova_gameprofile
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_gameprofile PUBLIC opennova_io)
