#include <net/admin/admin_protocol.h>

#include <base/io/le.h>
#include <base/io/strutil.h>
#include <net/novacrypto/nwu.h>

#include <algorithm>
#include <cstring>

namespace opennova {

namespace {

constexpr uint32_t kPacketMarker = 0x0A0D0000u;
// A sanity bound on a reply; the server's largest (PLAYER LIST, MISSION AVAILABLE) stay far
// below it.
constexpr uint32_t kReplyMaxBytes = 1u << 20;

std::vector<std::string_view> split_lines(std::string_view text) {
	std::vector<std::string_view> out;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string_view::npos) end = text.size();
		std::string_view line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		out.push_back(line);
		start = end + 1;
	}
	return out;
}

std::string_view trim_right(std::string_view s) {
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
	return s;
}

} // namespace

std::vector<uint8_t> admin_encode_packet(const uint8_t *payload, size_t size) {
	std::vector<uint8_t> out;
	out.reserve(ADMIN_PACKET_HEADER_BYTES + size);
	io::append_u32_le(out, kPacketMarker);
	io::append_u32_le(out, static_cast<uint32_t>(ADMIN_PACKET_HEADER_BYTES + size));
	if (size != 0) out.insert(out.end(), payload, payload + size);
	return out;
}

// [orig: CAdminServer_ProcessClientData @0x406ef0..0x406f06 (the marker, then the length)]
bool admin_decode_header(const uint8_t *header, size_t &payload_size) {
	if (io::read_u32_le(header) != kPacketMarker) return false;
	const uint32_t total = io::read_u32_le(header + 4);
	if (total < ADMIN_PACKET_HEADER_BYTES || total > kReplyMaxBytes) return false;
	payload_size = total - ADMIN_PACKET_HEADER_BYTES;
	return true;
}

bool admin_challenge_valid(const std::vector<uint8_t> &payload) {
	return payload.size() == ADMIN_CHALLENGE_BYTES && payload[0] == 0x01 && payload.back() == 0;
}

namespace {

// The cipher's three LCG words, keyed by the challenge string.
struct CipherWords {
	uint32_t s1 = 0;
	uint32_t s2 = 0;
	uint32_t s3 = 0;
};

// The seed: Σ(int8(k)² + i) + len + 0x32, then three steps of the 16-bit LCG (the NWU
// multiplier). [orig: Crypto_ComputeSeedFromBuffer @0x437250 (movsx @0x437280,
//  lea eax,[esi+edx+32h] @0x437295); Crypto_EncryptBuffer @0x43753e..0x43755d]
CipherWords cipher_words(const uint8_t *key, size_t key_len) {
	uint32_t seed = 0;
	for (size_t i = 0; i < key_len; ++i) {
		const int32_t v = static_cast<int8_t>(key[i]);
		seed += static_cast<uint32_t>(v * v) + static_cast<uint32_t>(i);
	}
	seed += static_cast<uint32_t>(key_len) + 0x32u;
	const uint32_t m = NWU_LCG_MAGIC;
	CipherWords w;
	w.s1 = (seed * m + 1) & 0xFFFFu;
	w.s2 = (m * w.s1 + 1) & 0xFFFFu;
	w.s3 = (m * w.s2 + 1) & 0xFFFFu;
	return w;
}

} // namespace

