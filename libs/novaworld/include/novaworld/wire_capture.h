#pragma once

// The NovaWorld outer-decode pipeline, run over a whole capture.
//
// Every in-match consumer — nw_pp, the cross-validation harnesses, and the
// replay-timeline builder — needs the same chain to get from raw UDP datagrams
// to per-tag inner payloads: NAPI envelope (CRC strip) -> outer NWU transform
// -> recover each side's SCRK from the ClientAuth/ServerAuth handshake -> 0x43/
// 0x83 session opcode -> SCRK-decrypt the protocol packet -> reassemble
// fragmented protocol messages -> emit (dir, tag, payload). That chain used to
// be copy-pasted into each consumer; this is the one shared implementation.
//
// See docs/net/novaworld-net-re.md §3 (outer stack) and §4 (tag dispatch).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova {

// A capture datagram reduced to what the in-game decode needs: the UDP payload
// (the NAPI envelope onward) and its 1-based capture order. Deliberately
// decoupled from apps/common's PcapDatagram so libs/ stays free of app-layer
// dependencies — callers map their own datagram type onto this.
struct CaptureDatagram {
	int frame_index = 0;
	int src_port = 0;  // UDP source port; 0 = unknown (hexcap / crafted port-less)
	int dst_port = 0;  // UDP dest port; 0 = unknown
	std::vector<uint8_t> payload;
};

// One fully-decoded in-game protocol message: the reassembled inner body a
// per-tag decoder (ingame_decode.h) consumes, tagged with its direction and the
// capture frame the message STARTED on (the first fragment, matching nw_pp).
struct InGameMessage {
	int frame_index = 0;          // first-fragment capture order
	char dir = '?';               // 'C' = client->server, 'S' = server->client
	uint16_t tag = 0;             // protocol full_tag; the low byte is the dispatch tag
	bool settings_update = false; // ProtocolMessage.flags.settings_update
	int session = 0;              // per-session id = the client-side UDP port (the
	                              // distinct-participant key); 0 = single/unknown session
	std::vector<uint8_t> payload; // reassembled inner body
};

// Resumable form of the outer-decode pipeline for a LIVE feed: push datagrams as
// they arrive (in wire/capture order) and receive the in-game messages that
// completed on each push. It holds the per-session SCRK pair + per-direction
// reassembly state across calls, so SCRK recovered from an early ClientAuth /
// ServerAuth decrypts later protocol packets — exactly as the batch function
// does. This is the form the in-engine net client uses; re-running the
// whole-capture function over a growing buffer each frame would be O(n^2).
//
// decode_capture_to_messages() is now a thin loop over push(), so a sequence of
// push() calls produces byte-identical output to one batch call over the same
// datagrams (guarded by nw_capture_decoder_test).
class CaptureDecoder {
public:
	CaptureDecoder();
	~CaptureDecoder();
	CaptureDecoder(CaptureDecoder &&) noexcept;
	CaptureDecoder &operator=(CaptureDecoder &&) noexcept;
	CaptureDecoder(const CaptureDecoder &) = delete;
	CaptureDecoder &operator=(const CaptureDecoder &) = delete;

	// Feed one datagram (the NAPI envelope onward). Returns the in-game messages
	// that completed on this datagram (0 or more, in order).
	std::vector<InGameMessage> push(const CaptureDatagram &datagram);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// Drive the outer-decode pipeline over `datagrams` (in capture order) and return
// the ordered stream of reassembled in-game messages. SCRK is recovered from the
// handshake as it streams by, so the datagrams must include the ClientAuth /
// ServerAuth packets for protocol messages to decrypt (a mid-session capture
// without them yields no protocol messages — the same limitation nw_pp has).
std::vector<InGameMessage>
decode_capture_to_messages(const std::vector<CaptureDatagram> &datagrams);

} // namespace opennova
