#pragma once

// The DEATH deploy screen's content feed (D-HUD-19 residue b): the two row
// loops and the STATIC message rules of the death-screen refresh, Godot-free
// with every lookup injected. The presenter turns these into list rows and
// widget text; the wire facts come from the joiner's ClientState (the 0x0A
// sub-block-0 timers, the 0x6E wave groups) or the authority's own view.
// [orig: UI_UpdateDeathScreenContent @0x5536a0 — the STATIC_RESPAWN_MSG1
//  arm @0x5538e7..0x553a7b, the zone loop + ListWidget_SortRows @0x553c5a,
//  the occupant loop @0x553c5f..0x553de3, STATIC_PSPRESPAWN_MSG1 /
//  STATIC_MEDIC_MSG1 / STATIC_CALLMEDIC_MSG @0x553e10..0x553f60]
// Witness record: docs/interface/hud-re.md (D-HUD-19).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace opennova::world {

// The DEATH deploy screen re-reads its content every 16 ticks (0.256 s at the
// 62.5 Hz tick); the cadence rides the tick, not a wall-clock timer.
// [orig: every 16 ticks @0x55477d]
inline constexpr int32_t kDeployRefreshTicks = 16;

// One player queued on a zone's wave [orig: the member handles at
// unk_A85CC4[zoneIdx*8+i], named through entity->Name @0x553d5a..0x553d7f].
struct DeployOccupant {
    uint16_t handle = 0xFFFF;
    std::string name;
    bool self = false; // "<b><cFF4040>** %s **" for the local player
};

// One team-owned spawn zone the list shows [orig: the first loop
// @0x553aef..0x553c55 — def present, local team, SECURED (zone-timer
// EntryById[9] >= [10]) with attrib 0x40000; text "<team color>'<letter>' <name>",
// row value = index + 1].
struct DeployZoneRow {
    int index = 0;        // SpawnZoneList index (0-based)
    char letter = 'A';    // 'A' + index
    std::string name_key; // WPNames/STRWPNAME%03d (index + 1)
    bool secured = false; // the first loop's listing gate
    uint16_t wave_countdown = 0; // entity+548 from the 0x6E fold
    std::vector<DeployOccupant> occupants; // the second loop's members
};

// The compiled row model of SPAWNPOINTS_LIST after both loops: the Default
// row (value 0), the zone rows (value index+1) in the witnessed
// ListWidget_SortRows(list, 0, 1) order — a case-insensitive ascending sort
// over the row TEXT [orig: cmp @0x6448a0 with sortParams {0 string, 1
// ascending} -> stricmp(a, b)] — then, per team zone with a wave entry, the
// occupant rows (value -1) inserted AFTER the row whose value is index+1,
// followed by ONE empty row. A zone with occupants but NO list row (it was
// not secured — the second loop has no secured gate) inserts at position 0,
// i.e. right after the Default row: the witnessed quirk, kept.
// [orig: the search @0x553c9c..0x553cbd, `insert_pos = 0` default @0x553c8e,
//  UIList_AddRow(text, -1, 0, insert_pos + 1) @0x553d8b, the empty row
//  @0x553dbb]
struct DeployListRow {
    std::string text;
    int value = 0; // 0 default, index+1 zone, -1 occupant/blank (never a pick)
};

// The zone-row text before the sort: "<color>'<letter>' <name>" [orig:
// sprintf("%s'%c' %s") @0x553c1f]; the Default row "<color>'<key>' <home>"
// [orig: @0x553b6b]. `team_color_tag` is "<c4040FF>" or "<cFF2020>" for team 2
// [orig: @0x553b1e..0x553b38].
struct DeployListInput {
    std::string team_color_tag;
    std::string default_key;  // Menu/DEFAULT_SPAWN_KEY
    std::string default_home; // Menu/<dword_7C75E0 key> ("HOME")
    std::vector<DeployZoneRow> zones;
    // WPNames/STRWPNAME%03d resolver (the embedder's gametext table).
    std::function<std::string(const std::string &key)> zone_name;
};

std::vector<DeployListRow> build_deploy_rows(const DeployListInput &in);

// The STATIC_RESPAWN_MSG1 arm: hidden, then the penalty timer wins
// ("%s  <cFF4040>%i" over Overlays/STROVER_PENALTYTIMER), else the wave zone
// listing the local player (`word_A85BC0 != -1` and SpawnZoneList_IndexOf
// >= 0): a NUMBERED zone (entity+538) prints "'<WPNames name>':  <cFF4040><countdown>",
// a lettered one "%c:  <cFF4040>%d" [orig: @0x5538e7..0x553a7b].
struct DeployStatusLine {
    enum class Kind { None, Penalty, Wave };
    Kind kind = Kind::None;
    int seconds = 0;   // penalty seconds or the wave countdown
    bool numbered = false;
    int zone_index = -1; // the wave zone's list index (Wave only)
};

