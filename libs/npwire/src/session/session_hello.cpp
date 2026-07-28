#include <npwire/session_hello.h>

#include <cstring>
#include <napi/tlv.h>

namespace opennova {

namespace {

// Parse one flat-TLV field at offset `pos`. Returns the next offset, or
// npos on malformed input.
size_t read_tlv_field(const uint8_t *data, size_t len, size_t pos,
                      std::string &out_name, const uint8_t *&out_value, uint16_t &out_size) {
	if (pos >= len) {
		return static_cast<size_t>(-1);
	}
	// Name: null-terminated ASCII.
	size_t p = pos;
	while (p < len && data[p] != 0) {
		++p;
	}
	if (p >= len) {
		return static_cast<size_t>(-1);
	}
	out_name.assign(reinterpret_cast<const char *>(data + pos), p - pos);
	++p; // skip null
	if (p + 2 > len) {
		return static_cast<size_t>(-1);
	}
	out_size = static_cast<uint16_t>(data[p]) | (static_cast<uint16_t>(data[p + 1]) << 8);
	p += 2;
	if (p + out_size > len) {
		return static_cast<size_t>(-1);
	}
	out_value = data + p;
	return p + out_size;
}

void append_name(std::vector<uint8_t> &buf, const char *name) {
	while (*name) {
		buf.push_back(static_cast<uint8_t>(*name++));
	}
	buf.push_back(0);
}

void append_size(std::vector<uint8_t> &buf, uint16_t sz) {
	buf.push_back(static_cast<uint8_t>(sz & 0xFFu));
	buf.push_back(static_cast<uint8_t>((sz >> 8) & 0xFFu));
}

// Append a string field. onnet's build_tlv appends the ASCII bytes plus a
// trailing NUL, with the size reflecting string.len + 1.
void append_string_field(std::vector<uint8_t> &buf, const char *name, const std::string &value) {
	append_name(buf, name);
	const uint16_t sz = static_cast<uint16_t>(value.size() + 1);
	append_size(buf, sz);
	buf.insert(buf.end(), value.begin(), value.end());
	buf.push_back(0);
}

// Append a uint32 field as 4 LE bytes.
void append_u32_field(std::vector<uint8_t> &buf, const char *name, uint32_t value) {
	append_name(buf, name);
	append_size(buf, 4);
	buf.push_back(static_cast<uint8_t>(value & 0xFFu));
	buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

// Append a raw byte field.
void append_bytes_field(std::vector<uint8_t> &buf, const char *name, const uint8_t *data, size_t len) {
	append_name(buf, name);
	append_size(buf, static_cast<uint16_t>(len));
	buf.insert(buf.end(), data, data + len);
}

std::string strip_nul(const uint8_t *data, size_t len) {
	return opennova::field_to_string(data, len);
}

uint32_t read_u32_le(const uint8_t *data, size_t len) {
	if (len < 4) return 0;
	return static_cast<uint32_t>(data[0]) |
			(static_cast<uint32_t>(data[1]) << 8) |
			(static_cast<uint32_t>(data[2]) << 16) |
			(static_cast<uint32_t>(data[3]) << 24);
}

} // namespace

std::array<uint8_t, 16> jointoperations_protocol_guid() {
	// QUuid(0xB074D646, 0x81F9, 0x475F,
	//       0x92,0xDA,0xDE,0xA7,0x24,0x7F,0x14,0x68)
	// in the retail static initializer at 0x7937a0. QUuid's first three
	// components occupy native little-endian memory; PG copies those 16 bytes.
	return {0x46, 0xD6, 0x74, 0xB0, 0xF9, 0x81, 0x5F, 0x47,
	        0x92, 0xDA, 0xDE, 0xA7, 0x24, 0x7F, 0x14, 0x68};
}

namespace {
// ASCII case-insensitive compare — the retail identity gate compares every string
// field through Napi_StrCaseEqual, not exact equality.
// [orig: Napi_StrCaseEqual @0x616e70, used by HandleClientJoin @0x62B750]
bool str_case_equal(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) !=
		    std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	}
	return true;
}
} // namespace

bool is_jointoperations_protocol_name(std::string_view protocol_name) {
	// The retail PN compare is case-insensitive, which subsumes the two spellings the
	// established NovaWorld PN router accepted; keep the policy in the neutral
	// game-wire layer so npruntime does not reimplement service routing.
	return str_case_equal(protocol_name, "JOINTOPERATIONS");
}

