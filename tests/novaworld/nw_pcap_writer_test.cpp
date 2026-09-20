// PcapUdpWriter pins: a capture written as datagrams arrive must be readable by
// the SAME reader that consumes retail captures, and must frame each record
// identically to the in-memory build_pcap_udp() the decode tests craft with.
//
// That second property is the one that matters for the whole point of writing
// captures at all — a self-recorded session and a retail-recorded one are only
// comparable if both decode through one pipeline. If the streaming and
// in-memory writers ever drift, this fails.

#include <base/pcapio/pcap_reader.h>
#include <base/pcapio/pcap_writer.h>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s\n", msg);                           \
			++failures;                                                        \
		}                                                                      \
	} while (0)

std::string temp_path(const char *stem) {
	return std::string(test_paths_temp_dir()) + "/" + stem;
}

// Three datagrams both directions, varied sizes including an empty-ish one.
std::vector<net::PcapDatagram> sample_datagrams() {
	std::vector<net::PcapDatagram> d(3);
	d[0].srcport = 40000; d[0].dstport = 17479;
	d[0].payload = {0x01, 0x02, 0x03, 0x04};
	d[0].ts_nanos = 1000000000ull;
	d[1].srcport = 17479; d[1].dstport = 40000;
	d[1].payload.assign(600, 0xAB);
	d[1].ts_nanos = 1000500000ull;
	d[2].srcport = 40000; d[2].dstport = 17479;
	d[2].payload = {0xFF};
	d[2].ts_nanos = 2000000000ull;
	return d;
}

void test_roundtrip_through_the_reader() {
	const std::string path = temp_path("nw_pcap_writer_roundtrip.pcap");
	const std::vector<net::PcapDatagram> want = sample_datagrams();

	{
		net::PcapUdpWriter w;
		CHECK(w.open(path), "the writer opens its file");
		constexpr uint32_t kLoopback = 0x7F000001u;
		for (const auto &d : want) {
			w.write(kLoopback, uint16_t(d.srcport), kLoopback,
					uint16_t(d.dstport), d.payload.data(), d.payload.size(),
					d.ts_nanos);
		}
		CHECK(w.records() == want.size(), "every datagram was accepted");
	} // closed by the destructor

	std::vector<net::PcapDatagram> got;
	CHECK(net::read_pcap_udp_file(path, got), "the repo's reader accepts it");
	CHECK(got.size() == want.size(), "every record survives the roundtrip");
	if (got.size() == want.size()) {
		for (size_t i = 0; i < got.size(); ++i) {
			CHECK(got[i].srcport == want[i].srcport, "src port survives");
			CHECK(got[i].dstport == want[i].dstport, "dst port survives");
			CHECK(got[i].payload == want[i].payload, "payload bytes survive");
			CHECK(got[i].frame_index == int(i + 1), "capture order survives");
		}
	}
	std::remove(path.c_str());
}

// The streaming writer and the in-memory builder must produce the SAME bytes for
// the same input — one framing implementation, proven rather than asserted in a
// comment.
void test_streaming_matches_in_memory_builder() {
	const std::string path = temp_path("nw_pcap_writer_identity.pcap");
	const std::vector<net::PcapDatagram> dgrams = sample_datagrams();

	{
		net::PcapUdpWriter w;
		CHECK(w.open(path), "the writer opens its file");
		constexpr uint32_t kLoopback = 0x7F000001u;
		for (const auto &d : dgrams) {
			w.write(kLoopback, uint16_t(d.srcport), kLoopback,
					uint16_t(d.dstport), d.payload.data(), d.payload.size(),
					d.ts_nanos);
		}
	}

	const std::vector<uint8_t> streamed = test_io::read_file(path);
	const std::vector<uint8_t> in_memory = net::build_pcap_udp(dgrams);
	CHECK(!streamed.empty(), "the streamed capture is non-empty");
	CHECK(streamed == in_memory,
			"streaming and in-memory framing are byte-identical");
	std::remove(path.c_str());
}

// A payload too large for one unfragmented IPv4 datagram is dropped WHOLE. A
// truncated record would desync every following record for a reader.
void test_oversize_is_dropped_not_truncated() {
	const std::string path = temp_path("nw_pcap_writer_oversize.pcap");
	const std::vector<uint8_t> huge(70000, 0x5A);
	const std::vector<uint8_t> ok = {0xDE, 0xAD};

	{
		net::PcapUdpWriter w;
		CHECK(w.open(path), "the writer opens its file");
		w.write(0x7F000001u, 1, 0x7F000001u, 2, huge.data(), huge.size(), 0);
		CHECK(w.records() == 0, "an oversize datagram is not counted");
		w.write(0x7F000001u, 1, 0x7F000001u, 2, ok.data(), ok.size(), 0);
		CHECK(w.records() == 1, "the following datagram still records");
	}

	std::vector<net::PcapDatagram> got;
	CHECK(net::read_pcap_udp_file(path, got), "the capture stays readable");
	CHECK(got.size() == 1, "only the well-sized datagram is present");
	if (got.size() == 1) CHECK(got[0].payload == ok, "and it is intact");
	std::remove(path.c_str());
}

// The path gate: no path, no file, no writer — the default every session runs.
void test_path_gate() {
	CHECK(net::PcapUdpWriter::from_path(std::string()) == nullptr,
			"an empty path yields no writer");
	CHECK(net::PcapUdpWriter::from_path(temp_path("no_such_dir_zz/x.pcap")) == nullptr,
			"an unopenable path yields no writer rather than a half-open one");
	const std::string path = temp_path("nw_pcap_writer_path_gate.pcap");
	std::remove(path.c_str());
	auto writer = net::PcapUdpWriter::from_path(path);
	CHECK(writer != nullptr, "a writable path yields an open writer");
	if (writer != nullptr) {
		CHECK(writer->is_open(), "the gated writer is open");
		CHECK(writer->records() == 0, "and has recorded nothing yet");
		writer->close();
	}
	std::remove(path.c_str());
}

// A writer that never opened must swallow writes rather than crash: a bad path
// should cost a capture, never a session.
void test_unopened_writer_is_inert() {
	net::PcapUdpWriter w;
	CHECK(!w.is_open(), "a fresh writer is closed");
	const uint8_t byte = 0x11;
	w.write(0x7F000001u, 1, 0x7F000001u, 2, &byte, 1, 0);
	CHECK(w.records() == 0, "writing while closed records nothing");
	w.close(); // idempotent
	CHECK(!w.is_open(), "closing twice is safe");
}

} // namespace

int main() {
	test_roundtrip_through_the_reader();
	test_streaming_matches_in_memory_builder();
	test_oversize_is_dropped_not_truncated();
	test_path_gate();
	test_unopened_writer_is_inert();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("nw_pcap_writer_test OK\n");
	return 0;
}
