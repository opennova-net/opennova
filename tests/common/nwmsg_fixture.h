// The committed retail in-match captures (fixtures/novaworld/run_*/*.nwmsg):
// the loader the codec tests share.
//
// The .nwmsg files are decrypted in-match protocol streams recorded from live
// retail Joint Operations sessions (2026-04-26 nethook runs). Format: one
// "bundle <label> <server|client> <enc> <key> <ts_us>" header, then
// "msg <flags> <tag> <body-hex|->" lines, then "end". <server|client> names the
// SENDER (server = S2C, client = C2S); <flags> is the raw inner-message flags
// byte (0x80 high table, 0x40 u16 length, 0x20 u8 length, 0x04 more fragments
// follow, 0x02 fragment end); <tag> is the low 8 bits of the message type.
// Fragments are recorded unreassembled and may straddle bundles.

#pragma once

#include <net/npwire/protocol_message.h>

#include <base/io/strutil.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace nwmsg {

struct FixtureMessage {
	uint8_t flags_raw = 0;
	uint16_t full_type = 0;
	std::vector<uint8_t> body;
};

struct FixtureBundle {
	std::string label;
	std::string role; // "server" (S2C) or "client" (C2S)
	std::vector<FixtureMessage> messages;
};

inline const char *const kRuns[] = {
	"run_20260426_113242",
	"run_20260426_120102",
	"run_20260426_120859",
};

inline bool load_nwmsg(const std::string &path, std::vector<FixtureBundle> &out,
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
			ls >> current.label >> current.role;
			if (current.role != "server" && current.role != "client") {
				err = "bad bundle role '" + current.role + "' in " + path;
				return false;
			}
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
			if (!body_s.empty() && !opennova::strutil::hex_to_bytes(body_s, m.body)) {
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

inline bool load_manifest(const std::string &path,
                          std::map<std::string, ManifestSection> &out, std::string &err) {
	std::ifstream file(path);
	if (!file) {
		err = "cannot open " + path;
		return false;
	}
	out.clear();
	std::string line;
	std::string section;
	auto trim = [](std::string &s) {
		while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
		while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
	};
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
inline opennova::ProtocolMessage message_from_fixture(const FixtureMessage &m) {
	opennova::ProtocolMessage msg;
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

inline uint16_t runtime_full_type(const FixtureMessage &m) {
	return static_cast<uint16_t>(
			((m.flags_raw & 0x80u) ? 0x100u : 0u) |
			static_cast<uint8_t>(m.full_type & 0xFFu));
}

// One logical message after fragment reassembly.
struct LogicalMessage {
	char dir = 'S';          // 'S' = S2C, 'C' = C2S
	uint16_t full_tag = 0;   // 0x100 | tag for the high table
	std::vector<uint8_t> body;
	bool fragmented = false;
};

// Reassemble one file's stream with a single per-file state, so fragments that
// straddle bundles join. A chain that starts mid-way (FRAG_END with nothing
// buffered) or is still open at the file's end fails.
inline bool reassembled_messages(const std::vector<FixtureBundle> &bundles,
                                 std::vector<LogicalMessage> &out, std::string &err) {
	out.clear();
	opennova::ProtocolReassemblyState state;
	for (const FixtureBundle &b : bundles) {
		const char dir = b.role == "server" ? 'S' : 'C';
		for (const FixtureMessage &m : b.messages) {
			const uint8_t frag = m.flags_raw & 0x06u;
			if ((frag & 0x02u) != 0 && state.buffer.empty()) {
				err = "fragment continuation with no chain open in bundle " + b.label;
				return false;
			}
			std::vector<uint8_t> payload;
			bool was_fragmented = false;
			if (!opennova::reassemble_protocol_payload(state, message_from_fixture(m),
			                                           payload, &was_fragmented))
				continue;
			LogicalMessage lm;
			lm.dir = dir;
			lm.full_tag = runtime_full_type(m);
			lm.body = std::move(payload);
			lm.fragmented = was_fragmented;
			out.push_back(std::move(lm));
		}
	}
	if (!state.buffer.empty()) {
		err = "fragment chain still open at end of file";
		return false;
	}
	return true;
}

} // namespace nwmsg
