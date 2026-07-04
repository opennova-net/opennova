// Replays the retail JO capture fixtures (fixtures/novaworld/run_*/*.nwmsg)
// through the ProtocolMessage codec.
//
// The .nwmsg files are decrypted in-match protocol streams recorded from live
// retail Joint Operations sessions (2026-04-26 nethook runs; format written by
// the worktree-net-final fixture tooling: one "bundle <label> <role> <dir>
// <key_fp> <ts_us>" header, then "msg <flags> <full_type> <body-hex>" lines,
// then "end"). Every message carries the original wire flags byte, so
// re-encoding from the recorded fields must reproduce a stream our parser
// reads back identically — that pins the codec against real retail traffic
// across the §4 dispatch-table tag space (docs/net/novaworld-net-re.md).
//
// Also pins two RE-doc facts straight from the captures:
//   §5.4 — the S2C 0x0B BMS header blob is 616 bytes, signature 42 4d 53 13,
//          map basename at +68.
//   §5.5 — retail ships 0x11 only inside the [0x1C, 0x0B, 0x66, 0x76, 0x11]
//          bundle, 0x11 last with an empty payload.

#include <npwire/protocol_message.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "common/test_expect.h"

using opennova::ProtocolMessage;
using opennova::make_protocol_message;

namespace {

struct FixtureMessage {
	uint8_t flags_raw = 0;
	uint16_t full_type = 0;
	std::vector<uint8_t> body;
};

struct FixtureBundle {
	std::string label;
	std::vector<FixtureMessage> messages;
};

bool hex_to_bytes(const std::string &hex, std::vector<uint8_t> &out) {
	if (hex.size() % 2 != 0) return false;
	out.clear();
	out.reserve(hex.size() / 2);
	for (size_t i = 0; i < hex.size(); i += 2) {
		auto nib = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};
		const int hi = nib(hex[i]);
		const int lo = nib(hex[i + 1]);
		if (hi < 0 || lo < 0) return false;
		out.push_back(static_cast<uint8_t>((hi << 4) | lo));
	}
	return true;
}

