#include <net/npwire/session_hello.h>

#include <cstring>
#include <base/io/le.h>
#include <base/io/strutil.h>
#include <net/napi/tlv.h>
#include <net/npwire/flat_tlv.h>

namespace opennova {

namespace {

// The field writers over the one flat-TLV grammar (npwire/flat_tlv.h). A
// string field carries its NUL inside the size (string.len + 1).
void append_string_field(std::vector<uint8_t> &buf, const char *name, const std::string &value) {
	append_flat_tlv(buf, name, reinterpret_cast<const uint8_t *>(value.c_str()),
			static_cast<uint16_t>(value.size() + 1));
}

// A uint32 field as 4 LE bytes.
void append_u32_field(std::vector<uint8_t> &buf, const char *name, uint32_t value) {
	uint8_t le[4];
	io::write_u32_le(le, value);
	append_flat_tlv(buf, name, le, 4);
}

// A raw byte field.
void append_bytes_field(std::vector<uint8_t> &buf, const char *name, const uint8_t *data, size_t len) {
	append_flat_tlv(buf, name, data, static_cast<uint16_t>(len));
}

std::string strip_nul(const uint8_t *data, size_t len) {
	return opennova::field_to_string(data, len);
}

uint32_t read_u32_le(const uint8_t *data, size_t len) {
	if (len < 4) return 0;
	return io::read_u32_le(data);
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

bool is_jointoperations_protocol_name(std::string_view protocol_name) {
	// The retail PN compare is case-insensitive, which subsumes the two spellings the
	// established NovaWorld PN router accepted; keep the policy in the neutral
	// game-wire layer so inmatch does not reimplement service routing. The retail
	// identity gate compares every string field through Napi_StrCaseEqual, not exact
	// equality. [orig: Napi_StrCaseEqual @0x616e70, used by HandleClientJoin @0x62B750]
	return strutil::iequals(protocol_name, "JOINTOPERATIONS");
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
	return strutil::iequals(hello.nvs, expected.nvs) &&
			is_jointoperations_protocol_name(hello.pn) &&
			hello.pg_present && hello.pg == expected.pg &&
			strutil::iequals(hello.pv1, expected.pv1);
}

bool matches_jointoperations_identity(const ClientAuth &auth) {
	const ClientHello expected = make_jointoperations_client_hello(auth.ci);
	return strutil::iequals(auth.nvs, expected.nvs) &&
			is_jointoperations_protocol_name(auth.pn) &&
			auth.pg_present && auth.pg == expected.pg &&
			strutil::iequals(auth.pv1, expected.pv1);
}

// [orig: NapiNPProtocol_HandleClientHello @0x6213b0 — the PM TLV lands in
//  `protocol_version` @0x62172c; `cmp edi, ebx / jnz loc_6218B6` @0x6217bc..
//  0x6217c2 jumps PAST the NVS/PN/PG/PV1 compares straight to the
//  SendServerInfoPacket call @0x6218fc, so only a zero PM is validated]
bool client_hello_admits(const ClientHello &hello) {
	return hello.pm != 0 || matches_jointoperations_identity(hello);
}

bool parse_client_hello(const uint8_t *data, size_t len, ClientHello &out) {
	if (!data) return false;
	out = ClientHello{};
	size_t pos = 0;
	while (pos < len) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(data, len, pos, field);
		if (next == kFlatTlvEnd) {
			// The retail walk exits on the first read failure and keeps what
			// it gathered.
			break;
		}
		const std::string_view name = field.name;
		const uint8_t *value = field.value;
		const uint16_t size = field.size;
		if (strutil::iequals(name, "NVS")) out.nvs = strip_nul(value, size);
		else if (strutil::iequals(name, "CO")) out.co = strip_nul(value, size);
		else if (strutil::iequals(name, "AP")) out.ap = strip_nul(value, size);
		else if (strutil::iequals(name, "BDAT")) out.bdat = strip_nul(value, size);
		else if (strutil::iequals(name, "DE")) out.de = read_u32_le(value, size);
		else if (strutil::iequals(name, "PN")) out.pn = strip_nul(value, size);
		else if (strutil::iequals(name, "PG") && size == 16) {
			std::memcpy(out.pg.data(), value, 16);
			out.pg_present = true;
		} else if (strutil::iequals(name, "PV1")) out.pv1 = strip_nul(value, size);
		else if (strutil::iequals(name, "PV2")) out.pv2 = strip_nul(value, size);
		else if (strutil::iequals(name, "PV3")) out.pv3 = strip_nul(value, size);
		else if (strutil::iequals(name, "CI")) out.ci = read_u32_le(value, size);
		// [orig: NapiNPProtocol_HandleClientHello @0x6213B0 — PM dword @0x62173C,
		//  zero-initialised @0x621504]
		else if (strutil::iequals(name, "PM")) out.pm = read_u32_le(value, size);
		else if (strutil::iequals(name, "EIP")) out.eip = read_u32_le(value, size);
		else if (strutil::iequals(name, "EPN")) out.epn = read_u32_le(value, size);
		else if (strutil::iequals(name, "ET")) out.et = read_u32_le(value, size);
		// Unknown tags intentionally ignored.
		pos = next;
	}
	// No tag is required: retail validates the version block only when PM is
	// zero (client_hello_admits), so an empty PN is a parse result, not a
	// parse failure.
	return true;
}

std::vector<uint8_t> client_hello_to_bytes(const ClientHello &msg) {
	std::vector<uint8_t> buf;
	buf.reserve(256);
	// Order mirrors the ClientHello struct declaration.
	if (!msg.nvs.empty())  append_string_field(buf, "NVS",  msg.nvs);
	if (!msg.co.empty())   append_string_field(buf, "CO",   msg.co);
	if (!msg.ap.empty())   append_string_field(buf, "AP",   msg.ap);
	if (!msg.bdat.empty()) append_string_field(buf, "BDAT", msg.bdat);
	if (msg.de != 0) append_u32_field(buf, "DE", msg.de);
	if (!msg.pn.empty()) append_string_field(buf, "PN", msg.pn);
	if (msg.pg_present) {
		append_bytes_field(buf, "PG", msg.pg.data(), msg.pg.size());
	}
	if (!msg.pv1.empty()) append_string_field(buf, "PV1", msg.pv1);
	if (!msg.pv2.empty()) append_string_field(buf, "PV2", msg.pv2);
	if (!msg.pv3.empty()) append_string_field(buf, "PV3", msg.pv3);
	if (msg.ci != 0) append_u32_field(buf, "CI", msg.ci);
	// PM only for a nonzero count: retail's announce builder has no "present but
	// zero" state [orig: NapiNPSession_SendAnnouncePacket @0x61fa00 —
	// `!transport_info[37]` @0x61fcca, then `if (count)` @0x61fcda].
	if (msg.pm != 0) append_u32_field(buf, "PM", msg.pm);
	if (msg.eip != 0) append_u32_field(buf, "EIP", msg.eip);
	if (msg.epn != 0) append_u32_field(buf, "EPN", msg.epn);
	if (msg.et != 0) append_u32_field(buf, "ET", msg.et);
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
	// The external-address triple is the caller's echo of the hello
	// [orig: SendServerInfoPacket(.., external_ip, external_port, external_type)].
	s.eip = client.eip;
	s.epn = client.epn;
	s.et = client.et;
	return s;
}

// ---- ClientAuth / ServerAuth -------------------------------------------

bool parse_client_auth(const uint8_t *data, size_t len, ClientAuth &out) {
	if (!data) return false;
	out = ClientAuth{};
	size_t pos = 0;
	while (pos < len) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(data, len, pos, field);
		if (next == kFlatTlvEnd) break;
		const std::string_view name = field.name;
		const uint8_t *value = field.value;
		const uint16_t size = field.size;
		// Identity block — the real server validates these in HandleClientJoin
		// @ 0x62B750 (NVS/PN/PG/PV1 + PV2); parse them so the round-trip is
		// exact and our own server records what the client claimed. Every tag
		// compares case-insensitively, as the retail walk does
		// [orig: Napi_StrCaseEqual(tag, "NVS") @0x62b915 .. "SCRK" @0x62bc30].
		if      (strutil::iequals(name, "NVS"))  out.nvs  = strip_nul(value, size);
		else if (strutil::iequals(name, "CO"))   out.co   = strip_nul(value, size);
		else if (strutil::iequals(name, "AP"))   out.ap   = strip_nul(value, size);
		else if (strutil::iequals(name, "BDAT")) out.bdat = strip_nul(value, size);
		else if (strutil::iequals(name, "PN"))   out.pn   = strip_nul(value, size);
		else if (strutil::iequals(name, "PG") && size == 16) {
			std::memcpy(out.pg.data(), value, 16);
			out.pg_present = true;
		}
		else if (strutil::iequals(name, "PV1"))  out.pv1  = strip_nul(value, size);
		else if (strutil::iequals(name, "PV2"))  out.pv2  = strip_nul(value, size);
		// Auth fields.
		else if (strutil::iequals(name, "CI"))   out.ci   = read_u32_le(value, size);
		else if (strutil::iequals(name, "HK"))   out.hk   = read_u32_le(value, size);
		else if (strutil::iequals(name, "CK"))   out.ck   = read_u32_le(value, size);
		else if (strutil::iequals(name, "NA"))   out.na   = strip_nul(value, size).substr(0, 63);
		else if (strutil::iequals(name, "PW"))   out.pw   = strip_nul(value, size).substr(0, 511);
		else if (strutil::iequals(name, "SIP"))  out.sip  = read_u32_le(value, size);
		else if (strutil::iequals(name, "SPN"))  out.spn  = read_u32_le(value, size);
		else if (strutil::iequals(name, "SCRK")) out.scrk = strip_nul(value, size);
		else if (strutil::iequals(name, "CU"))   out.cu.emplace_back(value, value + size);
		// Unknown tags (DE/PV3/NF/DCNT/RCNT/etc.) intentionally ignored.
		pos = next;
	}
	// Minimum sanity: CK should be nonzero for a valid ClientAuth.
	return out.ck != 0;
}

std::vector<uint8_t> client_auth_to_bytes(const ClientAuth &msg) {
	std::vector<uint8_t> buf;
	buf.reserve(512);
	// Identity block first, in retail's 0x42 order (CNapiNPConnection_
	// SendClientJoin @ 0x61fe20). NVS/PN/PG/PV1/PV2 are validated by the real
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
	// Auth fields. Retail (CNapiNPConnection_SendClientJoin @ 0x61fe20) gates
	// CI (node+20), HK (node+1488), CK (node+332), SIP (node+48) and SPN
	// (node+52) each on non-zero — emit only when set, to byte-match the 0x42.
	// [orig: CNapiNPConnection_SendClientJoin @ 0x61fe20]. SCRK comes after CU.
	if (msg.ci) append_u32_field(buf, "CI", msg.ci);
	if (msg.hk) append_u32_field(buf, "HK", msg.hk);
	if (msg.ck) append_u32_field(buf, "CK", msg.ck);
	if (!msg.na.empty()) append_string_field(buf, "NA", msg.na);
	if (!msg.pw.empty()) append_string_field(buf, "PW", msg.pw);
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
std::vector<uint8_t> connection_description_to_bytes(
		const DisconnectEvent &event) {
	std::vector<uint8_t> buf;
	buf.reserve(80);
	append_u32_field(buf, "DS", event.ds);
	append_u32_field(buf, "DC", event.dc);
	append_u32_field(buf, "DP1", event.dp1);
	append_u32_field(buf, "DP2", event.dp2);
	append_string_field(buf, "DSTR", event.dstr);
	append_u32_field(buf, "DPC", event.dpc);
	append_string_field(buf, "DDSTR", event.ddstr);
	return buf;
}

// [orig: CNapiNPConnection_SendDisconnectPacket @0x61f2a0]: the receiver-local key dword raw
// @0x61f3af, then the latched record's seven TLVs in DS/DC/DP1/DP2/DSTR/DPC/DDSTR order
// @0x61f3d0..0x61f4aa — the same writer serves 0x46 (is_client) and 0x86 (is_server); only the
// opcode differs (@0x61f367/@0x61f37b).
std::vector<uint8_t> disconnect_packet_body_to_bytes(uint32_t peer_key,
		const DisconnectEvent &event) {
	std::vector<uint8_t> buf;
	buf.reserve(96);
	io::append_u32_le(buf, peer_key);
	const std::vector<uint8_t> record = connection_description_to_bytes(event);
	buf.insert(buf.end(), record.begin(), record.end());
	return buf;
}

std::vector<uint8_t> client_goodbye_to_bytes(uint32_t remote_session_key,
		const DisconnectEvent &event) {
	return disconnect_packet_body_to_bytes(remote_session_key, event);
}

std::vector<uint8_t> client_goodbye_to_bytes(uint32_t remote_session_key) {
	return disconnect_packet_body_to_bytes(remote_session_key, DisconnectEvent{});
}

std::vector<uint8_t> server_goodbye_to_bytes(uint32_t client_ck, const DisconnectEvent &event) {
	return disconnect_packet_body_to_bytes(client_ck, event);
}

// [orig: CNapiNPConnection_HandleDescriptionPacket @0x621ae0]. The retail walk reads name-keyed
// TLVs in whatever order they arrive, compares each name through Napi_StrCaseEqual, ignores names
// it does not know (the cursor has already skipped their value by its length), and stops at an
// empty name or at the first field NapiNP_ReadTLV cannot read (@0x621b8c) — keeping every field
// gathered before it. Nothing after the walk tests what was parsed: the description is recorded
// with zero defaults and the connection moves to state 6 (@0x621d59), so an unknown-only or
// truncated body disconnects exactly like a complete one. DS is read off the wire and
// deliberately dropped: the receiver re-derives the role from its own connection. A value shorter
// than its field's width reads as zero here; retail's unguarded `*(_DWORD *)value` would read past
// it, and bounds safety is a platform primitive.
bool parse_disconnect_event(const uint8_t *data, size_t len, DisconnectEvent &out) {
	if (!data) return false;
	out = DisconnectEvent{};
	size_t pos = 0;
	while (pos < len) {
		// An empty name terminates the walk BEFORE its length is read @0x621b96.
		if (data[pos] == 0) break;
		FlatTlvField field;
		const size_t next = read_flat_tlv(data, len, pos, field);
		if (next == kFlatTlvEnd) break;
		const std::string_view name = field.name;
		const uint8_t *value = field.value;
		const uint16_t size = field.size;
		if (strutil::iequals(name, "DS")) {
			out.ds = read_u32_le(value, size);
		} else if (strutil::iequals(name, "DC")) {
			out.dc = read_u32_le(value, size);
		} else if (strutil::iequals(name, "DP1")) {
			out.dp1 = read_u32_le(value, size);
		} else if (strutil::iequals(name, "DP2")) {
			out.dp2 = read_u32_le(value, size);
		} else if (strutil::iequals(name, "DSTR")) {
			out.dstr = strip_nul(value, size);
		} else if (strutil::iequals(name, "DPC")) {
			out.dpc = read_u32_le(value, size);
		} else if (strutil::iequals(name, "DDSTR")) {
			out.ddstr = strip_nul(value, size);
		}
		pos = next;
	}
	return true;
}

std::vector<uint8_t> make_client_cu_chunk(uint8_t type, std::string_view name,
                                          std::string_view value) {
	// [type:1B][name + NUL][LE16 data_len][value + NUL], data_len = value.size()+1.
	std::vector<uint8_t> blob;
	blob.reserve(1 + name.size() + 1 + 2 + value.size() + 1);
	blob.push_back(type);
	blob.insert(blob.end(), name.begin(), name.end());
	blob.push_back(0);
	io::append_u16_le(blob, static_cast<uint16_t>(value.size() + 1));
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
	const uint16_t data_len = io::read_u16_le(data + p);
	p += 2;
	if (p + data_len > len) return false;
	out_value = strip_nul(data + p, data_len);
	return true;
}

// Field 13: the configured `mpmaxpacketsize` folded through the retail clamp
// ladder. Both template builders run the same three SIGNED compares against
// the game.cfg value (a negative value lands on the floor); only the ceiling
// differs.
// [orig: CNapiNetwork_Init @0x4ca4a0 — `if (!v) v = 1300` @0x4caa53,
//  `if (v < 100) v = 100` @0x4caa5f (`jge` @0x4caa66), `if (v > 0x4000)
//  v = 0x4000` @0x4caa6a (`jle` @0x4caa74); CNapiGameSession_InitNPConnection
//  @0x4d3be0 — the same ladder @0x4d3df4 (`jge` @0x4d3e07, `jle` @0x4d3e15)
//  with 0x10000 @0x4d3e0b]
uint32_t cs_max_packet_bytes(int32_t configured, int32_t ceiling) {
	int32_t v = configured;
	if (v == 0) v = kCsMaxPacketDefault;
	else if (v < kCsMaxPacketFloor) v = kCsMaxPacketFloor;
	else if (v > ceiling) v = ceiling;
	return static_cast<uint32_t>(v);
}

// The NOVAWORLDUDP service template, witnessed in IDA:
// CNapiGameSession_InitNPConnection writes two IDENTICAL 15-entry
// [field_index]=timeout_ms blocks (dir1 @ proto+3652, dir0 @ proto+3712);
// CNapiNPConnection_Create copies them into the connection and SendSessionInit
// emits cs_dirN[i].timeout_ms verbatim. Both directions are identical — there
// is NO client/server difference at index 12. index13 is the runtime MTU
// (dword_25509F0, clamp 100..0x10000, default 1300).
// [orig: CNapiGameSession_InitNPConnection @ 0x4d3e1f / CNapiNPConnection_Create @ 0x62acb0 / CNapiNPConnection_SendSessionInit @ 0x620ef0]
// (Prior values were onnet-derived guesses, wrong at idx 4/8/9/10/12/13 —
//  docs/net/novaworld-net-re.md D-NET-1.)
std::vector<CsField> novaworld_service_cs_fields(int32_t max_packet_bytes) {
	return {
		{0, 240000u}, {1, 4u}, {2, 0u}, {3, 0u},
		{4, 60000u}, {5, 1000u}, {6, 0xFFFFFFFFu}, {7, 0u},
		{8, 2048u}, {9, 128u}, {10, 100u}, {11, 500u},
		{12, 1u}, {13, cs_max_packet_bytes(max_packet_bytes, kCsMaxPacketCeilingService)},
		{14, 0xFFFFFFFFu},
	};
}

// The JOINTOPERATIONS in-game template — what a retail GAME host's 0x82 carries.
// CNapiNetwork_Init writes it into the game session's protocol object at
// +0xE44 (dir 0) / +0xE80 (dir 1) and CNapiNPConnection_Create copies the
// fifteen dwords into every connection (`rep movsd ecx=0Fh` @0x62ae7b/
// @0x62aeb8), which SendSessionInit @0x620ef0 emits verbatim. The block above
// is the NOVAWORLDUDP SERVICE protocol's (CNapiGameSession_InitNPConnection
// @0x4d3be0 writes it into the object it creates with PN "NOVAWORLDUDP"), so a
// game host advertising it handed retail joiners a 240 s dead-host timeout, a
// 60 s idle keepalive and a 1 s active probe (D-NET-1's closure read the wrong
// object; corrected 2026-09-10, jo-c cross-check).
// [orig: CNapiNetwork_Init @0x4ca4a0 — 120000 @0x4caa81/@0x4cab54, 4 @0x4caa90/
//  @0x4cab60, 30000 @0x4caab5/@0x4cab88, 10000 @0x4caac5/@0x4cab98, -1, 0, 512
//  @0x4caaf0/@0x4cabc0, 256 @0x4cab00/@0x4cabd0, 100 @0x4cab10, 1200 (0x4B0)
//  @0x4cab20, 1 @0x4cab2c, MTU @0x4cab3c, -1 @0x4cab48; dir 1 identical]
std::vector<CsField> jointoperations_cs_fields(int32_t max_packet_bytes) {
	return {
		{0, 120000u}, {1, 4u}, {2, 0u}, {3, 0u},
		{4, 30000u}, {5, 10000u}, {6, 0xFFFFFFFFu}, {7, 0u},
		{8, 512u}, {9, 256u}, {10, 100u}, {11, 1200u},
		{12, 1u}, {13, cs_max_packet_bytes(max_packet_bytes, kCsMaxPacketCeilingGame)},
		{14, 0xFFFFFFFFu},
	};
}

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
	a.client_cs = novaworld_service_cs_fields();
	a.server_cs = novaworld_service_cs_fields();
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
	uint8_t value[6];
	value[0] = direction;
	value[1] = f.field_index;
	io::write_u32_le(value + 2, f.value);
	append_flat_tlv(buf, "CS", value, 6);
}

void append_cu_field(std::vector<uint8_t> &buf, const std::string &name, const std::string &value) {
	// Inner format: 0x03 <name>\0 <LE16 value_len_including_trailing_NUL> <value>\0
	std::vector<uint8_t> inner;
	inner.push_back(0x03);
	inner.insert(inner.end(), name.begin(), name.end());
	inner.push_back(0);
	io::append_u16_le(inner, static_cast<uint16_t>(value.size() + 1));
	inner.insert(inner.end(), value.begin(), value.end());
	inner.push_back(0);
	append_flat_tlv(buf, "CU", inner.data(), static_cast<uint16_t>(inner.size()));
}

} // namespace

