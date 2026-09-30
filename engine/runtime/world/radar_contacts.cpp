// The local player's radar contact state and its producers — see
// radar_contacts.h.

#include <runtime/world/radar_contacts.h>

#include <runtime/audio/oneshot_play.h>
#include <runtime/hud/hud_minimap.h>
#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/geom.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_view.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

// The row identity (+0x00). Retail stores the producing entity's pointer and
// nothing reads it back but the missile list's own search; entity and round
// sources are tagged here so the two pools never alias.
constexpr uint32_t kRadarSourceEntity = 0x10000u;
constexpr uint32_t kRadarSourceRound = 0x20000u;
constexpr uint32_t kRadarSourceWire = 0x40000u; // a joiner's wire proxy
constexpr uint32_t kRadarSourceIndexMask = 0xFFFFu;

int32_t wrap_sub(int32_t a, int32_t b) {
	return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

// `cdq; xor eax, edx; sub eax, edx` — INT32_MIN stays negative.
int32_t abs32(int32_t v) {
	const uint32_t sign = static_cast<uint32_t>(v >> 31);
	return static_cast<int32_t>((static_cast<uint32_t>(v) ^ sign) - sign);
}

uint32_t entity_source_id(const Entity &entity) {
	return kRadarSourceEntity | (static_cast<uint32_t>(entity.handle.packed) & kRadarSourceIndexMask);
}

// An entity's Position in 16.16: the brain's carrier when it has one, else the
// float mirror.
void entity_position_q16(const World &world, const Entity &entity, int32_t out[3]) {
	if (const AiEntity *body = world.ai.for_handle(entity.handle)) {
		out[0] = body->pos[0];
		out[1] = body->pos[1];
		out[2] = body->pos[2];
		return;
	}
	out[0] = to_fixed(entity.position.x);
	out[1] = to_fixed(entity.position.y);
	out[2] = to_fixed(entity.position.z);
}

} // namespace

void radar_add_blip(RadarContactState &state, uint32_t rules, const int32_t local_pos[3],
		uint32_t local_yaw, uint32_t source, const int32_t pos[3], int32_t kind) {
	if ((rules & kRadarRulesNoTracers) != 0) return; // [orig: @0x59b283..0x59b28a]
	// The add's bearing picks one of four compass-edge timers, the quadrant
	// biased by 0x1FFFFFE0 [orig: @0x59b290..0x59b2e3].
	const uint32_t bearing = hud::radar_bearing_raw(wrap_sub(pos[0], local_pos[0]),
			wrap_sub(local_pos[1], pos[1]), hud::kRadarBearingScale);
	state.edge[(bearing - local_yaw - 0x1FFFFFE0u) >> 30] = kRadarEdgeTimer;
	// The first free row takes the add; a full table drops it
	// [orig: @0x59b2ed..0x59b306, the row stores @0x59b30e..0x59b32e].
	for (RadarContact &row : state.rows) {
		if (row.life != 0) continue;
		row.pos[0] = pos[0];
		row.pos[1] = pos[1];
		row.pos[2] = pos[2];
		row.source = source;
		row.life = kRadarContactLife;
		row.kind = kind;
		return;
	}
}

