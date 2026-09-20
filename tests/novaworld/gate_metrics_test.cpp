#include <net/novaworld/gate_metrics.h>
#include <net/novaworld/gate_probe.h>
#include <net/novacrypto/nwu.h>

#include <cstdio>
#include <string>
#include <vector>

static bool expect(bool ok, const char *why) {
	if (!ok) std::fprintf(stderr, "FAIL: %s\n", why);
	return ok;
}
static bool decode(std::string text, opennova::GateMetricsReport &report) {
	opennova::nwu_decrypt(reinterpret_cast<uint8_t *>(text.data()), text.size(), "1010101");
	return opennova::gate_metrics_decode(reinterpret_cast<const uint8_t *>(text.data()), text.size(), report);
}
int main() {
	opennova::GateMetricsReport report;
	// Literal vector independently computed from Crypto_EncryptBuffer @0x437510.
	const std::string hex = "e3cd215ee183505798f54f19235fab39e8d6e42c9437443b329a320e5177c805bac2d9225b02daf67dd97341e4078a08bca0c2c53be5cff8327206f6e5166ae5644d86bc5d07d0e31e7e18dbd6ec4b";
	std::vector<uint8_t> bytes;
	for (size_t i = 0; i < hex.size(); i += 2)
		bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
	if (!expect(opennova::gate_metrics_decode(bytes.data(), bytes.size(), report) &&
		report.block == "ID" && report.fields.size() == 3 && report.fields[0].value == "unit",
		"retail cipher vector decodes ID report")) return 1;
	for (const char *block : {"SERVER", "PLAYER", "ID", "SERVERMISSION", "PLAYERMISSION", "PING"}) {
		const std::string text = std::string("METPROTOCOL 1\r\nBLOCK \"") + block +
			"\"\r\n\tLABEL \"\"\r\n\tPID 4294967295\r\n\tGLANG \"en\"\r\n"
			"\tGTZB -300\r\n\tGLANG \"fr\"\r\n\tGTZB 60\r\nENDBLOCK\r\n";
		if (!expect(decode(text, report) && report.block == block && report.fields.size() == 6 &&
			report.fields[0].quoted && report.fields[0].value.empty() &&
			report.fields[3].value == "-300" && report.fields[4].value == "fr",
			"all reporters preserve ordered duplicate keys and signed/unsigned values")) return 1;
		for (size_t n = 0; n < text.size(); ++n)
			if (!expect(!decode(text.substr(0, n), report), "truncated report rejected")) return 1;
		if (!expect(!decode(text + '\0', report), "report has no trailing NUL")) return 1;
	}
	const auto gate = opennova::gate_probe_build(opennova::GATE_PROBE_TAG_JOINTOPS);
	if (!expect(!opennova::gate_metrics_decode(gate.data(), gate.size(), report),
		"ordinary GATEAPI probe is not metrics")) return 1;
	for (const char *row : {"LABEL \"x\"", "\tLABEL \"x", "\tGTZB -", "\tPID 12x"})
		if (!expect(!decode(std::string("METPROTOCOL 1\r\nBLOCK \"ID\"\r\n") + row +
			"\r\nENDBLOCK\r\n", report), "malformed field rejected")) return 1;
	if (!expect(report.block == "PING", "failure leaves prior report intact")) return 1;
	// The sixth reporter: Server_SendPingMetricsToGate @0x511BF0 nests one
	// ENTRY/ENDENTRY group per sorted ping entry after the LABEL/GCC/GV rows
	// (plus the dedicated-only COUNTRYNAME/LANG/TZB block).
	const std::string ping_head = "METPROTOCOL 1\r\nBLOCK \"PING\"\r\n\tLABEL \"jo\"\r\n"
		"\tGCC \"cc\"\r\n\tGV \"1.7.5.3\"\r\n";
	const std::string entry_a = "\tENTRY\r\n\t\tBIP1 192\r\n\t\tBIP2 168\r\n\t\tBIP3 1\r\n"
		"\t\tBIP4 10\r\n\t\tMS 45\r\n\tENDENTRY\r\n";
	const std::string entry_b = "\tENTRY\r\n\t\tBIP1 10\r\n\t\tBIP2 0\r\n\t\tBIP3 0\r\n"
		"\t\tBIP4 2\r\n\t\tMS 120\r\n\tENDENTRY\r\n";
	const std::string dedicated_ping = ping_head + "\tCOUNTRYNAME \"United States\"\r\n"
		"\tLANG \"English\"\r\n\tTZB 480\r\n" + entry_a + "ENDBLOCK\r\n";
	if (!expect(decode(dedicated_ping, report) && report.fields.size() == 6 &&
		report.fields[5].value == "480" && report.entries.size() == 1,
		"dedicated PING carries the locale rows before its entries")) return 1;
	if (!expect(decode(ping_head + "ENDBLOCK\r\n", report) && report.entries.empty() &&
		report.fields.size() == 3, "an entry-less PING decodes with no entries")) return 1;
	if (!expect(decode(ping_head + entry_a + entry_b + "ENDBLOCK\r\n", report) &&
		report.block == "PING" && report.fields.size() == 3 &&
		report.fields[2].name == "GV" && report.fields[2].value == "1.7.5.3" &&
		report.entries.size() == 2 && report.entries[0].size() == 5 &&
		report.entries[0][0].name == "BIP1" && report.entries[0][0].value == "192" &&
		!report.entries[0][0].quoted && report.entries[1][4].name == "MS" &&
		report.entries[1][4].value == "120",
		"PING decodes its nested ENTRY groups in order")) return 1;
	for (const char *body : {
			"\t\tBIP1 1\r\n",                       // nested row outside an entry
			"\tENTRY\r\n\t\tMS 5\r\n",              // unclosed at ENDBLOCK
			"\tENDENTRY\r\n",                       // close without open
			"\tENTRY\r\n\tENTRY\r\n\tENDENTRY\r\n", // nested open
			"\tENTRY\r\n\tMS 5\r\n\tENDENTRY\r\n",  // flat row inside an entry
			"\tENTRY \r\n\tENDENTRY\r\n",           // ENTRY is a bare keyword row
			"\tENTRY\r\n\t\t\tMS 5\r\n\tENDENTRY\r\n"}) // three tabs
		if (!expect(!decode(ping_head + body + "ENDBLOCK\r\n", report),
			"malformed entry grammar rejected")) return 1;
	if (!expect(report.entries.size() == 2, "failure leaves prior entries intact")) return 1;
	return 0;
}