bool load_nwmsg(const std::string &path, std::vector<FixtureBundle> &out,
                std::string &err) {
	std::ifstream file(path);
	if (!file) {
		err = "cannot open " + path;
		return false;
	}
	out.clear();
	std::string line;
	FixtureBundle current;
	bool in_bundle = false;
	bool saw_header = false;
	while (std::getline(file, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;
		if (line[0] == '#') {
			if (line.find("novaworld fixture v1") != std::string::npos) saw_header = true;
			continue;
		}
		std::istringstream ls(line);
		std::string word;
		ls >> word;
		if (word == "bundle") {
			if (in_bundle) {
				err = "nested bundle in " + path;
				return false;
			}
			current = FixtureBundle{};
			ls >> current.label;
			in_bundle = true;
		} else if (word == "msg") {
			if (!in_bundle) {
				err = "msg outside bundle in " + path;
				return false;
			}
			std::string flags_s, type_s, body_s;
			ls >> flags_s >> type_s;
			ls >> body_s; // "-" (or absent) for empty payloads
			FixtureMessage m;
			m.flags_raw = static_cast<uint8_t>(std::stoul(flags_s, nullptr, 16));
			m.full_type = static_cast<uint16_t>(std::stoul(type_s, nullptr, 16));
			if (body_s == "-") body_s.clear();
			if (!body_s.empty() && !hex_to_bytes(body_s, m.body)) {
				err = "bad hex in " + path + ": " + line.substr(0, 40);
				return false;
			}
			current.messages.push_back(std::move(m));
		} else if (word == "end") {
			if (!in_bundle) {
				err = "end outside bundle in " + path;
				return false;
			}
			out.push_back(std::move(current));
			in_bundle = false;
		} else {
			err = "unknown directive '" + word + "' in " + path;
			return false;
		}
	}
	if (in_bundle) {
		err = "unterminated bundle in " + path;
		return false;
	}
	if (!saw_header) {
		err = "missing fixture v1 header in " + path;
		return false;
	}
	return true;
}

// The manifest's per-file sections name the .nwmsg files and their counts.
struct ManifestSection {
	int bundles = -1;
	int messages = -1;
	std::set<uint16_t> types;
};

bool load_manifest(const std::string &path,
                   std::map<std::string, ManifestSection> &out, std::string &err) {
	std::ifstream file(path);
	if (!file) {
		err = "cannot open " + path;
		return false;
	}
	out.clear();
	std::string line;
	std::string section;
	while (std::getline(file, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		if (line.front() == '[' && line.back() == ']') {
			section = line.substr(1, line.size() - 2);
			out[section] = ManifestSection{};
			continue;
		}
		if (section.empty()) continue;
		const auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string key = line.substr(0, eq);
		std::string value = line.substr(eq + 1);
		auto trim = [](std::string &s) {
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
		};
		trim(key);
		trim(value);
		if (key == "bundles") out[section].bundles = std::stoi(value);
		else if (key == "messages") out[section].messages = std::stoi(value);
		else if (key == "types") {
			std::istringstream ts(value);
			std::string tok;
			while (std::getline(ts, tok, ',')) {
				trim(tok);
				if (!tok.empty())
					out[section].types.insert(
					    static_cast<uint16_t>(std::stoul(tok, nullptr, 16)));
			}
		}
	}
	return true;
}

// Build a ProtocolMessage carrying the EXACT recorded wire flags byte.
// make_protocol_message() coerces flags_raw==0 to LEN8, but retail really
// does send zero-flag messages (no length field, empty payload), so the
// roundtrip must preserve them.
ProtocolMessage message_from_fixture(const FixtureMessage &m) {
	ProtocolMessage msg;
	msg.tag = static_cast<uint8_t>(m.full_type & 0xFFu);
	msg.full_tag = static_cast<uint16_t>(
			((m.flags_raw & 0x80u) ? 0x100u : 0u) | msg.tag);
	msg.payload = m.body;
	msg.length = static_cast<uint32_t>(m.body.size());
	const uint8_t raw = m.flags_raw;
	msg.flags.settings_update = (raw & 0x80u) != 0;
	msg.flags.len16 = (raw & 0x40u) != 0;
	msg.flags.len8 = (raw & 0x20u) != 0;
	msg.flags.skip2 = (raw & 0x10u) != 0;
	msg.flags.skip1 = (raw & 0x08u) != 0;
	msg.flags.frag_cont = (raw & 0x04u) != 0;
	msg.flags.frag_end = (raw & 0x02u) != 0;
	msg.flags.msg_type_high_bit = (raw & 0x80u) != 0;
	msg.flags.raw = raw;
	return msg;
}

uint16_t runtime_full_type(const FixtureMessage &m) {
	return static_cast<uint16_t>(
			((m.flags_raw & 0x80u) ? 0x100u : 0u) |
			static_cast<uint8_t>(m.full_type & 0xFFu));
}

const char *const kRuns[] = {
	"run_20260426_113242",
	"run_20260426_120102",
	"run_20260426_120859",
};

} // namespace

int main() {
	const std::string fixture_root = FIXTURE_DIR;
	size_t total_messages = 0;
	size_t total_bundles = 0;
	std::set<uint16_t> seen_types;

	for (const char *run : kRuns) {
		const std::string run_dir = fixture_root + "/" + run;
		std::map<std::string, ManifestSection> manifest;
		std::string err;
		TEST_EXPECT(load_manifest(run_dir + "/manifest.txt", manifest, err));
		TEST_EXPECT(!manifest.empty());

		for (const auto &entry : manifest) {
			const std::string nwmsg_path = run_dir + "/" + entry.first;
			std::vector<FixtureBundle> bundles;
			if (!load_nwmsg(nwmsg_path, bundles, err)) {
				std::fprintf(stderr, "load failed: %s\n", err.c_str());
				return 1;
			}

			// Manifest counts must match the parsed reality.
			int message_count = 0;
			std::set<uint16_t> file_types;
			for (const auto &b : bundles) {
				message_count += static_cast<int>(b.messages.size());
				for (const auto &m : b.messages) file_types.insert(m.full_type);
			}
			TEST_EXPECT(static_cast<int>(bundles.size()) == entry.second.bundles);
			TEST_EXPECT(message_count == entry.second.messages);
			TEST_EXPECT(file_types == entry.second.types);

			// Codec roundtrip per bundle: rebuild each message from the
			// recorded wire fields, encode the bundle's inner-message
			// stream, parse it back, and require identical fields.
			for (const auto &b : bundles) {
				std::vector<ProtocolMessage> rebuilt;
				rebuilt.reserve(b.messages.size());
				for (const auto &m : b.messages) {
					// No capture uses the SKIP1/SKIP2 cursor bits; the
					// fixture format does not record skip bytes, so a
					// sighting here means the format needs extending.
					TEST_EXPECT((m.flags_raw & 0x18u) == 0);
					// Zero-flag messages carry no length field at all, so
					// a non-empty payload would be unrepresentable.
					if ((m.flags_raw & 0x60u) == 0) TEST_EXPECT(m.body.empty());
					rebuilt.push_back(message_from_fixture(m));
					TEST_EXPECT(rebuilt.back().full_tag == runtime_full_type(m));
				}

				std::vector<uint8_t> stream;
				TEST_EXPECT(opennova::encode_protocol_messages(rebuilt, stream));

				std::vector<ProtocolMessage> reparsed;
				TEST_EXPECT(opennova::parse_protocol_messages(
				    stream.data(), stream.size(), reparsed));
				TEST_EXPECT(reparsed.size() == b.messages.size());
				for (size_t i = 0; i < reparsed.size(); ++i) {
					TEST_EXPECT(reparsed[i].flags.raw == b.messages[i].flags_raw);
					TEST_EXPECT(reparsed[i].full_tag == runtime_full_type(b.messages[i]));
					TEST_EXPECT(reparsed[i].payload == b.messages[i].body);
				}

				total_messages += b.messages.size();
				++total_bundles;
				for (const auto &m : b.messages) seen_types.insert(runtime_full_type(m));
			}
		}

		// §5.5 pin — the BMS-state bundle: [0x1C, 0x0B, 0x66, 0x76, 0x11],
		// 0x11 last with an empty payload; §5.4 pin — 0x0B is the 616-byte
		// BMS header (signature 42 4d 53 13, map basename at +68).
		std::vector<FixtureBundle> bms_bundles;
		TEST_EXPECT(load_nwmsg(run_dir + "/bundle_1c_0b_66_76_11.nwmsg",
		                       bms_bundles, err));
		TEST_EXPECT(bms_bundles.size() == 1);
		const auto &bms = bms_bundles[0].messages;
		TEST_EXPECT(bms.size() == 5);
		const uint16_t expected_order[5] = {0x1C, 0x0B, 0x66, 0x76, 0x11};
		for (size_t i = 0; i < 5; ++i)
			TEST_EXPECT(bms[i].full_type == expected_order[i]);
		TEST_EXPECT(bms[4].body.empty());

		const auto &header_blob = bms[1].body;
		TEST_EXPECT(header_blob.size() == 616);
		TEST_EXPECT(header_blob[0] == 0x42 && header_blob[1] == 0x4d &&
		            header_blob[2] == 0x53 && header_blob[3] == 0x13);
		// Map basename at +68: NUL-terminated, non-empty, printable ASCII.
		TEST_EXPECT(header_blob[68] != 0);
		std::string basename;
		for (size_t i = 68; i < 100 && header_blob[i] != 0; ++i) {
			TEST_EXPECT(std::isprint(header_blob[i]));
			basename.push_back(static_cast<char>(header_blob[i]));
		}
		std::printf("[%s] BMS header basename: %s\n", run, basename.c_str());
	}

	std::printf("replayed %zu messages across %zu bundles, %zu distinct tags\n",
	            total_messages, total_bundles, seen_types.size());
	TEST_EXPECT(total_bundles >= 30);
	TEST_EXPECT(total_messages >= 100);
	return 0;
}
