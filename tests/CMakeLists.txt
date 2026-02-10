# Roundtrip test
add_executable(threedi_roundtrip_test threedi/roundtrip_test.cpp)
target_link_libraries(threedi_roundtrip_test PRIVATE opennova_threedi)
target_include_directories(threedi_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_roundtrip COMMAND threedi_roundtrip_test)

# GP format parsing test
add_executable(threedi_gp_parse_test threedi/gp_parse_test.cpp)
target_link_libraries(threedi_gp_parse_test PRIVATE opennova_threedi)
target_include_directories(threedi_gp_parse_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_gp_parse COMMAND threedi_gp_parse_test)

# GP format roundtrip test
add_executable(threedi_gp_roundtrip_test threedi/gp_roundtrip_test.cpp)
target_link_libraries(threedi_gp_roundtrip_test PRIVATE opennova_threedi)
target_include_directories(threedi_gp_roundtrip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME threedi_gp_roundtrip COMMAND threedi_gp_roundtrip_test)

# PANM unit tests
foreach(test ctrl decode runtime wave)
    add_executable(threedi_panm_${test}_test threedi/panm_${test}_test.cpp)
    target_link_libraries(threedi_panm_${test}_test PRIVATE opennova_threedi)
    target_include_directories(threedi_panm_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME threedi_panm_${test} COMMAND threedi_panm_${test}_test)
endforeach()

# ADM tests
foreach(test comment trim_value parse)
    add_executable(adm_${test}_test adm/adm_${test}_test.cpp)
    target_link_libraries(adm_${test}_test PRIVATE oed_adm)
    target_include_directories(adm_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME adm_${test} COMMAND adm_${test}_test)
endforeach()

# DEF tests
add_executable(def_comment_strip_test def/def_comment_strip_test.cpp)
target_link_libraries(def_comment_strip_test PRIVATE oed_def)
target_include_directories(def_comment_strip_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME def_comment_strip COMMAND def_comment_strip_test)

foreach(test items weapons ammo hudpos)
    add_executable(def_parse_${test}_test def/def_parse_${test}_test.cpp)
    target_link_libraries(def_parse_${test}_test PRIVATE oed_def)
    target_include_directories(def_parse_${test}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME def_parse_${test} COMMAND def_parse_${test}_test)
endforeach()

# PFF tests
add_executable(pff_unit_test pff/pff_unit_test.cpp)
target_link_libraries(pff_unit_test PRIVATE opennova_pff)
target_include_directories(pff_unit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pff_unit COMMAND pff_unit_test)
