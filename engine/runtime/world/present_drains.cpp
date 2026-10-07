// The per-tick presentation drain fills (present_drains.h): the world's
// outputs folded into the typed rows the present passes read. Pushed down from
// the Godot binding (ADR 0040 ladder E0): every fill reads world state only
// and the drains clear the ring they consumed.
#include <runtime/world/present_drains.h>

#include <base/io/bam.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/angle.h>
#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/throwables.h>
#include <runtime/world/vehicle_motor.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdint>
#include <utility>

namespace opennova::world {

void fill_throwable_visual_rows(const World &world, std::vector<ThrowableVisualRow> &r_rows) {
	r_rows.clear();
	const double kDegPerBam = kDegreesPerBam;
	auto push_entry = [&](int64_t key, int item_id, const Vec3 &pos,
			int32_t yaw_bam, int32_t pitch_bam, int32_t roll_bam,
			const char *move_effect, bool move_effect_live) {
		ThrowableVisualRow d;
		d.key = key;
		d.item_id = item_id;
		d.pos = pos;
		// the placer euler convention: rotation_deg = (pitch, MISSION yaw, roll)
		d.pitch_deg = static_cast<float>(double(pitch_bam) * kDegPerBam);
		d.yaw_deg = static_cast<float>(mission_yaw_deg_from_bam_heading(yaw_bam));
		d.roll_deg = static_cast<float>(double(roll_bam) * kDegPerBam);
		// effects_table tag 1 ("move") is a round-bound particle, not an
		// impact. Retail copies it to AmmoDef+0x70 [orig: @0x409fc2],
		// spawns/updates it through round+0x1cc [orig:
		// @0x4e9f58/@0x4ea8ae/@0x5f7410], then releases it with the round
		// [orig: Projectile_ReleaseEffects @0x4e8280].
		d.move_effect = move_effect != nullptr ? move_effect : "";
		// The round's emitter liveness (the +0x1CC handle mirror): the shell
		// spawns while this is set and holds no handle, and retires + forgets
		// the handle when it clears, so a round that dips under water releases
		// its plume and re-acquires one on surfacing [orig:
		// Projectile_UpdatePhysics @0x4ea019..0x4ea03e, the lazy spawn
		// @0x4e9f58..0x4e9f94; see docs/world/world-wac-ai-re.md].
		d.move_effect_live = move_effect_live;
		r_rows.push_back(std::move(d));
	};
	for (int i = 0; i < RoundSim::kCapacity; ++i) {
		const LiveRound &r = world.round_sim.rounds[static_cast<size_t>(i)];
		if (!r.active) continue;
		const AmmoTableEntry *ammo = world.tables.ammo.by_index(r.ammo_index);
		const char *move_effect = ammo != nullptr ? ammo->impact_effects[1].effect.c_str() : "";
		// TrcrID still binds the round's item callbacks on non-tracer shots,
		// but @0x4ec900 clears their visible model pointer. The tag-1 move
		// effect is independent of that presentation gate and can remain live
		// even when no item model is drawn.
		const int32_t visible_item = round_visible_item_id(r);
		if (visible_item == 0 && (move_effect == nullptr || move_effect[0] == '\0'))
			continue;
		// 512 pool slots need nine bits. Keep a tenth low bit spare and put
		// the monotonic lifetime above it so a same-slot replacement cannot
		// inherit the outgoing round's model/effect group.
		const uint64_t generation =
				r.presentation_generation != 0 ? r.presentation_generation : 1;
		const int64_t presentation_key =
				static_cast<int64_t>((generation << 10) | static_cast<uint64_t>(i));
		push_entry(presentation_key, visible_item, r.pos, r.yaw_bam, r.pitch_bam, r.roll_bam,
				move_effect, r.move_effect_live);
	}
	uint8_t viewer_team = 0xFF;
	if (const Entity *lp = world.registry.get(world.cached.local_player))
		viewer_team = static_cast<uint8_t>(lp->team);
	for (const PlacedDevice &d : world.throwables.devices) {
		if (!d.active) continue;
		// Viewer-side team variant with the retail base/friendly fallback when
		// no foe TrcrID is authored [orig: @ 0x5469db..0x546a15].
		const int item = throwable_item_for_viewer(d.item_friendly, d.item_enemy, d.team, viewer_team);
		if (item == 0) continue;
		const int64_t device_key = 0x4000000000000000LL | static_cast<int64_t>(d.entity.packed);
		push_entry(device_key, item, d.pos, d.yaw_bam, d.pitch_bam, d.roll_bam, "", false);
	}
}

void fill_vehicle_trail_visual_rows(const World &world, std::vector<VehicleTrailVisualRow> &r_rows) {
	r_rows.clear();
	world.registry.for_each([&](const Entity &entity) {
		if (!entity.alive || entity.veh.movement_effects_disabled ||
				((entity.flags | entity.engine_flags) & kEntityFlagDead) != 0)
			return;
		const VehicleTraits *traits = world.vehicles.traits.get(entity.item_id);
		if (traits == nullptr)
			return;
		for (uint8_t i = 0; i < 16; ++i) {
			const VehicleTrailPoint &point = entity.veh.trails.points[i];
			if (point.definition == 0 || point.definition > 4)
				continue;
			VehicleTrailVisualRow row;
			row.handle_packed = entity.handle.packed;
			row.registry_spawn_id = entity.registry_spawn_id;
			row.point = i;
			row.source_tick = point.source_tick;
			row.pos = point.position;
			row.dir = point.direction;
			row.effect = traits->trails[point.definition - 1].effect;
			row.magnitude_q16 = point.magnitude_q16;
			r_rows.push_back(std::move(row));
		}
	});
}

// Drain the round impacts the flight sim resolved since the last call, each row already
// resolved through the ammo effects_table (canonical tag -> {effect, sound}) and its
// per-leg presentation mask; rows with no enabled authored leg are dropped, matching
// the original impact presenter [orig: AmmoDef_ProcessImpactEffect @ 0x40a170;
// the physical handlers that call it are listed on world/round_sim.h RoundImpact,
// with the selection witness].
void drain_round_impact_rows(World &world, std::vector<RoundImpactPresentation> &r_rows) {
	r_rows.clear();
	const uint32_t now = world.logic_tick;
	for (const RoundImpact &imp : world.round_sim.impacts) {
		const AmmoTableEntry *ammo = world.tables.ammo.by_index(imp.ammo_index);
		if (ammo == nullptr || imp.effect_tag < 0) continue;
		// The ammo's row of the tag, else ammo def 0's bank at its place (a direct
		// reader's own row) [orig: AmmoDef_ProcessImpactEffect @0x40a1b8..0x40a1fd].
		const ImpactRowPick row = round_impact_row(world.tables.ammo, imp);
		const bool has_effect = imp.present_effect && !row.effect.empty();
		const bool has_sound = imp.present_sound && !row.sound.empty();
		if (!has_effect && !has_sound) continue;
		RoundImpactPresentation d;
		// The rows cross in mission space; the consumer axis-maps mission
		// (x,y,z) -> Godot (x, z, -y), the get_local_player_position convention.
		d.position = imp.position;
		d.direction = imp.direction;
		if (has_effect) d.effect = row.effect;
		if (has_sound) d.sound = row.sound;
		// A lifecycle rewind must never turn a future/stale source tick into an
		// unsigned multi-billion-tick particle pre-age request.
		d.age_ticks = now >= imp.tick ? now - imp.tick : 0u;
		d.source_tick = imp.tick;
		d.source_order = imp.source_order;
		d.section_tagged = imp.section_tagged;
		// The impact flash light rides the effect leg's own gate — retail
		// requires the effect entry AND the ammo light_impact radius (the
		// witness map on renderer/light_scene.h).
		if (has_effect && ammo->light_impact_radius > 0.0f) {
			d.has_light = true;
			d.light_radius = ammo->light_impact_radius;
			d.light_color_rgb24 = ammo->light_impact_color;
			d.light_ticks = ammo->light_impact_ticks;
		}
		r_rows.push_back(std::move(d));
	}
	world.round_sim.impacts.clear();
}

// The fire-presentation drain. Direction math mirrors the round spawn's
// mission-frame forward (cos yaw * cp, sin yaw * cp, sin pitch)
// [orig: RoundData_SpawnRound @0x4ec5e9]; the rows cross in the mission frame
// and the consumer axis-maps mission -> its device frame.
void drain_fire_presentation_rows(World &world, std::vector<FirePresentationRow> &r_rows) {
	r_rows.clear();
	const bool have_local = world.cached.local_player.valid();
	for (const FireEvent &fe : world.round_sim.fired) {
		FirePresentationRow d;
		d.origin = fe.origin;
		// Retail's two receive arms are mutually exclusive and present differently.
		// Bit 0 is tested first; only when it is CLEAR and bit 1 is set does the
		// adm-indexed arm run, and that arm spawns no ammo-def sound or effect.
		// A zero flags byte is host/AI-originated fire, which keeps the ammo-def
		// legs because retail presents those inline at the shooter instead.
		// [orig: @0x42f521 / @0x42f6ce; ammo legs @0x42f5dc / @0x42f6c2]
		d.adm_arm = (fe.wire_round_flags & round_event_flag::kAltFire) == 0 &&
				(fe.wire_round_flags & round_event_flag::kAdmIndexed) != 0;
		d.adm_index = fe.adm_index;
		const double bearing = static_cast<double>(fe.yaw_bam) * io::kRadiansPerBam;
		const double pitch = static_cast<double>(fe.pitch_bam) * io::kRadiansPerBam;
		const double cp = std::cos(pitch);
		d.forward = Vec3{static_cast<float>(std::cos(bearing) * cp),
				static_cast<float>(std::sin(bearing) * cp), static_cast<float>(std::sin(pitch))};
		d.shooter_handle = static_cast<int32_t>(fe.shooter_handle);
		const Entity *shooter = world.registry.get(fe.shooter);
		d.source_bms_id = shooter != nullptr ? shooter->bms_id : 0;
		d.is_local_player = have_local && fe.shooter == world.cached.local_player;
		d.ammo_index = fe.ammo_index;
		const AmmoTableEntry *ammo = world.tables.ammo.by_index(fe.ammo_index);
		if (ammo) d.effect = ammo->ai_launch_effect;
		d.mf_light = ammo ? ammo->mf_light : 0;
		// The SOUND legs of both arms moved onto the sim's logic clock with the
		// propagation-delay queue (world/fire_sound.h; drain_fire_sounds) — this
		// drain carries only the EFFECT legs.
		// The adm arm's replacement leg: retail executes the ADDRESSED def's action
		// rows instead of the ammo-def pair, and the FIRE row (slot 2) is the one that
		// carries the muzzle flash — its effect is the only one that can reach the
		// muzzle-glow leg, which retail gates on the action context being 2.
		// The row index needs no mapping: retail's per-def action array is 12 slots at
		// def+676 in the order of the suffix table, so def+684 IS slot 2, and our
		// weapon_action::kFire is the same ordinal.
		// [orig: array base/stride @0x54203d/@0x542231, bound @0x542239; suffix table
		//  g_WeaponActionTable @0x830B90; the +684 call @0x42f777/@0x42f98f; the glow
		//  gate @0x40205e/@0x402080 with the context stamped 2 @0x42f8a0]
		const WeaponTableEntry *fired_def = world.tables.weapons.by_index(fe.adm_index);
		const WeaponFsmAction *fire_row =
				fired_def != nullptr ? &fired_def->action_fsm.actions[weapon_action::kFire] : nullptr;
		if (fire_row) d.action_effect = fire_row->particle;
		// Resolved against the THIRD-PERSON model (gfx3): ActionDef+57 is the gfx3
		// userpoint index and +56 the gfx1 one — the opposite way round from three
		// currently-tracked doc lines. [orig: loader @0x54506c/@0x545092, resolver
		//  @0x54039e/@0x54040f]
		if (fire_row) d.action_userpoint = fire_row->particle_userpoint;
		// The 3P adm-arm anchor is the SHELL's: the rendered held-weapon node's
		// own userpoint (EntityPresenter.muzzle_world_for), which is what retail
		// spawns at — the muzzle-authority decision that closed the S12a
		// sim-posed shadow seam. The event carries the row's userpoint name; the
		// presentation layer resolves it against the node it renders.
		r_rows.push_back(std::move(d));
	}
	world.round_sim.fired.clear();
}

// The live death-piece pool snapshot — the present pass renders each piece as
// its single husk-model section [orig: the piece render mask piece[31]; §24].
void fill_death_pieces(const World &world, std::vector<DeathPieceRow> &r_pieces) {
	r_pieces.clear();
	for (size_t slot = 0; slot < world.death_pieces.pieces.size(); ++slot) {
		const DeathPiece &p = world.death_pieces.pieces[slot];
		if (!p.active) continue;
		DeathPieceRow d;
		d.slot = static_cast<int32_t>(slot);
		d.generation = p.generation;
		d.item_id = p.item_id;
		d.section = static_cast<int32_t>(p.section);
		// The debris-type row names the trail effect through the ONE native
		// table (death_piece_trail_effect) [orig: g_DeathPieceTypes
		// @ 0x8404f0 +0x2C]; "" = no trail authored.
		d.type_index = static_cast<int32_t>(p.type_index);
		d.scale = p.render_scale;
		d.pos = p.pos;
		d.heading = p.heading;
		d.pitch = p.pitch;
		d.roll = p.roll;
		d.settled = p.settled;
		d.trail = p.trail;
		r_pieces.push_back(std::move(d));
	}
}

void fill_round_glows(const World &world, std::vector<RoundGlowRow> &r_rows) {
	r_rows.clear();
	for (const LiveRound &r : world.round_sim.rounds) {
		if (!r.active || r.ammo_index < 0) continue;
		const AmmoTableEntry *ammo = world.tables.ammo.by_index(r.ammo_index);
		if (ammo == nullptr || ammo->light_move_radius <= 0.0f) continue;
		RoundGlowRow d;
		d.id = r.presentation_generation;
		// The spawn rides radius/2 above the round and the per-tick follow
		// re-centers at the round position [orig: @0x4ec8d6 / @0x4eaa9f,
		// see renderer/light_scene.h].
		d.pos = r.pos;
		d.radius = ammo->light_move_radius;
		d.color_rgb24 = ammo->light_move_color;
		r_rows.push_back(std::move(d));
	}
}

} // namespace opennova::world
