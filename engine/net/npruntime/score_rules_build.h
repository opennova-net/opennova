#pragma once

// score.ini -> world::ScoreRules for the session's game type.
//
// Retail resolves the row index from g_GameType [orig: load_scoring_table_for_game_type
// @ 0x52D300] and then reads awards as `scoringTable[74 + slot]`, where `slot` comes
// from the shipped name table [orig: off_830348 @ 0x830348 — 38 {name, slot} pairs,
// slots 0..37, read directly out of Jointops.exe]. The k = 74 + slot mapping is
// corroborated by four branch semantics in GameEvent_ProcessScoring @ 0x52F550:
// [74]=FIRE in the fire case, [76]=FRIENDLYKILL on the same-team branch,
// [77]=ENEMYKILL, [78]=SUICIDE on the self-kill branch, [79]=DEATH on the victim leg.
//
// We do NOT reproduce the 452-byte row image (its layout is not witnessed — see
// engine/formats/score/score.h). Awards are looked up BY NAME, which needs no slot
// arithmetic and cannot silently drift if the row layout is ever witnessed differently.

#include <formats/score/score.h>
#include <runtime/world/world.h>

#include <cstdint>

namespace opennova::np {

// Resolve the row for `game_type` [orig: @0x52D300 via score::row_for_game_type] and
// pull the awards this port reads. Returns a `!valid` ScoreRules (all zero) when the
// config carries no row for that game type — every award then no-ops rather than
// falling back to a guessed value.
world::ScoreRules build_score_rules(const score::File &config, uint32_t game_type);

} // namespace opennova::np
