#pragma once

// The ONE game-text seam the engine's feed builders read through: the shape
// of retail's GameText_GetStringWithFallback — a table section, a key, and
// the fallback returned when the key is absent (an absent table answers the
// fallback too). The embedder binds it over its string-table document; the
// builders never see the table. The end-round ladder keeps its own
// presence-aware form (hud/end_round_overlay.h EndRoundTextLookup) because
// its present-but-empty fold is witnessed.

#include <cstdio>
#include <functional>
#include <string>

namespace opennova::hud {

// The game-text table (loaded at boot [orig: Game_InitSubsystems @ 0x4A6CD0]) and the
// sections the game reads from it by name, each witnessed where it is read: the HUD's
// overlay lines, the weapon names, the waypoint names, the kill-feed sentences, the
// client lines, the loading-screen text and the objective header.
inline constexpr const char *kGameTextTable = "gametext.bin";
inline constexpr const char *kGameTextOverlays = "Overlays";
inline constexpr const char *kGameTextWepDes = "WepDes";
inline constexpr const char *kGameTextWPNames = "WPNames";
inline constexpr const char *kGameTextCannedMsg = "Canned Msg";
inline constexpr const char *kGameTextClient = "Client";
inline constexpr const char *kGameTextLoadingText = "LoadingText";
inline constexpr const char *kGameTextMisc = "Misc";
inline constexpr const char *kGameTextEpilog = "Epilog";
inline constexpr const char *kGameTextHud = "hud";

// One string of a table: its section and its key.
struct GameTextKey {
	const char *section;
	const char *key;
};

// The gametext keys a single-player mission's flow reads, each where it is read: the chat line a
// newly shown objective posts first [orig: HUD_ShowObjectiveNotification @0x5ba2e0, the key
// @0x5ba39b]; the objectives panel's heading [orig: HUD_DrawWinConditions @0x5ba940, @0x5ba986]; the
// waypoint label's words between its distance and its name [orig: HUD_DrawWaypointNameAndDistance
// @0x5947a0, @0x594956]; the name of a waypoint the mission's text names none for [orig:
// HUD_GetWaypointName @0x594630, @0x59477b]; the win screen's objectives, enemies, team and friendly
// fire lines [orig: Cine_EpilogStateMachineUpdate @0x576240, @0x5765e1, @0x576649, @0x5766c9,
// @0x57672a]; the lose screen's title [orig: Cinematic_EpilogUpdate @0x574491, @0x574623]; the end
// screens' key help [orig: @0x57471c (the lose screen), @0x5767b9 (the win screen)].
inline constexpr GameTextKey kGameTextNewObjective{kGameTextMisc, "STRMISC_NEWOBJECTIVE"};
inline constexpr GameTextKey kGameTextMissionObjectives{kGameTextOverlays, "STROVER_MISSIONOBJECTIVES"};
inline constexpr GameTextKey kGameTextWaypointTo{kGameTextHud, "mto"};
inline constexpr GameTextKey kGameTextWaypointNameDefault{kGameTextWPNames, "STRWPNAMEDEFAULT"};
inline constexpr GameTextKey kGameTextEpilogObjectiveBonus{kGameTextEpilog, "STREPILOG_OBJECTIVEBONUS"};
inline constexpr GameTextKey kGameTextEpilogEnemyUnits{kGameTextEpilog, "STREPILOG_ENEMYUNITS"};
inline constexpr GameTextKey kGameTextEpilogTeamUnits{kGameTextEpilog, "STREPILOG_TEAMUNITS"};
inline constexpr GameTextKey kGameTextEpilogFriendlyUnits{kGameTextEpilog, "STREPILOG_FRIENDLYUNITS"};
inline constexpr GameTextKey kGameTextMissionFailed{kGameTextOverlays, "STROVER_MISSION_FAILED"};
inline constexpr GameTextKey kGameTextEpilogKeyInfo{kGameTextEpilog, "STREPILOG_KEYINFO"};
// Every one, in the order above.
inline constexpr GameTextKey kMissionFlowKeys[] = {
		kGameTextNewObjective, kGameTextMissionObjectives, kGameTextWaypointTo,
		kGameTextWaypointNameDefault, kGameTextEpilogObjectiveBonus, kGameTextEpilogEnemyUnits,
		kGameTextEpilogTeamUnits, kGameTextEpilogFriendlyUnits, kGameTextMissionFailed,
		kGameTextEpilogKeyInfo,
};

// The keys the game forms from a number of a mission's records, sprintf's "%s%03i" of a prefix,
// each in its section of the mission's own text table (mission_sidecars' "text" row): a named
// location marker's, the next in spawn order [orig: Entity_SpawnFromBMSRecord @0x40f182..0x40f221];
// an entity's name [orig: Entity_SpawnFromBMSRecord @0x40ecbf..0x40ed0a, sprintf("STRNAME%03i",
// rec+4)]; a waypoint's [orig: Entity_SpawnFromBMSRecord @0x40f0be..0x40f0e0; HUD_GetWaypointName
// @0x59473d]; an objectives panel row's [orig: HUD_DrawWinConditions @0x5ba940]; a won or lost
// sub-goal's chat line [orig: EventAction_Dispatch case 14 @0x454552, case 15 @0x45460c]; a shown
// sub-goal's directive [orig: HUD_ShowObjectiveNotification @0x5ba316, @0x5ba34b]; a triggered text's
// line [orig: HUD_DisplayTriggeredText @0x51f190].
struct TextKeyForm {
	const char *section;
	const char *prefix;
};
inline constexpr TextKeyForm kLocationKey{"Locations", "LOCATION"};
inline constexpr TextKeyForm kPeopleNameKey{"PeopleNames", "STRNAME"};
inline constexpr TextKeyForm kWaypointNameKey{kGameTextWPNames, "STRWPNAME"};
inline constexpr TextKeyForm kWinConditionKey{"WinConditions", "STRWINCOND"};
inline constexpr TextKeyForm kWinMessageKey{"WinConditions", "STRWINMSG"};
inline constexpr TextKeyForm kLoseMessageKey{"LoseConditions", "STRLOSEMSG"};
inline constexpr TextKeyForm kWinDirectiveKey{"WinConditions", "STRWINDIRECTIVE"};
inline constexpr TextKeyForm kLoseDirectiveKey{"LoseConditions", "STRLOSEDIRECTIVE"};
inline constexpr TextKeyForm kTriggeredTextKey{"Triggered Text", "ID"};

// The key of a number: the form's prefix, then the number in at least three digits ("%s%03i":
// STRNAME005, ID1234).
inline std::string text_key(const TextKeyForm &form, int number) {
	char key[64];
	std::snprintf(key, sizeof(key), "%s%03i", form.prefix, number);
	return key;
}

// An EMPTY function is the "no string table" binding: every consumer answers
// the fallback through it, so a builder never tests the target before calling
// (call through game_text or the same guard, never the bare function).
using GameTextLookup = std::function<std::string(
		const char *section, const char *key, const char *fallback)>;

inline std::string game_text(const GameTextLookup &lookup, const char *section, const char *key,
		const char *fallback) {
	return lookup ? lookup(section, key, fallback) : std::string(fallback);
}
inline std::string game_text(const GameTextLookup &lookup, const GameTextKey &key, const char *fallback) {
	return game_text(lookup, key.section, key.key, fallback);
}
// The string of a form's key for `number`, in the form's section.
inline std::string game_text(const GameTextLookup &lookup, const TextKeyForm &form, int number,
		const char *fallback) {
	return game_text(lookup, form.section, text_key(form, number).c_str(), fallback);
}

} // namespace opennova::hud
