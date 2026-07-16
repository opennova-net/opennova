add_library(opennova_foliage STATIC
    src/foliage.cpp
    src/runtime.cpp
)

target_include_directories(opennova_foliage
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_foliage PUBLIC cxx_std_17)
