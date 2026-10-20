#pragma once

// THE SERVER'S FILE SEAM: the one door the host's log writers (the /PROFILE
// .sph recorder, the punt and cheat logs) reach the disk through. Retail
// opens each by a bare name relative to its working directory (fopen,
// _lopen/_lcreat, DeleteFileA, File_CheckExists); the embedder binds the
// seam to its own directory (opennova-serve: the working directory, ADR 0051
// decision 2) and a test binds it to memory. Platform file I/O, not a port.

#include <string>
#include <string_view>

namespace opennova::world {
struct Entity;
}

namespace opennova::inmatch {

struct NapiNPConnection;

class ServerFileSink {
public:
	virtual ~ServerFileSink() = default;
	// Whether `name` names an existing file [orig: File_CheckExists @0x75a5d0].
	virtual bool exists(const std::string &name) = 0;
	// Delete `name`; a missing file is no error [orig: DeleteFileA].
	virtual void remove(const std::string &name) = 0;
	// Write `bytes` to `name`: `append` adds them at the end (creating the file
	// when it is missing), otherwise the file is created or truncated first.
	// False when the file does not open.
	virtual bool write(const std::string &name, std::string_view bytes, bool append) = 0;
};

// The host process's log devices, owned by the embedder and handed to the
// host context through HostConfig (null = the device is off, as retail with
// its switch unset).
class ServerLogRecorder;
class PuntLog;
class NapiLog;
class ConnectLog;
struct ServerLogs {
	ServerLogRecorder *profile = nullptr; // /PROFILE <path> [orig: g_RunningWithProfile @0xb4c500]
	PuntLog *punt = nullptr;              // /PUNT.TXT, /PUNTLOG, /CHEATLOG
	// The protocol manager's net log, _NETLOG.TXT, which NETLOG toggles (server_console.h)
	// [orig: the manager's NapiLog, CNapiNetwork_SetNetLogFile @0x4C69A0].
	NapiLog *net = nullptr;
	// The protocol's _inout.txt log and the `/INOUT` switch that turns it on at the
	// socket open; the switch also gates the join validator's own REJECTING line
	// [orig: CNapiLog_Init(protocol + 0xF80) @0x625AE5..0x625B07; g_InoutLogEnabled, set by
	//  `/INOUT` @0x4A744F].
	NapiLog *inout = nullptr;
	bool inout_switch = false;
	// The working directory the console's dumps write to (SAVENFO's debug.nfo).
	ServerFileSink *files = nullptr;
	// _connectlog.txt, which `/CONNECTLOG` turns on (punt_log.h ConnectLog).
	ConnectLog *connect = nullptr;
};

// Server_WritePuntLog(slot, reason, "") at a punt the host logs: the line goes
// only for a slot that is not the host's own and not a bot, with `/NOPUNT`
// unset (no port host sets it). The gates also test a latch, slot+89896, that
// nothing in the image writes, so a slot already punted still logs; its
// description stays the first (Server_StageHostDisconnect) [orig: the gates
// ahead of each call: Server_ValidateWeaponCRC @0x501fd6..0x50200e,
// NapiNPServerMsg_HandleAntiCheatCRCCheck @0x5021a2..0x5021da,
// Server_CheckPlayerViolations @0x51acf2..0x51ad0d, Server_OnPlayerJoin
// @0x51A8A4..0x51A8D4 / @0x51A8FA..0x51A915: slot+5, slot+96483, the latch
// slot+89896, dword_B4C698]. Defined in punt_log.cpp.
void server_logs_punt(const ServerLogs &logs, const NapiNPConnection &conn, const char *reason);
// The /PROFILE deploy marker of a player's deploy transaction, after its
// frontier hint and ahead of its mobile-spawn seat re-pick (null = off)
// [orig: Server_DeployPlayer @0x517a27..0x517a37 ->
// CServerLog_WriteDeathMarker, between the 0x1E send @0x517a22 and @0x517A3E].
// Defined in server_log_recorder.cpp.
void server_logs_deploy(ServerLogRecorder *profile, const world::Entity &player);

} // namespace opennova::inmatch
