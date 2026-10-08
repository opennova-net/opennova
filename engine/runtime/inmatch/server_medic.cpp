#include <runtime/inmatch/server_medic.h>

#include <net/npwire/ingame_decode.h>   // PlayerDownedState
#include <net/npwire/ingame_encode.h>   // encode_player_downed_state
#include <net/npwire/ingame_message_id.h>
#include <runtime/inmatch/server_message_dispatch.h> // is_medic_recipient
#include <runtime/inmatch/server_tick.h>             // Server_RerollPlayerTickSeed
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

// The revive reads healer+660 == 5 (@0x517D07), the heal the slot's copy,
// slot+89820 == 5 (@0x50DE58); every class writer stores both
// [orig: NapiNPServerMsg_HandlePlayerLoadout @0x515AA8/@0x515AB0].
constexpr uint8_t kMedicClass = 5;
constexpr uint8_t kReviveEvent = 38;          // GameEvent_BuildPayload(0x26, ...)
constexpr uint8_t kHealEvent = 45;            // GameEvent_BuildPayload(0x2D, ...) @0x50DED8
constexpr int32_t kRevivePoseRaise = 0x4000;  // +0.25 world units @0x517E09

NapiNPConnection *connection_for(NapiNPServerCtx &ctx, world::EntityHandle entity) {
	for (NapiNPConnection &c : ctx.np_protocol.connection_list)
		if (c.link.owned_entity.valid() && c.link.owned_entity == entity) return &c;
	return nullptr;
}

uint8_t pool0_index(world::EntityHandle h) {
	if (!h.valid() || h.pool() != 0 || h.slot() > 0xFE) return 0xFF;
	return static_cast<uint8_t>(h.slot());
}

// S2C 0x1E [event][victim idx][healer idx][0xFF][x hi][y hi], mask 128: every
// active player [orig: GameEvent_BuildPayload @0x5054E0 from the revive
// @0x517E63..0x517EB6 and the heal @0x50DEAC..0x50DF02].
void send_medic_event(NapiNPServerCtx &ctx, uint8_t event, world::EntityHandle victim_handle,
		world::EntityHandle healer_handle, const world::Entity &victim) {
	const int32_t x = world::to_fixed(victim.position.x);
	const int32_t y = world::to_fixed(victim.position.y);
	const uint16_t x_hi = static_cast<uint16_t>(static_cast<uint32_t>(x) >> 16);
	const uint16_t y_hi = static_cast<uint16_t>(static_cast<uint32_t>(y) >> 16);
	const std::vector<uint8_t> body = {
			event, pool0_index(victim_handle), pool0_index(healer_handle), 0xFF,
			static_cast<uint8_t>(x_hi & 0xFF), static_cast<uint8_t>(x_hi >> 8),
			static_cast<uint8_t>(y_hi & 0xFF), static_cast<uint8_t>(y_hi >> 8)};
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (!is_in_match(c) || c.link.transport == nullptr) continue;
		c.link.transport->host_send(s2c::GAME_EVENT, body);
	}
}

