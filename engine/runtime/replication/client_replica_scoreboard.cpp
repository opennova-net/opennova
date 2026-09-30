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

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/visible_players.h>
#include <runtime/world/entity.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::replication {

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

// PlayerSlot_SetName: a nonzero account id with a registry node carrying it
// copies the node's tag into the slot's 9-byte buffer (8 characters); no id
// or no node empties it [orig: PlayerSlot_SetName @0x4348f0 —
// CLinkedList_FindByTag(netId) @0x434919, Napi_CopyString(.., node+81,
// table[4] = 9) @0x434935, "" @0x434909]. True when the tag changed.
bool resolve_registry_clan(ClientState &state, ClientRosterSlot &slot) {
	std::string tag;
	if (slot.account_id != 0u) {
		for (const ClientClanRegistryNode &node : state.clan_registry) {
			if (node.net_id != slot.account_id) continue;
			tag = node.tag.substr(0, 8);
			break;
		}
	}
	if (tag == slot.registry_clan) return false;
	slot.registry_clan = std::move(tag);
	return true;
}

// Every registry change re-resolves every live slot [orig:
// PlayerSlotTable_UpdateAllDisplayNames @0x434a00 walks the active list].
void resolve_all_registry_clans(ClientState &state) {
	bool changed = false;
	for (ClientRosterSlot &slot : state.roster)
		if (slot.bound) changed |= resolve_registry_clan(state, slot);
	if (changed) state.mark_changed();
}

} // namespace