struct DeployStatusInput {
    int penalty_seconds = 0;       // dword_A85B5C (0x0A slot+360)
    int self_zone_index = -1;      // SpawnZoneList_IndexOf(word_A85BC0), -1 none
    bool self_zone_numbered = false; // entity+538 nonzero
    int self_zone_countdown = 0;   // entity+548
};

DeployStatusLine build_deploy_status(const DeployStatusInput &in);

// The STATIC_RESPAWN_MSG1 text for a status line — the three sprintf arms
// [orig: @0x5538e7..0x553a7b: penalty "%s  <cFF4040>%i" over the
// Overlays/STROVER_PENALTYTIMER label; numbered wave "'%s':  <cFF4040>%d" over
// the WPNames name; lettered wave "%c:  <cFF4040>%d" with 'A' + index
// @0x553a5b]. `penalty_label` and `zone_name` are the embedder-resolved
// strings (with their fallbacks already applied); "" for Kind::None.
std::string deploy_status_text(const DeployStatusLine &line,
                               const std::string &penalty_label,
                               const std::string &zone_name);

// The other three statics [orig: @0x553e10..0x553f60]: STATIC_PSPRESPAWN_MSG1
// shows while the spawn-target hold (dword_A85B68, slot+364) is nonzero;
// STATIC_MEDIC_MSG1 + STATIC_CALLMEDIC_MSG show while the local revive
// window (dword_A85B60, slot+368) is nonzero AND the local entity's +0x1E0
// word is zero (`!weaponSlots[16]` @0x553ec5). That word is the "a medic is
// reviving me" latch — set by S2C 0x3A (NapiNPClientMsg_0x03A @0x422680) and
// by the host's revive sender (@0x517cd0 stamps the victim), cleared by the
// local respawn (Game_InitNewRound @0x422740) and mission start; it is NOT a
// mount reference (the pre-2026-09-10 reading; the IDB's weaponSlots[44]
// blob swallows it).
struct DeployStaticsInput {
    int hold_seconds = 0;
    int revive_seconds = 0;
    bool local_medic_reviving = false; // entity+0x1E0 != 0
};
struct DeployStaticsVisibility {
    bool psp_respawn = false;
    bool medic = false; // both MEDIC_MSG1 and CALLMEDIC_MSG
};
DeployStaticsVisibility deploy_statics_visibility(const DeployStaticsInput &in);



// Both instruction widgets and the permanent-death status pair. A secured
// spawn leaves the first widget's prior text intact and hides the second;
// the later RESPawn1 assignment overwrites the intermediate RESPawn2 text.
// [orig: UI_UpdateDeathScreenContent @ 0x5536A0; sub_43B910 @ 0x43B910]
struct DeployInstructionsInput {
    uint32_t game_type = 0;
    uint8_t team = 0;
    bool dead = false;
    bool permanent_death = false;
    bool spectators_allowed = false;
    bool check_secured_spawn = false; // S2C 0x0F game_flags bit1
    bool has_spawn_zones = false;
    bool has_full_team_spawn = false; // actual timer entry[9] >= entry[10]
    std::string player_name;
    std::string clan;
    std::string kill_announcement;
    int32_t round_ticks = -1;
    int alive_players = 0;
};
struct DeployInstructions {
    bool permanent_death = false;
    bool show_first = false;
    bool show_second = false;
    bool replace_first = true;
    std::string first_text;
    std::string second_text;
    bool show_round_status = false;
    std::string round_text;
    std::string remaining_players_text;
};
using DeployTextLookup = std::function<std::string(
        const char *section, const char *key, const char *fallback)>;
DeployInstructions build_deploy_instructions(
        const DeployInstructionsInput &in, const DeployTextLookup &lookup);

// The DEATH screen's STATIC facts for one client as one value the embedder
// fills (its Godot record wraps it by value, ADR 0043 d10): the sub-block-0
// timers, the queued status line, the psp/medic show gates and the medic-call
// cooldown/serial. The witnesses live on the builders above and on the
// embedder's fold of the 0x0A / 0x6E state.
struct DeployScreenStatus {
    int penalty_seconds = 0;
    int revive_seconds = 0;
    int hold_seconds = 0;
    DeployStatusLine line;
    DeployStaticsVisibility statics;
    DeployInstructions instructions;
    std::string respawn_text;
    int medic_cooldown_ticks = 0;
    int medic_request_serial = 0;
};

} // namespace opennova::world
