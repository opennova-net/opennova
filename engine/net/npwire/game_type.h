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
// @0x523620, §5.32/D-NET-205]. The Godot layer consumes this vocabulary
// through the NetProtocol binding (godot/src/network/nova_net_protocol.h).

#include <cstdint>
#include <world/game_type.h>

#include <mission/bms.h> // bms::AttribFlags — the mission-header game-mode bits

namespace opennova::game_type {

// The witnessed code words (retail LTGT_* keys).
// Code words and structural predicates live in world/game_type.h so runtime
// gameplay and this mission/wire adapter consume one vocabulary.

// Retail's mission-attrib -> g_GameType selection: the single-select game-mode
// bit from the mission header (bms::AttribFlags) picks the session code word.
// An ATTRIB_COOP mission derives the OBJECTIVE Co-op word 0x30020 (0x10010
// was an ASH_I5A capture value, never a default); a mission with NO
// multiplayer attrib (the retail training-mission case) resolves to the
// non-objective Co-op family. [orig: AI_GetTaskTypeFromFlags @ 0x40DAE0 ->
// Game_StartMission @ 0x524360]
constexpr uint32_t for_mission_mode(uint32_t attrib_mode) {
	using bms_flags = opennova::bms::AttribFlags;
	switch (static_cast<bms_flags>(attrib_mode)) {
	case bms_flags::Deathmatch:
		return kDeathmatch;
	case bms_flags::TeamDeathmatch:
		return kTeamDeathmatch;
	case bms_flags::Coop:
		return kObjectiveCoop;
	case bms_flags::KingOfTheHill:
		return kKingOfTheHill;
	case bms_flags::TeamKingOfTheHill:
		return kTeamKingOfTheHill;
	case bms_flags::SearchAndDestroy:
		return kSearchAndDestroy;
	case bms_flags::AttackAndDefend:
		return kAttackDefend;
	case bms_flags::CaptureTheFlag:
		return kCaptureTheFlag;
	case bms_flags::FlagBall:
		return kFlagBall;
	case bms_flags::AdvanceAndSecure:
		return kAdvanceAndSecure;
	case bms_flags::ConquerAndControl:
		return kConquerAndControl;
	default:
		return kCoop; // no (or unknown) multiplayer attrib -> stock/training Co-op
	}
}

static_assert(for_mission_mode(static_cast<uint32_t>(bms::AttribFlags::Coop)) == kObjectiveCoop);
static_assert(for_mission_mode(static_cast<uint32_t>(bms::AttribFlags::Deathmatch)) == kDeathmatch);
static_assert(for_mission_mode(static_cast<uint32_t>(bms::AttribFlags::SearchAndDestroy)) == kSearchAndDestroy);
static_assert(for_mission_mode(0) == kCoop, "no multiplayer attrib -> stock/training Co-op");

// --- The MULTI_PLAYER_HOST dialog's witnessed game-type rules (D-MNU-17) ----

// The GAME_TYPE spin's ALL-types value [orig: init_host_settings_dialog
// @ 0x558aee selects 255; the filter's show-all test @ 0x55717c].
inline constexpr int kHostFilterAll = 255;

// Which catalog rows the host MISSION_LIST shows at all: everything except
// STOCK (non-objective) Co-op — the pure-SP/training family
// [orig: the populate skip @ 0x558a70 — (code & 0x20000) == 0 &&
//  (code & 0xFFFDFFFF) == 0x10020 rows are never added].
constexpr bool host_list_visible(uint32_t g) {
	return !is_stock_coop(g);
}

// The 13-way code -> GAME_TYPE spin item value map the host dialog filters
// with (255 = ALL; 0 = unmapped) [orig: the shared switch —
// init_host_settings_dialog @ 0x558b16, filter_mission_list_by_game_type
// @ 0x5570a6, HostDialog_AddRemoveSelectedMissions @ 0x557cd9].
constexpr int host_filter_category(uint32_t g) {
	if (g == 0) return 11;
	if (g == kTeamDeathmatch) return 1;
	if (is_waypoint_family(g) && is_objective(g)) return 2;
	switch (g) {
	case kTeamKingOfTheHill: return 3;
	case kKingOfTheHill: return 4;
	case kSearchAndDestroy: return 5;
	case kAttackDefend: return 6;
	case kCaptureTheFlag: return 7;
	case kFlagBall: return 8;
	case kFlagMe: return 12;
	case kAdvanceAndSecure: return 9;
	case kConquerAndControl: return 10;
	default: return 0;
	}
}

// The SELECTED_MISSIONS type cell's gametext key in the GateTypeAbbrev
// section [orig: get_game_type_abbreviation @ 0x520fd0 host arm — the DM/TDM/
// KOTH/TKOTH/CTF/SD/AD/FB/FM/AAS/CAC literals + COOP for the waypoint family].
constexpr const char *host_abbreviation_key(uint32_t g) {
	if (is_waypoint_family(g)) return "COOP";
	if (g == 0) return "DM";
	switch (g) {
	case kTeamDeathmatch: return "TDM";
	case kKingOfTheHill: return "KOTH";
	case kTeamKingOfTheHill: return "TKOTH";
	case kCaptureTheFlag: return "CTF";
	case kSearchAndDestroy: return "SD";
	case kAttackDefend: return "AD";
	case kFlagBall: return "FB";
	case kFlagMe: return "FM";
	case kAdvanceAndSecure: return "AAS";
	case kConquerAndControl: return "CAC";
	default: return "";
	}
}

// The Tab board header's game-type rung — the Overlays-section gametext key
// [orig: HUD_GetGameTypeOverlayLabel @ 0x5b8680; the Co-op family first — the
// mask arm @ 0x5b8692 forgives the objective bit, so stock and objective
// Co-op share the rung]. The unnamed type-8 non-team mode shares DM's rung.
// An unlisted type returns "" and the rung stays blank.
constexpr const char *overlay_label_key(uint32_t g) {
	if (is_waypoint_family(g)) return "STROVER28";
	if (g == 0) return "STROVER29";
	switch (g) {
	case kTeamDeathmatch: return "STROVER64";
	case kKingOfTheHill: return "STROVER30";
	case kTeamKingOfTheHill: return "STROVER48";
	case kCaptureTheFlag: return "STROVER31";
	case kSearchAndDestroy: return "STROVER56";
	case kAttackDefend: return "STROVER57";
	case kFlagBall: return "STROVER58";
	case kFlagMe: return "STROVER29";
	case kAdvanceAndSecure: return "STROVER92";
	case kConquerAndControl: return "STROVER93";
	default: return "";
	}
}

// A freshly added mission's rotation ("Switch") default: on for team games
// without the objective bit [orig: the add branch @ 0x557e79..0x557e9f —
// entry+4416 = (code & 0x10000) && !(code & 0x20000)].
constexpr bool host_rotation_default(uint32_t g) {
	return (g & 0x10000u) != 0 && !is_objective(g);
}
// The rotation cell/toggle eligibility is the same predicate [orig: the
// column-2 toggle gate @ 0x558076].

static_assert(!host_list_visible(kCoop) && host_list_visible(kObjectiveCoop));
static_assert(host_list_visible(kTeamDeathmatch) && host_list_visible(kDeathmatch));
static_assert(host_filter_category(kObjectiveCoop) == 2);
static_assert(host_filter_category(kTeamDeathmatch) == 1);
static_assert(host_filter_category(kDeathmatch) == 11);
static_assert(host_filter_category(kConquerAndControl) == 10);
static_assert(host_rotation_default(kTeamDeathmatch) &&
		!host_rotation_default(kObjectiveCoop) &&
		!host_rotation_default(kDeathmatch));

}  // namespace opennova::game_type

