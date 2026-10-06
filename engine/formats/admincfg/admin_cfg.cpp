// admin.cfg (admin_cfg.h).
#include <formats/admincfg/admin_cfg.h>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/os_path.h>

#include <cstdio>
#include <cstring>

namespace opennova::admincfg {

namespace {

int hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

} // namespace

// The CRT's strtoul at radix 16 on a 32-bit unsigned long [orig: CRT_strtoul
// @0x76B302 -> strtoxl]: an overflow sets ULONG_MAX whatever the sign; a
// minus otherwise negates the result; no digits read 0.
uint32_t parse_rights(const char *token) {
	if (token == nullptr) return 0;
	const char *p = token;
	while (*p == ' ' || (*p >= '\t' && *p <= '\r')) ++p;
	bool negative = false;
	if (*p == '+' || *p == '-') negative = *p++ == '-';
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && hex_digit(p[2]) >= 0) p += 2;
	uint64_t value = 0;
	bool overflow = false;
	bool digits = false;
	for (int d = hex_digit(*p); d >= 0; d = hex_digit(*++p)) {
		digits = true;
		value = value * 16u + static_cast<uint32_t>(d);
		if (value > 0xFFFFFFFFull) {
			overflow = true;
			value = 0xFFFFFFFFull;
		}
	}
	if (!digits) return 0;
	if (overflow) return 0xFFFFFFFFu;
	const uint32_t result = static_cast<uint32_t>(value);
	return negative ? 0u - result : result;
}

// [orig: AdminConfigFile_ParseLine @0x405490 — the 12-byte `repe cmpsb`
//  against "ip_restrict" @0x40549E..0x4054AE (token 0, case-sensitive and
//  whole), the pattern token 2 @0x4054B0 into CAdminServer_AddIPRestriction
//  @0x405350, else the user: tokens 0 and 1 and strtoul(token 2, 16)
//  @0x4054C3..0x4054E5 into CAdminServer_AddUserEntry @0x405190]
AdminConfig parse(const char *text, size_t size) {
	AdminConfig out;
	io::for_each_config_file_line(text, size, [&](io::ConfigTokens &tokens) {
		if (std::strcmp(tokens.token(0), "ip_restrict") == 0) {
			out.ip_restrictions.emplace_back(tokens.token(2));
			return;
		}
		AdminUser user;
		user.name = tokens.token(0);
		user.password = tokens.token(1);
		user.rights = parse_rights(tokens.token(2));
		out.users.push_back(std::move(user));
	});
	return out;
}

bool load_file(const std::string &path, AdminConfig &out) {
	out = AdminConfig{};
	std::FILE *f = io::fopen_utf8(path.c_str(), "rb");
	if (f == nullptr) return false;
	std::string text;
	char chunk[4096];
	size_t got = 0;
	while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
	std::fclose(f);
	out = parse(text.data(), text.size());
	return true;
}

// [orig: CAdminServer_AcceptConnection @0x4055E8..0x405660 — the copy into
//  the 16-byte stack buffer @0x405600 (D-NET-361: unbounded there; the whole
//  pattern here), strtok(".") @0x405618/@0x405654, the '*' accept @0x405629,
//  `(u8)atol` @0x40562C, the compare under `255 << shift` @0x405648 (the
//  shift a char stepped by 8, so SHL's 5-bit count wraps it), the
//  out-of-tokens accept @0x405660]
bool pattern_admits(std::string_view pattern, uint32_t source_ip) {
	size_t at = 0;
	int index = 0;
	for (;;) {
		while (at < pattern.size() && pattern[at] == '.') ++at;
		if (at >= pattern.size()) return true; // no token left
		size_t end = at;
		while (end < pattern.size() && pattern[end] != '.') ++end;
		const std::string token(pattern.substr(at, end - at));
		if (token[0] == '*') return true;
		const uint32_t octet = static_cast<uint8_t>(io::retail_atol(token.c_str()));
		const uint32_t shift = static_cast<uint32_t>(8 * index) & 31u;
		if ((source_ip & (0xFFu << shift)) != (octet << shift)) return false;
		++index;
		at = end;
	}
}

bool admits(const AdminConfig &config, uint32_t source_ip) {
	if (config.ip_restrictions.empty()) return true;
	for (const std::string &pattern : config.ip_restrictions)
		if (pattern_admits(pattern, source_ip)) return true;
	return false;
}

} // namespace opennova::admincfg
