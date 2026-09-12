#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace opennova::inmatch {

// The two JOINTOPERATIONS connection-template values a loose `_NSTMOUT.TXT` in
// the game directory can override at network init: the receive-silence reap
// window (cs_dir0/cs_dir1 `timeout_ms`, CS field 0) and the outbound-message
// pool bound (`msg_out_max`, CS field 11). Both are stored into BOTH direction
// templates and copied onto every connection at create, so one file changes the
// host's server-side reap (NP.C:PT:SERTMOUT), the client-side reap
// (NP.C:PT:CLNTTMOUT) the host advertises in its 0x82 CS block, and the
// overflow disconnect (NP.C:MSGCRE). Every consumer guards on `< 0`, so -1
// disables the mechanism rather than firing immediately.
// [orig: CNapiNetwork_Init @0x4CA4A0 — timeout_ms=120000 @0x4ca9d7,
//  msg_out_max=1200 @0x4ca9dc, the file legs @0x4ca9e1..0x4caa4b, the stores
//  cs_dir1 @0x4caa81/@0x4cab20 and cs_dir0 @0x4cab54/@0x4cabf0; consumers
//  CNapiNPConnection_PumpStateMachine @0x62934c (state 1) / @0x6295a2 (state 5),
//  NapiNPMessage_Create @0x628048]
struct SessionTimeoutConfig {
	int32_t timeout_ms = 120000;
	int32_t msg_out_max = 0x4B0;
};

// The override file's CWD-relative name; retail resolves it with FindFirstFileA
// on the bare name and reads it with _lopen (a PFF entry is NOT visible to it).
// [orig: Napi_FileExists @0x61BD70; Napi_ReadFileAlloc @0x61CE00 -> NapiFile_Open @0x61C4D0]
inline constexpr char kSessionTimeoutOverrideFile[] = "_NSTMOUT.TXT";

// Apply the file's text to `cfg` exactly as CNapiNetwork_Init does: a
// case-insensitive "NEVER" PREFIX sets both values to -1; otherwise atol(text)
// >= 0 sets timeout_ms = 1000 * seconds and leaves msg_out_max alone (so "0"
// is a zero-millisecond reap and "abc" atol's to 0), and a negative number sets
// both to -1. [orig: StrStartsWithNoCase(buf, "NEVER") @0x4caa13 -> -1/-1
//  @0x4caa1f/@0x4caa22; atol @0x4caa2b; >= 0 -> 1000*sec @0x4caa44; < 0 ->
//  -1/-1 @0x4caa37/@0x4caa3a]
inline void parse_nstmout(std::string_view text, SessionTimeoutConfig &cfg) {
	constexpr std::string_view kNever = "NEVER";
	bool never = text.size() >= kNever.size();
	for (std::size_t i = 0; never && i < kNever.size(); ++i) {
		if (std::toupper(static_cast<unsigned char>(text[i])) != kNever[i]) never = false;
	}
	if (never) {
		cfg.timeout_ms = -1;
		cfg.msg_out_max = -1;
		return;
	}
	const std::string buffer(text);
	const long seconds = std::strtol(buffer.c_str(), nullptr, 10); // CRT atol
	if (seconds >= 0) {
		// retail: `imul eax, 1000` on the 32-bit atol result (@0x4caa44)
		cfg.timeout_ms = static_cast<int32_t>(
				static_cast<uint32_t>(static_cast<int64_t>(seconds) * 1000));
	} else {
		cfg.timeout_ms = -1;
		cfg.msg_out_max = -1;
	}
}

// Resolve the template for a session whose game directory is `game_root_dir`:
// the defaults, overridden by a loose `_NSTMOUT.TXT` there when one exists. An
// empty directory means no root is known and yields the defaults (retail reads
// relative to its own working directory, which IS the game directory).
inline SessionTimeoutConfig load_session_timeout_config(std::string_view game_root_dir) {
	SessionTimeoutConfig cfg;
	if (game_root_dir.empty()) return cfg;
	std::string path(game_root_dir);
	if (path.back() != '/' && path.back() != '\\') path.push_back('/');
	path += kSessionTimeoutOverrideFile;
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) return cfg;
	const std::string text((std::istreambuf_iterator<char>(file)),
			std::istreambuf_iterator<char>());
	parse_nstmout(text, cfg);
	return cfg;
}

} // namespace opennova::inmatch
