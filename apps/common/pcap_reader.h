#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::net {

// One IPv4/UDP datagram extracted from a capture: the UDP payload plus the
// ports and 1-based capture order. `payload` is exactly the bytes after the UDP
// header — the NAPI envelope onward, the same slice the legacy hexcap format
// stored on each line.
struct PcapDatagram {
	int srcport = 0;
	int dstport = 0;
	int frame_index = 0; // 1-based order within the capture
	std::vector<uint8_t> payload;
	// Appended last so existing positional aggregate-init `{src, dst, frame,
	// payload}` callers keep compiling (ts defaults to 0).
	uint64_t ts_nanos = 0; // capture timestamp, nanoseconds since the epoch (0 if absent)
};

// Parse a legacy pcap or pcapng buffer already in memory; append every IPv4/UDP
// datagram to `out`. Returns false on unrecognized magic / unsupported variant
// (big-endian pcapng). IP fragments are skipped — when `frags_dropped` is
// non-null it receives the count (loopback MTU 65535 → not expected).
//
// Supported link types: NULL/LOOP (BSD/OpenBSD loopback), Ethernet II, RAW,
// IPv4 — the slice nw_pp and the in-game decoders consume.
//
// Spec sources: pcap-savefile(5); the pcapng block-format spec
// (github.com/pcapng/pcapng) — SHB/IDB/EPB/SPB; tcpdump.org/linktypes.html.
bool read_pcap_udp(const uint8_t *data, size_t len,
                   std::vector<PcapDatagram> &out, int *frags_dropped = nullptr);

// Convenience wrapper: slurp `path` then read_pcap_udp(). Returns false if the
// file can't be read or isn't a pcap/pcapng. Loads the WHOLE file into memory —
// use stream_pcap_udp_file for multi-GB captures.
bool read_pcap_udp_file(const std::string &path, std::vector<PcapDatagram> &out,
                        int *frags_dropped = nullptr);

// Stream a pcap/pcapng file one record/block at a time, invoking `on_datagram`
// for each IPv4/UDP datagram in order, WITHOUT loading the whole file — flat
// memory regardless of file size (for multi-GB captures). The datagram reference
// is only valid for the duration of the callback; return false from it to stop
// early (e.g. once a partition is established). Returns false on bad magic / IO.
bool stream_pcap_udp_file(const std::string &path,
                          const std::function<bool(const PcapDatagram &)> &on_datagram,
                          int *frags_dropped = nullptr);

// Build a minimal legacy pcap (DLT_RAW: one synthetic IPv4+UDP frame per
// datagram) in memory — for crafting tiny inline captures in tests. Only
// srcport/dstport/ts_nanos/payload are used; src/dst IPs are 127.0.0.1. The
// timestamp is written at microsecond resolution (ts_nanos truncated), so the
// result round-trips through read_pcap_udp() at that resolution.
std::vector<uint8_t> build_pcap_udp(const std::vector<PcapDatagram> &dgrams);

} // namespace opennova::net