ClientHello make_jointoperations_client_hello(uint32_t client_index) {
	ClientHello hello;
	hello.nvs = "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic";
	hello.co = "NovaLogic Inc, Calabasas CA U.S.A.";
	hello.ap = "Jointops.exe";
	hello.bdat = "Jul 21 2009 18:54:42";
	hello.pn = "JOINTOPERATIONS";
	hello.pg = jointoperations_protocol_guid();
	hello.pg_present = true;
	hello.pv1 = "0.0.0 1/12/2004 EM";
	hello.pv2 = "16";
	hello.ci = client_index;
	return hello;
}

ClientAuth make_jointoperations_client_auth(uint32_t client_index, uint32_t client_key,
		uint32_t host_key, std::string_view player_name, std::string_view client_scrk) {
	const ClientHello hello = make_jointoperations_client_hello(client_index);
	ClientAuth auth;
	auth.nvs = hello.nvs;
	auth.co = hello.co;
	auth.ap = hello.ap;
	auth.bdat = hello.bdat;
	auth.pn = hello.pn;
	auth.pg = hello.pg;
	auth.pg_present = true;
	auth.pv1 = hello.pv1;
	auth.pv2 = hello.pv2;
	auth.ci = client_index;
	auth.hk = host_key;
	auth.ck = client_key;
	auth.na = std::string(player_name);
	auth.scrk = std::string(client_scrk);
	return auth;
}

// String fields compare case-insensitively like the witnessed gate; the PG GUID stays a
// raw 16-byte compare. [orig: HandleClientJoin @0x62B750 via Napi_StrCaseEqual @0x616e70]
bool matches_jointoperations_identity(const ClientHello &hello) {
	const ClientHello expected = make_jointoperations_client_hello(hello.ci);
	return str_case_equal(hello.nvs, expected.nvs) &&
			is_jointoperations_protocol_name(hello.pn) &&
			hello.pg_present && hello.pg == expected.pg &&
			str_case_equal(hello.pv1, expected.pv1);
}

bool matches_jointoperations_identity(const ClientAuth &auth) {
	const ClientHello expected = make_jointoperations_client_hello(auth.ci);
	return str_case_equal(auth.nvs, expected.nvs) &&
			is_jointoperations_protocol_name(auth.pn) &&
			auth.pg_present && auth.pg == expected.pg &&
			str_case_equal(auth.pv1, expected.pv1) &&
			str_case_equal(auth.pv2, expected.pv2);
}

bool parse_client_hello(const uint8_t *data, size_t len, ClientHello &out) {
	if (!data) return false;
	out = ClientHello{};
	size_t pos = 0;
	while (pos < len) {
		std::string name;
		const uint8_t *value = nullptr;
		uint16_t size = 0;
		const size_t next = read_tlv_field(data, len, pos, name, value, size);
		if (next == static_cast<size_t>(-1)) {
			// Tolerant: if we've parsed at least a few fields, accept partial.
			break;
		}
		if (name == "NVS") out.nvs = strip_nul(value, size);
		else if (name == "CO") out.co = strip_nul(value, size);
		else if (name == "AP") out.ap = strip_nul(value, size);
		else if (name == "BDAT") out.bdat = strip_nul(value, size);
		else if (name == "PN") out.pn = strip_nul(value, size);
		else if (name == "PG" && size == 16) {
			std::memcpy(out.pg.data(), value, 16);
			out.pg_present = true;
		} else if (name == "PV1") out.pv1 = strip_nul(value, size);
		else if (name == "PV2") out.pv2 = strip_nul(value, size);
		else if (name == "CI") out.ci = read_u32_le(value, size);
		else if (name == "EIP") out.eip = read_u32_le(value, size);
		else if (name == "EPN") out.epn = read_u32_le(value, size);
		// Unknown tags intentionally ignored.
		pos = next;
	}
	return !out.pn.empty();
}

