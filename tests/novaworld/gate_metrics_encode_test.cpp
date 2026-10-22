// The six METPROTOCOL reporters' encoders and the phase clock they print (D-NET-NEW-L9-22):
// each block's rows in retail's sprintf order, the datagram (Crypto_EncryptBuffer under "1010101"
// and the NAPI CRC envelope) pinned against the independently computed ID vector the decoder's
// test carries, every block read back by gate_metrics_decode, and CServerTick's transitions.
// [orig: Server_SendServerMetricsToGate @0x4FA4B0; Server_SendPlayerMetricsToGate @0x4FAA30;
//  Server_SendMetricsToGate @0x4FB0A0; Server_SendMissionMetrics @0x4FB3A0;
//  Server_SendPlayerMissionMetrics @0x4FB8F0; Server_SendPingMetricsToGate @0x511BF0;
//  CServerTick_Construct @0x5017D0; CServerTick_Reset @0x4F9F40; CServerTick_SetState @0x4FA090;
//  CServerTick_UpdateMaxPlayerCounts @0x4FA130; Crypto_EncryptBuffer @0x437510]

#include <net/napi/envelope.h>
#include <net/novaworld/gate_metrics.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char *why) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", why);
		++failures;
	}
}

std::vector<uint8_t> from_hex(const std::string &hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2)
		out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
	return out;
}

// A datagram back to its METPROTOCOL report: the envelope off, then the decoder (its SUB chain).
bool read_datagram(const std::vector<uint8_t> &dg, std::vector<uint8_t> &cipher,
                   opennova::GateMetricsReport &report) {
	cipher.assign(dg.size(), 0);
	size_t n = 0;
	if (opennova::napi_envelope_decode(dg.data(), dg.size(), cipher.data(), cipher.size(), &n) != 0)
		return false;
	cipher.resize(n);
	return opennova::gate_metrics_decode(cipher.data(), cipher.size(), report);
}

opennova::GateMetricsHost dedicated_host() {
	opennova::GateMetricsHost host;
	host.label = "unit";
	host.lobby = "jop_2_consumer";
	host.version = "V1.7.5.7";
	host.met_ext = true;
	host.locale.country_name = "United States";
	host.locale.language = "English";
	host.locale.tz_bias = 300;
	return host;
}

opennova::GateServerTick sample_tick() {
	opennova::GateServerTick tick;
	tick.start_ms = 0xFFFFF000u; // GetTickCount wraps between the stamp and the report
	tick.loading_ms = 12999;
	tick.waiting_ms = 3000;
	tick.playing_ms = 600001;
	tick.gameover_ms = 15500;
	tick.mission_completes = 2;
	tick.cycles = 1;
	tick.gcc = "us";
	tick.mission_file = "ash_g12a.bms";
	tick.max_players = 32;
	tick.most_players = 9;
	return tick;
}

opennova::GateMetricsPlayer sample_player() {
	opennova::GateMetricsPlayer player;
	player.version = "V1.7.5.7";
	player.country_code = "de";
	player.country_name = "Germany";
	player.language = "German";
	player.tz_bias = -60;
	player.pid = "123456";
	return player;
}

// RUNNING over the wrap: (0x00001000 - 0xFFFFF000) / 1000 unsigned = 8.
constexpr uint32_t kNow = 0x00001000u;

// The ID block against the cipher vector the decoder's test computed independently from
// Crypto_EncryptBuffer @0x437510: LABEL "unit", GCC "jop", GV "1.7.5.7", no locale rows.
void check_id_vector() {
	opennova::GateMetricsHost host;
	host.label = "unit";
	host.version = "1.7.5.7";
	const std::string text = opennova::gate_metrics_id_text(host, "jop");
	expect(text == "METPROTOCOL 1\r\nBLOCK \"ID\"\r\n\tLABEL \"unit\"\r\n\tGCC \"jop\"\r\n"
			"\tGV \"1.7.5.7\"\r\nENDBLOCK\r\n", "ID rows: LABEL, GCC, GV");
	const std::vector<uint8_t> vector = from_hex(
			"e3cd215ee183505798f54f19235fab39e8d6e42c9437443b329a320e5177c805bac2d9225b02daf67dd9"
			"7341e4078a08bca0c2c53be5cff8327206f6e5166ae5644d86bc5d07d0e31e7e18dbd6ec4b");
	std::vector<uint8_t> cipher;
	opennova::GateMetricsReport report;
	const std::vector<uint8_t> dg = opennova::gate_metrics_datagram(text);
	expect(dg.size() == text.size() + 4, "the envelope adds its four bytes, no NUL is sent");
	expect(read_datagram(dg, cipher, report) && cipher == vector,
			"the ID datagram's payload is Crypto_EncryptBuffer's bytes");
	expect(report.block == "ID" && report.fields.size() == 3 && report.fields[0].value == "unit",
			"the ID datagram decodes");
	// METEXT set: the machine locale's three rows close the block.
	const std::string with_locale = opennova::gate_metrics_id_text(dedicated_host(), "us");
	expect(with_locale == "METPROTOCOL 1\r\nBLOCK \"ID\"\r\n\tLABEL \"unit\"\r\n\tGCC \"us\"\r\n"
			"\tGV \"V1.7.5.7\"\r\n\tCOUNTRYNAME \"United States\"\r\n\tLANG \"English\"\r\n"
			"\tTZB 300\r\nENDBLOCK\r\n", "ID under METEXT carries COUNTRYNAME / LANG / TZB");
}