int32_t radar_update_contacts(RadarContactState &state, uint32_t tick,
		const int32_t viewer_pos[3], uint32_t viewer_yaw) {
	// Once per tick; the elapsed count may span several [orig: @0x59a7e0..0x59a806].
	if (state.last_tick == tick) return 0;
	const int32_t elapsed = static_cast<int32_t>(tick - state.last_tick);
	state.red12.fill(0); // [orig: @0x59a80a..0x59a85f]
	state.olive12.fill(0);
	state.red24.fill(0);
	state.olive24.fill(0);
	state.last_tick = tick; // [orig: @0x59a864]
	// ONE counter bounds the sweep while a separate pointer walks the rows, and
	// a kind-255 row fills the red 12-ring from the COUNTER to 11 and leaves
	// the counter there: every later row is still walked, but the sweep ends
	// that many rows short of the table's tail, whose rows then neither age nor
	// light [orig: esi / edx @0x59a86a..0x59a996; the fill @0x59a96d..0x59a97f].
	RadarContact *row = state.rows.data();
	for (int i = 0; i < kRadarContactRows; ++i, ++row) {
		if (row->life == 0) continue;                       // [orig: @0x59a874]
		if (row->life <= elapsed) {                         // [orig: @0x59a87c..0x59a87e]
			row->life = 0;                                  // [orig: @0x59a983]
			continue;
		}
		const uint32_t raw = hud::radar_bearing_raw(wrap_sub(row->pos[0], viewer_pos[0]),
				wrap_sub(viewer_pos[1], row->pos[1]), -hud::kRadarBearingScale);
		row->life -= elapsed;                               // [orig: @0x59a89a]
		switch (row->kind) {
		case kRadarKindRed12:                               // [orig: @0x59a8dc..0x59a8eb]
			state.red12[static_cast<size_t>(hud::radar_sector(raw, viewer_yaw, 12))] |= 1;
			break;
		case kRadarKindOlive12:                             // [orig: @0x59a902..0x59a90f]
			state.olive12[static_cast<size_t>(hud::radar_sector(raw, viewer_yaw, 12))] |= 1;
			break;
		case kRadarKindRed24:                               // [orig: @0x59a922..0x59a933]
			state.red24[static_cast<size_t>(hud::radar_sector(raw, viewer_yaw, 24))] |= 1;
			break;
		case kRadarKindOlive24:                             // [orig: @0x59a947..0x59a956]
			state.olive24[static_cast<size_t>(hud::radar_sector(raw, viewer_yaw, 24))] |= 1;
			break;
		case kRadarKindSelf:                                // [orig: @0x59a965..0x59a97f]
			while (i < 12) state.red12[static_cast<size_t>(i++)] |= 1;
			break;
		default:
			break;
		}
	}
	// The edge timers age by the same count, a signed compare of the widened
	// word [orig: @0x59a99e..0x59a9c7].
	for (uint16_t &edge : state.edge) {
		if (edge == 0) continue;
		if (static_cast<int32_t>(edge) <= elapsed)
			edge = 0;
		else
			edge = static_cast<uint16_t>(edge - elapsed);
	}
	// A nonzero count ages the map banks, MapOverlay_UpdateTimers(d); the
	// caller that owns the banks makes that call [orig: @0x59a9c9..0x59a9ce].
	return elapsed;
}

void radar_note_missile(RadarContactState &state, uint32_t source, const int32_t pos[3]) {
	const int32_t count = state.missile_count; // [orig: @0x59b200]
	// A listed missile refreshes its copy [orig: @0x59b212..0x59b22c,
	// @0x59b263..0x59b271].
	for (int32_t i = 0; i < count; ++i) {
		RadarMissile &row = state.missiles[static_cast<size_t>(i)];
		if (row.source != source) continue;
		row.pos[0] = pos[0];
		row.pos[1] = pos[1];
		row.pos[2] = pos[2];
		return;
	}
	if (count == kRadarMissileRows) return; // [orig: @0x59b231]
	state.missile_count = count + 1;        // [orig: @0x59b236]
	// The append lands one past the old count [orig: `16 * count + 16`
	// @0x59b241, stores @0x59b246..0x59b258].
	RadarMissile &row = state.missiles[static_cast<size_t>(count + 1)];
	row.source = source;
	row.pos[0] = pos[0];
	row.pos[1] = pos[1];
	row.pos[2] = pos[2];
}

void radar_reset(RadarContactState &state) {
	for (int i = 0; i < kRadarMissileRows; ++i)
		state.missiles[static_cast<size_t>(i)] = RadarMissile{}; // [orig: memset 0x400 @0x59dd49]
	state.rows.fill(RadarContact{});                              // [orig: memset 0xC00 @0x59dd59]
	state.edge.fill(0);                                           // [orig: @0x59dd65 / @0x59dd6a]
	state.last_tick = 0;                                          // [orig: @0x59dd83]
}

