# The NovaLogic game wire protocol: the in-game message codec (what
# netsim/npruntime encode/decode and nw_pp pretty-prints, plus the shared
# peer_addr.h POD), the NWU session framing half of session establishment
# (hello/keys, the flag-byte protocol_message framing), and the capture
# decode + replay chain (wire_capture, replay timeline, .sph serverlog).
# Extracted from libs/novaworld by ADR 0019: matchmaking (novaworld) sits ON
# this wire base, never the other way around. Headers stay flat under
# include/npwire/; sources group by leg under src/.
#
# Held to wire compatibility like every protocol lib (libs/CLAUDE.md): the
# witness record is docs/net/novaworld-net-re.md. C++-linked only — npwire
# is not part of the flat C ABI (opennova_shared).

add_library(opennova_npwire STATIC
    src/wire/entity_class.cpp
    src/wire/ingame_decode.cpp
    src/wire/ingame_encode.cpp
    src/session/session_hello.cpp
    src/session/nw_session_framing.cpp
    src/session/protocol_message.cpp
    src/replay/wire_capture.cpp
    src/replay/replay_timeline.cpp
    src/replay/serverlog_decode.cpp
)

target_include_directories(opennova_npwire
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_npwire
    PUBLIC opennova_io opennova_novacrypto opennova_napi
)

target_compile_features(opennova_npwire PUBLIC cxx_std_17)
