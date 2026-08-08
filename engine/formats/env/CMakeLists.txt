add_library(opennova_env STATIC
    src/env.cpp
    src/env_render.cpp
)

target_include_directories(opennova_env
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_env PUBLIC cxx_std_17)
target_link_libraries(opennova_env PRIVATE opennova_io)
