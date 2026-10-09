#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::env {

// A line of a .env the game's reader reads otherwise than the engine's record (Config, as load_env reads it and
// save_env writes it) holds: `blocks` where the record cannot hold what the game reads (a save would change what
// the game reads), else input the game ignores or reads otherwise, which a save writes as the game read it. Its
// line (1-based, every CR LF line counted), the keyword it is about, and why, in words.
struct EnvSourceIssue {
	bool blocks = false;
	size_t line = 0;
	std::string field;
	std::string message;
};

// The lines of a .env's text the game's reader reads otherwise than the record holds, in line order [orig:
// TimeOfDay_ParseProperty @ 0x57c590 over File_ParseASCIIFile @ 0x53D810's lines]: a line it skips (a keyword
// neither it nor the terrain's parser has an arm for; a terrain keyword is the terrain's, D-TERRAIN-18), a keyword
// written again outside the blocks (the last line wins), a colour line short of its three values (the missing ones
// an earlier line's), a time that reads as another, a tod_begin past the 16th (kMaxTodKeyframes), a last line no
// CR LF ends (it loses its final byte), an envscale that follows a colour it does not scale the way the record
// would (the record scales every colour by the last envscale: it blocks), and a terrain key's line no line
// write_trn_key_line writes reads back as (it blocks).
std::vector<EnvSourceIssue> env_source_issues(const std::string &text);

} // namespace opennova::env
