// The joiner role's decoded-gameplay appliers: the S2C gameplay drains after the client
// receive pump (the 0x23 remote WAC commands, the 0x3F objective notices, the 0x0A tag-2
// fired-round descriptors, the 0x49 reload echoes, the 0x35 weapon pickups), the replayed
// round's spawn, and the 0x0A phase-2 ENV sample onto the weather home. Moved verbatim out of
// joiner_role.cpp (the file sat at the 2500-line ratchet); see joiner_role.h for the ownership
// split; every phase keeps its witnesses.
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/client_weapon_replay.h>

#include <runtime/mission/mission_kernel.h>

#include <net/npwire/ingame_decode.h> // kRoundEventFlag* (the fire-mode byte)
#include <net/npwire/wire_handle.h>   // is_batch_end_sentinel / kInvalid

#include <base/io/fixed.h> // kFp16OneD

#include <runtime/wac/remote_command.h> // run_remote_command (the S2C 0x23 handler body)
#include <runtime/world/infantry.h>     // kAnimStanceFlag* (the witnessed stance bits)
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_table.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::inmatch {

namespace {

constexpr double kFixed16 = io::kFp16OneD;

} // namespace

// Drain typed S2C gameplay events after the client recv pump: tag-2 fires
// spawn visual-only rounds; the requester's 0x49 echo performs its refill.
void JoinerRole::apply_gameplay_events() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	rt.apply_received_effects(world);

	// S2C 0x23 replicated WAC commands: run the registry row's handler here
	// with the operands the wire carried — the same body the host VM ran for a
	// broadcast row, or ran only here for a targeted ptext/pwave/pconsol. Fx
	// and SoundSet operands index this peer's mounted catalogs.
	// [orig: GameMode_DispatchRemoteCommand @0x4F81E0 — the call-convention
	//  switch @0x4f8438 into the row's handler]
	const std::vector<ScriptRemoteCommand> remote_commands =
			rt.drain_script_remote_commands();
	if (!remote_commands.empty()) {
		const std::vector<std::string> effect_names =
				kernel.script_effect_catalog.interned_names();
		const wac::RemoteCommandNames names{
				&effect_names, &kernel.script_sound_catalog.names()};
		std::vector<world::ScriptRemoteArg> args;
		for (const ScriptRemoteCommand &command : remote_commands) {
			args.clear();
			args.reserve(command.args.size());
			for (const ScriptRemoteCommandArg &arg : command.args)
				args.push_back({static_cast<int32_t>(arg.value), arg.text});
			const wac::RemoteCommandResult result =
					wac::run_remote_command(world, command.command_index, args, names);
			// A wire row with no handler body here is the joiner's dispatch gap;
			// the host VM records the same kind at its instruction site.
			if (!result.handled)
				world.diagnostics.record({world::RuntimeGapKind::WacCommand,
						command.command_index, 0, -1, -1}, world.logic_tick);
		}
	}

	// S2C 0x3F: kind 0 re-runs the objective notification here with relay
	// flag 0 (off the authority it posts the chat lines and relays nothing),
	// then NEW_GOAL at the local player for flag 1. Kind 1, and the kind-0
	// arm's fall-through into the same reads, is the mission-text chat relay:
	// the key resolves in this peer's mission text and a non-empty line posts
	// (a kind-0 body has no tail, so its empty key posts nothing).
	// [orig: NapiNPClientMsg_0x03F @0x42BB20 — HUD_ShowObjectiveNotification
	//  @0x42bbc2, NEW_GOAL @0x42bbc7..0x42bbe4, MissionText_GetStringByKey
	//  @0x42bc26 -> GameMsg_AddChatLineAndRelay @0x42bc39 (its line gate
	//  @0x5ba170..0x5ba18d)]
	for (const ObjectiveNotification &notice : rt.drain_objective_notifications()) {
		if (notice.kind == 0) {
			world.show_objective_notification(notice.slot, notice.is_win, notice.is_active, 0);
			if (notice.flag == 1) {
				if (const world::Entity *player = world.registry.get(world.cached.local_player))
					world.out.fire_sounds.play_immediate("NEW_GOAL", player->position, 0);
			}
		}
		if ((notice.kind == 0 || notice.kind == 1) && !notice.key.empty() &&
				(world.rules.mp_session_peer || !world.rules.mp_session))
			world.out.effects.push({"mission_text_chat", notice.team, 0, 0, 0, notice.key});
	}

	// Retail's S2C 0x0A tag-2 record is a fired-round descriptor. Re-run the
	// normal round spawner so tracers and physical impacts are produced locally;
	// World::run_logic_tick admits this pool only under the explicit
	// mp_session && !projectile_authority visual-client gate. Every descriptor
	// is spawned VisualOnly below, and that mode gates every gameplay consequence.
	for (const auto &ev : rt.drain_round_events()) apply_round_event(ev);

	for (const WeaponReload &reload :
			rt.drain_reload_notifications()) {
		++lp.weapon.reload_received_serial;
		lp.weapon.reload_received_entity = reload.entity_handle;
		lp.weapon.reload_received_param = reload.reload_param;
		// ClientRuntime already applied the remote-Person receive-handler stamp
		// before that frame's body tick. This drain retains diagnostics, the
		// non-person branch's refill on an EWeap entity's own MountSlot, and the
		// self-only WeaponSlot_ReloadAmmo branch below.
		// [orig: NapiNPClientMsg_WeaponReload_0x049 @0x42C0A0 - the non-person
		//  WeaponSlot_ReloadAmmo call @0x42C116, whose EWeap slot @0x54172D]
		if (!rt.has_self_handle() || reload.entity_handle != rt.self_handle()) {
			// The slot is the one the entity's class init bound; one with no Def
			// reloads nothing [orig: WeaponSlot_ReloadAmmo @0x541720 - the slot and its
			//  Def tests @0x54178F..0x541791 / @0x54179A..0x54179C].
			world::Entity *mount = world.registry.get(world::EntityHandle{reload.entity_handle});
			if (mount != nullptr && mount->has_item_def &&
					(mount->item_attrib & world::kItemAttribEweap) != 0u)
				(void)world.vehicles.reload_weapon_slot(*mount);
			continue;
		}
		if (!lp.inventory_valid) continue;

		const int32_t combo = reload.reload_param;
		world::WeaponInventorySlot *reloaded = lp.inventory.slot(combo);
		const world::WeaponTableEntry *def =
				(reloaded != nullptr && reloaded->adm_index >= 0)
						? world.tables.weapons.by_index(
								static_cast<uint8_t>(reloaded->adm_index))
						: nullptr;
		if (reloaded == nullptr || def == nullptr) continue;

		// WeaponSlot_ReloadAmmo is run only on the echoed notification: refund
		// the payload-addressed slot's remaining clip to its ammo-class pool and
		// draw a full clip. A late echo still reloads that exact slot after a switch;
		// only the currently active personal slot has an FSM mirror to update here.
		// The refill reads, spends and writes through the player's seat
		// [orig: WeaponSlot_ReloadAmmo @0x5417C5..0x541842].
		const world::Entity *self_body = world.registry.get(world.cached.local_player);
		const world::WeaponAmmoSeat seat = self_body != nullptr
				? world::entity_ammo_seat(world, *self_body) : world::WeaponAmmoSeat{};
		const int32_t reloaded_rounds = world::weapon_inventory_reload_slot(
				world.tables.weapons, lp.inventory, combo, seat);
		if (lp.weapon.active && !lp.weapon.usegun_slot_active &&
				lp.inventory.equipped_combo >= 0) {
			world::WeaponSlotState &active_slot =
					*world::active_local_weapon_slot(world, lp.weapon);
			const world::WeaponInventorySlot *equipped =
					lp.inventory.slot(lp.inventory.equipped_combo);
			const world::WeaponTableEntry *equipped_def =
					(equipped != nullptr && equipped->adm_index >= 0)
							? world.tables.weapons.by_index(
									static_cast<uint8_t>(equipped->adm_index))
							: nullptr;
			if (equipped_def != nullptr) {
				active_slot.reserve = world::weapon_seat_pool(seat, equipped_def->ammo_class_id,
						world::weapon_pool_get(lp.inventory, equipped_def->ammo_class_id));
			}
			if (combo == lp.inventory.equipped_combo) {
				active_slot.clip = reloaded_rounds;
				active_slot.phase = static_cast<uint8_t>(
						active_slot.phase &
						~world::weapon_phase::kReloadPendingBit);
			}
		}
		++lp.weapon.reload_applied_serial;
		world::AiEntity *player = world.ai.for_handle(world.cached.local_player);
		if (player != nullptr && player->inf.active)
			player->inf.reload_anim_ticks = 80;
	}

	// The frame decays remote recoil after the emplacement channels consume it.
}

