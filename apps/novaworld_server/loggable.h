#pragma once

// Client-supplied text on its way into a log line, for every listener of the
// NovaWorld service (the HTTP routes, the gate and NW UDP receive threads,
// main()'s connection callbacks and engine log sink). Header-only, so the
// listeners built without the HTTP layer take it too.

#include <base/io/sha256.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server {

// Whether `s` holds a control byte (below 0x20, or 0x7F). Usernames and
// handles may hold none (auth.h); nor may a pushed ServerCommand arg
// (compose_server_command, host_routes.h, where the reason is spelled out).
inline bool has_control_byte(std::string_view s) {
	return std::any_of(s.begin(), s.end(), [](char c) {
		const auto byte = static_cast<unsigned char>(c);
		return byte < 0x20 || byte == 0x7F;
	});
}

// Whether UTF-8 `text` holds a C1 control (U+0080..U+009F: 0xC2 then
// 0x80..0x9F), which a user's text may hold no more than a control byte: the
// hosted settings' game name, a pushed ServerCommand arg and a console chat line.
inline bool has_c1_control(std::string_view text) {
	for (size_t i = 0; i + 1 < text.size(); ++i) {
		const auto lead = static_cast<unsigned char>(text[i]);
		const auto next = static_cast<unsigned char>(text[i + 1]);
		if (lead == 0xC2 && next >= 0x80 && next <= 0x9F)
			return true;
	}
	return false;
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

// The process's log secrets: values no log line may show, each registered while it is live
// (a hosted server's remote-admin password, for as long as its admin link holds it; ADR 0053
// d5). main()'s engine log sink masks every line it prints, and a holder masks its own lines
// (mask_log_secrets). A registration counts: a value registered twice stays masked until it
// is removed twice. A value under 8 bytes is not registered: masking it would shred the log,
// and no secret the service mints is that short.
namespace log_secrets_detail {
struct Registry {
	std::mutex mu;
	std::vector<std::string> values;
};
inline Registry &registry() {
	static Registry r;
	return r;
}
} // namespace log_secrets_detail

inline void add_log_secret(const std::string &secret) {
	if (secret.size() < 8)
		return;
	auto &r = log_secrets_detail::registry();
	std::lock_guard<std::mutex> lock(r.mu);
	r.values.push_back(secret);
}

inline void remove_log_secret(const std::string &secret) {
	auto &r = log_secrets_detail::registry();
	std::lock_guard<std::mutex> lock(r.mu);
	const auto it = std::find(r.values.begin(), r.values.end(), secret);
	if (it != r.values.end())
		r.values.erase(it);
}

// `line` with every registered secret replaced by "***".
inline std::string mask_log_secrets(std::string line) {
	auto &r = log_secrets_detail::registry();
	std::lock_guard<std::mutex> lock(r.mu);
	for (const std::string &secret : r.values) {
		for (size_t at = line.find(secret); at != std::string::npos; at = line.find(secret, at + 3))
			line.replace(at, secret.size(), "***");
	}
	return line;
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
