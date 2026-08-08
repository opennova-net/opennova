add_library(opennova_playersav STATIC
    src/weapon_sav.cpp
)

target_include_directories(opennova_playersav
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_playersav PUBLIC cxx_std_17)

set_target_properties(opennova_playersav PROPERTIES POSITION_INDEPENDENT_CODE ON)

# io/byte_reader.h + io/byte_writer.h back the parse/serialize cursors.
target_link_libraries(opennova_playersav PUBLIC opennova_io)
