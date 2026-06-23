#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// NW-UDP session-channel outer framing + per-session key material, shared by
// every host-side accept path (apps/novaworld_server's listener and the
// in-engine HostSessionAccept). Extracted verbatim from the original listener
// so the two paths can never drift — there is exactly one decode_inbound /
// encode_outbound / SCRK generator in the tree.
//
// "Outer framing" = the NAPI CRC envelope + the SESSION_NWU_KEY layer that
// wraps every 0x41/0x42/0x43/0x46/0x81/0x82/0x83 opcode. The inner SCRK layer
// (0x43 ProtocolMessage region) is handled by protocol_message.h, NOT here.

// 61-char SCRK matching retail captures (ClientAuth/ServerAuth SCRK are both
// 61 chars, alphabet = A-Z0-9). Random per session is sufficient — only the
// length + alphabet are wire-significant.
std::string make_dev_scrk();

// 60-char lowercase-hex NWUID (retail format). Random per session.
std::string make_dev_nwuid();

// Per-connection ServerAuth.SK. Retail's value is unique per session; a single
// hardcoded constant aliases two concurrent sessions to outside observers.
uint32_t make_random_session_u32();

// Decode an inbound NW-UDP datagram. On success, populates `opcode_out`
// (byte 0 of the CRC-stripped packet, plaintext) and `body_out` (the
// NWU-decrypted payload after byte 0). Names are swapped vs onnet —
// server-side decrypt is our nwu_encrypt.
bool nw_decode_inbound(const uint8_t *raw, size_t raw_len,
                       uint8_t &opcode_out, std::vector<uint8_t> &body_out);

// Encode an outbound reply: prepend `opcode`, NWU-encrypt the body part
// (server-side encrypt is our nwu_decrypt), then NAPI envelope-wrap.
std::vector<uint8_t> nw_encode_outbound(uint8_t opcode, std::vector<uint8_t> body);

} // namespace opennova
