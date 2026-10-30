// opennova-wire --parity-events: the canonical parity trace (apps/wire/parity_trace.h).
//
// The line stream is a contract: scripts/net/wait_parity_wire_ready.ps1 parses it and
// opennova-parity compares the same records in memory. Over the committed self-capture
// session this pins
//   - the executable prints exactly what the library printer prints;
//   - every line matches the readiness script's own line patterns;
//   - the collected records and the printed lines agree in count;
//   - the bytes: the LF-normalized output's SHA-256 for the pinned fixture;
//   - the round events' lines (PARITY_ROUND), which the committed session never fires,
//     over a synthetic S2C 0x0A through the same builder;
//   - an objective game's S2C 0x0A, whose body the session's protocol lays out (DFX2-1),
//     refused while no protocol is known and decoded under JO's.
// A fixture regeneration (nw_self_capture_test --write-fixture) changes both hashes:
// re-pin kFixtureSha256 and kOutputSha256 in the same commit, after checking that the
// new output still matches the old line contract (the printed hashes are the new ones).
//
// Usage: wire_parity_events_test <path to opennova-wire>

#include "parity_trace.h"

#include <base/io/file_io.h>
#include <base/io/sha256.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/protocol/protocol.h>

#include "common/run_command.h"
#include "common/temp_dir.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <regex>
#include <string>
#include <vector>

#ifndef SELF_CAPTURE_FIXTURE
#define SELF_CAPTURE_FIXTURE ""
#endif

using namespace opennova;

namespace {

constexpr const char *kFixtureSha256 =
		"e9497e8839ed06d567cd5715db063e00cb14b5155af1891c93d919767e84d1b9";
constexpr const char *kOutputSha256 =
		"956670539b687ba9274ed4106b90044f467fddd2cdc0f58a14d402fed0ab2741";

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
				   "server_auth|client_protocol|server_protocol|client_goodbye|"
				   "server_goodbye) decode=([01])$"),
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
		std::regex("^PARITY_ROUND frame=(\\d+) ts_ns=(\\d+) session=(\\d+) index=(\\d+) "
				   "shooter=(\\d+) flags=0x([0-9a-fA-F]{2}) adm=(\\d+) sub=(\\d+) "
				   "target=(\\d+) seq=(\\d+) slot=(\\d+)$"),
	};
	return patterns;
}

bool matches_contract(const std::string &line) {
	for (const std::regex &pattern : contract_patterns())
		if (std::regex_match(line, pattern))
			return true;
	return false;
}

size_t count_prefix(const std::vector<std::string> &lines, const char *prefix) {
	size_t n = 0;
	for (const std::string &line : lines)
		if (line.rfind(prefix, 0) == 0)
			++n;
	return n;
}

// A S2C 0x0A carrying one player record and two round events, as x8's 01tr-coop-aikill RR
// sent them to the joiner 0x005D: the rifleman 0x0019's targeted round (0x41, naming the
// joiner) and the mounted gunner 0x0006's untargeted one with a slot byte (0x32 | 0x80).
// Retail's writer puts the target word only under 0x40 and the slot byte only under 0x80
// [orig: NetPacket_SerializeRoundEvent @0x504820 (the flag @0x5048BF..0x5048C1, 0x80
// @0x5048C4..0x5048C8, the word @0x50494D..0x504966)]; the line holds 65535 / 0 otherwise.
void test_round_lines(const test_temp::TempDir &dir) {
	FrameUpdate fu;
	fu.flags2 = 2; // the env sub-block
	fu.health = 100;
	FrameUpdateRecord joiner;
	joiner.cls = EntityClass::Player;
	joiner.handle = 0x005D;
	joiner.type_id = kPlayerPersonTypeId;
	fu.records.push_back(joiner);
	RoundEventRecord rifle;
	rifle.flags = 0x41;
	rifle.adm_index = 21;
	rifle.shooter_handle = 0x0019;
	rifle.target_handle = 0x005D;
	rifle.shot_seq = 0xF7FF;
	RoundEventRecord gun;
	gun.flags = 0xB2;
	gun.adm_index = 38;
	gun.slot_byte = 2;
	gun.shooter_handle = 0x0006;
	gun.target_handle = 0x0042; // not on the wire without 0x40
	gun.shot_seq = 12;
	fu.round_events = { rifle, gun };

	InGameMessage message;
	message.frame_index = 7;
	message.dir = 'S';
	message.tag = 0x0A;
	message.session = 32776;
	message.participant = 1;
	message.payload = encode_frame_update(fu);
	CaptureDecodeResult decoded;
	decoded.messages.push_back(message);
	const std::map<int, uint64_t> stamps = { { 7, 1785000000000000000ull } };

	const std::string path = dir.file("rounds.txt");
	std::FILE *out = std::fopen(path.c_str(), "wb");
	expect(out != nullptr, "open the round output file");
	if (!out)
		return;
	{
		wire::ParityTracePrinter printer(out);
		wire::ParityTraceBuilder builder(printer, nullptr, nullptr);
		builder.push_decoded(decoded, stamps);
	}
	std::fclose(out);
	wire::ParityTrace trace;
	wire::ParityTraceCollector collector(trace);
	wire::ParityTraceBuilder builder(collector, nullptr, nullptr);
	builder.push_decoded(decoded, stamps);

	std::string text, error;
	expect(io::read_file_text(path, text, error), "read back the round output");
	const std::vector<std::string> lines = lines_of(text);
	for (const std::string &line : lines)
		expect(matches_contract(line), "every synthetic line matches the readiness patterns");
	expect(count_prefix(lines, "PARITY_ROUND ") == 2 && trace.rounds.size() == 2,
			"the frame's two round events print and collect");
	expect(trace.frames.size() == 1 && trace.frames[0].rounds == 2,
			"the frame state declares the two rounds the round lines carry");
	expect(lines.size() == 5 && lines[2].rfind("PARITY_ENTITY ", 0) == 0,
			"the round lines follow the frame's entity line");
	if (lines.size() != 5)
		return;
	expect(lines[3] ==
					"PARITY_ROUND frame=7 ts_ns=1785000000000000000 session=32776 index=0 "
					"shooter=25 flags=0x41 adm=21 sub=0 target=93 seq=63487 slot=0",
			"the targeted round prints its target");
	expect(lines[4] ==
					"PARITY_ROUND frame=7 ts_ns=1785000000000000000 session=32776 index=1 "
					"shooter=6 flags=0xb2 adm=38 sub=0 target=65535 seq=12 slot=2",
			"the untargeted round prints 65535 and its slot byte");
}

