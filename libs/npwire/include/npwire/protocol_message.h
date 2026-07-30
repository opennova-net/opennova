#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
// against the onnet reference implementation (`nwu_protocol.py` FRAG_CONT/
// FRAG_END enum; `nw_udp_server.py` process_protocol_message dispatch).
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

// The 0x43/0x83 session opcodes that carry protocol messages are named in
// npwire/session_keys.h (SESSION_OPCODE_PROTOCOL_MESSAGE /
// SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE) — one home, no duplicates here.
constexpr size_t PROTOCOL_PACKET_HEADER_SIZE = 13;

// The flag-byte bits documented above, as named masks. ProtocolMessageFlags
// (below) is the decoded bool view; raw composers/parsers use these.
// [orig: CNapiNPConnection_ParseMessages @0x625BC0 body branches;
//  CNapiNPConnection_DispatchMessage @0x622570 fragment handling]
inline constexpr uint8_t PROTOCOL_MSG_FLAG_SETTINGS_UPDATE = 0x80; // selects the msginfo_high_* dispatch table
inline constexpr uint8_t PROTOCOL_MSG_FLAG_LEN16 = 0x40;
inline constexpr uint8_t PROTOCOL_MSG_FLAG_LEN8 = 0x20;
inline constexpr uint8_t PROTOCOL_MSG_FLAG_SKIP2 = 0x10;
inline constexpr uint8_t PROTOCOL_MSG_FLAG_SKIP1 = 0x08;
inline constexpr uint8_t PROTOCOL_MSG_FLAG_FRAG_CONT = 0x04;
inline constexpr uint8_t PROTOCOL_MSG_FLAG_FRAG_END = 0x02;

// full_tag = (SETTINGS_UPDATE ? 0x100 : 0) | tag — the 9-bit dispatch key.
// Tags at/above this base dispatch through the high (settings/control) table,
// never the in-game msg-id tables (docs/net/novaworld-net-re.md §4).
inline constexpr uint16_t PROTOCOL_FULL_TAG_HIGH_BASE = 0x100;

// The four registered high-table control messages (§4 "High-table control
// messages"; NAPI control, not gameplay — in-game msg ids live in
// npwire/ingame_message_id.h).
namespace hightag {
inline constexpr uint8_t CS_CONFIG_UPDATE = 0x00;      // [orig: CNapiNPConnection_HandleCSConfigUpdate @0x621940]
inline constexpr uint8_t NAME_TAG_UPDATE = 0x01;       // [orig: CNapiNPConnection_HandleNameTagUpdate @0x6219F0]
inline constexpr uint8_t DATA_TRANSFER_CONTROL = 0x02; // [orig: CNapiNPConnection_ProcessDataTransferControl @0x62A040]
inline constexpr uint8_t DESCRIPTION_PACKET = 0x03;    // the DISCONNECT/PUNT carrier (§5.64)
                                                       // [orig: CNapiNPConnection_HandleDescriptionPacket @0x621AE0]
} // namespace hightag

// Full-tag form of hightag::DESCRIPTION_PACKET — the §5.64 disconnect/punt TLV
// carrier. Lives here because this header owns full_tag and the high bit; the
// TLV body parser (DisconnectEvent) stays in session_hello.h.
inline constexpr uint16_t PROTOCOL_TAG_CONNECTION_DESCRIPTION =
        PROTOCOL_FULL_TAG_HIGH_BASE | hightag::DESCRIPTION_PACKET;
static_assert(PROTOCOL_TAG_CONNECTION_DESCRIPTION == 0x103,
              "witnessed wire value; the composition must not drift");

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

// PREFER frame_session_packet: it owns the seq/ack/session_id stamping every live
// owner needs, and hand-stamping a header beside this call is how the listener's
// copy of that logic drifted (ADR 0013). This raw form stays public for tests and
// capture fixtures that must control the header explicitly.
bool encode_protocol_packet_plaintext(const ProtocolPacketHeader &hdr,
                                      const std::vector<ProtocolMessage> &messages,
                                      std::string_view scrk,
                                      std::vector<uint8_t> &body_out);