// The challenge string keys the cipher; the server decrypts the whole payload with the inverse
// (Crypto_DecryptBuffer @0x4375b0), so the zero byte after the fields is encrypted too.
// [orig: Crypto_EncryptBuffer @0x437510; RAT.exe @0x4012d2..0x401361 (65 bytes, zero-filled)]
std::array<uint8_t, ADMIN_LOGIN_BYTES> admin_encode_login(const std::vector<uint8_t> &challenge,
                                                          std::string_view user, std::string_view password) {
	std::array<uint8_t, ADMIN_LOGIN_BYTES> buf{};
	std::memcpy(buf.data(), user.data(), std::min(user.size(), ADMIN_LOGIN_FIELD_MAX_CHARS));
	std::memcpy(buf.data() + 32, password.data(), std::min(password.size(), ADMIN_LOGIN_FIELD_MAX_CHARS));

	const size_t key_len = static_cast<size_t>(
			std::find(challenge.begin(), challenge.end(), uint8_t{0}) - challenge.begin());
	if (key_len == 0) return buf;
	const uint8_t *key = challenge.data();
	const CipherWords w = cipher_words(key, key_len);
	const uint32_t m = NWU_LCG_MAGIC;

	// The key added cyclically. [orig: Crypto_AddKeyString @0x437170]
	for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(buf[i] + key[i % key_len]);
	// Reversed when s3 is odd. [orig: Buffer_ReverseInPlace2 @0x437330]
	if ((w.s3 & 1u) != 0) std::reverse(buf.begin(), buf.end());
	// buf[i] += i + add, add stepping by (u8)s2 from (u8)s1. [orig: Crypto_AddProgressiveKey @0x4371c0]
	uint8_t add = static_cast<uint8_t>(w.s1);
	const uint8_t step = static_cast<uint8_t>(w.s2);
	for (size_t i = 0; i < buf.size(); ++i) {
		buf[i] = static_cast<uint8_t>(buf[i] + i + add);
		add = static_cast<uint8_t>(add + step);
	}
	// The LCG stream from s3. [orig: Crypto_PRNGEncrypt @0x437200]
	uint32_t state = w.s3;
	for (uint8_t &b : buf) {
		state = (m * state + 1) & 0xFFFFu;
		b = static_cast<uint8_t>(b + (state & 0xFFu));
	}
	return buf;
}

void admin_decrypt_buffer(uint8_t *buf, size_t size, const uint8_t *key, size_t key_len) {
	if (buf == nullptr || size < 1 || key_len == 0) return; // [orig: @0x4375b4 `length >= 1`]
	const CipherWords w = cipher_words(key, key_len);
	const uint32_t m = NWU_LCG_MAGIC;
	uint32_t state = w.s3;
	for (size_t i = 0; i < size; ++i) {
		state = (m * state + 1) & 0xFFFFu;
		buf[i] = static_cast<uint8_t>(buf[i] - (state & 0xFFu));
	}
	uint8_t add = static_cast<uint8_t>(w.s1);
	for (size_t i = 0; i < size; ++i) {
		buf[i] = static_cast<uint8_t>(buf[i] - i - add);
		add = static_cast<uint8_t>(add + static_cast<uint8_t>(w.s2));
	}
	if ((w.s3 & 1u) != 0) std::reverse(buf, buf + size);
	for (size_t i = 0; i < size; ++i) buf[i] = static_cast<uint8_t>(buf[i] - key[i % key_len]);
}

// [orig: RAT.exe @0x401492..0x4014f3 (strlen + 1 bytes after the header);
//  ProcessClientData @0x406f20..0x406f27 (an authenticated packet must end in its NUL)]
std::vector<uint8_t> admin_encode_command(std::string_view command) {
	std::vector<uint8_t> out(command.begin(), command.end());
	out.push_back(0);
	return out;
}

// Every reply is SendResponse's strcpy, its length counting the NUL.
// [orig: CAdminServer_SendResponse @0x402d40]
bool admin_reply_text(const std::vector<uint8_t> &payload, std::string &out) {
	const auto nul = std::find(payload.begin(), payload.end(), uint8_t{0});
	out.assign(payload.begin(), nul);
	return nul != payload.end();
}

// [orig: HandleLogin @0x405a0c ("OK - User: %s successfully logged in.")]
bool admin_login_accepted(std::string_view reply) {
	return strutil::starts_with_icase(reply, "OK - ") &&
	       reply.find("successfully logged in.") != std::string_view::npos;
}

// The handlers' failures are "ERROR - <text>", a refused or malformed verb "USAGE - [...]".
// [orig: DispatchCommand @0x406a8d..0x406d58 (the usage reply); HandlePlayer @0x403f63
//  ("ERROR - Not in Game State.")]
bool admin_reply_is_error(std::string_view reply) {
	return strutil::starts_with_icase(reply, "ERROR - ") || strutil::starts_with_icase(reply, "USAGE - ");
}

