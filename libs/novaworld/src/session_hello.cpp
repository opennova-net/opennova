#include <novaworld/session_hello.h>

#include <cstring>

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
	while (len > 0 && data[len - 1] == 0) {
		--len;
	}
	return std::string(reinterpret_cast<const char *>(data), len);
}

uint32_t read_u32_le(const uint8_t *data, size_t len) {
	if (len < 4) return 0;
	return static_cast<uint32_t>(data[0]) |
			(static_cast<uint32_t>(data[1]) << 8) |
			(static_cast<uint32_t>(data[2]) << 16) |
			(static_cast<uint32_t>(data[3]) << 24);
}

} // namespace

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
		if (name == "CI") out.ci = read_u32_le(value, size);
		else if (name == "HK") out.hk = read_u32_le(value, size);
		else if (name == "CK") out.ck = read_u32_le(value, size);
		else if (name == "NA") out.na = strip_nul(value, size);
		else if (name == "SIP") out.sip = read_u32_le(value, size);
		else if (name == "SPN") out.spn = read_u32_le(value, size);
		else if (name == "SCRK") out.scrk = strip_nul(value, size);
		else if (name == "CU") out.cu.emplace_back(value, value + size);
		// Ignore anything else (jodemo re-sends NVS/CO/AP/BDAT/PN/PG/... here
		// too; onnet's parser doesn't consume them and neither do we).
		pos = next;
	}
	// Minimum sanity: CK should be nonzero for a valid ClientAuth.
	return out.ck != 0;
}

std::vector<uint8_t> client_auth_to_bytes(const ClientAuth &msg) {
	std::vector<uint8_t> buf;
	buf.reserve(256);
	append_u32_field(buf, "CI",  msg.ci);
	append_u32_field(buf, "HK",  msg.hk);
	append_u32_field(buf, "CK",  msg.ck);
	if (!msg.na.empty()) append_string_field(buf, "NA", msg.na);
	append_u32_field(buf, "SIP", msg.sip);
	append_u32_field(buf, "SPN", msg.spn);
	if (!msg.scrk.empty()) append_string_field(buf, "SCRK", msg.scrk);
	for (const auto &blob : msg.cu) {
		append_bytes_field(buf, "CU", blob.data(), blob.size());
	}
	return buf;
}

// Values copied from onnet's nwu_protocol.py (CLIENT_CS_FIELD_VALUES /
// SERVER_CS_FIELD_VALUES). [UNVERIFIED — from onnet, not IDA]: the
// binary's CS builder has not yet been located; confirm via xref to the
// `CS` literal once we have a bigger capture corpus.
std::vector<CsField> default_client_cs_fields() {
	return {
		{0, 240000u}, {1, 4u}, {2, 0u}, {3, 0u},
		{4, 30000u}, {5, 1000u}, {6, 0xFFFFFFFFu}, {7, 0u},
		{8, 4096u}, {9, 4u}, {10, 32u}, {11, 500u},
		{12, 1u}, {13, 1000u}, {14, 0xFFFFFFFFu},
	};
}

std::vector<CsField> default_server_cs_fields() {
	return {
		{0, 240000u}, {1, 4u}, {2, 0u}, {3, 0u},
		{4, 30000u}, {5, 1000u}, {6, 0xFFFFFFFFu}, {7, 0u},
		{8, 4096u}, {9, 4u}, {10, 32u}, {11, 500u},
		{12, 4u}, {13, 1000u}, {14, 0xFFFFFFFFu},
	};
}

ServerAuth build_server_auth(const ClientAuth &client,
                             uint32_t client_ip_net,
                             uint16_t client_port,
                             uint32_t server_sk,
                             std::string_view server_scrk,
                             std::string_view novaworld_name,
                             std::string_view novaworld_web_url,
                             std::string_view nwuid) {
	ServerAuth a;
	a.ci = client.ci;
	a.ck = client.ck;
	a.sk = server_sk;
	a.client_cs = default_client_cs_fields();
	a.server_cs = default_server_cs_fields();
	a.cu.emplace_back("NovaworldName", std::string(novaworld_name));
	a.cu.emplace_back("NovaworldWebDomainNameAndPortNumber", std::string(novaworld_web_url));
	a.cu.emplace_back("NWUID", std::string(nwuid));
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
	append_u32_field(buf, "RIP", msg.rip);
	append_u32_field(buf, "RPN", msg.rpn);
	return buf;
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
	append_u32_field(buf, "UT", msg.ut);
	append_string_field(buf, "PN", msg.pn);
	append_bytes_field(buf, "PG", msg.pg.data(), msg.pg.size());
	append_string_field(buf, "PV1", msg.pv1);
	append_string_field(buf, "PV2", msg.pv2);
	append_string_field(buf, "PV3", msg.pv3);
	append_u32_field(buf, "HK", msg.hk);
	append_string_field(buf, "SN", msg.sn);
	if (msg.is_game_server) {
		// Game-server ServerHello shape witnessed in retail capture frame
		// 62334 (notes/retail_capture2_decoded.txt:599-623). Client-side
		// processor name is not in our kong-named IDB yet (search xrefs
		// to the literal field-name strings via the parse_napi_tlvs path
		// from `Crypto_DecryptBuffer@0x5E5230` callers; the ClientHello
		// parser at `parse_client_hello` / `NapiNPSession_SendDescription@0x5E9840`
		// is the analog mirror). Field meanings observed across captured
		// matches:
		//   SF = server flags (retail = 0)
		//   P1 = gametype bitmask (0x10000 = AS gametype; low 0x10 bit
		//        unwitnessed — likely a per-mission feature flag, not a
		//        terrain identifier)
		//   P2 = game config (purpose unwitnessed; retail = 0x404)
		//   NP = current player count
		//   MP = max-players or game-mode parameter (retail = 2)
		// Game-server hello REPLACES PL with these — PL is gone.
		append_u32_field(buf, "SF", msg.sf);
		append_u32_field(buf, "P1", msg.p1);
		append_u32_field(buf, "P2", msg.p2);
		append_u32_field(buf, "NP", msg.np);
		append_u32_field(buf, "MP", msg.mp);
	} else {
		// Matchmaking hello on :64206 keeps PL (witnessed in retail capture
		// frame 3608 / line 29-47 of retail_capture2_decoded.txt — no
		// SF/P1/P2/NP/MP, no SUS1/SUS2). Same TLV-parser at the client side,
		// just different field set seen by upstream consumers.
		append_string_field(buf, "PL", msg.pl);
	}
	append_u32_field(buf, "NC", msg.nc);
	append_u32_field(buf, "RIP", msg.rip);
	append_u32_field(buf, "RPN", msg.rpn);
	if (msg.is_game_server) {
		// SUS1 = unique session id (GSID-NN-XXXXXXXX-timestamp-hash).
		// SUS2 = expansion-pack archive name. 'jox01' is the Kendari
		// expansion pack — one of its terrain datasets is `dvxi5`, used by
		// the ASH_I5A.bms mission. The terrain itself is loaded from
		// `Dvxi5.{trn,cpt,...}`; SUS2 just tells the client which expansion
		// archive holds the assets.
		// Both witnessed in retail capture frame 62334.
		append_string_field(buf, "SUS1", msg.sus1);
		append_string_field(buf, "SUS2", msg.sus2);
	}
	append_u32_field(buf, "EIP", msg.eip);
	append_u32_field(buf, "EPN", msg.epn);
	return buf;
}

} // namespace opennova
