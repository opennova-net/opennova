#pragma once

// Request cookie reading and Set-Cookie writing shared by the HTTP listener's
// TUs: the retail NW*.dll routes (http_listener.cpp) and the website's session
// access (web_access.cpp).

#include "accounts/auth.h"

#include <crow.h>

#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace opennova::novaworld_server {

inline std::map<std::string, std::string> parse_cookie_header(std::string_view header) {
	std::map<std::string, std::string> out;
	size_t pos = 0;
	while (pos < header.size()) {
		// Cookie pairs are separated by ';' OR ','. Retail's IB3 client uses
		// commas (RFC 2965 style); splitting on ';' alone swallows every pair
		// after the first into one value (e.g. NWHANDLE hidden inside the
		// NWJOINSESSIONTAG value), so the joiner can't be identified at
		// /NWJoin.dll -> empty PUBPCID -> "login information is absent (GDC024)".
		// werkzeug (onnet) splits on both — match it.
		while (pos < header.size() &&
		       (header[pos] == ';' || header[pos] == ',' || header[pos] == ' ')) ++pos;
		const auto eq = header.find('=', pos);
		if (eq == std::string_view::npos) break;
		const auto end = header.find_first_of(";,", eq + 1);
		const auto val_end = (end == std::string_view::npos) ? header.size() : end;
		std::string name(header.substr(pos, eq - pos));
		std::string value(header.substr(eq + 1, val_end - eq - 1));
		// Defensive: strip any leftover leading comma (onnet's lstrip(",")).
		while (!name.empty() && name.front() == ',') name.erase(0, 1);
		out.emplace(std::move(name), std::move(value));
		pos = val_end + 1;
	}
	return out;
}

// Whether `value` may go into a Set-Cookie value byte for byte. Refused: a
// control byte (below 0x20, or 0x7F; CR and LF would split the response, NUL
// ends it), ';' (ends the value and starts an attribute: a stock client reads
// a Set-Cookie value up to ';', CR or NUL [orig: HttpCookie_Parse @ 0x658f60])
// and ',' (a second cookie to a comma-splitting jar: parse_cookie_header above,
// after retail's RFC 2965-style Cookie headers). Everything else passes as it
// is: the stock parser keeps a space, a quote or a non-ASCII byte in the value,
// and an account's nwhandle may hold them, so refusing them would blank a real
// handle. Every value the service sends a stock client passes (retail's
// VER1=3, VER2=2345, GT=jop:cus2, CC=us, the e:n:key EPASK, the
// NWServer:...:SESSIONTAG:n:hex tags, A-P PUB* values, PCIDs, addresses).
inline bool cookie_value_ok(std::string_view value) {
	for (const char c : value) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte < 0x20 || byte == 0x7F || c == ';' || c == ',')
			return false;
	}
	return true;
}

// Whether `name` is an HTTP token (RFC 7230 tchar), as a cookie name must be.
inline bool cookie_name_ok(std::string_view name) {
	if (name.empty())
		return false;
	for (const char c : name) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte <= 0x20 || byte >= 0x7F)
			return false;
		if (std::string_view("()<>@,;:\\\"/[]?={}").find(c) != std::string_view::npos)
			return false;
	}
	return true;
}

// The one way a Set-Cookie header's text is built: `name=value` and then
// `attributes` (the service's own literal, e.g. "Path=/"). A value
// cookie_value_ok refuses goes out empty, the cookie cleared as the routes'
// own placeholders clear it, and the refusal is logged escaped; "" (send no
// header) for a name that is not a token.
inline std::string set_cookie_text(
		std::string_view name, std::string_view value, std::string_view attributes) {
	if (!cookie_name_ok(name)) {
		std::fprintf(stderr, "[http] refused a Set-Cookie name: '%s'\n", loggable(name).c_str());
		return {};
	}
	std::string text(name);
	text += '=';
	if (cookie_value_ok(value)) {
		text += value;
	} else {
		std::fprintf(stderr, "[http] refused a Set-Cookie %s value: '%s'\n",
				std::string(name).c_str(), loggable(value).c_str());
	}
	if (!attributes.empty()) {
		text += "; ";
		text += attributes;
	}
	return text;
}

// Appends the Set-Cookie set_cookie_text builds (nothing for a refused name).
inline void add_set_cookie(crow::response &res, std::string_view name, std::string_view value,
		std::string_view attributes) {
	std::string text = set_cookie_text(name, value, attributes);
	if (!text.empty())
		res.add_header("Set-Cookie", std::move(text));
}

// Whether `text` may stand in a response header name or value: no control
// byte but HTAB. AccessMiddleware::after_handle drops any header that fails it,
// the backstop behind set_cookie_text for every header any route builds.
inline bool header_text_ok(std::string_view text) {
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if ((byte < 0x20 && c != '\t') || byte == 0x7F)
			return false;
	}
	return true;
}

// Crow stores request headers in a case-INSENSITIVE multimap and
// get_header_value() returns only the FIRST match. Retail's IB3 client sends
// each cookie as its OWN "Cookie:" header, so reading a single header silently
// drops every other cookie — at /NWJoin.dll that loses NWHANDLE, the joiner
// can't be identified, PUBPCID comes out empty and the client reports "login
// info invalid or expired" (host/login happen to work because the one cookie
// Crow returns is the session tag they need). werkzeug (onnet) merges every
// Cookie header into request.cookies; match it by concatenating them all here
// before parsing. [verified: 3 separate Cookie: headers -> Crow exposes 1]
inline std::string request_cookie_header(const crow::request &req) {
	std::string combined;
	const auto range = req.headers.equal_range("Cookie");
	for (auto it = range.first; it != range.second; ++it) {
		if (it->second.empty()) continue;
		if (!combined.empty()) combined += "; ";
		combined += it->second;
	}
	return combined;
}

} // namespace opennova::novaworld_server
