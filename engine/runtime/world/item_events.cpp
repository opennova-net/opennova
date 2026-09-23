// Item event callbacks and the pool-specific +0x2AC clock.
// [orig: g_EntityClassEventCallbackTable @0x813000]
#include <runtime/world/destruction.h>
#include <runtime/world/world.h>
#include <runtime/world/collision.h>
#include <base/io/bam.h>
#include <base/io/strutil.h>
#include <runtime/world/angle.h>
#include <runtime/terrain_query/height_field.h>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <runtime/audio/ambient_mixer.h>

namespace opennova::world {
namespace {

// [orig: Entity_SpawnRegionalEffect @0x408290 / Entity_CalcTimeOfDayRegion @0x408110]
void regional_sound_event(World &world, Entity &entity, int phase) {
    if (phase != 0) return;
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    if (traits == nullptr) return;
    bool has_sound = false;
    for (const auto &shot : traits->regional_sounds) has_sound |= !shot.name.empty();
    if (!has_sound) return;
    const int32_t time = io::bam_add(world.env.time_of_day, (entity.handle.packed & 15) << 11);
    const int region = time > 4 * 65536 && time < 10 * 65536 ? 0 :
            time > 10 * 65536 && time < 17 * 65536 ? 1 :
            time > 17 * 65536 && time < 21 * 65536 ? 2 : 3;
    const auto &shot = traits->regional_sounds[region];
    if (!shot.name.empty()) {
        SoundSlotEvent sound;
        sound.source_handle = entity.handle.packed;
        sound.slot = static_cast<uint8_t>(audio::kSlotShotDawn + region);
        int32_t pos[3] = {int32_t(entity.position.x * 65536),
                int32_t(entity.position.y * 65536), int32_t(entity.position.z * 65536)};
        if (traits->has_sound_point) {
            // The SOUND userpoint rides the cached entity orientation matrix
            // (entity+0xB4), which carries the def scale on its rotation
            // diagonal: the same placement matrix the ambient leg uses.
            // [orig: Math_FixedPointTransformPoint22(entity->orientationMatrix,
            //  userpoint, bonePos) @0x4083A9]
            const int32_t local[] = {int32_t(traits->sound_point.x * 65536),
                    int32_t(traits->sound_point.y * 65536), int32_t(traits->sound_point.z * 65536)};
            entity_placement_matrix(entity).transform_point(local, pos);
        }
        sound.pos[0] = pos[0];
        sound.pos[1] = pos[1];
        sound.pos[2] = pos[2];
        std::strncpy(sound.set_name, shot.name.c_str(), 24);
        world.out.slot_sounds.push_back(sound);
    }
    // This draw still occurs when only a different region resolves a sound.
    // It is one step of the inline dword_31BFBB8 rotate LCG (the owner of the
    // throwable fan stream), not PRNG_Next16 on dword_31BFBB0
    // [orig: @0x4083F0; the delay sum @0x40840D].
    const uint64_t product = uint64_t(int64_t(shot.range_ticks)) * world.throwables.fan_prng() + 0x8000u;
    entity.class_think_ticks = io::bam_add(shot.base_ticks, int32_t(uint32_t(product >> 16)));
}

} // namespace

// The event-callback table in its shipped row order, each row resolved to the
// ported death body (kUnwitnessed while its callback is unported). The walk
// is a whole-string stricmp; a miss and the empty tag take row 0.
// [orig: g_EntityClassEventCallbackTable @0x813000, count 41 @0x8133D8;
//  Entity_LookupRenderCallbacks @0x407dc0 — stricmp @0x407de2, the row-0
//  default @0x407dee; EntityDef_InitAllCallbacks @0x4a5aa9 pushes "Null" for
//  an empty tag]
ItemDeathClass item_death_class_from_tag(const char *ai_function) {
	struct Row {
		const char *name;
		ItemDeathClass cls;
	};
	static constexpr Row kRows[] = {
		{"null", ItemDeathClass::kNull},          // @0x813000 -> 0x406FF0
		{"org0", ItemDeathClass::kUnwitnessed},   // @0x813018 -> 0x407310 (persons: not this notify)
		{"org1", ItemDeathClass::kUnwitnessed},   // @0x813030 -> 0x407310
		{"plyr", ItemDeathClass::kUnwitnessed},   // @0x813048 -> 0x407720
		{"brrl", ItemDeathClass::kBarrel},   // @0x813060 -> 0x407CC0
		{"envs", ItemDeathClass::kEnvironmentSound},   // @0x813078 -> 0x408290
		{"ewep", ItemDeathClass::kEwep},          // @0x813090 -> 0x4409A0
		{"ele0", ItemDeathClass::kElevator},   // @0x8130A8 -> 0x4A20D0
		{"gnrc", ItemDeathClass::kGnrc},          // @0x8130C0 -> 0x407020
		{"gnrl", ItemDeathClass::kGnrl},          // @0x8130D8 -> 0x407F80
		{"gnl2", ItemDeathClass::kGnl2},          // @0x8130F0 -> 0x4070F0
		{"flag", ItemDeathClass::kFlag},   // @0x813108 -> 0x408430
		{"squib", ItemDeathClass::kSquib},  // @0x813120 -> 0x449810
		{"nade", ItemDeathClass::kNull},          // @0x813138 -> no event callback (0)
		{"schl", ItemDeathClass::kUnwitnessed},   // @0x813150 -> 0x443670
		{"clym", ItemDeathClass::kUnwitnessed},   // @0x813168 -> 0x4438C0
		{"vmne", ItemDeathClass::kUnwitnessed},   // @0x813180 -> 0x443BB0
		{"lndm", ItemDeathClass::kUnwitnessed},   // @0x813198 -> 0x441A40
		{"bldg", ItemDeathClass::kBuilding},   // @0x8131B0 -> 0x43EE60
		{"bld2", ItemDeathClass::kCollapsingBuilding},   // @0x8131C8 -> 0x43EEE0
		{"cran", ItemDeathClass::kCrane},   // @0x8131E0 -> 0x43FC70
		{"door", ItemDeathClass::kDoor},   // @0x8131F8 -> 0x43F370
		{"target", ItemDeathClass::kTarget}, // @0x813210 -> 0x43F880
		{"emit", ItemDeathClass::kEmitter},   // @0x813228 -> 0x43F8F0
		{"towr", ItemDeathClass::kTower},   // @0x813240 -> 0x4406A0
		{"tree", ItemDeathClass::kTree},          // @0x813258 -> 0x440210
		{"palm", ItemDeathClass::kPalm},   // @0x813270 -> 0x53C4C0
		{"psec", ItemDeathClass::kNull},          // @0x813288 -> 0x406FF0
		{"CHel", ItemDeathClass::kUnwitnessed},   // @0x8132A0 -> 0x4581B0
		// [orig: sub_443630 @0x443630 == the Entity null row body @0x406FF0
		//  (`+0x2AC = 0x1000000; return`), byte-identical]
		{"rokt", ItemDeathClass::kNull},          // @0x8132B8 -> 0x443630
		{"stng", ItemDeathClass::kNull},          // @0x8132D0 -> 0x443630
		{"hlfr", ItemDeathClass::kNull},          // @0x8132E8 -> 0x443630
		{"jvln", ItemDeathClass::kNull},          // @0x813300 -> 0x443630
		// [orig: sub_443640 @0x443640 == the Entity null row body @0x406FF0]
		{"arty", ItemDeathClass::kNull},          // @0x813318 -> 0x443640
		// [orig: nullsub_65 @0x443650 / nullsub_66 @0x443660 — a bare retn]
		{"aflr", ItemDeathClass::kNone},          // @0x813330 -> 0x443650
		{"gflr", ItemDeathClass::kNone},          // @0x813348 -> 0x443660
		{"pwrp", ItemDeathClass::kNull},          // @0x813360 -> 0x406FF0
		{"cveh", ItemDeathClass::kUnwitnessed},   // @0x813378 -> 0x4583C0
		{"cbot", ItemDeathClass::kUnwitnessed},   // @0x813390 -> 0x462130
		{"cpln", ItemDeathClass::kUnwitnessed},   // @0x8133A8 -> 0x462120
		{"ctrn", ItemDeathClass::kUnwitnessed},   // @0x8133C0 -> 0x462140
	};
	if (ai_function == nullptr || ai_function[0] == '\0') return ItemDeathClass::kNull;
	for (const Row &row : kRows)
		if (strutil::iequals(row.name, ai_function)) return row.cls;
	return ItemDeathClass::kNull; // [orig: the row-0 miss default @0x407dee]
}

namespace {

// [orig: sub_50C840 @0x50C840]
void emit_item_death_state(World &world, Entity &entity) {
    if (!world.rules.mp_session || !world.rules.logic_authority || !entity.has_item_def) return;
    emit_item_state(world, entity, 0);
    if (entity.item_attrib & kItemAttribObjectiveTarget) {
        world.match.record_target_destroyed(world, entity.handle, entity.last_attacker);
        entity.objective_death_scored = true;
    }
}

// Flags |= 2 as the class bodies write it: the entity is dead from this tick
// on (the attachment, seat and AI passes read `alive`).
void mark_class_dead(Entity &target) {
	target.engine_flags |= kEntityFlagDead;
	target.alive = false;
}

// Flags |= 4 IS the husk swap in retail (the renderer reads the bit); our
// presenter consumes the event. One swap per entity.
void land_class_husk(World &world, Entity &target) {
	if ((target.engine_flags & kEntityFlagHusk) != 0) return;
	target.engine_flags |= kEntityFlagHusk;
	world.out.destruction.husk_swaps.push_back(HuskSwapEvent{target.net_id,
			target.handle.packed, target.bms_id, target.spawn_origin, target.item_id,
			target.spawned_piece_mask, target.position});
	++world.out.destruction.items_destroyed;
}

// The death presentation the gnrl/gnl2/ewep callbacks run themselves: the
// def death sound at the entity position and ONE transient particledeath
// effect (the interned def+0x412 handle, unattached, undirected) — not the
// Entity_InitDeathSounds bank/KZ chain, which only the tree and gnrc rows
// reach. [orig: Sound_PlayWithDistanceAttenuation(def+0x860, &Position,
//  entity) + submit_effect_descriptor(0, 0, &Position, word def+0x412): gnrl
//  @0x408044/@0x40806b, gnl2 @0x40714c/@0x40716f and @0x4072ba/@0x4072dd,
//  ewep @0x440c64/@0x440c87]
void emit_class_death_sound_and_effect(World &world, const Entity &target) {
	const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
	if (traits == nullptr) return;
	DestructionEvents &ev = world.out.destruction;
	if (!traits->sound_death.empty())
		ev.sounds.push_back(DestructionSoundEvent{traits->sound_death, target.position});
	if (!traits->particledeath.empty())
		ev.effects.push_back(
				DestructionEffectEvent{traits->particledeath, target.position, Vec3{}});
}

// The "gnrl" event callback [orig: sub_407F80 — row @0x8130D8].
void gnrl_death_event(World &world, Entity &target, int phase, int32_t section) {
	// A husked entity only re-arms its think [orig: @0x407f89 Flags & 4 ->
	// +0x2AC = 0x3E0]. The 10-slot seat validation walk that follows
	// (@0x407f98..0x407fee) is alive-time seat upkeep the seat system owns.
	if ((target.engine_flags & kEntityFlagHusk) != 0) { target.class_think_ticks = 992; return; }
	if (!world.rules.logic_authority) {
		// The client kill leg acts on the S2C 0x13 phase alone [orig:
		// @0x407fff; Flags |= 6 @0x408005, death tick if unset @0x408018,
		// +0x2AC = 0x3E0 @0x40801f, scar clear @0x408029, the sound/effect
		// tail].
		if (phase != 4) return;
        target.class_think_ticks = 992;
		mark_class_dead(target);
		if (target.death_tick == 0) target.death_tick = world.logic_tick;
		land_class_husk(world, target);
		world.out.scars.clear_entity(target.handle);
		emit_class_death_sound_and_effect(world, target);
		return;
	}
	if ((target.engine_flags & kEntityFlagDead) == 0) {
		if (target.health > 0) { target.class_think_ticks = 1860; return; } // alive: +0x2AC = 0x744 [orig: @0x4080ba]
		// The authority kill leg [orig: @0x4080bd..0x4080f4]: scar clear,
		// Flags |= 6, +0x2AC = 0x20 (its expiry meets the Husk gate above),
		// the death tick (unconditional), sub_50C840 @0x4080e3 — the S2C 0x26
		// entity-state send + the def-attrib-0x8000 kill scoring, both staged
		// on the net track / the damage sites (tracked §24) — then the
		// sound/effect tail.
		world.out.scars.clear_entity(target.handle);
		mark_class_dead(target);
		land_class_husk(world, target);
		target.death_tick = world.logic_tick;
        target.class_think_ticks = 32;
        emit_item_death_state(world, target);
		emit_class_death_sound_and_effect(world, target);
		return;
	}
	// Dead without the husk (a Flags 2 written elsewhere — no engine path does
	// so today): the resend arm [orig: @0x40807b..0x4080a6 —
	// Server_SendEntityStatePacket(entity, hitRecord[14]), Flags |= 4, death
	// tick if unset, +0x2AC = 0x1F].
	emit_item_state(world, target, section);
	target.class_think_ticks = 31;
	land_class_husk(world, target);
	if (target.death_tick == 0) target.death_tick = world.logic_tick;
}

// The Flags&2 think expiry of the "gnrc" callback [orig: @0x407072..0x4070a6
// — Entity_UpdateDeathTransforms(entity, 0) @0x407081 (the unitType piece
// dispatch, which lands Flags 6, then Entity_InitDeathSounds), scar clear
// @0x407087, Server_SendEntityStatePacket(entity, hitRecord[14]) @0x40708e,
// Flags |= 4 @0x407096, +0x2AC = 0x20 @0x40709b].
void gnrc_death_expiry(World &world, Entity &target, int32_t section) {
	target.class_think_ticks = 32;
	entity_update_death_transforms(world, target, /*silent=*/false);
	world.out.scars.clear_entity(target.handle);
    emit_item_state(world, target, section);
	land_class_husk(world, target); // the dispatch rows landed it: the guard holds
}

// The "gnrc" event callback [orig: sub_407020 — row @0x8130C0].
void gnrc_death_event(World &world, Entity &target, int phase, int32_t section) {
	if (!world.rules.logic_authority) {
		// The client kill leg [orig: @0x40702a phase 4 only; scar clear
		// @0x40703a, Entity_UpdateDeathTransforms(entity, 0) @0x407045, scar
		// clear @0x40704b, Flags |= 6 @0x407053, +0x2AC = 0x400 @0x407057].
		if (phase != 4) return;
        target.class_think_ticks = 1024;
		world.out.scars.clear_entity(target.handle);
		entity_update_death_transforms(world, target, /*silent=*/false);
		world.out.scars.clear_entity(target.handle);
		mark_class_dead(target);
		land_class_husk(world, target);
		return;
	}
	if ((target.engine_flags & kEntityFlagHusk) != 0) { target.class_think_ticks = 1024; return; } // +0x2AC = 0x400 [orig: @0x40706a]
	if ((target.engine_flags & kEntityFlagDead) != 0) {
		gnrc_death_expiry(world, target, section); // [orig: @0x40706e — the Flags&2 leg]
		return;
	}
	if (target.health > 0) { target.class_think_ticks = 992; return; } // +0x2AC = 0x3E0 [orig: @0x4070a7]
	// The first authority kill leg [orig: @0x4070b2..0x4070e2]: scar clear,
	// Flags |= 2 (dead, NOT yet husked), +0x2AC = 4 — the expiry above runs
	// on its next expired pool visit (tick_item_event_pool) — the death tick
	// (unconditional), sub_50C840 @0x4070d9 (the 0x26 send + kill scoring,
	// staged on the net track / the damage sites, tracked §24).
	world.out.scars.clear_entity(target.handle);
	mark_class_dead(target);
	target.death_tick = world.logic_tick;
    target.class_think_ticks = 4;
    emit_item_death_state(world, target);
}

// The Flags&2 think expiry of the "gnl2" callback [orig: @0x4071ef..0x40725f]:
// Server_BroadcastExplosionEffect(lastAttacker, {x, y, z + 1.0, yaw, pitch,
// roll}) @0x407233 — the S2C 0x21 fan-out (a net-track seam) plus its local
// half Entity_SpawnExplosionEffects @0x4399c0: Effect_AirExp at the raised
// point (@0x4399e3), one kz_M406HE blast there credited to the attacker with
// radius 0 = the ammo's kz_maxradius (@0x439a06), the attacker's weapon
// detonation sound and SP random draws via spawn_item_explosion — then
// Server_SendEntityStatePacket(entity, hitRecord[14]) @0x40723d, Flags |= 4
// @0x407242, death tick if unset @0x407258, +0x2AC = 0x20 @0x40725f.
void gnl2_death_expiry(World &world, Entity &target, int32_t section) {
	target.class_think_ticks = 32;
	const Vec3 raised{target.position.x, target.position.y, target.position.z + 1.0f};
    spawn_item_explosion(world, world.registry.get(target.last_attacker),
            {to_fixed(raised.x),to_fixed(raised.y),to_fixed(raised.z)},
            bam_heading_from_mission_yaw_deg(target.yaw), 60);
    emit_item_state(world, target, section);
	land_class_husk(world, target);
	if (target.death_tick == 0) target.death_tick = world.logic_tick;
}

// The "gnl2" event callback [orig: Entity_HandleDeathEvent @0x4070F0 — row
// @0x8130F0].
void gnl2_death_event(World &world, Entity &target, int phase, int32_t section) {
	if (!world.rules.logic_authority) {
		// The client kill leg [orig: @0x40710b phase 4: scar clear @0x407113,
		// Flags |= 6 @0x407118, death tick if unset @0x40712d, +0x2AC = 0x400
		// @0x407136, the sound/effect tail @0x40714c/@0x40716f]. The alive
		// branch that follows registers the def soundloop emitter
		// (SoundEmitter_Register @0x407199) — the audio track's loop, not a
		// death leg.
		if (phase != 4) return;
        target.class_think_ticks = 1024;
		world.out.scars.clear_entity(target.handle);
		mark_class_dead(target);
		if (target.death_tick == 0) target.death_tick = world.logic_tick;
		land_class_husk(world, target);
		emit_class_death_sound_and_effect(world, target);
		return;
	}
	if ((target.engine_flags & kEntityFlagHusk) != 0) { target.class_think_ticks = 1024; return; } // +0x2AC = 0x400 [orig: @0x4071d9]
	if ((target.engine_flags & kEntityFlagDead) != 0) {
		gnl2_death_expiry(world, target, section); // [orig: @0x4071ed — the Flags&2 leg]
		return;
	}
	// Alive: the soundloop emitter registration / +0x2AC = 0x780 [orig: @0x407276].
	if (target.health > 0) { update_item_ambient_sound(world, target); target.class_think_ticks = 1920; return; }
	// The first authority kill leg [orig: @0x407279..0x4072dd]: scar clear,
	// Flags |= 2, +0x2AC = 0x20 — the expiry above runs 32 ticks on — the
	// death tick (unconditional), sub_50C840 @0x40729f (0x26 + scoring,
	// staged), the death sound and the one particledeath effect.
	world.out.scars.clear_entity(target.handle);
	mark_class_dead(target);
	target.death_tick = world.logic_tick;
    target.class_think_ticks = 32;
    emit_item_death_state(world, target);
	emit_class_death_sound_and_effect(world, target);
}

// The "ewep" event callback [orig: Entity_UpdateChildAttachment @0x4409A0 —
// row @0x813090]. Its alive legs (@0x4409c6..0x440b58: the parent-bone pose
// copy into the weapon slots and the occupant yaw/pitch follow with the
// local-player clamp) are the attachment and seat systems' per-frame work;
// this is the death half.
void ewep_death_event(World &world, Entity &target, int phase, int32_t section) {
	// A husked emplacement only re-arms [orig: @0x4409ae Flags & 4 ->
	// +0x2AC = 0x3E0].
	if ((target.engine_flags & kEntityFlagHusk) != 0) { target.class_think_ticks = 992; return; }
    if (target.class_think_ticks <= 0 && !target.ground_target.valid()) target.class_think_ticks = 992;
    if (target.ground_target.valid() && !target.primary_occupant.valid()) target.class_think_ticks = 62;
	const EntityHandle occupant = target.primary_occupant; // entity+0x170 occupantEntity
	if (!world.rules.logic_authority) {
		// The client kill leg performs NO detach — a joiner's gunner is
		// dismounted by the server's own detach packet [orig: @0x440b6c
		// phase 4: Flags |= 6 @0x440b72, death tick if unset @0x440b85,
		// +0x2AC = 0x3E0 @0x440b8c, scar clear @0x440b96, the sound/effect
		// tail].
		if (phase != 4) return;
        target.class_think_ticks = 992;
		mark_class_dead(target);
		if (target.death_tick == 0) target.death_tick = world.logic_tick;
		land_class_husk(world, target);
		world.out.scars.clear_entity(target.handle);
		emit_class_death_sound_and_effect(world, target);
		return;
	}
	if ((target.engine_flags & kEntityFlagDead) == 0) {
		if (target.health > 0) return; // [orig: @0x440c07]
		// Kick the gunner off BEFORE the husk flags land [orig: @0x440c0d..
		// 0x440c1a — the occupant whose parentEntity (+0x16C) is this
		// emplacement -> Entity_DetachFromVehicleIfServer @0x4359d0 (the
		// authority gate we are inside) -> Entity_DetachFromVehicle
		// @0x4355F0].
		const Entity *rider = world.registry.get(occupant);
		if (rider != nullptr && rider->mount_target == target.handle)
			world.vehicles.detach(occupant);
		// Then the kill leg [orig: @0x440c23..0x440c87]: scar clear, Flags |=
		// 6, +0x2AC = 0x1F (its expiry meets the Husk gate above), the death
		// tick (unconditional), sub_50C840 @0x440c49 (0x26 + scoring,
		// staged), the sound/effect tail.
		world.out.scars.clear_entity(target.handle);
		mark_class_dead(target);
		land_class_husk(world, target);
		target.death_tick = world.logic_tick;
        target.class_think_ticks = 31;
        emit_item_death_state(world, target);
		emit_class_death_sound_and_effect(world, target);
		return;
	}
	// Dead without the husk: the second leg [orig: @0x440bb7..0x440bf2 —
	// re-detach the occupant if still seated (no parent check @0x440bb9),
	// Server_SendEntityStatePacket(entity, hitRecord[14]) @0x440bd1, Flags |=
	// 4 @0x440bd6, death tick if unset, +0x2AC = 0x1F @0x440bf2].
	target.class_think_ticks = 31;
	if (occupant.valid()) world.vehicles.detach(occupant);
    emit_item_state(world, target, section);
	land_class_husk(world, target);
	if (target.death_tick == 0) target.death_tick = world.logic_tick;
}

// The "tree" event callback [orig: Entity_HandleDestructibleDeathEvent
// @0x440210 — row @0x813258]: the authority destroys at health <= 0 (the S2C
// 0x26 send @0x4402a7 ahead of Entity_ProcessDestructibleDeath @0x4402b1), a
// client on the net-kill phase 4 (@0x440269); a husked entity resends its
// state (@0x440272) and re-arms. Also the pre-dispatch body for rows built
// without a def and for the class rows still unported (kUnwitnessed).
void tree_death_event(World &world, Entity &target, int phase, int32_t section) {
    if ((target.engine_flags & kEntityFlagDead) != 0) { target.class_think_ticks = 992; return; }
    if (phase == 0) {
        target.class_think_ticks = 992;
        regional_sound_event(world, target, phase);
    }
    if (world.rules.logic_authority) {
        if ((target.engine_flags & kEntityFlagHusk) != 0) {
            emit_item_state(world, target, section);
            target.class_think_ticks = 992;
            return;
        }
        if (target.health > 0) return;
        emit_item_state(world, target, section);
        process_destructible_death(world, target);
        target.class_think_ticks = 992;
    } else if (phase == 4) {
        emit_item_state(world, target, section);
        process_destructible_death(world, target);
    }
}

// [orig: Entity_HandleDeathOnAuthority @0x407CC0]
void barrel_event(World &world, Entity &target) {
    if (!world.rules.logic_authority) return;
    if ((target.engine_flags & kEntityFlagDead) != 0) {
        spawn_item_explosion(world, nullptr,
                {to_fixed(target.position.x),to_fixed(target.position.y),to_fixed(target.position.z)},
                bam_heading_from_mission_yaw_deg(target.yaw), 10);
        if (world.rules.mp_session) world.out.entity_events.push_back(EntityRemoveEvent{target.handle.packed});
        world.commands.remove_ssn(target.handle);
    } else if (target.health > 0) {
        target.class_think_ticks = 1920;
    } else {
        world.out.scars.clear_entity(target.handle);
        mark_class_dead(target);
        target.class_think_ticks = 10;
    }
}

void queue_class_blast(World &world, const Entity &entity, const char *name, float radius) {
    const int index = world.tables.ammo.index_of(name);
    const auto *ammo = world.tables.ammo.by_index(index);
    if (ammo == nullptr) return;
    ExplosionEntry blast;
    blast.pos = entity.position;
    blast.ammo_index = index;
    blast.type = ammo->kztype;
    blast.owner = entity.handle;
    blast.hit_word = 1;
    blast.radius_override = radius;
    world.explosions.queue_explosion(world, blast);
}

// [orig: bld2 callback Entity_ProcessCraneDestruction @0x43EEE0]
void collapsing_building_event(World &world, Entity &entity, int phase, bool crane) {
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    if (traits == nullptr || !traits->model_loaded || !traits->model_bounds_loaded) return;
    // Crane halves discover one another by exact XY and GHDR name, scanning pool 2.
    // [orig: crane callback @0x43FCA0..0x43FD7D]
    if (crane && !entity.attach_parent.valid()) {
        const char *partner = strutil::iequals(traits->graphic_name, "scrane") ? "scrane2" :
                strutil::iequals(traits->graphic_name, "scrane2") ? "scrane" : nullptr;
        if (partner != nullptr) {
            world.registry.for_each_in_pool(2, [&](const Entity &candidate) {
                if (entity.attach_parent.valid()) return;
                const auto *other = world.tables.item_death_traits.get(candidate.item_id);
                if (other == nullptr || !other->model_loaded || !strutil::iequals(other->graphic_name, partner)) return;
                if (int32_t(candidate.position.x * 65536) != int32_t(entity.position.x * 65536) ||
                        int32_t(candidate.position.y * 65536) != int32_t(entity.position.y * 65536)) return;
                entity.attach_parent = candidate.handle;
                world.registry.get(candidate.handle)->attach_parent = entity.handle;
            });
        }
    }
    const auto release_crane_half = [&] {
        if (crane && strutil::iequals(traits->graphic_name, "scrane"))
            if (Entity *half = world.registry.get(entity.attach_parent))
                half->death_motion = DeathMotionMode::CraneFalling;
    };
    const auto death_sound = [&] {
        if (!traits->sound_death.empty())
            world.out.destruction.sounds.push_back({traits->sound_death, entity.position});
    };
    if (phase == 4) {
        death_sound();
        world.out.scars.clear_entity(entity.handle);
        mark_class_dead(entity);
        land_class_husk(world, entity);
        entity.health = 0;
        entity.death_tick = world.logic_tick;
        release_crane_half();
    } else if (phase == 0 && (entity.engine_flags & 6) == 0 && entity.health > 0) {
        regional_sound_event(world, entity, phase);
    }
    if ((entity.engine_flags & kEntityFlagDead) != 0 && entity.collapse_step < 64) {
        if (entity.collapse_step == 0 && !traits->particledeath.empty()) {
            const int32_t origin[] = {int32_t(entity.position.x * 65536),
                    int32_t(entity.position.y * 65536), int32_t(entity.position.z * 65536)};
            const auto matrix = collision_matrix_from_euler(
                    bam_heading_from_mission_yaw_deg(entity.yaw), bam_from_degrees_wrapped(entity.pitch),
                    bam_from_degrees_wrapped(entity.roll), origin);
            const int32_t width = io::bam_sub(traits->model_max_q16[0], traits->model_min_q16[0]);
            const int32_t height = io::bam_sub(traits->model_max_q16[1], traits->model_min_q16[1]);
            const int nx = std::max(2, width / 393216);
            const int ny = std::max(2, height / 393216);
            const int32_t dx = width / nx;
            const int32_t dy = height / ny;
            int32_t point[3] = {traits->model_min_q16[0], traits->model_min_q16[1], 0};
            for (int edge = 0; edge < 4; ++edge) {
                const int steps = (edge & 1) != 0 ? ny : nx;
                point[0] = (edge == 1 || edge == 2) ? traits->model_max_q16[0] : traits->model_min_q16[0];
                point[1] = edge >= 2 ? traits->model_max_q16[1] : traits->model_min_q16[1];
                for (int step = 0; step < steps; ++step) {
                    int32_t pos[3];
                    matrix.transform_point(point, pos);
                    world.out.destruction.effects.push_back({traits->particledeath,
                            {pos[0] / 65536.0f, pos[1] / 65536.0f, pos[2] / 65536.0f}, {}});
                    const int axis = edge & 1;
                    point[axis] = io::bam_add(point[axis],
                            edge < 2 ? (axis == 0 ? dx : dy) : -(axis == 0 ? dx : dy));
                }
            }
        }
        if (entity.collapse_step >= 8 && entity.death_tick == 0) {
            entity.death_tick = world.logic_tick;
            // Retail also invalidates terrain-render cache tiles over these
            // bounds; the current terrain renderer has no baked item cache.
            // [orig: CVertexBuffer_RemoveFromList @0x605C10]
            // Four stack arguments: x, y, kind, GPM radius. Crane's kind 0
            // is a no-op (and consumes no CRT draw).
            // [orig: bld2 @0x43F1E3..0x43F1F1; cran @0x440087..0x440095]
            world.out.terrain_scorches.emit_sized(int32_t(entity.position.x * 65536),
                    int32_t(entity.position.y * 65536), crane ? 0 : 7,
                    traits->model_radius_q16, world.logic_tick);
        }
        ++entity.collapse_step;
        entity.class_think_ticks = 8;
    } else if ((entity.engine_flags & kEntityFlagDead) != 0) {
        land_class_husk(world, entity);
        if (entity.death_tick == 0) entity.death_tick = world.logic_tick;
        entity.class_think_ticks = 62;
    } else if (world.rules.logic_authority) {
        if (entity.health > 0) {
            entity.class_think_ticks = crane ? 1860 : 1920;
        } else {
            death_sound();
            queue_class_blast(world, entity, "kz_OrganicBlast", traits->kz);
            queue_class_blast(world, entity, "kz_MItemBlast", traits->kz);
            world.out.scars.clear_entity(entity.handle);
            emit_item_state(world, entity, 0);
            // The direct send is followed by sub_50C840's second send.
            emit_item_death_state(world, entity);
            mark_class_dead(entity);
            land_class_husk(world, entity);
            if (entity.death_tick == 0) entity.death_tick = world.logic_tick;
            entity.class_think_ticks = 16;
            entity.collapse_step = 0;
            release_crane_half();
        }
    }
}

// [orig: bldg callback @0x43EE60 -> Entity_ClearHealthInBounds]
void building_event(World &world, Entity &target) {
    if (!world.rules.logic_authority) { target.class_think_ticks = 1024; return; }
    if ((target.engine_flags & kEntityFlagDead) != 0) { target.class_think_ticks = 1024; return; }
    if (target.health > 0) return;
    const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
    if (traits == nullptr || !traits->model_loaded) return;
    world.out.scars.clear_entity(target.handle);
    const int32_t pos[3] = {int32_t(target.position.x * 65536),
            int32_t(target.position.y * 65536), int32_t(target.position.z * 65536)};
    const int32_t radius[3] = {traits->model_radius_xy_q16,
            traits->model_radius_xy_q16, traits->model_radius_z_q16};
    int32_t lo[3], hi[3];
    for (int axis = 0; axis < 3; ++axis) {
        lo[axis] = io::bam_sub(pos[axis], radius[axis]);
        hi[axis] = io::bam_add(pos[axis], radius[axis]);
    }
    for (int pool = 0; pool <= 2; ++pool) {
        world.registry.for_each_in_pool(pool, [&](const Entity &row) {
            // The +0x1C ItemTypeIndex gate precedes the bounds test in every
            // pool [orig: Entity_ClearHealthInBounds @0x509E89 / @0x509EE9 /
            //  @0x509F55].
            if (row.item_id == 0) return;
            if (pool == 0 && row.damage_state != 0) return;
            if (pool == 2 && row.has_item_def && row.item_type == 5) return;
            const int32_t p[3] = {int32_t(row.position.x * 65536),
                    int32_t(row.position.y * 65536), int32_t(row.position.z * 65536)};
            for (int axis = 0; axis < 3; ++axis)
                if (p[axis] < lo[axis] || p[axis] > hi[axis]) return;
            world.registry.get(row.handle)->health = 0;
        });
    }
    mark_class_dead(target);
    land_class_husk(world, target);
    target.class_think_ticks = 1024;
}

} // namespace

void emit_item_state(World &world, Entity &target, int32_t section) {
    if (world.rules.logic_authority && world.rules.mp_session) {
        // GetTickCount's portable monotonic millisecond equivalent.
        target.last_state_sent_ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        world.out.entity_events.push_back(ItemStateEvent{target.handle.packed, static_cast<int16_t>(section)});
    }
}

// [orig: compute_lod_fade_timers @0x5C3F40]
void update_item_destroy_fade(World &world, Entity &entity) {
    entity.destroy_phases_q16.fill(0);
    entity.destroy_progress = 0;
    if ((entity.engine_flags & kEntityFlagHusk) == 0) return;
    const auto *traits=world.tables.item_death_traits.get(entity.item_id);
    if (!traits) return;
    int32_t elapsed=int32_t(world.logic_tick-entity.death_tick);
    if (entity.destroy_timer) {
        if (elapsed < entity.destroy_timer) return;
        entity.destroy_timer=0; entity.death_tick=world.logic_tick; elapsed=0;
    }
    const int32_t duration=traits->destroy_timing_ticks[1] ? traits->destroy_timing_ticks[1] : 50;
    const int32_t step=traits->destroy_timing_ticks[2] ? traits->destroy_timing_ticks[2] : 25;
    const int32_t total=io::bam_add(duration,int32_t(uint32_t(step)*4u));
    // Zero denominators are malformed authored data; preserve finite render values.
    entity.destroy_progress=total ? double(elapsed)/total : 0;
    const auto phase=[](double value) { return int32_t(std::clamp(value,0.0,1.0)*65536.0); };
    entity.destroy_phases_q16[0]=phase(entity.destroy_progress);
    for (int i=0;i<5;++i)
        entity.destroy_phases_q16[i+1]=duration ? phase(double(
                io::bam_sub(elapsed,int32_t(uint32_t(step)*uint32_t(i))))/duration) : 0;
}

// [orig: Entity_UpdateEnvSoundEmitter @0x4A8080]
void update_item_ambient_sound(World &world, const Entity &entity) {
    const auto *traits=world.tables.item_death_traits.get(entity.item_id);
    if (!traits) return;
    const int32_t hours=io::bam_add(world.env.time_of_day,(entity.handle.packed&15)<<11);
    const auto region=audio::time_of_day_region(hours/65536.0f);
    const auto &set=traits->regional_loops[region.region];
    if (set.empty()) return;
    SoundEmitterEvent sound;
    sound.source_spawn_id=entity.registry_spawn_id; sound.source_handle=entity.handle.packed;
    sound.source_bms_id=entity.bms_id; sound.emitted_tick=world.logic_tick;
    sound.pos={entity.position.x+entity.bbox_center.x,entity.position.y+entity.bbox_center.y,
            entity.position.z+entity.bbox_center.z};
    if (traits->has_sound_point) {
        const int32_t point[]={to_fixed(traits->sound_point.x),to_fixed(traits->sound_point.y),
                to_fixed(traits->sound_point.z)};
        int32_t transformed[3];
        entity_placement_matrix(entity).transform_point(point,transformed);
        sound.pos={transformed[0]/65536.0f,transformed[1]/65536.0f,transformed[2]/65536.0f};
    }
    sound.lifetime_ticks=entity.item_type==4 ? 72 : entity.item_type==7 ? 62 :
            entity.item_type==2 || entity.item_type==5 || entity.item_type==6 ? 31 : 10;
    const int32_t blend=set==traits->regional_loops[region.adjacent] ? 65535 :
            std::min(65535, int32_t(region.blend*65536.0f));
    sound.volume_q8_8=uint16_t((uint32_t(blend)*65535u+32768u)>>16);
    sound.pitch_q16=65536; sound.lane=uint8_t(region.region); sound.set_name=set;
    world.out.sound_emitters.publish(std::move(sound));
}

void apply_item_state_event(World &world, Entity &target, int16_t section) {
    if (target.item_id == 0) return;
    target.health = 0;
    if ((target.engine_flags & kEntityFlagDead) == 0)
        destruction_notify_item_damage(world, target, 4, {section, 0});
}

void destruction_notify_item_damage(World &world, Entity &target, int phase, ItemHitContext hit) {
	// The entity+0x1C8 event callback, keyed by the def's ai_function class
	// row and invoked as cb(entity, phase, 0): 1 from the round damage, 2
	// from the blast damage [orig: @0x4e6f93], 4 from the S2C 0x13 net kill
	// [orig: @0x42ebf5]. Organics run the person callbacks and AI-driven
	// vehicles die through their state machine (rows 21/23), not here.
	if (target.kind == EntityKind::Organic || target.is_ai_capable) return;
    if (target.item_section_piece && !target.palm_sections) {
        target.class_think_ticks = 0x1000000; // clone event callback is the Null row sub_406FF0 @0x440365
        return;
    }
	const ItemDeathTraits *traits = world.tables.item_death_traits.get(target.item_id);
	switch (traits != nullptr ? traits->death_class : ItemDeathClass::kUnwitnessed) {
	case ItemDeathClass::kNull:
        target.class_think_ticks = 0x1000000;
		return; // [orig: 0x406FF0 — +0x2AC = 0x1000000 and nothing else]
	case ItemDeathClass::kNone:
		return; // [orig: nullsub_65 @0x443650 / nullsub_66 @0x443660 — retn]
	case ItemDeathClass::kGnrc:
		gnrc_death_event(world, target, phase, hit.section);
		return;
	case ItemDeathClass::kGnrl:
		gnrl_death_event(world, target, phase, hit.section);
		return;
	case ItemDeathClass::kGnl2:
		gnl2_death_event(world, target, phase, hit.section);
		return;
	case ItemDeathClass::kEwep:
		ewep_death_event(world, target, phase, hit.section);
		return;
    case ItemDeathClass::kSquib:
        squib_event(world, target, phase);
        return;
    case ItemDeathClass::kFlag:
        world.match.tick_flag_event(world, target);
        return;
    case ItemDeathClass::kEnvironmentSound:
        regional_sound_event(world, target, phase);
        return;
    case ItemDeathClass::kElevator:
        target.class_think_ticks = 62; // [orig: sub_4A20D0 @0x4A20D0]
        return;
    case ItemDeathClass::kDoor:
        world.doors.command(world, target, phase);
        return;
    case ItemDeathClass::kTarget:
        if (phase == 1) {
            // The hit callback checks base+SECTION, not base+(section-first).
            // [orig: @0x43F889..0x43F8DC]
            if (hit.section < 1 || hit.section > 30) return;
            if (!world.doors.target_section_closed(target, hit.section)) return;
            const uint32_t mask = 1u << ((hit.section - target.door_first_bone) & 31);
            world.doors.command(world, target, 6, mask);
        } else {
            world.doors.command(world, target, phase);
        }
        return;
    case ItemDeathClass::kBarrel:
        barrel_event(world, target);
        return;
    case ItemDeathClass::kTower:
        if (tower_item_event(world, target, phase, hit))
            regional_sound_event(world, target, phase);
        return;
    case ItemDeathClass::kPalm:
        palm_item_event(world, target, phase, hit);
        return;
    case ItemDeathClass::kEmitter:
        world.item_emitters.event(world, target, *traits, phase);
        return;
    case ItemDeathClass::kCrane:
        collapsing_building_event(world, target, phase, true);
        return;
    case ItemDeathClass::kCollapsingBuilding:
        collapsing_building_event(world, target, phase, false);
        return;
    case ItemDeathClass::kBuilding:
        building_event(world, target);
        return;
	case ItemDeathClass::kTree:
	case ItemDeathClass::kUnwitnessed:
		tree_death_event(world, target, phase, hit.section);
		return;
	}
}

// [orig: Entity_ApplyGravityAndGroundCheck @0x43FB30;
// Entity_UpdateWaterPhysicsAndEffects @0x4A92E0]
bool tick_item_class_motion(World &world, Entity &entity,
        const terrain::TerrainHeightField *terrain) {
    update_item_destroy_fade(world, entity);
    if (tick_item_section_motion(world, entity)) return true;
    if (entity.death_motion == DeathMotionMode::CraneFalling) {
        int32_t ground = terrain != nullptr && terrain->valid() ? int32_t(
                terrain::height_field_height_world_bilinear(*terrain,
                    entity.position.x, -entity.position.y) * 65536) : 0;
        if (const Entity *parent = world.registry.get(entity.attach_parent)) {
            if (const auto *traits = world.tables.item_death_traits.get(parent->item_id))
                ground = io::bam_add(ground, io::bam_sub(
                        io::bam_abs(traits->model_section0_min_z_q16),
                        io::bam_abs(traits->model_section0_max_z_q16)));
        }
        entity.veh.slide_z = io::bam_sub(entity.veh.slide_z, 65536);
        int32_t z = io::bam_add(int32_t(entity.position.z * 65536), entity.veh.slide_z);
        if (z <= ground) {
            entity.health = -1;
            entity.class_think_ticks = 0;
            entity.death_motion = DeathMotionMode::BuildingEffects;
            z = ground;
        }
        entity.position.z = z / 65536.0f;
        return true;
    }
    if (entity.death_motion != DeathMotionMode::BuildingEffects) return false;
    const auto *traits = world.tables.item_death_traits.get(entity.item_id);
    if (traits == nullptr || traits->physics != 0) return true;
    if ((entity.engine_flags & 6u) == 0) update_item_ambient_sound(world, entity);
    if ((entity.engine_flags & kEntityFlagHusk) == 0) return true;
    const double progress = entity.destroy_progress;
    // The one-time 0x26 send uses subtype bit 0x20.
    if (world.rules.mp_session && world.rules.logic_authority && (entity.sub_type & 0x20) == 0) {
        emit_item_state(world, entity, 0);
        entity.sub_type |= 0x20;
    }
    if ((!traits->primary_husk_loaded || progress >= 1.0) &&
            world.rules.logic_authority && (entity.sub_type & 0x80) == 0 && entity.item_type == 5) {
        entity.sub_type |= 0x80;
        queue_class_blast(world, entity, "kz_OrganicBlast", traits->kz);
        queue_class_blast(world, entity, "kz_MItemBlast", traits->kz);
    }
    return true;
}

// The pool-2/3 cohort walks of the entity update: each row on its slot cohort
// (tick&7 / tick&0x3F) runs its class callback when its +0x2AC clock is
// expired, else steps the clock by the stride (a pool-2 item without a
// callback reloads 62 instead), then its +0x1C4 update callback -- so wreck
// motion advances once per cohort visit, never per tick. A minefield row's
// callback is its minefield think. Pool 1 is the per-row visit
// (World::update_pool1_slot).
// [orig: Entity_UpdateAllEntities -- pool 2 @0x4C2244..0x4C2302: the cohort
//  @0x4C225A, the clock @0x4C2291, the callback @0x4C22B3, the 62 reload
//  @0x4C22BA, the -8 @0x4C22C6..0x4C22C9, the update callback `call eax`
//  @0x4C22E7 (the pool-2 emitter update @0x4C22FA follows it); pool 3
//  @0x4C230C..0x4C2398: the cohort @0x4C2322, the clock @0x4C2369, the
//  callback @0x4C2378, the -64 @0x4C237F..0x4C2382, the update callback
//  @0x4C2393]
void tick_item_event_pool(World &world, int pool) {
    if (pool < 2 || pool > 3) return;
    const uint32_t stride = pool == 2 ? 8u : 64u;
    const float water_z = world.env.water_z != 0 ? world.env.water_z / 65536.0f : -1.0e9f;
    const size_t cap = world.registry.pool_capacity(pool);
    for (size_t slot = 0; slot < cap; ++slot) {
        const EntityHandle handle = EntityHandle::make(pool, static_cast<int>(slot));
        Entity *entity = world.registry.get(handle);
        if (entity == nullptr) continue;
        const uint64_t lifetime = entity->registry_spawn_id;
        const bool cohort = (slot & (stride - 1)) == (world.logic_tick & (stride - 1));
        if (cohort && entity->minefield.think) {
            // [orig: Entity_LandmineThink @ 0x441A40 on the same cohort clock]
            if (!entity->hidden) {
                if (entity->minefield.age > 0)
                    entity->minefield.age -= static_cast<int32_t>(stride);
                else
                    world.minefields.think(world, *entity);
            }
        } else if (cohort && !entity->is_ai_capable) {
            if (entity->class_think_ticks <= 0) {
                if (pool == 2 && world.ai.collision != nullptr)
                    world.ai.collision->refresh_blink(world, *entity);
                const ItemDeathTraits *traits = world.tables.item_death_traits.get(entity->item_id);
                if (traits != nullptr) destruction_notify_item_damage(world, *entity, 0);
                else if (pool == 2) entity->class_think_ticks = 62;
            } else {
                entity->class_think_ticks = io::bam_sub(entity->class_think_ticks, int32_t(stride));
            }
        }
        entity = world.registry.get(handle);
        if (entity == nullptr || entity->registry_spawn_id != lifetime) continue;
        // The renderer recomputes the fade timers every frame it draws a
        // husked entity, independent of the update cohort; the presenter
        // reads the sim's copy [orig: render_sector_entity @0x5C4200].
        update_item_destroy_fade(world, *entity);
        // The update callback: the pure slot cohort [orig: @0x4C22E7 / @0x4C2393].
        if (!cohort) continue;
        if (entity->squib.motor) {
            tick_squib(world, *entity);
            continue;
        }
        tick_item_death_motion(world, *entity, world.tables.terrain, water_z, world.out.destruction);
    }
}

} // namespace opennova::world
