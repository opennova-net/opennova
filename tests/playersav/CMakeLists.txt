# weapon.sav player-profile parse/serialize tests (libs/playersav).
# The retail-corpus leg is gated on OPENNOVA_WEAPON_SAV and skips as a pass.
add_executable(playersav_weapon_sav_test weapon_sav_test.cpp)
target_link_libraries(playersav_weapon_sav_test PRIVATE opennova_playersav)
target_include_directories(playersav_weapon_sav_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(playersav_weapon_sav_test PRIVATE cxx_std_17)
add_test(NAME playersav_weapon_sav COMMAND playersav_weapon_sav_test)