std::vector<uint8_t> client_hello_to_bytes(const ClientHello &msg) {
	std::vector<uint8_t> buf;
	buf.reserve(256);
	// Order mirrors the ClientHello struct declaration.
	if (!msg.nvs.empty())  append_string_field(buf, "NVS",  msg.nvs);
	if (!msg.co.empty())   append_string_field(buf, "CO",   msg.co);
	if (!msg.ap.empty())   append_string_field(buf, "AP",   msg.ap);
	if (!msg.bdat.empty()) append_string_field(buf, "BDAT", msg.bdat);
	append_string_field(buf, "PN", msg.pn);
	if (msg.pg_present) {
		append_bytes_field(buf, "PG", msg.pg.data(), msg.pg.size());
	}
	if (!msg.pv1.empty()) append_string_field(buf, "PV1", msg.pv1);
	if (!msg.pv2.empty()) append_string_field(buf, "PV2", msg.pv2);
	append_u32_field(buf, "CI",  msg.ci);
	append_u32_field(buf, "EIP", msg.eip);
	append_u32_field(buf, "EPN", msg.epn);
	return buf;
}

ServerHello build_server_hello(const ClientHello &client,
                               uint32_t client_ip_net,
                               uint16_t client_port) {
	ServerHello s;
	s.ci = client.ci;
	s.pn = client.pn.empty() ? std::string("NOVAWORLDUDP") : client.pn;
	if (client.pg_present) {
		s.pg = client.pg;
	}
	s.pv1 = client.pv1.empty() ? s.pv1 : client.pv1;
	s.pv2 = client.pv2.empty() ? s.pv2 : client.pv2;
	s.rip = client_ip_net;
	s.rpn = client_port;
	s.eip = client.eip;
	s.epn = client.epn;
	return s;
}

// ---- ClientAuth / ServerAuth -------------------------------------------

bool parse_client_auth(const uint8_t *data, size_t len, ClientAuth &out) {
	if (!data) return false;
	out = ClientAuth{};
	size_t pos = 0;
	while (pos < len) {
		std::string name;
		const uint8_t *value = nullptr;
		uint16_t size = 0;
		const size_t next = read_tlv_field(data, len, pos, name, value, size);
		if (next == static_cast<size_t>(-1)) break;
		// Identity block — the real server validates these in HandleClientJoin
		// @ 0x62B750 (NVS/PN/PG/PV1 + PV2); parse them so the round-trip is
		// exact and our own server records what the client claimed.
		if      (name == "NVS")  out.nvs  = strip_nul(value, size);
		else if (name == "CO")   out.co   = strip_nul(value, size);
		else if (name == "AP")   out.ap   = strip_nul(value, size);
		else if (name == "BDAT") out.bdat = strip_nul(value, size);
		else if (name == "PN")   out.pn   = strip_nul(value, size);
		else if (name == "PG" && size == 16) {
			std::memcpy(out.pg.data(), value, 16);
			out.pg_present = true;
		}
		else if (name == "PV1")  out.pv1  = strip_nul(value, size);
		else if (name == "PV2")  out.pv2  = strip_nul(value, size);
		// Auth fields.
		else if (name == "CI")   out.ci   = read_u32_le(value, size);
		else if (name == "HK")   out.hk   = read_u32_le(value, size);
		else if (name == "CK")   out.ck   = read_u32_le(value, size);
		else if (name == "NA")   out.na   = strip_nul(value, size);
		else if (name == "SIP")  out.sip  = read_u32_le(value, size);
		else if (name == "SPN")  out.spn  = read_u32_le(value, size);
		else if (name == "SCRK") out.scrk = strip_nul(value, size);
		else if (name == "CU")   out.cu.emplace_back(value, value + size);
		// Unknown tags (DE/PV3/PW/NF/DCNT/RCNT/etc.) intentionally ignored.
		pos = next;
	}
	// Minimum sanity: CK should be nonzero for a valid ClientAuth.
	return out.ck != 0;
}

