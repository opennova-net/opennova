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

#include <net/npwire/protocol_message.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/nwmsg_fixture.h"
#include "common/test_expect.h"

using opennova::ProtocolMessage;
using nwmsg::FixtureBundle;
using nwmsg::ManifestSection;
using nwmsg::kRuns;
using nwmsg::load_manifest;
using nwmsg::load_nwmsg;
using nwmsg::message_from_fixture;
using nwmsg::runtime_full_type;

namespace {

// `<name>_packets.nwmsg` re-splits `<name>.nwmsg` on decoded protocol-packet
// boundaries instead of captured frames: the same message stream, other bundles.
bool same_message_stream(const std::vector<FixtureBundle> &a,
                         const std::vector<FixtureBundle> &b) {
	std::vector<const nwmsg::FixtureMessage *> fa, fb;
	for (const auto &x : a) for (const auto &m : x.messages) fa.push_back(&m);
	for (const auto &x : b) for (const auto &m : x.messages) fb.push_back(&m);
	if (fa.size() != fb.size()) return false;
	for (size_t i = 0; i < fa.size(); ++i)
		if (fa[i]->flags_raw != fb[i]->flags_raw || fa[i]->full_type != fb[i]->full_type ||
		    fa[i]->body != fb[i]->body)
			return false;
	return true;
}

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

		// Each `X_packets.nwmsg` carries exactly `X.nwmsg`'s message stream.
		for (const auto &entry : manifest) {
			const std::string &name = entry.first;
			const std::string suffix = "_packets.nwmsg";
			if (name.size() <= suffix.size() ||
			    name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
				continue;
			const std::string plain = name.substr(0, name.size() - suffix.size()) + ".nwmsg";
			if (manifest.count(plain) == 0) continue; // e.g. server_load_packets has no sibling
			std::vector<FixtureBundle> packets, frames;
			TEST_EXPECT(load_nwmsg(run_dir + "/" + name, packets, err));
			TEST_EXPECT(load_nwmsg(run_dir + "/" + plain, frames, err));
			TEST_EXPECT(same_message_stream(packets, frames));
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
