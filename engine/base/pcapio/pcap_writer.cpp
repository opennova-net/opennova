#include <base/pcapio/pcap_writer.h>

#include <base/io/le.h>

#include <chrono>
#include <cstdlib>

namespace opennova::net {
namespace {

constexpr uint32_t PCAP_MAGIC_LE = 0xa1b2c3d4u;
constexpr uint32_t LINKTYPE_RAW = 101; // raw IP — each record is a bare IPv4 frame

void put32(std::vector<uint8_t> &out, uint32_t v) {
	io::append_u32_le(out, v);
}

void put16(std::vector<uint8_t> &out, uint16_t v) {
	io::append_u16_le(out, v);
}

void put16_be(std::vector<uint8_t> &out, uint16_t v) {
	out.push_back(uint8_t(v >> 8));
	out.push_back(uint8_t(v));
}

// Standard one's-complement header checksum over the 20-byte IPv4 header.
uint16_t ipv4_header_checksum(const uint8_t *hdr, size_t len) {
	uint32_t sum = 0;
	for (size_t i = 0; i + 1 < len; i += 2) {
		sum += (uint32_t(hdr[i]) << 8) | hdr[i + 1];
	}
	while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
	return uint16_t(~sum);
}

} // namespace

void append_pcap_global_header(std::vector<uint8_t> &out) {
	put32(out, PCAP_MAGIC_LE);
	put16(out, 2); // version_major
	put16(out, 4); // version_minor
	put32(out, 0); // thiszone
	put32(out, 0); // sigfigs
	put32(out, 65535); // snaplen
	put32(out, LINKTYPE_RAW);
}

void append_pcap_udp_record(std::vector<uint8_t> &out, uint32_t src_ip,
		uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
		const uint8_t *payload, size_t len, uint64_t ts_nanos) {
	// 20 B IPv4 + 8 B UDP must still fit the 16-bit total-length field.
	if (len > 0xFFFFu - 28u) return;
	const uint16_t total_len = uint16_t(20 + 8 + len);

	uint8_t ip[20]{};
	ip[0] = 0x45; // ver 4, IHL 5
	ip[1] = 0;    // tos
	ip[2] = uint8_t(total_len >> 8);
	ip[3] = uint8_t(total_len);
	ip[4] = 0; ip[5] = 0;     // id
	ip[6] = 0x40; ip[7] = 0;  // flags = DF
	ip[8] = 64;               // ttl
	ip[9] = 17;               // UDP
	ip[10] = 0; ip[11] = 0;   // checksum placeholder
	ip[12] = uint8_t(src_ip >> 24); ip[13] = uint8_t(src_ip >> 16);
	ip[14] = uint8_t(src_ip >> 8);  ip[15] = uint8_t(src_ip);
	ip[16] = uint8_t(dst_ip >> 24); ip[17] = uint8_t(dst_ip >> 16);
	ip[18] = uint8_t(dst_ip >> 8);  ip[19] = uint8_t(dst_ip);
	const uint16_t cksum = ipv4_header_checksum(ip, 20);
	ip[10] = uint8_t(cksum >> 8);
	ip[11] = uint8_t(cksum);

	const uint32_t incl = uint32_t(total_len);
	put32(out, uint32_t(ts_nanos / 1000000000ull));         // ts_sec
	put32(out, uint32_t((ts_nanos % 1000000000ull) / 1000)); // ts_usec
	put32(out, incl); // incl_len
	put32(out, incl); // orig_len
	out.insert(out.end(), ip, ip + 20);
	put16_be(out, src_port);
	put16_be(out, dst_port);
	put16_be(out, uint16_t(8 + len)); // udp length
	put16_be(out, 0);                 // udp checksum (0 = legal over IPv4)
	out.insert(out.end(), payload, payload + len);
}

std::vector<uint8_t> build_pcap_udp(const std::vector<PcapDatagram> &dgrams) {
	std::vector<uint8_t> buf;
	append_pcap_global_header(buf);
	constexpr uint32_t kLoopback = 0x7F000001u; // 127.0.0.1, both ends
	for (const auto &d : dgrams) {
		append_pcap_udp_record(buf, kLoopback, uint16_t(d.srcport), kLoopback,
				uint16_t(d.dstport), d.payload.data(), d.payload.size(),
				d.ts_nanos);
	}
	return buf;
}

PcapUdpWriter::~PcapUdpWriter() { close(); }

bool PcapUdpWriter::open(const std::string &path) {
	close();
	file_ = std::fopen(path.c_str(), "wb");
	if (file_ == nullptr) return false;
	scratch_.clear();
	append_pcap_global_header(scratch_);
	if (std::fwrite(scratch_.data(), 1, scratch_.size(), file_) != scratch_.size()) {
		close();
		return false;
	}
	records_ = 0;
	return true;
}

void PcapUdpWriter::close() {
	if (file_ == nullptr) return;
	std::fclose(file_);
	file_ = nullptr;
}

void PcapUdpWriter::write(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
		uint16_t dst_port, const uint8_t *payload, size_t len,
		uint64_t ts_nanos) {
	if (file_ == nullptr || (payload == nullptr && len != 0)) return;
	scratch_.clear();
	append_pcap_udp_record(scratch_, src_ip, src_port, dst_ip, dst_port,
			payload, len, ts_nanos);
	if (scratch_.empty()) return; // oversize; dropped whole, never truncated
	if (std::fwrite(scratch_.data(), 1, scratch_.size(), file_) != scratch_.size()) {
		// A capture that cannot be written is worthless but must not take the
		// session with it: stop recording and leave what landed readable.
		close();
		return;
	}
	// Flushed per record so a capture survives a crash mid-session — the case
	// a capture is most wanted for.
	std::fflush(file_);
	++records_;
}

std::unique_ptr<PcapUdpWriter> PcapUdpWriter::from_path(const std::string &path) {
	if (path.empty()) return nullptr;
	auto writer = std::make_unique<PcapUdpWriter>();
	if (!writer->open(path)) return nullptr;
	return writer;
}

uint64_t pcap_now_nanos() {
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	return uint64_t(
			std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace opennova::net