// THE S2C 0x6A CLAN REGISTRY FOLD. A non-authority client only: action 2
// removes the node for its netId; actions 1 and 3 find-or-create it and
// overwrite its name (64) and tag (8); action 3 additionally queues the C2S
// 0x4E {netId} walk continuation; every change re-resolves every live slot's
// tag; any other action does nothing [orig: NapiNPClientMsg_HandlePlayerJoinLeave
// @0x432510 — the is_authority return @0x43252f, action 2 sub_52B330
// @0x432578, CLinkedList_FindOrCreateByNetId @0x43262e (Napi_CopyString 65/9
// @0x52b572/@0x52b58d), PlayerSlotTable_UpdateAllDisplayNames @0x43258b /
// @0x43263e, the 0x4E queue @0x43266c].
void ClientReplicaPipeline::apply_clan_roster(const std::vector<uint8_t> &body) {
	if (authority_recipient_) return;
	ClanRosterUpdate update;
	// An unclean body still applies what the reader took (retail has no
	// validity return — a short id reads 0, an unterminated name keeps its
	// partial copy with an empty tag); only an unknown action is a no-op
	// [orig: the action ladder @0x432555..0x43255f, `return` @0x4325a1].
	if (!decode_clan_roster_update(body.data(), body.size(), update)) ++malformed_bodies_;
	if (update.action != kClanRosterAdd && update.action != kClanRosterRemove &&
			update.action != kClanRosterWalkReply)
		return;
	std::vector<ClientClanRegistryNode> &registry = state_.clan_registry;
	auto node = registry.begin();
	while (node != registry.end() && node->net_id != update.account_id) ++node;
	if (update.action == kClanRosterRemove) {
		if (node != registry.end()) registry.erase(node);
	} else {
		if (node == registry.end()) {
			registry.push_back(ClientClanRegistryNode{});
			node = registry.end() - 1;
			node->net_id = update.account_id;
		}
		node->name = update.name.substr(0, kClanRosterNameChars);
		node->tag = update.tag.substr(0, kClanRosterTagChars);
		if (update.action == kClanRosterWalkReply)
			state_.pending_clan_walk_requests.push_back(update.account_id);
	}
	resolve_all_registry_clans(state_);
}

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
	sb.alive_player_count = 0;
	sb.rows.reserve(list.players.size());
	for (const PlayerListRow &r : list.players) {
		ClientRosterSlot &slot = state_.roster[r.slot_id];
		if (!slot.bound) {
			// Retail: PlayerSlotTable_GetActiveSlot null -> the row is skipped
			// and a C2S 0x22 {slot, 0x1CF7} re-request is queued, one per
			// dropped row [orig: @0x42fc05..0x42fc3a]. The DATA rule is the
			// drop; the runtime's send path frames the queued slot ids.
			++sb.rows_dropped_unknown_slot;
			sb.pending_sync_requests.push_back(r.slot_id);
			continue;
		}
		ClientScoreboardRow row;
		row.slot_id = r.slot_id;
		// The status word lands only under the SU gate; clear, the parser
		// zeroes it [orig: `if (!g_ScoreboardStatusSuffixEnabled) ping = 0`
		// @0x42fbf9..0x42fbfb, the store @0x42fdb4].
		row.status_flags = state_.scoreboard_status_suffix != 0 ? r.status_flags : uint16_t{0};
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
		// The registry tag at 7 characters [orig: Napi_CopyString(rec + 32,
		// slot+0x20, 8) @0x42fd85; the empty store @0x42fd8f].
		row.label = slot.registry_clan.substr(0, 7);
		slot.team = row.team;
		apply_team_to_entity(state_, slot, row.team);
		if (state_.permanent_death && !row.spectator && slot.entity_slot >= 0) {
			const auto *entity = state_.find(static_cast<uint16_t>(slot.entity_slot));
			// Missing entity bindings are the safe boundary of the retail null dereference.
			if (entity && (entity->state_flags & 2u) == 0) ++sb.alive_player_count;
		}
		sb.rows.push_back(std::move(row));
	}
	sb.team_count = list.team_count;  // [orig: g_ScoreboardTeamCount @0x42fdda]
	sb.teams.clear();
	sb.teams.reserve(list.teams.size());
	for (const PlayerListTeamRow &t : list.teams) {
		ClientScoreboardTeam team;
		// Both words sign-extend [orig: movsx @0x42fe00 / @0x42fe16].
		team.score1 = static_cast<int16_t>(t.score1);
		team.score2 = static_cast<int16_t>(t.score2);
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
		// A live slot vanishing is presenter state like the rows (the board
		// reads the LIVE slot's quality/binding), so it moves the revision —
		// edge-triggered, like the entity-team write.
		// The mute flags (+50) and the local squad colour (+51) are the two
		// bytes neither the clear nor a later activation touches.
		const bool was_bound = slot.bound;
		const uint8_t mute = slot.radio_mute_flags;
		const uint8_t color = slot.squad_color;
		slot = ClientRosterSlot{};
		slot.radio_mute_flags = mute;
		slot.squad_color = color;
		if (was_bound) state_.mark_changed();
		return;
	}
	bool changed = !slot.bound; // a (re)bind
	if (!slot.bound) {
		// re-bind re-init [orig: @0x4346c0], keeping +50 / +51
		const uint8_t mute = slot.radio_mute_flags;
		const uint8_t color = slot.squad_color;
		slot = ClientRosterSlot{};
		slot.radio_mute_flags = mute;
		slot.squad_color = color;
	}
	slot.bound = true;
	// The entity binding is NOT bitmask-gated: every non-removal sync restamps
	// it [orig: @0x431477/@0x431480; the no-entity -1 form @0x431489].
	const int16_t entity_slot = sync.entity_slot_id == 0xFF
			? int16_t{-1}
			: static_cast<int16_t>(sync.entity_slot_id);
	changed |= slot.entity_slot != entity_slot;
	slot.entity_slot = entity_slot;
	// Named fields land per-FIELD last-write-wins: a sync that omits a bit
	// leaves that field alone rather than clearing it.
	if ((sync.field_bitmask & kPlayerSyncHasName) != 0u) {
		changed |= slot.name != sync.name;
		slot.name = sync.name;
	}
	if ((sync.field_bitmask & kPlayerSyncHasTeamString) != 0u) {
		changed |= slot.clan != sync.clan;
		slot.clan = sync.clan;
	}
	if ((sync.field_bitmask & kPlayerSyncHasTeamByte) != 0u) {
		// The entity write edge-bumps on its own; a team change on a slot with
		// no entity to write through still counts as a roster change.
		const bool team_changed = slot.team != sync.team;
		slot.team = sync.team;
		apply_team_to_entity(state_, slot, sync.team); // [orig: @0x4315fc]
		changed |= team_changed && slot.entity_slot < 0;
	}
	if ((sync.field_bitmask & kPlayerSyncHasDownedState) != 0u) {
		const uint8_t seconds = static_cast<uint8_t>(sync.downed_state & 0x7Fu);
		const bool request = (sync.downed_state & 0x80u) != 0u;
		changed |= slot.downed_revive_seconds != seconds ||
				slot.medic_request_active != request;
		slot.downed_revive_seconds = seconds;
		slot.medic_request_active = request;
	}
	// The command map's squad bytes [orig: 0x20 -> +45, 0x1000 -> +46, 0x40 ->
	// +48, 0x80 -> +49 in NapiNPClientMsg_PlayerSync @0x431370]. No team-list
	// refresh rides these.
	if ((sync.field_bitmask & kPlayerSyncHasVehicleScore) != 0u)
		slot.vehicle_score = sync.field_0020;
	if ((sync.field_bitmask & kPlayerSyncHasLateJoinFlag) != 0u) {
		changed |= slot.spectator != (sync.field_1000 != 0);
		slot.spectator = sync.field_1000 != 0;
	}
	if ((sync.field_bitmask & kPlayerSyncHasSquad) != 0u) {
		changed |= slot.squad_leader != sync.field_0040;
		slot.squad_leader = sync.field_0040;
	}
	if ((sync.field_bitmask & kPlayerSyncHasSide) != 0u) {
		changed |= slot.fireteam != sync.field_0080;
		slot.fireteam = sync.field_0080;
	}
	if ((sync.field_bitmask & kPlayerSyncHasQuality) != 0u) {
		const uint8_t quality = sync.quality > 4u ? uint8_t{4} : sync.quality; // [orig: @0x43170d]
		changed |= slot.quality != quality;
		slot.quality = quality;
	}
	if ((sync.field_bitmask & kPlayerSyncHasAccountId) != 0u) {
		// The account id lands in slot dword 15 and the slot's registry tag
		// re-resolves at once [orig: `mov [ebx+3Ch], esi` @0x431736 ->
		// PlayerSlot_SetName @0x43173d].
		slot.account_id = sync.account_id;
		changed |= resolve_registry_clan(state_, slot);
	}
	// The binding, name, clan and quality are what the board projects from
	// the LIVE slot, so a change to any of them moves ClientState.revision
	// like the entity rows do (edge-triggered; the team byte bumps above).
	if (changed) state_.mark_changed();
}

