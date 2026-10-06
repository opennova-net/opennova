// admin.cfg (formats/admincfg): the user lines, the rights token's strtoul at radix 16, the
// ip_restrict lines and their quirks, and the whitelist matcher. The file is authored inline
// here: retail never writes admin.cfg, so there is no writer and no fixture to roundtrip.
// [orig: AdminConfigFile_ParseLine @0x405490; CAdminServer_AddUserEntry @0x405190;
//  CAdminServer_AddIPRestriction @0x405350; CAdminServer_AcceptConnection @0x4055BF..0x40567C]

#include <formats/admincfg/admin_cfg.h>

#include <cstdio>
#include <string>

using namespace opennova::admincfg;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

AdminConfig parse_text(const std::string &text) {
	return parse(text.data(), text.size());
}

uint32_t ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
	return a | (b << 8) | (c << 16) | (d << 24);
}

} // namespace

int main() {
	// The shipped file: no users, one pattern.
	{
		const AdminConfig cfg = parse_text("// Remote admin users\r\nip_restrict = 192.168.*\r\n");
		expect(cfg.users.empty() && cfg.ip_restrictions.size() == 1 && cfg.ip_restrictions[0] == "192.168.*",
		       "the shipped shape: one pattern, token 2");
		expect(!admits(cfg, ip(127, 0, 0, 1)) && admits(cfg, ip(192, 168, 3, 4)),
		       "192.168.* refuses loopback and admits the LAN");
	}
	// Users: the name, the password, the rights through strtoul(token, NULL, 16).
	{
		const AdminConfig cfg = parse_text(
				"admin pw 0D\r\n"
				"two \"pass word\" 0x0D\r\n"
				"three pw d\r\n"
				"bad pw zz\r\n"
				"huge pw FFFFFFFFF\r\n"
				"neg pw -1\r\n"
				"nopass\r\n"
				"; a comment line\r\n"
				"/skipped pw FF\r\n");
		expect(cfg.users.size() == 7, "a first token starting with / is skipped, a comment line has no token");
		if (cfg.users.size() == 7) {
			expect(cfg.users[0].name == "admin" && cfg.users[0].password == "pw" && cfg.users[0].rights == 0x0D, "0D");
			expect(cfg.users[1].password == "pass word" && cfg.users[1].rights == 0x0D, "a quoted password, 0x0D");
			expect(cfg.users[2].rights == 0x0D, "d");
			expect(cfg.users[3].rights == 0, "a non-hex value reads 0");
			expect(cfg.users[4].rights == 0xFFFFFFFFu, "an overflow saturates, every right including PETERRABBIT");
			expect(cfg.users[5].rights == 0xFFFFFFFFu, "a minus negates");
			expect(cfg.users[6].name == "nopass" && cfg.users[6].password.empty() && cfg.users[6].rights == 0,
			       "a user line without a password reads empty");
		}
		expect(cfg.ip_restrictions.empty(), "no ip_restrict line: no pattern array");
		expect(admits(cfg, ip(8, 8, 8, 8)), "and every address passes");
	}
	// ip_restrict's quirks.
	{
		const AdminConfig glued = parse_text("ip_restrict=192.168.*\r\n");
		expect(glued.ip_restrictions.empty() && glued.users.size() == 1 && glued.users[0].name == "ip_restrict=192.168.*" &&
		               glued.users[0].password.empty() && glued.users[0].rights == 0,
		       "no separator: one token, a user of that name");
		const AdminConfig short_line = parse_text("ip_restrict 10.0.0.1\r\n");
		expect(short_line.ip_restrictions.size() == 1 && short_line.ip_restrictions[0].empty() &&
		               admits(short_line, ip(1, 2, 3, 4)),
		       "the pattern is token 2: `ip_restrict x` reads an empty pattern, which admits everything");
		const AdminConfig upper = parse_text("IP_RESTRICT = 10.*\r\n");
		expect(upper.ip_restrictions.empty() && upper.users.size() == 1, "the keyword is case-sensitive");
	}
	// The matcher: octet i under shift 8i, `*` accepts the rest, running out accepts.
	{
		expect(pattern_admits("192.168", ip(192, 168, 9, 9)), "two tokens admit the /16");
		expect(pattern_admits("192.168.*.5", ip(192, 168, 1, 1)), "the * accepts the rest, the .5 never read");
		expect(!pattern_admits("192.168.1.5", ip(192, 168, 1, 6)), "a full mismatch");
		expect(pattern_admits("", ip(1, 2, 3, 4)), "an empty pattern");
		expect(pattern_admits("10..0.0.1", ip(10, 0, 0, 1)), "strtok collapses a run of dots");
		expect(pattern_admits("300.0.0.1", ip(44, 0, 0, 1)), "(u8)atol: 300 is 44");
		expect(pattern_admits("10.0.0.1.10", ip(10, 0, 0, 1)) && !pattern_admits("10.0.0.1.11", ip(10, 0, 0, 1)),
		       "a fifth token's shift wraps to octet 0");
		const AdminConfig two = parse_text("ip_restrict = 10.*\r\nip_restrict = 127.0.0.1\r\n");
		expect(admits(two, ip(127, 0, 0, 1)) && admits(two, ip(10, 9, 9, 9)) && !admits(two, ip(11, 0, 0, 1)),
		       "the first matching pattern admits");
	}
	// parse_rights alone.
	{
		expect(parse_rights("  7f") == 0x7F && parse_rights("+40") == 0x40 && parse_rights("0x") == 0 &&
		               parse_rights("80000000") == 0x80000000u && parse_rights("") == 0,
		       "strtoul's edges");
	}
	if (g_failures == 0) {
		std::printf("admin_cfg: OK\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
