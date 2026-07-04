#pragma once

// `/PROFILE <file>` server-log (".sph") recording decoder.
//
// When Joint Operations is launched with `/profile <file>`, the engine opens a
// FOURCC-chunked recording (host.sph / client.sph) and dumps, every 8th engine
// tick (62 Hz -> ~7.75 Hz), a per-frame snapshot of every pool-0 (player)
// entity's runtime state — id, position (16.16), 32-bit BAM heading, flags —
// plus a player roster and join/death/disconnect events. It is the engine's own
// *decoded* view of a session, so it is an independent, decryption-free value
// oracle for validating the in-game replication RE (the wire decoders in
// ingame_decode.h). Wire-format witnesses + the cross-validation live in
// docs/net/novaworld-net-re.md §5.22; the field tables there are authoritative.
//
// On-disk chunk = [char[4] tag][u16 length][u16 pad][payload]; `length` is the
// TOTAL chunk size incl. the 8-byte header. Tags are the reversed mnemonic
// (the engine writes a u32 multichar constant little-endian, or strcpy's the
// already-reversed literal):
//   on-disk  mnemonic  writer [orig]
//   "NGEB"   BEGN      Game_TeardownMission open path @ 0x524482 (header)
//   "FEDP"   PDEF      CServerLog_WritePlayerNameRecord @ 0x4e1cc0 (roster)
//   "GEBF"   FBEG      CServerLog_WriteTimestampRecord  @ 0x4e1aa0 (frame marker)
//   "TADP"   PDAT      CServerLog_WritePositionRecord   @ 0x4e1b00 (entity sample)
//   "CPSP"   CDAT      CServerLog_WriteEntityDataRecord @ 0x4e1bd0 (168 B blob)
//   "KRBP"   PBRK      CServerLog_WriteDeathMarker      @ 0x4e1e00 (death event)
//   "MERP"   PREM      CServerLog_WriteDisconnectMarker @ 0x4e1c50 (disconnect)
//   "DNE."   .END      CServerLog_CloseAndFree          @ 0x4e1a10 (end-of-file)
//
// The per-frame write loop lives in Game_ProcessMainFrame @ 0x5263f0 and
// iterates g_pool_list[0] (POOL 0 = players) — an independent witness that pool
// 0 is the player pool.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

// 16.16 fixed-point -> double (engine world units).
inline double serverlog_fp16(int32_t v) { return double(v) / 65536.0; }
// 32-bit BAM -> degrees (0..360).
inline double serverlog_bam32_deg(uint32_t b) {
	return double(b) * (360.0 / 4294967296.0);
}

// FEDP / PDEF — one roster entry. Appended in file order; a second entry for an
// id already present is the engine recording a join/re-add. [orig:
// CServerLog_WritePlayerNameRecord @ 0x4e1cc0]
struct ServerLogPlayer {
	uint32_t net_id = 0;      // entity[124] (or entity[120] for bots)
	uint8_t  team = 0;        // entity+354: 0 neutral / 1 Blue / 2 Red — the
	                          // AUTHORITATIVE team (PDAT's +42 is a stat byte).
	std::string name;         // NUL-terminated player name (may be empty).
};

// TADP / PDAT — one pool-0 entity sample within a frame. The on-disk record
// stores (-entity[2], entity[3], entity[1]) at +12/+16/+20; we reconstruct the
// entity's world position triple entity+4/+8/+12 so it lines up 1:1 with the
// compact-record positions in ingame_decode.h. [orig:
// CServerLog_WritePositionRecord @ 0x4e1b00]
struct ServerLogEntity {
	uint32_t net_id = 0;       // +8  entity[30]/[31] (bot vs net id)
	int32_t  pos_x = 0;        // entity+4  (= +20 on disk),    i32 16.16
	int32_t  pos_y = 0;        // entity+8  (= -(+12 on disk)),  i32 16.16
	int32_t  pos_z = 0;        // entity+12 (= +16 on disk),     i32 16.16
	uint32_t yaw_bam = 0;      // +24 entity+16 — 32-bit BAM heading
	uint32_t angle2 = 0;       // +28 entity+24 — 2nd Euler angle
	uint32_t flags = 0;        // +36 entity[9] (entity+36)
	uint16_t vehicle_flag = 0; // +40 1 iff entity[91] (in a vehicle)
	uint16_t stat_byte = 0;    // +42 playerSlot+0x15F78 — a per-player stat byte
	                           // (decompiler mislabels it "team"; 0 in early
	                           // frames). Team lives in the FEDP roster.
};

// GEBF / FBEG marker plus the TADP records that follow it. [orig:
// CServerLog_WriteTimestampRecord @ 0x4e1aa0]
struct ServerLogFrame {
	uint32_t frame_index = 0;  // engine tick >> 3
	std::vector<ServerLogEntity> entities;
};

enum class ServerLogEventKind { Death, Disconnect };

// KRBP / PBRK (death) or MERP / PREM (disconnect). [orig:
// CServerLog_WriteDeathMarker @ 0x4e1e00 / CServerLog_WriteDisconnectMarker @ 0x4e1c50]
struct ServerLogEvent {
	ServerLogEventKind kind = ServerLogEventKind::Death;
	uint32_t net_id = 0;       // entity id of the affected player
	uint32_t at_frame = 0;     // frame_index of the most recent frame (timeline anchor)
};

struct ServerLogDocument {
	uint32_t version = 0;        // BEGN version (= 2)
	std::string mission;         // BEGN mission basename (e.g. "mission.bms")
	std::vector<ServerLogPlayer> roster;  // FEDP, in file order
	std::vector<ServerLogFrame>  frames;  // FBEG + its PDAT records
	std::vector<ServerLogEvent>  events;  // PBRK / PREM, interleaved by time
	uint32_t cdat_count = 0;     // CPSP records seen (entity-data blobs)
	bool ended_clean = false;    // saw the .END chunk
	bool leftover_clean = false; // consumed exactly to .END (or to EOF)
	size_t leftover_bytes = 0;   // unconsumed bytes when a chunk was malformed
};

// Decode an entire .sph buffer. Returns true iff every chunk was consumed
// cleanly (a well-formed file ends with .END at EOF). On a malformed chunk the
// walk stops and `leftover_bytes` records what was left.
bool decode_server_log(const uint8_t *data, size_t len, ServerLogDocument &out);

} // namespace opennova
