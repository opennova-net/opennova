// opennova-wire --parity-events: the canonical parity trace (apps/wire/parity_trace.h).
//
// The line stream is a contract: scripts/net/wait_parity_wire_ready.ps1 parses it and
// opennova-parity compares the same records in memory. Over the committed self-capture
// session this pins
//   - the executable prints exactly what the library printer prints;
//   - every line matches the readiness script's own line patterns;
//   - the collected records and the printed lines agree in count;
//   - the bytes: the LF-normalized output's SHA-256 for the pinned fixture.
// A fixture regeneration (nw_self_capture_test --write-fixture) changes both hashes:
// re-pin kFixtureSha256 and kOutputSha256 in the same commit, after checking that the
// new output still matches the old line contract (the printed hashes are the new ones).
//
// Usage: wire_parity_events_test <path to opennova-wire>

#include "parity_trace.h"

#include <base/io/file_io.h>
#include <base/io/sha256.h>

#include "common/run_command.h"
#include "common/temp_dir.h"

#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#ifndef SELF_CAPTURE_FIXTURE
#define SELF_CAPTURE_FIXTURE ""
#endif

using namespace opennova;

namespace {

constexpr const char *kFixtureSha256 =
		"1fbc8ea46ef3039943b97031357b73b6872b9b8bba809bd26a642d7f58a4e2a0";
constexpr const char *kOutputSha256 =
		"34e08747528ad84e24232c6d206461d15f94563a04d5c814206557c52e259017";

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (cond)
		return;
	std::fprintf(stderr, "FAIL: %s\n", what);
	++g_failures;
}

std::string without_cr(const std::string &text) {
	std::string out;
	out.reserve(text.size());
	for (const char c : text)
		if (c != '\r')
			out.push_back(c);
	return out;
}

std::vector<std::string> lines_of(const std::string &text) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start < text.size()) {
		const size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			out.push_back(text.substr(start));
			break;
		}
		out.push_back(text.substr(start, end - start));
		start = end + 1;
	}
	return out;
}

// The patterns scripts/net/wait_parity_wire_ready.ps1 matches, verbatim.
const std::vector<std::regex> &contract_patterns() {
	static const std::vector<std::regex> patterns = {
		std::regex("^PARITY_DATAGRAM frame=(\\d+) ts_ns=(\\d+) src=(\\d+) dst=(\\d+) "
				   "len=(\\d+) outer=([01]) opcode=(0x[0-9a-fA-F]{2}|--) "
				   "class=(invalid|unknown|client_hello|client_auth|server_hello|"
				   "server_auth|client_protocol|server_protocol) decode=([01])$"),
		std::regex("^PARITY_HANDSHAKE frame=(\\d+) ts_ns=(\\d+) dir=([SC]) session=(\\d+) "
				   "opcode=0x(41|42|81|82) decode=([01]) len=(\\d+) body=([0-9a-fA-F]*)$"),
		std::regex("^PARITY_PACKET frame=(\\d+) ts_ns=(\\d+) dir=([SC]) session=(\\d+) "
				   "sid=0x([0-9a-fA-F]{8}) seq=(\\d+) ack=(\\d+) flags=0x([0-9a-fA-F]{2}) "
				   "records=(\\d+) tags=(\\S+) wire=(\\S+)$"),
		std::regex("^PARITY_EVENT frame=(\\d+) ts_ns=(\\d+) dir=([SC]) session=(\\d+) "
				   "participant=(\\d+) tag=0x([0-9a-fA-F]{2}) settings=([01]) len=(\\d+) "
				   "body=([0-9a-fA-F]*|-)$"),
		std::regex("^PARITY_STATE frame=(\\d+) ts_ns=(\\d+) dir=C session=(\\d+) tag=0x0c "
				   "kind=uplink decode=([01]) len=(\\d+) handle=(\\d+) type=(\\d+) "
				   "sub=(\\d+) carrier=(\\d+) pos=(-?\\d+),(-?\\d+),(-?\\d+) "
				   "orient=(-?\\d+),(-?\\d+) move=(\\d+) state=(\\d+) "
				   "analog=(\\d+),(\\d+),(\\d+) adm=(\\d+) priority_nonzero=(\\d+) "
				   "priority=(\\d+):(\\d+),(\\d+):(\\d+),(\\d+):(\\d+),(\\d+):(\\d+)$"),
		std::regex("^PARITY_STATE frame=(\\d+) ts_ns=(\\d+) dir=S session=(\\d+) tag=0x0a "
				   "kind=frame decode=([01]) len=(\\d+) sub=(\\d+) flags=(\\d+),(\\d+) "
				   "anchor=(-?\\d+),(-?\\d+),(-?\\d+) local=([01]) "
				   "local_state=(\\d+),(-?\\d+),(-?\\d+) records=(\\d+) players=(\\d+) "
				   "vehicles=(\\d+) infantry=(\\d+) none=(\\d+) rounds=(\\d+) "
				   "carried=(\\d+) "
				   "weapon=([01]),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(-?\\d+) "
				   "timer=([01]),(\\d+),(\\d+),(\\d+),(\\d+),(-?\\d+) "
				   "env=([01]),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+),(\\d+) "
				   "objective=([01]),(-?\\d+),(-?\\d+),(-?\\d+),(-?\\d+) "
				   "mount=([01]),(\\d+),([01]),(\\d+),(\\d+)$"),
		std::regex("^PARITY_ENTITY frame=(\\d+) ts_ns=(\\d+) session=(\\d+) class=([PVIN]) "
				   "handle=(\\d+) type=(\\d+) pos=(\\d+),(\\d+),(\\d+) "
				   "orient=(-?\\d+),(-?\\d+) state=(\\d+),(\\d+) vital=(\\d+)$"),
	};
	return patterns;
}