// The per-session SEQUENCING state (ADR 0013): one outbound counter plus the inbound sequence echoed
// as ACK. The three framing paths (host S2C / joiner C2S / lobby C2S) fill a ProtocolPacketHeader with
// `seq_num = next_outbound_seq++` and `ack_count = last_inbound_seq`. Owners with a complete retail
// recovery pump opt into contiguous admission; generic consumers use a no-queue high-water gate
// that admits newer sequences across gaps and suppresses zero/stale/duplicate packets. The per-site
// initial value differs by convention and is preserved by each owner
// (in-match starts at 1; the lobby field default is 0 but ClientSession::start() resets it to 1 — the
// live first packet is seq=1 either way).
// One decoded future packet held behind a missing sequence. Retail keeps the encrypted packet on
// its per-connection packet queue; retaining the decoded value here is equivalent at this seam.
struct QueuedSessionPacket {
	ProtocolPacketHeader header{};
	std::vector<ProtocolMessage> messages;
};

// The default cs_dir0.packet_queue_max copied into every retail connection. The JOINTOPERATIONS
// game template sets the same 100 as the NOVAWORLDUDP service template.
// Future packets beyond this many queued sequence numbers are consumed but not retained.
// [orig: CNapiNetwork_Init @0x4ca4a0 stores @0x4cab10/@0x4cabe0; HandleSessionPacket @0x626c18]
constexpr size_t SESSION_PACKET_QUEUE_MAX = 100;

struct SessionSequencing {
	uint32_t next_outbound_seq = 1; // post-incremented per framed packet
	uint32_t last_inbound_seq = 0;  // highest contiguous inbound seq; echoed as next ack_count
	// Only owners with a complete 0x44/0x84 receive-batch pump enable the retail contiguous gate.
	// Generic protocol consumers preserve the earlier decode-and-latch behavior so sharing this
	// framing helper cannot create an unrecoverable queue behind a missing packet.
	bool ordered_recovery_enabled = false;
	std::map<uint32_t, QueuedSessionPacket> queued_inbound;
	// Set when any future packet is observed and cleared only at the owner's receive-batch boundary.
	// The queue may have drained by then; in that case the boundary clears this without sending a
	// needless NACK. This is the retail recv-pump latch, not a per-datagram send trigger.
	bool missing_request_pending = false;
	// Retail retains the reliable message nodes assigned to each outbound packet sequence, not the
	// encrypted datagram. A requested resend reconstructs the records under that old sequence with
	// the sender's current ACK in the session header.
	std::map<uint32_t, std::vector<ProtocolMessage>> retained_outbound;
	size_t retained_outbound_message_count = 0;
	// Zero keeps retention disabled for protocol users that have not opted into the 0x44/0x84 flow.
	// Joint Operations game-session connections set this to retail's cs_dir0.msg_out_max (1200).
	size_t outbound_message_limit = 0;
};

// Joint Operations overrides the generic NAPI template's outbound-message pool to 0x4B0 records
// for both directions. Connection owners opt into retention by assigning this limit; zero remains
// the default for protocol-only callers that do not own the 0x44/0x84 recovery pump.
// [orig: CNapiNetwork_Init @0x4CAB20/@0x4CABF0]
constexpr size_t JO_SESSION_OUTBOUND_MESSAGE_MAX = 0x4B0;

// Retail's 0x44/0x84 NACK carries at most sixteen requested packet sequences. Its outer-NWU-
// decrypted body is `[peer_local_key:u32le][requested_seq:u32le...]`; there is no count field.
// [orig: BuildMissingSeqList @0x6234B0; SendMissingSeqList @0x623560;
// NapiNP_HandleResendList @0x623800]
constexpr size_t SESSION_RESEND_LIST_MAX = 16;

std::vector<uint32_t> build_session_missing_sequence_list(
		const SessionSequencing &seq, bool include_zero);

bool encode_session_resend_list(uint32_t remote_key,
		const std::vector<uint32_t> &requested_sequences,
		std::vector<uint8_t> &body_out);

bool decode_session_resend_list(const uint8_t *body, size_t body_len,
		uint32_t local_key, std::vector<uint32_t> &requested_sequences_out);

// Metadata from packets admitted by the active receive policy. Header-only packets count as
// admitted; stale/duplicate packets do not. Under ordered recovery, future packets remain
// unadmitted until their gap closes, and max_ack_count preserves the cumulative effect when one
// close drains several queued packet headers.
struct SessionDeframeAdmission {
	struct Packet {
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
	};

	bool admitted = false;
	uint32_t max_ack_count = 0;
	// True when this datagram was ahead of the contiguous frontier, including a duplicate already
	// queued or a new packet dropped because the future queue is full. Runtime clears the equivalent
	// retail latch after deciding whether a nonempty queue needs one 0x44/0x84 request.
	bool future_packet_seen = false;
	// Every packet admitted while closing this receive frontier, in sequence order. Keeping the
	// packet header beside its messages is required by semantic handlers whose decision depends on
	// the containing packet's cumulative ACK (deployment release is one such handler). The legacy
	// flat `messages_out` remains available for callers that do not need packet-local metadata.
	std::vector<Packet> packets;
};