namespace opennova::game_rules {

// Retail's Config_SetDefaults session-rule baseline, also witnessed in 00TRg's
// S2C 0x08 block (retail frame 146). These seed a fresh host's rule globals;
// the live wire fields they default live in npruntime GameConfig (which keeps
// zeros so a dev host stays inert). [orig: Config_SetDefaults; the g_* rule
// globals @ 0x24D2140.. — engine/net/npruntime game_config.h names each]
inline constexpr uint32_t kDefaultRespawnTime = 30;
inline constexpr uint32_t kDefaultTimeLimitMinutes = 10;
inline constexpr uint32_t kDefaultReplayEnabled = 1;
inline constexpr uint32_t kDefaultMaxTeamLives = 100;
inline constexpr uint32_t kDefaultScoreLimit = 50;
inline constexpr uint32_t kDefaultMaxScore = 5;
inline constexpr uint32_t kDefaultKothDelta = 5;
inline constexpr uint32_t kDefaultFlagReturnTicks = 210;
// Unnumbered ChangeTeam triggers capture over this many 1 Hz passes; setting
// zero or negative selects the retail instant branch. TakeoverSpeed 1 selects
// the control-delta base 24. [orig: Config_SetDefaults @0x54D030 writes
// dword_2550B78=15 / dword_2550B84=1; applied to g_capture_duration and
// g_capture_speed_setting @0x551D3E..0x551D55]
inline constexpr int32_t kDefaultCaptureDurationSeconds = 15;
inline constexpr int32_t kDefaultCaptureSpeedSetting = 1;
// [orig: Config_SetDefaults @0x54D030 writes g_MpNumTeams = 2]
inline constexpr uint32_t kDefaultNumTeams = 2;
inline constexpr uint32_t kDefaultRespawnTimeout = 5;
inline constexpr uint32_t kDefaultStartDelay = 0;
inline constexpr uint32_t kDefaultDestroyBuildings = 0;
inline constexpr uint32_t kDefaultDeathMessages = 1;

// The retail host's custom-message default, echoed by the 0x7B reply body.
// [orig: g_sessionvar_custom_text @ 0x522123]
inline constexpr char kCustomTextDefault[] = "Put your message here.";

// The wire callsign cap: the roster/entity name rides a Name[16] cstring
// (15 characters + NUL), so a longer callsign can never round-trip the wire
// echo and would break the joiner's name-match self-identification
// (D-NET-169). [orig: the entity+244 Name[16] copy — npwire/ingame_decode.h]
inline constexpr int kMaxCallsignLength = 15;

}  // namespace opennova::game_rules
