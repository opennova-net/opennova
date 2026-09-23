#include <runtime/inmatch/server_idle_timers.h>

#include <cstring>
#include <vector>

#include <net/npwire/ingame_decode.h>     // PlaySoundCommand
#include <net/npwire/ingame_encode.h>     // encode_play_sound
#include <net/npwire/ingame_message_id.h> // s2c::PLAY_SOUND
#include <runtime/audio/sound_profile.h>  // compose_entity_sound_set (the drowning composites)
#include <runtime/world/geom.h>           // to_fixed
#include <runtime/world/infantry.h>       // the drown death animation

namespace opennova::inmatch {

namespace {

// Server_SendOverlayActionToAlive with the entity's composite for `type`: the
// S2C 0x34 sound at the entity's position to every ALIVE in-match player
// (mask 128); the listen host's own copy rides the local slot-sound route.
// [orig: SoundProfile_FindByEntityAndType @0x528180 ->
//  Server_SendOverlayActionToAlive @0x50A1B0]
void fan_entity_sound_to_alive(NapiNPServerCtx &ctx, world::World &world,
		const world::Entity &source, int type) {
	char set_name[24] = {};
	audio::compose_entity_sound_set(source.anim_slot, type, set_name, sizeof(set_name));
	if (set_name[0] == 0) return;
	PlaySoundCommand cmd;
	cmd.flag = 1;
	cmd.sound_name = set_name;
	cmd.has_pos = true;
	cmd.pos_x = static_cast<int16_t>(world::to_fixed(source.position.x) >> 16);
	cmd.pos_y = static_cast<int16_t>(world::to_fixed(source.position.y) >> 16);
	cmd.pos_z = static_cast<int16_t>(world::to_fixed(source.position.z) >> 16);
	const std::vector<uint8_t> body = encode_play_sound(cmd);
	for (NapiNPConnection &candidate : ctx.np_protocol.connection_list) {
		if (!is_in_match(candidate) || candidate.link.transport == nullptr ||
				!candidate.link.owned_entity.valid())
			continue;
		const world::Entity *listener = world.registry.get(candidate.link.owned_entity);
		if (listener == nullptr || listener->health <= 0) continue; // the mask-128 alive filter
		if (candidate.link.mode == replication::TransportMode::Loopback) {
			world::SoundSlotEvent local;
			local.source_handle = source.handle.packed;
			local.pos[0] = world::to_fixed(source.position.x);
			local.pos[1] = world::to_fixed(source.position.y);
			local.pos[2] = world::to_fixed(source.position.z);
			local.slot = 0;
			std::memcpy(local.set_name, set_name, sizeof(set_name));
			world.out.slot_sounds.push_back(local);
			continue;
		}
		candidate.link.transport->host_send(s2c::PLAY_SOUND, body, /*reliable=*/false);
	}
}

} // namespace

int32_t breath_sample_limit(const world::World &world) {
	return static_cast<int32_t>(
			static_cast<uint32_t>(world.script.wac_values.breathtime) * 4u);
}

void Server_UpdateEntityIdleTimers(NapiNPServerCtx &ctx, world::World &world) {
	// A running pre-round countdown holds every sample. The second test,
	// dword_A87050 @0x50D77F, repeats the frame's own gate on the
	// Server_TickUpdate call (Game_ProcessMainFrame @0x5266AE), so it never
	// holds here. [orig: `cmp g_preround_delay_timer,ebx; jnz` @0x50D773]
	if (world.preround_delay_seconds != 0) return;
	const int32_t limit = breath_sample_limit(world);
	// The surfaced-dive split, 160% of breathtime: `lea edx,[edx+edx*4]; shl
	// edx,5` then the signed magic divide by 100.
	// [orig: Server_UpdateEntityIdleTimers @0x50D892..0x50D8AD]
	const int32_t gasp_after = static_cast<int32_t>(
			static_cast<uint32_t>(world.script.wac_values.breathtime) * 160u) / 100;
	for (NapiNPConnection &conn : ctx.np_protocol.connection_list) {
		// A live slot in state 6 [orig: slot+4 @0x50D7A2; `cmp [esi+20h],6;
		// jnz` @0x50D7B7..0x50D7BB]; a spectator slot never samples
		// [orig: !slot+100567 @0x50D7AB].
		if (!is_in_match(conn) || !conn.link.owned_entity.valid()) continue;
		if (conn.link.spectator) continue;
		world::Entity *player = world.registry.get(conn.link.owned_entity);
		if (player == nullptr) continue;
		// Only the dead flag clears the counter here.
		// [orig: `test byte ptr [ecx+24h],2; jnz` @0x50D7C3..0x50D7C7]
		if (((player->flags | player->engine_flags) & world::kEntityFlagDead) != 0u) {
			conn.link.underwater_breath_samples = 0;
			continue;
		}

		// The eye (entity Z + entity+0x74) against the raw water plane,
		// signed; retail carries no authored-water test at this site.
		// [orig: @0x50D7CD..0x50D7D9 `add eax,[ecx+0Ch]; cmp eax,
		//  Env_WaterHeightFixed; jge`]
		if (world::to_fixed(player->position.z) + player->eye_offset_z >= world.env.water_z) {
			// Surfacing after more than four samples plays the breath (a dive
			// no longer than 160% of the breath value) or the gasp composite.
			// [orig: Server_UpdateEntityIdleTimers @0x50D882..0x50D8CB]
			const int32_t prev = static_cast<int32_t>(conn.link.underwater_breath_samples);
			if (prev > 4 && player->anim_slot != 0) {
				fan_entity_sound_to_alive(ctx, world, *player,
						prev <= gasp_after ? audio::kEntitySoundSurfaceBreath
						                   : audio::kEntitySoundSurfaceGasp);
			}
			conn.link.underwater_breath_samples = 0;
			continue;
		}

		const int32_t samples = static_cast<int32_t>(++conn.link.underwater_breath_samples);
		if (samples <= limit) {
			// Three warnings ahead of the kill, 12 / 36 / 24 samples before it
			// [orig: the compares @0x50D842..0x50D861, the WATER_GAG composite
			//  @0x50D863..0x50D878].
			if ((samples == limit - 12 || samples == limit - 36 || samples == limit - 24) &&
					player->anim_slot != 0)
				fan_entity_sound_to_alive(ctx, world, *player, audio::kEntitySoundWaterGag);
			continue;
		}
		// The drowning: the attacker slot (+0x178) cleared, so the death reports
		// no killer, the drown death selection, Health -1, then the body's own
		// callback raises the ordinary death transaction.
		// [orig: `mov [ecx+178h],ebx` @0x50D800; Entity_ComputeAnimSlotIndex
		//  call @0x50D80A; `mov word ptr [eax+11Eh],0FFFFh` @0x50D819; the
		//  +0x1C8 call @0x50D838]
		player->last_attacker = world::EntityHandle{};
		player->death_anim_state = world::compute_death_anim_state(
				0, 0, world::death_cause::kDrown);
		player->health = -1;
		world::RoundDeath death;
		death.victim = player->handle;
		death.victim_handle = player->handle.packed;
		// The snapshot every RoundDeath producer stamps; the host classifier
		// itself reads the live entity word (see classify_player_death).
		death.event_flags = player->cause_flags & 0xF00u;
		world.round_sim.deaths.push_back(death);
	}
}

} // namespace opennova::inmatch
