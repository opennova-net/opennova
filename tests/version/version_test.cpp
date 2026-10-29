// Pins base/version: Version's parse rule (the one the service's release routes, the editor's
// release request and the CMake stamp share), its text, retail_text and packed forms; the
// __DATE__ " " __TIME__ form against retail's two BDATs; OneBuild's wire version and label; the
// combined line; and that this program's one_build() carries a stamp that reads back.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include <base/version/one_build.h>
#include <base/version/version.h>
#include <base/version/version_line.h>

#include "common/test_expect.h"

using namespace opennova;

namespace {

Version v(const char *text) {
	const std::optional<Version> parsed = Version::parse(text);
	return parsed ? *parsed : Version{};
}

int test_parse() {
	const std::string longest = "1.2.3." + std::string(26, '0'); // 32 characters
	const std::string good[] = { "0.2.0", "1.0", "1.7.5.8", "0.0.22.123", "7", "4294967295.0",
		longest };
	for (const std::string &text : good)
		TEST_EXPECT(Version::parse(text).has_value());
	const std::string bad[] = { "", "v1.0", "V1.0", "1..2", "1.", ".1", "1.2.3.4.5", "1.x", " 1.0",
		"1.0 ", "+1.0", "-1.0", "1,0", "4294967296.0", "0x1.0", longest + "0" };
	for (const std::string &text : bad)
		TEST_EXPECT(!Version::parse(text).has_value());
	const Version release = v("0.0.22.123");
	TEST_EXPECT(release.count == 4 && release.parts[0] == 0 && release.parts[1] == 0 &&
			release.parts[2] == 22 && release.parts[3] == 123);
	const Version short_one = v("1.0");
	TEST_EXPECT(short_one.count == 2 && short_one.parts[2] == 0 && short_one.parts[3] == 0);
	TEST_EXPECT(v("0.2.0").text() == "0.2.0" && v("1.0").text() == "1.0" &&
			v("0.0.22.123").text() == "0.0.22.123" && Version{}.text().empty());
	TEST_EXPECT(v("1.0") == v("1.0") && v("1.0") != v("1.0.0") && v("1.0") != v("1.1"));
	std::printf("parse: one to four numbers between dots, nothing else\n");
	return 0;
}

int test_retail_forms() {
	TEST_EXPECT(v("1.7.5.7").retail_text() == "V1.7.5.7");
	TEST_EXPECT(v("0.0.22").retail_text() == "V0.0.22.0");
	TEST_EXPECT(v("1.7.5.7").packed() == 0x01070507u);
	TEST_EXPECT(v("0.0.22.0").packed() == 0x00001600u && v("0.0.22").packed() == 0x00001600u);
	TEST_EXPECT(v("0.0.256").packed() == 0 && v("1.0.0.300").packed() == 0);
	// Retail's two build dates, JO's BDAT and DFX2's, from the Unix times they name.
	TEST_EXPECT(build_date_text(1248202482) == "Jul 21 2009 18:54:42");
	TEST_EXPECT(build_date_text(1246966389) == "Jul  7 2009 11:33:09");
	TEST_EXPECT(build_date_text(0) == "Jan  1 1970 00:00:00");
	TEST_EXPECT(build_date_text(951782400) == "Feb 29 2000 00:00:00");
	TEST_EXPECT(build_date_text(-1) == "Dec 31 1969 23:59:59");
	std::printf("retail forms: V%%u.%%u.%%u.%%u, the packed dword, the BDAT date\n");
	return 0;
}

int test_one_build() {
	OneBuild build;
	build.version = v("0.0.22.123");
	TEST_EXPECT(build.wire_version() == v("0.0.22.0"));
	build.version = v("0.1");
	TEST_EXPECT(build.wire_version() == v("0.1.0.0") && build.wire_version().count == 4);
	build.version = v("0.0.22");
	TEST_EXPECT(build.wire_version().retail_text() == "V0.0.22.0" &&
			build.wire_version().packed() == 0x00001600u);
	TEST_EXPECT(build.label() == "ONE 0.0.22");
	build.commit = "a1b2c3d";
	TEST_EXPECT(build.label() == "ONE 0.0.22 (a1b2c3d)");
	build.staging = true;
	TEST_EXPECT(build.label() == "ONE 0.0.22 (a1b2c3d, staging)");
	build.commit.clear();
	TEST_EXPECT(build.label() == "ONE 0.0.22 (staging)");
	build.time = 1248202482;
	TEST_EXPECT(build.date_text() == "Jul 21 2009 18:54:42");

	// This program's own: CMake refused any stamp that does not read back.
	const OneBuild &own = one_build();
	TEST_EXPECT(own.version.count >= 1 && own.version.count <= 4);
	TEST_EXPECT(Version::parse(own.version.text()) == own.version);
	TEST_EXPECT(own.version.parts[0] <= 255 && own.version.parts[1] <= 255 &&
			own.version.parts[2] <= 255);
	TEST_EXPECT(own.commit.size() <= 7 && own.time >= 0);
	TEST_EXPECT(own.label().rfind("ONE " + own.version.text(), 0) == 0);
	std::printf("one_build: %s, built %s\n", own.label().c_str(), own.date_text().c_str());
	return 0;
}

int test_version_line() {
	OneBuild one;
	one.version = v("0.0.22");
	one.commit = "a1b2c3d";
	Versions versions;
	versions.game = "OP: Acid Gambit";
	versions.game_version = v("0.1.4");
	versions.one = one;
	versions.protocol = "JO";
	versions.protocol_version = "16";
	TEST_EXPECT(version_line(versions) ==
			"OP: Acid Gambit 0.1.4 \xC2\xB7 ONE 0.0.22 (a1b2c3d) \xC2\xB7 JO protocol 16");

	// No installed release, no commit.
	Versions jo;
	jo.game = "Joint Operations";
	jo.one.version = v("0.0.22");
	jo.protocol = "JO";
	jo.protocol_version = "16";
	TEST_EXPECT(version_line(jo) == "Joint Operations \xC2\xB7 ONE 0.0.22 \xC2\xB7 JO protocol 16");

	// No protocol: the line ends at ONE's label.
	versions.protocol.clear();
	versions.protocol_version.clear();
	TEST_EXPECT(version_line(versions) == "OP: Acid Gambit 0.1.4 \xC2\xB7 ONE 0.0.22 (a1b2c3d)");

	// No game: ONE's label leads.
	jo.game.clear();
	TEST_EXPECT(version_line(jo) == "ONE 0.0.22 \xC2\xB7 JO protocol 16");
	std::printf("version_line: the parts known, joined by a middle dot\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_parse();
	failures += test_retail_forms();
	failures += test_one_build();
	failures += test_version_line();
	if (failures == 0)
		std::printf("version: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
