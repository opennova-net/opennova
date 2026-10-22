#include <net/novaworld/gate_metrics.h>
#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>

namespace opennova {

bool gate_metrics_decode(const uint8_t *data, size_t size, GateMetricsReport &report) {
	// [orig: Server_SendServerMetricsToGate @0x4FA4B0; Server_SendPlayerMetricsToGate @0x4FAA30;
	// Server_SendMetricsToGate @0x4FB0A0; Server_SendMissionMetrics @0x4FB3A0;
	// Server_SendPlayerMissionMetrics @0x4FB8F0; Server_SendPingMetricsToGate @0x511BF0]
	// Reporters encrypt strlen bytes (no trailing NUL) with "1010101".
	// Crypto_EncryptBuffer @0x437510 has the same seed + ADD/reverse/progressive/
	// LCG chain as NapiNP_EncryptBuffer, including multiplier 0x04B05731.
	// PING nests its per-player rows: `\tENTRY` @0x511f20, `\t\tBIP1..BIP4`
	// @0x511f4f..0x511fdd, `\t\tMS` @0x51200b, `\tENDENTRY` @0x512040, then the
	// shared `ENDBLOCK` @0x512079 and the same cipher @0x5120ab.
	if (!data || size == 0 || size > 65507) return false;
	std::string plain(reinterpret_cast<const char *>(data), size);
	nwu_encrypt(reinterpret_cast<uint8_t *>(plain.data()), plain.size(), "1010101");
	std::string_view remaining(plain);
	auto line = [&remaining](std::string_view &out) {
		const auto end = remaining.find("\r\n");
		if (end == std::string_view::npos) return false;
		out = remaining.substr(0, end);
		remaining.remove_prefix(end + 2);
		return true;
	};
	auto field = [](std::string_view row, GateMetricsField &out) {
		const auto space = row.find(' ');
		if (space == std::string_view::npos || space == 0) return false;
		const auto key = row.substr(0, space);
		if (!std::all_of(key.begin(), key.end(), [](char c) {
			return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		})) return false;
		auto value = row.substr(space + 1);
		if (value.empty()) return false;
		const bool quoted = value.front() == '"';
		if (quoted) {
			if (value.size() < 2 || value.back() != '"') return false;
			value = value.substr(1, value.size() - 2);
			if (value.find_first_of("\"\t\r\n") != std::string_view::npos ||
				value.find('\0') != std::string_view::npos) return false;
		} else {
			auto digits = value;
			if (digits.front() == '-') digits.remove_prefix(1);
			if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
				[](char c) { return c >= '0' && c <= '9'; })) return false;
		}
		out = {std::string(key), std::string(value), quoted};
		return true;
	};
	std::string_view row;
	if (!line(row) || row != "METPROTOCOL 1") return false;
	if (!line(row) || row.size() < 9 || row.substr(0, 7) != "BLOCK \"" ||
		row.back() != '"') return false;
	GateMetricsReport decoded;
	decoded.block = row.substr(7, row.size() - 8);
	if (decoded.block != "SERVER" && decoded.block != "PLAYER" &&
		decoded.block != "ID" && decoded.block != "SERVERMISSION" &&
		decoded.block != "PLAYERMISSION" && decoded.block != "PING") return false;
	bool in_entry = false;
	std::vector<GateMetricsField> entry;
	while (line(row)) {
		if (row == "ENDBLOCK") {
			if (in_entry || !remaining.empty()) return false;
			report = std::move(decoded);
			return true;
		}
		if (row.empty() || row.front() != '\t') return false;
		row.remove_prefix(1);
		if (row == "ENTRY") {
			if (in_entry) return false;
			in_entry = true;
			entry.clear();
			continue;
		}
		if (row == "ENDENTRY") {
			if (!in_entry) return false;
			in_entry = false;
			decoded.entries.push_back(std::move(entry));
			entry.clear();
			continue;
		}
		const bool nested = !row.empty() && row.front() == '\t';
		if (nested != in_entry) return false;
		if (nested) row.remove_prefix(1);
		GateMetricsField parsed;
		if (!field(row, parsed)) return false;
		(nested ? entry : decoded.fields).push_back(std::move(parsed));
	}
	return false;
}

