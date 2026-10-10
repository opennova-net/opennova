#pragma once

// Request cookie reading shared by the HTTP listener's TUs: the retail NW*.dll
// routes (http_listener.cpp) and the website's session access (web_access.cpp).

#include <crow.h>

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