std::vector<uint8_t> server_auth_rejection_to_bytes(const ServerAuth &msg) {
	// The rejection 0x82 is a different, smaller builder than the accept form:
	// exactly CI, CK, CR (=0), JFC, JFP, and JFS only when non-empty — no MI,
	// SK, CS run, CU, SCRK, NA, or reflected address. [orig:
	// NapiNPProtocol_SendJoinRejection @0x620cd0 (ex "SendDrawOverlay")]
	std::vector<uint8_t> buf;
	buf.reserve(96);
	append_u32_field(buf, "CI", msg.ci);
	append_u32_field(buf, "CK", msg.ck);
	append_u32_field(buf, "CR", msg.cr);
	append_u32_field(buf, "JFC", msg.jfc);
	append_u32_field(buf, "JFP", msg.jfp);
	if (!msg.jfs.empty()) append_string_field(buf, "JFS", msg.jfs);
	return buf;
}

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
	// [orig: CNapiNPConnection_SendSessionInit @ 0x620ef0 (@ 0x62121e / 0x621242)]
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
	const uint16_t vlen = io::read_u16_le(data + p);
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
		FlatTlvField field;
		const size_t next = read_flat_tlv(data, len, pos, field);
		if (next == kFlatTlvEnd) break;
		const std::string_view name = field.name;
		const uint8_t *value = field.value;
		const uint16_t size = field.size;
		// Case-insensitive tag walk, as the retail 0x82 reader does
		// [orig: NapiNP_HandleServerJoinResponse @0x629840 via Napi_StrCaseEqual @0x616e70].
		if      (strutil::iequals(name, "CI"))  out.ci  = read_u32_le(value, size);
		else if (strutil::iequals(name, "MI"))  out.mi  = read_u32_le(value, size);
		else if (strutil::iequals(name, "CK"))  out.ck  = read_u32_le(value, size);
		else if (strutil::iequals(name, "CR"))  { out.cr  = read_u32_le(value, size); saw_cr = true; }
		else if (strutil::iequals(name, "JFC")) { out.jfc = read_u32_le(value, size); saw_rejection_detail = true; }
		else if (strutil::iequals(name, "JFP")) { out.jfp = read_u32_le(value, size); saw_rejection_detail = true; }
		else if (strutil::iequals(name, "JFS")) { out.jfs = strip_nul(value, size); saw_rejection_detail = true; }
		else if (strutil::iequals(name, "SK"))  out.sk  = read_u32_le(value, size);
		else if (strutil::iequals(name, "CS") && size == 6) {
			// [direction][field_index][LE uint32]. direction 1 = client, 0 = server.
			const uint8_t direction = value[0];
			CsField f{value[1], read_u32_le(value + 2, 4)};
			if (direction == 1) out.client_cs.push_back(f);
			else                out.server_cs.push_back(f);
		}
		else if (strutil::iequals(name, "CU")) {
			std::string cu_name, cu_value;
			if (parse_cu_inner(value, size, cu_name, cu_value)) {
				out.cu.emplace_back(std::move(cu_name), std::move(cu_value));
			}
		}
		else if (strutil::iequals(name, "SCRK")) out.scrk = strip_nul(value, size);
		else if (strutil::iequals(name, "NA"))   out.na   = strip_nul(value, size);
		else if (strutil::iequals(name, "RIP"))  out.rip  = read_u32_le(value, size);
		else if (strutil::iequals(name, "RPN"))  out.rpn  = read_u32_le(value, size);
		// Unknown tags intentionally ignored.
		pos = next;
	}
	// Accepted ServerAuth carries SCRK for subsequent traffic. Rejected joins
	// legitimately omit SCRK and instead carry JFC/JFP/JFS details.
	return !out.scrk.empty() || (saw_cr && out.cr != 1 && saw_rejection_detail);
}

