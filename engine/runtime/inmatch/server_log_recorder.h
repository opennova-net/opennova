#pragma once

// THE /PROFILE SERVER LOG: the engine's own decoded view of a session, written
// as a .sph chunk stream (formats/sph; records net/npwire/serverlog_encode.h).
// `/PROFILE <path>` arms it for the process [orig: Game_ParseCommandLineAndInit
// @0x4a778f..0x4a77bb — the next token copied to g_ProfileLogPath @0xb4c504,
// g_RunningWithProfile @0xb4c500 = 1]; one recorder object, retail's g_ServerLog
// @0xb79448, then lives the whole run:
//
//   every mission start opens a fresh file and writes BEGN, before the mission
//     loads [orig: Game_StartMission @0x524475..0x524487 ->
//     ServerLog_OpenForWrite @0x4e1ec0];
//   the end of that start lists the mission's pool-0 roster as PDEF rows
//     [orig: Game_StartMission @0x526135..0x52617b];
//   every player add appends its PDEF [orig: Server_PlayerAdd @0x51d2fd..0x51d30f];
//   every eighth logic tick (tick & 7 == 7) writes FBEG (tick >> 3) and one
//     PDAT per pool-0 player [orig: Game_ProcessMainFrame @0x526879..0x5268e7];
//   a player's deploy writes PBRK [orig: Server_ProcessPlayerDeath — the deploy
//     transaction — @0x517a27..0x517a37];
//   a player's disconnect writes PREM [orig: Server_HandlePlayerDisconnect
//     @0x51b80e..0x51b822];
//   the mission teardown writes .END and closes the file, before the post-mission
//     pass and the slot disconnects [orig: Game_TeardownMission @0x5223e2..0x5223f0
//     -> CServerLog_CloseAndFree @0x4e1a10].
//
// The device under it is the embedder's file seam (server_files.h). The record
// field maps are docs/net/novaworld-net-re.md §5.22.

#include <runtime/inmatch/server_files.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {
class World;
struct Entity;
} // namespace opennova::world

namespace opennova::inmatch {

struct NapiNPServerCtx;

// The file the open writes: the /PROFILE path with everything after its FIRST
// '.' replaced by "sph" (or ".sph" appended when it has none); while that file
// exists, the digits ahead of the dot count up — a name with none takes 1 —
// and past 10 wrap to 1, deleting each file the wrapped search lands on.
// [orig: ServerLog_OpenForWrite @0x4e1f03..0x4e1ff9 — Path_ReplaceOrAppendExtension
//  @0x53c780 (the first '.'), File_CheckExists @0x4e1f37, the digit scan
//  @0x4e1f62..0x4e1f71, atol + 1 @0x4e1f7a, `> 10` -> 1 @0x4e1f83..0x4e1f8a,
//  DeleteFileA @0x4e1ff9]
std::string server_log_file_name(ServerFileSink &files, const std::string &profile_path);

// The id a record carries for an entity: a player's (Flags 0x100) owning
// connection id (entity+0x78), anyone else's DcbId (entity+0x7C, the SSN)
// [orig: the `Flags & 0x100` pick in every writer, e.g. @0x4e1b50..0x4e1b5d].
uint32_t server_log_entity_id(const world::Entity &e);

class ServerLogRecorder {
public:
	// The 1 MiB write buffer the writers fill and flush [orig: j_operator_new
	// (0x100000) @0x4e2039].
	static constexpr size_t kBufferBytes = 0x100000;
	// The roster table's capacity, initialised to -1 at every open
	// [orig: memset(log + 12, 0xFF, 0x200) @0x4e1efe].
	static constexpr int kRosterIds = 128;

	ServerLogRecorder(ServerFileSink &files, std::string profile_path);
	// The process-exit close [orig: sub_7947A0 -> CServerLog_CloseAndFree].
	~ServerLogRecorder();
	ServerLogRecorder(const ServerLogRecorder &) = delete;
	ServerLogRecorder &operator=(const ServerLogRecorder &) = delete;

	// ServerLog_OpenForWrite: close any open file, pick the name, create it and
	// buffer BEGN over `map_file_name`. False when the file does not open (the
	// recorder then writes nothing until the next open).
	bool open(const std::string &map_file_name);
	// CServerLog_CloseAndFree: a buffer holding anything gets .END, then the
	// buffer flushes and the file closes.
	void close();
	bool is_open() const { return open_; }
	// The file the last open picked.
	const std::string &file_name() const { return file_; }

	// PDEF, once per id: an id already in the roster table writes nothing.
	void write_player(const world::Entity &e);
	// The mission start's roster pass over pool 0: players, and on a
	// session-less authority every row.
	void write_mission_roster(const NapiNPServerCtx &ctx, const world::World &world);
	// The per-tick leg: on a tick with tick & 7 == 7, FBEG and the pool-0 PDATs.
	void write_frame(const NapiNPServerCtx &ctx, const world::World &world, uint32_t tick);
	// PBRK for an id in the roster table; nothing otherwise.
	void write_death(const world::Entity &e);
	// PREM, with no roster-table test.
	void write_disconnect(const world::Entity &e);

private:
	// A writer's flush rule: when the record would not fit (each writer's own
	// test, two of which double the buffered length), the buffer goes to the
	// file first.
	void flush_if(bool full);
	void flush();
	bool roster_has(uint32_t id) const;

	ServerFileSink &files_;
	std::string profile_path_;
	std::string file_;
	bool open_ = false;
	std::vector<uint8_t> buffer_;
	std::array<uint32_t, kRosterIds> roster_{};
	int32_t roster_count_ = 0;
};

} // namespace opennova::inmatch