// An objective game's S2C 0x0A (the game type a 0x7B's extra word named, its 0x20000 bit
// set) lays its objective body out by the session's protocol (DFX2-1): with none named and
// no session hello to learn it from, the builder refuses the frame and stops; under JO's it
// decodes.
void test_objective_frame_needs_protocol() {
	InGameMessage summary;
	summary.frame_index = 3;
	summary.dir = 'S';
	summary.tag = s2c::FULL_PLAYER_INFO;
	summary.session = 32776;
	summary.participant = 1;
	// Five empty strings, the extra word 0x30020 (co-op), two empty strings.
	summary.payload = { 0, 0, 0, 0, 0, 0x20, 0x00, 0x03, 0x00, 0, 0 };
	FrameUpdate fu;
	fu.health = 100;
	InGameMessage frame;
	frame.frame_index = 4;
	frame.dir = 'S';
	frame.tag = s2c::PER_FRAME_UPDATE;
	frame.session = 32776;
	frame.participant = 1;
	frame.payload = encode_frame_update(fu);
	CaptureDecodeResult decoded;
	decoded.messages = { summary, frame };
	const std::map<int, uint64_t> stamps;

	wire::ParityTrace unnamed;
	wire::ParityTraceCollector unnamed_collector(unnamed);
	wire::ParityTraceBuilder refused(unnamed_collector, nullptr, nullptr);
	refused.push_decoded(decoded, stamps);
	expect(!refused.error().empty() && unnamed.frames.empty(),
			"an objective frame with no protocol known stops the trace");

	wire::ParityTrace named;
	wire::ParityTraceCollector named_collector(named);
	wire::ParityTraceBuilder decodes(named_collector, nullptr, title_protocol("jo"));
	decodes.push_decoded(decoded, stamps);
	expect(decodes.error().empty() && named.frames.size() == 1,
			"an objective frame decodes under the named protocol");
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
		wire::ParityTraceBuilder builder(printer, nullptr, nullptr); // the hello names the protocol
		const bool read = net::stream_pcap_udp_file(fixture, [&](const net::PcapDatagram &d) {
			builder.push(d);
			return true;
		});
		expect(read, "the fixture streams as a pcap");
	}
	std::fclose(lib_out);
	wire::ParityTrace trace;
	expect(wire::read_parity_trace(fixture, nullptr, trace, error),
			"read_parity_trace reads the fixture");

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
	for (const std::string &line : lines)
		if (!matches_contract(line) && ++unmatched <= 3)
			std::fprintf(stderr, "unmatched contract line: %.160s\n", line.c_str());
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
	expect(count_prefix(lines, "PARITY_ROUND ") == trace.rounds.size(),
			"round lines == collected rounds");
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

	test_round_lines(dir);
	test_objective_frame_needs_protocol();

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("OK: %zu lines, sha256 %s\n", lines.size(), output_sha.c_str());
	return 0;
}
