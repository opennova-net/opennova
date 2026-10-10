#include <net/novaworld/http_login.h>

#include <net/novacrypto/epask.h>
#include <net/novacrypto/url_cipher.h>
#include <net/novaworld/gate_response.h> // parse_ipv4_octets (Network_ParseIPv4AddressOctets)

#include <base/io/crt_ftol.h>
#include <base/io/log.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

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
	// [orig: Buffer_ReverseBytes @0x61B290 (@0x62E01D); sub_6173B0 @0x6173B0, a plain
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

std::vector<std::pair<std::string, std::string>>
parse_set_cookie_values(const std::vector<std::string> &set_cookie_values) {
	std::vector<std::pair<std::string, std::string>> out;
	for (const auto &line : set_cookie_values) {
		// Take the leading "name=value" pair, before the first attribute ';'.
		const auto semi = line.find(';');
		const std::string pair =
		    semi == std::string::npos ? line : line.substr(0, semi);
		const auto eq = pair.find('=');
		if (eq == std::string::npos) continue;
		// Trim surrounding whitespace from the name (some servers emit
		// "Set-Cookie: NAME=..."); values are taken verbatim.
		size_t name_begin = 0;
		while (name_begin < eq && (pair[name_begin] == ' ' || pair[name_begin] == '\t')) {
			++name_begin;
		}
		size_t name_end = eq;
		while (name_end > name_begin &&
		       (pair[name_end - 1] == ' ' || pair[name_end - 1] == '\t')) {
			--name_end;
		}
		if (name_end == name_begin) continue;
		out.emplace_back(pair.substr(name_begin, name_end - name_begin),
		                 pair.substr(eq + 1));
	}
	return out;
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
	// into the game-session BT join field; a NovaWorld host rejects a wrong BT
	// with code 9. Re-serialize through atol like retail so leading zeros / stray
	// bytes normalize; a missing/garbage CK keeps the "0" LAN default.
	// [orig: URL_ParseConnectionQueryString @0x54dfb0 CK arm; net_config.bt =
	//  atol(decoded CK) @0x569b8e]
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
	auto it = values_.find(name);
	if (it == values_.end()) {
		order_.push_back(name);
	}
	values_[name] = value;
}

void CookieJar::merge_set_cookie_values(
    const std::vector<std::string> &set_cookie_values) {
	for (const auto &kv : parse_set_cookie_values(set_cookie_values)) {
		set(kv.first, kv.second);
	}
}

const std::string *CookieJar::find(const std::string &name) const {
	auto it = values_.find(name);
	return it == values_.end() ? nullptr : &it->second;
}

std::vector<uint8_t> CookieJar::build_prefixed_blob(const std::string &prefix) const {
	std::vector<uint8_t> blob;
	for (const std::string &name : order_) {
		if (name.size() < prefix.size() ||
		    name.compare(0, prefix.size(), prefix) != 0) {
			continue;
		}
		const auto it = values_.find(name);
		const std::string &value = it == values_.end() ? name : it->second;
		blob.insert(blob.end(), name.begin(), name.end());
		blob.push_back(0);
		blob.insert(blob.end(), value.begin(), value.end());
		blob.push_back(0);
	}
	return blob;
}

std::vector<std::string> CookieJar::cookie_header_lines() const {
	// Retail emits one "Cookie: name=value;" header per cookie (trailing ';'),
	// not a single merged line [orig: CUIBrowser_SendHTTPRequest @ 0x658840,
	// per-entry sprintf "Cookie: %s=%s;"].
	std::vector<std::string> lines;
	lines.reserve(order_.size());
	for (const auto &name : order_) {
		lines.push_back(name + "=" + values_.at(name) + ";");
	}
	return lines;
}

} // namespace opennova
