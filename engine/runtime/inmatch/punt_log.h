#pragma once

// THE PUNT AND CHEAT LOGS: the host's text logs of the players it punts.
//
//   _PUNT.TXT  — one appended line per punt the host logs, armed by the
//                `/PUNT.TXT` or `/PUNTLOG` switch, each of which deletes the
//                file and writes a START line [orig: Game_ParseCommandLineAndInit
//                @0x4a782d..0x4a7891 -> g_PuntLogEnabled @0xC86FBC;
//                Server_WritePuntLog @0x4f9c70]. The lines come from the
//                weapon-table CRC reply (WCRC), the ammo CRC reply (ACRC), the
//                join's two map checksums (HMCRC / CMCRC) and the violation
//                sweep's suicide arm (#S>9), each written only for a slot that
//                is not the host's own, not a bot, not already punted and with
//                `/NOPUNT` unset [orig: Server_ValidateWeaponCRC @0x50200e,
//                NapiNPServerMsg_HandleAntiCheatCRCCheck @0x5021da,
//                Server_OnPlayerJoin @0x51a8e1 / @0x51a922,
//                Server_CheckPlayerViolations @0x51ad0d].
//   _CHEAT.TXT — armed by `/CHEATLOG`, which deletes it and writes the START
//                line; nothing else in the image writes it [orig:
//                @0x4a77fc..0x4a7825 -> g_CheatLogEnabled @0xC86FC0;
//                Server_LogCheatDetection @0x4f9d40, whose one caller is that
//                switch].
//   punt.log   — the dump of the host's in-memory punt table, rewritten whole
//                [orig: Server_DumpPuntLogToFile @0x500260]. The table is
//                cleared at every session init and nothing ever stores a row
//                (its only data references are that clear and the dump), so
//                the file retail writes is always empty. The dump runs from the
//                Server-class `PuntLog` action (catalog row 86, code 48, no
//                default key) and from the shutdown, behind the error-log flag
//                no caller sets [orig: Input_HandleActionBinding case 48
//                @0x49b833; Game_ShutdownSubsystems @0x4a53f9 -> sub_53C770
//                @0x53c770 gated on dword_24E5DD8].
//
// Every write goes through the embedder's file seam (server_files.h).

#include <runtime/inmatch/server_files.h>

#include <array>
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace opennova::inmatch {

// The CRT ctime text with its trailing non-digits cut: "Www Mmm dd hh:mm:ss
// yyyy" (MSVC's zero-padded day) [orig: Napi_GetTimeString @0x61d0e0 —
// _ctime64 @0x61d0fb, the trailing strip @0x61d131..0x61d151].
std::string napi_time_string(std::time_t t);
// The CRT asctime text, newline included [orig: asctime(_localtime64(..))
// @0x500289..0x50028f].
std::string crt_asctime_text(std::time_t t);

// The line ending NapiFile_AppendLine adds after every line [orig:
// g_NapiFileLineEnding @0x849d38 -> "\r\n" @0x7c1310].
inline constexpr const char *kNapiLineEnding = "\r\n";

// One _PUNT.TXT line, line ending excluded: with a slot
// `"<time> : NN : \"<name>\" : <reason> : <details>"`, without one
// `"<time> : <reason> : <details>"` [orig: "%s : %2.2ld : \"%s\" : %s : %s"
// @0x7cee98, "%s : %s : %s" @0x7cee88].
std::string punt_log_line(const std::string &time, int32_t slot_index, const std::string &name,
		const std::string &reason, const std::string &details);
std::string punt_log_line(const std::string &time, const std::string &reason,
		const std::string &details);
// One _CHEAT.TXT line: `"<time> : <description>"` without a slot
// [orig: "%s : %s" @0x7ceeb8; the slot form "%s : %2.2ld : \"%s\" : %s" @0x7ceec0
// has no caller].
std::string cheat_log_line(const std::string &time, const std::string &description);

// The punt table's 112-byte row [orig: g_PuntLog @0xC74480, 256 rows to
// g_EntityLimitTable @0xC7B480; the row layout Server_PlayerPuntCRCMisMatch
// @0x50f380 builds on its stack and the dump reads]: +0 the reason type, +8
// the time, +16 name[32], +48 the IPv4 octets, +52 the player CRC, +56 the
// server CRC, +60 filename[16], +76 mission[36].
struct PuntRecord {
	int8_t type = 0; // 0 = an empty row
	int64_t time = 0;
	std::string name;
	std::array<uint8_t, 4> ip{};
	uint32_t player_crc = 0;
	uint32_t server_crc = 0;
	std::string filename;
	std::string mission;
};
inline constexpr int kPuntRecordCount = 256;

// The reason names a row's type indexes [orig: g_PuntReasonStrings @0x82f148,
// 35 entries, "NULL" first]; "" past the table.
const char *punt_reason_name(int type);

// punt.log's text: per non-empty row, `"Player <name>(a.b.c.d) was punted from
// mission <mission> for <reason> on <asctime>"` then `"Player CRC = <X>, Server
// CRC = <X>, Filename = <file>\n\n"` [orig: the two fprintf @0x5002c2 /
// @0x5002d9]. Retail opens the file in text mode ("w"), so every '\n' reaches
// the disk as "\r\n"; the text here is the disk's. `asctime_text` renders a
// row's time (crt_asctime_text in production).
std::string punt_log_dump_text(const std::vector<PuntRecord> &rows,
		const std::function<std::string(std::time_t)> &asctime_text);

class PuntLog {
public:
	explicit PuntLog(ServerFileSink &files);

	// The clock the lines stamp (napi_time_string over the current time by
	// default); a test pins it.
	void set_time_text(std::function<std::string()> now) { now_ = std::move(now); }

	// `/PUNT.TXT` / `/PUNTLOG`: delete _PUNT.TXT, arm, append the START line
	// [orig: @0x4a783f..0x4a785b / @0x4a7875..0x4a7891].
	void arm_punt_log();
	// `/CHEATLOG`: delete _CHEAT.TXT, arm, append the START line, which retail
	// also posts to its own SYSTEM ring (Chat_AddSystemMessage @0x4f9dd4).
	void arm_cheat_log();
	bool punt_log_armed() const { return punt_armed_; }
	bool cheat_log_armed() const { return cheat_armed_; }

	// Server_WritePuntLog(slot, reason, details) for a slot: one _PUNT.TXT line
	// while armed.
	void write_punt(int32_t slot_index, const std::string &name, const char *reason,
			const char *details);

	// The punt table: cleared at every session init [orig: Server_InitNewRoundState
	// memset(&g_PuntLog, 0, 0x7000) @0x51c990]; no producer stores a row.
	void clear_records();
	const std::vector<PuntRecord> &records() const { return records_; }
	// Server_DumpPuntLogToFile: rewrite punt.log from the table.
	void dump(const std::function<std::string(std::time_t)> &asctime_text = crt_asctime_text);

private:
	void append_line(const char *file, const std::string &line);

	ServerFileSink &files_;
	std::function<std::string()> now_;
	bool punt_armed_ = false;
	bool cheat_armed_ = false;
	std::vector<PuntRecord> records_;
};

} // namespace opennova::inmatch
