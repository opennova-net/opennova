#pragma once

// The canonical parity trace: one capture decoded into the structured records the
// LAN parity tooling compares (ADR 0050 R4). `opennova-wire --parity-events`
// prints it as stable lines; `opennova-parity` keeps it in memory and compares
// runs. Both drive the SAME builder over the shared outer-decode pipeline
// (wire_capture.h), so the printed contract and the comparator never disagree.
//
//   PARITY_HANDSHAKE  one per 0x41/0x42/0x81/0x82 datagram: its NWU-stripped body
//   PARITY_DATAGRAM   one per UDP payload, its outer class and acceptance
//   PARITY_PACKET     one per decrypted 0x43/0x83 session packet: header, record
//                     tags and the physical raw-flags/LEN/SKIP framing
//   PARITY_EVENT      one per reassembled message; the body is the byte witness,
//                     except the two high-rate gameplay bodies (C2S 0x0C, S2C
//                     0x0A) whose decoded STATE/ENTITY records carry the contract
//   PARITY_STATE      the decoded C2S 0x0C uplink / S2C 0x0A frame header
//   PARITY_ENTITY     one per S2C 0x0A per-entity compact record
//
// Every record carries its capture timestamp (ts_ns, nanoseconds since the epoch;
// 0 when the input has none) and its capture frame. The line formats are a
// contract scripts/net/wait_parity_wire_ready.ps1 parses: never change one.

#include "item_tables.h"

#include <net/npwire/wire_capture.h>

#include <base/pcapio/pcap_reader.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace opennova {
struct TitleIdentity; // net/title/title_identity.h
}

namespace opennova::wire {

// The title a session handshake datagram (a 0x41 / 0x42 / 0x81) names by its PN, as a host's
// gate reads it (title_identity_by_protocol_name); null for any other datagram or an unknown PN.
// The in-match bodies a title edits (the S2C 0x0A objective body, DFX2-1) decode by it.
const TitleIdentity *handshake_title(const std::vector<uint8_t> &raw);

struct ParityHandshake {
	int frame = 0;
	uint64_t ts_ns = 0;
	char dir = '?'; // 'C' for 0x41/0x42, 'S' for 0x81/0x82
	int session = 0; // the client-side UDP port
	uint8_t opcode = 0;
	bool decoded = false; // the opcode's hello/auth parser accepted the body
	std::vector<uint8_t> body; // the flat-TLV body after the opcode, NWU-stripped
};

struct ParityDatagram {
	int frame = 0;
	uint64_t ts_ns = 0;
	int src_port = 0;
	int dst_port = 0;
	size_t length = 0; // UDP payload bytes
	bool outer_decoded = false;
	uint8_t opcode = 0; // meaningful only when outer_decoded
	CaptureDatagramClass datagram_class = CaptureDatagramClass::Invalid;
	bool decoded = false;
};

struct ParityWireRecord {
	uint16_t full_tag = 0;
	uint8_t raw_flags = 0;
	uint32_t encoded_length = 0;
	std::vector<uint8_t> skip_bytes;
};

struct ParityPacket {
	int frame = 0;
	uint64_t ts_ns = 0;
	char dir = '?';
	int session = 0;
	uint32_t sid = 0;
	uint32_t seq = 0;
	uint32_t ack = 0;
	uint8_t flags = 0;
	std::vector<uint16_t> tags; // full 9-bit tags, packet order
	std::vector<ParityWireRecord> records; // the physical records, same order
};

struct ParityEvent {
	int frame = 0; // the frame the message started on (its first fragment)
	uint64_t ts_ns = 0;
	char dir = '?';
	int session = 0;
	int participant = 0;
	uint16_t full_tag = 0; // the 9-bit dispatch identity; the low byte is the tag
	bool settings_update = false;
	size_t length = 0;
	bool material = false; // the body is the witness (false: C2S 0x0C, S2C 0x0A)
	std::vector<uint8_t> body; // empty when !material
};

// The decoded C2S 0x0C extended uplink (kind=uplink).
struct ParityUplinkState {
	int frame = 0;
	uint64_t ts_ns = 0;
	int session = 0;
	bool decoded = false; // a clean extended sub-op decode to the last byte
	size_t length = 0;
	uint32_t handle = 0, type_id = 0, sub_op = 0, carrier = 0;
	int32_t pos_x = 0, pos_y = 0, pos_z = 0;
	int32_t heading = 0, pitch = 0;
	uint32_t move = 0, state_flags = 0;
	uint32_t analog_x = 0, analog_y = 0, analog_z = 0;
	uint32_t adm = 0;
	uint32_t priority_nonzero = 0;
	uint32_t priority_handle[4] = { 0, 0, 0, 0 };
	uint32_t priority_score[4] = { 0, 0, 0, 0 };
};

// The decoded S2C 0x0A frame header (kind=frame).
struct ParityFrameState {
	int frame = 0;
	uint64_t ts_ns = 0;
	int session = 0;
	bool decoded = false;
	size_t length = 0;
	uint32_t sub_block = 0, flags1 = 0, flags2 = 0;
	int32_t anchor_x = 0, anchor_y = 0, anchor_z = 0;
	bool local_tail = false;
	uint32_t state_flag_byte = 0;
	int32_t health = 0, armor = 0;
	size_t records = 0;
	uint32_t players = 0, vehicles = 0, infantry = 0, none = 0;
	size_t rounds = 0;
	uint32_t carried = 0;
	bool weapon_present = false;
	uint32_t weapon[7] = { 0, 0, 0, 0, 0, 0, 0 }; // preround, 360, 368, 364, 356, 460, reload
	int32_t weapon_uniform_team_mask = 0;
	bool timer_present = false;
	uint32_t timer_state[4] = { 0, 0, 0, 0 };
	int32_t timer_seconds = 0;
	bool env_present = false;
	uint32_t env[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }; // fog dist/accel, tod, quake, cloud, rain,
												  // overcast, param
	bool objective_present = false;
	int32_t objective[4] = { 0, 0, 0, 0 };
	bool mount_present = false;
	uint32_t mount_handle = 0;
	bool mount_bound = false;
	uint32_t mount_clip = 0, mount_reserve = 0;
};

// One S2C 0x0A per-entity compact record.
struct ParityEntity {
	int frame = 0;
	uint64_t ts_ns = 0;
	int session = 0;
	char cls = '?'; // P player, V vehicle, I infantry, N no network callback
	uint32_t handle = 0, type_id = 0;
	uint32_t pos_x = 0, pos_y = 0, pos_z = 0; // the compressed codes, as sent
	int32_t orient_a = 0, orient_b = 0;
	uint32_t state_a = 0, state_b = 0, vital = 0;
	// The decompressed offset (network_decompress_fixedpoint, i32 16.16) and, for a
	// record not on a carrier, the world position: the frame anchor plus that offset.
	// A carrier-local record (a mount / ground carrier / vehicle parent handle) is
	// relative to its carrier, so its world fields hold the offset alone. Not printed
	// (the PARITY_ENTITY line is a contract).
	int32_t offset_x = 0, offset_y = 0, offset_z = 0;
	int32_t world_x = 0, world_y = 0, world_z = 0;
	bool carrier_local = false;
};

// The records in emission order, delivered as they are built.
class ParityTraceSink {
public:
	virtual ~ParityTraceSink() = default;
	virtual void on_handshake(const ParityHandshake &) {}
	virtual void on_datagram(const ParityDatagram &) {}
	virtual void on_packet(const ParityPacket &) {}
	virtual void on_event(const ParityEvent &) {}
	virtual void on_uplink(const ParityUplinkState &) {}
	virtual void on_frame(const ParityFrameState &) {}
	virtual void on_entity(const ParityEntity &) {}
};

// Builds the trace over the shared decoder. Streaming (a pcap): push() each
// datagram in capture order; per datagram it emits the handshake record, the
// datagram record, the packet record, then each completed message's event and
// its decoded state/entities. Batch (a hexcap, no timestamps, no handshake
// records): push_decoded() one whole decode_capture result.
class ParityTraceBuilder {
public:
	// `item_classes` (optional) sizes the S2C 0x0A trailing records; without it
	// only the universal player template decodes.
	explicit ParityTraceBuilder(ParityTraceSink &sink, const ItemClassMap *item_classes = nullptr);

