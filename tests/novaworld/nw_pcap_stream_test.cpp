// Streaming pcap-reader regression — stream_pcap_udp_file must yield exactly the
// same datagrams (order, ports, timestamps, payloads) as the whole-file
// read_pcap_udp, and must honor early-exit (return false from the callback).
// Crafts an inline pcap (build_pcap_udp), writes it to a temp file, and compares.
// No fixtures, runs in CI.

#include <base/io/le.h>
#include <base/pcapio/pcap_reader.h>
#include <base/pcapio/pcap_writer.h> // build_pcap_udp

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova::net;

namespace {

int g_failures = 0;
#define EXPECT(cond)                                                            \
	do {                                                                        \
		if (!(cond)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
			g_failures++;                                                       \
		}                                                                       \
	} while (0)

bool same(const PcapDatagram &a, const PcapDatagram &b) {
	return a.srcport == b.srcport && a.dstport == b.dstport &&
	       a.ts_nanos == b.ts_nanos && a.payload == b.payload;
}

// A raw IPv4 UDP packet (linktype 101): its 20-byte IP header, its 8-byte UDP header and
// `payload`, the IP and UDP lengths exact, so a reader never needs the bytes past it.
std::vector<uint8_t> raw_udp(const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> p = {0x45, 0, 0, 0, 0, 0, 0, 0, 64, 17, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
	const size_t total = 20 + 8 + payload.size();
	p[2] = uint8_t(total >> 8);
	p[3] = uint8_t(total);
	const size_t udp = 8 + payload.size();
	for (const uint8_t b : {uint8_t(0x03), uint8_t(0xE8), uint8_t(0x07), uint8_t(0xD0), uint8_t(udp >> 8),
		     uint8_t(udp), uint8_t(0), uint8_t(0)})
		p.push_back(b);
	p.insert(p.end(), payload.begin(), payload.end());
	return p;
}

// One pcapng block: its type, its length, `body`, its length again.
void block(std::vector<uint8_t> &out, uint32_t type, const std::vector<uint8_t> &body) {
	const uint32_t total = uint32_t(12 + body.size());
	opennova::io::append_u32_le(out, type);
	opennova::io::append_u32_le(out, total);
	out.insert(out.end(), body.begin(), body.end());
	opennova::io::append_u32_le(out, total);
}

// A little-endian pcapng holding one raw-IPv4 interface and, after it, an EPB whose cap_len is
// `epb_cap_len` and an SPB whose pkt_len is `spb_pkt_len`, each over the same 32-byte packet.
std::vector<uint8_t> pcapng_with_lengths(uint32_t epb_cap_len, uint32_t spb_pkt_len) {
	const std::vector<uint8_t> packet = raw_udp({'A', 'B', 'C', 'D'});
	std::vector<uint8_t> out, body;
	opennova::io::append_u32_le(body, 0x1A2B3C4Du); // byte-order magic
	opennova::io::append_u16_le(body, 1);           // version 1.0
	opennova::io::append_u16_le(body, 0);
	opennova::io::append_u32_le(body, 0xFFFFFFFFu); // section length: unknown
	opennova::io::append_u32_le(body, 0xFFFFFFFFu);
	block(out, 0x0A0D0D0Au, body);
	body.clear();
	opennova::io::append_u16_le(body, 101); // LINKTYPE_RAW
	opennova::io::append_u16_le(body, 0);
	opennova::io::append_u32_le(body, 65535);
	block(out, 0x00000001u, body);
	body.clear();
	for (const uint32_t w : {0u, 0u, 0u, epb_cap_len, uint32_t(packet.size())})
		opennova::io::append_u32_le(body, w);
	body.insert(body.end(), packet.begin(), packet.end());
	block(out, 0x00000006u, body);
	body.clear();
	opennova::io::append_u32_le(body, spb_pkt_len);
	body.insert(body.end(), packet.begin(), packet.end());
	block(out, 0x00000003u, body);
	return out;
}

} // namespace

int main() {
	// Craft a spread of datagrams: varied ports, microsecond-aligned timestamps
	// (build_pcap_udp writes µs resolution), and varied payload sizes.
	std::vector<PcapDatagram> dgrams;
	for (int i = 0; i < 250; ++i) {
		PcapDatagram d;
		d.srcport = 32768 + (i % 3);
		d.dstport = 40000 + (i % 5);
		d.ts_nanos = uint64_t(i) * 1000000ull; // 1 ms steps (µs-exact)
		d.payload.assign(size_t(10 + (i % 200)), uint8_t(i & 0xFF));
		dgrams.push_back(std::move(d));
	}

	const std::vector<uint8_t> buf = build_pcap_udp(dgrams);
	EXPECT(!buf.empty());

	// An IPv4 total length shorter than its own header plus UDP header is
	// malformed. It must not underflow the captured-payload calculation and
	// surface a datagram.
	std::vector<uint8_t> malformed = build_pcap_udp({dgrams.front()});
	constexpr size_t kFirstIpOffset = 24 + 16;
	EXPECT(malformed.size() >= kFirstIpOffset + 20);
	malformed[kFirstIpOffset + 2] = 0;
	malformed[kFirstIpOffset + 3] = 20;
	std::vector<PcapDatagram> malformed_batch;
	EXPECT(read_pcap_udp(malformed.data(), malformed.size(), malformed_batch));
	EXPECT(malformed_batch.empty());

	// A pcapng EPB or SPB claiming more bytes than its block holds is skipped, a length near
	// 2^32 included, which a 32-bit `20 + cap_len` (or `4 + pkt_len`) wraps past the check:
	// over the true lengths both blocks read; with cap_len 0xFFFFFFF0 and pkt_len 0xFFFFFFFD
	// neither does, in memory or streamed.
	const std::vector<uint8_t> honest = pcapng_with_lengths(32, 32);
	std::vector<PcapDatagram> honest_batch;
	EXPECT(read_pcap_udp(honest.data(), honest.size(), honest_batch));
	EXPECT(honest_batch.size() == 2);
	if (honest_batch.size() == 2)
		EXPECT(honest_batch[0].payload == std::vector<uint8_t>({'A', 'B', 'C', 'D'}) &&
		       honest_batch[1].srcport == 1000 && honest_batch[1].dstport == 2000);
	const std::vector<uint8_t> wrapping = pcapng_with_lengths(0xFFFFFFF0u, 0xFFFFFFFDu);
	std::vector<PcapDatagram> wrapping_batch;
	EXPECT(read_pcap_udp(wrapping.data(), wrapping.size(), wrapping_batch));
	EXPECT(wrapping_batch.empty());
	const std::string wrapping_tmp = "nw_pcap_stream_test.wrap.pcapng";
	{
		std::ofstream f(wrapping_tmp, std::ios::binary);
		f.write(reinterpret_cast<const char *>(wrapping.data()), std::streamsize(wrapping.size()));
	}
	int wrapping_streamed = 0;
	EXPECT(stream_pcap_udp_file(wrapping_tmp, [&](const PcapDatagram &) {
		++wrapping_streamed;
		return true;
	}));
	EXPECT(wrapping_streamed == 0);
	std::remove(wrapping_tmp.c_str());

	const std::string tmp = "nw_pcap_stream_test.tmp.pcap";
	{
		std::ofstream f(tmp, std::ios::binary);
		EXPECT(bool(f));
		f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
	}

	// Batch (whole-file in memory) vs streaming — must be identical.
	std::vector<PcapDatagram> batch;
	EXPECT(read_pcap_udp(buf.data(), buf.size(), batch));
	std::vector<PcapDatagram> streamed;
	EXPECT(stream_pcap_udp_file(tmp, [&](const PcapDatagram &d) {
		streamed.push_back(d);
		return true;
	}));

	EXPECT(batch.size() == dgrams.size());
	EXPECT(streamed.size() == batch.size());
	if (streamed.size() == batch.size()) {
		bool all = true;
		for (size_t i = 0; i < batch.size(); ++i)
			if (!same(streamed[i], batch[i])) { all = false; break; }
		EXPECT(all);
	}

	// Early-exit: stop after 10 datagrams.
	int seen = 0;
	stream_pcap_udp_file(tmp, [&](const PcapDatagram &) {
		++seen;
		return seen < 10; // false on the 10th -> stop
	});
	EXPECT(seen == 10);

	std::remove(tmp.c_str());

	if (g_failures) {
		std::printf("\n%d assertion(s) failed\n", g_failures);
		return 1;
	}
	std::printf("PASS: stream_pcap_udp_file matches read_pcap_udp byte-for-byte "
	            "(%zu datagrams) and honors early-exit.\n", batch.size());
	return 0;
}
