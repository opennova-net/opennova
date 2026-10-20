#include <runtime/inmatch/server_idle_timers.h>

#include <cstring>
#include <vector>

#include <net/npwire/ingame_decode.h>     // PlaySoundCommand
#include <net/npwire/ingame_encode.h>     // encode_play_sound
#include <net/npwire/ingame_message_id.h> // s2c::PLAY_SOUND
#include <runtime/audio/oneshot_play.h>   // SoundSetIndex (the loaded banks)
#include <runtime/audio/sound_profile.h>  // compose_entity_sound_set (the drowning composites)
#include <runtime/world/geom.h>           // to_fixed
#include <runtime/world/infantry.h>       // the drown death animation

namespace opennova::inmatch {

namespace {

// Server_SendOverlayActionToAlive with the entity's composite for `type`: the
// S2C 0x34 sound at the entity's position to every slot in state 6/7 (mask
// 128, which reads no entity or health, so a dead player hears it too); the
// listen host's own copy rides the local slot-sound route. A composite no
// loaded bank holds is a null set, which sends nothing.
// [orig: SoundProfile_FindByEntityAndType @0x528180 (SoundBank_FindSetByNameAnyBank
//  @0x5281D6, 0 on a miss) -> Server_SendOverlayActionToAlive @0x50A1B0 (the
//  null test @0x50A1B4..0x50A1B6, mask 0x80 @0x50A1D3);
//  NapiNPServer_SendFiltered @0x4C893E..0x4C8953]
void fan_entity_sound_in_match(NapiNPServerCtx &ctx, world::World &world,
		const world::Entity &source, int type) {
	char set_name[24] = {};
	audio::compose_entity_sound_set(source.anim_slot, type, set_name, sizeof(set_name));
	if (set_name[0] == 0) return;
	if (world.tables.sound_sets == nullptr || !world.tables.sound_sets->has(set_name)) return;
	PlaySoundCommand cmd;
	cmd.flag = 1;
	cmd.sound_name = set_name;
	cmd.has_pos = true;
	cmd.pos_x = static_cast<int16_t>(source.position.x.q16 >> 16);
	cmd.pos_y = static_cast<int16_t>(source.position.y.q16 >> 16);
	cmd.pos_z = static_cast<int16_t>(source.position.z.q16 >> 16);
	const std::vector<uint8_t> body = encode_play_sound(cmd);
	for (NapiNPConnection &candidate : ctx.np_protocol.connection_list) {
		if (!active_player_recipient(candidate)) continue;
		if (candidate.link.mode == replication::TransportMode::Loopback) {
			world::SoundSlotEvent local;
			local.source_handle = source.handle.packed;
			local.pos[0] = source.position.x.q16;
			local.pos[1] = source.position.y.q16;
			local.pos[2] = source.position.z.q16;
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

void Server_UpdatePlayerBreathTimers(NapiNPServerCtx &ctx, world::World &world) {
	// A running pre-round countdown holds every sample. The second test,
	// dword_A87050 @0x50D77F, repeats the frame's own gate on the
	// Server_TickUpdate call (Game_ProcessMainFrame @0x5266AE), so it never
	// holds here. [orig: `cmp g_PreRoundDelayTimer,ebx; jnz` @0x50D773]
	if (world.preround_delay_seconds != 0) return;
	const int32_t limit = breath_sample_limit(world);
	// The surfaced-dive split, 160% of breathtime: `lea edx,[edx+edx*4]; shl
	// edx,5` then the signed magic divide by 100.
	// [orig: Server_UpdatePlayerBreathTimers @0x50D892..0x50D8AD]
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
		//  g_EnvWaterHeightFixed; jge`]
		if (player->position.z.q16 + player->eye_offset_z >= world.env.water_z) {
			// Surfacing after more than four samples plays the breath (a dive
			// no longer than 160% of the breath value) or the gasp composite,
			// for a body with an ItemDef (+0x20).
			// [orig: Server_UpdatePlayerBreathTimers @0x50D882..0x50D8CB, the
			//  ItemDef test `cmp [ecx+20h], ebx` @0x50D88D..0x50D890]
			const int32_t prev = static_cast<int32_t>(conn.link.underwater_breath_samples);
			if (prev > 4 && player->has_item_def) {
				fan_entity_sound_in_match(ctx, world, *player,
						prev <= gasp_after ? audio::kEntitySoundSurfaceBreath
						                   : audio::kEntitySoundSurfaceGasp);
			}
			conn.link.underwater_breath_samples = 0;
			continue;
		}

		const int32_t samples = static_cast<int32_t>(++conn.link.underwater_breath_samples);
		if (samples <= limit) {
			// Three warnings ahead of the kill, 12 / 36 / 24 samples before it,
			// for a body with an ItemDef (+0x20) [orig: the compares
			// @0x50D842..0x50D861, the ItemDef test @0x50D863..0x50D866, the
			// WATER_GAG composite @0x50D868..0x50D878].
			if ((samples == limit - 12 || samples == limit - 36 || samples == limit - 24) &&
					player->has_item_def)
				fan_entity_sound_in_match(ctx, world, *player, audio::kEntitySoundWaterGag);
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
