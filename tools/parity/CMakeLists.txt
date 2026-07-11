add_library(opennova_parity_tool_core STATIC
    src/evidence_compare.cpp
    src/network_enrichment.cpp
    src/pcapng_export.cpp
    src/tool.cpp)

target_include_directories(opennova_parity_tool_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(opennova_parity_tool_core PUBLIC cxx_std_17)
target_link_libraries(opennova_parity_tool_core
    PUBLIC
        opennova_parity
        opennova_npwire
        opennova_app_common)

add_executable(opennova_parity_tool
    src/main.cpp)
target_compile_features(opennova_parity_tool PRIVATE cxx_std_17)
target_link_libraries(opennova_parity_tool PRIVATE
    opennova_parity_tool_core)

if(MSVC)
    target_compile_options(opennova_parity_tool_core PRIVATE /W4 /permissive-)
    target_compile_options(opennova_parity_tool PRIVATE /W4 /permissive-)
else()
    target_compile_options(opennova_parity_tool_core PRIVATE
        -Wall -Wextra -Wpedantic)
    target_compile_options(opennova_parity_tool PRIVATE
        -Wall -Wextra -Wpedantic)
endif()
