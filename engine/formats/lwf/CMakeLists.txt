add_library(opennova_lwf STATIC
    src/lwf.cpp
)

target_include_directories(opennova_lwf
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_lwf PUBLIC cxx_std_17)

set_target_properties(opennova_lwf PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
