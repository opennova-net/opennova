// The registry seam (base/os_registry): the key paths retail's helpers refuse (no subkey, a root
// none of its six names), the VirtualStore copy UAC keeps of a key for a legacy 32-bit executable
// whose token is not elevated, and, where the platform has no registry, the stand-in file's round
// trip (the text to its first NUL, the read's room counting the NUL, a missing or unnamed stand-in
// read as no value). On Windows the test reads a machine key that does not exist, and writes only
// its own scratch key under HKEY_CURRENT_USER\Software\OpenNovaTest (common/scratch_registry.h),
// over which it runs the virtualized read and write.
// [orig: NapiRegistry_OpenKeyPath @0x619e80 - the component count @0x619ee7, the root names
//  @0x61a010..0x61a0ae; CScrollWnd_SetRange @0x619d70; NapiRegistry_SetStringValue @0x619dc0]

#include <base/os_registry/os_registry.h>

#include "common/scratch_registry.h"
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

// The VirtualStore copy of a key: HKEY_LOCAL_MACHINE\Software\<rest> as the user's
// Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\<rest>, where stock dfx2.exe, run by a user
// whose token is not elevated, keeps its activation (the copy holding its CDKIID, witnessed on a
// 64-bit Windows 11 in PR #1311's final review); none for an elevated token, nor for a key outside
// HKEY_LOCAL_MACHINE\Software. (Paths mapped; no registry reached.)
int virtual_store_paths() {
	const std::string dfx2 =
			"HKEY_LOCAL_MACHINE\\Software\\NovaLogic\\Delta Force Xtreme 2\\1.00.0000";
	TEST_EXPECT(opennova::os_registry_virtual_store_key(dfx2, false) ==
			"HKEY_CURRENT_USER\\Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node\\"
			"NovaLogic\\Delta Force Xtreme 2\\1.00.0000");
	TEST_EXPECT(opennova::os_registry_virtual_store_key(
						"hkey_local_machine\\SOFTWARE\\Vendor", false) ==
			"HKEY_CURRENT_"
			"USER\\Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node\\Vendor");
	TEST_EXPECT(opennova::os_registry_virtual_store_key(dfx2, true).empty());
	TEST_EXPECT(
			opennova::os_registry_virtual_store_key("HKEY_CURRENT_USER\\Software\\Vendor", false)
					.empty());
	TEST_EXPECT(opennova::os_registry_virtual_store_key("HKEY_LOCAL_MACHINE\\System\\Vendor", false)
					.empty());
	TEST_EXPECT(
			opennova::os_registry_virtual_store_key("HKEY_LOCAL_MACHINE\\SoftwareX\\Vendor", false)
					.empty());
	TEST_EXPECT(opennova::os_registry_virtual_store_key("HKLM\\Software\\Vendor", false).empty());
	return 0;
}

#if defined(_WIN32)
// A key no install makes: nothing read, `out` left alone. (Read only: the machine's registry is
// never written.)
int absent_value() {
	std::string out = "unchanged";
	TEST_EXPECT(!opennova::os_registry_read_string(
			"hkey_local_machine\\Software\\OpenNova\\os_registry_test_absent", "CDKIID", 0x2000, "",
			out));
	TEST_EXPECT(out == "unchanged");
	return 0;
}

// The access UAC's virtualization makes for a token that is not elevated, over two keys under the
// scratch key: `machine`, which this user may read and not write (as HKEY_LOCAL_MACHINE is to such
// a token), and `copy`, its VirtualStore copy. The write the machine key refuses lands in the copy;
// the read takes the copy's value first, the machine key's where the copy holds no such value, and
// the copy's own refusal (a value longer than the room) stands. With no copy named (an elevated
// token's access) the machine key alone.
int virtualized_access() {
	using opennova::os_registry_read_string_virtualized;
	using opennova::os_registry_write_string_virtualized;
	test_registry::ScratchKey scratch("os_registry");
	const std::string machine = scratch.path("machine");
	const std::string copy = scratch.path("copy");
	const std::string absent = scratch.path("absent");
	// The machine key's own values, written while it takes writes, and so not to the copy.
	TEST_EXPECT(os_registry_write_string_virtualized(machine, copy, "CDKIID", "MACHINE", ""));
	TEST_EXPECT(os_registry_write_string_virtualized(machine, copy, "SECOND", "MACHINE2", ""));
	std::string out = "unchanged";
	TEST_EXPECT(!os_registry_read_string_virtualized(copy, "", "CDKIID", 0x2000, "", out) &&
			out == "unchanged");
	TEST_EXPECT(scratch.deny_writes("machine"));
	// No copy: the machine key's refusal stands.
	TEST_EXPECT(!os_registry_write_string_virtualized(machine, "", "CDKIID", "REFUSED", ""));
	// Refused by the machine key, made to the copy; the machine key's value as it was.
	TEST_EXPECT(os_registry_write_string_virtualized(machine, copy, "CDKIID", "COPY", ""));
	TEST_EXPECT(os_registry_read_string_virtualized(machine, "", "CDKIID", 0x2000, "", out) &&
			out == "MACHINE");
	TEST_EXPECT(os_registry_read_string_virtualized(copy, "", "CDKIID", 0x2000, "", out) &&
			out == "COPY");
	// The copy's value first; the machine key's where the copy has none, or no copy exists.
	TEST_EXPECT(os_registry_read_string_virtualized(machine, copy, "CDKIID", 0x2000, "", out) &&
			out == "COPY");
	TEST_EXPECT(os_registry_read_string_virtualized(machine, copy, "SECOND", 0x2000, "", out) &&
			out == "MACHINE2");
	TEST_EXPECT(os_registry_read_string_virtualized(machine, absent, "CDKIID", 0x2000, "", out) &&
			out == "MACHINE");
	// The copy's value too long for a room of eight: refused, though the machine key's fits.
	TEST_EXPECT(os_registry_write_string_virtualized(machine, copy, "CDKIID", "COPY-LONGER", ""));
	out = "unchanged";
	TEST_EXPECT(!os_registry_read_string_virtualized(machine, copy, "CDKIID", 8, "", out) &&
			out == "unchanged");
	TEST_EXPECT(os_registry_read_string_virtualized(machine, "", "CDKIID", 8, "", out) &&
			out == "MACHINE");
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
	rc |= virtual_store_paths();
#if defined(_WIN32)
	rc |= absent_value();
	rc |= virtualized_access();
#else
	rc |= stand_in_round_trip();
#endif
	if (rc == 0)
		std::printf("OK: os_registry\n");
	return rc;
}