namespace {

// DispatchCommand's split: ' ' and '\t'..'\r' separate (the C locale's isspace over a
// sign-extended char, which reads 0 for every byte >= 0x80), at most 25 tokens, the 25th
// running to the line's end. [orig: CAdminServer_DispatchCommand @0x4067A0..0x4067EB]
std::vector<std::string_view> split_command(std::string_view line) {
	const auto is_space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
	std::vector<std::string_view> tokens;
	size_t i = 0;
	while (i < line.size() && tokens.size() < 25) {
		while (i < line.size() && is_space(line[i])) ++i;
		if (i == line.size()) break;
		size_t end = i;
		while (end < line.size() && !is_space(line[end])) ++end;
		if (tokens.size() == 24) end = line.size();
		tokens.push_back(line.substr(i, end - i));
		i = end;
	}
	return tokens;
}

bool ieq(std::string_view a, std::string_view b) {
	return strutil::iequals(a, b);
}

// The keys SET writes; any other key with a value draws the usage and then
// `ERROR - Setting Not Found.`. [orig: CAdminServer_HandleSetCommand @0x405A60 — the compares
//  @0x405B93..0x406185, the not-found tail]
constexpr const char *kSetKeys[] = {
	"AutoBalanceOnRecycle", "PuntVote", "VotePercent", "VoteNumPlayersReq", "ChangeTeam",
	"ChangeTeamInterval", "ChangeTeamPenalty", "ChangeTeamDelay", "StartDelay", "DoMinPingCheck",
	"MinPing", "DoMaxPingCheck", "MaxPing", "MaxFriendlyKills", "GameTime", "FriendlyFire",
	"FriendlyTags", "TeamTriggerClaymore", "Tracers", "KOTHLimit", "KillLimit", "MaxScore",
	"FatBullets", "OneShotKill", "ArmoryTimer", "ServerName", "ServerPassword", "SideAPassword",
	"SideBPassword",
};

AdminReplyPlan exact(int count) {
	return AdminReplyPlan{ AdminReplyFraming::Exact, count };
}

} // namespace

// Each row is §6.9's: a handler sends one packet per arm unless noted.
AdminReplyPlan admin_reply_plan(std::string_view command) {
	const std::vector<std::string_view> t = split_command(command);
	if (t.empty()) return exact(1); // the usage
	const std::string_view verb = t[0];
	// QUIT returns 0 and the slot closes, with no reply. [orig: @0x406811]
	if (ieq(verb, "QUIT")) return exact(0);
	// SET: no argument, or a key alone (a password cleared, else the usage): one; a key and a
	// value: one, or the usage and `ERROR - Setting Not Found.` for a key it does not know.
	if (ieq(verb, "SET")) {
		if (t.size() < 3) return exact(1);
		for (const char *key : kSetKeys) {
			if (ieq(t[1], key)) return exact(1);
		}
		return exact(2);
	}
	// PLAYER: LIST, no argument, or a sub-verb without a target: one. A target the server does
	// not find answers `ERROR - Player Not Found.` before the sub-verb is read; PUNT, SWAPTEAM,
	// KILL, ZEROSCORE and CEASEFIRE answer one either way; BAN <#> one or two; BAN ALL one or
	// more per qualifying slot, or none; any other sub-verb none for a found target.
	// [orig: CAdminServer_HandlePlayer @0x403EF0 — LIST @0x403F7C, the target @0x40407E..0x404136,
	//  BAN ALL @0x404298..0x40444B, BAN <#> @0x40445D..0x404539, CEASEFIRE @0x404543..0x404573]
	if (ieq(verb, "PLAYER")) {
		if (t.size() < 2 || ieq(t[1], "LIST") || t.size() < 3) return exact(1);
		const std::string_view sub = t[1];
		if (ieq(sub, "PUNT") || ieq(sub, "SWAPTEAM") || ieq(sub, "KILL") || ieq(sub, "ZEROSCORE") ||
				ieq(sub, "CEASEFIRE"))
			return exact(1);
		if (ieq(sub, "BAN")) {
			if (ieq(t[2], "ALL")) return AdminReplyPlan{ AdminReplyFraming::QuietGap, 0 };
			return AdminReplyPlan{ AdminReplyFraming::BanTarget, 0 };
		}
		return AdminReplyPlan{ AdminReplyFraming::QuietGap, 0 };
	}
	// GOTO: no argument, the usage alone; MENUSTATE the tail (and the main menu's ERROR before
	// it); GAMESTATE and any other target a reply of their own and then the tail.
	// [orig: CAdminServer_HandleGotoCommand @0x4049D0 — the usage @0x4049E4, the arms' reply
	//  @0x404A6F, the tail @0x404A88 / @0x404AAF]
	if (ieq(verb, "GOTO")) {
		if (t.size() < 2) return exact(1);
		if (ieq(t[1], "MENUSTATE")) return AdminReplyPlan{ AdminReplyFraming::MenuState, 0 };
		return exact(2);
	}
	// GET, MISSION (every sub-verb, ADD's `OK - Entry Added` in any state, AVAILABLE's empty
	// catalog an empty reply), WEAPON, CMD, CHAT (GET's empty window an empty reply), ADMINUSER,
	// BANLIST, PETERRABBIT and the usage: one.
	// [orig: CAdminServer_HandleGet @0x403390; CAdminServer_HandleMissionCommand @0x4062E0;
	//  CAdminServer_HandleWeaponCommand @0x4045A0; CAdminServer_HandleCmdCommand @0x4048D0;
	//  CAdminServer_HandleChatCommand @0x404AC0; CAdminServer_HandleAdminUserCommand @0x404CB0;
	//  CAdminServer_HandleBanCommand @0x404D70; CAdminServer_HandleFunCommand @0x404E30]
	return exact(1);
}

