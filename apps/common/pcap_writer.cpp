#include "pcap_writer.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace opennova::net {

namespace {

// Classic pcap global header (little-endian), 24 bytes.
struct PcapGlobalHeader {
	uint32_t magic;        // 0xa1b2c3d4
	uint16_t version_major; // 2
	uint16_t version_minor; // 4
	int32_t thiszone;       // 0
	uint32_t sigfigs;       // 0
	uint32_t snaplen;       // 65535
	uint32_t linktype;      // 101 = DLT_RAW (raw IPv4)
};

// Per-record header (little-endian), 16 bytes.
struct PcapRecordHeader {
	uint32_t ts_sec;
	uint32_t ts_usec;
	uint32_t incl_len; // bytes captured (== orig_len for us)
	uint32_t orig_len;
};

// RFC 791 IPv4 header, 20 bytes, big-endian on wire.
struct Ipv4Header {
	uint8_t ver_ihl;     // 0x45 = version 4, IHL 5 (20 bytes)
	uint8_t tos;         // 0
	uint16_t total_len;  // big-endian (header + UDP)
	uint16_t id;         // 0
	uint16_t flags_frag; // 0x4000 = DF
	uint8_t ttl;         // 64
	uint8_t protocol;    // 17 = UDP
	uint16_t checksum;   // big-endian
	uint32_t src_ip;     // big-endian (MSO first)
	uint32_t dst_ip;     // big-endian
};

// UDP header, 8 bytes, big-endian on wire.
struct UdpHeader {
	uint16_t src_port; // big-endian
	uint16_t dst_port; // big-endian
	uint16_t length;   // big-endian (header + payload)
	uint16_t checksum; // 0 (valid per RFC 768 when IPv4)
};

inline uint16_t hton16(uint16_t v) {
	return static_cast<uint16_t>((v >> 8) | (v << 8));
}

inline uint16_t ipv4_checksum(const void *hdr, size_t len) {
	const uint8_t *p = static_cast<const uint8_t *>(hdr);
	uint32_t sum = 0;
	for (size_t i = 0; i + 1 < len; i += 2) {
		sum += (static_cast<uint32_t>(p[i]) << 8) | p[i + 1];
	}
	if (len & 1) sum += static_cast<uint32_t>(p[len - 1]) << 8;
	while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
	return static_cast<uint16_t>(~sum & 0xFFFFu);
}

uint32_t pack_ipv4_be(const std::array<uint8_t, 4> &octets) {
	// Endpoint stores octets MSO first; IPv4 wire is also MSO first,
	// so we just pack them in the same order and the resulting uint32
	// on-wire bytes match. Use htonl-style packing here.
	return (static_cast<uint32_t>(octets[0]) << 24) |
	       (static_cast<uint32_t>(octets[1]) << 16) |
	       (static_cast<uint32_t>(octets[2]) << 8) |
	       (static_cast<uint32_t>(octets[3]));
}

} // namespace

bool PcapWriter::open(const std::string &path) {
	if (file_) {
		std::fprintf(stderr, "PcapWriter: already open, ignoring second open(%s)\n", path.c_str());
		return false;
	}
	file_ = std::fopen(path.c_str(), "wb");
	if (!file_) {
		std::fprintf(stderr, "PcapWriter: failed to open '%s'\n", path.c_str());
		return false;
	}
	PcapGlobalHeader hdr{};
	hdr.magic = 0xA1B2C3D4u;
	hdr.version_major = 2;
	hdr.version_minor = 4;
	hdr.thiszone = 0;
	hdr.sigfigs = 0;
	hdr.snaplen = 65535;
	hdr.linktype = 101; // DLT_RAW
	std::fwrite(&hdr, sizeof(hdr), 1, file_);
	std::fflush(file_);
	return true;
}

void PcapWriter::close() {
	if (file_) {
		std::fflush(file_);
		std::fclose(file_);
		file_ = nullptr;
	}
}

