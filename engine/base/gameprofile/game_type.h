#pragma once

// The g_GameType vocabulary — the witnessed code words, the two witnessed BITS
// the engine tests, and every witnessed selector over them (the mission-attrib
// derivation, the score-table row, the team count, the host dialog's rules).
// ONE home (ADR 0043 d4): the runtime gameplay, the mission/wire binding and
// the shell all consume this header; it sits in base/ so the wire-only net
// group and the runtime both reach it without either including the other.
// Code words are opaque beyond the two bits: never decompose them further
// (e.g. Search & Destroy's 0x90002 high bits carry no independently witnessed
// meaning).
//
// Witnesses: [orig: g_GameType @0x24D2128; Server_CheckWinConditions
// @0x51AD40; ScoreRules_GetPrimaryScoreField (ex sub_52C850) @0x52C850]; the
// retail LTGT_* gametext key table [orig: LoadingScreen g_GameType switch —
// docs/interface/loading-screen-re.md]; the waypoint-family selector
// [orig: NapiNPMsg_0x7B_BuildPayload selector @0x507822, §5.32]; the objective
// bit gate on the 0x0A sub-block 3 [orig: 0x430361..0x4303D0, §5.9]; the
// two-part stock-Co-op test [orig: serialize_mission_info_to_datastream
// @0x523620, §5.32/D-NET-205]. The Godot layer consumes this vocabulary
// through the NetProtocol binding (godot/src/network/net_protocol.h).

#include <cstdint>

#include <formats/mission/bms.h> // bms::AttribFlags — the mission-header game-mode bits