bool admin_replies_complete(const AdminReplyPlan &plan, const std::vector<std::string> &replies) {
	switch (plan.framing) {
		case AdminReplyFraming::Exact:
			return replies.size() >= static_cast<size_t>(plan.count);
		case AdminReplyFraming::BanTarget:
			if (replies.empty()) return false;
			return replies.size() >= 2 || !strutil::starts_with_icase(replies[0], "OK - Banned Player #");
		case AdminReplyFraming::MenuState:
			if (replies.empty()) return false;
			return replies.size() >= 2 || replies[0] != "ERROR - Already in 'Menu' State";
		case AdminReplyFraming::QuietGap:
			return false;
	}
	return false;
}

// The header "NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n", then one
// "%-16s\t%2d\t%2d\t%d\t%d\t%d\t%d\n" row per active slot: the name padded (never cut) to 16,
// the slot number, the team. The host's own slot 0 is named by GameText Server/STRSRV19, "Host"
// (a NovaWorld listen host's takes its character name and is a player).
// [orig: CAdminServer_HandlePlayer @0x403f7c (the header), @0x403fe9 (the row);
//  Server_InitNewRoundState @0x51c9c3..0x51ca2a]
std::vector<AdminPlayer> admin_parse_player_list(std::string_view reply) {
	std::vector<AdminPlayer> out;
	for (std::string_view line : split_lines(reply)) {
		const size_t tab1 = line.find('\t');
		if (tab1 == std::string_view::npos) continue;
		const size_t tab2 = line.find('\t', tab1 + 1);
		const size_t tab3 = tab2 == std::string_view::npos ? tab2 : line.find('\t', tab2 + 1);
		if (tab3 == std::string_view::npos) continue;
		const auto slot = strutil::parse_int(std::string(strutil::trim_view(line.substr(tab1 + 1, tab2 - tab1 - 1))));
		if (!slot) continue; // the header row
		AdminPlayer player;
		player.slot = *slot;
		player.name = std::string(trim_right(line.substr(0, tab1)));
		player.team = std::string(strutil::trim_view(line.substr(tab2 + 1, tab3 - tab2 - 1)));
		if (player.name.empty() || (player.slot == 0 && player.name == "Host")) continue;
		// Class, Kills, Deaths, PING: the four "%d" columns after the team.
		int *const columns[] = { &player.player_class, &player.kills, &player.deaths, &player.ping };
		size_t start = tab3 + 1;
		for (int *column : columns) {
			if (start > line.size()) break;
			const size_t end = std::min(line.find('\t', start), line.size());
			if (const auto value = strutil::parse_int(std::string(strutil::trim_view(line.substr(start, end - start)))))
				*column = *value;
			start = end + 1;
		}
		out.push_back(std::move(player));
	}
	return out;
}

