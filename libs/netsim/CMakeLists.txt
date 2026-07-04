add_library(opennova_netsim STATIC
    src/loopback_channel.cpp
    src/serializing_sink.cpp
    src/entity_wire_bridge.cpp
    src/net_client_view.cpp
    src/connection_fan.cpp
    src/udp_session_transport.cpp
)

target_include_directories(opennova_netsim
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The single deliberate bridge between the runtime world and the wire codec
# (ADR 0009/0011): the only library that depends on BOTH opennova_world and
# opennova_novaworld. Keeps libs/world net-agnostic and libs/npwire sim-agnostic.
target_link_libraries(opennova_netsim PUBLIC opennova_world opennova_npwire)

target_compile_features(opennova_netsim PUBLIC cxx_std_17)

set_target_properties(opennova_netsim PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
