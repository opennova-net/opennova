add_library(opennova_gameprofile STATIC
    src/gameprofile.c
)

target_include_directories(opennova_gameprofile
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
