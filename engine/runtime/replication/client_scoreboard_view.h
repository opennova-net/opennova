// The Tab board's PRESENT-TIME projection: the folded 0x16 rows joined with
// the LIVE roster exactly as retail's drawer reads them at draw time — the
// connection-icon quality [orig: the out-of-band icon gate @0x4241e2], the
// team-mode live-entity gate [orig: the entity-null fallthrough @0x423d1b],
// and the "clan name" record join order [orig: "%s %s" @0x42fd46]. The two
// struct families stay separate on purpose: ClientScoreboardRow is the folded
// wire RECORD (names captured at 0x16 apply time so a leaver's line stays
// readable), while hud::ScoreboardEntry carries draw-time reads of the live
// roster that must move between 0x16s. This projection is the one place that
// joins them.
//
// Deliberately NOT projected until their drawers land (recorded D-HUD-24
// residuals): score2, the teams[] table (koth_hold/ctf_flag — the per-mode
// team-score header block and flag-carrier line [orig: @0x4232bf-0x423a12]),
// and rows_dropped_unknown_slot (a diagnostic).
#pragma once

#include <cstdint>
#include <vector>

#include <runtime/hud/hud_scoreboard.h>
#include <runtime/replication/client_state.h>

namespace opennova::replication {

struct ClientScoreboardHeader {
	bool known = false;
	bool team_mode = false;
	bool timed = false;  // solo-KOTH only [orig: @0x50dd54]
	// The header's player count is the accepted rows MINUS the trailer's
	// spectator count [orig: the subtraction @0x4231dd inside @0x423060].
	int players = 0;
	int spectators = 0;  // trailer byte
	int in_game = 0;     // trailer byte
};

// Fills `out_rows` with the drawn projection (wire order — the server sorts,
// the client never re-sorts) and returns the header counts.
ClientScoreboardHeader project_scoreboard(
		const ClientState &state, std::vector<hud::ScoreboardEntry> &out_rows);

// The header counts alone (no row materialization).
ClientScoreboardHeader scoreboard_header(const ClientState &state);

// (The game-type label rung lives with the rest of the game-type key maps:
// npwire game_type.h overlay_label_key [orig: @0x5b8680].)

} // namespace opennova::replication