void JoinerRole::apply_round_event(const replication::ClientRoundEvent &ev) {
    auto &world = kernel_->world;
    auto &rt = *runtime;
    auto &kernel = *kernel_;

	// The retail deserializer dispatches only the alt/projectile bit or the
	// standard adm-indexed bit [orig: @0x42f2a8]. Other flag shapes do not
	// enter RoundData_SpawnRound.
	if ((ev.flags & (kRoundEventFlagAltFire | kRoundEventFlagAdmIndexed)) == 0)
		return;
	// The event names its shooter, which must resolve to a live entity with an
	// item def on this client, or the whole event is dropped: no 0xFFFF, no pool
	// past 4, no slot past its pool, no empty slot or def. Here a shooter is a
	// decoded row with its type or a native twin with its def.
	// [orig: NetPacket_DeserializeRoundEvent - the 0xFFFF test @0x42F451, the pool
	//  @0x42F465, the slot @0x42F47C..0x42F482, the entity and its +0x20 def
	//  @0x42F497..0x42F4A1]
	{
		if (wire_handle::is_batch_end_sentinel(ev.shooter_handle)) return;
		const replication::ClientEntityState *row = rt.state().find(ev.shooter_handle);
		const world::Entity *twin = world.registry.get(world::EntityHandle{ev.shooter_handle});
		if ((row == nullptr || row->type_id == 0) && (twin == nullptr || !twin->has_item_def))
			return;
	}
	// Bit 0 is tested first: the ammo-def arm reads the byte as the round's ammo
	// index and spawns with no weapon. Only with bit 0 clear does the adm-indexed
	// arm resolve the byte as a weapon and fly that weapon's round.
	// [orig: NetPacket_DeserializeRoundEvent - the arm test @0x42F521, the ammo
	//  bound @0x42F52C..0x42F53E, the null weapon @0x42F582 into
	//  RoundData_SpawnRound @0x42F5BA; the adm arm's AdmDef_GetEntryByIndex
	//  @0x42F6D9 and its ammo byte @0x42F6F9]
	const bool ammo_arm = (ev.flags & kRoundEventFlagAltFire) != 0;
	int32_t round_ammo = -1;
	if (ammo_arm) {
		if (world.tables.ammo.by_index(ev.adm_index) == nullptr) return;
		round_ammo = ev.adm_index;
	} else {
		const world::WeaponTableEntry *adm =
				world.tables.weapons.by_index(ev.adm_index);
		if (adm == nullptr || adm->ammo_index < 0) return;
		round_ammo = adm->ammo_index;
	}
	world::RoundSpawnParams round;
	world::RoundSourceState source;
	round.owner = world::EntityHandle{};
	round.shooter_handle = ev.shooter_handle;
	// The mounted shooter's own vehicle joins the trace exclusion exactly
	// like retail's mount rule — see wire_carrier_exclusion_for.
	round.shooter_carrier_handle = wire_carrier_exclusion_for(
			rt.state(), ev.shooter_handle, kernel.seat_specs);
	// Retail resolves the wire shooter entity and copies its TEAM into the
	// spawned round — the friend/enemy throwable item and tracer styling key
	// on it. Without this every remote grenade wore the enemy variant (and a
	// variant with no motor row froze mid-air). [orig: @0x4ec705]
	if (replication::ClientEntityState *shooter_row =
				rt.state().find(ev.shooter_handle)) {
		round.shooter_team = shooter_row->team;
		const int anim = shooter_row->anim_state_id;
		// The category follows the retail animation-flags table, not a
		// hand-maintained list of familiar locomotion clips. In particular,
		// 170/171 remain crouched, 172 is prone, and idle_mortar (46) is
		// neither. [orig: g_AnimStateFlagsTable @0x8139E8, mapped onto +0x12C by
		// NetPacket_SerializePlayerState @0x4C11FD..0x4C1242; category read in
		// RoundData_SpawnRound @0x4EC129..0x4EC14C, the row @0x4EC2A6..0x4EC2FD]
		const uint32_t anim_flags = world::infantry_anim_flags(anim);
		const bool prone = (anim_flags & world::kAnimStanceFlagProne) != 0;
		// The spread's up/down limit reads the raw prone bit, not the row
		// [orig: RoundData_SpawnRound @0x4EC137, read @0x4EC454].
		source.prone = prone;
		const bool crouched =
				(anim_flags & world::kAnimStanceFlagCrouched) != 0;
		const bool mounted =
				round.shooter_carrier_handle != wire_handle::kInvalid;
		// The shooter's Flags dword as a retail client holds a decoded peer's:
		// the spawn word and the client motor's bits (the row's mirror), with the
		// latest compact record's byte folded in under 0xFD, as the 0x0A fold
		// does (`Flags ^= (Flags ^ wire) & 0xFD`): Parachute 0x20 rides the
		// record, Drowning 0x8000 and LadderContact 0x100000 are the motor's.
		// [orig: NetPacket_SerializePlayerState @0x4C1307..0x4C1311 (a player);
		//  @0x4C065D..0x4C0662 (an infantry)]
		const uint32_t mirror = shooter_row->rm_entity_flags;
		const uint32_t shooter_flags = shooter_row->state_flags_known
				? (mirror & ~0xFDu) | (shooter_row->state_flags & 0xFDu)
				: mirror;
		// Retail tests eye Z (Position.Z + CameraOffset.Z) against the fixed
		// water plane [orig: RoundData_SpawnRound @0x4EC0D0 (@0x4EC2DE..0x4EC2EA)]. The
		// decoded row has no CameraOffset carrier, so raw fixed position Z is
		// the bounded projection. Do not invent a standing-eye constant here.
		// The plane is compared raw (no dry sentinel) [orig: `cmp ecx,
		// g_EnvWaterHeightFixed` @0x4EC2E4 / @0x4EC87F].
		const bool below_water = shooter_row->z < world.env.water_z;
		source.person_with_item_def =
				shooter_row->cls == EntityClass::Player ||
				shooter_row->cls == EntityClass::Infantry;
		source.player = shooter_row->cls == EntityClass::Player;
		// The recoil's x4 leg: Drowning, or the eye below the water
		// [orig: RoundData_SpawnRound `test ecx, 8000h` @0x4EC342 (the shotgun
		//  leg) / @0x4EC86D (the single round), the eye @0x4EC34E..0x4EC35A /
		//  @0x4EC879..0x4EC885].
		source.underwater =
				(shooter_flags & world::kEntityFlagDrowning) != 0u || below_water;
		// The stance row: Parachute, Drowning or LadderContact, or the eye below
		// the water, forces the standing row 2; a mount after it wins (row 1).
		// [orig: RoundData_SpawnRound `test dword ptr [ebp+24h], 108020h`
		//  @0x4EC2D5, the eye @0x4EC2DE..0x4EC2EA, row 2 @0x4EC2EC, the mount
		//  @0x4EC2F4..0x4EC2FD]
		const bool standing_override =
				(shooter_flags & (world::kEntityFlagParachute | world::kEntityFlagDrowning |
				                  world::kEntityFlagLadderContact)) != 0u ||
				below_water;
		source.stance_category = mounted ? 1 : (standing_override ? 2 :
				(prone ? 0 : (crouched ? 1 : 2)));
		source.scope_raised =
				(shooter_row->state_flags &
				 world::kEntityFlagScopeRaised) != 0;
		// The spawn's Indoors stamp reads the resolved shooter's live Flags
		// (client_weapon_replay.cpp) [orig: RoundData_SpawnRound @0x4EC830].
		source.indoors = replica_shooter_indoors(*shooter_row,
				replication::replica_row_entity(&materializer_, world,
						world::EntityHandle{shooter_row->handle}, shooter_row->type_id));
		source.recoil_pitch = &shooter_row->recoil_pitch;
		// The zero-elevation row keys on the shooter's equipped AdmDef, the
		// replica's 0x0A off-16 echo [orig: RoundData_SpawnRound entity+0x2B0
		// @0x4ec16f].
		source.equipped_adm_index = shooter_row->equipped_adm_index;
		replica_weapon_action_source(*shooter_row, world, ev.flags, source);
		round.source_state = &source;
		// The adm-arm action sounds play at the SHOOTER's position, and a
		// decoded remote shooter has no local entity — supply its row
		// position for the sim's fire-sound leg (world/fire_sound.h).
		// [orig: entity+4 @ 0x4020ef; the pool resolve @ 0x42f491]
		round.shooter_pos = world::Vec3{
				static_cast<float>(shooter_row->x / kFixed16),
				static_cast<float>(shooter_row->y / kFixed16),
				static_cast<float>(shooter_row->z / kFixed16)};
		round.shooter_pos_valid = true;
	}
	round.origin.set_q16({ev.origin_x, ev.origin_y, ev.origin_z}); // the wire's words (D-NET-507)
	round.dir_yaw_bam = ev.dir_yaw_bam;
	round.dir_pitch_bam = ev.dir_pitch_bam;
	round.ammo_index = round_ammo;
	round.adm_index = ev.adm_index;
	round.no_weapon = ammo_arm;
	round.shot_seq = ev.shot_seq;
	round.subtype = ev.subtype;
	round.charge = ev.slot_byte;
	// Carry the arm through so presentation can honour retail's split: the
	// adm-indexed arm spawns no ammo-def effect at the wire position (which is
	// the shooter's EYE — Position + CameraOffset), it executes the addressed
	// def's action rows at the weapon's own userpoint instead.
	// [orig: @0x42f521 / @0x42f6ce]
	round.wire_round_flags = ev.flags;
	world.round_sim.spawn(
			world, round, world::RoundConsequenceMode::VisualOnly);

}

void JoinerRole::apply_weather_sample() {
	mission::MissionKernel &kernel = *kernel_;
	world::World &world = kernel.world;
	world::LocalPlayer &lp = kernel.local;
	ClientRuntime &rt = *runtime;
	// Each newly received phase-2 ENV sub-block lands in the weather home's
	// TARGET globals exactly once; the local currents keep chasing
	// [orig: NapiNPClientMsg_0x00A case 2 @ 0x430244..0x43034c].
	const replication::ClientEnvironmentState &environment = rt.state().environment;
	if (!environment.present || environment.revision == weather_revision_seen_) return;
	weather_revision_seen_ = environment.revision;
	world::WeatherWireSample sample;
	sample.fog_dist = environment.fog_dist;
	sample.fog_accel = environment.fog_accel;
	sample.tod_fixed = environment.tod_fixed;
	sample.quake_ticks = environment.quake_ticks;
	sample.cloud_scroll = environment.cloud_scroll;
	sample.rain_pct = environment.rain_pct;
	sample.overcast = environment.overcast;
	sample.precipitation_kind = environment.env_param;
	world.weather.apply_wire_sample(sample);
}

} // namespace opennova::inmatch
