#pragma once

#include <cstdint>

namespace opennova {

// NWU cipher keys used on the UDPNOVAWORLD (session) channel.
//
// The session channel differs from the gate channel in two ways:
//   1. The first byte of the CRC-stripped payload is an opcode (not
//      encrypted); the NWU cipher is applied only to bytes [1..].
//   2. The NWU key is a 50-character hardcoded literal, not "GATEAPI".
//
// Key literal witnessed at `.rdata:0x75b7a4`, referenced from
// `NapiNPSession_SendDescription@0x5e9840` via
//   Crypto_DecryptBuffer(&payload[1], len - 1,
//                        "asdfj2349857qu23rija;sdlvzx09caweklrj1234hldfj");
// onnet's Python emulator uses the same literal (via `NW_UDP_KEY`
// environment variable with this default), confirming this is the
// jodemo-expected key and not a deployment-configurable choice.
//
// Session opcode layout (byte 0 of CRC-stripped packet):
//   0x41 — ClientHello (C2S)
//   0x42 — ClientJoin  (C2S)
//   0x43 — ProtocolMessage (C2S, payload is a stream of containers)
//   0x44 — ClientResendList (C2S)
//   0x46 — ClientGoodBye (C2S)
//   0x81 — ServerHello (S2C response to 0x41)
//   0x82 — ServerJoin  (S2C response to 0x42)
//   0x83 — ProtocolMessage (S2C, payload is a stream of containers)
//   0x84 — ServerResendList (S2C)

inline constexpr const char *SESSION_NWU_KEY =
		"asdfj2349857qu23rija;sdlvzx09caweklrj1234hldfj";

// Opcodes (witnessed by onnet's NWUProtocol dispatch; individual
// verifications in IDA are per-handler work).
inline constexpr uint8_t SESSION_OPCODE_CLIENT_HELLO = 0x41;
// 0x42/0x82 are the novaworld-service auth handshake. onnet calls them
// "ClientJoin"/"ServerJoin" — misleading (no game-server join is involved
// here — see plan + feedback_net_terminology memory), so those names are
// not mirrored.
inline constexpr uint8_t SESSION_OPCODE_CLIENT_AUTH = 0x42;
inline constexpr uint8_t SESSION_OPCODE_PROTOCOL_MESSAGE = 0x43;
inline constexpr uint8_t SESSION_OPCODE_CLIENT_RESEND_LIST = 0x44;
inline constexpr uint8_t SESSION_OPCODE_CLIENT_GOODBYE = 0x46;
inline constexpr uint8_t SESSION_OPCODE_SERVER_HELLO = 0x81;
inline constexpr uint8_t SESSION_OPCODE_SERVER_AUTH = 0x82;
inline constexpr uint8_t SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE = 0x83;
inline constexpr uint8_t SESSION_OPCODE_SERVER_RESEND_LIST = 0x84;

} // namespace opennova
