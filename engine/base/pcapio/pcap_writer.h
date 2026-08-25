#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace opennova::net {

// WRITING the capture shape this repo already reads: a legacy pcap whose link
// type is DLT_RAW (101), one synthetic IPv4+UDP frame per datagram.
//
// The framing is deliberately identical to the retail-side hook's, which writes
// the same legacy header and synthesizes the same IPv4+UDP wrapper around each
// payload. That symmetry is the point: a capture taken from our client and one
// taken from the original game decode through the SAME pipeline (pcap_reader ->
// npwire decode_capture_to_messages -> apps/nw_pp), so the two are directly
// comparable per (direction, wire tag) instead of merely similar.
//
// Synthetic, not observed: the IP header is manufactured around a payload the
// process already holds, so these captures record what the APPLICATION sent and
// received. A NIC-level capture (dumpcap) records what reached the wire. They
// agree on payload bytes and disagree on anything the OS network stack owns —
// fragmentation, checksums, retransmit timing. For wire-tag coverage work that
// difference does not matter; for anything about the stack itself, it does.

// The 24-byte legacy-pcap global header (little-endian, DLT_RAW).
void append_pcap_global_header(std::vector<uint8_t> &out);

// One DLT_RAW record: the pcap record header, then a synthetic IPv4 header
// (with a real header checksum) and UDP header wrapping `payload`. IPs are
// native byte order (0x7F000001 == 127.0.0.1). A payload too large for a single
// unfragmented IPv4 datagram is dropped rather than truncated, so a reader
// never sees a half record.
void append_pcap_udp_record(std::vector<uint8_t> &out, uint32_t src_ip,
		uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
		const uint8_t *payload, size_t len, uint64_t ts_nanos);

// A capture file that grows as datagrams arrive, rather than buffering the
// session and serializing at the end — a match runs for minutes at 62 Hz, so
// the whole-session-in-memory shape build_pcap_udp() has (fine for the tiny
// inline captures tests craft) is the wrong one here.
class PcapUdpWriter {
public:
	PcapUdpWriter() = default;
	~PcapUdpWriter();
	PcapUdpWriter(const PcapUdpWriter &) = delete;
	PcapUdpWriter &operator=(const PcapUdpWriter &) = delete;

	// Create/truncate `path` and write the global header. False if the path
	// cannot be opened; the writer then stays closed and every write() is a
	// no-op, so a capture-enabled build never fails a session over a bad path.
	bool open(const std::string &path);
	bool is_open() const { return file_ != nullptr; }
	// Flush and close. Safe to call twice; the destructor calls it.
	void close();

	// Append one datagram. Silently ignored when closed.
	void write(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
			uint16_t dst_port, const uint8_t *payload, size_t len,
			uint64_t ts_nanos);

	// Datagrams accepted so far (excludes any dropped as oversize).
	uint64_t records() const { return records_; }

	// Open a writer when `env_var` names a non-empty path, else return nullptr.
	// The shared gate for every capture site, so a build nobody asked to record
	// pays one getenv at construction and nothing after.
	static std::unique_ptr<PcapUdpWriter> from_env(const char *env_var);

private:
	std::FILE *file_ = nullptr;
	uint64_t records_ = 0;
	std::vector<uint8_t> scratch_; // reused per record, so no per-datagram alloc
};

// Wall-clock nanoseconds for a capture timestamp.
uint64_t pcap_now_nanos();

} // namespace opennova::net
