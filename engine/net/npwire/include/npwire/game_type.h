#pragma once

// The g_GameType vocabulary — the witnessed code words and the two witnessed
// BITS the engine tests. Code words are opaque beyond those two bits: never
// decompose them further (e.g. Search & Destroy's 0x90002 high bits carry no
// independently witnessed meaning).
//
// Witnesses: the retail LTGT_* gametext key table
// [orig: LoadingScreen g_GameType switch — docs/interface/loading-screen-re.md,
// g_GameType @ 0x24d2128]; the waypoint-family selector
// [orig: NapiNPMsg_0x7B_BuildPayload selector @0x507822, §5.32]; the objective
// bit gate on the 0x0A sub-block 3 [orig: 0x430361..0x4303D0, §5.9]; the
// two-part stock-Co-op test [orig: serialize_mission_info_to_datastream
// @0x523620, §5.32/D-NET-205]. GDScript twin: the mission-attrib table in
// godot/adapter/world/host_session_config.gd (GUT-pinned).

#include <cstdint>

namespace opennova::game_type {

// The witnessed code words (retail LTGT_* keys).
inline constexpr uint32_t kDeathmatch        = 0x00000; // LTGT_DM
inline constexpr uint32_t kKingOfTheHill     = 0x00001; // LTGT_KOTH
inline constexpr uint32_t kTeamDeathmatch    = 0x10000; // LTGT_TDM
inline constexpr uint32_t kTeamKingOfTheHill = 0x10001; // LTGT_TKOTH
inline constexpr uint32_t kAttackDefend      = 0x10002; // LTGT_AD
inline constexpr uint32_t kCaptureTheFlag    = 0x10004; // LTGT_CTF
inline constexpr uint32_t kFlagBall          = 0x10008; // LTGT_FB
inline constexpr uint32_t kAdvanceAndSecure  = 0x10010; // LTGT_AAS
inline constexpr uint32_t kCoop              = 0x10020; // LTGT_COOP (stock/training Co-op)
inline constexpr uint32_t kSearchAndDestroy  = 0x90002; // LTGT_SD
inline constexpr uint32_t kConquerAndControl = 0x50010; // LTGT_CAC

// The two witnessed bits.
inline constexpr uint32_t kObjectiveBit       = 0x20000;    // 0x0A sub-block 3 gate
inline constexpr uint32_t kWaypointFamilyMask  = 0xFFFDFFFF; // the §5.32 selector pair
inline constexpr uint32_t kWaypointFamilyValue = 0x10020;

// Objective/waypoint Co-op — the shipped stock-mission gametype.
inline constexpr uint32_t kObjectiveCoop = kCoop | kObjectiveBit;
static_assert(kObjectiveCoop == 0x30020);

// The waypoint gametype family: stock Co-op 0x10020 AND objective Co-op
// 0x30020 (the objective bit is the one the mask forgives).
constexpr bool is_waypoint_family(uint32_t g) {
	return (g & kWaypointFamilyMask) == kWaypointFamilyValue;
}
constexpr bool is_objective(uint32_t g) {
	return (g & kObjectiveBit) != 0;
}
// Retail's literal two-part stock-Co-op test (deliberately narrower than the
// 0x7B selector — D-NET-205).
constexpr bool is_stock_coop(uint32_t g) {
	return is_waypoint_family(g) && !is_objective(g);
}

static_assert(is_waypoint_family(kCoop) && is_waypoint_family(kObjectiveCoop));
static_assert(!is_waypoint_family(kTeamDeathmatch) && !is_waypoint_family(kDeathmatch));
static_assert(is_stock_coop(kCoop) && !is_stock_coop(kObjectiveCoop));
static_assert(is_objective(kObjectiveCoop) && !is_objective(kCoop));

}  // namespace opennova::game_type
