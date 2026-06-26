add_library(opennova_npruntime STATIC
    src/server_session.cpp
)

target_include_directories(opennova_npruntime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The IDA-faithful in-match NAPI NP runtime (the named server/client state machines + frame
# loop). Builds on the netsim seam (world <-> wire bridge + transport) and the wire codecs.
# Strictly Godot-free and socket-free: the owner (apps/nw_server or the Godot binding) pumps
# bytes through netsim::ISessionTransport.
target_link_libraries(opennova_npruntime PUBLIC opennova_netsim)

target_compile_features(opennova_npruntime PUBLIC cxx_std_17)

set_target_properties(opennova_npruntime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
