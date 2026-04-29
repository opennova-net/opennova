#pragma once

#include "net_sockets.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace opennova::net {

// Minimal pcap writer: classic libpcap format, linktype DLT_RAW (101).
// Each record is a synthetic IPv4+UDP header followed by the raw payload,
// so Wireshark can open + dissect the file normally.
//
// Not thread-safe; the emulator is single-threaded. Guard with a mutex
// yourself if that ever changes.
class PcapWriter {
public:
	PcapWriter() = default;
	~PcapWriter() { close(); }

	PcapWriter(const PcapWriter &) = delete;
	PcapWriter &operator=(const PcapWriter &) = delete;

	// Open `path` and write the pcap global header. Returns true on success.
	// If already open, logs a warning and returns false.
	bool open(const std::string &path);

	// Is the writer currently open?
	bool is_open() const noexcept { return file_ != nullptr; }

	// Write one UDP record. `src`/`dst` are IPv4 endpoints (dotted quad
	// stored MSO-first in Endpoint). `data`/`len` is the UDP payload.
	// Silently no-ops when not open.
	void write_udp(const Endpoint &src, const Endpoint &dst,
	               const uint8_t *data, size_t len);

	// Flush + close. Safe to call even when not open.
	void close();

private:
	std::FILE *file_ = nullptr;
};

} // namespace opennova::net
