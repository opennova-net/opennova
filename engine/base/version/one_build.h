// ONE, the OpenNova Engine: engine/ and the programs built from it (opennova.exe, onplay,
// oneditor, opennova-serve, the tools, the NovaWorld service). Its build is the OpenNova release
// it was made from, from the git tags (v0.0.22 is 0.0.22; a pull request's CI build is
// 0.0.22.<run>), the commit, whether it is a staging build (a pull request's, which updates
// from and plays on staging) and when its commit was made.
//
// one_build() is that build, compiled into one TU, one_build.cpp, from the CMake cache
// variables OPENNOVA_VERSION, OPENNOVA_COMMIT, OPENNOVA_STAGING and OPENNOVA_BUILD_TIME
// (engine/base/CMakeLists.txt resolves and checks them): no other source carries a version
// literal of ONE's, so a new stamp rebuilds that one TU alone.
//
// Infrastructure, not a port.
#pragma once

#include <cstdint>
#include <string>

#include <base/version/version.h>

namespace opennova {

struct OneBuild {
	// The release: "0.0.22" from a tag, "0.0.22.123" for a staging run.
	Version version;
	// The commit's short sha ("a1b2c3d"); empty where the build names none.
	std::string commit;
	// A staging build (a pull request's CI run).
	bool staging = false;
	// The commit's time, Unix seconds UTC (0 where the build names none). Never the compile's
	// clock, which would keep a stamp from ever matching the compiler cache.
	int64_t time = 0;

	// The version retail's engine-build slots carry: the release's three parts (a missing one 0)
	// and a fourth 0. "0.0.22.123" is 0.0.22.0, "0.1" 0.1.0.0.
	Version wire_version() const;
	// The build's time in the compiler's __DATE__ " " __TIME__ form, the one retail's BDAT
	// carries (build_date_text).
	std::string date_text() const;
	// "ONE 0.0.22 (a1b2c3d)", "ONE 0.0.22.123 (a1b2c3d, staging)"; "ONE 0.0.22" with no commit.
	std::string label() const;
};

// This program's build: the stamp one_build.cpp is compiled with.
const OneBuild &one_build();

} // namespace opennova