std::vector<uint8_t> client_auth_to_bytes(const ClientAuth &msg) {
	std::vector<uint8_t> buf;
	buf.reserve(512);
	// Identity block first, in retail's 0x42 order (NapiNPConnection_
	// SendClientHello @ 0x61fe20). NVS/PN/PG/PV1/PV2 are validated by the real
	// server in HandleClientJoin @ 0x62B750; CO/AP/BDAT are free but retail
	// still emits them. Omit a string tag when empty / PG when absent, exactly
	// as the retail builder gates each NapiNP_WriteTLV on a non-empty field.
	if (!msg.nvs.empty())  append_string_field(buf, "NVS",  msg.nvs);
	if (!msg.co.empty())   append_string_field(buf, "CO",   msg.co);
	if (!msg.ap.empty())   append_string_field(buf, "AP",   msg.ap);
	if (!msg.bdat.empty()) append_string_field(buf, "BDAT", msg.bdat);
	if (!msg.pn.empty())   append_string_field(buf, "PN",   msg.pn);
	if (msg.pg_present)    append_bytes_field(buf, "PG", msg.pg.data(), msg.pg.size());
	if (!msg.pv1.empty())  append_string_field(buf, "PV1",  msg.pv1);
	if (!msg.pv2.empty())  append_string_field(buf, "PV2",  msg.pv2);
	// Auth fields. Retail (NapiNPConnection_SendClientHello @ 0x61fe20) gates
	// CI (node+20), HK (node+1488), CK (node+332), SIP (node+48) and SPN
	// (node+52) each on non-zero — emit only when set, to byte-match the 0x42.
	// [orig: NapiNPConnection_SendClientHello @ 0x61fe20]. SCRK comes after CU.
	if (msg.ci) append_u32_field(buf, "CI", msg.ci);
	if (msg.hk) append_u32_field(buf, "HK", msg.hk);
	if (msg.ck) append_u32_field(buf, "CK", msg.ck);
	if (!msg.na.empty()) append_string_field(buf, "NA", msg.na);
	if (msg.sip) append_u32_field(buf, "SIP", msg.sip);
	if (msg.spn) append_u32_field(buf, "SPN", msg.spn);
	for (const auto &blob : msg.cu) {
		append_bytes_field(buf, "CU", blob.data(), blob.size());
	}
	if (!msg.scrk.empty()) append_string_field(buf, "SCRK", msg.scrk);
	return buf;
}

