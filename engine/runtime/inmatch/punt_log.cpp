// The punt and cheat logs (punt_log.h).
#include <runtime/inmatch/punt_log.h>

#include <runtime/inmatch/napi_np_connection.h>

#include <cstdio>

namespace opennova::inmatch {

namespace {

constexpr const char *kPuntFile = "_PUNT.TXT";   // [orig: @0x7c998c]
constexpr const char *kCheatFile = "_CHEAT.TXT"; // [orig: @0x7c99ac]
constexpr const char *kPuntDumpFile = "punt.log"; // [orig: @0x7cf454]

// [orig: g_PuntReasonStrings @0x82f148 — the 35 pointers @0x82f148..0x82f1d0]
constexpr const char *kReasons[] = {
	"NULL",
	"bad weapon ammo type",
	"bad weapon fire delay",
	"bad weapon scope mag",
	"bad weapon attribs",
	"bad ammo clip size",
	"player killing",
	"player being dead too long",
	"bad color map CRC",
	"bad height map CRC",
	"bad tilestrip CRC",
	"bad detail map CRC",
	"bad height scale",
	"bad water CRC",
	"bad sky CRC",
	"bad fog CRC",
	"attrib request not received",
	"attrib received doesnt match servers",
	"bad move Z",
	"bad move XY",
	"bad ammo CRC",
	"bad weapon CRC",
	"bad Z Prolonged flight",
	"bad Z Prolonged paraglide",
	"timestamp request not received",
	"timestamp error",
	"squirrely things afoot",
	"crc mismatch",
	"need a quicker CRC response",
	"bad weapon fire rate",
	"bad game frame number",
	"mole",
	"skin",
	"CPT",
	"need a quicker skin response",
};

std::tm local_tm(std::time_t t) {
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	return tm;
}

// The MSVC CRT asctime layout: "Www Mmm dd hh:mm:ss yyyy\n", the day zero
// padded (the C standard's space-padded "%3d" is not what the retail CRT
// writes). English names regardless of the locale, as the CRT's own tables.
std::string asctime_layout(const std::tm &tm) {
	static const char *const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%s %s %02d %02d:%02d:%02d %04d\n",
			kDays[(tm.tm_wday % 7 + 7) % 7], kMonths[(tm.tm_mon % 12 + 12) % 12], tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900);
	return buf;
}

} // namespace

std::string crt_asctime_text(std::time_t t) { return asctime_layout(local_tm(t)); }

std::string napi_time_string(std::time_t t) {
	// ctime is asctime(localtime); the strip drops every trailing non-digit,
	// the newline [orig: @0x61d133..0x61d151].
	std::string s = crt_asctime_text(t);
	while (!s.empty() && !(s.back() >= '0' && s.back() <= '9')) s.pop_back();
	return s;
}

std::string punt_log_line(const std::string &time, int32_t slot_index, const std::string &name,
		const std::string &reason, const std::string &details) {
	char slot[16];
	std::snprintf(slot, sizeof(slot), "%2.2ld", static_cast<long>(slot_index));
	return time + " : " + slot + " : \"" + name + "\" : " + reason + " : " + details;
}

std::string punt_log_line(const std::string &time, const std::string &reason,
		const std::string &details) {
	return time + " : " + reason + " : " + details;
}

std::string cheat_log_line(const std::string &time, const std::string &description) {
	return time + " : " + description;
}

const char *punt_reason_name(int type) {
	constexpr int count = static_cast<int>(sizeof(kReasons) / sizeof(kReasons[0]));
	return type >= 0 && type < count ? kReasons[type] : "";
}

std::string punt_log_dump_text(const std::vector<PuntRecord> &rows,
		const std::function<std::string(std::time_t)> &asctime_text) {
	std::string text;
	for (const PuntRecord &row : rows) {
		if (row.type == 0) continue; // [orig: `if (*(_BYTE *)entry)` @0x500280]
		char line[512];
		// [orig: "Player %s(%d.%d.%d.%d) was punted from mission %s for %s on %s"
		//  @0x7cf414 — the reason g_PuntReasonStrings[(char)type] @0x500297]
		std::snprintf(line, sizeof(line), "Player %s(%d.%d.%d.%d) was punted from mission %s for %s on ",
				row.name.c_str(), row.ip[0], row.ip[1], row.ip[2], row.ip[3], row.mission.c_str(),
				punt_reason_name(row.type));
		text += line;
		text += asctime_text ? asctime_text(static_cast<std::time_t>(row.time)) : std::string();
		// [orig: "Player CRC = %X, Server CRC = %X, Filename = %s\n\n" @0x7cf3e0]
		std::snprintf(line, sizeof(line), "Player CRC = %X, Server CRC = %X, Filename = %s\n\n",
				row.player_crc, row.server_crc, row.filename.c_str());
		text += line;
	}
	// The text-mode stream writes each '\n' as "\r\n" [orig: fopen("punt.log",
	// "w") @0x50026b].
	std::string disk;
	disk.reserve(text.size() + text.size() / 16);
	for (const char c : text) {
		if (c == '\n') disk.push_back('\r');
		disk.push_back(c);
	}
	return disk;
}

PuntLog::PuntLog(ServerFileSink &files)
	: files_(files), now_([] { return napi_time_string(std::time(nullptr)); }) {}

void PuntLog::append_line(const char *file, const std::string &line) {
	// NapiFile_AppendLine: open (or create), seek to the end, write the text
	// and the line ending [orig: @0x61c020].
	files_.write(file, line + kNapiLineEnding, /*append=*/true);
}

void PuntLog::arm_punt_log() {
	files_.remove(kPuntFile);  // [orig: DeleteFileA @0x4a784a / @0x4a7880]
	punt_armed_ = true;        // [orig: @0x4a7844 / @0x4a787a]
	// Server_WritePuntLog(0, "START", "") [orig: @0x4a785b / @0x4a7891]
	append_line(kPuntFile, punt_log_line(now_(), "START", ""));
}

void PuntLog::arm_cheat_log() {
	files_.remove(kCheatFile); // [orig: DeleteFileA @0x4a7819]
	cheat_armed_ = true;       // [orig: @0x4a7813]
	// Server_LogCheatDetection(0, "START") [orig: @0x4a7825 -> the line
	//  @0x4f9da4, the append @0x4f9dca]
	append_line(kCheatFile, cheat_log_line(now_(), "START"));
}

void PuntLog::write_punt(int32_t slot_index, const std::string &name, const char *reason,
		const char *details) {
	// [orig: Server_WritePuntLog @0x4f9c70 — the gate @0x4f9cb3, the slot form
	//  @0x4f9cf1, the append @0x4f9d1a]
	if (!punt_armed_) return;
	append_line(kPuntFile, punt_log_line(now_(), slot_index, name, reason, details));
}

void server_logs_punt(const ServerLogs &logs, const NapiNPConnection &conn, const char *reason) {
	if (logs.punt == nullptr) return;
	if (conn.link.mode == replication::TransportMode::Loopback || conn.host_disconnect_sent) return;
	logs.punt->write_punt(conn.reply.player_slot, conn.reply.player_name, reason, "");
}

void PuntLog::clear_records() {
	records_.assign(static_cast<size_t>(kPuntRecordCount), PuntRecord{});
}

void PuntLog::dump(const std::function<std::string(std::time_t)> &asctime_text) {
	// fopen("punt.log", "w") truncates; a file that does not open writes nothing
	// [orig: @0x50026b..0x500277].
	files_.write(kPuntDumpFile, punt_log_dump_text(records_, asctime_text), /*append=*/false);
}

} // namespace opennova::inmatch