RadarSource radar_entity_source(const World &world, EntityHandle source) {
	RadarSource out;
	if (!source.valid()) return out;
	out.id = kRadarSourceEntity | (static_cast<uint32_t>(source.packed) & kRadarSourceIndexMask);
	out.local = source == world.cached.local_player;
	if (const Entity *entity = world.registry.get(source))
		out.link = world.registry.get(entity->primary_occupant);
	return out;
}

RadarSource radar_round_source(const World &world, const LiveRound &round, uint32_t slot) {
	RadarSource out;
	out.id = kRadarSourceRound | (slot & kRadarSourceIndexMask);
	out.link = world.registry.get(round.owner); // projectile +0x170 = the shooter
	return out;
}

int32_t radar_damage_kind(const RadarSource &source) {
	if (source.local) return kRadarKindSelf;             // [orig: @0x4dd8b3..0x4dd8c5]
	int32_t kind = kRadarKindRed12;                      // [orig: @0x4dd8c7]
	if (source.id != 0 && source.link != nullptr)        // [orig: @0x4dd8c9..0x4dd8d5]
		kind = source.link->player_class != 6 ? kRadarKindRed12 : kRadarKindRed24; // [orig: @0x4dd8d7..0x4dd8e4]
	return kind;
}

void radar_add_blip(World &world, uint32_t source, const int32_t pos[3], int32_t kind) {
	LocalPlayer *local = world.local_player_state;
	if (local == nullptr) return;
	const AiEntity *body = world.ai.for_handle(world.cached.local_player);
	if (body == nullptr) return;
	radar_add_blip(local->radar, world.rules.mpattrib, body->pos,
			static_cast<uint32_t>(body->heading), source, pos, kind);
}