// S2C 0x4C: the table is freed and rebuilt from the snapshot. An entry names
// a slot the roster has not bound yet: the slot is created then (active, the
// fields zeroed), exactly as a first 0x46 would; the entity the entry names
// is kept for the own-slot test, nulled where retail's resolve fails.
// [orig: NapiNPClientMsg_0x04C @0x428570 — free/zero @0x428580..0x4285a1,
//  PlayerSlotTable_GetActiveSlot @0x42862f else PlayerSlotTable_GetOrInitSlot
//  @0x428643 (the create @0x4346de..0x434719), the handle resolve
//  @0x428679..0x42868c, the pair stores @0x42868e/@0x428695]
void ClientReplicaPipeline::apply_visible_players(const std::vector<uint8_t> &body) {
	VisiblePlayers snapshot;
	bool clean = false;
	decode_visible_players(body.data(), body.size(), snapshot, &clean);
	if (!clean) ++malformed_bodies_;
	state_.visible_players.clear();
	state_.visible_players.reserve(snapshot.entries.size());
	for (const VisiblePlayers::Entry &e : snapshot.entries) {
		ClientRosterSlot &slot = state_.roster[e.slot];
		if (!slot.bound) {
			slot = ClientRosterSlot{};
			slot.bound = true;
		}
		ClientVisiblePlayer entry;
		entry.slot = e.slot;
		const world::EntityHandle h{e.entity_handle};
		entry.entity_handle = (h.valid() && h.pool() < world::kEntityPoolCount)
				? e.entity_handle : world::EntityHandle::kInvalid;
		state_.visible_players.push_back(entry);
	}
	state_.mark_changed();
}

// S2C 0x4D: a player joined. Its slot's downed state clears; for another
// player's slot this client asks for the slot's full 0x46 row and a fresh
// 0x4C snapshot. The own slot's notice raises tip 22 while the death screen
// is up and the round runs — the tip system is unported, so that leg does
// nothing here. [orig: NapiNPClientMsg_HandleSpawnSlot @0x4317B0 —
//  PlayerSlot_SetDownedState(slot, 0, 0) @0x4317e5, the local-slot test
//  @0x4317f0, CTipSystem_HandleEvent(22) + dword_24C18F0 = 186
//  @0x431855..0x431863, the 0x22 {slot, 0x1CF7} @0x43181d and the empty 0x23
//  @0x43183e]
void ClientReplicaPipeline::apply_spawn_slot_notice(const std::vector<uint8_t> &body) {
	SpawnSlotNotice notice;
	decode_spawn_slot_notice(body.data(), body.size(), notice);
	ClientRosterSlot &slot = state_.roster[notice.slot];
	if (slot.bound && (slot.downed_revive_seconds != 0 || slot.medic_request_active)) {
		slot.downed_revive_seconds = 0;
		slot.medic_request_active = false;
		state_.mark_changed();
	}
	if (notice.slot == local_player_slot_) return;
	ClientVisiblePlayersRefresh refresh;
	refresh.slot = notice.slot;
	refresh.fields = kSpawnSlotSyncFields;
	state_.pending_visible_refreshes.push_back(refresh);
}

// Retail tests the entity's Flags 0x100 player-class bit [orig:
// NapiNPClientMsg_TeamAssign @0x431a15]; only players carry it, and every
// player is a pool-0 entity (Entity_SpawnFromAnimSlotProperty @0x43c3e3,
// the bit @0x43c433). A decoded row carries the class; the listen client's
// players are its own world entities with no row, and each is its slot's
// 0x46 entity binding.
bool ClientReplicaPipeline::is_player_entity(uint16_t handle) const {
	const world::EntityHandle h{handle};
	if (!h.valid() || h.pool() != 0) return false;
	if (const ClientEntityState *row = state_.find(handle))
		if (row->cls == EntityClass::Player) return true;
	for (const ClientRosterSlot &slot : state_.roster)
		if (slot.bound && slot.entity_slot == h.slot()) return true;
	return false;
}

} // namespace opennova::replication
