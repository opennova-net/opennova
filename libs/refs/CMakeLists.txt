add_library(opennova_refs STATIC
    src/refs.cpp
    src/refs_env.cpp
    src/refs_cbin.cpp
    src/refs_def.cpp
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
)

target_compile_features(opennova_refs PUBLIC cxx_std_17)