void round_tracer_whiz(World &world, LiveRound &round, const AmmoTableEntry *ammo,
		const FixedVec3 &start, const FixedVec3 &end, const FixedVec3 &velocity) {
	// The tick's tail copies the Position into +0x80 right after the whiz on
	// every path, so the zip gate below reads the copy the previous tick (or
	// the spawn) left [orig: Projectile_UpdatePhysics @0x4ea9fa..0x4eaa15].
	const int32_t prev_z = round.prev_z_q16;
	round.prev_z_q16 = start.z;
	if (ammo == nullptr) return;
	// The round's ammo-flags copy carries the latch; an fgrenade never whizzes
	// [orig: +0x114 = ammo flags @0x4ec615; `test 10040000h` @0x4ea98e].
	const uint32_t flags = ammo->flags | (round.whiz_latched ? 0x40000u : 0u);
	if ((flags & 0x10040000u) != 0) return;
	// The listener box: start or end within the whiz radius on X, then on Y
	// [orig: @0x4ea99a..0x4ea9ea over g_SoundListenerPos @0x24D6630].
	const Vec3 &listener = world.out.fire_sounds.listener();
	const int32_t lx = to_fixed(listener.x);
	const int32_t ly = to_fixed(listener.y);
	const int32_t lz = to_fixed(listener.z);
	const int32_t radius = ammo->whiz_radius_q16; // [orig: ammo +0x8C @0x4ea9a8]
	const auto within = [radius](int32_t a, int32_t b) { return abs32(wrap_sub(a, b)) < radius; };
	if (!within(start.x, lx) && !within(end.x, lx)) return;
	if (!within(start.y, ly) && !within(end.y, ly)) return;

	// Projectile_SpawnTracerScarEffect over the ray record: the start, the
	// normalised flight direction v * (2^32 / |v|) [orig: @0x4ea181..0x4ea206],
	// and the listener's projection t along it [orig: @0x4e5ac7..0x4e5b5c].
	const int32_t magnitude = fixed_magnitude(velocity);
	if (magnitude == 0) return; // the zero-velocity leaf never builds a ray
	const int32_t inverse = static_cast<int32_t>(0x100000000LL / magnitude);
	const int32_t dir[3] = {retail_q16_mul_rhu(velocity.x, inverse),
			retail_q16_mul_rhu(velocity.y, inverse), retail_q16_mul_rhu(velocity.z, inverse)};
	const int32_t t = static_cast<int32_t>(
			static_cast<uint32_t>(retail_q16_mul_rhu(dir[0], wrap_sub(lx, start.x))) +
			static_cast<uint32_t>(retail_q16_mul_rhu(dir[1], wrap_sub(ly, start.y))) +
			static_cast<uint32_t>(retail_q16_mul_rhu(dir[2], wrap_sub(lz, start.z))));
	const Entity *local = world.registry.get(world.cached.local_player);
	// The projectile's +0x170 shooter: a registry entity, or on a joiner the
	// wire proxy its round event named, resolved as the retail client
	// resolves the handle to its own pool slot [orig: @0x42f491 -> @0x4ec670].
	const Entity *shooter = world.registry.get(round.owner);
	RoundSim::WireActor wire;
	const bool wire_shooter = shooter == nullptr && round.shooter_handle != 0xFFFF &&
			world.round_sim.wire_actor_provider &&
			world.round_sim.wire_actor_provider(round.shooter_handle, wire);
	const bool has_shooter = shooter != nullptr || wire_shooter;
	// The ray excludes the shooter unless the ammo is shrapnel (ray[17]), so
	// the +0x170 == ray+0x44 test holds for every non-shrapnel round and for
	// a shooterless shrapnel one [orig: @0x4ea286..0x4ea29d].
	const bool shooter_excluded = (flags & 0x4u) == 0 || !has_shooter;
	// [orig: @0x4e5b5e..0x4e5b93]
	if (local == nullptr || t < 0 || t >= radius || (shooter_excluded && t < 0x10000))
		return;
	// The zip: the ammo's tag-3 row at the closest point o + t*dir, the
	// impact record {no entity, no section, the point, 0x80000000 (the sound
	// leg alone), the projectile} [orig: @0x4e5b99..0x4e5c4a ->
	// AmmoDef_ProcessImpactEffect(ammo, 3, record)]. Its projectile gate drops a
	// ClipWaterFx round whose Z and +0x88 both sit at or under the water plane
	// [orig: AmmoDef_ProcessImpactEffect @0x40a18d..0x40a1b1]; the flags word
	// gates the sound on its sign bit and the effect and light on 0x40000000
	// [orig: @0x40a20d; @0x40a22f; @0x40a280].
	const bool clipped = (ammo->flags & kAmmoFlagClipWaterFx) != 0 &&
			start.z <= world.env.water_z && prev_z <= world.env.water_z;
	if (!clipped && world.round_sim.impacts.size() < RoundSim::kMaxPendingImpacts) {
		RoundImpact zip;
		zip.position = Vec3{
				static_cast<float>(from_fixed(start.x + retail_q16_mul_rhu(t, dir[0]))),
				static_cast<float>(from_fixed(start.y + retail_q16_mul_rhu(t, dir[1]))),
				static_cast<float>(from_fixed(start.z + retail_q16_mul_rhu(t, dir[2])))};
		zip.ammo_index = round.ammo_index;
		zip.effect_tag = 3; // "zip" [orig: push 3 @0x4e5c20]
		zip.present_effect = false;
		zip.present_sound = true;
		zip.tick = world.logic_tick;
		zip.source_order = world.round_sim.next_impact_order++;
		world.round_sim.impacts.push_back(zip);
	}
	// The shooter of a non-silenced round (ammo flag 8) lights its own bearing
	// unless it is a teammate in a team game [orig: @0x4e5c4f..0x4e5c80]; a class-6
	// shooter holding a category-3 weapon takes the olive 24-ring, anyone else
	// the olive 12-ring [orig: @0x4e5c82..0x4e5ca6; Radar_AddBlip @0x4e5cb1].
	if (has_shooter && (ammo->flags & 0x8u) == 0) {
		const uint8_t team = shooter != nullptr ? static_cast<uint8_t>(shooter->team) : wire.team;
		if (team != static_cast<uint8_t>(local->team) ||
				(world.match.rules().game_type & 0x10000u) == 0) {
			const int32_t player_class = shooter != nullptr ? shooter->player_class
					: wire.player_class;
			const WeaponTableEntry *weapon = world.tables.weapons.by_index(shooter != nullptr
							? shooter->equipped_adm_index : wire.equipped_adm_index);
			const int32_t kind = player_class == 6 && weapon != nullptr &&
					weapon->category == 3 ? kRadarKindOlive24 : kRadarKindOlive12;
			int32_t pos[3];
			uint32_t source;
			if (shooter != nullptr) {
				entity_position_q16(world, *shooter, pos);
				source = entity_source_id(*shooter);
			} else {
				pos[0] = wire.pos[0];
				pos[1] = wire.pos[1];
				pos[2] = wire.pos[2];
				source = kRadarSourceWire | round.shooter_handle;
			}
			radar_add_blip(world, source, pos, kind);
		}
	}
	round.whiz_latched = true; // [orig: +0x114 |= 0x40000 @0x4e5cb9]
}

