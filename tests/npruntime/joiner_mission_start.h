#pragma once
// The embedder's end of a joiner's mission start for a harness with no load to run (its
// world needs none, or loaded before it dialed): the S2C 0x1A releases the mission start and
// the runtime holds every frame until the embedder's load ends with push_mission_start_end
// (ClientRuntime::run_frame). Called right after the frame whose receive took the 0x1A, it
// ships the push's datagrams in that frame, after its own sends (JoinerRole::poll_mission_start
// with its load done).
// [orig: NapiClient_WaitForGameStart @0x42CC10 - the send @0x42CC92, the flag test @0x42CCBF;
//  Game_StartMission @0x524360 - the call @0x524E29, the send @0x526255]
#include <runtime/inmatch/client_runtime.h>

#include <cstdint>
#include <utility>
#include <vector>

template <class Ship>
void push_released_mission_start(opennova::inmatch::ClientRuntime &runtime, Ship ship) {
	if (!runtime.mission_start_released()) return;
	for (std::vector<uint8_t> &datagram : runtime.push_mission_start_end()) ship(std::move(datagram));
}
