add_library(opennova_refs STATIC
    src/refs.cpp
    src/refs_env.cpp
    src/refs_cbin.cpp
    src/refs_def.cpp
    src/refs_threedi.cpp
    src/refs_mission.cpp
    src/refs_mnu.cpp
    src/ref_graph.cpp
)

target_include_directories(opennova_refs
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_refs
    PRIVATE
        opennova_env
        opennova_cbin
        opennova_def
        opennova_threedi
        opennova_mission
        opennova_mnu
)

target_compile_features(opennova_refs PUBLIC cxx_std_17)