// [orig: CServerTick_Construct @0x5017D0]
void GateServerTick::construct(uint32_t now_ms, std::string_view cc_bin, int32_t config_max_players,
                               int32_t players, int32_t team1, int32_t team2) {
	*this = GateServerTick{};
	start_ms = now_ms;
	// Napi_CopyString(this + 36, cc, 32): 31 characters and the NUL.
	gcc = std::string(cc_bin.substr(0, 31));
	max_players = config_max_players;
	update_max_player_counts(players, team1, team2);
}

// [orig: CServerTick_Reset @0x4F9F40]
void GateServerTick::reset(uint32_t now_ms) {
	state = 0;
	start_ms = now_ms;
	phase_start_ms = 0;
	loading_ms = waiting_ms = playing_ms = gameover_ms = 0;
	mission_completes = 0;
	cycles = 0;
	gcc.clear();
	max_players = most_players = most_team1 = most_team2 = 0;
}

// [orig: CServerTick_SetState @0x4FA090]
void GateServerTick::set_state(uint32_t new_state, uint32_t now_ms) {
	if (state == new_state) return;
	switch (state) {
	case 1: loading_ms += now_ms - phase_start_ms; break;
	case 2: waiting_ms += now_ms - phase_start_ms; break;
	case 3: playing_ms += now_ms - phase_start_ms; break;
	case 4:
		gameover_ms += now_ms - phase_start_ms;
		if (new_state == 1) ++cycles;
		break;
	default: break;
	}
	state = new_state;
	phase_start_ms = now_ms;
	if (new_state == 4) ++mission_completes;
}

// [orig: Server_ProcessPeriodicTimerUpdate @0x4FBF72..0x4FBFD3; Server_TickUpdate
//  @0x51D9D4..0x51DA3B, @0x51DAD0..0x51DB1B]
void GateServerTick::close_phase(uint32_t now_ms) {
	if (state == 0) return;
	switch (state) {
	case 1: loading_ms += now_ms - phase_start_ms; break;
	case 2: waiting_ms += now_ms - phase_start_ms; break;
	case 3: playing_ms += now_ms - phase_start_ms; break;
	case 4: gameover_ms += now_ms - phase_start_ms; break;
	default: break;
	}
	state = 0;
	phase_start_ms = now_ms;
}

// [orig: CServerTick_UpdateMaxPlayerCounts @0x4FA130]
void GateServerTick::update_max_player_counts(int32_t players, int32_t team1, int32_t team2) {
	if (players > most_players) most_players = players;
	if (team1 > most_team1) most_team1 = team1;
	if (team2 > most_team2) most_team2 = team2;
}

