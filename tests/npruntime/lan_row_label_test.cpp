// The LAN list's row text (npwire/lan_discovery.h lan_discovery_row_label): the
// two sprintf forms, the first-byte expansion test, the 31-byte expansion copy
// and the 255-byte row.
// [orig: UI_ProcessLANSessionStateMachine @0x558de0 -- @0x559448..0x559458,
//  @0x559471, @0x559493 / @0x5594b9, @0x5594cc..0x5594dc]

#include <net/npwire/lan_discovery.h>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect(bool cond, const char *msg) {
	if (cond) return;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
}

} // namespace

int main() {
	using opennova::lan_discovery_row_label;
	expect(lan_discovery_row_label("current", "", 1, 4) == "current (1/4)", "no expansion: the short form");
	expect(lan_discovery_row_label("biggy", "JOTR", 12, 32) == "biggy - JOTR (12/32)",
			"an expansion: the long form");
	// The test is the first byte, so a blank variant still takes the long form,
	// and a nameless session prints its empty name.
	expect(lan_discovery_row_label("srv", " ", 0, 8) == "srv -   (0/8)", "a blank expansion is advertised");
	expect(lan_discovery_row_label("", "", 0, 8) == " (0/8)", "an empty name prints empty");
	// %ld of the record's signed words.
	expect(lan_discovery_row_label("neg", "", -1, 8) == "neg (-1/8)", "the counts print signed");
	// The record's 32-byte expansion field: its first 31 bytes.
	const std::string long_variant(40, 'x');
	expect(lan_discovery_row_label("a", long_variant, 1, 2) == "a - " + std::string(31, 'x') + " (1/2)",
			"the expansion copy keeps 31 bytes");
	// A %s stops at the first NUL.
	expect(lan_discovery_row_label(std::string("ab\0cd", 5), std::string("e\0f", 3), 1, 2) == "ab - e (1/2)",
			"a NUL ends each string");
	// The row keeps 255 bytes.
	const std::string label = lan_discovery_row_label(std::string(300, 'n'), "", 1, 2);
	expect(label.size() == 255 && label == std::string(255, 'n'), "the row keeps the first 255 bytes");
	// cp1252 bytes pass through untouched.
	expect(lan_discovery_row_label("Spie\xDF", "", 2, 4) == "Spie\xDF (2/4)", "code-page bytes pass through");
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("lan_row_label: ok\n");
	return 0;
}
