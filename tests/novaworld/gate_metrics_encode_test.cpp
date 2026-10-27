// The six METPROTOCOL reporters' encoders and the phase clock they print (D-NET-995):
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
#include <net/novaworld/nwu_lobby_session.h> // send_join_gate_ping_report
#include <net/novaworld/ping_sweep.h>        // the fold's kPingFailed / kPingNeverAttempted
#include <net/npwire/idatagram_socket.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
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

// The ping manager the join's PING reads: a sweep's rows in list order, the answered ones
// shell-sorted on the round trip. The order below is CPairList_ShellSort's, transcribed from its
// disasm (12 rows: the first gap is 4 from 12 / 9 = 1, then 1; an entry shifts past every
// earlier one whose round trip is not below its own, so equal round trips reorder).
// [orig: Server_SendPingMetricsToGate @0x511BF0 - the walk @0x511E86..0x511EB3;
//  CPairList_ShellSort @0x526C00 - the first gap @0x526C08..0x526C27, the stop @0x526C7B]
void check_ping_manager() {
	opennova::GatePingManager manager;
	const int ms[12] = {40, 15, 40, 7, 15, 90, 40, 3, 15, 61, 7, 40};
	std::vector<std::pair<int64_t, std::string>> rows;
	for (int i = 0; i < 12; ++i) rows.emplace_back(100 + i, "10.0.0." + std::to_string(i + 1));
	rows.emplace_back(200, "10.0.1.1");  // a row nothing answers
	rows.emplace_back(201, "0.0.0.0");   // the unreported address, never attempted
	manager.start_sweep(7, rows);
	expect(manager.entries.size() == 14 && manager.entries[3].ip[0] == 10 &&
			manager.entries[3].ip[3] == 4 && !manager.entries[3].answered,
			"a sweep's start: one pending entry per row, the address octet 0 first");
	for (int i = 0; i < 12; ++i) manager.apply_result(7, 100 + i, ms[i]);
	manager.apply_result(7, 200, opennova::kPingFailed);
	manager.apply_result(7, 201, opennova::kPingNeverAttempted);
	manager.apply_result(6, 200, 5); // a superseded sweep's result drops
	const std::vector<opennova::GateMetricsPingEntry> sorted =
			opennova::gate_ping_sorted_entries(manager);
	const int order[12] = {7, 3, 10, 4, 1, 8, 11, 2, 0, 6, 9, 5};
	bool in_order = sorted.size() == 12;
	for (size_t k = 0; in_order && k < 12; ++k)
		in_order = sorted[k].ip[3] == order[k] + 1 && sorted[k].ms == ms[order[k]];
	expect(in_order, "PING's entries: the answered rows in CPairList_ShellSort's order");
	opennova::GatePingManager four;
	four.start_sweep(1, {{1, "1.0.0.1"}, {2, "1.0.0.2"}, {3, "1.0.0.3"}, {4, "1.0.0.4"}});
	four.apply_result(1, 1, 20);
	four.apply_result(1, 2, 10);
	four.apply_result(1, 3, 20);
	four.apply_result(1, 4, 10);
	const std::vector<opennova::GateMetricsPingEntry> small = opennova::gate_ping_sorted_entries(four);
	expect(small.size() == 4 && small[0].ip[3] == 4 && small[1].ip[3] == 2 && small[2].ip[3] == 3 &&
			small[3].ip[3] == 1, "under 9 entries the one gap 1: equal round trips reverse");
	manager.start_sweep(8, {{300, "10.0.2.1"}});
	expect(manager.entries.size() == 1 && !manager.entries[0].answered,
			"the next sweep frees the old entries");
}

