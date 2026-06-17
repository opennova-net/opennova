#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {

// Opcode 0x43 (ClientProtocolMessage) / 0x83 (ServerProtocolMessage) wire
// layout — the all-purpose post-auth traffic carrier.
//
// **Witnessed in retail Jointops.exe** (re-validated 2026-04-25, see
// notes/ida_witness_matrix.md + notes/dispatcher_table.md):
//   `NapiNPProtocol_HandleSessionPacket @ 0x626A00` — opcode 0x43/0x83
//      entry point. Decrypts payload with SESSION_NWU_KEY, validates that
//      bytes 0..3 (`session_key`) match `conn->session_keys.local_key`,
//      reads `session_seq` at bytes 4..7 for ordering/dedup, then forwards
//      to ParseMessages once `session_seq == session_seq_last + 1`.
//   `NapiNPConnection_ParseMessages @ 0x625BC0` — re-decrypts with
//      SESSION_NWU_KEY then advances cursor past the 4-byte session_key
//      (via `cursor = v46` = `dst + 4`), reads `session_seq` (bytes 4..7,
//      stored as `conn->session_seq_last`), `ack_seq` (bytes 8..11, used
//      to prune outbound `msg_list`), then a 1-byte reserved/flags field
//      at offset 12 (cursor advances, value discarded). Inner stream
//      decrypted with `conn->crypto_key` (the per-session SCRK) starts at
//      byte 13.
//   `NapiNPConnection_DispatchMessage @ 0x622570` — per-message dispatch +
//      fragment reassembly via `NapiBuffer @ conn+860`.
//   `Nwu_HandleClientSession @ 0x626CF0` — opcode 0x43 thunk →
//      `HandleSessionPacket(..., has_seq=1)`.
//   `NapiNP_EncryptBuffer @ 0x6187B0` / `NapiNP_DecryptBuffer @ 0x618880`
//   SESSION_NWU_KEY string `"asdfj2349857qu23rija;..."` at `0x7DFC50`.
//
// Wire layout of one 0x43 packet after CRC-envelope strip:
//   offset size   field
//     +0    1     opcode (0x43 client, 0x83 server)
//     +1    4     session_id   — must match `conn->session_keys.local_key`
//                                so peer recognises this connection (set
//                                from CK exchanged in ClientAuth)
//     +5    4     seq_num      — per-packet ordering; receiver dedupes
//                                and only forwards when seq == last+1
//     +9    4     ack_count    — peer's "last seq we received from them"
//                                (prunes outbound retransmit queue)
//     +13   1     connection_flags — reserved on receive (cursor advances
//                                    but value discarded); send as 0
//     +14   N     inner-message region: each message is
//                   [flags:u8][tag:u8]
//                   (if flags & 0x20) [len_u8]
//                   (else if flags & 0x40) [len_u16_LE]   else len = 0
//                   (if flags & 0x08) [skip 1 byte]
//                   (if flags & 0x10) [skip 2 bytes]
//                   [payload of `len` bytes]
//                 doubly NWU-encrypted: first with SCRK (per-session,
//                 exchanged in ClientAuth/ServerAuth), then with the
//                 session key as part of the outer encrypt.
//
// Flag-byte bits per `NapiNPConnection_ParseMessages` body branches and
// `NapiNPConnection_DispatchMessage` fragment handling. Cross-checked
// against onnet's reference implementation at
// `opennova-godot-new/.scratch/onnet/onnw/nwu_protocol.py` (FRAG_CONT/
// FRAG_END enum) and `nw_udp_server.py` (process_protocol_message dispatch).
//   0x80 SETTINGS_UPDATE  — selects msginfo_high_* table in dispatcher
//   0x40 LEN16            — 16-bit length field follows
//   0x20 LEN8             — 8-bit length field follows
//   0x10 SKIP2            — 2 bytes consumed after length (not stored)
//   0x08 SKIP1            — 1 byte consumed after length (not stored)
//   0x04 FRAG_CONT        — "more fragments coming" (set on first AND mid
//                            fragments, cleared on the final fragment).
//                            Dispatcher rule: dispatch ONLY when this bit
//                            is clear, regardless of FRAG_END. The three
//                            states are: 0x04 alone = first chunk (reset
//                            buffer + append), 0x06 = mid chunk (append
//                            only), 0x02 alone = final chunk (append +
//                            dispatch reassembled).
//   0x02 FRAG_END         — set on mid AND final fragments
//   0x01 unused/reserved   — not part of the tag number in retail
//
// History: prior comment block cited `sub_5E91F0` / `sub_5E72E0` as
// jodemo.exe witnesses; those addresses resolve to particle-system code
// in the current Jointops.exe IDB. The 13-byte header layout itself is
// correct (verified against retail captures and re-confirmed via the
// Jointops decompile chain above).