	void push(const net::PcapDatagram &datagram);
	void push_decoded(
			const CaptureDecodeResult &decoded, const std::map<int, uint64_t> &frame_timestamps);

private:
	void emit_handshake(
			int frame, uint64_t ts_ns, int src_port, int dst_port, const std::vector<uint8_t> &raw);
	void emit_decoded(
			const CaptureDecodeResult &decoded, const std::map<int, uint64_t> &frame_timestamps);
	void emit_message(const InGameMessage &message, uint64_t ts_ns);

	ParityTraceSink &sink_;
	const ItemClassMap *item_classes_;
	CaptureDecoder decoder_;
	std::map<int, uint64_t> frame_ts_;
	// g_GameType, learned from the S2C 0x7B `extra` field: the 0x0A sub-block 3
	// objective body is gated on it, off the wire.
	uint32_t game_type_ = 0;
	// The title the handshake names (handshake_title): a DFX2 host's objective
	// body carries the co-op checkpoint. Null decodes JO's.
	const TitleIdentity *title_ = nullptr;
};

// The `opennova-wire --parity-events` line printer.
class ParityTracePrinter : public ParityTraceSink {
public:
	explicit ParityTracePrinter(std::FILE *out) : out_(out) {}
	void on_handshake(const ParityHandshake &record) override;
	void on_datagram(const ParityDatagram &record) override;
	void on_packet(const ParityPacket &record) override;
	void on_event(const ParityEvent &record) override;
	void on_uplink(const ParityUplinkState &record) override;
	void on_frame(const ParityFrameState &record) override;
	void on_entity(const ParityEntity &record) override;

private:
	std::FILE *out_;
};

// A whole capture held in memory, each kind in emission order.
struct ParityTrace {
	std::vector<ParityHandshake> handshakes;
	std::vector<ParityDatagram> datagrams;
	std::vector<ParityPacket> packets;
	std::vector<ParityEvent> events;
	std::vector<ParityUplinkState> uplinks;
	std::vector<ParityFrameState> frames;
	std::vector<ParityEntity> entities;
};

class ParityTraceCollector : public ParityTraceSink {
public:
	explicit ParityTraceCollector(ParityTrace &trace) : trace_(trace) {}
	void on_handshake(const ParityHandshake &record) override;
	void on_datagram(const ParityDatagram &record) override;
	void on_packet(const ParityPacket &record) override;
	void on_event(const ParityEvent &record) override;
	void on_uplink(const ParityUplinkState &record) override;
	void on_frame(const ParityFrameState &record) override;
	void on_entity(const ParityEntity &record) override;

private:
	ParityTrace &trace_;
};

// Stream a pcap/pcapng into `out`. False when the file cannot be read as one.
bool read_parity_trace(const std::string &path, const ItemClassMap *item_classes, ParityTrace &out);

// The PARITY_DATAGRAM class names.
const char *parity_datagram_class_name(CaptureDatagramClass datagram_class);

} // namespace opennova::wire
