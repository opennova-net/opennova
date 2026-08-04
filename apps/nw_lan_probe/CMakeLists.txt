add_library(opennova_lan_probe_client STATIC
    lan_probe_client.cpp
)

target_include_directories(opennova_lan_probe_client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(opennova_lan_probe_client
    PUBLIC
        opennova_npruntime
        opennova_app_common
)

target_compile_features(opennova_lan_probe_client PUBLIC cxx_std_17)

add_executable(opennova_nw_lan_probe
    main.cpp
)

target_link_libraries(opennova_nw_lan_probe
    PRIVATE
        opennova_lan_probe_client
)

target_compile_features(opennova_nw_lan_probe PRIVATE cxx_std_17)
set_target_properties(opennova_nw_lan_probe PROPERTIES OUTPUT_NAME "nw-lan-probe")