void revive_player(NapiNPServerCtx &ctx, world::World &world,
		const world::MedicInteraction &r) {
	world::Entity *victim = world.registry.get(r.victim);
	world::Entity *healer = world.registry.get(r.healer);
	if (victim == nullptr || healer == nullptr) return;
	// Both need a player slot: an AI or other slotless healer never revives
	// [orig: GameEvent_RevivePlayer @0x517CD0 — Entity_ValidatePtr(victim)
	//  @0x517CD9..0x517CE7, Entity_ValidatePtr(healer) @0x517CF3..0x517D01].
	NapiNPConnection *victim_connection = connection_for(ctx, r.victim);
	NapiNPConnection *healer_connection = connection_for(ctx, r.healer);
	if (victim_connection == nullptr || healer_connection == nullptr) return;
	if (healer->player_class != kMedicClass) return;        // @0x517D07
	// The victim's revive window is open (slot+368: 120 at the death, 0 for a
	// death no medic may revive or a window that ran out) @0x517D14..0x517D1A.
	if (victim_connection->link.downed_revive_seconds == 0u) return;
	// The auto-medic opt-out: a victim who turned automedic off needs a live
	// medic request (slot+372 / +89856) @0x517D20..0x517D2E.
	if (!victim_connection->link.auto_medic_enabled &&
			!victim_connection->link.medic_request_active)
		return;
	// The last attacker is neither the healer nor the victim: a medic never
	// revives a teammate he killed, nobody revives a suicide @0x517D34..0x517D44.
	if (victim->last_attacker == r.healer || victim->last_attacker == r.victim) return;

	victim->medic_reviving = true;                            // +0x1E0 = 1 @0x517D4F
	// The victim slot: the revive pose armed, the revive window and the
	// respawn penalty closed [orig: +89932 = 1 @0x517D5B, +368 = 0 @0x517D61,
	// +360 = 0 @0x517D67].
	victim_connection->reply.revive_pose_valid = true;
	victim_connection->link.downed_revive_seconds = 0;
	victim_connection->link.respawn_delay_seconds = 0;
	world.zones.spawn_waves.remove_player(r.victim);          // @0x517D6D

	// 0x54 [handle][0], mask 0x580 = the Medic set of the victim's team.
	{
		PlayerDownedState state;
		state.entity_handle = r.victim.packed;
		state.revive_seconds = 0;
		state.medic_request_active = false;
		const std::vector<uint8_t> body = encode_player_downed_state(state);
		for (NapiNPConnection &candidate : ctx.np_protocol.connection_list) {
			if (!is_medic_recipient(candidate, world, victim->team)) continue;
			candidate.link.transport->host_send(s2c::PLAYER_DOWNED_STATE, body);
		}
	}
	// The medic's MEDICSAVE [orig: GameEvent_RevivePlayer @0x517CD0 (the
	// GameEvent_ProcessScoring(g_GameType, healer, 6, 0, 0) call @0x517DC5)].
	world.match.record_revive(world, r.healer);

	// The revive position the following deploy consumes. Retail stores the
	// victim's yaw/pitch/roll beside it (@0x517DF1..0x517E03), but no code reads
	// them: the deploy restores the position alone (D-NET-377).
	// [orig: @0x517DCD..0x517E09; the reader Server_PositionPlayerForSpawn @0x50D60A]
	{
		SessionReplyState &st = victim_connection->reply;
		st.revive_pos[0] = world::to_fixed(victim->position.x);
		st.revive_pos[1] = world::to_fixed(victim->position.y);
		st.revive_pos[2] = world::to_fixed(victim->position.z) + kRevivePoseRaise;
	}
	// The window again and the medic request cleared
	// [orig: +368 = 0 @0x517E14, byte +89856 = 0 @0x517E1A].
	victim_connection->link.downed_revive_seconds = 0;
	victim_connection->link.medic_request_active = false;
	// 0x3A (empty, mask 160 -> the victim's active slot, class 1 reliable)
	// then the seed reroll [orig: @0x517E29..0x517E47].
	if (is_in_match(*victim_connection) && victim_connection->link.transport != nullptr) {
		victim_connection->link.transport->host_send(s2c::MEDIC_REVIVING, {});
		victim_connection->link.transport->host_send(
				s2c::TICK_SEED, Server_RerollPlayerTickSeed(*victim_connection));
	}
	// 0x1E event 38 only when the healer slot's hide bytes +97536/+97537 are
	// clear (+97536 tracks the spectator latch +100567, +97537 is never set)
	// [orig: @0x517E4C..0x517E61, the byte tests @0x517E53 / @0x517E5B; the
	//  heal's twin @0x50DE86..0x50DE96; the send @0x517E63..0x517EB6].
	if (healer_connection->link.spectator) return;
	send_medic_event(ctx, kReviveEvent, r.victim, r.healer, *victim);
}

void heal_player(NapiNPServerCtx &ctx, world::World &world,
		const world::MedicInteraction &h) {
	world::Entity *victim = world.registry.get(h.victim);
	if (victim == nullptr || !victim->has_item_def) return;          // @0x50DE35
	const world::Entity *healer = world.registry.get(h.healer);
	NapiNPConnection *healer_connection = connection_for(ctx, h.healer); // @0x50DE46
	if (healer == nullptr || healer_connection == nullptr) return;   // @0x50DE52
	if (healer->player_class != kMedicClass) return;                 // @0x50DE58
	if (connection_for(ctx, h.victim) == nullptr) return;            // @0x50DE66..0x50DE70
	// The max is the def hp: a session is the only place two player slots
	// exist [orig: GameEvent_HealPlayer @0x50DE77..0x50DE7F;
	// Entity_GetMaxHealthWithDifficulty @0x43B8AD].
	victim->health = world::retail_signed_i16(victim->health_max);
	if (healer_connection->link.spectator) return;                   // @0x50DE86..0x50DE96
	world.match.record_heal(world, h.healer, h.victim);              // @0x50DEA4
	send_medic_event(ctx, kHealEvent, h.victim, h.healer, *victim);  // @0x50DEAC..0x50DF02
}

} // namespace

void Server_RouteMedicInteractions(NapiNPServerCtx &ctx, world::World &world) {
	if (world.round_sim.medic_interactions.empty()) return;
	for (const world::MedicInteraction &interaction : world.round_sim.medic_interactions) {
		if (interaction.revive)
			revive_player(ctx, world, interaction);
		else
			heal_player(ctx, world, interaction);
	}
	world.round_sim.medic_interactions.clear();
}

} // namespace opennova::inmatch
