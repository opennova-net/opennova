add_library(opennova_particle STATIC
    src/parser.cpp
    src/particle.cpp
    src/writer.cpp
    src/emitter.cpp
    src/effect_scene.cpp
)

target_include_directories(opennova_particle
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_particle PUBLIC opennova_io)

target_compile_features(opennova_particle PUBLIC cxx_std_17)