namespace opennova::game_type {

inline constexpr uint32_t kDeathmatch = 0x00000u;
inline constexpr uint32_t kKingOfTheHill = 0x00001u;
inline constexpr uint32_t kFlagMe = 0x00008u;
inline constexpr uint32_t kTeamDeathmatch = 0x10000u;
inline constexpr uint32_t kTeamKingOfTheHill = 0x10001u;
inline constexpr uint32_t kAttackDefend = 0x10002u;
inline constexpr uint32_t kCaptureTheFlag = 0x10004u;
inline constexpr uint32_t kFlagBall = 0x10008u;
inline constexpr uint32_t kAdvanceAndSecure = 0x10010u;
inline constexpr uint32_t kCoop = 0x10020u;
inline constexpr uint32_t kObjectiveCoop = 0x30020u;
inline constexpr uint32_t kConquerAndControl = 0x50010u;
inline constexpr uint32_t kSearchAndDestroy = 0x90002u;

inline constexpr uint32_t kTeamBit = 0x10000u;
inline constexpr uint32_t kObjectiveBit = 0x20000u;
inline constexpr uint32_t kWaypointFamilyMask = 0xFFFDFFFFu;
inline constexpr uint32_t kWaypointFamilyValue = 0x10020u;

constexpr bool is_team(uint32_t game_type) {
    return (game_type & kTeamBit) != 0;
}

constexpr bool is_objective(uint32_t game_type) {
    return (game_type & kObjectiveBit) != 0;
}

constexpr bool is_waypoint_family(uint32_t game_type) {
    return (game_type & kWaypointFamilyMask) == kWaypointFamilyValue;
}

constexpr bool is_stock_coop(uint32_t game_type) {
    return is_waypoint_family(game_type) && !is_objective(game_type);
}

// Every g_GameType code word Game_StartMission can produce; anything else is
// a typo, not a mode (what the dedicated host's --game-type override is
// validated against). [orig: Game_StartMission @0x524360 type switch]
constexpr bool is_retail_code_word(uint32_t game_type) {
    switch (game_type) {
    case kDeathmatch:
    case kKingOfTheHill:
    case kFlagMe:
    case kTeamDeathmatch:
    case kTeamKingOfTheHill:
    case kAttackDefend:
    case kCaptureTheFlag:
    case kFlagBall:
    case kAdvanceAndSecure:
    case kCoop:
    case kObjectiveCoop:
    case kConquerAndControl:
    case kSearchAndDestroy:
        return true;
    default:
        return false;
    }
}

// The raw retail score-table row selector. Row zero is a real selector result
// that the consumers normalize to Co-op row 2; it is distinct from Deathmatch
// row 11. Flag Me deliberately maps to 12 even though the table has only rows
// 0..11, so its event scorer, FIELD loader, and status-value copy all fail
// closed. [orig: load_scoring_table_for_game_type @0x52D300;
// GameEvent_ProcessScoring @0x52F550; Server_BuildStatusReport @0x530A60]
constexpr uint8_t score_table_index(uint32_t game_type) {
    if (game_type == kDeathmatch)
        return 11;
    if (game_type == kTeamDeathmatch)
        return 1;
    if (is_waypoint_family(game_type) && is_objective(game_type))
        return 2;
    switch (game_type) {
    case kTeamKingOfTheHill:
        return 3;
    case kKingOfTheHill:
        return 4;
    case kSearchAndDestroy:
        return 5;
    case kAttackDefend:
        return 6;
    case kCaptureTheFlag:
        return 7;
    case kFlagBall:
        return 8;
    case kAdvanceAndSecure:
        return 9;
    case kConquerAndControl:
        return 10;
    case kFlagMe:
        return 12;
    default:
        return 0;
    }
}

constexpr bool has_score_table(uint32_t game_type) {
    return score_table_index(game_type) <= 11;
}

// Retail permits four active sides only for the three symmetric team modes.
// Every other team code serializes exactly two sides, regardless of the host's
// mp_numteams setting; solo modes serialize zero. This count is shared by both
// live and end-round scoreboards.
// [orig: Server_BuildAndBroadcastScoreboard @0x50D960;
// Server_BuildEndOfRoundScoreboard @0x508F30]
constexpr uint8_t active_team_count(uint32_t game_type, uint8_t configured) {
    if (!is_team(game_type))
        return 0;
    const bool supports_four =
        game_type == kTeamDeathmatch || game_type == kTeamKingOfTheHill ||
        game_type == kFlagBall;
    return configured == 4 && supports_four ? uint8_t{4} : uint8_t{2};
}

static_assert(kObjectiveCoop == (kCoop | kObjectiveBit));
static_assert(is_waypoint_family(kCoop) && is_waypoint_family(kObjectiveCoop));
static_assert(!is_waypoint_family(kTeamDeathmatch));
static_assert(active_team_count(kTeamDeathmatch, 4) == 4);
static_assert(active_team_count(kAdvanceAndSecure, 4) == 2);
static_assert(active_team_count(kFlagMe, 4) == 0);
static_assert(score_table_index(kDeathmatch) == 11);
static_assert(score_table_index(kCoop) == 0);
static_assert(score_table_index(kObjectiveCoop) == 2);
static_assert(!has_score_table(kFlagMe));
static_assert(is_retail_code_word(kDeathmatch) && is_retail_code_word(kObjectiveCoop));
static_assert(!is_retail_code_word(2) && !is_retail_code_word(0x10003u));

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

// The session g_GameType word a mission HEADER implies: the mission
// catalog's derivation over the raw attrib_flags word (no multiplayer bit ->
// stock Co-op 0x10020). The one home of this derivation -- every embedder
// (the listen bring-up, the dedicated host, the shell, the ctest rig) calls
// it. [orig: AI_GetTaskTypeFromFlags @0x40DAE0 -> Game_StartMission
// @0x524360; docs/net/novaworld-net-re.md 5.2c]
inline uint32_t for_mission_attribs(bms::AttribFlags attrib_flags) {
	return for_mission_mode(bms::selected_game_mode(attrib_flags));
}
// The same over the raw header word a world retains (World::mission_attrib_flags).
inline uint32_t for_mission_attribs(uint32_t attrib_flags) {
	return for_mission_attribs(static_cast<bms::AttribFlags>(attrib_flags));
}

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
// the live wire fields they default live in the session GameConfig (which keeps
// zeros so a dev host stays inert). [orig: Config_SetDefaults; the g_* rule
// globals @ 0x24D2140.. — engine/runtime/inmatch game_config.h names each]
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
// Deploy waves are asymmetric by default: unnumbered/base spawns deploy
// immediately, while numbered zones release one player every ten seconds.
// [orig: Config_SetDefaults @0x54D030 writes dword_2550B7C=0 and
// dword_2550B80=10; apply_session_settings_to_globals @0x551D3E..0x551D55]
inline constexpr int32_t kDefaultSpawnWaveTimeBase = 0;
inline constexpr int32_t kDefaultSpawnWaveTimeZone = 10;
// Retail names the parsed setting `nodefaultspawnpoints` and the live global
// g_respawn_requires_team_dead, but the gameplay predicate checks spawn-zone
// availability, not living players: Default Spawn is denied while the team has
// an unnumbered zone or a fully controlled numbered zone.
// [orig: Config_SetDefaults @0x54D34C writes dword_2550B94=0;
// Config_ParseSettingsLine @0x550C73; Entity_HasAliveEntityOfTeam @0x4FC7B0]
inline constexpr uint32_t kDefaultSpawnRequiresNoTeamZone = 0;
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
// (D-NET-169). [orig: the entity+244 Name[16] copy — net/npwire/ingame_decode.h]
inline constexpr int kMaxCallsignLength = 15;

}  // namespace opennova::game_rules