namespace {

// One reporter's text, row by row, as its sprintf chain appends to the 64 KiB stack buffer.
class MetricsText {
public:
	explicit MetricsText(const char *block) {
		text_ = "METPROTOCOL 1\r\n";
		text_ += "BLOCK \"";
		text_ += block;
		text_ += "\"\r\n";
	}
	// `\t<KEY> "%s"`
	void string_row(const char *key, std::string_view value) {
		text_ += '\t';
		text_ += key;
		text_ += " \"";
		text_ += value;
		text_ += "\"\r\n";
	}
	// `<indent><KEY> %ld`
	void number_row(const char *key, long value, const char *indent = "\t") {
		char digits[24];
		std::snprintf(digits, sizeof(digits), "%ld", value);
		text_ += indent;
		text_ += key;
		text_ += ' ';
		text_ += digits;
		text_ += "\r\n";
	}
	void bare_row(const char *row) {
		text_ += row;
		text_ += "\r\n";
	}
	std::string finish() {
		text_ += "ENDBLOCK\r\n";
		return std::move(text_);
	}
	size_t size() const { return text_.size(); }

private:
	std::string text_;
};

// A DWORD printed "%ld": the word's bits read signed, the 32-bit long it is pushed as.
long as_long(uint32_t word) {
	return static_cast<long>(static_cast<int32_t>(word));
}

// The rows every tick reporter opens with: LABEL, LOBBY (the lobby name's copy into the
// 512-byte locale buffer) and GAMETYPE (GameType_GetAbbreviation(g_GameType, 1)), then RUNNING
// and the four phase totals, each ms word divided by 1000 unsigned.
// [orig: Server_SendServerMetricsToGate @0x4FA57D..0x4FA749]
void tick_rows(MetricsText &out, const GateMetricsHost &host, std::string_view game_type,
               const GateServerTick &tick, uint32_t now_ms) {
	out.string_row("LABEL", host.label);
	out.string_row("LOBBY", host.lobby);
	out.string_row("GAMETYPE", game_type);
	out.number_row("RUNNING", as_long((now_ms - tick.start_ms) / 1000u));
	out.number_row("LOADINGMISSION", as_long(tick.loading_ms / 1000u));
	out.number_row("WAITINGTOBEGIN", as_long(tick.waiting_ms / 1000u));
	out.number_row("PLAYING", as_long(tick.playing_ms / 1000u));
	out.number_row("GAMEOVERSTATS", as_long(tick.gameover_ms / 1000u));
}

// The machine's locale rows, under the gate's METEXT alone: COUNTRYNAME / LANG / TZB, or with
// the G prefix in the two player blocks.
// [orig: Server_SendServerMetricsToGate @0x4FA87E..0x4FA98D; Server_SendPlayerMetricsToGate
//  @0x4FAE5F..0x4FAF6D]
void machine_locale_rows(MetricsText &out, const GateMetricsLocale &locale, bool game_prefix) {
	out.string_row(game_prefix ? "GCOUNTRYNAME" : "COUNTRYNAME", locale.country_name);
	out.string_row(game_prefix ? "GLANG" : "LANG", locale.language);
	out.number_row(game_prefix ? "GTZB" : "TZB", locale.tz_bias);
}

} // namespace

// [orig: Server_SendServerMetricsToGate @0x4FA4B0]
std::string gate_metrics_server_text(const GateMetricsHost &host, std::string_view game_type,
                                     const GateServerTick &tick, uint32_t now_ms) {
	MetricsText out("SERVER");
	tick_rows(out, host, game_type, tick, now_ms);
	out.number_row("NUMMISSIONCOMPLETES", tick.mission_completes);
	out.number_row("NUMCYCLES", tick.cycles);
	out.string_row("GCC", tick.gcc);
	out.string_row("GV", host.version);
	out.number_row("MAXPLAYERS", tick.max_players);
	out.number_row("MOSTPLAYERS", tick.most_players);
	if (host.met_ext) machine_locale_rows(out, host.locale, false);
	return out.finish();
}

// [orig: Server_SendMetricsToGate @0x4FB0A0]
std::string gate_metrics_id_text(const GateMetricsHost &host, std::string_view cc_bin) {
	MetricsText out("ID");
	out.string_row("LABEL", host.label);
	out.string_row("GCC", cc_bin);
	out.string_row("GV", host.version);
	if (host.met_ext) machine_locale_rows(out, host.locale, false);
	return out.finish();
}

// [orig: Server_SendMissionMetrics @0x4FB3A0]
std::string gate_metrics_server_mission_text(const GateMetricsHost &host,
                                             std::string_view game_type,
                                             const GateServerTick &mission, uint32_t now_ms) {
	MetricsText out("SERVERMISSION");
	tick_rows(out, host, game_type, mission, now_ms);
	out.string_row("MISSIONFILENAME", mission.mission_file);
	out.string_row("GCC", mission.gcc);
	out.string_row("GV", host.version);
	out.number_row("MAXPLAYERS", mission.max_players);
	out.number_row("MOSTPLAYERS", mission.most_players);
	if (host.met_ext) machine_locale_rows(out, host.locale, false);
	return out.finish();
}