// "%d. %s (%s)\n": the row index, the file, the title (+0x414). A file name can hold spaces and
// a title parentheses, so the title starts at the first " (" after the file's extension, and
// at the first " (" when no known extension shows. [orig: CAdminServer_HandleMissionCommand
//  @0x406490..0x40654C]
std::vector<AdminCatalogEntry> admin_parse_mission_available(std::string_view reply) {
	std::vector<AdminCatalogEntry> out;
	for (std::string_view line : split_lines(reply)) {
		const size_t dot = line.find(". ");
		if (dot == std::string_view::npos || line.empty() || line.back() != ')') continue;
		const auto index = strutil::parse_int(std::string(line.substr(0, dot)));
		if (!index || *index < 0) continue; // the row index counts from 0
		const std::string_view rest = line.substr(dot + 2);
		size_t open = std::string_view::npos;
		for (const char *ext : { ".bms (", ".npj (", ".npz (" }) {
			for (size_t at = 0; at + 6 <= rest.size(); ++at) {
				if (strutil::starts_with_icase(rest.substr(at), ext)) {
					open = std::min(open, at + 4);
					break;
				}
			}
		}
		if (open == std::string_view::npos) open = rest.find(" (");
		if (open == std::string_view::npos) continue;
		AdminCatalogEntry entry;
		entry.index = *index;
		entry.file = std::string(rest.substr(0, open));
		entry.title = std::string(rest.substr(open + 2, rest.size() - open - 3));
		out.push_back(std::move(entry));
	}
	return out;
}

// The marks are found from the end, after the last " - (" (a file name can hold " - "); an
// absent mark is "()" or "<>". [orig: CAdminServer_HandleMissionCommand @0x406338..0x406483]
std::vector<AdminRotationEntry> admin_parse_mission_list(std::string_view reply) {
	std::vector<AdminRotationEntry> out;
	for (std::string_view line : split_lines(reply)) {
		const size_t colon = line.find(": ");
		const size_t tags = line.rfind(" - (");
		if (colon == std::string_view::npos || tags == std::string_view::npos || tags <= colon + 2) continue;
		const auto index = strutil::parse_int(std::string(line.substr(0, colon)));
		if (!index || *index < 0) continue; // the entry index counts from 0
		const std::string_view marks = line.substr(tags + 3);
		AdminRotationEntry entry;
		entry.index = *index;
		entry.file = std::string(line.substr(colon + 2, tags - colon - 2));
		entry.swap_sides = marks.find("(2x)") != std::string_view::npos;
		entry.flipped = marks.find("(IS FLIPPED)") != std::string_view::npos;
		entry.one_shot = marks.find("(ONE_SHOT)") != std::string_view::npos;
		entry.current = marks.find("<CURRENT MISSION>") != std::string_view::npos;
		entry.next = marks.find("<NEXT MISSION>") != std::string_view::npos;
		out.push_back(std::move(entry));
	}
	return out;
}

std::vector<std::string> admin_parse_chat_window(std::string_view reply) {
	std::vector<std::string> out;
	for (std::string_view line : split_lines(reply)) {
		if (!line.empty()) out.emplace_back(line);
	}
	return out;
}

// Each row is the key left-justified in 21 columns, "= ", the value.
std::vector<std::pair<std::string, std::string>> admin_parse_game_settings(std::string_view reply) {
	std::vector<std::pair<std::string, std::string>> out;
	for (std::string_view line : split_lines(reply)) {
		const size_t eq = line.find("= ");
		if (eq == std::string_view::npos) continue;
		const std::string_view key = strutil::trim_view(line.substr(0, eq));
		if (key.empty()) continue;
		out.emplace_back(std::string(key), std::string(line.substr(eq + 2)));
	}
	return out;
}

} // namespace opennova