// ---- ServerHello serializer (restored below) ---------------------------

namespace {

bool guid_is_null(const std::array<uint8_t, 16> &pg) {
	for (const uint8_t b : pg) {
		if (b != 0) return false;
	}
	return true;
}

} // namespace

// The 0x81 reply to a 0x41 ClientHello — a flat, single-branch builder that
// writes CI, HK, SN and SF unconditionally and gates everything else on the
// field itself: strings on a non-empty first byte, dwords on nonzero, PG on
// a non-null GUID, and the CN/LNG/TZB trio on its own enable. There is NO
// "PL" tag (the earlier game-server-vs-matchmaking two-branch was a
// fiction; the gate :64206 matchmaking hello is a separate sender). Field
// meanings (cross-checked vs the retail-lan-host-join golden): SF =
// `log_buffer[0] != 0`, P1 = gametype bitmask, P2 = build flags, NP =
// current player count, MP = max players; SUS1 = the GSID the NovaWorld
// host-verify reply handed the host, SUS2 = the expansion archive.
// [orig: NapiNPProtocol_SendServerInfoPacket @0x6204b0 — CI @0x62057e, CO
//  @0x6205b1, AP @0x6205ea, BDAT @0x620620, DE @0x620647, UT @0x620683, PN
//  @0x6206ba, PG (NapiGUID_IsNull gate) @0x6206e4, PV1 @0x62071a, PV2
//  @0x620750, PV3 @0x62078a, HK @0x6207ad, SN @0x6207da, CN/LNG/TZB block
//  @0x6207e2..0x62086a, SF @0x620898, P1..P8 @0x6208bf..0x6209d0, NP
//  @0x6209f7, MP @0x620a1e, NPW @0x620a45, NC @0x620a6c, RIP @0x620a94, RPN
//  @0x620abc, SUS1..SUS4 @0x620af2..0x620b9a, EIP @0x620bc0, EPN @0x620be6,
//  ET @0x620c0c]
std::vector<uint8_t> server_hello_to_bytes(const ServerHello &msg) {
	std::vector<uint8_t> buf;
	buf.reserve(256);
	append_u32_field(buf, "CI", msg.ci);
	if (!msg.co.empty())   append_string_field(buf, "CO", msg.co);
	if (!msg.ap.empty())   append_string_field(buf, "AP", msg.ap);
	if (!msg.bdat.empty()) append_string_field(buf, "BDAT", msg.bdat);
	if (msg.de) append_u32_field(buf, "DE", msg.de);
	if (msg.ut) append_u32_field(buf, "UT", msg.ut);
	if (!msg.pn.empty())   append_string_field(buf, "PN", msg.pn);
	if (!guid_is_null(msg.pg)) append_bytes_field(buf, "PG", msg.pg.data(), msg.pg.size());
	if (!msg.pv1.empty())  append_string_field(buf, "PV1", msg.pv1);
	if (!msg.pv2.empty())  append_string_field(buf, "PV2", msg.pv2);
	if (!msg.pv3.empty())  append_string_field(buf, "PV3", msg.pv3);
	append_u32_field(buf, "HK", msg.hk);
	append_string_field(buf, "SN", msg.sn);
	if (msg.locale_block) {
		// Inside the block the two strings are written unconditionally (an
		// empty one ships its NUL) and TZB always follows.
		append_string_field(buf, "CN", msg.cn);
		append_string_field(buf, "LNG", msg.lng);
		append_u32_field(buf, "TZB", msg.tzb);
	}
	append_u32_field(buf, "SF", msg.sf);
	if (msg.p1) append_u32_field(buf, "P1", msg.p1);
	if (msg.p2) append_u32_field(buf, "P2", msg.p2);
	if (msg.p3) append_u32_field(buf, "P3", msg.p3);
	if (msg.p4) append_u32_field(buf, "P4", msg.p4);
	if (msg.p5) append_u32_field(buf, "P5", msg.p5);
	if (msg.p6) append_u32_field(buf, "P6", msg.p6);
	if (msg.p7) append_u32_field(buf, "P7", msg.p7);
	if (msg.p8) append_u32_field(buf, "P8", msg.p8);
	if (msg.np) append_u32_field(buf, "NP", msg.np);
	if (msg.mp) append_u32_field(buf, "MP", msg.mp);
	if (msg.npw) append_u32_field(buf, "NPW", msg.npw);
	if (msg.nc) append_u32_field(buf, "NC", msg.nc);
	if (msg.rip) append_u32_field(buf, "RIP", msg.rip);
	if (msg.rpn) append_u32_field(buf, "RPN", msg.rpn);
	if (!msg.sus1.empty()) append_string_field(buf, "SUS1", msg.sus1);
	if (!msg.sus2.empty()) append_string_field(buf, "SUS2", msg.sus2);
	if (!msg.sus3.empty()) append_string_field(buf, "SUS3", msg.sus3);
	if (!msg.sus4.empty()) append_string_field(buf, "SUS4", msg.sus4);
	if (msg.eip) append_u32_field(buf, "EIP", msg.eip);
	if (msg.epn) append_u32_field(buf, "EPN", msg.epn);
	if (msg.et) append_u32_field(buf, "ET", msg.et);
	return buf;
}

