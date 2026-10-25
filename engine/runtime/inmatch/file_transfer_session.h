#pragma once

// The process's two file-transfer session words: the id each S2C 0x02 probe carries as its
// second word, and the countdown that steps it once more a second into a mission. Every
// authority mission start (a map change's, a single player's and its restart's included) adds
// one to the id and arms the countdown at 62; the server tick's head counts it down while it is
// positive and adds one more to the id when it reaches zero; a joiner runs neither. So a fresh
// host's first mission probes with 1 inside its first 62 server ticks and with 2 after them, and
// its next start probes with 3.
// The countdown at zero also arms Server_TickUpdate's once-a-second re-probe of a state-6 slot
// whose CNetPlayer +0x94 flag is set; nothing sets that flag (its three stores all write zero:
// the create's @0x4C79F5, the probe's own @0x4C945E and the walk's @0x51DBD3), so the re-probe
// never runs and has no counterpart here.
// [orig: g_FileTransferSessionId @0xC86FCC, g_FileTransferSessionStepCountdown @0xC86FD0 (both
//  zero in the image); Game_StartMission @0x524360 (the authority arm's add @0x5244B4, ebp = 1
//  @0x524388; the store of 62 @0x5244C6); Server_TickUpdate @0x51D7E0 (the countdown
//  @0x51D809..0x51D81E, the add @0x51D820; the re-probe's gate @0x51DB9F, its walk
//  @0x51DBA7..0x51DBEC); NapiNPPlayer_Create @0x4C7850 (the store @0x4C79F5, ebx = 0 @0x4C7856);
//  CNetPlayer_SendFileTransferChunk @0x4C9440 (the store @0x4C945E)]

#include <cstdint>

namespace opennova::inmatch {

struct FileTransferSession {
	// The countdown each authority start arms. [orig: Game_StartMission @0x524360 (the store
	//  @0x5244C6)]
	static constexpr int32_t kStepCountdownTicks = 62;

	uint32_t id = 0;
	int32_t step_countdown = 0;

	// The authority's mission start. [orig: Game_StartMission @0x524360 (the add @0x5244B4,
	//  the store @0x5244C6)]
	void start_mission() {
		id += 1;
		step_countdown = kStepCountdownTicks;
	}

	// The server tick's head: a signed test, then the decrement, and the step when it reaches
	// zero. [orig: Server_TickUpdate @0x51D7E0 (`cmp` @0x51D809, `jle` @0x51D811, `sub`
	//  @0x51D818, `jnz` @0x51D81E, the add @0x51D820)]
	void step_server_tick() {
		if (step_countdown <= 0) return;
		if (--step_countdown == 0) id += 1;
	}
};

} // namespace opennova::inmatch
