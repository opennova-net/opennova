add_library(opennova_app_common STATIC
    net_sockets.cpp
    pcap_writer.cpp
    pcap_reader.cpp
)

target_include_directories(opennova_app_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(opennova_app_common PUBLIC cxx_std_17)

if(WIN32)
    target_link_libraries(opennova_app_common PUBLIC ws2_32)
endif()
