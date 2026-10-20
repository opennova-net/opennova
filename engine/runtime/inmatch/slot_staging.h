// The parity runner's slot staging on our host (the E5 gameplay scenarios'
// `staging` entries): the authority's Position write for the body a player
// slot holds. The onHook bridge's apply_pose_slot does the same on a retail
// host: it walks the slot table (an index under the capacity, the row's
// in-use byte, the Person at +0) and writes the Person's three Position
// words. Position only: a staged heading never holds (the player's own look
// returns next frame), and on the authority a staged pose holds against the
// player's own uplinks only past the chase's 2-unit drop (D-NET-NEW-LP-2).
// [orig: the slot walk Server_NotifyPlayerKicked @0x5082DE - the capacity test
//  @0x508304..0x508316, the row g_PlayerSlots + index * 0x188E8 @0x50831C..0x508322,
//  the in-use byte +4 @0x50832E..0x508332; the authority's Position write for a
//  slot's Person Server_PositionPlayerForSpawn @0x50CF60 - Person = [slot+0]
//  @0x50CF76 / @0x50CF7C, Position +4/+8/+0xC @0x50CFBE / @0x50CFCA / @0x50CFD0]
#pragma once

#include <cstdint>
#include <string>

namespace opennova::inmatch {

struct NapiNPServerCtx;

enum class SlotStagingStatus : uint8_t {
	Staged,          // the slot's body now stands at the position
	NotAuthority,    // no host session (a joiner, or no world): nothing to write
	NoPlayer,        // the slot holds no added player
	NoBody,          // the slot's player holds no body
	OutOfRange,      // the position is outside the 16.16 range
};

struct SlotStagingResult {
	SlotStagingStatus status = SlotStagingStatus::NotAuthority;
	uint16_t handle = 0xFFFF;       // the body's wire handle when staged
	int32_t position_q16[3] = {};   // the words written
	std::string error;              // the refusal, for the probe's per-entry result
};

// Stage the body slot `slot` holds at the BMS position (x, y, z) in units: the
// same 16.16 conversion the scenario driver's setup pose takes
// (devtools::script_pose_from_mission).
SlotStagingResult stage_slot_position(NapiNPServerCtx &ctx, int slot, double x, double y,
		double z);

} // namespace opennova::inmatch
