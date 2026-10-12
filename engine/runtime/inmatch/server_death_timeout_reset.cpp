// The host side of the Respawn key's request -- see server_death_timeout_reset.h.

#include <runtime/inmatch/server_death_timeout_reset.h>

#include <runtime/world/entity.h>
#include <runtime/world/round_sim.h> // RoundDeath
#include <runtime/world/world.h>

namespace opennova::inmatch {

void Server_HandleDeathTimeoutReset(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world) {
	if (ctx.is_authority == 0) return; // @0x510542
	// The sender's player slot and its entity [orig: @0x51054E..0x510566].
	if (conn.phase < ConnectionPhase::PlayerAdded) return;
	world::Entity *entity = world.registry.get(conn.link.owned_entity);
	if (entity == nullptr) return;
	// The edge's gates: an unhidden body (Flags bit 0) whose dead bit is
	// clear. A body already at Health <= 0 had its transaction raised by the
	// damage that dropped it.
	const uint32_t flags = entity->flags | entity->engine_flags;
	const bool raises_death = entity->alive && entity->health > 0 &&
			(flags & world::kEntityFlagCarried) == 0u && (flags & world::kEntityFlagDead) == 0u;
	entity->health = 0;                                               // +0x11E @0x510568
	entity->last_attacker = world::EntityHandle{};                    // +0x178 @0x51056F
	conn.link.respawn_delay_seconds = ctx.config.respawn_timeout;     // +360 @0x51057B
	conn.link.spawn_target_hold_seconds = ctx.config.respawn_timeout; // +364 @0x510587
	conn.link.downed_revive_seconds = 0;                              // +368 @0x51058D
	if (!raises_death) return;
	world::RoundDeath death;
	death.victim = entity->handle;
	death.victim_handle = entity->handle.packed;
	death.event_flags = entity->cause_flags & 0xF00u;
	// The edge runs in the entity pass, after the rest of this receive pump:
	// the tick tests it at its head (a team change that killed the body first
	// drops the record) and routes it after the pump's spectator converts
	// unless the body is hidden or dead by then.
	death.player_edge = true;
	world.round_sim.deaths.push_back(death);
}

} // namespace opennova::inmatch
