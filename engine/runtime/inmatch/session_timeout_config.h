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
//
// The struct also carries the other cs_dir0 slots a connection's own pumps read,
// at their JOINTOPERATIONS template values: a joiner overlays them from the
// host's 0x82 CS block and from later H:0x00 CS updates
// (apply_session_cs_field). Each slot's consumer:
//   1  recv_max_per_tick       the teardown's disconnect-packet burst
//                              [orig: TeardownActiveConnection @0x6253ef]
//   4  idle_send_interval_ms   PumpSendIntervals' EMPTY leg [orig: @0x629041]
//   5  active_send_interval_ms PumpSendIntervals' ACTIVE leg [orig: @0x628ff1]
//   10 packet_queue_max        HandleSessionPacket's out-of-order queue bound
//                              [orig: @0x626c18]
//   13 max_packet_bytes        BuildOutgoingPackets' packet ceiling [orig: @0x628436]
//   14 max_packets_per_tick    BuildOutgoingPackets' packets per call [orig: @0x62844e]
// [orig: CNapiNetwork_Init @0x4cab60 (4), @0x4cab88 (30000), @0x4cab98 (10000),
//  @0x4cabe0 (100), @0x4cab3c (mpmaxpacketsize, 1300 by default), @0x4cac18 (-1, the
//  `or ecx, -1` @0x4caad5); the field map is NapiCSConfig at conn+0x17C (cs_dir0)]
struct SessionTimeoutConfig {
	int32_t timeout_ms = 120000;              // CS field 0
	int32_t recv_max_per_tick = 4;            // CS field 1
	int32_t idle_send_interval_ms = 30000;    // CS field 4
	int32_t active_send_interval_ms = 10000;  // CS field 5
	int32_t packet_queue_max = 100;           // CS field 10
	int32_t msg_out_max = 0x4B0;              // CS field 11
	int32_t max_packet_bytes = 1300;          // CS field 13
	int32_t max_packets_per_tick = -1;        // CS field 14
};

// Store one CS slot the way CNapiNPConnection_HandleCSConfigUpdate and
// NapiNP_HandleServerJoinResponse store it into cs_dir0: the raw dword,
// whatever its value. Slots this struct does not carry have no consumer here.
// [orig: HandleCSConfigUpdate @0x6219c8; HandleServerJoinResponse @0x629b63]
inline void apply_session_cs_field(SessionTimeoutConfig &cfg, uint32_t slot, int32_t value) {
	switch (slot) {
	case 0: cfg.timeout_ms = value; break;
	case 1: cfg.recv_max_per_tick = value; break;
	case 4: cfg.idle_send_interval_ms = value; break;
	case 5: cfg.active_send_interval_ms = value; break;
	case 10: cfg.packet_queue_max = value; break;
	case 11: cfg.msg_out_max = value; break;
	case 13: cfg.max_packet_bytes = value; break;
	case 14: cfg.max_packets_per_tick = value; break;
	default: break;
	}
}

// The override file's CWD-relative name; retail resolves it with FindFirstFileA
// on the bare name and reads it with _lopen (a PFF entry is NOT visible to it).
// [orig: Napi_FileExists @0x61BD70; Napi_ReadFileAlloc @0x61CE00 -> NapiFile_Open @0x61C4D0]
inline constexpr char kSessionTimeoutOverrideFile[] = "_NSTMOUT.TXT";

// Apply the file's text to `cfg` exactly as CNapiNetwork_Init does: a
// case-insensitive "NEVER" PREFIX sets both values to -1; otherwise atol(text)
// >= 0 sets timeout_ms = 1000 * seconds and leaves msg_out_max alone (so "0"
// is a zero-millisecond reap and "abc" atol's to 0), and a negative number sets
// both to -1. [orig: String_StartsWithNoCase(buf, "NEVER") @0x4caa13 -> -1/-1
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