void resolve_ammo_whiz_radii(AmmoTable &ammo, const audio::SoundSetIndex *sets) {
	for (AmmoTableEntry &entry : ammo.entries) {
		entry.whiz_radius_q16 = 0;
		if (sets == nullptr) continue;
		// The staged rows in ascending tag order: the move row (1), then the
		// zip row (3), each with a resolved sound set [orig: @0x40a046..0x40a07f].
		for (const int tag : {1, 3}) {
			const AmmoImpactEffectRow &row = entry.impact_effects[static_cast<size_t>(tag)];
			if (!row.authored || row.sound.empty()) continue;
			const audio::SetLocation set = sets->find(row.sound);
			if (!set.valid()) continue;
			int32_t radius = static_cast<int32_t>(static_cast<uint32_t>(set.cull_range) << 16);
			if (radius > 3276800) radius = 3276800; // 50 units [orig: @0x40a05b / @0x40a07d]
			entry.whiz_radius_q16 = radius;
		}
	}
}

void radar_note_guided_missile(World &world, const LiveRound &round, uint32_t slot) {
	LocalPlayer *local = world.local_player_state;
	if (local == nullptr || !world.cached.local_player.valid()) return;
	const EntityHandle self = world.cached.local_player;
	const uint32_t source = kRadarSourceRound | (slot & kRadarSourceIndexMask);
	const uint16_t target_handle = static_cast<uint16_t>(round.guided.target);
	// A joiner's pool-0 target is a wire proxy: its registry holds only its
	// own body there, so the wire handle resolves through the replica rows,
	// the local player's own handle being the target-is-local arm; pools 1..3
	// are registry twins at their wire handles [orig:
	// Entity_SerializeGuidedMissileState @0x447ece].
	const bool wire_target = world.round_sim.wire_actor_provider &&
			(target_handle & 0xF000u) == 0 && target_handle != 0xFFFFu;
	const Entity *target = wire_target ? nullptr : world.registry.get(EntityHandle{round.guided.target});
	// The target's first occupant, then the target itself; each arm stores
	// the lock tone before the list note [orig: @0x4465db..0x446602 (the
	// store @0x4465f8); @0x44660a..0x446622 (the store @0x446618)].
	if (target != nullptr && target->primary_occupant.valid() && target->primary_occupant == self) {
		local->radar.lock_tone = kRadarLockToneTicks;
		radar_note_missile(local->radar, source, round.guided.pos);
	}
	bool target_is_local = target != nullptr && target->handle == self;
	if (wire_target) {
		RoundSim::WireActor actor;
		target_is_local = world.round_sim.wire_actor_provider(target_handle, actor) && actor.is_local;
	}
	if (target_is_local) {
		local->radar.lock_tone = kRadarLockToneTicks;
		radar_note_missile(local->radar, source, round.guided.pos);
	}
}

