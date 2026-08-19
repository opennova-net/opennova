#include "netsim/client_scoreboard_view.h"

namespace opennova::netsim {

namespace {

ClientScoreboardHeader header_of(const ClientScoreboard &sb) {
	ClientScoreboardHeader h;
	h.known = sb.known;
	h.team_mode = sb.team_mode;
	h.timed = sb.timed;
	h.spectators = sb.spectator_count;
	h.in_game = sb.in_game_count;
	// Accepted rows minus the trailer's spectator count — the witnessed
	// header arithmetic [orig: @0x4231dd]. Retail never clamps: rows and the
	// trailer come from the same serializer, but a clamp keeps a malformed
	// trailer from drawing a negative count.
	h.players = static_cast<int>(sb.rows.size()) - h.spectators;
	if (h.players < 0) h.players = 0;
	return h;
}

} // namespace

ClientScoreboardHeader project_scoreboard(
		const ClientState &state, std::vector<hud::ScoreboardEntry> &out_rows) {
	const ClientScoreboard &sb = state.scoreboard;
	out_rows.clear();
	out_rows.reserve(sb.rows.size());
	for (const ClientScoreboardRow &r : sb.rows) {
		const ClientRosterSlot &slot = state.roster[r.slot_id];
		hud::ScoreboardEntry e;
		e.slot_id = r.slot_id;
		e.status_flags = r.status_flags;
		e.score1 = r.score1;
		e.team = r.team;
		e.spectator = r.spectator;
		// The board's name is the ROW-carried join in the parser's own order
		// — "clan name" [orig: sprintf("%s %s", clan, name) @0x42fd46 into
		// the 56-byte record] — which is what keeps a leaver's line readable
		// after the 0x46 slot wipe.
		e.name = r.clan.empty() ? r.name : r.clan + " " + r.name;
		// The connection icon and the team-mode draw gate read the LIVE slot:
		// a wiped slot's 0 quality draws no icon [orig: the out-of-band gate
		// @0x4241e2], and a row without a live entity vanishes from team
		// boards [orig: the entity-null fallthrough @0x423d1b].
		e.quality = slot.quality;
		e.has_entity = slot.bound && slot.entity_slot >= 0;
		out_rows.push_back(std::move(e));
	}
	return header_of(sb);
}

ClientScoreboardHeader scoreboard_header(const ClientState &state) {
	return header_of(state.scoreboard);
}

} // namespace opennova::netsim