// A per-session CRYPTO view (ADR 0013), assembled at frame/deframe time from a connection's handshake
// SCRK fields. It captures the direction asymmetry the three paths differ by: which SCRK encrypts our
// outbound inner region vs. decrypts the peer's inbound one, and which negotiated key is stamped into
// the outbound header session_id. (The views are non-owning — they point into the connection's stable
// std::string SCRK members and are consumed within one synchronous frame/deframe call.)
struct SessionCrypto {
	std::string_view out_scrk;       // encrypts our outbound inner region (frame_session_packet)
	std::string_view in_scrk;        // decrypts the peer's inbound inner region (deframe_session_packet)
	uint32_t session_id = 0;         // the peer local_key stamped into the outbound header
	// The receiver's local key expected in an inbound header. `nullopt` preserves the generic
	// decode/test API; live ordered JO/game-session owners provide the negotiated key. Retail
	// rejects a mismatch before reading sequence state or decrypting/parsing the inner stream.
	// [orig: NapiNPProtocol_HandleSessionPacket @0x626b72]
	std::optional<uint32_t> expected_inbound_session_id;
};

// Frame `messages` into a ProtocolPacketHeader + SCRK-encrypted inner body (NO outer NWU envelope — the
// caller applies nw_encode_outbound). Stamps session_id, seq_num =
// seq.next_outbound_seq++, ack_count = seq.last_inbound_seq, connection_flags = 0. Returns false only if
// the inner encode fails.
// [orig: CNapiNPConnection_SendSessionPacket @ 0x61edd0] fills the same 13-byte header after the opcode:
//   [0] remote_key (session_keys.remote_key @ conn+0x150) -> session_id
//   [4] packet_seq  (out_packet_seq @ conn+0x7ac, ++'d per packet in BuildOutgoingPackets @ 0x628430)
//   [8] ack_seq     (recv_ack_seq  @ conn+0x7b8, = last inbound packet seq)
//   [12] reserved byte 0 -> connection_flags
// then SCRK-encrypts the inner records with the OUTBOUND tx_crypto_key (net_state+0x9e @ conn+0xCC),
// and the caller wraps header+records in the static NWU session key (opcode excluded).
bool frame_session_packet(SessionSequencing &seq, const SessionCrypto &crypto,
                          const std::vector<ProtocolMessage> &messages,
                          std::vector<uint8_t> &body_out);

// Reconstruct one requested session packet. An old sequence does not advance the counter; exactly
// `next_outbound_seq` creates a new (normally empty) packet and advances it; a future sequence is
// rejected. Missing/ACK-retired records produce a valid header-only packet, matching retail.
bool frame_session_packet_for_sequence(SessionSequencing &seq, const SessionCrypto &crypto,
		uint32_t packet_sequence, std::vector<uint8_t> &body_out);

// Retire reliable records whose assigned packet sequence is covered by an ACK from a packet that
// crossed the contiguous receive gate.
void acknowledge_session_packets(SessionSequencing &seq, uint32_t ack_sequence);

// Inverse: validate the receiver-local session_id, SCRK-decrypt `body`, then apply the owner's receive
// policy. With `ordered_recovery_enabled`, exactly the next sequence dispatches; stale/duplicate
// sequences succeed with no messages; future sequences queue until the gap closes, when all newly
// contiguous messages drain in order. Generic consumers use a no-queue high-water policy: a newer
// sequence is admitted even across a gap, while zero/stale/duplicate sequences are suppressed.
// Returns false (leaving seq untouched) only if decode fails; a wrong session_id is quietly
// consumed without changing sequencing state. `admission_out`, when supplied, distinguishes admitted
// header-only packets from stale/future packets and reports the greatest ACK carried by every packet
// admitted during this call. The outer NWU envelope must already be stripped.
// [orig: NapiNPProtocol_HandleSessionPacket @0x626A00 gates/queues @0x626be0..0x626c3a;
// CNapiNPConnection_ParseMessages @0x625bc0 latches the admitted seq and decrypts records with the
// INBOUND crypto_key @conn+0x10c — the peer key, distinct from the outbound tx_crypto_key.]
bool deframe_session_packet(SessionSequencing &seq, const SessionCrypto &crypto,
                            const uint8_t *body, size_t body_len,
                            ProtocolPacketHeader &hdr_out,
                            std::vector<ProtocolMessage> &messages_out,
                            SessionDeframeAdmission *admission_out = nullptr);

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