void check_server() {
	const std::string text =
			opennova::gate_metrics_server_text(dedicated_host(), "TDM", sample_tick(), kNow);
	expect(text == "METPROTOCOL 1\r\nBLOCK \"SERVER\"\r\n\tLABEL \"unit\"\r\n"
			"\tLOBBY \"jop_2_consumer\"\r\n\tGAMETYPE \"TDM\"\r\n\tRUNNING 8\r\n"
			"\tLOADINGMISSION 12\r\n\tWAITINGTOBEGIN 3\r\n\tPLAYING 600\r\n\tGAMEOVERSTATS 15\r\n"
			"\tNUMMISSIONCOMPLETES 2\r\n\tNUMCYCLES 1\r\n\tGCC \"us\"\r\n\tGV \"V1.7.5.7\"\r\n"
			"\tMAXPLAYERS 32\r\n\tMOSTPLAYERS 9\r\n\tCOUNTRYNAME \"United States\"\r\n"
			"\tLANG \"English\"\r\n\tTZB 300\r\nENDBLOCK\r\n", "SERVER rows in retail's order");
	opennova::GateMetricsHost plain = dedicated_host();
	plain.met_ext = false;
	const std::string no_locale =
			opennova::gate_metrics_server_text(plain, "TDM", sample_tick(), kNow);
	expect(no_locale.find("COUNTRYNAME") == std::string::npos &&
			no_locale.size() + std::strlen("\tCOUNTRYNAME \"United States\"\r\n\tLANG \"English\"\r\n"
			"\tTZB 300\r\n") == text.size(), "SERVER without METEXT drops the locale rows alone");
	std::vector<uint8_t> cipher;
	opennova::GateMetricsReport report;
	expect(read_datagram(opennova::gate_metrics_datagram(text), cipher, report) &&
			report.block == "SERVER" && report.fields.size() == 17 &&
			report.fields[3].name == "RUNNING" && report.fields[3].value == "8",
			"SERVER decodes");
}

void check_server_mission() {
	const std::string text = opennova::gate_metrics_server_mission_text(
			dedicated_host(), "TDM", sample_tick(), kNow);
	expect(text == "METPROTOCOL 1\r\nBLOCK \"SERVERMISSION\"\r\n\tLABEL \"unit\"\r\n"
			"\tLOBBY \"jop_2_consumer\"\r\n\tGAMETYPE \"TDM\"\r\n\tRUNNING 8\r\n"
			"\tLOADINGMISSION 12\r\n\tWAITINGTOBEGIN 3\r\n\tPLAYING 600\r\n\tGAMEOVERSTATS 15\r\n"
			"\tMISSIONFILENAME \"ash_g12a.bms\"\r\n\tGCC \"us\"\r\n\tGV \"V1.7.5.7\"\r\n"
			"\tMAXPLAYERS 32\r\n\tMOSTPLAYERS 9\r\n\tCOUNTRYNAME \"United States\"\r\n"
			"\tLANG \"English\"\r\n\tTZB 300\r\nENDBLOCK\r\n",
			"SERVERMISSION: no completion or cycle rows, the mission file after the phase totals");
	std::vector<uint8_t> cipher;
	opennova::GateMetricsReport report;
	expect(read_datagram(opennova::gate_metrics_datagram(text), cipher, report) &&
			report.block == "SERVERMISSION" && report.fields.size() == 16, "SERVERMISSION decodes");
}