// [orig: Nwu_HandleServerHello @0x626d20 — every tag compared through
//  Napi_StrCaseEqual, zero defaults, the walk stops at an empty name]
bool parse_server_hello(const uint8_t *data, size_t len, ServerHello &out) {
	if (!data) return false;
	// The reader zeroes EVERY field before the walk, so an absent tag reads as
	// zero/empty: clear exactly the builder defaults ServerHello{} carries (the
	// numeric fields and the other strings already default to zero/empty).
	// [orig: Nwu_HandleServerHello @0x626d20 — every field zeroed before the walk
	//  @0x626E3B..0x626F20]
	out = ServerHello{};
	out.co.clear();
	out.ap.clear();
	out.bdat.clear();
	out.pn.clear();
	out.pv1.clear();
	out.pv2.clear();
	out.pv3.clear();
	out.hk = 0;
	out.sn.clear();
	bool saw_hk = false;
	size_t pos = 0;
	while (pos < len) {
		FlatTlvField field;
		const size_t next = read_flat_tlv(data, len, pos, field);
		if (next == kFlatTlvEnd) break;
		if (field.name.empty()) break;
		const std::string_view name = field.name;
		const uint8_t *value = field.value;
		const uint16_t size = field.size;
		if      (strutil::iequals(name, "CI"))   out.ci   = read_u32_le(value, size);
		else if (strutil::iequals(name, "CO"))   out.co   = strip_nul(value, size);
		else if (strutil::iequals(name, "AP"))   out.ap   = strip_nul(value, size);
		else if (strutil::iequals(name, "BDAT")) out.bdat = strip_nul(value, size);
		else if (strutil::iequals(name, "DE"))   out.de   = read_u32_le(value, size);
		else if (strutil::iequals(name, "UT"))   out.ut   = read_u32_le(value, size);
		else if (strutil::iequals(name, "PN"))   out.pn   = strip_nul(value, size);
		else if (strutil::iequals(name, "PG") && size == 16) std::memcpy(out.pg.data(), value, 16);
		else if (strutil::iequals(name, "PV1"))  out.pv1  = strip_nul(value, size);
		else if (strutil::iequals(name, "PV2"))  out.pv2  = strip_nul(value, size);
		else if (strutil::iequals(name, "PV3"))  out.pv3  = strip_nul(value, size);
		else if (strutil::iequals(name, "HK")) { out.hk = read_u32_le(value, size); saw_hk = true; }
		else if (strutil::iequals(name, "SN"))   out.sn   = strip_nul(value, size);
		else if (strutil::iequals(name, "CN")) { out.cn = strip_nul(value, size); out.locale_block = true; }
		else if (strutil::iequals(name, "LNG")) { out.lng = strip_nul(value, size); out.locale_block = true; }
		else if (strutil::iequals(name, "TZB")) { out.tzb = read_u32_le(value, size); out.locale_block = true; }
		else if (strutil::iequals(name, "SF")) { out.sf = read_u32_le(value, size); out.is_game_server = true; }
		else if (strutil::iequals(name, "P1"))   out.p1   = read_u32_le(value, size);
		else if (strutil::iequals(name, "P2"))   out.p2   = read_u32_le(value, size);
		else if (strutil::iequals(name, "P3"))   out.p3   = read_u32_le(value, size);
		else if (strutil::iequals(name, "P4"))   out.p4   = read_u32_le(value, size);
		else if (strutil::iequals(name, "P5"))   out.p5   = read_u32_le(value, size);
		else if (strutil::iequals(name, "P6"))   out.p6   = read_u32_le(value, size);
		else if (strutil::iequals(name, "P7"))   out.p7   = read_u32_le(value, size);
		else if (strutil::iequals(name, "P8"))   out.p8   = read_u32_le(value, size);
		else if (strutil::iequals(name, "NP"))   out.np   = read_u32_le(value, size);
		else if (strutil::iequals(name, "MP"))   out.mp   = read_u32_le(value, size);
		else if (strutil::iequals(name, "NPW"))  out.npw  = read_u32_le(value, size);
		else if (strutil::iequals(name, "NC"))   out.nc   = read_u32_le(value, size);
		else if (strutil::iequals(name, "RIP"))  out.rip  = read_u32_le(value, size);
		else if (strutil::iequals(name, "RPN"))  out.rpn  = read_u32_le(value, size);
		else if (strutil::iequals(name, "SUS1")) out.sus1 = strip_nul(value, size);
		else if (strutil::iequals(name, "SUS2")) out.sus2 = strip_nul(value, size);
		else if (strutil::iequals(name, "SUS3")) out.sus3 = strip_nul(value, size);
		else if (strutil::iequals(name, "SUS4")) out.sus4 = strip_nul(value, size);
		else if (strutil::iequals(name, "EIP"))  out.eip  = read_u32_le(value, size);
		else if (strutil::iequals(name, "EPN"))  out.epn  = read_u32_le(value, size);
		else if (strutil::iequals(name, "ET"))   out.et   = read_u32_le(value, size);
		// Unknown tags intentionally ignored.
		pos = next;
	}
	// The host key is the one field the client must echo back in ClientAuth.
	return saw_hk;
}

} // namespace opennova