// PLAYER's second locale pair keeps the G prefix of the first (GLANG / GTZB again), where
// PLAYERMISSION prints PLANG / PTZB.
// [orig: Server_SendPlayerMetricsToGate @0x4FAA30 - GLANG @0x4FAFD0, GTZB @0x4FB001]
std::string gate_metrics_player_text(const GateMetricsHost &host, std::string_view game_type,
                                     std::string_view server_gcc, const GateServerTick &slot,
                                     const GateMetricsPlayer &player, uint32_t now_ms) {
	MetricsText out("PLAYER");
	tick_rows(out, host, game_type, slot, now_ms);
	out.number_row("NUMMISSIONCOMPLETES", slot.mission_completes);
	out.number_row("NUMCYCLES", slot.cycles);
	out.string_row("PCC", player.country_code);
	out.string_row("GCC", server_gcc);
	out.string_row("PV", player.version);
	out.string_row("GV", host.version);
	out.string_row("PID", player.pid);
	if (host.met_ext) {
		machine_locale_rows(out, host.locale, true);
		out.string_row("PCOUNTRYNAME", player.country_name);
		out.string_row("GLANG", player.language);
		out.number_row("GTZB", player.tz_bias);
	}
	return out.finish();
}

// [orig: Server_SendPlayerMissionMetrics @0x4FB8F0 - PLANG @0x4FBE80, PTZB @0x4FBEB1]
std::string gate_metrics_player_mission_text(const GateMetricsHost &host,
                                             std::string_view game_type,
                                             const GateServerTick &mission,
                                             const GateServerTick &slot,
                                             const GateMetricsPlayer &player, uint32_t now_ms) {
	MetricsText out("PLAYERMISSION");
	tick_rows(out, host, game_type, slot, now_ms);
	out.string_row("MISSIONFILENAME", mission.mission_file);
	out.string_row("PCC", player.country_code);
	out.string_row("GCC", mission.gcc);
	out.string_row("PV", player.version);
	out.string_row("GV", host.version);
	out.string_row("PID", player.pid);
	if (host.met_ext) {
		machine_locale_rows(out, host.locale, true);
		out.string_row("PCOUNTRYNAME", player.country_name);
		out.string_row("PLANG", player.language);
		out.number_row("PTZB", player.tz_bias);
	}
	return out.finish();
}

// [orig: Server_SendPingMetricsToGate @0x511BF0]
std::string gate_metrics_ping_text(const GateMetricsHost &host, std::string_view cc_bin,
                                   const std::vector<GateMetricsPingEntry> &sorted_entries) {
	MetricsText out("PING");
	out.string_row("LABEL", host.label);
	out.string_row("GCC", cc_bin);
	out.string_row("GV", host.version);
	if (host.met_ext) machine_locale_rows(out, host.locale, false);
	for (const GateMetricsPingEntry &entry : sorted_entries) {
		if (out.size() >= 0x8000) continue;
		out.bare_row("\tENTRY");
		out.number_row("BIP1", entry.ip[0], "\t\t");
		out.number_row("BIP2", entry.ip[1], "\t\t");
		out.number_row("BIP3", entry.ip[2], "\t\t");
		out.number_row("BIP4", entry.ip[3], "\t\t");
		out.number_row("MS", entry.ms, "\t\t");
		out.bare_row("\tENDENTRY");
	}
	return out.finish();
}

// [orig: Crypto_EncryptBuffer @0x437510 (the ADD chain, our nwu_decrypt); NapiSocket_SendTo
//  @0x62CFA0's encrypt flag (the envelope)]
std::vector<uint8_t> gate_metrics_datagram(std::string_view text) {
	std::vector<uint8_t> cipher(text.begin(), text.end());
	nwu_decrypt(cipher.data(), cipher.size(), "1010101");
	std::vector<uint8_t> packet(cipher.size() + 4);
	size_t packet_size = 0;
	if (napi_envelope_encode(cipher.data(), cipher.size(), packet.data(), packet.size(),
	                         &packet_size) != 0)
		return {};
	packet.resize(packet_size);
	return packet;
}

} // namespace opennova
