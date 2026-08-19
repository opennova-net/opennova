// The Tab board's two folded lanes.
//
// S2C 0x16 carries the scoreboard itself — a flags byte, the player rows, a
// per-team table and the in-game/spectator trailer
// [orig: server NetPacket_SerializeScoreboard0x16 @0x504B80 /
//  Server_BuildAndBroadcastScoreboard @0x50D960 (every 311 ticks);
//  client NapiNPClientMsg_PlayerList @0x42FAE0].
// S2C 0x46 carries the connection-slot roster the board joins names from
// [orig: NapiNPClientMsg_PlayerSync @0x431370].
//
// Two witnessed retention rules the fold reproduces, both of which a naive
// "latest message wins" would get wrong:
//   * an EMPTY 0x16 never clobbers a populated board — a round-end update (and
//     the next map's first) frequently carries zero rows, and retail's board
//     keeps showing the last populated list;
//   * a 0x46 REMOVAL keeps the slot's name binding — the roster is the board's
//     name-join table, not a liveness set, and retail still names a slot whose
//     entity has gone.
//
// Rows are stored in WIRE ORDER: the server sorts them (team modes by the
// accumulated points, others by the mode stat) and the client never re-sorts
// [orig: Player_ComputeScore @0x500A80]. `slot_id` is authoritative, not the
// row's position.

#include "netsim/client_replica_pipeline.h"

#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_id.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::netsim {

void ClientReplicaPipeline::apply_player_list(const std::vector<uint8_t> &body) {
	PlayerList list;
	if (!decode_player_list(body.data(), body.size(), list)) {
		++malformed_bodies_;
		return;
	}
	// The empty-update guard. A board that has never been populated still
	// accepts an empty list (that is a legitimate "no players yet" state); a
	// populated one holds until real rows arrive.
	if (list.players.empty() && state_.scoreboard.known &&
	    !state_.scoreboard.rows.empty())
		return;

	ClientScoreboard &sb = state_.scoreboard;
	sb.known = true;
	sb.team_mode = (list.flags & 0x01u) != 0u;
	sb.timed = (list.flags & 0x02u) != 0u;
	sb.in_game_count = list.in_game_count;
	sb.spectator_count = list.spectator_count;
	sb.rows.clear();
	sb.rows.reserve(list.players.size());
	for (const PlayerListRow &r : list.players) {
		ClientScoreboardRow row;
		row.slot_id = r.slot_id;
		row.status_flags = r.status_flags;
		row.score1 = r.score1;
		row.score2 = r.score2;
		row.spectator = (r.flags & 0x01u) != 0u;
		row.team = static_cast<uint8_t>(r.flags >> 1);
		sb.rows.push_back(row);
	}
	sb.teams.clear();
	sb.teams.reserve(list.teams.size());
	for (const PlayerListTeamRow &t : list.teams) {
		ClientScoreboardTeam team;
		team.score1 = t.score1;
		team.score2 = t.score2;
		team.player_count = t.player_count;
		team.alive_count = t.alive_count;
		sb.teams.push_back(team);
	}
	++sb.revision;
}

void ClientReplicaPipeline::apply_player_sync(const std::vector<uint8_t> &body) {
	PlayerSync sync;
	if (!decode_player_sync(body.data(), body.size(), sync)) {
		++malformed_bodies_;
		return;
	}
	ClientRosterSlot &slot = state_.roster[sync.slot_id];
	// A removal carries no body: keep the binding (see the file header) and
	// leave the fields exactly as they were.
	if (sync.removal) return;
	slot.bound = true;
	// Per-FIELD last-write-wins: a sync that omits a bit leaves that field
	// alone rather than clearing it.
	if ((sync.field_bitmask & kPlayerSyncHasName) != 0u) slot.name = sync.name;
	if ((sync.field_bitmask & kPlayerSyncHasTeamString) != 0u) slot.clan = sync.clan;
	if ((sync.field_bitmask & kPlayerSyncHasQuality) != 0u) slot.quality = sync.quality;
}

} // namespace opennova::netsim
