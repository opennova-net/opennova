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
	for (const char *block : {"SERVER", "PLAYER", "ID", "SERVERMISSION", "PLAYERMISSION"}) {
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
	const auto gate = opennova::gate_probe_build();
	if (!expect(!opennova::gate_metrics_decode(gate.data(), gate.size(), report),
		"ordinary GATEAPI probe is not metrics")) return 1;
	for (const char *row : {"LABEL \"x\"", "\tLABEL \"x", "\tGTZB -", "\tPID 12x"})
		if (!expect(!decode(std::string("METPROTOCOL 1\r\nBLOCK \"ID\"\r\n") + row +
			"\r\nENDBLOCK\r\n", report), "malformed field rejected")) return 1;
	if (!expect(report.block == "PLAYERMISSION", "failure leaves prior report intact")) return 1;
	return 0;
}
