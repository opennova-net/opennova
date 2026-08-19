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
// Retention follows the witnessed parser, not a "latest message wins" or a
// "hold the last populated list" heuristic:
//   * every well-formed 0x16 applies UNCONDITIONALLY — retail zeroes its row
//     count before the row loop and parses the team table and trailer even
//     for a zero-row list [orig: @0x42fb46], so an empty update yields an
//     empty board;
//   * a row whose connection slot has no roster binding is DROPPED — retail
//     skips inactive slots and queues a C2S 0x22 {slot, 0x1CF7} re-request
//     [orig: @0x42fc05..0x42fc3a; the retry send is a D-HUD-24 residual];
//   * name/clan are joined INTO the row at apply time, exactly where retail
//     copies them into its 56-byte records [orig: @0x42fd4c..0x42fd8f] — a
//     later roster removal does not blank rows already on the board;
//   * a 0x46 removal deactivates and wipes the slot [orig:
//     PlayerSlot_ClearAndUnlink @0x434730; re-bind re-init @0x4346c0].
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

namespace {

// Both 0x16 rows and the 0x46 team byte write the live entity team through
// the slot's entity binding [orig: @0x42fc88 / @0x4315fc]. Entity rows are
// ClientState.revision-covered presenter state, so an actual change bumps it —
// the same on-change form as the 0x50 sibling (apply_team_assign).
void apply_team_to_entity(ClientState &state, const ClientRosterSlot &slot,
		uint8_t team) {
	if (slot.entity_slot < 0) return;
	const uint16_t handle = static_cast<uint16_t>(slot.entity_slot); // pool 0
	for (ClientEntityState &es : state.entities) {
		if (es.handle != handle) continue;
		if (es.team == team && es.team_known) return;
		es.team = team;
		es.team_known = true;
		state.mark_changed();
		return;
	}
}

} // namespace

void ClientReplicaPipeline::apply_player_list(const std::vector<uint8_t> &body) {
	PlayerList list;
	if (!decode_player_list(body.data(), body.size(), list)) {
		++malformed_bodies_;
		return;
	}
	ClientScoreboard &sb = state_.scoreboard;
	sb.known = true;
	sb.team_mode = (list.flags & 0x01u) != 0u;
	sb.timed = (list.flags & 0x02u) != 0u;
	sb.in_game_count = list.in_game_count;
	sb.spectator_count = list.spectator_count;
	sb.rows.clear();
	sb.rows.reserve(list.players.size());
	for (const PlayerListRow &r : list.players) {
		ClientRosterSlot &slot = state_.roster[r.slot_id];
		if (!slot.bound) {
			// Retail: PlayerSlotTable_GetActiveSlot null -> the row is skipped
			// and a C2S 0x22 {slot, 0x1CF7} re-request is queued [orig:
			// @0x42fc05..0x42fc3a]. The retry is the embedder's send-path
			// residual; the DATA rule is the drop.
			++sb.rows_dropped_unknown_slot;
			continue;
		}
		ClientScoreboardRow row;
		row.slot_id = r.slot_id;
		row.status_flags = r.status_flags;
		// The one sign-extension site — the wire u16 lands signed in the
		// record, retail's movsx [orig: @0x42fb9d].
		row.score1 = static_cast<int16_t>(r.score1);
		row.score2 = r.score2;
		row.spectator = (r.flags & 0x01u) != 0u;
		row.team = static_cast<uint8_t>(r.flags >> 1);
		// The parse-time joins and side-writes of the accepted row: name/clan
		// into the record [orig: @0x42fd4c..0x42fd8f], the row team onto the
		// slot and its live entity [orig: @0x42fc7c/@0x42fc88].
		row.name = slot.name;
		row.clan = slot.clan;
		slot.team = row.team;
		apply_team_to_entity(state_, slot, row.team);
		sb.rows.push_back(std::move(row));
	}
	sb.teams.clear();
	sb.teams.reserve(list.teams.size());
	for (const PlayerListTeamRow &t : list.teams) {
		ClientScoreboardTeam team;
		team.score1 = t.score1;
		team.score2 = t.score2;
		team.koth_hold = t.koth_hold;
		team.ctf_flag = t.ctf_flag;
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
	if (sync.removal) {
		// PlayerSlot_ClearAndUnlink deactivates the slot and wipes its
		// bookkeeping [orig: @0x434730]. It happens to skip the clan/quality
		// bytes, but an unbound slot is never read (0x16 rows for it drop)
		// and a re-bind re-inits every field [orig: @0x4346c0], so the reset
		// covers both witnessed states with one form.
		slot = ClientRosterSlot{};
		return;
	}
	if (!slot.bound) slot = ClientRosterSlot{}; // re-bind re-init [orig: @0x4346c0]
	slot.bound = true;
	// The entity binding is NOT bitmask-gated: every non-removal sync restamps
	// it [orig: @0x431477/@0x431480; the no-entity -1 form @0x431489].
	slot.entity_slot = sync.entity_slot_id == 0xFF
			? int16_t{-1}
			: static_cast<int16_t>(sync.entity_slot_id);
	// Named fields land per-FIELD last-write-wins: a sync that omits a bit
	// leaves that field alone rather than clearing it.
	if ((sync.field_bitmask & kPlayerSyncHasName) != 0u) slot.name = sync.name;
	if ((sync.field_bitmask & kPlayerSyncHasTeamString) != 0u) slot.clan = sync.clan;
	if ((sync.field_bitmask & kPlayerSyncHasTeamByte) != 0u) {
		slot.team = sync.team;
		apply_team_to_entity(state_, slot, sync.team); // [orig: @0x4315fc]
	}
	if ((sync.field_bitmask & kPlayerSyncHasQuality) != 0u)
		slot.quality = sync.quality > 4u ? uint8_t{4} : sync.quality; // [orig: @0x43170d]
}

} // namespace opennova::netsim
