// The command map's squad and waypoint S2C folds: the roster's squad bytes,
// the HUD order lines, and the consequences (the lines, the sounds, the pool-4
// rows) the embedding role applies (ClientSquadEvent).
// Witness record: docs/interface/hud-re.md "The command map" (D-HUD-19).

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/squad_messages.h>

namespace opennova::replication {

namespace {

// PlayerSlotTable_GetActiveSlot: the slot when it is active (+13), else null.
// [orig: PlayerSlotTable_GetActiveSlot @0x434780]
ClientRosterSlot *active_slot(ClientState &state, uint8_t index) {
	ClientRosterSlot &slot = state.roster[index];
	return slot.bound ? &slot : nullptr;
}

} // namespace

int ClientReplicaPipeline::local_roster_slot() const {
	// g_LocalPlayerEntity+0x154, the local player's own slot index. The host
	// stamps a player entity's +0x154 from its slot's +20 id, the id S2C 0x04
	// byte 17 carries as g_LocalPlayerSlotId, so both name one slot: the one
	// set_local_player_slot latches (a joiner's 0x04, the listen host's own
	// connection), live while the player is dead or spectating.
	// [orig: Server_PlayerAdd @0x51d087..0x51d08b; Server_InitAllPlayerEntitiesForRound
	//  @0x516b97..0x516b9a; NapiNPClientMsg_SessionSlotConfig @0x4254a2]
	return local_player_slot_;
}

void ClientReplicaPipeline::apply_squad_join(const std::vector<uint8_t> &body) {
	// [orig: NapiNPClientMsg_HandleSquadJoin @0x425600 — the link + fireteam
	//  reset @0x42564a..0x425652; `leader == local slot` @0x42565e (the line
	//  unless +50 & 2 @0x42566c, the RECRUIT_ACCEPT sound unless +50 & 1
	//  @0x4256a0, the waypoint push j_Server_BroadcastChatToAllPlayers
	//  @0x4256d1); `leader == 0xFF` @0x4256db -> the foreign-waypoint sweep
	//  @0x4256e0 whichever member the message names (no local-slot test);
	//  the team-list refresh @0x4256e5]
	const SquadJoin join = decode_squad_join(body.data(), body.size());
	ClientRosterSlot *slot = active_slot(state_, join.member);
	if (slot == nullptr) return;
	slot->squad_leader = join.leader;
	slot->fireteam = 0;
	if (join.leader == local_player_slot_) {
		ClientSquadEvent event;
		event.kind = ClientSquadEvent::Kind::MemberJoined;
		event.slot = join.member;
		event.entity_slot = slot->entity_slot;
		event.name = slot->name;
		event.mute = slot->radio_mute_flags;
		event.feed_order = next_feed_order_++;
		pending_effect_commands_.push_back(std::move(event));
	} else if (join.leader == 0xFFu) {
		ClientSquadEvent event;
		event.kind = ClientSquadEvent::Kind::SquadLeft;
		event.slot = join.member;
		event.feed_order = next_feed_order_++;
		pending_effect_commands_.push_back(std::move(event));
	}
	++state_.squad_revision;
	state_.mark_changed();
}

void ClientReplicaPipeline::apply_squad_order(const std::vector<uint8_t> &body) {
	// [orig: NapiNPClientMsg_0x072 @0x425710 — a non-empty line plays
	//  MP_COMMAND1 @0x425749; Team_SetNameByIndex @0x59c2d0 keeps kind < 2
	//  (strncpy 127)]
	const SquadOrder order = decode_squad_order(body.data(), body.size());
	if (!order.text.empty()) {
		ClientSquadEvent event;
		event.kind = ClientSquadEvent::Kind::OrderSound;
		event.feed_order = next_feed_order_++;
		pending_effect_commands_.push_back(std::move(event));
	}
	if (order.kind < 2) {
		state_.squad_orders[order.kind] = order.text.substr(0, 127);
		state_.mark_changed();
	}
}

void ClientReplicaPipeline::apply_fireteam_set(const std::vector<uint8_t> &body) {
	// [orig: NapiNPClientMsg_0x073 @0x425770 — the store @0x4257c0; on the
	//  local slot the LEADER's +50: bit 2 skips everything @0x4257ec, bit 1
	//  skips MP_COMMAND1 @0x4257f4, else the HUD_CMAP_SETFIRETEAM line; the
	//  team-list refresh @0x425892]
	const FireteamSet set = decode_fireteam_set(body.data(), body.size());
	ClientRosterSlot *slot = active_slot(state_, set.member);
	if (slot == nullptr) return;
	slot->fireteam = set.fireteam;
	if (set.member == local_player_slot_) {
		const ClientRosterSlot *leader = active_slot(state_, slot->squad_leader);
		if (leader != nullptr && (leader->radio_mute_flags & 2u) == 0u) {
			ClientSquadEvent event;
			event.kind = ClientSquadEvent::Kind::FireteamSet;
			event.slot = set.member;
			event.value = set.fireteam;
			event.mute = leader->radio_mute_flags;
			event.feed_order = next_feed_order_++;
		pending_effect_commands_.push_back(std::move(event));
		}
	}
	++state_.squad_revision;
	state_.mark_changed();
}

void ClientReplicaPipeline::apply_squad_recruited(const std::vector<uint8_t> &body) {
	// [orig: NapiNPClientMsg_PlayerRecruited @0x4258b0 — the RECRUIT sound
	//  unless +50 & 1 @0x4258d9, the HUD_CMAP_RECRUIT line unless +50 & 2
	//  @0x425903]
	const uint8_t recruiter = decode_squad_recruited(body.data(), body.size());
	const ClientRosterSlot *slot = active_slot(state_, recruiter);
	if (slot == nullptr) return;
	ClientSquadEvent event;
	event.kind = ClientSquadEvent::Kind::Recruited;
	event.slot = recruiter;
	event.entity_slot = slot->entity_slot;
	event.name = slot->name;
	event.mute = slot->radio_mute_flags;
	event.feed_order = next_feed_order_++;
		pending_effect_commands_.push_back(std::move(event));
}

void ClientReplicaPipeline::apply_go_code(const std::vector<uint8_t> &body) {
	// [orig: NapiNPClientMsg_0x078 @0x425970 -> Server_PlayGoCodeSoundAndChat
	//  (slot entity, code, slot +50) @0x5522e0]
	const GoCode code = decode_go_code(body.data(), body.size());
	const ClientRosterSlot *slot = active_slot(state_, code.leader);
	if (slot == nullptr) return;
	ClientSquadEvent event;
	event.kind = ClientSquadEvent::Kind::GoCode;
	event.slot = code.leader;
	event.entity_slot = slot->entity_slot;
	event.value = code.code;
	event.mute = slot->radio_mute_flags;
	event.feed_order = next_feed_order_++;
		pending_effect_commands_.push_back(std::move(event));
}

void ClientReplicaPipeline::apply_waypoint_create(const std::vector<uint8_t> &body) {
	// [orig: NapiNPClientMsg_0x033 @0x425fa0 -> Waypoint_CreateForPlayer(x, y,
	//  name, Pool_GetEntryUnchecked(0, owner))]
	const WaypointCreate create = decode_waypoint_create(body.data(), body.size());
	ClientSquadEvent event;
	event.kind = ClientSquadEvent::Kind::WaypointCreate;
	event.name = create.name;
	event.x = create.x;
	event.y = create.y;
	event.handle = create.owner_index;
	event.feed_order = next_feed_order_++;
		pending_effect_commands_.push_back(std::move(event));
}

void ClientReplicaPipeline::apply_destroy_entity(const std::vector<uint8_t> &body) {
	// [orig: NapiNPClientMsg_0x07C @0x426020 — 0xFFFF returns; a short body
	//  reads 0; the pool < 5 / slot < capacity resolve then Entity_Destroy]
	ClientSquadEvent event;
	event.kind = ClientSquadEvent::Kind::EntityDestroy;
	event.handle = decode_entity_handle16(body.data(), body.size());
	if (event.handle == 0xFFFFu) return;
	event.feed_order = next_feed_order_++;
		pending_effect_commands_.push_back(std::move(event));
}

} // namespace opennova::replication