size_t count_prefix(const std::vector<std::string> &lines, const char *prefix) {
	size_t n = 0;
	for (const std::string &line : lines)
		if (line.rfind(prefix, 0) == 0)
			++n;
	return n;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: wire_parity_events_test <opennova-wire>\n");
		return 2;
	}
	const std::string wire_exe = argv[1];
	const std::string fixture = SELF_CAPTURE_FIXTURE;
	std::vector<uint8_t> fixture_bytes;
	std::string error;
	if (!io::read_file_bytes(fixture, fixture_bytes, error) || fixture_bytes.size() < 1024) {
		std::fprintf(stderr,
				"FAIL: the self-capture fixture is missing or an LFS pointer (%s); run "
				"git lfs pull --include=\"fixtures/novaworld/**\"\n",
				fixture.c_str());
		return 1;
	}

	test_temp::TempDir dir("wire_parity_events");

	// The library: printer to a binary-mode file (LF line ends) and collector.
	const std::string lib_path = dir.file("lib.txt");
	std::FILE *lib_out = std::fopen(lib_path.c_str(), "wb");
	expect(lib_out != nullptr, "open the library output file");
	if (!lib_out)
		return 1;
	{
		wire::ParityTracePrinter printer(lib_out);
		wire::ParityTraceBuilder builder(printer);
		const bool read = net::stream_pcap_udp_file(fixture, [&](const net::PcapDatagram &d) {
			builder.push(d);
			return true;
		});
		expect(read, "the fixture streams as a pcap");
	}
	std::fclose(lib_out);
	wire::ParityTrace trace;
	expect(wire::read_parity_trace(fixture, nullptr, trace), "read_parity_trace reads the fixture");

	std::string lib_text;
	expect(io::read_file_text(lib_path, lib_text, error), "read back the library output");

	// The executable prints the same bytes (its stdout is text mode on Windows).
	const std::string exe_path = dir.file("exe.txt");
	const std::string cmd = test_cmd::quoted(wire_exe) + " " + test_cmd::quoted(fixture) +
			" --parity-events > " + test_cmd::quoted(exe_path);
	expect(test_cmd::run(cmd) == 0, "opennova-wire --parity-events exits 0");
	std::string exe_text;
	expect(io::read_file_text(exe_path, exe_text, error), "read back the executable output");
	exe_text = without_cr(exe_text);
	expect(exe_text == lib_text,
			"opennova-wire --parity-events prints the library printer's bytes");

	// Every line is one of the contract's lines.
	const std::vector<std::string> lines = lines_of(lib_text);
	size_t unmatched = 0;
	for (const std::string &line : lines) {
		bool matched = false;
		for (const std::regex &pattern : contract_patterns())
			if (std::regex_match(line, pattern)) {
				matched = true;
				break;
			}
		if (!matched && ++unmatched <= 3)
			std::fprintf(stderr, "unmatched contract line: %.160s\n", line.c_str());
	}
	expect(unmatched == 0, "every --parity-events line matches the readiness script's patterns");

	// The collector holds exactly the printed records.
	expect(count_prefix(lines, "PARITY_HANDSHAKE ") == trace.handshakes.size(),
			"handshake lines == collected handshakes");
	expect(count_prefix(lines, "PARITY_DATAGRAM ") == trace.datagrams.size(),
			"datagram lines == collected datagrams");
	expect(count_prefix(lines, "PARITY_PACKET ") == trace.packets.size(),
			"packet lines == collected packets");
	expect(count_prefix(lines, "PARITY_EVENT ") == trace.events.size(),
			"event lines == collected events");
	expect(count_prefix(lines, "PARITY_STATE ") == trace.uplinks.size() + trace.frames.size(),
			"state lines == collected uplinks + frames");
	expect(count_prefix(lines, "PARITY_ENTITY ") == trace.entities.size(),
			"entity lines == collected entities");
	expect(!trace.handshakes.empty() && !trace.uplinks.empty() && !trace.frames.empty() &&
					!trace.entities.empty(),
			"the session exercises handshakes, uplinks, frames and entities");

	// The bytes, for the pinned fixture.
	const std::string fixture_sha = io::sha256_hex(fixture_bytes.data(), fixture_bytes.size());
	const std::string output_sha = io::sha256_hex(lib_text.data(), lib_text.size());
	if (fixture_sha != kFixtureSha256) {
		std::fprintf(stderr,
				"FAIL: the self-capture fixture changed (sha256 %s); re-pin kFixtureSha256 "
				"and kOutputSha256 (now %s) after checking the line contract\n",
				fixture_sha.c_str(), output_sha.c_str());
		++g_failures;
	} else {
		expect(output_sha == kOutputSha256, "the --parity-events bytes match the pinned hash");
		if (output_sha != kOutputSha256)
			std::fprintf(stderr, "  output sha256 %s\n", output_sha.c_str());
	}

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("OK: %zu lines, sha256 %s\n", lines.size(), output_sha.c_str());
	return 0;
}
