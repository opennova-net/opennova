#pragma once

#include <cstdint>

namespace opennova {

// NWU cipher keys used on the UDPNOVAWORLD (session) channel.
//
// The session channel differs from the gate channel in two ways:
//   1. The first byte of the CRC-stripped payload is an opcode (not
//      encrypted); the NWU cipher is applied only to bytes [1..].
//   2. The NWU key is a 46-character hardcoded literal, not "GATEAPI".
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
//   0x86 — ServerGoodBye (S2C; the host-initiated teardown burst)

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
// The host-initiated teardown burst: the same disconnect-record body as the
// C2S 0x46 ClientGoodBye, keyed by the CLIENT's key (CK). Dispatched by the
// retail client's opcode table entry 12 -> Nwu_HandleServerGoodbye.
// [orig: g_np_opcode_handlers @0x849D90 entry 12 -> Nwu_HandleServerGoodbye
//  @0x624310; writer opcode select CNapiNPConnection_SendDisconnectPacket
//  @0x61f367 (0x86 when is_server, 0x46 @0x61f37b when is_client)]
inline constexpr uint8_t SESSION_OPCODE_SERVER_GOODBYE = 0x86;
// The outer-namespace ping pair: the receiver-local key dword, then the WR (u8
// wants-reply) and MS (u32 sender ms) flat TLVs; a WR reply echoes MS with WR
// clear and the receiver stores `now - MS` as the session RTT.
// [orig: g_np_opcode_handlers @0x849D90 entry 4 {0x45 -> Nwu_HandleClientPing
//  @0x624220} and entry 11 {0x85 -> Nwu_HandleServerPing @0x6242E0}, both thin
//  wrappers over Nwu_HandlePing @0x623A70; writer CNapiNPConnection_SendPing
//  @0x61F080 selects 0x85 when is_server @0x61F131, 0x45 for a client @0x61F145]
inline constexpr uint8_t SESSION_OPCODE_CLIENT_PING = 0x45;
inline constexpr uint8_t SESSION_OPCODE_SERVER_PING = 0x85;

} // namespace opennova