constexpr uint8_t PROTOCOL_OPCODE_CLIENT = 0x43;
constexpr uint8_t PROTOCOL_OPCODE_SERVER = 0x83;
constexpr size_t PROTOCOL_PACKET_HEADER_SIZE = 13;

// Per-packet connection header (13 bytes, little-endian dwords).
struct ProtocolPacketHeader {
	uint32_t session_id = 0;
	uint32_t seq_num = 0;
	uint32_t ack_count = 0;
	uint8_t connection_flags = 0;
};

// Decoded inner ProtocolMessage flag-byte bits.
struct ProtocolMessageFlags {
	bool settings_update = false;  // 0x80
	bool len16 = false;            // 0x40
	bool len8 = false;             // 0x20
	bool skip2 = false;            // 0x10 — consumes 2 bytes after length
	bool skip1 = false;            // 0x08 — consumes 1 byte after length
	bool frag_cont = false;        // 0x04 — "more fragments coming" (set
	                               //         on first AND mid fragments)
	bool frag_end = false;         // 0x02 — "is final or mid fragment"
	bool msg_type_high_bit = false; // 0x80 — full_tag = 0x100 | tag
	uint8_t raw = 0;
};

// One inner message within a 0x43 payload.
struct ProtocolMessage {
	ProtocolMessageFlags flags;
	uint8_t tag = 0;
	uint16_t full_tag = 0; // 9-bit ((flags&0x80 ? 0x100 : 0) | tag)
	uint32_t length = 0;   // payload length in bytes
	std::vector<uint8_t> payload;
	// SKIP1/SKIP2 payload bytes — retail discards these on receive but
	// the cursor still advances. We retain them for byte-exact re-encode.
	// Empty when neither skip bit is set.
	std::vector<uint8_t> skip_bytes;
};

// Fragment reassembly state for one protocol session. The retail protocol
// uses FRAG_CONT (0x04) as "more fragments follow"; the flush point is the
// first message without FRAG_CONT.
struct ProtocolReassemblyState {
	std::vector<uint8_t> buffer;
};

ProtocolMessage make_protocol_message(uint8_t tag, std::vector<uint8_t> payload,
                                      uint8_t flags_raw = 0);

// Decode the 13-byte outer header from the first 13 bytes of the
// (outer-NWU-decrypted) packet body. Returns false on short input.
bool parse_protocol_packet_header(const uint8_t *data, size_t len,
                                  ProtocolPacketHeader &out);

// Parse the inner-message region (already inner-SCRK-decrypted). Returns
// true on success and populates `out` with one entry per inner message.
// Tolerant of trailing garbage (stops cleanly when nothing parseable
// remains).
bool parse_protocol_messages(const uint8_t *data, size_t len,
                             std::vector<ProtocolMessage> &out);

// Append one message to an already-decrypted inner-message stream. If
// msg.flags.raw is 0, the encoder chooses LEN8 or LEN16 from payload size.
bool append_protocol_message(std::vector<uint8_t> &out,
                             const ProtocolMessage &msg);

bool encode_protocol_messages(const std::vector<ProtocolMessage> &messages,
                              std::vector<uint8_t> &out);

// Encode/decode a ProtocolMessage body when the caller has already handled
// the outer SESSION_NWU_KEY transform around the 0x43/0x83 opcode payload.
// These helpers only touch the SCRK-encrypted inner-message region.
bool decode_protocol_packet_plaintext(const uint8_t *body, size_t body_len,
                                      std::string_view scrk,
                                      ProtocolPacketHeader &hdr_out,
                                      std::vector<ProtocolMessage> &messages_out);

bool encode_protocol_packet_plaintext(const ProtocolPacketHeader &hdr,
                                      const std::vector<ProtocolMessage> &messages,
                                      std::string_view scrk,
                                      std::vector<uint8_t> &body_out);

// Applies retail fragment semantics and returns true when `payload_out`
// contains a complete payload ready for higher-level dispatch.
bool reassemble_protocol_payload(ProtocolReassemblyState &state,
                                 const ProtocolMessage &msg,
                                 std::vector<uint8_t> &payload_out,
                                 bool *was_fragmented = nullptr);

// Convenience: given a buffer that has already had its CRC envelope
// stripped and opcode byte skipped (so `body` is the outer-encrypted
// region), run the two-layer decrypt + parse end-to-end. `session_nwu_key`
// is the global SESSION_NWU_KEY literal; `client_scrk` is the
// per-session key we received in ClientAuth. `body` is outer-decrypted in
// place; the inner-message region is decoded into `messages_out`.
bool decode_protocol_packet(uint8_t *body, size_t body_len,
                            std::string_view session_nwu_key,
                            std::string_view client_scrk,
                            ProtocolPacketHeader &hdr_out,
                            std::vector<ProtocolMessage> &messages_out);

} // namespace opennova
