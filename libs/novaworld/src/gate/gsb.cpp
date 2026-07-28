#include <novaworld/gsb.h>

#include <novacrypto/nwu.h>

#include <cstring>
#include <string>

#include <io/strutil.h>

namespace opennova {

// Witnessed against the retail GSB parser NapiGameList_ProcessEncryptedResponse
// @ 0x63d740 (docs/net/novaworld-net-re.md §7 Waves 7+9, the 2026-07-27 grill).
// The blob is a flat chunk stream [magic:4][u32 LE len][payload], advance len+8
// — magic is a PREFIX and "GSB " is the first chunk's tag, NOT a bare file
// header (D-NET-32/33). Chunk roles: "GSB "=reset (only honored when payload
// dword0==0x00010000, @ 0x63d8f2), "FLDS"=field names (replaces the table),
// "SVRS"=server rows (records ACCUMULATE, D-NET-191), "XXXX"=finalize
// (D-NET-34; retail then pings every row and never advances past it). Row =
// [u32 rid][4-byte IPv4 in_addr][N values][u16 player_count][player names]
// (D-NET-35 as corrected by D-NET-190: the second dword is the ping-target IP,
// not a port — NapiGameList_StartPingSweep @ 0x63bcf0 formats entry+4 via
// Network_FormatIPAddressToString).

namespace {

// FLDS column set — 26 columns, witnessed byte-for-byte against the genuine
// .204 GSB blob (fixtures/novaworld/nw204_jop_2.gsb, gsb_real204_decode_test):
// the names AND order below match retail exactly. The join `rid` is NOT a column
// — it is the row's first u32 (see build_servers_payload / parse_servers).
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

// IPv4 dotted-quad -> the row's 4 wire bytes, in_addr byte order (a.b.c.d in
// memory) — exactly what retail casts to `struct in_addr` at entry+4
// [orig: NapiGameList_StartPingSweep @ 0x63BCF0]. Unparseable input emits
// 0.0.0.0 rather than failing the whole blob.
void push_ipv4_dotted(std::vector<uint8_t> &buf, const std::string &dotted) {
	uint8_t octets[4] = {0, 0, 0, 0};
	unsigned acc = 0, digits = 0, idx = 0;
	bool ok = true;
	for (const char c : dotted) {
		if (c >= '0' && c <= '9') {
			acc = acc * 10 + static_cast<unsigned>(c - '0');
			if (++digits > 3 || acc > 255) { ok = false; break; }
		} else if (c == '.') {
			if (digits == 0 || idx >= 3) { ok = false; break; }
			octets[idx++] = static_cast<uint8_t>(acc);
			acc = 0;
			digits = 0;
		} else {
			ok = false;
			break;
		}
	}
	if (ok && idx == 3 && digits > 0) {
		octets[3] = static_cast<uint8_t>(acc);
	} else {
		octets[0] = octets[1] = octets[2] = octets[3] = 0;
	}
	buf.insert(buf.end(), octets, octets + 4);
}

std::string ipv4_bytes_to_dotted(const uint8_t bytes[4]) {
	std::string out;
	for (int i = 0; i < 4; ++i) {
		if (i) out.push_back('.');
		out += std::to_string(static_cast<unsigned>(bytes[i]));
	}
	return out;
}

// Emit one chunk: [4-byte magic PREFIX][u32 LE payload_length][encrypted payload].
// The payload is encrypted with the ADD chain (our nwu_decrypt == retail
// NapiNP_EncryptBuffer @ 0x6187b0); the retail parser decodes with the inverse
// SUBTRACT chain (NapiNP_DecryptBuffer @ 0x618880 == our nwu_encrypt).
void append_chunk(std::vector<uint8_t> &out,
                  const char magic[4],
                  std::vector<uint8_t> payload) {
	if (!payload.empty()) {
		nwu_decrypt(payload.data(), payload.size(), GSB_NWU_KEY);
	}
	out.push_back(static_cast<uint8_t>(magic[0]));
	out.push_back(static_cast<uint8_t>(magic[1]));
	out.push_back(static_cast<uint8_t>(magic[2]));
	out.push_back(static_cast<uint8_t>(magic[3]));
	push_u32_le(out, static_cast<uint32_t>(payload.size()));
	out.insert(out.end(), payload.begin(), payload.end());
}

// Pull a field value out of a GsbServerEntry keyed by the FIELD_NAMES index.
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

// "GSB " init/reset chunk payload — the retail parser only checks that it begins
// with the u32 0x00010000 (@ 0x63d8f2); the trailing bytes are a version/date +
// "jop_2" blob it ignores. (Was the onnet "IVAR" chunk.)
std::vector<uint8_t> build_init_payload() {
	static const uint8_t RAW[] = {
		0x00, 0x00, 0x01, 0x00,   // u32 0x00010000 (required prefix)
		0xe9, 0x07, 0x0a, 0x0a,
		0x13, 0x20, 0x30, 0x00,
		0x00, 0x05,
		'j', 'o', 'p', '_', '2',
		0x00,
	};
	return std::vector<uint8_t>(RAW, RAW + sizeof(RAW));
}

// FLDS chunk — field-name table: [u16 LE count][count × NUL-term name].
std::vector<uint8_t> build_fields_payload() {
	std::vector<uint8_t> payload;
	push_u16_le(payload, static_cast<uint16_t>(GSB_FIELD_COUNT));
	for (size_t i = 0; i < GSB_FIELD_COUNT; ++i) {
		push_ascii_cstr(payload, std::string(GSB_FIELD_NAMES[i]));
	}
	return payload;
}

// SVRS chunk — per-server rows.
//   [u16 LE server_count]
//   per server: [u32 LE rid][4-byte IPv4, in_addr order]
//               [field_count × (ASCII value + NUL)]   (FIELD_NAMES order)
//               [u16 LE player_count][player_count × (player name + NUL)]
std::vector<uint8_t> build_servers_payload(const std::vector<GsbServerEntry> &servers) {
	std::vector<uint8_t> payload;
	push_u16_le(payload, static_cast<uint16_t>(servers.size()));
	for (const auto &s : servers) {
		push_u32_le(payload, s.rid);   // host id (the @RID@ join value)
		push_ipv4_dotted(payload, s.ip);  // ping target [orig: 0x63BCF0]
		for (size_t i = 0; i < GSB_FIELD_COUNT; ++i) {
			push_ascii_cstr(payload, field_value(s, i));
		}
		push_u16_le(payload, static_cast<uint16_t>(s.player_names.size()));
		for (const auto &name : s.player_names) {
			push_ascii_cstr(payload, name);
		}
	}
	return payload;
}

} // namespace

namespace {

// ---- GSB parse (client-direction inverse of the builder) ----------------

bool read_u16_le(const uint8_t *data, size_t len, size_t &pos, uint16_t &out) {
	if (pos + 2 > len) return false;
	out = static_cast<uint16_t>(data[pos]) | (static_cast<uint16_t>(data[pos + 1]) << 8);
	pos += 2;
	return true;
}

bool read_u32_le(const uint8_t *data, size_t len, size_t &pos, uint32_t &out) {
	if (pos + 4 > len) return false;
	out = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
	      (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
	pos += 4;
	return true;
}

// Read a NUL-terminated ASCII string from a decrypted payload.
bool read_cstr(const std::vector<uint8_t> &buf, size_t &pos, std::string &out) {
	const size_t start = pos;
	while (pos < buf.size() && buf[pos] != 0x00) ++pos;
	if (pos >= buf.size()) return false; // no terminator
	out.assign(reinterpret_cast<const char *>(buf.data() + start), pos - start);
	++pos; // skip the NUL
	return true;
}

// One decrypted chunk: 4-byte magic + the SUBTRACT-chain-decrypted payload.
struct GsbChunk {
	char magic[4];
	std::vector<uint8_t> payload;
};

// Read one chunk at `pos`: [4-byte magic][u32 LE len][len bytes].
// Decrypts the payload in place with the SUBTRACT chain (our nwu_encrypt) — the
// inverse of append_chunk's nwu_decrypt.
bool read_chunk(const uint8_t *data, size_t len, size_t &pos, GsbChunk &out) {
	if (pos + 8 > len) return false; // need magic(4) + length(4)
	std::memcpy(out.magic, data + pos, 4);
	pos += 4;
	uint32_t payload_len = 0;
	if (!read_u32_le(data, len, pos, payload_len)) return false;
	// Bounds check in 64-bit so a wire-controlled payload_len near UINT32_MAX
	// can't wrap the addition on a 32-bit build and slip an OOB read past the
	// `pos + payload_len > len` test. gsb_parse_response runs on server bytes.
	if (static_cast<uint64_t>(pos) + payload_len > static_cast<uint64_t>(len)) {
		return false;
	}
	out.payload.assign(data + pos, data + pos + payload_len);
	pos += payload_len;
	if (!out.payload.empty()) {
		nwu_encrypt(out.payload.data(), out.payload.size(), GSB_NWU_KEY);
	}
	return true;
}

bool magic_is(const GsbChunk &c, const char (&tag)[5]) {
	return c.magic[0] == tag[0] && c.magic[1] == tag[1] &&
	       c.magic[2] == tag[2] && c.magic[3] == tag[3];
}

using opennova::strutil::to_lower;

int parse_int_or_zero(const std::string &s) {
	try { return std::stoi(s); } catch (...) { return 0; }
}

// Assign one positional field value into the entry, keyed by the field name from
// the FLDS table (case-insensitive — the retail browser folds case on lookup).
void assign_field(GsbServerEntry &e, const std::string &name, const std::string &value) {
	const std::string key = to_lower(name);
	if      (key == "servername")  e.server_name  = value;
	else if (key == "gametype")    e.game_type    = value;
	else if (key == "missionname") e.mission_name = value;
	else if (key == "region")      e.region       = value;
	else if (key == "players")     e.players      = parse_int_or_zero(value);
	else if (key == "maxplayers")  e.max_players  = parse_int_or_zero(value);
	else if (key == "dedicated")   e.dedicated    = value;
	else if (key == "timeleft")    e.time_left    = value;
	else if (key == "password")    e.password     = value;
	else if (key == "country")     e.country      = value;
	else if (key == "msg")         e.msg          = value;
	else if (key == "age")         e.age          = value;
	else if (key == "timeofday")   e.time_of_day  = value;
	else if (key == "stat")        e.stat         = value;
	else if (key == "levelrange")  e.level_range  = value;
	else if (key == "locked")      e.locked       = value;
	else if (key == "tracers")     e.tracers      = value;
	else if (key == "skins")       e.skins        = value;
	else if (key == "bbmode")      e.bb_mode      = value;
	else if (key == "mod")         e.mod          = value;
	else if (key == "pix")         e.pix          = value;
	else if (key == "pbserver")    e.pb_server    = value;
	else if (key == "ver1")        e.ver1         = value;
	else if (key == "exp")         e.exp          = value;
	else if (key == "expbits")     e.exp_bits     = value;
	else if (key == "joicon2")     e.joicon2      = value;
	// Unknown field names intentionally ignored.
}

// FLDS: [u16 count][count × field-name cstr].
bool parse_fields(const std::vector<uint8_t> &payload, std::vector<std::string> &names) {
	size_t pos = 0;
	uint16_t count = 0;
	if (!read_u16_le(payload.data(), payload.size(), pos, count)) return false;
	names.clear();
	names.reserve(count);
	for (uint16_t i = 0; i < count; ++i) {
		std::string name;
		if (!read_cstr(payload, pos, name)) return false;
		names.push_back(std::move(name));
	}
	return true;
}

// SVRS: [u16 count][count × ([u32 rid][4-byte IPv4][N values][u16 player_count][names])],
// where N == field_names.size() and values are read positionally. APPENDS to
// `servers` — retail accumulates rows across SVRS records with no clear
// [orig: 0x63d740 @ 0x63dbec..0x63dc0d]; only a valid "GSB " reset record (or a
// new fetch) empties the list.
bool parse_servers(const std::vector<uint8_t> &payload,
                   const std::vector<std::string> &field_names,
                   std::vector<GsbServerEntry> &servers) {
	size_t pos = 0;
	uint16_t count = 0;
	if (!read_u16_le(payload.data(), payload.size(), pos, count)) return false;
	servers.reserve(servers.size() + count);
	for (uint16_t i = 0; i < count; ++i) {
		GsbServerEntry e{};
		if (!read_u32_le(payload.data(), payload.size(), pos, e.rid)) return false;  // host id
		if (pos + 4 > payload.size()) return false;
		e.ip = ipv4_bytes_to_dotted(payload.data() + pos);  // in_addr wire bytes
		pos += 4;
		for (const std::string &name : field_names) {
			std::string value;
			if (!read_cstr(payload, pos, value)) return false;
			assign_field(e, name, value);
		}
		uint16_t player_count = 0;
		if (!read_u16_le(payload.data(), payload.size(), pos, player_count)) return false;
		e.player_names.reserve(player_count);
		for (uint16_t p = 0; p < player_count; ++p) {
			std::string name;
			if (!read_cstr(payload, pos, name)) return false;
			e.player_names.push_back(std::move(name));
		}
		servers.push_back(std::move(e));
	}
	return true;
}

} // namespace

bool gsb_parse_response(const uint8_t *data, size_t len, GsbResponse &out) {
	out = GsbResponse{};
	if (!data || len < 8) return false; // need at least one chunk header

	size_t pos = 0;
	bool reached_terminator = false;
	while (pos < len) {
		GsbChunk chunk;
		if (!read_chunk(data, len, pos, chunk)) return false;
		if (magic_is(chunk, "GSB ")) {
			// Reset record: honored only when the payload starts with u32
			// 0x00010000 [orig: 0x63d740 @ 0x63d8f2] — retail then frees the
			// field table + every accumulated row and zeroes the totals; an
			// undersized/mismatched "GSB " record is skipped, not an error.
			if (chunk.payload.size() >= 4 &&
			    chunk.payload[0] == 0x00 && chunk.payload[1] == 0x00 &&
			    chunk.payload[2] == 0x01 && chunk.payload[3] == 0x00) {
				out.field_names.clear();
				out.servers.clear();
			}
		} else if (magic_is(chunk, "FLDS")) {
			// Retail skips a FLDS record with payload < 2 [orig: @ 0x63d7c2].
			if (chunk.payload.size() >= 2) {
				if (!parse_fields(chunk.payload, out.field_names)) return false;
			}
		} else if (magic_is(chunk, "SVRS")) {
			// Retail skips an SVRS record with payload < 2 [orig: @ 0x63da43].
			if (chunk.payload.size() >= 2) {
				if (!parse_servers(chunk.payload, out.field_names, out.servers)) return false;
			}
		} else if (magic_is(chunk, "XXXX")) {
			reached_terminator = true;
			break;
		}
		// Unknown chunk tags are walked past (read_chunk advanced the cursor).
	}
	if (!reached_terminator) return false;

	out.total_servers = static_cast<int>(out.servers.size());
	out.total_players = 0;
	for (const auto &s : out.servers) {
		out.total_players += static_cast<int>(s.player_names.size());
	}
	return true;
}

std::vector<uint8_t> gsb_build_response(const std::vector<GsbServerEntry> &servers) {
	std::vector<uint8_t> out;
	out.reserve(512 + 256 * servers.size());

	// Flat chunk stream — no bare header; "GSB " is the first chunk's tag.
	append_chunk(out, "GSB ", build_init_payload());
	append_chunk(out, "FLDS", build_fields_payload());
	append_chunk(out, "SVRS", build_servers_payload(servers));
	append_chunk(out, "XXXX", {});

	return out;
}

} // namespace opennova
