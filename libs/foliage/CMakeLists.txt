add_library(opennova_foliage STATIC
    src/foliage.cpp
    src/placement.cpp
    src/far_mesh_emitter.cpp
    src/dispatcher.cpp
    src/model_placement.cpp
    src/model_dispatcher.cpp
    src/fd_bake.cpp
)

target_include_directories(opennova_foliage
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_foliage PUBLIC cxx_std_17)
