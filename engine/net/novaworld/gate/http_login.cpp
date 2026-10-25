#include <net/novaworld/http_login.h>

#include <net/novacrypto/epask.h>
#include <net/novacrypto/url_cipher.h>
#include <net/novaworld/gate_response.h> // parse_ipv4_octets (Network_ParseIPv4AddressOctets)

#include <base/io/crt_ftol.h>
#include <base/io/log.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

namespace opennova {

std::string subnet_key(const std::string &host) {
	// The key is the host read as a C string. A host the dotted-quad parse accepts comes
	// back whole: no end pointer is passed, so a tail after the fourth octet is never
	// looked at, and an octet past 255 parses (its low byte kept).
	// [orig: Network_TruncateIPToSubnet @0x62DFE0: the parse @0x62DFF5, the `jnz` @0x62DFFF]
	std::string key = host.c_str();
	std::array<uint8_t, 4> octets{};
	if (parse_ipv4_octets(key, octets)) return key;
	// Any other host is reversed, cut at its second '.' and reversed back, keeping its
	// last two dot-labels; with no dot or one it is reversed back whole, unchanged.
	// [orig: Buffer_ReverseBytes @0x61B290 (@0x62E01D); Napi_StrStr @0x6173B0, a plain
	//  strstr for "." (@0x62E028, then past the first dot @0x62E060); the cut @0x62E088;
	//  the reverses back @0x62E04D / @0x62E07E / @0x62E09D]
	std::reverse(key.begin(), key.end());
	const size_t first_dot = key.find('.');
	if (first_dot != std::string::npos) {
		const size_t second_dot = key.find('.', first_dot + 1);
		if (second_dot != std::string::npos) key.resize(second_dot);
	}
	std::reverse(key.begin(), key.end());
	return key;
}

bool build_login_post_body(const EpaskParams &pub, const std::vector<LoginFormField> &fields,
                           std::string &body) {
	body.clear();
	std::string encrypted;
	for (const auto &f : fields) {
		if (!body.empty()) body.push_back('&');
		body += f.name;
		body.push_back('=');
		if (!f.encrypt) {
			body += f.value;
		} else if (epask_encrypt(f.value, pub, encrypted)) {
			body += encrypted;
		} else {
			body.clear();
			return false;
		}
	}
	return true;
}

bool build_credentials_post_body(
    const EpaskParams &pub, const std::string &name, const std::string &password,
    std::string &body, const std::vector<std::pair<std::string, std::string>> &hidden) {
	std::vector<LoginFormField> fields;
	fields.reserve(hidden.size() + 3);
	// The echoed public key the server uses to decrypt the rest of the body.
	fields.push_back({"EPASK", epask_to_string(pub), false});
	// The two EDIT-widget credentials, encrypted under the bundle.
	fields.push_back({"NAME", name, true});
	fields.push_back({"PASSWORD", password, true});
	// IB3_FORM passthrough fields, plaintext, in caller order.
	for (const auto &kv : hidden) {
		fields.push_back({kv.first, kv.second, false});
	}
	return build_login_post_body(pub, fields, body);
}

namespace {

// The CRT isspace of the "C" locale: space, then tab through carriage return.
bool crt_isspace(char c) {
	return c == ' ' || (c >= '\t' && c <= '\r');
}

// The header text as a C string: a byte at or past its end reads as its terminating NUL.
struct CookieText {
	std::string_view text;
	char at(size_t p) const { return p < text.size() ? text[p] : '\0'; }
	// The value-like run from `p` to ';', CR or NUL.
	size_t run_end(size_t p) const {
		while (at(p) != ';' && at(p) != '\r' && at(p) != '\0') ++p;
		return p;
	}
	// String_CompareNoCaseN(text + p, word, len(word)) == 0.
	bool starts_icase(size_t p, std::string_view word) const {
		for (size_t i = 0; i < word.size(); ++i) {
			if (strutil::ascii_tolower(at(p + i)) != strutil::ascii_tolower(word[i])) return false;
		}
		return true;
	}
	std::string slice(size_t from, size_t to) const {
		from = std::min(from, text.size());
		return std::string(text.substr(from, std::min(to, text.size()) - from));
	}
};

// One cookie at `start`; false when it is refused. See parse_set_cookie_headers.
// [orig: HttpCookie_Parse @0x658F60]
bool parse_one_cookie(const CookieText &t, size_t start, CookieEntry &out) {
	out = CookieEntry{};
	size_t scan = start;
	if (t.at(scan) != '=') {
		char ch = t.at(scan);
		while (ch != '\r' && ch != '\0') {
			ch = t.at(++scan);
			if (ch == '=') break;
		}
		if (ch != '=') return false;
	}
	out.name = t.slice(start, scan);
	size_t pos = scan + 1;
	const size_t value_end = t.run_end(pos);
	out.value = t.slice(pos, value_end);
	pos = value_end;
	for (;;) {
		// A step past the terminating NUL reads past retail's buffer; the walk ends here.
		if (pos >= t.text.size()) return true;
		++pos;
		while (crt_isspace(t.at(pos))) ++pos;
		const size_t name_at = pos;
		while (t.at(pos) != '=' && t.at(pos) != '\r' && t.at(pos) != '\0') ++pos;
		if (t.starts_icase(name_at, "EXPIRES")) {
			if (t.at(pos) != '=') return false;
			// The 29 bytes from the attribute's own name; past the text its NUL ends them.
			out.expires = t.slice(name_at, name_at + 29);
			pos = t.run_end(pos + 1) + 29;
		} else if (t.starts_icase(name_at, "PATH")) {
			if (t.at(pos) != '=') return false;
			const size_t end = t.run_end(pos + 1);
			out.path = t.slice(pos + 1, end);
			pos = end;
		} else if (t.starts_icase(name_at, "DOMAIN")) {
			if (t.at(pos) != '=') return false;
			const size_t end = t.run_end(pos + 1);
			out.domain = t.slice(pos + 1, end);
			pos = end;
		} else if (t.starts_icase(name_at, "SECURE")) {
			out.secure = 1;
			pos += 6;
		}
		if (t.at(pos) == '\r' && t.at(pos + 1) == '\n') return true;
	}
}

// The path rule of the store: String_CompareNoCase(a, b) == 0, a null string equal only to a null
// one. [orig: String_CompareNoCase @0x650000]
bool same_nullable_icase(const std::optional<std::string> &a, const std::optional<std::string> &b) {
	if (!a || !b) return !a && !b;
	return strutil::iequals(*a, *b);
}

// String_SafeLen(s) == 0.
bool nullable_empty(const std::optional<std::string> &s) {
	return !s || s->empty();
}

bool is_root_path(const std::optional<std::string> &s) {
	return s && strutil::iequals(*s, "/");
}

// The answer's header text as the browser's response handler reads it: each line and CR LF, then
// the blank line's CR LF.
std::string response_header_text(const std::vector<std::string> &response_headers) {
	std::string joined;
	for (const std::string &line : response_headers) {
		joined += line;
		joined += "\r\n";
	}
	joined += "\r\n";
	// The response is a C string to the browser: a NUL ends it.
	joined.resize(std::strlen(joined.c_str()));
	return joined;
}

} // namespace

std::vector<CookieEntry> parse_set_cookie_headers(const std::vector<std::string> &response_headers) {
	const std::string joined = response_header_text(response_headers);
	const CookieText t{joined};
	std::vector<CookieEntry> out;
	size_t from = 0;
	for (;;) {
		const size_t hit = strutil::ifind(t.text.substr(from), "SET-COOKIE");
		if (hit == std::string_view::npos) break;
		size_t cookie = from + hit + 12;
		while (crt_isspace(t.at(cookie)) || t.at(cookie) == ':') ++cookie;
		if (cookie >= t.text.size()) break;
		from = cookie;
		CookieEntry entry;
		if (parse_one_cookie(t, cookie, entry)) out.push_back(std::move(entry));
	}
	return out;
}

// See http_login.h.
// [orig: HTTP_ResponseHandler @0x659480 - @0x6594E2..0x6594F8, @0x65951C..0x659528]
std::vector<CookieEntry> response_cookie_batch(int status, const std::vector<std::string> &response_headers) {
	if (status == 304) return {};
	if ((status == 301 || status == 302) &&
	    strutil::ifind(response_header_text(response_headers), "LOCATION") != std::string_view::npos)
		return {};
	return parse_set_cookie_headers(response_headers);
}

JoiConnection parse_joi_connection_string(const std::string &body) {
	JoiConnection out;
	// Isolate the first bracketed run: [ ... ].
	const auto open = body.find('[');
	if (open == std::string::npos) return out;
	const auto close = body.find(']', open + 1);
	if (close == std::string::npos) return out;
	std::string inner = body.substr(open + 1, close - open - 1);

	// Split on '&' into KEY=VALUE pairs (trim surrounding whitespace/newlines on
	// each field, which the template wraps the <TITLE> contents with).
	size_t pos = 0;
	while (pos <= inner.size()) {
		const auto amp = inner.find('&', pos);
		const auto end = amp == std::string::npos ? inner.size() : amp;
		std::string field = inner.substr(pos, end - pos);
		// Trim ASCII whitespace from both ends.
		size_t b = 0, e = field.size();
		while (b < e && (field[b] == ' ' || field[b] == '\t' || field[b] == '\r' ||
		                 field[b] == '\n')) {
			++b;
		}
		while (e > b && (field[e - 1] == ' ' || field[e - 1] == '\t' ||
		                 field[e - 1] == '\r' || field[e - 1] == '\n')) {
			--e;
		}
		field = field.substr(b, e - b);
		const auto eq = field.find('=');
		if (eq != std::string::npos) {
			const std::string key = field.substr(0, eq);
			const std::string value = field.substr(eq + 1);
			if (key == "NK") out.nk = value;
			else if (key == "CK") out.ck = value;
			else if (key == "NI") out.ni = value;
			else if (key == "NP") out.np = value;
			else if (key == "BK") out.bk = value;
			// [orig: LN `atol` @0x54e33e; GS copied @0x54e38a]
			else if (key == "LN") out.ln = io::retail_atol(value.c_str());
			else if (key == "GS") out.gs = value;
		}
		if (amp == std::string::npos) break;
		pos = amp + 1;
	}
	if (!out.nk.empty()) {
		const std::string decoded = url_cipher_decode(out.nk, URL_CIPHER_KEY_NK);
		const size_t colon = decoded.find(':');
		if (colon != std::string::npos) {
			out.host_ip = decoded.substr(0, colon);
			out.host_port = decoded.substr(colon + 1);
		} else {
			out.host_ip = decoded;
		}
	}
	// CK decodes (its own url_cipher key) to a decimal the retail client atol()s
	// into the game session's APPID join field; a NovaWorld host rejects a wrong
	// APPID with code 9. Re-serialize through atol like retail so leading zeros / stray
	// bytes normalize; a missing/garbage CK keeps the "0" LAN default.
	// [orig: URL_ParseConnectionQueryString @0x54dfb0 CK arm; UI_JoinSelectedSession
	//  @0x5699d0 - net_config.app_id (+0x40) = atol(decoded CK) @0x569b8e]
	if (!out.ck.empty()) {
		const std::string decoded_ck = url_cipher_decode(out.ck, URL_CIPHER_KEY_CK);
		out.app_id = std::to_string(io::retail_atol(decoded_ck.c_str()));
		// Lifecycle trace (kInfo -> MCP log ring): the CK -> APPID derivation.
		opennova::io::logf(opennova::io::LogLevel::kInfo,
				"joi: CK='%s' decoded='%s' APPID='%s' host=%s:%s",
				out.ck.c_str(), decoded_ck.c_str(), out.app_id.c_str(),
				out.host_ip.c_str(), out.host_port.c_str());
	}
	// NK is the only dial authority: a string without "NK=" is no join at all (no
	// PlaySetup, no dial), whatever NI/NP carry -- those feed only the proxy
	// rendezvous. [orig: URL_ParseConnectionQueryString @0x54e13c strstr "NK=" ->
	//  jz @0x54e146 (return 0); NI/NP read @0x54e214.. into the proxy buffers only]
	out.ok = !out.host_ip.empty() && !out.host_port.empty();
	return out;
}

bool joi_endpoint_usable(const std::string &host_ip, long port) {
	return port != 0 && host_ip.size() >= 8;
}

void CookieJar::set(const std::string &name, const std::string &value) {
	CookieEntry entry;
	entry.name = name;
	entry.value = value;
	update({entry});
}

void CookieJar::update(const std::vector<CookieEntry> &batch) {
	if (batch.empty()) return;
	if (entries_.empty()) {
		entries_ = batch;
		return;
	}
	for (const CookieEntry &entry : batch) {
		bool matched = false;
		for (CookieEntry &old : entries_) {
			if (!strutil::iequals(old.name, entry.name)) continue;
			if (!same_nullable_icase(old.path, entry.path) &&
			    !(is_root_path(old.path) && nullable_empty(entry.path)) &&
			    !(is_root_path(entry.path) && nullable_empty(old.path)))
				continue;
			old.value = entry.value;
			matched = true;
			break;
		}
		if (!matched) entries_.push_back(entry);
	}
}

const std::string *CookieJar::find(const std::string &name) const {
	for (const CookieEntry &entry : entries_) {
		if (strutil::iequals(entry.name, name)) return &entry.value;
	}
	return nullptr;
}

// The glob compares the literal after a CRT case fold, so "pubpcid" matches "PUB*" as "PUBPCID"
// does (D-NET-1386) [orig: Config_QueryMatchingEntries @0x64EB70 - the glob matcher @0x651820,
// the fold @0x65190C..0x651923].
std::vector<uint8_t> CookieJar::build_prefixed_blob(const std::string &prefix) const {
	std::vector<uint8_t> blob;
	for (const CookieEntry &entry : entries_) {
		if (entry.name.size() < prefix.size() ||
				!strutil::iequals(std::string_view(entry.name).substr(0, prefix.size()), prefix))
			continue;
		blob.insert(blob.end(), entry.name.begin(), entry.name.end());
		blob.push_back(0);
		blob.insert(blob.end(), entry.value.begin(), entry.value.end());
		blob.push_back(0);
	}
	return blob;
}

// The join's CD set: every PUB* cookie, sent only while the whole set is 1..0x7FE bytes; a larger
// one sends no CD, which the host refuses with code 23 (D-NET-1387) [orig:
// Game_ApplySessionSettingsToGlobals @0x551500 - the "PUB*" gather @0x551B0C into a 0x800-byte
// buffer, Config_QueryMatchingEntries returning every match's length; NetPacket_BuildAnnouncePayload
// @0x4C4BF0 - the size test @0x4C4C15..0x4C4C1E].
std::vector<uint8_t> join_cd_cookie(const CookieJar &jar) {
	std::vector<uint8_t> cd = jar.build_prefixed_blob("PUB");
	if (cd.size() > kJoinCdCookieMax) cd.clear();
	return cd;
}

std::vector<std::string> CookieJar::cookie_header_lines() const {
	std::vector<std::string> lines;
	lines.reserve(entries_.size());
	for (const CookieEntry &entry : entries_) lines.push_back(entry.name + "=" + entry.value + ";");
	return lines;
}

// [orig: Game_ApplySessionSettingsToGlobals @0x551500 - @0x551A96..0x551AC1;
//  CookieJar_GetValueByName @0x64EA90]
std::string novaworld_character_name(const CookieJar &jar) {
	constexpr std::size_t kCharCopy = 31; // the getter's size, NUL included
	const std::string *value = jar.find("CHAR");
	return value != nullptr ? value->substr(0, kCharCopy) : std::string();
}

} // namespace opennova
