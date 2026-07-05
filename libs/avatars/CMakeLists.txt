add_library(opennova_avatars STATIC
    src/avatars.cpp
)

target_include_directories(opennova_avatars
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_avatars PUBLIC cxx_std_17)

set_target_properties(opennova_avatars PROPERTIES POSITION_INDEPENDENT_CODE ON)
