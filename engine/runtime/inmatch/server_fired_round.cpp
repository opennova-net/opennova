// The host side of a client's fired round -- see server_fired_round.h.

#include <runtime/inmatch/server_fired_round.h>

#include <cstdint>

#include <base/io/fixed.h> // fp16_16_to_float
#include <net/npwire/ingame_decode.h> // ClientFiredRound / decode_client_fired_round
#include <net/npwire/wire_handle.h>   // is_batch_end_sentinel
#include <runtime/inmatch/server_tick.h> // Server_AcceptsPlayerFireTick
#include <runtime/world/entity.h>
#include <runtime/world/player_weapon.h> // controller_fire_pose
#include <runtime/world/round_sim.h>
#include <runtime/world/weapon_fire_gate.h>
#include <runtime/world/weapon_inventory.h> // the slot-ammo seat legs (WeaponAmmoSeat)
#include <runtime/world/world.h>

namespace opennova::inmatch {

void Server_HandleClientFiredRound(const GameConfig &config, NapiNPConnection &conn,
		const uint8_t *payload, size_t size, world::World *world) {
	SessionReplyState &st = conn.reply;
	// [orig: NapiNPServerMsg_0x006_ClientFiredRound @0x513310, slot lookup @0x513338]
	// Residual Server_ClientFiredRound compensation (D-NET-152): saved-live-pose
	// subtraction @0x50BBAC and moving-carrier re-anchor branch @0x50BC41.
	// Cease-fire and spectator gates precede the clock, ammo, and round
	// effects. Freshness is checked on this host, never on a joiner's
	// predicted-fire path [orig: Entity_FireWeaponAndSendPacket @0x42BD80].
	if (world == nullptr || !conn.burst.spawned ||
			world->rules.cease_fire || conn.link.spectator) return;
	// The host's own loopback fire is a net-path no-op [orig: @0x50c18d returns 0
	// for the local player — its fire already ran locally].
	if (conn.link.mode == replication::TransportMode::Loopback) return;
	ClientFiredRound fr;
	size_t fire_consumed = 0;
	if (!decode_client_fired_round(payload, size, fr,
	                               fire_consumed))
		return;
	// The request's fire direction, BAM-extended [orig: @0x513436 / @0x513449].
	int32_t fire_yaw = int32_t(uint32_t(fr.dir_x) << 16);
	int32_t fire_pitch = int32_t(uint32_t(fr.dir_y) << 16);
	// Handle validity [orig: @0x513543], then anti-spoof: the claimed shooter must
	// BE this connection's own entity [orig: @0x51358d, doubled at @0x50bf37 -> -9].
	if (wire_handle::is_batch_end_sentinel(fr.shooter_handle))
		return;
	if (!conn.link.owned_entity.valid() ||
	    conn.link.owned_entity.packed != fr.shooter_handle)
		return;
	world::Entity *shooter = world->registry.get(conn.link.owned_entity);
	if (shooter == nullptr) return;
	if (!Server_AcceptsPlayerFireTick(conn, fr.current_tick, world->out.rounds.frame_clock,
			config.effective_send_holdoff_ticks())) return;

	if (((shooter->flags | shooter->engine_flags) & 0x102u) == 0x102u) return;
	const bool alt_fire = (fr.fire_flags & 0x01) != 0; // [orig: @0x50bb0d]
	// The weapon whose round fires: the request's, or the slot's.
	uint8_t fired_adm = fr.adm_index;
	// Whether the round clears the fire action's re-entry (below).
	bool round_lands = true;
	// The fire action's mode byte for an armory-fed primary round: the
	// firing slot's word before the spend, low two bits << 4, | 2.
	int32_t fired_mode = -1;
	// ADM + ammo authority — armory-fed hosts only (a table-less host accepts,
	// mirroring the 0x5A echo fallback, D-NET-141). Alt fire skips the adm lookup,
	// the clip, and the equipped mirror [orig: @0x50bb0f / the @0x50be2b alt path].
	if (!world->tables.weapons.empty() && !alt_fire) {
		const world::WeaponTableEntry *adm = world->tables.weapons.by_index(fr.adm_index);
		if (adm == nullptr) return; // [orig: "Tried to fire NULL wpn, %i" @0x50bb49]
		// Primary fire mirrors the fired weapon onto the entity ahead of every
		// later refusal (the owner checks, the can-fire's NO AMMO), so a refused
		// round still moves it. The same leg's entity+352 word (the client's
		// ammo-def byte) is D-WPN-8's, unmodeled [orig: Server_ClientFiredRound
		//  @0x50BD2D..0x50BD56, +352 @0x50BD48, entity+688 = adm @0x50BD56 - the
		//  §5.10 off-16 source; the -8 / -9 / -12 / -14 / -15 returns follow
		//  @0x50BF37..0x50C169].
		shooter->equipped_adm_index = fr.adm_index;

        if (world::weapon_fire_owner_status(*world, *shooter, adm, false) != 0)
            return;
		// Off a mount the round fires through the player's own slot of the
		// fired combo, and the can-fire tests that slot under the def it holds.
		// A combo the player was never granted holds no def, which the can-fire
		// refuses ("NO AMMO!", -15) before any clip leg, clipless weapons too; a
		// slot granted to another weapon of the combo fires that weapon.
		// [orig: Server_ClientFiredRound - the combo @0x50C0C2..0x50C0D7, the
		//  personal slot @0x50C104..0x50C116 -> WeaponSlot_CanFire @0x541BA0
		//  (the def test @0x541BB3, the slot def's legs @0x541BE1..0x541D08),
		//  the refusal @0x50C13A..0x50C169; the slot's def fires @0x50C27F /
		//  @0x50C297 / @0x50C30D]
		const bool mounted_route =
				shooter->use_gun_slot_swapped && shooter->mount_target.valid();
		world::PersonalWeaponSlot *personal = nullptr;
		world::WeaponSlotState *mounted_slot = nullptr;
		const world::WeaponTableEntry *slot_def = nullptr;
		if (mounted_route) {
			// On a UseGun seat, or a controller seat under an EWeap carrier, the
			// can-fire and the fire take the carrier's slot under the def it
			// holds, whatever weapon the request names; a carrier with no slot
			// refuses, never a fallback into the player's personal combo map.
			// [orig: Server_ClientFiredRound - the seat test @0x50C0CF..0x50C102,
			//  Entity_GetWeaponSlots @0x50C127 -> WeaponSlot_CanFire @0x50C12E;
			//  the network arm's seat test @0x50C197..0x50C1CE, the slot
			//  @0x50C1E3..0x50C1F2, its def @0x50C297, the fire action
			//  @0x50C300..0x50C30D]
			uint8_t mounted_adm = 0xFF;
			world::Entity *mount = world->registry.get(shooter->mount_target);
			if (mount != nullptr) {
				mounted_slot = world->vehicles.resolve_mounted_ammo_slot(*mount);
				if (mounted_slot == &mount->primary_weapon_slot) {
					mounted_adm = mount->primary_weapon_slot_adm;
				} else if (mounted_slot != nullptr) {
					world::Entity *parent = world->registry.get(mount->ground_target);
					if (parent != nullptr && mounted_slot == &parent->primary_weapon_slot)
						mounted_adm = parent->primary_weapon_slot_adm;
				}
			}
			if (mounted_slot == nullptr) return;
			slot_def = world->tables.weapons.by_index(mounted_adm);
			if (slot_def == nullptr) return;
			fired_adm = mounted_adm;
		} else {
			const auto found = shooter->weapon_slots.find(
					static_cast<uint16_t>(uint16_t(adm->category) * 65u + adm->rank));
			if (found == shooter->weapon_slots.end()) return;
			personal = &found->second;
			slot_def = world->tables.weapons.by_index(personal->adm_index);
			if (slot_def == nullptr) return;
			fired_adm = personal->adm_index;
		}
		// [orig: WeaponAction_Fire @0x542C11 (slot+0x10 ahead of
		//  Weapon_ConsumeAmmo @0x542C75) -> Entity_FireWeaponAndSendPacket's
		//  fire_cmd+12 @0x42BE80 -> roundParams[2] @0x50C786 -> ring+30
		//  @0x4FDCDE]
		const int32_t slot_clip_before = static_cast<int16_t>(mounted_slot != nullptr
				? mounted_slot->clip : personal->clip);
		fired_mode = ((slot_clip_before & 3) << 4) | 2;
        world::WeaponFsmInputs fire_inputs;
        world::weapon_fire_environment_inputs(*world, *shooter, fire_inputs);
        world::WeaponFsmDef fire_def = slot_def->action_fsm;
        fire_def.flags = slot_def->flags;
        fire_def.clip_capacity = slot_def->clipsize;
        fire_def.ammo_cost = slot_def->ammo_class_count;
        // The can-fire reads its pool and bucket through the shooter's seat
        // [orig: WeaponSlot_CanFire @0x541BA0 - the bucket @0x541C6D..0x541C79
        //  (WeaponSlot_GetSharedLoadedRounds), the pool @0x541CC7 (Entity_GetScoreValueBySlotType)].
        const world::WeaponAmmoSeat seat = world::entity_ammo_seat(*world, *shooter);
        world::WeaponSlotState fire_slot;
        const int pool = slot_def->ammo_class_id;
        fire_slot.reserve = world::weapon_seat_pool(seat, pool,
                pool >= 0 && pool < 128 ? st.ammo_pools[pool] : 0);
        fire_slot.clip = 1; // the real mounted/personal clip is checked below
        if (!world::weapon_fsm_can_fire(fire_def, fire_slot, fire_inputs)) return;
		if (slot_def->clipsize != -1) {
			// A bucket weapon's loaded rounds read through the seat: a UseGun
			// seat reads the carrier's MountSlot clip and spends its +710 word, a
			// controller seat reads 0 (so refuses); otherwise the player's own
			// bucket. [orig: WeaponSlot_CanFire @0x541C6D..0x541C89;
			//  Weapon_ConsumeAmmo @0x540850 - the seat legs @0x54086B..0x540894,
			//  the bucket @0x5408B5..0x5408C9, the slot word @0x540907..0x540913;
			//  Entity_SpendMountSlotReserve @0x5461D0]
			if (slot_def->ammo_bucket != 0) {
				const uint32_t bucket = static_cast<uint32_t>(slot_def->ammo_bucket);
				const bool own_bucket = bucket < st.shared_clips.size() &&
						bucket < world->tables.weapons.ammo_class_names.size();
				// A bucket past the class count reads 0 at any seat
				// [orig: WeaponSlot_GetSharedLoadedRounds @0x5405FC..0x540603].
				if (!own_bucket ||
						world::weapon_seat_bucket(seat, st.shared_clips[bucket]) == 0)
					return;
				if (seat.parent_slot == 3) {
					world::Entity *carrier = world->registry.get(shooter->mount_target);
					if (carrier != nullptr) {
						int32_t word = static_cast<int16_t>(carrier->primary_weapon_slot.reserve);
						if (word != -1) {
							word -= slot_def->ammo_class_count;
							if (word < 0) word = 0;
							carrier->primary_weapon_slot.reserve = static_cast<int16_t>(word);
						}
					}
				} else if (st.shared_clips[bucket] != 0) {
					--st.shared_clips[bucket];
				}
			} else if (mounted_slot != nullptr) {
				if (static_cast<int16_t>(mounted_slot->clip) == 0) return;
				--mounted_slot->clip;
			} else {
				world::PersonalWeaponSlot &slot = *personal;
				if (static_cast<int16_t>(slot.clip) == 0) return;
				--slot.clip;
			}
		}
		// The accepted fire re-points the shooter's EquippedSlot: the borrowed
		// mount slot it fired through, else the personal slot of the fired
		// combo, which also ends a borrow a refused UseGun attach left on a body
		// on foot (D-NET-420). [orig: Server_ClientFiredRound @0x50c1d4..0x50c28d]
		if (mounted_route) {
			shooter->equipped_slot = {};
			shooter->equipped_slot.kind = world::WeaponSlotRef::kMount;
			shooter->equipped_slot.mount = shooter->mount_target;
		} else {
			shooter->equipped_slot = {};
			shooter->equipped_slot.kind = world::WeaponSlotRef::kPersonal;
			shooter->equipped_slot.combo = uint16_t(adm->category) * 65u + adm->rank;
			shooter->equipped_slot.adm = personal->adm_index;
		}
		// The slot's fire action re-enters the handler with the weapon that
		// fires, whose local pass judges the round again: the owner checks
		// on that weapon, then the claimed origin's reach from the shooter.
		// A refusal there drops the round, its score and the spawn-protection
		// clear; the clip is spent either way (the spend follows the
		// re-entry) and the fire-tick floor still stamps (the network pass
		// returned 0). [orig: WeaponAction_Fire @0x542C5E ->
		//  Entity_FireWeaponAndSendPacket @0x42BF34 -> Server_ClientFiredRound's
		//  local pass - the owner checks @0x50C083..0x50C0B9, the reach
		//  @0x50C33C..0x50C6FA; Weapon_ConsumeAmmo @0x542C75; the 0x06
		//  handler's stamp @0x51370E..0x513740]
		// On a controller seat under an EWeap carrier the network arm rebuilds the
		// fire pose when the carrier names a weapon userpoint for the request's
		// barrel (its flags' bits 4-5): the request's pose is zeroed and replaced
		// by the transform, which the slot latches and the second pass, the ring
		// and the round then read. The transform poses that barrel's record only
		// when the slot's own pre-spend barrel (its clip & 3, at the column its next
		// action selects: 1 after a RECOIL chain, else 0) names a point too;
		// otherwise it is the carrier's raw pose. [orig: Server_ClientFiredRound -
		//  the controller test @0x50C1EB..0x50C1F4, Entity_GetWeaponSlotByte(carrier,
		//  flags >> 4 & 3, 0) @0x50C1FA..0x50C219, the record @0x50C21B..0x50C23C,
		//  the pose zeroed @0x50C239..0x50C250, slot+0x2C = 2 @0x50C259,
		//  Entity_ComputeUserpointTransform @0x50C275 - the column
		//  @0x545B09..0x545B31, the slot's byte @0x545B46..0x545B4B, the posed leg
		//  @0x545BA5..0x545C09, the raw copy @0x545C1B..0x545C41; the latch from the
		//  request @0x50C2C8..0x50C2E9]
		if (mounted_route && shooter->mount_type == world::SeatType::Controller) {
			world::Entity *carrier = world->registry.get(shooter->mount_target);
			const int barrel = (fr.fire_flags >> 4) & 3;
			if (carrier != nullptr && world::weapon_userpoint_byte(*carrier, barrel, 0) != 0) {
				int32_t pose[6] = {};
				const int column = mounted_slot != nullptr &&
						mounted_slot->next == world::weapon_action::kRecoil ? 1 : 0;
				if (world::weapon_userpoint_byte(*carrier,
						int(uint32_t(slot_clip_before) & 3u), column) != 0)
					world::controller_fire_pose(*world, *carrier, slot_def, barrel, pose);
				else
					(void)world::carrier_weapon_world_pose(*world, *carrier, nullptr, 0, 0,
							pose); // no slot def: the raw leg
				fr.pos_x = pose[0];
				fr.pos_y = pose[1];
				fr.pos_z = pose[2];
				fire_yaw = pose[3];
				fire_pitch = pose[4];
			}
		}
		const world::FixedVec3 claimed{fr.pos_x, fr.pos_y, fr.pos_z};
		round_lands =
				world::weapon_fire_owner_status(*world, *shooter, slot_def, false) == 0 &&
				world::weapon_fire_origin_status(*world, *shooter, *slot_def, claimed,
						/*replay_client_action=*/false) == 0;
		// The authority arms the slot the fire went through, after the controller
		// pose read its pre-fire next action: FIRE runs on it (its shot and spend
		// are this handler's legs), and the weapon walk pumps it on from there, a
		// remote player's own row included (D-NET-336).
		// [orig: Server_ClientFiredRound @0x50BAA0 - the arm @0x50C2B0..0x50C2F0,
		//  the fire action @0x50C30D -> WeaponAction_Fire @0x542B10]
		{
			world::WeaponSlotState &armed =
					mounted_slot != nullptr ? *mounted_slot
					                        : static_cast<world::WeaponSlotState &>(*personal);
			world::WeaponFsmInputs arm_inputs;
			arm_inputs.is_local = false;
			world::WeaponFsmEvents arm_events;
			world::weapon_fsm_arm_network_fire(fire_def, armed, arm_inputs, arm_events);
		}
	}
	// Stamp the claimed target on the shooter; the tag-2 serializer reads it LIVE
	// [orig: @0x50c2ad shooter+104->+12; NetPacket_SerializeRoundEvent @0x50485a].
	world::EntityHandle fire_target{};
	if (!wire_handle::is_batch_end_sentinel(fr.target_handle)) {
		const world::EntityHandle th{fr.target_handle};
		if (world->registry.get(th) != nullptr) fire_target = th; // [orig: @0x5135d2]
	}
	shooter->last_fire_target = fire_target;
	// A primary round re-enters through the adm fire action, whose
	// local pass scores the shot (scorer event 1) once it clears the
	// pose checks; alt fire returns through RoundData_AddRound first.
	// [orig: Server_ClientFiredRound @0x50BAA0 — alt @0x50BB0D, the
	//  event-1 call @0x50C727]
	if (!alt_fire && round_lands)
		world->match.record_shot(*world, conn.link.owned_entity);
	// The landed primary round ends the shooter's spawn protection: an
	// in-session authority clears entity+292 for a non-spectator slot whose
	// value is nonzero, on the same local pass, so alt fire never clears it.
	// The listen host's own fire takes the same clear through
	// Entity_FireWeaponAndSendPacket's authority leg (world/player_weapon.cpp
	// local commit). [orig: Server_ClientFiredRound @0x50BAA0
	//  @0x50c736..0x50c75d; the alt return @0x50BE4B..0x50BE65]
	if (!alt_fire && round_lands && world->rules.mp_session &&
	    shooter->damage_state != 0 && !conn.link.spectator)
		shooter->damage_state = 0;

	// Ring append [orig: RoundData_AddRound @0x4fdb40]. The ring stores the
	// PRE-SPREAD origin/direction — exactly the client's claimed fire pose
	// [orig: @0x4fdbce reads the request before RoundData_SpawnRound's spread].
	world::RoundEvent ev;
	ev.shooter_handle = fr.shooter_handle;
	ev.origin_x = fr.pos_x;
	ev.origin_y = fr.pos_y;
	ev.origin_z = fr.pos_z;
	ev.dir_yaw = fire_yaw;
	ev.dir_pitch = fire_pitch;
	// The round's sequence word: the client's hit_part on the primary
	// path, recomposed with the slot's roster index on the alt path
	// [orig: word_B7C670 = hit_part @0x50c2ba / @0x50C774; the alt
	//  (slot+20 << 9) | (hit_part & 0x1FF) @0x50BD8F..0x50BDA5 -> the round's
	//  +120 word -> ring+28 @0x4fdcf5].
	const uint16_t shot_seq = alt_fire
			? uint16_t((uint32_t(conn.reply.player_slot) << 9) | (fr.hit_part & 0x1FFu))
			: fr.hit_part;
	ev.shot_seq = shot_seq;
	// ring+30: the fire action's mode byte for an armory-fed primary round,
	// else the request's fire-flags byte (alt fire's AddRound, a
	// table-less host) [orig: @0x4fdcde; the alt path @0x50BDFC].
	ev.mode_flags = fired_mode >= 0 ? uint8_t(fired_mode) : fr.fire_flags;
	// Shooter fire-context composite [orig: ctx & 0x3F @0x50bd83, recombined
	// | (ctx >> 7) << 7 into roundParams[4] @0x50c7bd -> ring+31].
	ev.subtype = uint8_t((fr.extra_byte2 & 0x3F) | ((fr.extra_byte2 >> 7) << 7));
	ev.slot_byte = fr.misc_byte; // [orig: ring+32 <- fireRequest+80 @0x4fdcfc]
	ev.adm_index = fired_adm;
	// The authoritative round spawns SYNCHRONOUSLY with the ring append
	// [orig: RoundData_AddRound @0x4fdb40 inline-calls RoundData_SpawnRound
	// @0x4ec0d0 — the ring is only the tag-2 fan-out log; §5.60]. Ammo = the
	// adm's load-time-resolved round_type (+4; +84 is the stat id); an
	// armory- or ammo-less host skips the sim (fire still echoes).
	if (round_lands && !world->tables.ammo.empty()) {
		// The round's ammo: the fired weapon's round on the primary path;
		// on the alt path the request's byte is the ammo index itself, and
		// the weapon rides only under the adm-indexed flag bit.
		// [orig: Server_ClientFiredRound - the alt read of +81 @0x50BB0F,
		//  deltaZ @0x50BB7B into roundParams[3] @0x50BDF1; RoundData_AddRound
		//  @0x4FDB95..0x4FDBB5 (the flags & 2 adm entry), the ammo
		//  hit_params[2] @0x4FDC10]
		const world::WeaponTableEntry *fire_adm =
				world->tables.weapons.by_index(fired_adm);
		int32_t round_ammo = fire_adm != nullptr ? fire_adm->ammo_index : -1;
		if (alt_fire) {
			round_ammo = fr.adm_index < world->tables.ammo.entries.size() &&
							world->tables.ammo.entries[fr.adm_index].valid
					? int32_t(fr.adm_index)
					: -1;
		}
		if (round_ammo >= 0) {
			world::RoundSpawnParams rp;
			rp.owner = conn.link.owned_entity;
			rp.shooter_handle = fr.shooter_handle;
			rp.origin.set_q16({fr.pos_x, fr.pos_y, fr.pos_z}); // the wire's words (D-NET-507)
			rp.dir_yaw_bam = ev.dir_yaw;
			rp.dir_pitch_bam = ev.dir_pitch;
			rp.ammo_index = round_ammo;
			rp.adm_index = fired_adm;
			rp.no_weapon = alt_fire && (fr.fire_flags & 0x02) == 0;
			// The alt leg returns through RoundData_AddRound alone: the host runs no
			// launch presentation for it, no ammo effect, glow or sound (the round's
			// own flight presents as any round's). [orig: Server_ClientFiredRound -
			//  the alt AddRound @0x50BE52 and its return @0x50BE57..0x50BE64]
			rp.launch_presented = alt_fire;
			// A primary round came through the slot's fire action on this host, whose
			// FIRE row presents its effect and glow [orig: the fire action
			//  @0x50C30D -> WeaponAction_Fire @0x542CB4].
			rp.action_leg_presentation = !alt_fire && fired_mode >= 0;
			rp.shot_seq = shot_seq;
			rp.subtype = ev.subtype;
			// PowerThrow charge from fireRequest+80; RoundData_SpawnRound
			// scales velocity for bytes 1..254 [orig: @0x4ec5bb].
			rp.charge = fr.misc_byte;
			// The ring takes the spawned round's word: its id, a fan's last
			// pellet's, pool slot 0's for a spawn that allocated nothing
			// [orig: RoundData_AddRound - RoundData_SpawnRound @0x4FDC58, ring+28
			//  = pool[ret]+0x78 @0x4FDCF1..0x4FDCF5; the fans return the cursor
			//  @0x4EBE6D].
			world->round_sim.spawn(*world, rp);
			ev.shot_seq = world->round_sim.spawned_round_word();
		}
	}
	if (round_lands) world->out.rounds.add(ev);
	// A rejected shot leaves this floor untouched. Alt fire stamps only
	// its tick; primary adds the baked ADM action interval.
	// [orig: NapiNPServerMsg_0x006_ClientFiredRound @0x513310, stamp @0x513740]
	if (conn.fire_tick_mode) {
		const auto *adm = world->tables.weapons.by_index(fr.adm_index);
		conn.fire_tick_floor = fr.current_tick +
				(!alt_fire && adm != nullptr ? adm->fire_interval_ticks() : 0u);
	}
	// No reactive reply — the echo rides the per-frame 0x0A fan (replication
	// select_round_events), reaching every OTHER in-match recipient.
}

} // namespace opennova::inmatch
