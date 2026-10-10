// admin.cfg — the remote-admin console's users and its address whitelist.
//
// A plain-text file in the game's working directory, read once at boot when
// the console is set up [orig: Game_InitSubsystems @0x4A72B8..0x4A72C2 ->
// CAdminServer_LoadConfig @0x406D80]. Retail never writes it. The record is
// docs/net/novaworld-net-re.md §6.9 ("admin.cfg, admin_log.txt and the
// whitelist").
//
// THE FILE. The game.cfg line walk and tokenizer (io::for_each_config_file_line:
// space, comma and tab separate, `"` quotes, `//` or `;` outside quotes ends a
// line, a first token starting with `/` skips it, a missing token reads empty)
// into one callback per line [orig: File_ParseASCIIFileWithCallback @0x53D980
// -> AdminConfigFile_ParseLine @0x405490]:
//
//  - a line whose token 0 is exactly `ip_restrict` (case-sensitive) adds its
//    token 2 as a whitelist pattern; the `=` is token 1 and never read;
//  - every other line is a user: token 0 the name, token 1 the password,
//    token 2 the rights through strtoul(token, NULL, 16).
//
// So `ip_restrict=192.168.*` (no separator) is one token and becomes a user of
// that name with an empty password and no rights; `ip_restrict 192.168.*`
// reads an empty pattern, which admits every address; and a user line with no
// password admits any password (HandleLogin compares over the stored length).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::admincfg {

// The console file's name in the working directory [orig: Game_InitSubsystems
// @0x4A72B8 pushes "admin.cfg"].
inline constexpr const char *kFileName = "admin.cfg";

// The rights word's bits, as CAdminServer_DispatchCommand tests them: the
// dword's low byte for every verb up to BANLIST, its sign bit for PETERRABBIT.
// GOTO, CHAT, ADMINUSER and BANLIST all open on 0x40; the usage reply lists
// CHAT, ADMIN and BANLIST by 0x80, 0x100 and 0x200 instead.
// [orig: CAdminServer_DispatchCommand @0x406833..0x406A6D; the usage
//  @0x406A8D..0x406D58]
namespace right {
inline constexpr uint32_t kGet = 0x1u;
inline constexpr uint32_t kSet = 0x2u;
inline constexpr uint32_t kMission = 0x4u;
inline constexpr uint32_t kPlayer = 0x8u;
inline constexpr uint32_t kWeapon = 0x10u;
inline constexpr uint32_t kCmd = 0x20u;
inline constexpr uint32_t kGoto = 0x40u;     // also CHAT, ADMINUSER, BANLIST
inline constexpr uint32_t kChatListed = 0x80u;
inline constexpr uint32_t kAdminListed = 0x100u;
inline constexpr uint32_t kBanListListed = 0x200u;
inline constexpr uint32_t kPeterRabbit = 0x80000000u;
} // namespace right

// One user line: {name, password, rights}, kept in file order
// [orig: CAdminServer_AddUserEntry @0x405190 (12-byte entries, the array grown
//  by five)].
struct AdminUser {
	std::string name;
	std::string password;
	uint32_t rights = 0;
};

struct AdminConfig {
	std::vector<AdminUser> users;
	// The `ip_restrict` patterns, in file order. Empty: the file had no such
	// line, so the pattern array was never allocated and every address passes
	// [orig: CAdminServer_AddIPRestriction @0x405350; the null test in
	//  CAdminServer_AcceptConnection @0x4055BF].
	std::vector<std::string> ip_restrictions;
};

// The rights token: the CRT's strtoul(token, NULL, 16) on a 32-bit unsigned
// long: leading white space (cp1252's set, 0xA0 included), an optional sign,
// an optional 0x / 0X, hex digits; nothing parsed reads 0, a value past 32 bits
// saturates to 0xFFFFFFFF (every right, PETERRABBIT included), and a minus
// negates the result, the saturated one too: "-FFFFFFFFF" reads 1.
// [orig: CRT_strtoul @0x76B302, the call in AdminConfigFile_ParseLine @0x4054CE]
uint32_t parse_rights(const char *token);

// The whole file's lines through the callback. [orig: CAdminServer_LoadConfig
// @0x406D80 frees the user entries first; the patterns are kept, but the boot
// loads the file once]
AdminConfig parse(const char *text, size_t size);

// parse() over the file at `path`. False (and an empty config) when the file
// does not open: File_ParseASCIIFileWithCallback returns 1 and LoadConfig
// E_FAIL, and the console still listens with no users and no whitelist.
// [orig: File_ParseASCIIFileWithCallback @0x53D9C9; CAdminServer_LoadConfig
//  @0x406DB2]
bool load_file(const std::string &path, AdminConfig &out);

// The whitelist match of one pattern against a source address. `source_ip`
// is the raw in_addr dword (`a | b<<8 | c<<16 | d<<24` for a.b.c.d, the
// PeerAddr::ip packing). The pattern is split on '.' with strtok (so runs of
// dots collapse and a leading dot is skipped); token i is compared with octet
// i, `(addr >> 8i) & 0xFF == (u8)atol(token)`, the shift wrapping so a fifth
// token compares octet 0 again; a token starting with `*` accepts the rest,
// and running out of tokens accepts too. So `192.168` and `192.168.*.5` both
// admit all of 192.168/16, and an empty pattern admits every address.
// [orig: CAdminServer_AcceptConnection @0x4055E8..0x405660]
bool pattern_admits(std::string_view pattern, uint32_t source_ip);

// The first pattern that matches admits; none closes the socket before the
// challenge; no pattern at all admits every address.
// [orig: CAdminServer_AcceptConnection @0x4055BF..0x40567C]
bool admits(const AdminConfig &config, uint32_t source_ip);

} // namespace opennova::admincfg
