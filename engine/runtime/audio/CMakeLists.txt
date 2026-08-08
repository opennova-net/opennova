add_library(opennova_audio STATIC
    src/ambient_mixer.cpp
    src/bank_chain.cpp
    src/envs_markers.cpp
    src/sound_selector.cpp
    src/sound_profile.cpp
)

target_include_directories(opennova_audio
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# def + mission_format: the S13 envs-marker resolution reads the items.def
# dispatch tags/soundloops against the mission's placed entities (both leaf
# parse libs — no cycle).
target_link_libraries(opennova_audio PUBLIC
    opennova_io opennova_def opennova_mission_format)

target_compile_features(opennova_audio PUBLIC cxx_std_17)

set_target_properties(opennova_audio PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
