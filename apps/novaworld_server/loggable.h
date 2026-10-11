#pragma once

// Client-supplied text on its way into a log line, for every listener of the
// NovaWorld service (the HTTP routes, the gate and NW UDP receive threads,
// main()'s connection callbacks and engine log sink). Header-only, so the
// listeners built without the HTTP layer take it too.

#include <base/io/sha256.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace opennova::novaworld_server {

// Whether `s` holds a control byte (below 0x20, or 0x7F). Usernames and
// handles may hold none (auth.h); nor may an admin ServerCommand arg
// (http_listener.cpp, where the reason is spelled out).
inline bool has_control_byte(std::string_view s) {
	return std::any_of(s.begin(), s.end(), [](char c) {
		const auto byte = static_cast<unsigned char>(c);
		return byte < 0x20 || byte == 0x7F;
	});
}

// `text` as a log line may carry it: each control byte (below 0x20, or 0x7F)
// as \xNN, so no client input can forge a line, and cut after `max_bytes`
// bytes (".." marks the cut). For usernames, handles, URLs, query and form
// values, cookies, tags, admin ServerCommand lines, the names a NovaWorld
// session or a gate probe carries and every other client-supplied string.
inline std::string loggable(std::string_view text, size_t max_bytes = 64) {
	std::string out;
	out.reserve(std::min(text.size(), max_bytes) + 2);
	for (size_t i = 0; i < text.size() && i < max_bytes; ++i) {
		const auto byte = static_cast<unsigned char>(text[i]);
		if (byte < 0x20 || byte == 0x7F) {
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\x%02X", static_cast<unsigned>(byte));
			out += escaped;
		} else {
			out.push_back(text[i]);
		}
	}
	if (text.size() > max_bytes)
		out += "..";
	return out;
}

// A bearer secret's stand-in in a log line: "<redacted #" and the first 8 hex
// digits of its SHA-256 ">", enough to tie the lines about one value together
// and nothing to present it with; "(none)" for an empty one. For the retail
// session tags (LOGINSESSIONTAG, NWJOINSESSIONTAG: the tag names a live login,
// and a host tag serves its HOSTKEY), the persistent-login cookies and the
// website's session token.
inline std::string loggable_secret(std::string_view secret) {
	if (secret.empty())
		return "(none)";
	return "<redacted #" + io::sha256_hex(io::sha256(secret)).substr(0, 8) + ">";
}

// Whether a cookie (or form field) named `name` carries a bearer secret: a
// session tag or token (any name holding "session", the retail
// LOGINSESSIONTAG / NWJOINSESSIONTAG and the website's opennova_session
// alike) or a persistent login (PERSISTENTEXPRESSLOGINDATA, a password-less
// login resume, and PERSISTENTREMEMBERLOGINDATA, the remembered credentials).
// Case aside.
inline bool is_secret_cookie(std::string_view name) {
	std::string lower(name);
	for (char &c : lower) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return lower.find("session") != std::string::npos || lower.rfind("persistent", 0) == 0;
}

} // namespace opennova::novaworld_server
