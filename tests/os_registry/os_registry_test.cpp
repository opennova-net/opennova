// The registry seam (base/os_registry): the key paths retail's helpers refuse (no subkey, a root
// none of its six names), and, where the platform has no registry, the stand-in file's round trip
// (the text to its first NUL, the read's room counting the NUL, a missing or unnamed stand-in
// read as no value). On Windows the test only reads a key that does not exist, so it never
// writes the machine's registry.
// [orig: NapiRegistry_OpenKeyPath @0x619e80 - the component count @0x619ee7, the root names
//  @0x61a010..0x61a0ae; CScrollWnd_SetRange @0x619d70; NapiRegistry_SetStringValue @0x619dc0]

#include <base/os_registry/os_registry.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <cstdio>
#include <string>

namespace {

int refused_paths() {
	std::string out = "unchanged";
	// One component, no subkey: refused.
	TEST_EXPECT(
			!opennova::os_registry_read_string("HKEY_LOCAL_MACHINE", "CDKIID", 0x2000, "", out));
	TEST_EXPECT(
			!opennova::os_registry_read_string("HKEY_LOCAL_MACHINE\\", "CDKIID", 0x2000, "", out));
	// A root none of retail's six names.
	TEST_EXPECT(!opennova::os_registry_read_string(
			"HKLM\\Software\\NovaLogic", "CDKIID", 0x2000, "", out));
	TEST_EXPECT(!opennova::os_registry_write_string("HKEY_NOWHERE\\Software", "CDKIID", "X", ""));
	// No value name.
	TEST_EXPECT(!opennova::os_registry_read_string(
			"HKEY_LOCAL_MACHINE\\Software", "", 0x2000, "", out));
	TEST_EXPECT(out == "unchanged");
	return 0;
}

#if defined(_WIN32)
// A key no install makes: nothing read, `out` left alone. (Read only: the test never writes.)
int absent_value() {
	std::string out = "unchanged";
	TEST_EXPECT(!opennova::os_registry_read_string(
			"hkey_local_machine\\Software\\OpenNova\\os_registry_test_absent", "CDKIID", 0x2000, "",
			out));
	TEST_EXPECT(out == "unchanged");
	return 0;
}
#else
// The stand-in file: the text stored to its first NUL, read back whole, the room counting the NUL.
int stand_in_round_trip() {
	test_temp::TempDir dir("os_registry");
	const std::string file = dir.file("cdkiid.dat");
	const std::string key =
			"HKEY_LOCAL_MACHINE\\Software\\NovaLogic\\Delta Force Xtreme 2\\1.00.0000";
	std::string out = "unchanged";
	TEST_EXPECT(!opennova::os_registry_read_string(key, "CDKIID", 0x2000, file, out));
	TEST_EXPECT(out == "unchanged");
	TEST_EXPECT(
			opennova::os_registry_write_string(key, "CDKIID", std::string("ABCDEF\0GH", 9), file));
	TEST_EXPECT(
			opennova::os_registry_read_string(key, "CDKIID", 0x2000, file, out) && out == "ABCDEF");
	// Six characters and the NUL need a room of seven.
	std::string tight = "unchanged";
	TEST_EXPECT(!opennova::os_registry_read_string(key, "CDKIID", 6, file, tight) &&
			tight == "unchanged");
	TEST_EXPECT(
			opennova::os_registry_read_string(key, "CDKIID", 7, file, tight) && tight == "ABCDEF");
	// No stand-in named: no value, nothing stored.
	TEST_EXPECT(!opennova::os_registry_read_string(key, "CDKIID", 0x2000, "", out));
	TEST_EXPECT(!opennova::os_registry_write_string(key, "CDKIID", "X", ""));
	return 0;
}
#endif

} // namespace

int main() {
	int rc = 0;
	rc |= refused_paths();
#if defined(_WIN32)
	rc |= absent_value();
#else
	rc |= stand_in_round_trip();
#endif
	if (rc == 0)
		std::printf("OK: os_registry\n");
	return rc;
}
