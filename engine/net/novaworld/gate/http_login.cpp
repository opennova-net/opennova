#include <novaworld/http_login.h>

#include <novacrypto/epask.h>
#include <novacrypto/url_cipher.h>

#include <cstdio>

namespace opennova {

std::string subnet_key(const std::string &host) {
	// [orig: Network_TruncateIPToSubnet @ 0x62dfe0] truncate only when the host
	// is a valid dotted-decimal IPv4 (the retail parse gate); then keep the
	// first two octets. Anything else (a DNS name) is returned unchanged.
	int a = 0, b = 0, c = 0, d = 0;
	char extra = 0;
	// Reject embedded whitespace/garbage: require exactly four octets and no
	// trailing characters. Each octet must be 0..255.
	if (std::sscanf(host.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &extra) == 4 &&
	    a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255) {
		return std::to_string(a) + "." + std::to_string(b);
	}
	return host;
}

std::string build_login_post_body(const EpaskParams &pub,
                                  const std::vector<LoginFormField> &fields) {
	std::string body;
	for (const auto &f : fields) {
		if (!body.empty()) body.push_back('&');
		body += f.name;
		body.push_back('=');
		body += f.encrypt ? epask_encrypt(f.value, pub) : f.value;
	}
	return body;
}

std::string build_credentials_post_body(
    const EpaskParams &pub, const std::string &name, const std::string &password,
    const std::vector<std::pair<std::string, std::string>> &hidden) {
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
	return build_login_post_body(pub, fields);
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
	if (out.host_ip.empty()) out.host_ip = out.ni;
	if (out.host_port.empty()) out.host_port = out.np;
	out.ok = !out.host_ip.empty() && !out.host_port.empty();
	return out;
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
