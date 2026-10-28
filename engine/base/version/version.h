// A dotted version, one to four decimal numbers: ONE's ("0.0.22", a staging build's
// "0.0.22.123"), a game's release ("0.1.4", "1.7.5.7", a command file's [HEADER] VERSION) and the
// four numbers retail's engine-build slots carry. `parse` is the one rule every reader of a typed
// or stamped version applies (the service's release routes, the editor's release request, the
// build's stamp in CMake); `text` spells it back, `retail_text` the way retail's binaries print
// their build, and `packed` the one-byte-per-part dword.
//
// Infrastructure, not a port: the forms are retail's, the type is ours.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace opennova {

struct Version {
	// The numbers, first to last; a part past `count` is 0.
	std::array<uint32_t, 4> parts{};
	// How many numbers the version names, 1 to 4 (0 for none, the default).
	uint8_t count = 0;

	// One to four decimal numbers separated by single dots, 32 characters at most, each number
	// at most 4294967295: "0.2.0", "1.0", "1.7.5.7". Nothing else: no leading "V", no sign, no
	// blank, no empty part ("1..2", "1."). nullopt for anything else.
	static std::optional<Version> parse(std::string_view text);

	// The numbers it names, dotted: "0.2.0".
	std::string text() const;
	// Retail's spelling of a build, sprintf("V%i.%i.%i.%i") over all four parts (a part past
	// `count` is 0): {1,7,5,7} is "V1.7.5.7", {0,0,22} "V0.0.22.0".
	std::string retail_text() const;
	// One byte per part, the first in the high byte: {1,7,5,7} is 0x01070507, {0,0,22,0}
	// 0x00001600. 0 when any part is past 255.
	uint32_t packed() const;

	// The same numbers named the same way ("1.0" is not "1.0.0").
	bool operator==(const Version &other) const;
	bool operator!=(const Version &other) const { return !(*this == other); }
};

// A UTC time in Unix seconds as the compiler's __DATE__ " " __TIME__ spells it, the form retail's
// BDAT carries: "Mmm dd yyyy hh:mm:ss" with the day padded by a space ("Jul 21 2009 18:54:42",
// "Jul  7 2009 11:33:09").
std::string build_date_text(int64_t unix_utc);

} // namespace opennova