void check_player() {
	const std::string text = opennova::gate_metrics_player_text(
			dedicated_host(), "TDM", "us", sample_tick(), sample_player(), kNow);
	expect(text == "METPROTOCOL 1\r\nBLOCK \"PLAYER\"\r\n\tLABEL \"unit\"\r\n"
			"\tLOBBY \"jop_2_consumer\"\r\n\tGAMETYPE \"TDM\"\r\n\tRUNNING 8\r\n"
			"\tLOADINGMISSION 12\r\n\tWAITINGTOBEGIN 3\r\n\tPLAYING 600\r\n\tGAMEOVERSTATS 15\r\n"
			"\tNUMMISSIONCOMPLETES 2\r\n\tNUMCYCLES 1\r\n\tPCC \"de\"\r\n\tGCC \"us\"\r\n"
			"\tPV \"V1.7.5.7\"\r\n\tGV \"V1.7.5.7\"\r\n\tPID \"123456\"\r\n"
			"\tGCOUNTRYNAME \"United States\"\r\n\tGLANG \"English\"\r\n\tGTZB 300\r\n"
			"\tPCOUNTRYNAME \"Germany\"\r\n\tGLANG \"German\"\r\n\tGTZB -60\r\nENDBLOCK\r\n",
			"PLAYER: the player's pair keeps the G prefix (GLANG / GTZB twice)");
	std::vector<uint8_t> cipher;
	opennova::GateMetricsReport report;
	expect(read_datagram(opennova::gate_metrics_datagram(text), cipher, report) &&
			report.block == "PLAYER" && report.fields.size() == 21 &&
			report.fields[19].name == "GLANG" && report.fields[19].value == "German" &&
			report.fields[20].value == "-60", "PLAYER decodes, duplicate keys in order");
}

void check_player_mission() {
	opennova::GateServerTick slot = sample_tick();
	slot.start_ms = kNow - 61999; // RUNNING 61
	slot.mission_file = "not printed: the mission tick's file is";
	slot.gcc = "not printed: the mission tick's GCC is";
	const std::string text = opennova::gate_metrics_player_mission_text(
			dedicated_host(), "TDM", sample_tick(), slot, sample_player(), kNow);
	expect(text == "METPROTOCOL 1\r\nBLOCK \"PLAYERMISSION\"\r\n\tLABEL \"unit\"\r\n"
			"\tLOBBY \"jop_2_consumer\"\r\n\tGAMETYPE \"TDM\"\r\n\tRUNNING 61\r\n"
			"\tLOADINGMISSION 12\r\n\tWAITINGTOBEGIN 3\r\n\tPLAYING 600\r\n\tGAMEOVERSTATS 15\r\n"
			"\tMISSIONFILENAME \"ash_g12a.bms\"\r\n\tPCC \"de\"\r\n\tGCC \"us\"\r\n"
			"\tPV \"V1.7.5.7\"\r\n\tGV \"V1.7.5.7\"\r\n\tPID \"123456\"\r\n"
			"\tGCOUNTRYNAME \"United States\"\r\n\tGLANG \"English\"\r\n\tGTZB 300\r\n"
			"\tPCOUNTRYNAME \"Germany\"\r\n\tPLANG \"German\"\r\n\tPTZB -60\r\nENDBLOCK\r\n",
			"PLAYERMISSION: the slot's clock, the mission tick's file and GCC, PLANG / PTZB");
	std::vector<uint8_t> cipher;
	opennova::GateMetricsReport report;
	expect(read_datagram(opennova::gate_metrics_datagram(text), cipher, report) &&
			report.block == "PLAYERMISSION" && report.fields.size() == 20,
			"PLAYERMISSION decodes");
}

