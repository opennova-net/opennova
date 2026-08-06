add_library(opennova_controls STATIC
    src/controls.cpp
)

target_include_directories(opennova_controls
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_controls PUBLIC cxx_std_17)
