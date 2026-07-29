// Streaming pcap-reader regression — stream_pcap_udp_file must yield exactly the
// same datagrams (order, ports, timestamps, payloads) as the whole-file
// read_pcap_udp, and must honor early-exit (return false from the callback).
// Crafts an inline pcap (build_pcap_udp), writes it to a temp file, and compares.
// No fixtures, runs in CI.

#include <pcapio/pcap_reader.h>

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