// The join's throttle and PING's own gates.
// [orig: MultiPlayer_JoinSessionStateMachine @0x56A320 - @0x56A4DE..0x56A506;
//  Server_SendPingMetricsToGate @0x511BF0 - @0x511C0B..0x511C39]
void check_ping_gates() {
	uint32_t tick = 0;
	expect(!opennova::gate_ping_report_due(tick, 0, 5000) && tick == 0, "METPING 0: no report");
	expect(!opennova::gate_ping_report_due(tick, -3, 5000) && tick == 0, "METPING below 0: none");
	expect(opennova::gate_ping_report_due(tick, 2, 5000) && tick == 5000, "the first report stamps");
	expect(!opennova::gate_ping_report_due(tick, 2, 125000) && tick == 5000,
			"exactly minutes x 60000 ms later: still throttled");
	expect(opennova::gate_ping_report_due(tick, 2, 125001) && tick == 125001, "one ms more: due");
	tick = 0xFFFFF000u;
	expect(!opennova::gate_ping_report_due(tick, 1, 0x00001000u), "the elapsed time wraps unsigned");
	tick = 0;
	expect(opennova::gate_ping_report_due(tick, 1, 0) && tick == 0,
			"a stamp of 0 reads as none at the next join");

	opennova::GateMetricsHost host;
	host.label = "jo";
	host.version = "V1.7.5.7";
	opennova::GatePingManager none;
	expect(opennova::gate_ping_report_text(none, 0x0100007Fu, 5, host, "us").empty(),
			"no entry at all: no PING");
	opennova::GatePingManager pending;
	pending.start_sweep(1, {{1, "10.0.0.1"}});
	expect(opennova::gate_ping_report_text(pending, 0, 5, host, "us").empty(), "no gate address: none");
	expect(opennova::gate_ping_report_text(pending, 0x0100007Fu, 0, host, "us").empty(),
			"METPING under 1: none");
	expect(opennova::gate_ping_report_text(pending, 0x0100007Fu, 5, host, "us") ==
			"METPROTOCOL 1\r\nBLOCK \"PING\"\r\n\tLABEL \"jo\"\r\n\tGCC \"us\"\r\n"
			"\tGV \"V1.7.5.7\"\r\nENDBLOCK\r\n",
			"an entry nothing answered still lets PING go, with no ENTRY group");
}

class RecordingSocket final : public opennova::IDatagramSocket {
public:
	std::vector<std::pair<opennova::PeerAddr, std::vector<uint8_t>>> sent;
	int recv_from(uint8_t *, std::size_t, opennova::PeerAddr &) override { return 0; }
	void send_to(const opennova::PeerAddr &to, const uint8_t *data, std::size_t len) override {
		sent.emplace_back(to, std::vector<uint8_t>(data, data + len));
	}
};

// The joiner's sender: the throttle ahead of PING's gates (a report the gates stop still stamps),
// the gate reply's METLABEL / METEXT, the datagram to the gate address at METIPPORT.
// [orig: MultiPlayer_JoinSessionStateMachine @0x56A320 - the stamp @0x56A506 ahead of the call
//  @0x56A50B; Server_SendPingMetricsToGate @0x511BF0 - the send @0x5120CE]
void check_join_ping_send() {
	opennova::NwuLobbySession::GateMemory memory;
	memory.response.met_label = "jo-metrics";
	memory.response.met_ping = 5;
	memory.response.met_port = 27000;
	opennova::GatePingManager pings;
	pings.start_sweep(1, {{1, "192.168.1.10"}, {2, "10.0.0.2"}});
	pings.apply_result(1, 1, 45);
	opennova::GateMetricsHost host;
	host.version = "V1.7.5.7";
	host.label = "ignored: the gate reply's label prints";
	RecordingSocket socket;
	opennova::send_join_gate_ping_report(socket, 1000, memory, pings, host, "us");
	expect(socket.sent.empty() && pings.report_tick == 1000,
			"no gate address: nothing sent, the throttle stamped all the same");
	memory.gate_address = 0x0A00A8C0u; // 192.168.0.10
	opennova::send_join_gate_ping_report(socket, 1000 + 300000, memory, pings, host, "us");
	expect(socket.sent.empty(), "inside METPING's five minutes: no report");
	opennova::send_join_gate_ping_report(socket, 1000 + 300001, memory, pings, host, "us");
	std::vector<uint8_t> cipher;
	opennova::GateMetricsReport report;
	expect(socket.sent.size() == 1 && socket.sent[0].first.ip == 0x0A00A8C0u &&
			socket.sent[0].first.port == 27000, "PING goes to the gate address at METIPPORT");
	expect(!socket.sent.empty() && read_datagram(socket.sent[0].second, cipher, report) &&
			report.block == "PING" && report.fields.size() == 3 &&
			report.fields[0].value == "jo-metrics" && report.fields[1].value == "us" &&
			report.fields[2].value == "V1.7.5.7" && report.entries.size() == 1 &&
			report.entries[0].size() == 5 && report.entries[0][0].value == "192" &&
			report.entries[0][4].value == "45",
			"PING: the gate reply's LABEL, CC.BIN, GV, the one answered entry");
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
	check_ping_manager();
	check_ping_gates();
	check_join_ping_send();
	check_server_tick();
	if (failures != 0) {
		std::fprintf(stderr, "gate_metrics_encode: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("gate_metrics_encode: ok\n");
	return 0;
}
