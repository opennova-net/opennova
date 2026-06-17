#include "pcap_reader.h"

#include <fstream>

namespace opennova::net {

namespace {

constexpr uint32_t PCAP_MAGIC_LE = 0xa1b2c3d4u;
constexpr uint32_t PCAP_MAGIC_BE = 0xd4c3b2a1u;
constexpr uint32_t PCAP_MAGIC_NSEC_LE = 0xa1b23c4du; // nanosecond ts
constexpr uint32_t PCAP_MAGIC_NSEC_BE = 0x4d3cb2a1u;

constexpr uint32_t PCAPNG_BLOCK_SHB = 0x0a0d0d0au;
constexpr uint32_t PCAPNG_BLOCK_IDB = 0x00000001u;
constexpr uint32_t PCAPNG_BLOCK_EPB = 0x00000006u;
constexpr uint32_t PCAPNG_BLOCK_SPB = 0x00000003u;

constexpr uint32_t LINKTYPE_NULL = 0;      // BSD loopback: 4-byte AF_xxx (host order)
constexpr uint32_t LINKTYPE_ETHERNET = 1;  // 14-byte Ethernet II header
constexpr uint32_t LINKTYPE_RAW = 101;     // raw IP
constexpr uint32_t LINKTYPE_LOOP = 108;    // OpenBSD loopback: 4-byte AF_xxx (BE)
constexpr uint32_t LINKTYPE_IPV4 = 228;    // raw IPv4

constexpr uint16_t AF_INET = 2;

uint16_t read_u16_le(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t read_u32_le(const uint8_t *p) {
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
	       uint32_t(p[3]) << 24;
}
uint32_t read_u32_be(const uint8_t *p) {
	return uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 |
	       uint32_t(p[0]) << 24;
}
uint16_t read_u16_be(const uint8_t *p) { return uint16_t(p[1]) | uint16_t(p[0]) << 8; }

// Peel the data-link header. Returns the offset into `frame` where the IPv4
// header starts, or -1 if not IPv4 / unsupported linktype.
int strip_link_header(uint32_t linktype, const uint8_t *frame, size_t len) {
	switch (linktype) {
	case LINKTYPE_NULL:
	case LINKTYPE_LOOP: {
		if (len < 4) return -1;
		// NULL: host byte order; LOOP: big-endian. The AF value is small
		// (2 = AF_INET) so we detect by checking both orders.
		uint32_t af_le = read_u32_le(frame);
		uint32_t af_be = read_u32_be(frame);
		uint32_t af = (af_le < 256) ? af_le : af_be;
		if (af != AF_INET) return -1;
		return 4;
	}
	case LINKTYPE_ETHERNET: {
		if (len < 14) return -1;
		if (read_u16_be(frame + 12) != 0x0800) return -1; // not IPv4
		return 14;
	}
	case LINKTYPE_RAW:
	case LINKTYPE_IPV4:
		return 0;
	default:
		return -1;
	}
}

void extract_ipv4_udp(const uint8_t *pkt, size_t len, uint32_t linktype,
                      int frame_index, std::vector<PcapDatagram> &out,
                      int &fragments_dropped) {
	const int ip_off = strip_link_header(linktype, pkt, len);
	if (ip_off < 0 || size_t(ip_off) + 20 > len) return;
	const uint8_t *ip = pkt + ip_off;
	if ((ip[0] >> 4) != 4) return; // IPv4 only
	const size_t ihl = size_t(ip[0] & 0x0F) * 4;
	if (ihl < 20 || size_t(ip_off) + ihl > len) return;
	if (ip[9] != 17) return; // not UDP
	// Drop IP fragments (fragment offset != 0 or MF flag set).
	const uint16_t flags_frag = read_u16_be(ip + 6);
	if ((flags_frag & 0x1FFF) != 0 || (flags_frag & 0x2000) != 0) {
		fragments_dropped++;
		return;
	}
	const uint16_t ip_total_len = read_u16_be(ip + 2);
	const size_t udp_off = size_t(ip_off) + ihl;
	if (udp_off + 8 > len) return;
	const uint8_t *udp = pkt + udp_off;
	const uint16_t srcport = read_u16_be(udp + 0);
	const uint16_t dstport = read_u16_be(udp + 2);
	const uint16_t udp_len = read_u16_be(udp + 4);
	if (udp_len < 8) return;
	// Trust UDP length over captured length when it's plausible (truncates
	// trailing pad bytes some link layers add).
	const size_t payload_off = udp_off + 8;
	size_t payload_len = udp_len - 8;
	const size_t cap_avail = (size_t(ip_off) + ip_total_len <= len)
	                             ? (size_t(ip_off) + ip_total_len - payload_off)
	                             : (len - payload_off);
	if (payload_len > cap_avail) payload_len = cap_avail;
	if (payload_off + payload_len > len) return;
	PcapDatagram d;
	d.srcport = int(srcport);
	d.dstport = int(dstport);
	d.frame_index = frame_index;
	d.payload.assign(pkt + payload_off, pkt + payload_off + payload_len);
	out.push_back(std::move(d));
}

uint16_t ipv4_checksum(const uint8_t *hdr, size_t len) {
	uint32_t sum = 0;
	for (size_t i = 0; i + 1 < len; i += 2)
		sum += (uint32_t(hdr[i]) << 8) | hdr[i + 1];
	if (len & 1) sum += uint32_t(hdr[len - 1]) << 8;
	while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
	return uint16_t(~sum & 0xFFFFu);
}

} // namespace

bool read_pcap_udp(const uint8_t *data, size_t len, std::vector<PcapDatagram> &out,
                   int *frags_dropped) {
	if (!data || len < 24) return false;
	int fragments_dropped = 0;
	int frame_index = 0;

	const uint32_t magic = read_u32_le(data);
	if (magic == PCAP_MAGIC_LE || magic == PCAP_MAGIC_BE ||
	    magic == PCAP_MAGIC_NSEC_LE || magic == PCAP_MAGIC_NSEC_BE) {
		const bool be = (magic == PCAP_MAGIC_BE || magic == PCAP_MAGIC_NSEC_BE);
		auto r32 = [&](const uint8_t *p) { return be ? read_u32_be(p) : read_u32_le(p); };
		const uint32_t linktype = r32(data + 20);
		size_t off = 24;
		while (off + 16 <= len) {
			const uint32_t incl_len = r32(data + off + 8);
			if (off + 16 + incl_len > len) break;
			frame_index++;
			extract_ipv4_udp(data + off + 16, incl_len, linktype, frame_index, out,
			                 fragments_dropped);
			off += 16 + incl_len;
		}
	} else if (magic == PCAPNG_BLOCK_SHB) {
		if (len < 28) return false;
		const uint32_t bom = read_u32_le(data + 8);
		if (bom != 0x1A2B3C4Du) return false; // big-endian pcapng unsupported
		uint32_t cur_linktype = LINKTYPE_ETHERNET;
		size_t off = 0;
		while (off + 8 <= len) {
			const uint32_t btype = read_u32_le(data + off);
			const uint32_t blen = read_u32_le(data + off + 4);
			if (blen < 12 || off + blen > len) break;
			const uint8_t *body = data + off + 8;
			const size_t body_len = blen - 12; // minus type + len*2
			if (btype == PCAPNG_BLOCK_IDB && body_len >= 8) {
				cur_linktype = read_u32_le(body) & 0xFFFF;
			} else if (btype == PCAPNG_BLOCK_EPB && body_len >= 20) {
				// EPB: interface_id(4) ts_high(4) ts_low(4) cap_len(4) pkt_len(4) data...
				const uint32_t cap_len = read_u32_le(body + 12);
				if (20 + cap_len <= body_len) {
					frame_index++;
					extract_ipv4_udp(body + 20, cap_len, cur_linktype, frame_index, out,
					                 fragments_dropped);
				}
			} else if (btype == PCAPNG_BLOCK_SPB && body_len >= 4) {
				const uint32_t pkt_len = read_u32_le(body);
				if (4 + pkt_len <= body_len) {
					frame_index++;
					extract_ipv4_udp(body + 4, pkt_len, cur_linktype, frame_index, out,
					                 fragments_dropped);
				}
			}
			off += blen;
		}
	} else {
		return false;
	}

	if (frags_dropped) *frags_dropped = fragments_dropped;
	return true;
}

bool read_pcap_udp_file(const std::string &path, std::vector<PcapDatagram> &out,
                        int *frags_dropped) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamsize n = f.tellg();
	if (n < 0) return false;
	std::vector<uint8_t> buf(static_cast<size_t>(n));
	f.seekg(0);
	f.read(reinterpret_cast<char *>(buf.data()), n);
	if (!f.good() && !f.eof()) return false;
	return read_pcap_udp(buf.data(), buf.size(), out, frags_dropped);
}

std::vector<uint8_t> build_pcap_udp(const std::vector<PcapDatagram> &dgrams) {
	std::vector<uint8_t> buf;
	auto put32 = [&](uint32_t v) {
		buf.push_back(uint8_t(v));
		buf.push_back(uint8_t(v >> 8));
		buf.push_back(uint8_t(v >> 16));
		buf.push_back(uint8_t(v >> 24));
	};
	auto put16 = [&](uint16_t v) {
		buf.push_back(uint8_t(v));
		buf.push_back(uint8_t(v >> 8));
	};
	auto put16_be = [&](uint16_t v) {
		buf.push_back(uint8_t(v >> 8));
		buf.push_back(uint8_t(v));
	};

	// Global header (little-endian), DLT_RAW so each record is a bare IPv4 frame.
	put32(PCAP_MAGIC_LE);
	put16(2); // version_major
	put16(4); // version_minor
	put32(0); // thiszone
	put32(0); // sigfigs
	put32(65535); // snaplen
	put32(LINKTYPE_RAW);

	for (const auto &d : dgrams) {
		const size_t plen = d.payload.size();
		if (plen > 0xFFFFu - 28u) continue;
		const uint16_t total_len = uint16_t(20 + 8 + plen);

		uint8_t ip[20]{};
		ip[0] = 0x45; // ver 4, IHL 5
		ip[1] = 0;    // tos
		ip[2] = uint8_t(total_len >> 8);
		ip[3] = uint8_t(total_len);
		ip[4] = 0; ip[5] = 0; // id
		ip[6] = 0x40; ip[7] = 0; // flags = DF
		ip[8] = 64;   // ttl
		ip[9] = 17;   // UDP
		ip[10] = 0; ip[11] = 0; // checksum placeholder
		ip[12] = 127; ip[13] = 0; ip[14] = 0; ip[15] = 1; // src 127.0.0.1
		ip[16] = 127; ip[17] = 0; ip[18] = 0; ip[19] = 1; // dst 127.0.0.1
		const uint16_t cksum = ipv4_checksum(ip, 20);
		ip[10] = uint8_t(cksum >> 8);
		ip[11] = uint8_t(cksum);

		const uint32_t incl = uint32_t(total_len);
		put32(0);    // ts_sec (deterministic)
		put32(0);    // ts_usec
		put32(incl); // incl_len
		put32(incl); // orig_len
		buf.insert(buf.end(), ip, ip + 20);
		put16_be(uint16_t(d.srcport));
		put16_be(uint16_t(d.dstport));
		put16_be(uint16_t(8 + plen)); // udp length
		put16_be(0);                  // udp checksum (0 = legal over IPv4)
		buf.insert(buf.end(), d.payload.begin(), d.payload.end());
	}
	return buf;
}

} // namespace opennova::net
