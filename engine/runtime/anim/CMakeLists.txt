add_library(opennova_anim STATIC
    src/adm.cpp
    src/aim_overlay.cpp
    src/anim_sample.cpp
)

target_include_directories(opennova_anim
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# adm.h uses io/export.h from a public header.
target_link_libraries(opennova_anim
    PUBLIC
        opennova_bad
        opennova_io
)

target_compile_features(opennova_anim PUBLIC cxx_std_17)
