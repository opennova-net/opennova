add_library(opennova_def STATIC
    src/def.cpp          # the .def umbrella + loadout weight/encumbrance
    src/def_ammo.cpp     # AMMO.DEF
    src/def_hudpos.cpp   # HUDPOS.DEF
    src/def_items.cpp    # ITEMS.DEF
    src/def_scan.cpp     # the shared .def text scanner
    src/def_weapons.cpp  # WEAPONS.DEF
)

target_include_directories(opennova_def
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_def PUBLIC cxx_std_11)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_def PUBLIC opennova_io)
