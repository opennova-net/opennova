add_library(opennova_audio STATIC
    src/ambient_mixer.cpp
    src/sound_selector.cpp
    src/sound_profile.cpp
)

target_include_directories(opennova_audio
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_audio PUBLIC opennova_io)

target_compile_features(opennova_audio PUBLIC cxx_std_17)

set_target_properties(opennova_audio PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
