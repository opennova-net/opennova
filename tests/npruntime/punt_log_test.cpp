// The punt and cheat logs (inmatch/punt_log.h): the switches' START lines,
// a synthetic punt's _PUNT.TXT line, the line ending, the CRT time text, the
// reason table, and punt.log's text over a synthetic table row (and the empty
// file the real, never-filled table dumps).
// [orig: Game_ParseCommandLineAndInit @0x4a77fc..0x4a7891; Server_WritePuntLog
//  @0x4f9c70; Server_LogCheatDetection @0x4f9d40; NapiFile_AppendLine
//  @0x61c020; Napi_GetTimeString @0x61d0e0; Server_DumpPuntLogToFile @0x500260;
//  g_PuntReasonStrings @0x82f148]
#include <runtime/inmatch/punt_log.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

class MemoryFiles final : public inmatch::ServerFileSink {
public:
	std::map<std::string, std::string> files;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override { files.erase(name); }
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		std::string &f = files[name];
		if (!append) f.clear();
		f.append(bytes.data(), bytes.size());
		return true;
	}
};

constexpr const char *kNow = "Mon Oct 05 12:34:56 2026";

void test_punt_lines() {
	MemoryFiles files;
	files.files["_PUNT.TXT"] = "an earlier run\r\n";
	inmatch::PuntLog log(files);
	log.set_time_text([] { return std::string(kNow); });
	// Unarmed: Server_WritePuntLog writes nothing.
	log.write_punt(3, "Joe", "WCRC", "");
	CHECK(files.files["_PUNT.TXT"] == "an earlier run\r\n");

	log.arm_punt_log(); // the switch deletes the file, then the START line
	CHECK(log.punt_log_armed());
	CHECK(files.files["_PUNT.TXT"] == "Mon Oct 05 12:34:56 2026 : START : \r\n");
	log.write_punt(3, "Joe", "WCRC", "");
	log.write_punt(12, "Ann", "#S>9", "");
	CHECK(files.files["_PUNT.TXT"] ==
			"Mon Oct 05 12:34:56 2026 : START : \r\n"
			"Mon Oct 05 12:34:56 2026 : 03 : \"Joe\" : WCRC : \r\n"
			"Mon Oct 05 12:34:56 2026 : 12 : \"Ann\" : #S>9 : \r\n");
	CHECK(files.files.count("_CHEAT.TXT") == 0);

	files.files["_CHEAT.TXT"] = "old";
	log.arm_cheat_log();
	CHECK(log.cheat_log_armed());
	CHECK(files.files["_CHEAT.TXT"] == "Mon Oct 05 12:34:56 2026 : START\r\n");

	CHECK(inmatch::punt_log_line("T", 7, "N", "ACRC", "d") == "T : 07 : \"N\" : ACRC : d");
	CHECK(inmatch::punt_log_line("T", 123, "N", "R", "") == "T : 123 : \"N\" : R : ");
	CHECK(inmatch::punt_log_line("T", "START", "") == "T : START : ");
	CHECK(inmatch::cheat_log_line("T", "START") == "T : START");
}

void test_time_text() {
	// 2026-01-05 12:00:00 UTC: the 5th or the 6th in every zone, so the day
	// shows MSVC's zero padding.
	const std::time_t t = 1767614400;
	const std::string asc = inmatch::crt_asctime_text(t);
	CHECK(asc.size() == 25 && asc.back() == '\n');
	CHECK(asc.substr(4, 3) == "Jan" && asc[8] == '0');
	CHECK(asc.substr(20, 4) == "2026");
	const std::string napi = inmatch::napi_time_string(t);
	CHECK(napi == asc.substr(0, 24)); // the trailing newline cut
}

void test_reasons_and_dump() {
	CHECK(std::string(inmatch::punt_reason_name(0)) == "NULL");
	CHECK(std::string(inmatch::punt_reason_name(9)) == "bad height map CRC");
	CHECK(std::string(inmatch::punt_reason_name(10)) == "bad tilestrip CRC");
	CHECK(std::string(inmatch::punt_reason_name(21)) == "bad weapon CRC");
	CHECK(std::string(inmatch::punt_reason_name(34)) == "need a quicker skin response");
	CHECK(std::string(inmatch::punt_reason_name(35)).empty());
	CHECK(std::string(inmatch::punt_reason_name(-1)).empty());

	inmatch::PuntRecord row;
	row.type = 9;
	row.time = 100;
	row.name = "Joe";
	row.ip = {10, 0, 0, 7};
	row.player_crc = 0xDEADBEEF;
	row.server_crc = 0x1234ABCD;
	row.filename = "Elev";
	row.mission = "mission.bms";
	std::vector<inmatch::PuntRecord> rows(3);
	rows[1] = row;
	const auto fixed_asctime = [](std::time_t) { return std::string("Thu Jan 01 00:01:40 1970\n"); };
	CHECK(inmatch::punt_log_dump_text(rows, fixed_asctime) ==
			"Player Joe(10.0.0.7) was punted from mission mission.bms for bad height map CRC on "
			"Thu Jan 01 00:01:40 1970\r\n"
			"Player CRC = DEADBEEF, Server CRC = 1234ABCD, Filename = Elev\r\n\r\n");

	// The host's own table is never filled: its dump rewrites punt.log empty.
	MemoryFiles files;
	files.files["punt.log"] = "stale";
	inmatch::PuntLog log(files);
	log.clear_records();
	CHECK(log.records().size() == static_cast<size_t>(inmatch::kPuntRecordCount));
	log.dump(fixed_asctime);
	CHECK(files.files["punt.log"].empty());
}

} // namespace

int main() {
	test_punt_lines();
	test_time_text();
	test_reasons_and_dump();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("punt log: ok\n");
	return 0;
}