// [orig: CNapiNPConnection_SendDisconnectPacket @0x61f2a0]. The key dword is written raw ahead
// of the TLV stream; every TLV is written unconditionally (DSTR/DDSTR ship their NUL even when
// empty), and the receiver (Nwu_HandleDisconnect @0x623ce0) walks them case-insensitively with
// zero defaults, validating only the leading key dword.
std::vector<uint8_t> client_goodbye_to_bytes(uint32_t remote_session_key) {
	std::vector<uint8_t> buf;
	buf.reserve(64);
	buf.push_back(static_cast<uint8_t>(remote_session_key & 0xFFu));
	buf.push_back(static_cast<uint8_t>((remote_session_key >> 8) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((remote_session_key >> 16) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((remote_session_key >> 24) & 0xFFu));
	append_u32_field(buf, "DS", 0);  // role — the receiver discards it and re-derives locally
	append_u32_field(buf, "DC", 0);  // reason code: 0 = ordinary leave
	append_u32_field(buf, "DP1", 0);
	append_u32_field(buf, "DP2", 0);
	append_string_field(buf, "DSTR", std::string());
	append_u32_field(buf, "DPC", 0);
	append_string_field(buf, "DDSTR", std::string());
	return buf;
}

// [orig: CNapiNPConnection_HandleDescriptionPacket @0x621ae0]. The retail walk reads name-keyed
// TLVs in whatever order they arrive, compares each name through Napi_StrCaseEqual, ignores names
// it does not know (the cursor has already skipped their value by its length), and stops at an
// empty name. DS is read off the wire and deliberately dropped: the receiver re-derives the role
// from its own connection. A value shorter than its field's width reads as zero here; retail's
// unguarded `*(_DWORD *)value` would read past it, and bounds safety is a platform primitive.
bool parse_disconnect_event(const uint8_t *data, size_t len, DisconnectEvent &out) {
	if (!data) return false;
	out = DisconnectEvent{};
	bool saw_known_field = false;
	size_t pos = 0;
	while (pos < len) {
		// An empty name terminates the walk BEFORE its length is read @0x621b96.
		if (data[pos] == 0) break;
		std::string name;
		const uint8_t *value = nullptr;
		uint16_t size = 0;
		const size_t next = read_tlv_field(data, len, pos, name, value, size);
		if (next == static_cast<size_t>(-1)) return false;
		if (str_case_equal(name, "DS")) {
			out.ds = read_u32_le(value, size);
		} else if (str_case_equal(name, "DC")) {
			out.dc = read_u32_le(value, size);
		} else if (str_case_equal(name, "DP1")) {
			out.dp1 = read_u32_le(value, size);
		} else if (str_case_equal(name, "DP2")) {
			out.dp2 = read_u32_le(value, size);
		} else if (str_case_equal(name, "DSTR")) {
			out.dstr = strip_nul(value, size);
		} else if (str_case_equal(name, "DPC")) {
			out.dpc = read_u32_le(value, size);
		} else if (str_case_equal(name, "DDSTR")) {
			out.ddstr = strip_nul(value, size);
		} else {
			pos = next;
			continue;
		}
		saw_known_field = true;
		pos = next;
	}
	return saw_known_field;
}

std::vector<uint8_t> make_client_cu_chunk(uint8_t type, std::string_view name,
                                          std::string_view value) {
	// [type:1B][name + NUL][LE16 data_len][value + NUL], data_len = value.size()+1.
	std::vector<uint8_t> blob;
	blob.reserve(1 + name.size() + 1 + 2 + value.size() + 1);
	blob.push_back(type);
	blob.insert(blob.end(), name.begin(), name.end());
	blob.push_back(0);
	const uint16_t data_len = static_cast<uint16_t>(value.size() + 1);
	blob.push_back(static_cast<uint8_t>(data_len & 0xFFu));
	blob.push_back(static_cast<uint8_t>((data_len >> 8) & 0xFFu));
	blob.insert(blob.end(), value.begin(), value.end());
	blob.push_back(0);
	return blob;
}

bool parse_client_cu_chunk(const uint8_t *data, size_t len, uint8_t &out_type,
                           std::string &out_name, std::string &out_value) {
	if (!data || len < 1) return false;
	out_type = data[0];
	size_t p = 1;
	const size_t name_start = p;
	while (p < len && data[p] != 0) ++p;
	if (p >= len) return false; // no name NUL
	out_name.assign(reinterpret_cast<const char *>(data + name_start), p - name_start);
	++p; // skip name NUL
	if (p + 2 > len) return false;
	const uint16_t data_len = static_cast<uint16_t>(data[p]) |
			(static_cast<uint16_t>(data[p + 1]) << 8);
	p += 2;
	if (p + data_len > len) return false;
	out_value = strip_nul(data + p, data_len);
	return true;
}

// Engine CS template, witnessed in IDA: CNapiGameSession_InitNPConnection writes
// two IDENTICAL 15-entry [field_index]=timeout_ms blocks (dir1 @ proto+3652,
// dir0 @ proto+3712); NapiNPConnection_Create copies them into the connection and
// SendSessionInit emits cs_dirN[i].timeout_ms verbatim. Both directions are
// identical — there is NO client/server difference at index 12. index13 is the
// runtime MTU (dword_25509F0, clamp 100..0x10000, default 1300).
// [orig: CNapiGameSession_InitNPConnection @ 0x4d3e1f / NapiNPConnection_Create @ 0x62acb0 / NapiNPConnection_SendSessionInit @ 0x620ef0]
// (Prior values were onnet-derived guesses, wrong at idx 4/8/9/10/12/13 —
//  docs/net/novaworld-net-re.md D-NET-1.)
static std::vector<CsField> engine_cs_fields() {
	return {
		{0, 240000u}, {1, 4u}, {2, 0u}, {3, 0u},
		{4, 60000u}, {5, 1000u}, {6, 0xFFFFFFFFu}, {7, 0u},
		{8, 2048u}, {9, 128u}, {10, 100u}, {11, 500u},
		{12, 1u}, {13, 1300u}, {14, 0xFFFFFFFFu},
	};
}

std::vector<CsField> default_client_cs_fields() { return engine_cs_fields(); }
std::vector<CsField> default_server_cs_fields() { return engine_cs_fields(); }

ServerAuth build_server_auth(const ClientAuth &client,
                             uint32_t client_ip_net,
                             uint16_t client_port,
                             uint32_t server_sk,
                             std::string_view server_scrk,
                             std::string_view novaworld_name,
                             std::string_view novaworld_web_url,
                             std::string_view nwuid,
                             bool include_novaworld_cu) {
	ServerAuth a;
	a.ci = client.ci;
	a.ck = client.ck;
	a.sk = server_sk;
	a.client_cs = default_client_cs_fields();
	a.server_cs = default_server_cs_fields();
	// [D-NET Wave 3] CU is sourced from the host's type-3 msg_out queue (@0x620ef0); only the NovaWorld
	// lobby flow populates it. A LAN/SP host queues none -> emits NO CU (verified vs the retail LAN golden).
	if (include_novaworld_cu) {
		a.cu.emplace_back("NovaworldName", std::string(novaworld_name));
		a.cu.emplace_back("NovaworldWebDomainNameAndPortNumber", std::string(novaworld_web_url));
		a.cu.emplace_back("NWUID", std::string(nwuid));
	}
	a.scrk = std::string(server_scrk);
	a.na = client.na;
	a.rip = client_ip_net;
	a.rpn = client_port;
	return a;
}

namespace {

void append_cs_field(std::vector<uint8_t> &buf, uint8_t direction, const CsField &f) {
	// TLV name "CS" + 6-byte value: [direction][field_index][LE uint32].
	append_name(buf, "CS");
	append_size(buf, 6);
	buf.push_back(direction);
	buf.push_back(f.field_index);
	buf.push_back(static_cast<uint8_t>(f.value & 0xFFu));
	buf.push_back(static_cast<uint8_t>((f.value >> 8) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((f.value >> 16) & 0xFFu));
	buf.push_back(static_cast<uint8_t>((f.value >> 24) & 0xFFu));
}

void append_cu_field(std::vector<uint8_t> &buf, const std::string &name, const std::string &value) {
	// Inner format: 0x03 <name>\0 <LE16 value_len_including_trailing_NUL> <value>\0
	std::vector<uint8_t> inner;
	inner.push_back(0x03);
	inner.insert(inner.end(), name.begin(), name.end());
	inner.push_back(0);
	const uint16_t vlen = static_cast<uint16_t>(value.size() + 1);
	inner.push_back(static_cast<uint8_t>(vlen & 0xFFu));
	inner.push_back(static_cast<uint8_t>((vlen >> 8) & 0xFFu));
	inner.insert(inner.end(), value.begin(), value.end());
	inner.push_back(0);
	append_name(buf, "CU");
	append_size(buf, static_cast<uint16_t>(inner.size()));
	buf.insert(buf.end(), inner.begin(), inner.end());
}

} // namespace

std::vector<uint8_t> server_auth_to_bytes(const ServerAuth &msg) {
	std::vector<uint8_t> buf;
	buf.reserve(512);
	append_u32_field(buf, "CI", msg.ci);
	append_u32_field(buf, "MI", msg.mi);
	append_u32_field(buf, "CK", msg.ck);
	append_u32_field(buf, "CR", msg.cr);
	if (msg.jfc) append_u32_field(buf, "JFC", msg.jfc);
	if (msg.jfp) append_u32_field(buf, "JFP", msg.jfp);
	if (!msg.jfs.empty()) append_string_field(buf, "JFS", msg.jfs);
	append_u32_field(buf, "SK", msg.sk);
	for (const auto &f : msg.client_cs) {
		append_cs_field(buf, /*direction=*/1, f);
	}
	for (const auto &f : msg.server_cs) {
		append_cs_field(buf, /*direction=*/0, f);
	}
	for (const auto &pair : msg.cu) {
		append_cu_field(buf, pair.first, pair.second);
	}
	append_string_field(buf, "SCRK", msg.scrk);
	append_string_field(buf, "NA", msg.na);
	// RIP/RPN gated on non-zero, mirroring SIP/SPN: SendSessionInit emits RIP
	// only when peer_addr!=0 and RPN only when peer_port!=0.
	// [orig: NapiNPConnection_SendSessionInit @ 0x620ef0 (@ 0x62121e / 0x621242)]
	if (msg.rip) append_u32_field(buf, "RIP", msg.rip);
	if (msg.rpn) append_u32_field(buf, "RPN", msg.rpn);
	return buf;
}

namespace {

// Decode one CU inner blob: `0x03 <name>\0 <LE16 value_len_incl_NUL>
// <value>\0` (inverse of append_cu_field). Tolerant — returns false if the
// shape doesn't hold so the caller can skip it.
bool parse_cu_inner(const uint8_t *data, size_t len,
                    std::string &out_name, std::string &out_value) {
	if (len < 1 || data[0] != 0x03) return false;
	size_t p = 1;
	const size_t name_start = p;
	while (p < len && data[p] != 0) ++p;
	if (p >= len) return false; // no NUL terminator
	out_name.assign(reinterpret_cast<const char *>(data + name_start), p - name_start);
	++p; // skip name NUL
	if (p + 2 > len) return false;
	const uint16_t vlen = static_cast<uint16_t>(data[p]) |
			(static_cast<uint16_t>(data[p + 1]) << 8);
	p += 2;
	if (p + vlen > len) return false;
	out_value = strip_nul(data + p, vlen);
	return true;
}

} // namespace

bool parse_server_auth(const uint8_t *data, size_t len, ServerAuth &out) {
	if (!data) return false;
	out = ServerAuth{};
	out.client_cs.clear();
	out.server_cs.clear();
	out.cu.clear();
	bool saw_cr = false;
	bool saw_rejection_detail = false;
	size_t pos = 0;
	while (pos < len) {
		std::string name;
		const uint8_t *value = nullptr;
		uint16_t size = 0;
		const size_t next = read_tlv_field(data, len, pos, name, value, size);
		if (next == static_cast<size_t>(-1)) break;
		if      (name == "CI")  out.ci  = read_u32_le(value, size);
		else if (name == "MI")  out.mi  = read_u32_le(value, size);
		else if (name == "CK")  out.ck  = read_u32_le(value, size);
		else if (name == "CR")  { out.cr  = read_u32_le(value, size); saw_cr = true; }
		else if (name == "JFC") { out.jfc = read_u32_le(value, size); saw_rejection_detail = true; }
		else if (name == "JFP") { out.jfp = read_u32_le(value, size); saw_rejection_detail = true; }
		else if (name == "JFS") { out.jfs = strip_nul(value, size); saw_rejection_detail = true; }
		else if (name == "SK")  out.sk  = read_u32_le(value, size);
		else if (name == "CS" && size == 6) {
			// [direction][field_index][LE uint32]. direction 1 = client, 0 = server.
			const uint8_t direction = value[0];
			CsField f{value[1], read_u32_le(value + 2, 4)};
			if (direction == 1) out.client_cs.push_back(f);
			else                out.server_cs.push_back(f);
		}
		else if (name == "CU") {
			std::string cu_name, cu_value;
			if (parse_cu_inner(value, size, cu_name, cu_value)) {
				out.cu.emplace_back(std::move(cu_name), std::move(cu_value));
			}
		}
		else if (name == "SCRK") out.scrk = strip_nul(value, size);
		else if (name == "NA")   out.na   = strip_nul(value, size);
		else if (name == "RIP")  out.rip  = read_u32_le(value, size);
		else if (name == "RPN")  out.rpn  = read_u32_le(value, size);
		// Unknown tags intentionally ignored.
		pos = next;
	}
	// Accepted ServerAuth carries SCRK for subsequent traffic. Rejected joins
	// legitimately omit SCRK and instead carry JFC/JFP/JFS details.
	return !out.scrk.empty() || (saw_cr && out.cr != 1 && saw_rejection_detail);
}

// ---- ServerHello serializer (restored below) ---------------------------

std::vector<uint8_t> server_hello_to_bytes(const ServerHello &msg) {
	std::vector<uint8_t> buf;
	buf.reserve(256);
	// Emit in the same order as onnet's to_dict so the on-wire ordering
	// matches what clients tend to expect.
	append_u32_field(buf, "CI", msg.ci);
	append_string_field(buf, "CO", msg.co);
	append_string_field(buf, "AP", msg.ap);
	append_string_field(buf, "BDAT", msg.bdat);
	// [orig: NapiNPProtocol_SendServerInfoPacket @ 0x6204b0]
	if (msg.ut) append_u32_field(buf, "UT", msg.ut);
	append_string_field(buf, "PN", msg.pn);
	append_bytes_field(buf, "PG", msg.pg.data(), msg.pg.size());
	append_string_field(buf, "PV1", msg.pv1);
	append_string_field(buf, "PV2", msg.pv2);
	append_string_field(buf, "PV3", msg.pv3);
	append_u32_field(buf, "HK", msg.hk);
	append_string_field(buf, "SN", msg.sn);
	// [D-NET-16/17/18] Flat single-branch builder, faithful to the witnessed
	// [orig: NapiNPProtocol_SendServerInfoPacket @0x6204b0] (the 0x81 reply to a 0x41 ClientHello):
	//   - SF is emitted UNCONDITIONALLY (the original always writes it = `log_buffer[0] != 0`).
	//   - P1 (server_flags) / P2 (build_flags) / NP (np_count) / MP (max_players) each ONLY when nonzero
	//     (the original individually gates every count field; there is no all-or-nothing block).
	//   - There is NO "PL" tag in this builder — the earlier game-server-vs-matchmaking two-branch was a
	//     fiction (PL never appears on the 0x81 wire; the gate :64206 matchmaking hello is a SEPARATE
	//     sender). The parser below stays lenient (it can still read a stray PL/SF from a foreign
	//     capture) so decoders keep working; only the ENCODER is made faithful.
	// Field meanings (cross-checked vs the retail-lan-host-join golden): P1 = gametype bitmask, P2 = game
	// config, NP = current player count, MP = max-players/game-mode parameter.
	append_u32_field(buf, "SF", msg.sf);
	if (msg.p1) append_u32_field(buf, "P1", msg.p1);
	if (msg.p2) append_u32_field(buf, "P2", msg.p2);
	if (msg.np) append_u32_field(buf, "NP", msg.np);
	if (msg.mp) append_u32_field(buf, "MP", msg.mp);
	append_u32_field(buf, "NC", msg.nc);
	append_u32_field(buf, "RIP", msg.rip);
	append_u32_field(buf, "RPN", msg.rpn);
	// SUS1 (unique session id GSID-NN-...) / SUS2 (expansion-pack archive, e.g. 'jox01' = Kendari) —
	// the original gates each on a NON-EMPTY string, not on a game-server toggle (frame 62334).
	if (!msg.sus1.empty()) append_string_field(buf, "SUS1", msg.sus1);
	if (!msg.sus2.empty()) append_string_field(buf, "SUS2", msg.sus2);
	append_u32_field(buf, "EIP", msg.eip);
	append_u32_field(buf, "EPN", msg.epn);
	return buf;
}

bool parse_server_hello(const uint8_t *data, size_t len, ServerHello &out) {
	if (!data) return false;
	out = ServerHello{};
	// Gated fields that are absent on the wire parse as absent. Keep this
	// explicit so future builder defaults cannot leak into decoded discovery
	// rows (for example, an empty dedicated host omits NP).
	out.pl.clear();
	out.p1 = 0;
	out.p2 = 0;
	out.np = 0;
	out.mp = 0;
	out.sus1.clear();
	out.sus2.clear();
	bool saw_hk = false;
	size_t pos = 0;
	while (pos < len) {
		std::string name;
		const uint8_t *value = nullptr;
		uint16_t size = 0;
		const size_t next = read_tlv_field(data, len, pos, name, value, size);
		if (next == static_cast<size_t>(-1)) break;
		if      (name == "CI")   out.ci   = read_u32_le(value, size);
		else if (name == "CO")   out.co   = strip_nul(value, size);
		else if (name == "AP")   out.ap   = strip_nul(value, size);
		else if (name == "BDAT") out.bdat = strip_nul(value, size);
		else if (name == "UT")   out.ut   = read_u32_le(value, size);
		else if (name == "PN")   out.pn   = strip_nul(value, size);
		else if (name == "PG" && size == 16) std::memcpy(out.pg.data(), value, 16);
		else if (name == "PV1")  out.pv1  = strip_nul(value, size);
		else if (name == "PV2")  out.pv2  = strip_nul(value, size);
		else if (name == "PV3")  out.pv3  = strip_nul(value, size);
		else if (name == "HK") { out.hk = read_u32_le(value, size); saw_hk = true; }
		else if (name == "SN")   out.sn   = strip_nul(value, size);
		else if (name == "PL")   out.pl   = strip_nul(value, size);
		else if (name == "SF") { out.sf = read_u32_le(value, size); out.is_game_server = true; }
		else if (name == "P1")   out.p1   = read_u32_le(value, size);
		else if (name == "P2")   out.p2   = read_u32_le(value, size);
		else if (name == "NP")   out.np   = read_u32_le(value, size);
		else if (name == "MP")   out.mp   = read_u32_le(value, size);
		else if (name == "NC")   out.nc   = read_u32_le(value, size);
		else if (name == "RIP")  out.rip  = read_u32_le(value, size);
		else if (name == "RPN")  out.rpn  = read_u32_le(value, size);
		else if (name == "SUS1") out.sus1 = strip_nul(value, size);
		else if (name == "SUS2") out.sus2 = strip_nul(value, size);
		else if (name == "EIP")  out.eip  = read_u32_le(value, size);
		else if (name == "EPN")  out.epn  = read_u32_le(value, size);
		// Unknown tags intentionally ignored.
		pos = next;
	}
	// The host key is the one field the client must echo back in ClientAuth.
	return saw_hk;
}

} // namespace opennova
