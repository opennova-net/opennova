add_library(opennova_parity_opennova STATIC
    src/opennova_capture.cpp)

target_include_directories(opennova_parity_opennova
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(opennova_parity_opennova
    PUBLIC opennova_parity opennova_netsim opennova_world)
target_compile_features(opennova_parity_opennova PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(opennova_parity_opennova PRIVATE /W4 /permissive-)
else()
    target_compile_options(opennova_parity_opennova
        PRIVATE -Wall -Wextra -Wpedantic)
endif()