void PcapWriter::write_udp(const Endpoint &src, const Endpoint &dst,
                           const uint8_t *data, size_t len) {
	if (!file_ || len > 0xFFFFu - 28u) return;

	const auto now = std::chrono::system_clock::now();
	const auto usec = std::chrono::duration_cast<std::chrono::microseconds>(
			now.time_since_epoch()).count();

	Ipv4Header ip{};
	ip.ver_ihl = 0x45;
	ip.tos = 0;
	ip.total_len = hton16(static_cast<uint16_t>(20 + 8 + len));
	ip.id = 0;
	ip.flags_frag = hton16(0x4000u); // DF
	ip.ttl = 64;
	ip.protocol = 17; // UDP
	ip.checksum = 0;

	// Store IP fields in big-endian in the struct already. We build a
	// tiny big-endian scratch buffer for the checksum because our struct
	// has platform-layout bit/byte order.
	uint8_t ip_be[20]{};
	ip_be[0] = ip.ver_ihl;
	ip_be[1] = ip.tos;
	ip_be[2] = static_cast<uint8_t>(ip.total_len & 0xFFu);
	ip_be[3] = static_cast<uint8_t>(ip.total_len >> 8);
	ip_be[4] = static_cast<uint8_t>(ip.id & 0xFFu);
	ip_be[5] = static_cast<uint8_t>(ip.id >> 8);
	ip_be[6] = static_cast<uint8_t>(ip.flags_frag & 0xFFu);
	ip_be[7] = static_cast<uint8_t>(ip.flags_frag >> 8);
	ip_be[8] = ip.ttl;
	ip_be[9] = ip.protocol;
	ip_be[10] = 0;
	ip_be[11] = 0; // checksum placeholder
	const uint32_t src_be = pack_ipv4_be(src.ip);
	const uint32_t dst_be = pack_ipv4_be(dst.ip);
	ip_be[12] = static_cast<uint8_t>((src_be >> 24) & 0xFFu);
	ip_be[13] = static_cast<uint8_t>((src_be >> 16) & 0xFFu);
	ip_be[14] = static_cast<uint8_t>((src_be >> 8) & 0xFFu);
	ip_be[15] = static_cast<uint8_t>(src_be & 0xFFu);
	ip_be[16] = static_cast<uint8_t>((dst_be >> 24) & 0xFFu);
	ip_be[17] = static_cast<uint8_t>((dst_be >> 16) & 0xFFu);
	ip_be[18] = static_cast<uint8_t>((dst_be >> 8) & 0xFFu);
	ip_be[19] = static_cast<uint8_t>(dst_be & 0xFFu);

	const uint16_t cksum = ipv4_checksum(ip_be, sizeof(ip_be));
	ip_be[10] = static_cast<uint8_t>((cksum >> 8) & 0xFFu);
	ip_be[11] = static_cast<uint8_t>(cksum & 0xFFu);

	uint8_t udp_be[8]{};
	udp_be[0] = static_cast<uint8_t>((src.port >> 8) & 0xFFu);
	udp_be[1] = static_cast<uint8_t>(src.port & 0xFFu);
	udp_be[2] = static_cast<uint8_t>((dst.port >> 8) & 0xFFu);
	udp_be[3] = static_cast<uint8_t>(dst.port & 0xFFu);
	const uint16_t udp_len = static_cast<uint16_t>(8 + len);
	udp_be[4] = static_cast<uint8_t>((udp_len >> 8) & 0xFFu);
	udp_be[5] = static_cast<uint8_t>(udp_len & 0xFFu);
	udp_be[6] = 0;
	udp_be[7] = 0; // checksum = 0 (legal for IPv4/UDP)

	PcapRecordHeader rec{};
	rec.ts_sec = static_cast<uint32_t>(usec / 1000000);
	rec.ts_usec = static_cast<uint32_t>(usec % 1000000);
	rec.incl_len = static_cast<uint32_t>(20 + 8 + len);
	rec.orig_len = rec.incl_len;

	std::fwrite(&rec, sizeof(rec), 1, file_);
	std::fwrite(ip_be, sizeof(ip_be), 1, file_);
	std::fwrite(udp_be, sizeof(udp_be), 1, file_);
	if (len) std::fwrite(data, 1, len, file_);
	std::fflush(file_);
}

} // namespace opennova::net
