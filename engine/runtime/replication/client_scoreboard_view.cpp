#include <runtime/replication/client_scoreboard_view.h>

namespace opennova::replication {

namespace {

ClientScoreboardHeader header_of(const ClientScoreboard &sb) {
	ClientScoreboardHeader h;
	h.known = sb.known;
	h.team_mode = sb.team_mode;
	h.timed = sb.timed;
	h.spectators = sb.spectator_count;
	h.in_game = sb.in_game_count;
	// Accepted rows minus the trailer's spectator count — the witnessed
	// header arithmetic [orig: @0x4231dd], unclamped like retail (rows and
	// the trailer come from the same serializer).
	h.players = static_cast<int>(sb.rows.size()) - h.spectators;
	return h;
}

} // namespace

ClientScoreboardHeader project_scoreboard(
		const ClientState &state, std::vector<hud::ScoreboardEntry> &out_rows,
		const ScoreboardEntityLookup &entity_of) {
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
		// — "clan name" [orig: the non-empty guard @0x42fd38, then
		// sprintf("%s %s", clan, name) @0x42fd46..0x42fd4c into the 56-byte record; an
		// empty first string copies the name alone @0x42fd6a] — which is
		// what keeps a leaver's line readable after the 0x46 slot wipe.
		e.name = r.clan.empty() ? r.name : r.clan + " " + r.name;
		// The record's 32-byte name field [orig: Napi_CopyString(rec, .., 32)
		// @0x42fd59/@0x42fd6a].
		if (e.name.size() > 31) e.name.resize(31);
		e.label = r.label;
		// The connection icon and the team-mode draw gate read the LIVE slot:
		// a wiped slot's 0 quality draws no icon [orig: the out-of-band gate
		// @0x4241e2], and a row without a live entity vanishes from team
		// boards [orig: the entity-null fallthrough @0x423d1b].
		e.quality = slot.quality;
		e.has_entity = slot.bound && slot.entity_slot >= 0;
		// The team and class the drawer reads off the live entity
		// [orig: @0x423d21 / @0x423d8a].
		if (e.has_entity && entity_of) {
			const ScoreboardEntityFacts facts =
					entity_of(static_cast<uint16_t>(slot.entity_slot));
			if (facts.found) {
				e.team = facts.team;
				e.player_class = facts.player_class;
			}
		}
		out_rows.push_back(std::move(e));
	}
	return header_of(sb);
}

ClientScoreboardHeader scoreboard_header(const ClientState &state) {
	return header_of(state.scoreboard);
}

std::string feed_actor_name(const ClientState &state, uint16_t index,
		const std::string &entity_name) {
	for (const ClientRosterSlot &slot : state.roster) {
		if (!slot.bound || slot.entity_slot < 0 ||
				static_cast<uint16_t>(slot.entity_slot) != index)
			continue;
		// The 256-byte name buffer [orig: Napi_CopyString(.., 256) @0x422e1d;
		// String_AppendN(.., 256) @0x422e40..0x422e64].
		std::string name = slot.name.substr(0, 255);
		if (!slot.registry_clan.empty()) {
			name += "<ch>";
			name += slot.registry_clan;
			name += "<co>";
			if (name.size() > 255) name.resize(255);
		}
		return name;
	}
	return entity_name; // [orig: the entity+0xF4 copy @0x422e6e..0x422e80]
}

} // namespace opennova::replication