void check_ping() {
	opennova::GateMetricsHost host;
	host.label = "jo";
	host.version = "V1.7.5.7";
	std::vector<opennova::GateMetricsPingEntry> entries(2);
	entries[0] = {{192, 168, 1, 10}, 45};
	entries[1] = {{10, 0, 0, 2}, 120};
	const std::string text = opennova::gate_metrics_ping_text(host, "cc", entries);
	expect(text == "METPROTOCOL 1\r\nBLOCK \"PING\"\r\n\tLABEL \"jo\"\r\n\tGCC \"cc\"\r\n"
			"\tGV \"V1.7.5.7\"\r\n\tENTRY\r\n\t\tBIP1 192\r\n\t\tBIP2 168\r\n\t\tBIP3 1\r\n"
			"\t\tBIP4 10\r\n\t\tMS 45\r\n\tENDENTRY\r\n\tENTRY\r\n\t\tBIP1 10\r\n\t\tBIP2 0\r\n"
			"\t\tBIP3 0\r\n\t\tBIP4 2\r\n\t\tMS 120\r\n\tENDENTRY\r\nENDBLOCK\r\n",
			"PING: one ENTRY group per sorted entry");
	std::vector<uint8_t> cipher;
	opennova::GateMetricsReport report;
	expect(read_datagram(opennova::gate_metrics_datagram(text), cipher, report) &&
			report.block == "PING" && report.entries.size() == 2, "PING decodes");
	// An entry goes in only while the text is under 0x8000 bytes: past it the rest are dropped
	// and the block still closes. Every entry here is 79 bytes (three-digit octets, a
	// five-digit MS). [orig: Server_SendPingMetricsToGate - the test @0x511EF0]
	std::vector<opennova::GateMetricsPingEntry> many(1000, {{255, 255, 255, 255}, 12345});
	const std::string capped = opennova::gate_metrics_ping_text(host, "cc", many);
	const size_t body = capped.size() - std::strlen("ENDBLOCK\r\n");
	expect(body >= 0x8000 && body - 79 < 0x8000, "the last entry starts under 0x8000 bytes");
	expect(read_datagram(opennova::gate_metrics_datagram(capped), cipher, report) &&
			report.entries.size() < many.size() && report.entries.size() > 400,
			"the capped PING still decodes");
}

void check_server_tick() {
	opennova::GateServerTick t;
	t.mission_file = "dropped by the construct";
	t.construct(100, "us", 32, 3, 2, 1);
	expect(t.state == 0 && t.start_ms == 100 && t.phase_start_ms == 0 && t.gcc == "us" &&
			t.mission_file.empty() && t.max_players == 32 && t.most_players == 3 &&
			t.most_team1 == 2 && t.most_team2 == 1, "construct");
	t.set_state(1, 200);
	expect(t.state == 1 && t.phase_start_ms == 200 && t.loading_ms == 0, "0 -> 1 accrues nothing");
	t.set_state(3, 1200);
	expect(t.loading_ms == 1000 && t.state == 3, "1 -> 3 accrues the loading time");
	t.set_state(3, 1500);
	expect(t.phase_start_ms == 1200, "the same phase is no change");
	t.set_state(4, 5200);
	expect(t.playing_ms == 4000 && t.mission_completes == 1 && t.cycles == 0,
			"entering 4 counts a mission complete");
	t.set_state(1, 6200);
	expect(t.gameover_ms == 1000 && t.cycles == 1 && t.mission_completes == 1, "4 -> 1 counts a cycle");
	t.set_state(2, 6700);
	t.set_state(4, 7700);
	t.set_state(3, 8700);
	expect(t.waiting_ms == 1000 && t.gameover_ms == 2000 && t.cycles == 1 &&
			t.mission_completes == 2, "4 -> 3 counts no cycle");
	t.close_phase(9700);
	expect(t.state == 0 && t.playing_ms == 5000 && t.phase_start_ms == 9700 && t.cycles == 1,
			"the report's close accrues the running phase and counts nothing");
	t.close_phase(9900);
	expect(t.phase_start_ms == 9700, "no running phase: the close leaves the stamp");
	t.update_max_player_counts(2, 5, 0);
	expect(t.most_players == 3 && t.most_team1 == 5 && t.most_team2 == 1, "the high marks only rise");
	t.mission_file = "ash_g12a.bms";
	t.reset(10000);
	expect(t.state == 0 && t.start_ms == 10000 && t.loading_ms == 0 && t.waiting_ms == 0 &&
			t.playing_ms == 0 && t.gameover_ms == 0 && t.mission_completes == 0 && t.cycles == 0 &&
			t.gcc.empty() && t.max_players == 0 && t.most_players == 0 && t.most_team1 == 0 &&
			t.mission_file == "ash_g12a.bms", "reset: the GCC and the caps zeroed, the file kept");
	t.construct(11000, std::string(40, 'x'), 16, 0, 0, 0);
	expect(t.gcc.size() == 31, "the GCC's copy keeps 31 characters");
}

} // namespace

int main() {
	check_id_vector();
	check_server();
	check_server_mission();
	check_player();
	check_player_mission();
	check_ping();
	check_server_tick();
	if (failures != 0) {
		std::fprintf(stderr, "gate_metrics_encode: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("gate_metrics_encode: ok\n");
	return 0;
}
