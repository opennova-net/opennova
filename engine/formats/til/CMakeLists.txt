add_library(opennova_til STATIC
    src/til_io.cpp
    src/til_foliage_blocker.cpp
    src/til_overlay_bake.cpp
)

target_include_directories(opennova_til
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_til PUBLIC cxx_std_17)
