#include <net/novaworld/lobby_update.h>

#include <base/io/strutil.h>

#include <cstdio>
#include <string_view>

namespace opennova {

// [orig: String_SanitizeForLobby @0x4fe750] — copy, then four passes replacing
// ' ' (@0x4fe7a8), '?' (@0x4fe7c4), '@' (@0x4fe7db) and '=' (@0x4fe7f4) with
// '+'; an empty source writes the literal "---" (@0x4fe771).
std::string lobby_sanitize_value(std::string_view value) {
	if (value.empty()) {
		return "---";
	}
	std::string out(value);
	for (char &ch : out) {
		if (ch == ' ' || ch == '?' || ch == '@' || ch == '=') {
			ch = '+';
		}
	}
	return out;
}

// [orig: Lobby_UpdateServerInfo @0x4ff473..0x4ff62c]
std::string lobby_update_build(const LobbyStatusBlob &blob) {
	std::string out;
	out.reserve(768);

	// sprintf("%s ", g_LobbyName) @0x4ff49d, then " HostKey = %s" @0x4ff4e1
	// over the sanitized key: two spaces between the lobby name and HostKey.
	out += blob.lobby_name;
	out += ' ';
	out += " HostKey = ";
	out += lobby_sanitize_value(blob.host_key);

	// Every Host list node, key and value both sanitized, " %s = %s" @0x4ff54a.
	for (const auto &kv : blob.host_vars) {
		out += ' ';
		out += lobby_sanitize_value(kv.first);
		out += " = ";
		out += lobby_sanitize_value(kv.second);
	}

	// dword_24D2188 @0x4ff560: one " p=%s" per PlayerName @0x4ff5c5, or a
	// lone " p=" when the PlayerList carries none @0x4ff5ff.
	if (blob.send_player_names) {
		if (blob.player_names.empty()) {
			out += " p=";
		} else {
			for (const std::string &player : blob.player_names) {
				out += " p=";
				out += lobby_sanitize_value(player);
			}
		}
	}

	return out;
}

bool lobby_update_parse(std::string_view text, LobbyStatusBlob &out) {
	out = LobbyStatusBlob{};
	// Sanitized keys/values never contain a space, so the blob tokenizes on
	// single spaces; the double space before HostKey yields an empty token.
	std::vector<std::string_view> tokens;
	size_t pos = 0;
	while (pos <= text.size()) {
		const size_t next = text.find(' ', pos);
		const std::string_view tok = text.substr(pos, next == std::string_view::npos
		                                              ? std::string_view::npos : next - pos);
		if (!tok.empty()) tokens.push_back(tok);
		if (next == std::string_view::npos) break;
		pos = next + 1;
	}
	// <lobby> HostKey = <hk>
	if (tokens.size() < 4 || tokens[1] != "HostKey" || tokens[2] != "=") return false;
	out.lobby_name = std::string(tokens[0]);
	out.host_key   = std::string(tokens[3]);

	size_t i = 4;
	while (i < tokens.size()) {
		if (tokens[i].size() >= 2 && tokens[i][0] == 'p' && tokens[i][1] == '=') break;
		if (i + 2 >= tokens.size() || tokens[i + 1] != "=") return false;
		out.host_vars.emplace_back(std::string(tokens[i]), std::string(tokens[i + 2]));
		i += 3;
	}
	out.send_player_names = i < tokens.size();
	for (; i < tokens.size(); ++i) {
		const std::string_view tok = tokens[i];
		if (tok.size() < 2 || tok[0] != 'p' || tok[1] != '=') return false;
		if (tok.size() > 2) out.player_names.emplace_back(tok.substr(2));
	}
	return true;
}

std::string lobby_status_value(const LobbyStatusBlob &blob, std::string_view key) {
	for (const auto &kv : blob.host_vars) {
		if (strutil::iequals(kv.first, key)) return kv.second;
	}
	return {};
}

} // namespace opennova
