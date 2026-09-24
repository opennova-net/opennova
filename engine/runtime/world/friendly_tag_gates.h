#pragma once

// The friendly-tag drawer's SELECTION gates, shared by every walk that feeds
// the HUD element (D-HUD-20): the authority's World walks in
// world/friendly_tags.cpp and the joiner's roster walk in
// runtime/replication/client_roster_tags.cpp. One implementation so the two walks cannot
// drift — the roster copy had lost the item-def bail.
// [orig: HUD_DrawEntityLabel entry bails @0x5a39df..0x5a39fb and the
//  HUD_DrawFriendlyTagsPass gates @0x5a44c7..0x5a44f8 / @0x5a4552..0x5a457d]
// Witness record: docs/interface/hud-re.md (D-HUD-20).

#include <runtime/world/entity.h>

#include <cstdint>

namespace opennova::world {

// The drawer's entry bails: never the local player [orig: entity ==
// playerEntity @0x5a39df], never a CARRIED entity (Flags & 1 — hidden while
// attached) [orig: @0x5a39eb], and only with a resolved item def
// [orig: itemDef == NULL @0x5a39fb]. Dead entities are NOT bailed: the bad
// tier's downed legs are how a fallen teammate keeps its label.
inline bool friendly_tag_entry_bails(bool is_local, uint32_t flags,
                                     bool has_item_def) {
    if (is_local) return true;
    if ((flags & kEntityFlagCarried) != 0) return true;
    if (!has_item_def) return true;
    return false;
}

// The pass gates, in retail order. The pass admits neutral team 0 or the
// local team, or anyone while the death screen is up [orig: pool-0
// @0x5a44c7..0x5a44f8; players @0x5a4552..0x5a456b]; then `g_GameType ||
// death screen` [orig: @0x5a44e8 / @0x5a456d]. The drawer then compares the
// entity team with the local player's after its death-screen arm has drawn
// every team [orig: HUD_DrawEntityLabel death arm @0x5a3c33..0x5a3c3a; team
// compare @0x5a3c6b..0x5a3c7d]: an unequal team, neutral 0 included, draws
// magenta only under `g_enemyTagsVisible` and otherwise bails
// [orig: @0x5a3c7f..0x5a3c95]. The grant's S2C 0x0A edge only rises with the
// death screen here, whose arm already admits every team; its spectator-mode
// and action-130 writers are unported, so the drawer's ordinary-play leg is
// the bail. Magenta outside the death screen can only reach team 0: the pass
// gate has already dropped a real enemy.
inline bool friendly_tag_pass_gates(uint8_t team, uint8_t local_team,
                                    bool death_screen, uint32_t game_type) {
    if (team != 0 && team != local_team && !death_screen) return false;
    if (game_type == 0 && !death_screen) return false;
    if (team != local_team && !death_screen) return false;
    return true;
}

} // namespace opennova::world
