add_library(opennova_anim STATIC
    src/anim_sample.cpp
)

target_include_directories(opennova_anim
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_anim
    PUBLIC
        opennova_bad
        opennova_adm
)

target_compile_features(opennova_anim PUBLIC cxx_std_17)
