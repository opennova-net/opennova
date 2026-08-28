# weapon.sav player-profile parse/serialize tests (engine/formats/playersav).
# The retail-corpus leg reads the install's weapon.sav (OPENNOVA_JO_DIR) and notes SKIP-LEG without it.
add_executable(playersav_weapon_sav_test weapon_sav_test.cpp)
target_link_libraries(playersav_weapon_sav_test PRIVATE opennova_formats)
target_include_directories(playersav_weapon_sav_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(playersav_weapon_sav_test PRIVATE cxx_std_17)
add_test(NAME playersav_weapon_sav COMMAND playersav_weapon_sav_test)
