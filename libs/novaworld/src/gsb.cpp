#include <novaworld/gsb.h>

#include <novacrypto/nwu.h>

#include <cstring>
#include <string>

namespace opennova {

// NW-G2 (grill wave 2, 2026-06-11): retail Jointops.exe (JO:CA) parses the
// GSB-style chunk format — the strings "SVRS" (@0x63d793) and "FLDS"
// (@0x63d7b1) are present, and "GLB "/"PLYR"/"FIEL"-as-a-chunk are absent.
// Our builder below emits IVAR/FLDS/SVRS/XXXX, which matches retail. The
// earlier note (project_net_phase_c2_visibility) that the browser uses
// "GLB /FIEL/DATA/PLYR" is the *jodemo* demo binary's separate format; if
// demo support matters it needs the jodemo IDB and a distinct builder.
// Verdict: MATCHING for retail. See docs/net/novaworld-net-re.md §8.

namespace {

// Mirrors onnw/gsb.py::FIELD_NAMES (26 fields, order matters — the client's
// browser UI reads the server row as a positional list keyed against the
// FIELDS definition sent in the preceding SVRS chunk).
inline const char *const GSB_FIELD_NAMES[] = {
		"ServerName",   //  0
		"GameType",     //  1
		"MissionName",  //  2
		"Region",       //  3
		"Players",      //  4
		"MaxPlayers",   //  5
		"Dedicated",    //  6
		"TimeLeft",     //  7
		"Password",     //  8
		"Country",      //  9
		"Msg",          // 10
		"Age",          // 11
		"TimeOfDay",    // 12
		"Stat",         // 13
		"LevelRange",   // 14
		"Locked",       // 15
		"Tracers",      // 16
		"Skins",        // 17
		"BBMode",       // 18
		"Mod",          // 19
		"PIX",          // 20
		"PBSERVER",     // 21
		"VER1",         // 22
		"Exp",          // 23
		"Expbits",      // 24
		"Joicon2",      // 25
};
constexpr size_t GSB_FIELD_COUNT = sizeof(GSB_FIELD_NAMES) / sizeof(GSB_FIELD_NAMES[0]);

// LE write helpers.
void push_u16_le(std::vector<uint8_t> &buf, uint16_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void push_u32_le(std::vector<uint8_t> &buf, uint32_t v) {
	buf.push_back(static_cast<uint8_t>(v & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void push_ascii_cstr(std::vector<uint8_t> &buf, const std::string &s) {
	buf.insert(buf.end(), s.begin(), s.end());
	buf.push_back(0x00);
}

// Mirrors onnw/gsb.py::_pack_chunk: encrypt the payload with the 22-digit
// GSB NWU key, then prefix the encrypted length and suffix the 4-byte magic
// tag that identifies the chunk kind.
//
// NOTE on encrypt-vs-decrypt direction: the function names `nwu_encrypt` /
// `nwu_decrypt` in this codebase mirror jodemo.exe's naming
// (`PFF_EncryptBuffer` = our nwu_encrypt = SUBTRACT chain; `Crypto_DecryptBuffer`
// = our nwu_decrypt = ADD chain). Onnet's Python names `nwu_encrypt` and
// `nwu_decrypt` are SWAPPED relative to ours: onnet's `nwu_encrypt` is the
// ADD chain (== our `nwu_decrypt`), and onnet's `nwu_decrypt` is the SUBTRACT
// chain (== our `nwu_encrypt`).
//
// Onnet's `_pack_chunk` calls `nwu_encrypt(payload)` — i.e. ADD chain. To
// match byte-for-byte we call `nwu_decrypt` here. Retail decodes by applying
// the SUBTRACT chain (== onnet's `nwu_decrypt` == our `nwu_encrypt`).
void append_chunk(std::vector<uint8_t> &out,
                  std::vector<uint8_t> payload,
                  const char magic[4]) {
	if (!payload.empty()) {
		nwu_decrypt(payload.data(), payload.size(), GSB_NWU_KEY);
	}
	push_u32_le(out, static_cast<uint32_t>(payload.size()));
	out.insert(out.end(), payload.begin(), payload.end());
	out.push_back(static_cast<uint8_t>(magic[0]));
	out.push_back(static_cast<uint8_t>(magic[1]));
	out.push_back(static_cast<uint8_t>(magic[2]));
	out.push_back(static_cast<uint8_t>(magic[3]));
}

// Pull a field value out of a GsbServerEntry keyed by the FIELD_NAMES
// index. The mapping is hand-kept in sync with FIELD_NAMES above.
std::string field_value(const GsbServerEntry &s, size_t idx) {
	auto int_s = [](int v) { return std::to_string(v); };
	switch (idx) {
		case  0: return s.server_name;
		case  1: return s.game_type;
		case  2: return s.mission_name;
		case  3: return s.region;
		case  4: return int_s(s.players);
		case  5: return int_s(s.max_players);
		case  6: return s.dedicated;
		case  7: return s.time_left;
		case  8: return s.password;
		case  9: return s.country;
		case 10: return s.msg;
		case 11: return s.age;
		case 12: return s.time_of_day;
		case 13: return s.stat;
		case 14: return s.level_range;
		case 15: return s.locked;
		case 16: return s.tracers;
		case 17: return s.skins;
		case 18: return s.bb_mode;
		case 19: return s.mod;
		case 20: return s.pix;
		case 21: return s.pb_server;
		case 22: return s.ver1;
		case 23: return s.exp;
		case 24: return s.exp_bits;
		case 25: return s.joicon2;
		default: return {};
	}
}

// IVAR chunk — opaque fixed blob that onnet ships verbatim. The bytes
// aren't independently verified beyond "onnet uses this literal and
// retail accepts it" (`onnw/gsb.py::_build_ivar_chunk`). Looks like a
// mini KV header: version/timestamp/"jop_2\0" plus padding.
std::vector<uint8_t> build_ivar_payload() {
	static const uint8_t RAW[] = {
		0x00, 0x00, 0x01, 0x00,
		0xe9, 0x07, 0x0a, 0x0a,
		0x13, 0x20, 0x30, 0x00,
		0x00, 0x05,
		'j', 'o', 'p', '_', '2',
		0x00,
	};
	return std::vector<uint8_t>(RAW, RAW + sizeof(RAW));
}

// FLDS chunk — summary KV block (TotalServers / TotalPlayers).
std::vector<uint8_t> build_summary_payload(const std::vector<GsbServerEntry> &servers) {
	int total_players = 0;
	for (const auto &s : servers) {
		total_players += s.players;
	}
	std::vector<uint8_t> payload;
	push_u16_le(payload, 2);
	push_ascii_cstr(payload, "TotalServers");
	push_ascii_cstr(payload, std::to_string(servers.size()));
	push_ascii_cstr(payload, "TotalPlayers");
	push_ascii_cstr(payload, std::to_string(total_players));
	return payload;
}

// SVRS (fields) chunk — field-name table. Content is a LE u16 count
// followed by the null-terminated field names in FIELD_NAMES order.
std::vector<uint8_t> build_fields_payload() {
	std::vector<uint8_t> payload;
	push_u16_le(payload, static_cast<uint16_t>(GSB_FIELD_COUNT));
	for (size_t i = 0; i < GSB_FIELD_COUNT; ++i) {
		push_ascii_cstr(payload, std::string(GSB_FIELD_NAMES[i]));
	}
	return payload;
}

// SVRS (servers) chunk — per-server rows.
//
// Row layout (per onnw/gsb.py::_build_servers_chunk):
//   [u32 LE rid]
//   [4 bytes ip (big-endian octets)]
//   [FIELD_COUNT × (ASCII value + 0x00)]
//   [u16 LE 0]  — trailing marker
std::vector<uint8_t> build_servers_payload(const std::vector<GsbServerEntry> &servers) {
	std::vector<uint8_t> payload;
	push_u16_le(payload, static_cast<uint16_t>(servers.size()));
	for (const auto &s : servers) {
		push_u32_le(payload, s.rid);
		payload.push_back(s.ip[0]);
		payload.push_back(s.ip[1]);
		payload.push_back(s.ip[2]);
		payload.push_back(s.ip[3]);
		for (size_t i = 0; i < GSB_FIELD_COUNT; ++i) {
			push_ascii_cstr(payload, field_value(s, i));
		}
		push_u16_le(payload, 0);
	}
	return payload;
}

} // namespace

std::vector<uint8_t> gsb_build_response(const std::vector<GsbServerEntry> &servers) {
	std::vector<uint8_t> out;
	out.reserve(512 + 256 * servers.size());

	// 4-byte unencrypted header.
	out.push_back('G');
	out.push_back('S');
	out.push_back('B');
	out.push_back(' ');

	append_chunk(out, build_ivar_payload(),           "IVAR");
	append_chunk(out, build_summary_payload(servers), "FLDS");
	append_chunk(out, build_fields_payload(),         "SVRS");
	append_chunk(out, build_servers_payload(servers), "SVRS");
	append_chunk(out, {},                             "XXXX");

	return out;
}

} // namespace opennova
