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
// Not projected here: score2 (no retail reader), the teams[] table and the
// session facts (the role feed inmatch::scoreboard_feed carries them to the
// header block), and rows_dropped_unknown_slot (a diagnostic).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
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

// The drawer's reads of a row's LIVE entity — its team byte and playerClass
// [orig: player_entity->Team @0x423d21, ->playerClass @0x423d8a] — which live
// on the role's own entity model (the authority's pools, a joiner's decoded
// rows), so the embedder's role feed answers them by pool-0 handle.
struct ScoreboardEntityFacts {
	bool found = false;
	uint8_t team = 0;
	uint8_t player_class = 0;
};
using ScoreboardEntityLookup = std::function<ScoreboardEntityFacts(uint16_t handle)>;

// Fills `out_rows` with the drawn projection (wire order — the server sorts,
// the client never re-sorts) and returns the header counts. Without a lookup
// a row keeps its 0x16 team and no class.
ClientScoreboardHeader project_scoreboard(
		const ClientState &state, std::vector<hud::ScoreboardEntry> &out_rows,
		const ScoreboardEntityLookup &entity_of = {});

// The header counts alone (no row materialization).
ClientScoreboardHeader scoreboard_header(const ClientState &state);

// The kill feed's actor name for a pool-0 index: the live connection slot
// driving that entity (PlayerSlot_FindByEntityPtr) supplies its 0x46 name
// (slot+0x14) followed by "<ch>" + its registry clan tag (slot+0x20) + "<co>"
// when the tag is non-empty; an entity no slot drives falls back to its own
// name (entity+0xF4, `entity_name`) [orig: HUD_FormatKillEventMessage
// @0x422DA0 — the slot find @0x422e01/@0x422eba, Napi_CopyString(name, 256)
// @0x422e1d, the tag test @0x422e2c and String_AppendN "<ch>" / tag / "<co>"
// @0x422e40..0x422e64; the entity-name copy @0x422e6e].
std::string feed_actor_name(const ClientState &state, uint16_t index,
		const std::string &entity_name);

// (The game-type label rung lives with the rest of the game-type key maps:
// npwire game_type.h overlay_label_key [orig: @0x5b8680].)


// The Tab-board header as the shell needs it: the projection's counts plus
// the session game type and names (joiner-decoded, empty on a host until the
// host sessionvars are plumbed, D-HUD-24). One value the embedder fills; its
// Godot record wraps it by value (ADR 0043 d10).
struct ClientScoreboardSession {
	ClientScoreboardHeader header;
	uint32_t game_type = 0;
	std::string server_name;
	std::string mission_name;
};

} // namespace opennova::replication