void radar_tick_lock_tone(RadarContactState &state) {
	if (state.lock_tone != 0) --state.lock_tone; // [orig: @0x5293a5..0x5293af]
}

int32_t radar_hud_frame(World &world, uint32_t tick, bool pass_runs, bool map_site,
		bool menu_paused, hud::HudMinimapRadar &out) {
	out = hud::HudMinimapRadar{};
	LocalPlayer *local = world.local_player_state;
	Entity *player = local != nullptr ? world.registry.get(world.cached.local_player) : nullptr;
	const AiEntity *body = local != nullptr ? world.ai.for_handle(world.cached.local_player) : nullptr;
	if (body == nullptr || player == nullptr) return 0; // [orig: the null-player early-out @0x5a80a1]
	RadarContactState &state = local->radar;
	// The white hit flash skips the whole pass [orig: @0x5a8098..0x5a809f].
	const bool runs = pass_runs && !screen_flash_hud_overlays_suppressed(local->view.flash);
	int32_t aged = 0;
	if (runs && !local->view.death_screen_active) {
		// [orig: @0x5a8164..0x5a817d]
		aged = radar_update_contacts(state, tick, body->pos, static_cast<uint32_t>(body->heading));
		// The incoming-lock loop: re-registered every pass frame while the
		// tone word is live, outside the in-game menu pause, on the local
		// player's own lane at its Position [orig: @0x5a8185..0x5a81bf].
		if (!menu_paused && state.lock_tone != 0) {
			SoundEmitterEvent tone;
			tone.source_spawn_id = player->registry_spawn_id;
			tone.source_handle = player->handle.packed;
			tone.pos = player->position;
			tone.source_bms_id = player->bms_id;
			tone.emitted_tick = world.logic_tick;
			tone.lane = kRadarLockToneLane;
			tone.lifetime_ticks = kRadarLockToneLifetime;
			tone.pitch_q16 = 0x10000;
			tone.volume_q8_8 = 0xFFFF;
			tone.set_name = kRadarLockToneSet;
			world.out.sound_emitters.publish(std::move(tone));
		}
	} else if (runs && map_site) {
		// The death screen skips the pass's own call; the corner map's site
		// then stands in, a no-op within a tick otherwise
		// [orig: HUD_DrawMapOverlay @0x5a790a..0x5a791c].
		aged = radar_update_contacts(state, tick, body->pos, static_cast<uint32_t>(body->heading));
	}
	out.red12 = state.red12;
	out.olive12 = state.olive12;
	out.red24 = state.red24;
	out.olive24 = state.olive24;
	out.rules_no_tracers = (world.rules.mpattrib & kRadarRulesNoTracers) != 0;
	out.local_x = body->pos[0];
	out.local_y = body->pos[1];
	// The ring reads each listed missile's LIVE position through the stored
	// pointer [orig: HUD_DrawDirectionalIndicatorRing @0x598279..0x598294].
	for (int32_t i = 0; i < state.missile_count; ++i) {
		const RadarMissile &row = state.missiles[static_cast<size_t>(i)];
		hud::HudMinimapRadar::Threat threat;
		threat.present = row.source != 0 ? 1 : 0;
		if ((row.source & kRadarSourceRound) != 0) {
			const size_t slot = row.source & kRadarSourceIndexMask;
			if (slot < world.round_sim.rounds.size()) {
				threat.x = to_fixed(world.round_sim.rounds[slot].pos.x);
				threat.y = to_fixed(world.round_sim.rounds[slot].pos.y);
			}
		}
		out.threats.push_back(threat);
	}
	// The per-frame clear, after the map draw [orig: @0x5a87ef].
	if (runs) state.missile_count = 0;
	return aged;
}

} // namespace opennova::world
