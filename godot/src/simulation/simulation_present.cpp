// Simulation — presentation reads: entity/pose getters, the present-effect
// pose cache, the packed present snapshots (AI pool + client replicas), HUD views,
// and the drains (effects, fire, destruction, round impacts, tracers).
#include "simulation/simulation_internal.h"
#include "util/color_convert.h"
#include "simulation/hud_view_records.h"
#include "simulation/tracer_ribbon_frame.h" // the compiled tracer strips
#include "simulation/destruction_events.h"
#include "simulation/debug_cards.h"
#include "util/axes.h"

#include "simulation/entity_card.h" // the typed per-entity debug card (ADR 0042 d5)
#include "simulation/entity_row.h"  // one typed entity-directory row

#include <runtime/inmatch/client_replica_card.h> // the joiner's decoded replica section
#include <runtime/inmatch/minimap_markers.h> // the retained marker rows (bank walk + local restore)
#include <runtime/hud/hud_minimap_feed.h>  // the feed layout the snapshot carries
#include <runtime/inmatch/client_replica_present_projection.h> // the canonical decoded-client projection (ADR 0031)

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <runtime/replication/client_state.h> // minimap_team_argb (the ONE palette home)
#include <runtime/replication/entity_wire_bridge.h> // entity_class_of / player_wire_net_id (the host's own rows)
#include <runtime/world/zone_chain.h> // zone_chain_zone_info_byte
#include <net/npwire/ingame_decode.h> // kRoundEventFlag* (the fire-mode byte)
#include <runtime/world/minimap_footprint.h> // the OOBJ occlusion ground-slice footprint mesh
#include <runtime/world/minimap_overlay.h>   // classifier + the blip draw policy
#include <runtime/renderer/tracer_frame.h> // the styled tracer-ribbon compile
#include <runtime/world/entity.h> // kEntityFlag* (the wire state_flags byte IS entity+36 low)

using namespace sim_internal;

namespace {

// The presented mission yaw of one pool row. Retail draws entity+0x10 (the
// 32-bit engine-frame heading); the port keeps that mirror on the vehicle motor
// (VehicleMotorState::yaw_bam) and on the infantry AI row (AiEntity::heading),
// while Entity::yaw is the whole-degree mission mirror every other row carries.
double pool_present_yaw_deg(const opennova::world::Entity &e,
		const opennova::world::AiEntity *ae, opennova::EntityClass cls) {
	if (e.emplacement_parent.valid() && e.emplacement_pose_metadata_resolved)
		return static_cast<double>(e.yaw); // World::update_emplacement_attachments' exact result
	if (cls == opennova::EntityClass::Vehicle && e.veh.yaw_seeded)
		return opennova::world::mission_yaw_deg_from_bam_heading(e.veh.yaw_bam);
	if (cls == opennova::EntityClass::Infantry && ae != nullptr)
		return opennova::world::mission_yaw_deg_from_bam_heading(ae->heading);
	return static_cast<double>(e.yaw);
}


inline void write_present_section_mask(float *row, uint32_t hidden_mask) {
	row[Simulation::PF_SECTION_MASK_VALID] = 1.0f;
	row[Simulation::PF_SECTION_MASK_LO] =
			static_cast<float>(hidden_mask & 0xFFFFu);
	row[Simulation::PF_SECTION_MASK_HI] =
			static_cast<float>((hidden_mask >> 16) & 0xFFFFu);
}

} // namespace

TypedArray<ThrowableVisualRow> Simulation::get_throwable_visuals() const {
	TypedArray<ThrowableVisualRow> out;
	if (!kernel_) return out;
	const double kDegPerBam = opennova::world::kDegreesPerBam;
	auto push_entry = [&](int64_t key, int item_id, const opennova::world::Vec3 &pos,
			int32_t yaw_bam, int32_t pitch_bam, int32_t roll_bam,
			const char *move_effect, bool move_effect_live) {
		Ref<ThrowableVisualRow> d;
		d.instantiate();
		d->set_key(key);
		d->set_item_id(item_id);
		d->set_pos(mission_to_godot(pos));
		// the placer euler convention: rotation_deg = (pitch, MISSION yaw, roll)
		d->set_rotation_deg(Vector3(
				static_cast<float>(double(pitch_bam) * kDegPerBam),
				static_cast<float>(
						opennova::world::mission_yaw_deg_from_bam_heading(yaw_bam)),
				static_cast<float>(double(roll_bam) * kDegPerBam)));
		// effects_table tag 1 ("move") is a round-bound particle, not an
		// impact. Retail copies it to AmmoDef+0x70 [orig: @0x409fc2],
		// spawns/updates it through round+0x1cc [orig:
		// @0x4e9f58/@0x4ea8ae/@0x5f7410], then releases it with the round
		// [orig: Projectile_ReleaseEffects @0x4e8280].
		d->set_move_effect(String(move_effect != nullptr ? move_effect : ""));
		// The round's emitter liveness (the +0x1CC handle mirror): the shell
		// spawns while this is set and holds no handle, and retires + forgets
		// the handle when it clears, so a round that dips under water releases
		// its plume and re-acquires one on surfacing [orig:
		// Projectile_UpdatePhysics @0x4ea019..0x4ea03e, the lazy spawn
		// @0x4e9f58..0x4e9f94; see docs/world/world-wac-ai-re.md].
		d->set_move_effect_live(move_effect_live);
		out.push_back(d);
	};
	for (int i = 0; i < opennova::world::RoundSim::kCapacity; ++i) {
		const opennova::world::LiveRound &r =
				kernel_->world.round_sim.rounds[static_cast<size_t>(i)];
		if (!r.active) continue;
		const opennova::world::AmmoTableEntry *ammo =
				kernel_->world.tables.ammo.by_index(r.ammo_index);
		const char *move_effect = ammo != nullptr
				? ammo->impact_effects[1].effect.c_str()
				: "";
		// TrcrID still binds the round's item callbacks on non-tracer shots,
		// but @0x4ec900 clears their visible model pointer. The tag-1 move
		// effect is independent of that presentation gate and can remain live
		// even when no item model is drawn.
		const int32_t visible_item =
				opennova::world::round_visible_item_id(r);
		if (visible_item == 0 &&
				(move_effect == nullptr || move_effect[0] == '\0'))
			continue;
		// 512 pool slots need nine bits. Keep a tenth low bit spare and put
		// the monotonic lifetime above it so a same-slot replacement cannot
		// inherit the outgoing round's model/effect group.
		const uint64_t generation =
				r.presentation_generation != 0 ? r.presentation_generation : 1;
		const int64_t presentation_key = static_cast<int64_t>(
				(generation << 10) | static_cast<uint64_t>(i));
		push_entry(presentation_key, visible_item, r.pos, r.yaw_bam,
				r.pitch_bam, r.roll_bam,
				move_effect, r.move_effect_live);
	}
	uint8_t viewer_team = 0xFF;
	if (const opennova::world::Entity *lp =
			kernel_->world.registry.get(kernel_->world.cached.local_player))
		viewer_team = static_cast<uint8_t>(lp->team);
	for (const opennova::world::PlacedDevice &d : kernel_->world.throwables.devices) {
		if (!d.active) continue;
		// Viewer-side team variant with the retail base/friendly fallback when
		// no foe TrcrID is authored [orig: @ 0x5469db..0x546a15].
		const int item = opennova::world::throwable_item_for_viewer(
				d.item_friendly, d.item_enemy, d.team, viewer_team);
		if (item == 0) continue;
		const int64_t device_key =
				0x4000000000000000LL |
				static_cast<int64_t>(d.entity.packed);
		push_entry(device_key, item, d.pos, d.yaw_bam, d.pitch_bam,
				d.roll_bam, "", false);
	}
	return out;
}

Ref<WaypointHudView> Simulation::get_waypoint_hud_view() const {
	// The current-waypoint slice of the per-frame HUD info rebuild, plus the
	// mission-scripted show gate. [orig: HUD_BuildEntityInfo @ 0x4b88b7..0x4b8914
	// (hudInfo+373 number, +400/404/408 position) + g_showWaypoints @ 0x27238BC]
	Ref<WaypointHudView> out;
	out.instantiate();
	const opennova::world::WaypointTrack *track = kernel_ ? &kernel_->world.script.waypoints : nullptr;
	out->set_show(track != nullptr && track->show);
	out->set_count(track ? static_cast<int>(track->entries.size()) : 0);
	const opennova::world::WaypointEntry *cur = track ? track->current_entry() : nullptr;
	out->set_current(cur ? static_cast<int>(track->current) : -1);
	out->set_number(cur ? static_cast<int>(track->current) + 1 : 0);
	out->set_name_id(cur ? static_cast<int>(cur->name_id) : 0);
	// Fixed 16.16 mission (x,y,z) -> Godot (x, z, -y), like every entity read.
	out->set_position(cur ? Vector3(cur->x / 65536.0f, cur->z / 65536.0f, -(cur->y / 65536.0f))
						  : Vector3());
	out->set_done(cur != nullptr && cur->done);
	return out;
}

Ref<HudMapGridOrigin> Simulation::get_hud_map_grid_origin() const {
	// The map grid-label origin: the mission's first type-2043 marker. The
	// host stashes it at promotion from the mission doc; a JOINER promotes a
	// marker-less wire-header BMS (D-NET-194), so its origin resolves from
	// the replicated pool-3 entity in the decoded view instead — the same
	// client-side pool scan retail's HUD init runs (witness at
	// World::map_grid_origin_x / HudMinimapInput::grid_origin_x).
	Ref<HudMapGridOrigin> out;
	out.instantiate();
	bool present = kernel_ != nullptr && kernel_->world.tables.map_grid_origin_present;
	int32_t x_q16 = present ? kernel_->world.tables.map_grid_origin_x : 0;
	int32_t y_q16 = present ? kernel_->world.tables.map_grid_origin_y : 0;
	if (!present && runtime_ != nullptr) {
		present = opennova::replication::client_minimap_grid_origin(
				runtime_->state(), x_q16, y_q16);
	}
	out->set_present(present);
	out->set_position(present
			? Vector3(x_q16 / 65536.0f, 0.0f, -(y_q16 / 65536.0f))
			: Vector3());
	return out;
}

PackedInt32Array Simulation::get_hud_minimap_snapshot() const {
	const opennova::world::Entity *local_player = kernel_ != nullptr
			? kernel_->world.registry.get(kernel_->world.cached.local_player)
			: nullptr;
	const uint16_t local_marker_handle = local_player != nullptr
			? static_cast<uint16_t>(get_local_player_wire_handle())
			: opennova::world::EntityHandle::kInvalid;
	// Between 62 Hz logic ticks every input is unchanged (the retained banks
	// bump ClientMinimapState::revision; entity resolves, policies, and the
	// local row advance only with the tick), so display frames reuse the
	// built array. The baseline restore invalidates across epochs.
	const uint64_t revision =
			runtime_ ? runtime_->state().minimap.revision : 0;
	const uint64_t tick = kernel_ != nullptr
			? static_cast<uint64_t>(kernel_->world.logic_tick) : 0;
	if (minimap_snapshot_valid_ && revision == minimap_snapshot_revision_ &&
			tick == minimap_snapshot_tick_ &&
			local_marker_handle == minimap_snapshot_local_handle_) {
		return minimap_snapshot_cache_;
	}
	// The rows (the bank walk, the policy resolve, the restored local row) and
	// the feed layout are the engine's; this leg only packs the array.
	opennova::inmatch::MinimapMarkerInputs in;
	in.map = runtime_ ? &runtime_->state().minimap : nullptr;
	in.world = &kernel_->world;
	in.local_marker_handle = local_marker_handle;
	in.local_heading_bam = static_cast<int32_t>(get_local_player_heading_bam());
	std::vector<opennova::hud::HudMinimapMarker> markers;
	opennova::inmatch::build_minimap_markers(in, markers);
	std::vector<int32_t> feed;
	opennova::hud::minimap_feed_encode(markers, feed);
	PackedInt32Array out;
	out.resize(static_cast<int64_t>(feed.size()));
	std::copy(feed.begin(), feed.end(), out.ptrw());
	minimap_snapshot_cache_ = out;
	minimap_snapshot_revision_ = revision;
	minimap_snapshot_tick_ = tick;
	minimap_snapshot_local_handle_ = local_marker_handle;
	minimap_snapshot_valid_ = true;
	return out;
}

PackedInt32Array Simulation::get_hud_minimap_footprints() const {
	// Static footprint polygons for every visible footprint-class entity:
	// the OOBJ occlusion ground-slice mesh transformed by the entity pose, in
	// the witnessed fill colors and the overlay ctx alpha (witness at
	// world::minimap_footprint_fill_argb) with the occlusion ground-slice
	// mesh placed by the entity pose (world::minimap_footprint_place).
	PackedInt32Array out;
	out.push_back(1); // feed version
	out.push_back(0); // row count, patched below
	if (!kernel_) return out;
	int count = 0;
	std::unordered_map<int32_t, opennova::world::MinimapFootprintMesh> meshes;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &entity) {
		if (!opennova::world::minimap_overlay_entity_enabled(entity)) return;
		const opennova::world::MinimapOverlayClassification row =
				opennova::world::classify_minimap_overlay(entity);
		if (!row.visible) return;
		const opennova::world::MinimapBlipDrawPolicy policy =
				opennova::world::minimap_blip_draw_policy(entity, row.icon);
		if (!policy.footprint) return;
		const int32_t model_id = kernel_->occlusion.instance_model_id(
				entity.handle);
		if (model_id < 0) return;
		auto mesh_it = meshes.find(model_id);
		if (mesh_it == meshes.end()) {
			const opennova::world::OcclusionModel *model =
					kernel_->occlusion.model(model_id);
			if (model == nullptr) return;
			mesh_it = meshes.emplace(model_id,
					opennova::world::minimap_footprint_from_occlusion(
							*model)).first;
		}
		const opennova::world::MinimapFootprintMesh &mesh = mesh_it->second;
		if (mesh.empty()) return;
		// Color + placement live engine-side (world::minimap_footprint_*):
		// the same yaw-degree -> BAM placement matrix the collision instance
		// uses, so footprint, model, and shell agree.
		std::vector<int32_t> fill_xy;
		std::vector<int32_t> edge_xy;
		opennova::world::minimap_footprint_place(mesh, entity, fill_xy,
				edge_xy);
		out.push_back(entity.handle.packed);
		out.push_back(static_cast<int32_t>(
				opennova::world::minimap_footprint_fill_argb(entity)));
		out.push_back(static_cast<int32_t>(fill_xy.size()));
		for (const int32_t value : fill_xy) out.push_back(value);
		out.push_back(static_cast<int32_t>(edge_xy.size()));
		for (const int32_t value : edge_xy) out.push_back(value);
		++count;
	});
	out.set(1, count);
	return out;
}

TypedArray<ObjectiveRow> Simulation::get_objectives_view() const {
	// The SP objectives panel's row walk: slots 1..8 until a 0/255 win id.
	// [orig: HUD_DrawWinConditions @0x5ba9e0 — byte_A7628B[slot] 0/255 break;
	//  row gate = show-win bit @0x5ba9ff; checkmark = won bit @0x5bab35]
	TypedArray<ObjectiveRow> out;
	if (!kernel_) return out;
	const auto &sg = kernel_->world.script.subgoals;
	for (int slot = 1; slot <= 8; ++slot) {
		const uint8_t id = sg.win_text_ids[slot];
		if (id == 0 || id == 255) break;
		Ref<ObjectiveRow> row;
		row.instantiate();
		row->set_slot(slot);
		row->set_text_id(static_cast<int>(id));
		row->set_shown((sg.show_win & (1u << slot)) != 0);
		row->set_done((sg.won & (1u << slot)) != 0);
		out.push_back(row);
	}
	return out;
}
// Drain the round impacts the flight sim resolved since the last call, each row already
// resolved through the ammo effects_table (canonical tag -> {effect, sound}) and its
// per-leg presentation mask; rows with no enabled authored leg are dropped, matching
// the original impact presenter [orig: AmmoDef_ProcessImpactEffect @ 0x40a170;
// ballistic wrapper Projectile_SpawnImpactEffect @ 0x4e9b80; selection witness
// on world/round_sim.h RoundImpact].
TypedArray<RoundImpactRow> Simulation::drain_round_impacts() {
	TypedArray<RoundImpactRow> out;
	if (!kernel_) return out;
	const uint32_t now = kernel_->world.logic_tick;
	for (const opennova::world::RoundImpact &imp : kernel_->world.round_sim.impacts) {
		const opennova::world::AmmoTableEntry *ammo = kernel_->world.tables.ammo.by_index(imp.ammo_index);
		if (ammo == nullptr) continue;
		if (imp.effect_tag < 0 || imp.effect_tag >= opennova::world::kImpactEffectTagCount)
			continue;
		const opennova::world::AmmoImpactEffectRow &row = ammo->impact_effects[imp.effect_tag];
		const bool has_effect = imp.present_effect && !row.effect.empty();
		const bool has_sound = imp.present_sound && !row.sound.empty();
		if (!has_effect && !has_sound) continue;
		Ref<RoundImpactRow> d;
		d.instantiate();
		// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position convention.
		d->set_position(mission_to_godot(imp.position));
		d->set_direction(mission_to_godot(imp.direction));
		d->set_effect(has_effect ? String::utf8(row.effect.c_str()) : String());
		d->set_sound(has_sound ? String::utf8(row.sound.c_str()) : String());
		// A lifecycle rewind must never turn a future/stale source tick into an
		// unsigned multi-billion-tick particle pre-age request.
		const uint32_t age_ticks = now >= imp.tick ? now - imp.tick : 0u;
		d->set_age_ticks(static_cast<int64_t>(age_ticks));
		d->set_source_tick(static_cast<int64_t>(imp.tick));
		d->set_source_order(static_cast<int64_t>(imp.source_order));
		// The impact flash light rides the effect leg's own gate — retail
		// requires the effect entry AND the ammo light_impact radius (the
		// witness map on renderer/light_scene.h).
		if (has_effect && ammo->light_impact_radius > 0.0f) {
			d->set_has_light(true);
			d->set_light_radius(ammo->light_impact_radius);
			d->set_light_color(opennova::color_from_rgb24(ammo->light_impact_color));
			d->set_light_ticks(ammo->light_impact_ticks);
		}
		out.push_back(d);
	}
	kernel_->world.round_sim.impacts.clear();
	return out;
}

TypedArray<TerrainScorchRow> Simulation::drain_terrain_scorches() {
	TypedArray<TerrainScorchRow> out;
	if (!kernel_) return out;
	for (const opennova::world::TerrainScorchEvent &event :
			kernel_->world.out.terrain_scorches.pending()) {
		const opennova::terrain::TerrainScorchEntry &mission =
				event.mission_bounds;
		Ref<TerrainScorchRow> row;
		row.instantiate();
		row->set_texture_index(static_cast<int64_t>(mission.texture_index));
		row->set_minimum_x_q16(static_cast<int64_t>(mission.minimum_x_q16));
		row->set_maximum_x_q16(static_cast<int64_t>(mission.maximum_x_q16));
		// mission (x,y,z) -> Godot (x,z,-y): negation swaps the ordered
		// extrema on the second ground-plane axis.
		row->set_minimum_z_q16(-static_cast<int64_t>(mission.maximum_z_q16));
		row->set_maximum_z_q16(-static_cast<int64_t>(mission.minimum_z_q16));
		row->set_source_tick(static_cast<int64_t>(event.tick));
		row->set_source_order(static_cast<int64_t>(event.source_order));
		out.push_back(row);
	}
	kernel_->world.out.terrain_scorches.clear_pending();
	return out;
}

TypedArray<MissionEffect> Simulation::drain_effects() {
	TypedArray<MissionEffect> out;
	if (!world_installed_) return out;
	for (const opennova::world::Effect &e : kernel_->world.out.effects.entries()) {
		Ref<MissionEffect> d = MissionEffect::make(String(e.kind.c_str()), e.a, e.b, e.c,
				String(e.str.c_str()));
		d->set_d(e.d);
		if (e.kind == "vehicle_control_started" ||
				e.kind == "vehicle_control_stopped")
			d->set_wire_handle(e.d);
		out.push_back(d);
	}
	kernel_->world.out.effects.clear();
	return out;
}

// The shell fire-presentation drain — see the header note. Direction math mirrors
// the round spawn's mission-frame forward (cos yaw * cp, sin yaw * cp, sin pitch)
// [orig: RoundData_SpawnRound @0x4ec5e9], axis-mapped mission -> godot (x, z, -y).
TypedArray<FirePresentationEvent> Simulation::drain_fire_presentation_events() {
	TypedArray<FirePresentationEvent> out;
	if (!world_installed_) return out;
	constexpr double kRadPerBam = (2.0 * 3.14159265358979323846) / 4294967296.0;
	const bool have_local = kernel_->world.cached.local_player.valid();
	for (const opennova::world::FireEvent &fe : kernel_->world.round_sim.fired) {
		Ref<FirePresentationEvent> d;
		d.instantiate();
		d->set_origin(mission_to_godot(fe.origin));
		// Retail's two receive arms are mutually exclusive and present differently.
		// Bit 0 is tested first; only when it is CLEAR and bit 1 is set does the
		// adm-indexed arm run, and that arm spawns no ammo-def sound or effect.
		// A zero flags byte is host/AI-originated fire, which keeps the ammo-def
		// legs because retail presents those inline at the shooter instead.
		// [orig: @0x42f521 / @0x42f6ce; ammo legs @0x42f5dc / @0x42f6c2]
		d->set_adm_arm((fe.wire_round_flags & opennova::kRoundEventFlagAltFire) == 0 &&
				(fe.wire_round_flags & opennova::kRoundEventFlagAdmIndexed) != 0);
		d->set_adm_index(fe.adm_index);
		const double bearing = static_cast<double>(fe.yaw_bam) * kRadPerBam;
		const double pitch = static_cast<double>(fe.pitch_bam) * kRadPerBam;
		const double cp = std::cos(pitch);
		d->set_forward(Vector3(static_cast<real_t>(std::cos(bearing) * cp),
				static_cast<real_t>(std::sin(pitch)),
				static_cast<real_t>(-std::sin(bearing) * cp)));
		d->set_shooter_handle(static_cast<int>(fe.shooter_handle));
		const opennova::world::Entity *shooter = kernel_->world.registry.get(fe.shooter);
		d->set_source_bms_id(shooter != nullptr ? shooter->bms_id : 0);
		d->set_is_local_player(have_local && fe.shooter == kernel_->world.cached.local_player);
		d->set_ammo_index(fe.ammo_index);
		const opennova::world::AmmoTableEntry *ammo = kernel_->world.tables.ammo.by_index(fe.ammo_index);
		d->set_effect(ammo ? String(ammo->ai_launch_effect.c_str()) : String());
		d->set_mf_light(ammo ? ammo->mf_light : 0);
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
		//  g_weaponActionTable @0x830B90; the +684 call @0x42f777/@0x42f98f; the glow
		//  gate @0x40205e/@0x402080 with the context stamped 2 @0x42f8a0]
		const opennova::world::WeaponTableEntry *fired_def =
				kernel_->world.tables.weapons.by_index(fe.adm_index);
		const opennova::world::WeaponFsmAction *fire_row =
				fired_def != nullptr
						? &fired_def->action_fsm.actions[opennova::world::weapon_action::kFire]
						: nullptr;
		d->set_action_effect(fire_row ? String(fire_row->particle) : String());
		// Resolved against the THIRD-PERSON model (gfx3): ActionDef+57 is the gfx3
		// userpoint index and +56 the gfx1 one — the opposite way round from three
		// currently-tracked doc lines. [orig: loader @0x54506c/@0x545092, resolver
		//  @0x54039e/@0x54040f]
		d->set_action_userpoint(
				fire_row ? String(fire_row->particle_userpoint) : String());
		// The 3P adm-arm anchor is the SHELL's: the rendered held-weapon node's
		// own userpoint (WirePresentPass.muzzle_world_for), which is what retail
		// spawns at — the muzzle-authority decision that closed the S12a
		// sim-posed shadow seam. The event carries the row's userpoint name; the
		// presentation layer resolves it against the node it renders.
		out.push_back(d);
	}
	kernel_->world.round_sim.fired.clear();
	return out;
}

// world/fire_sound.h mirrors the npwire arm bits so the sim's seed can split
// the arms without linking the net stack — pin the pairing here, where both
// headers are visible (the S7a weapon_flag pattern).
static_assert(opennova::world::round_event_flag::kAltFire ==
				opennova::kRoundEventFlagAltFire,
		"round_event_flag::kAltFire must match the npwire decoder bit");
static_assert(opennova::world::round_event_flag::kAdmIndexed ==
				opennova::kRoundEventFlagAdmIndexed,
		"round_event_flag::kAdmIndexed must match the npwire decoder bit");

// The presenting shell's listener stamp — the camera position, once per frame
// before the tick batch, feeding the sim's fire-sound distance gate
// [orig: listener_pos @ 0x24D6630; world/fire_sound.h]. Never called on a
// dedicated host, which is the witnessed peer gate.
void Simulation::set_sound_listener(const Vector3 &p_listener_godot) {
	if (!world_installed_) return;
	kernel_->world.out.fire_sounds.set_listener(opennova::world::Vec3{
			p_listener_godot.x, -p_listener_godot.z, p_listener_godot.y});
}

// The ready fire-sound drain: immediate near shots, the adm-arm action-row
// sets, and expired propagation-delayed slots, in play order on the logic
// clock. The shell plays each row positionally; the set's max-range cull
// stays at play time in the audio bank (D-AI-8).
// [orig: Entity_PlaySound3D_FullVolume @ 0x528e20 / the pending drain
//  Sound_TickPendingSlots @ 0x529310]
TypedArray<FireSoundRow> Simulation::drain_fire_sounds() {
	TypedArray<FireSoundRow> out;
	if (!world_installed_) return out;
	for (const opennova::world::ReadyFireSound &sound :
			kernel_->world.out.fire_sounds.drain()) {
		Ref<FireSoundRow> d;
		d.instantiate();
		d->set_soundset(String(sound.set_name.c_str()));
		d->set_pos(mission_to_godot(sound.pos));
		d->set_source_bms_id(sound.source_bms_id);
		out.push_back(d);
	}
	return out;
}

// The destruction presentation drain (world/destruction.h; §24): one call per
// present, converting the sim's events into godot-space dictionaries. Mission
// (x, y, z-up) -> Godot (x, z, -y), the drain_fire_presentation_events rule.
Ref<DestructionDrain> Simulation::drain_destruction_events() {
	if (!world_installed_) return Ref<DestructionDrain>();
	opennova::world::DestructionEvents &ev = kernel_->world.out.destruction;
	Ref<DestructionDrain> out;
	out.instantiate();
	for (const opennova::world::DestructionEffectEvent &e : ev.effects) {
		out->add_effect(DestructionEffectEvent::make(String(e.effect.c_str()),
				mission_to_godot(e.pos), static_cast<int>(e.family), mission_to_godot(e.dir),
				static_cast<int>(e.attach_net_id), e.attach_bms_id,
				static_cast<int>(e.attach_wire_handle),
				static_cast<int64_t>(e.attach_spawn_origin)));
	}
	for (const opennova::world::DestructionSoundEvent &s : ev.sounds) {
		out->add_sound(DestructionSoundEvent::make(String(s.sound.c_str()), mission_to_godot(s.pos)));
	}
	for (const opennova::world::HuskSwapEvent &h : ev.husk_swaps) {
		Ref<HuskSwapEvent> row = HuskSwapEvent::make(h.bms_id, h.item_id,
				static_cast<int64_t>(h.spawn_origin), static_cast<int>(h.wire_handle));
		row->set_net_id(static_cast<int>(h.net_id));
		row->set_spawned_piece_mask(static_cast<int64_t>(h.spawned_piece_mask));
		row->set_pos(mission_to_godot(h.pos));
		out->add_husk_swap(row);
	}
	for (const opennova::world::DeathLightEvent &l : ev.death_lights) {
		out->add_death_light(DeathLightEvent::make(mission_to_godot(l.pos), l.radius));
	}
	out->set_explosions_processed(ev.explosions_processed);
	out->set_items_destroyed(ev.items_destroyed);
	out->set_crackles(ev.crackles); // wreck-fire crackle rolls fired (S12b)
	out->set_debris_triangles(ev.debris_triangles);
	out->set_glass_points(ev.glass_points);
	ev.clear();
	return out;
}

// The live death-piece pool snapshot — the present pass renders each piece as
// its single husk-model section [orig: the piece render mask piece[31]; §24].
TypedArray<DeathPieceRow> Simulation::get_death_pieces() const {
	TypedArray<DeathPieceRow> out;
	if (!world_installed_) return out;
	for (size_t slot = 0; slot < kernel_->world.death_pieces.pieces.size(); ++slot) {
		const opennova::world::DeathPiece &p = kernel_->world.death_pieces.pieces[slot];
		if (!p.active) continue;
		Ref<DeathPieceRow> d;
		d.instantiate();
		d->set_slot(static_cast<int>(slot));
		d->set_generation(static_cast<int64_t>(p.generation));
		d->set_item_id(p.item_id);
		d->set_section(static_cast<int>(p.section));
		d->set_type_index(static_cast<int>(p.type_index));
		// The debris-type trail effect, from the ONE native table [orig:
		// g_death_piece_types @ 0x8404f0 +0x2C]; "" = no trail authored.
		d->set_trail(String(
				opennova::world::death_piece_trail_effect(p.type_index)));
		d->set_scale(p.render_scale);
		d->set_pos(mission_to_godot(p.pos));
		d->set_heading(p.heading);
		d->set_pitch(p.pitch);
		d->set_settled(p.settled);
		out.push_back(d);
	}
	return out;
}

// Per-entity destruction diagnostics (probe/F3 seam): the gate inputs the
// damage chain reads, resolved by bms_id. {} = no such entity.
Ref<DestructionDebugCard> Simulation::get_destruction_debug(int p_bms_id) const {
	Ref<DestructionDebugCard> out;
	out.instantiate();
	if (!kernel_) return out;
	const opennova::world::Entity *found = nullptr;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (found == nullptr && e.bms_id == p_bms_id) found = &e;
	});
	if (found == nullptr) return out;
	out->set_found(true);
	out->set_bms_id(found->bms_id);
	out->set_net_id(static_cast<int>(found->net_id));
	out->set_kind(static_cast<int>(found->kind));
	out->set_pool(found->handle.pool());
	out->set_item_id(found->item_id);
	out->set_health(static_cast<int>(found->health));
	out->set_health_max(static_cast<int>(found->health_max));
	out->set_alive(found->alive);
	out->set_bound_radius(static_cast<float>(found->bound_radius));
	out->set_engine_flags(static_cast<int64_t>(found->engine_flags));
	out->set_is_ai_capable(found->is_ai_capable);
	out->set_has_collision_instance(
			kernel_->collision.has_instance(kernel_->world, found->handle));
	const opennova::world::ItemDeathTraits *t =
			kernel_->world.tables.item_death_traits.get(found->item_id);
	out->set_has_death_traits(t != nullptr);
	if (t != nullptr) {
		out->set_armor_impact(static_cast<int>(t->armor_impact));
		out->set_armor_blast(static_cast<int>(t->armor_blast));
		out->set_unit_type(static_cast<int>(t->unit_type));
		out->set_kz(static_cast<int>(t->kz));
		out->set_has_husk(t->has_husk);
		out->set_husk_model_loaded(t->husk_model_loaded);
		PackedVector3Array kz_points;
		for (const opennova::world::Vec3 &point : t->kz_points)
			kz_points.push_back(Vector3(point.x, point.y, point.z));
		out->set_kz_point_count(static_cast<int>(t->kz_points.size()));
		out->set_kz_points(kz_points);
		PackedVector3Array bridge_dead_points;
		for (const opennova::world::Vec3 &point : t->bridge_dead_points)
			bridge_dead_points.push_back(Vector3(point.x, point.y, point.z));
		out->set_bridge_dead_point_count(static_cast<int>(t->bridge_dead_points.size()));
		out->set_bridge_dead_points(bridge_dead_points);
		PackedVector3Array glass_positions;
		PackedVector3Array glass_directions;
		for (const opennova::world::GlassPointTrait &point : t->glass_points) {
			glass_positions.push_back(Vector3(
					point.local_pos.x, point.local_pos.y, point.local_pos.z));
			glass_directions.push_back(Vector3(
					point.local_dir.x, point.local_dir.y, point.local_dir.z));
		}
		out->set_glass_point_count(static_cast<int>(t->glass_points.size()));
		out->set_glass_point_positions(glass_positions);
		out->set_glass_point_directions(glass_directions);
	}
	out->set_pos(mission_to_godot(found->position));
	return out;
}

// The live tracer trail channels for the ribbon layer — see the header note.
// Mission -> godot axis map (x, z, -y), matching the other presentation drains.
PackedFloat32Array Simulation::get_tracer_trails() const {
	PackedFloat32Array out;
	if (!world_installed_) return out;
	for (const opennova::world::TracerTrailChannel &c : kernel_->world.round_sim.trails.channels) {
		if (!c.active || c.count <= 0) continue;
		const int64_t base = out.size();
		out.resize(base + 3 + static_cast<int64_t>(c.count) * 4);
		float *w = out.ptrw() + base;
		w[0] = static_cast<float>(c.style_id);
		w[1] = static_cast<float>(c.age);
		w[2] = static_cast<float>(c.count);
		float *pw = w + 3;
		for (int i = 0; i < c.count; ++i, pw += 4) {
			const opennova::world::TracerTrailPoint &p = c.pts[static_cast<size_t>(i)];
			pw[0] = p.pos.x;
			pw[1] = p.pos.z;
			pw[2] = -p.pos.y;
			pw[3] = p.w;
		}
	}
	return out;
}

// The in-flight round glows — see the header note. One row per active round
// whose ammo authors `light_move`; the id is the round's presentation
// generation so pool-slot reuse never teleports a glow.
TypedArray<RoundGlowRow> Simulation::get_round_glow_rows() const {
	TypedArray<RoundGlowRow> out;
	if (!world_installed_) return out;
	for (const opennova::world::LiveRound &r : kernel_->world.round_sim.rounds) {
		if (!r.active || r.ammo_index < 0) continue;
		const opennova::world::AmmoTableEntry *ammo =
				kernel_->world.tables.ammo.by_index(r.ammo_index);
		if (ammo == nullptr || ammo->light_move_radius <= 0.0f) continue;
		Ref<RoundGlowRow> d;
		d.instantiate();
		d->set_id(static_cast<int64_t>(r.presentation_generation));
		// The spawn rides radius/2 above the round and the per-tick follow
		// re-centers at the round position [orig: @0x4ec8d6 / @0x4eaa9f,
		// see renderer/light_scene.h].
		d->set_pos(mission_to_godot(r.pos));
		d->set_radius(ammo->light_move_radius);
		d->set_color(opennova::color_from_rgb24(ammo->light_move_color));
		out.push_back(d);
	}
	return out;
}

// The styled half of the trail split: rows in, per-family strip runs out.
// Family packing (positions + colors arrays) is transport shape only; the
// geometry/color math lives in renderer/tracer_frame.cpp with its citations.
Ref<TracerRibbonFrame> Simulation::compile_tracer_ribbons(const PackedFloat32Array &rows,
		const Vector3 &camera) {
	std::vector<opennova::renderer::TracerChannelInput> channels;
	const float *r = rows.ptr();
	const int64_t size = rows.size();
	int64_t i = 0;
	while (r != nullptr && i + 2 < size) {
		opennova::renderer::TracerChannelInput c;
		c.style_id = static_cast<int>(r[i]);
		c.age = static_cast<int>(r[i + 1]);
		c.count = static_cast<int>(r[i + 2]);
		i += 3;
		if (c.count <= 0 || i + static_cast<int64_t>(c.count) * 4 > size) {
			break;
		}
		c.points = r + i;
		i += static_cast<int64_t>(c.count) * 4;
		channels.push_back(c);
	}
	opennova::renderer::TracerRibbonFrame frame;
	opennova::renderer::compile_tracer_ribbons(channels.data(), channels.size(),
			{static_cast<float>(camera.x), static_cast<float>(camera.y),
					static_cast<float>(camera.z)},
			frame);
	auto pack_family = [](const std::vector<float> &run) {
		Ref<TracerRibbonStrip> family;
		family.instantiate();
		const int64_t verts = static_cast<int64_t>(run.size() / 7);
		PackedVector3Array positions;
		PackedColorArray colors;
		positions.resize(verts);
		colors.resize(verts);
		Vector3 *pw = positions.ptrw();
		Color *cw = colors.ptrw();
		for (int64_t v = 0; v < verts; ++v) {
			const float *f = run.data() + v * 7;
			pw[v] = Vector3(f[0], f[1], f[2]);
			cw[v] = Color(f[3], f[4], f[5], f[6]);
		}
		family->set_positions(positions);
		family->set_colors(colors);
		return family;
	};
	Ref<TracerRibbonFrame> out;
	out.instantiate();
	out->set_additive(pack_family(frame.additive));
	out->set_alpha(pack_family(frame.alpha));
	out->set_channels(frame.channels);
	return out;
}

// The typed entity inspection API (ADR 0042 d5): the directory join and the
// per-entity card are engine facts (world/inspect.h); this binding forwards
// and converts into the typed records. The joiner's decoded replica section
// is the npruntime card (runtime/inmatch/client_replica_card.h).
TypedArray<EntityRow> Simulation::entity_directory() const {
	TypedArray<EntityRow> out;
	if (!kernel_) return out;
	const std::vector<opennova::world::inspect::EntityRow> rows = native_entity_directory();
	for (const opennova::world::inspect::EntityRow &row : rows) {
		Ref<EntityRow> typed;
		typed.instantiate();
		typed->assign(row);
		out.push_back(typed);
	}
	return out;
}

std::vector<opennova::world::inspect::EntityRow> Simulation::native_entity_directory() const {
	if (!kernel_) return {};
	// A joiner never mixes its non-authoritative tooling AI pool into the
	// decoded view; the host joins registry rows to their AI cards.
	return opennova::world::inspect::entity_directory(kernel_->world, /*with_brains=*/!joiner_);
}

opennova::world::inspect::EntityCard Simulation::native_entity_card(int p_handle) const {
	if (!kernel_ || p_handle < 0 || p_handle > 0xFFFF) return {};
	// The directory's rule: a joiner's non-authoritative tooling AI pool never
	// joins the decoded view, so the F3 card shows no AI half for a row the
	// list beside it calls brainless.
	return opennova::world::inspect::build_entity_card(
			kernel_->world, /*with_brains=*/!joiner_,
			opennova::world::EntityHandle{static_cast<uint16_t>(p_handle)},
			[this](int32_t adm_id) { return kernel_->root_motion.adm_name(adm_id); });
}

Ref<EntityCard> Simulation::entity_card(int p_handle) const {
	if (!kernel_ || p_handle < 0 || p_handle > 0xFFFF) return Ref<EntityCard>();
	const opennova::world::EntityHandle handle{static_cast<uint16_t>(p_handle)};
	Ref<EntityCard> card;
	card.instantiate();
	// The MCP/test card keeps both halves on every role (its joiner readers
	// diff the tooling pool against the replica section).
	card->assign(opennova::world::inspect::build_entity_card(
			kernel_->world, /*with_brains=*/true, handle,
			[this](int32_t adm_id) { return kernel_->root_motion.adm_name(adm_id); }));
	// The joiner's decoded replica row for the same handle, when one exists.
	if (joiner_ && runtime_ != nullptr) {
		card->assign_replica(opennova::inmatch::client_replica_card(
				runtime_->state(), handle.packed));
	}
	return card->native_valid() ? card : Ref<EntityCard>();
}

Ref<EntityCard> Simulation::entity_card_by_ai_index(int p_index) const {
	if (!kernel_) return Ref<EntityCard>();
	const AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return Ref<EntityCard>();
	return entity_card(static_cast<int>(e->handle.packed));
}

Ref<EntityCard> Simulation::entity_card_by_net_id(int p_net_id) const {
	if (!kernel_ || p_net_id <= 0 || p_net_id > 0xFFFF) return Ref<EntityCard>();
	const opennova::world::EntityHandle h = kernel_->world.registry.find_by_net_id(
			static_cast<uint16_t>(p_net_id));
	if (!h.valid()) return Ref<EntityCard>();
	return entity_card(static_cast<int>(h.packed));
}

String Simulation::ai_state_name(int p_state) {
	return String(opennova::world::ai_state_name(p_state));
}

String Simulation::infantry_anim_key(int p_state) {
	return String(opennova::world::infantry_anim_key(p_state).c_str());
}

int64_t Simulation::infantry_anim_flags(int p_state) {
	if (p_state < 0 || p_state >= opennova::world::kInfantryAnimStateCount) return 0;
	return static_cast<int64_t>(opennova::world::kInfantryAnimFlags[p_state]);
}

int Simulation::get_entity_count() const {
	return kernel_ ? kernel_->world.ai.count() : 0;
}

int Simulation::get_entity_kind(int p_index) const {
	if (!kernel_) return -1;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = kernel_->world.registry.get(e->handle);
	if (!ent) return -1;
	return opennova::world::spawn_origin_kind(ent->spawn_origin); // [orig promote: (kind<<24)|index]
}

Vector3 Simulation::get_entity_position(int p_index) const {
	if (!kernel_) return Vector3();
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return Vector3();
	// mission (x, y, z) 16.16 -> Godot (x, z, -y) world units. [orig render remap: (x, z, -y).]
	return Vector3(static_cast<float>(e->pos[0] / kFixed16),
	               static_cast<float>(e->pos[2] / kFixed16),
	               static_cast<float>(-e->pos[1] / kFixed16));
}

float Simulation::get_entity_yaw_deg(int p_index) const {
	if (!kernel_) return 0.0f;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return 0.0f;
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(e->heading));
}

int Simulation::get_entity_state(int p_index) const {
	if (!kernel_) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kCurState];
}

int Simulation::get_entity_net_id(int p_index) const {
	if (!kernel_) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	return e ? e->net_id : 0;
}

// The distant MODEL/depth-mask foliage tier is the hide-in-grass mechanic: the
// sector-entity walk only calls Foliage_UpdateModelTiles around entities whose
// MoveOrder carries a stance bit (0x100 prone / 0x200 crouch) and whose
// groundEntity is empty — never around placed objects, which leave MoveOrder 0.
// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded (flags & 0x300),
// groundEntity gate @ 0x5c7dd5..0x5c7df7; stance writers
// Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd,
// NapiNPServerMsg_HandleStanceChange @ 0x501c60]
PackedVector3Array Simulation::get_foliage_mask_anchor_positions() const {
	PackedVector3Array out;
	if (!kernel_) return out;
	for (int i = 0; i < kernel_->world.ai.count(); ++i) {
		AiEntity *e = kernel_->world.ai.at(i);
		if (!e) continue;
		const opennova::world::Entity *ent = kernel_->world.registry.get(e->handle);
		if (!ent) continue;
		if ((ent->net_stance_bits & 0x3u) == 0) continue;
		if (ent->ground_target.valid()) continue;
		out.push_back(Vector3(static_cast<float>(e->pos[0] / kFixed16),
		                      static_cast<float>(e->pos[2] / kFixed16),
		                      static_cast<float>(-e->pos[1] / kFixed16)));
	}
	return out;
}

PackedVector3Array Simulation::get_entity_effect_state_for_ssn(int p_ssn) const {
	PackedVector3Array out;
	if (kernel_ == nullptr || p_ssn <= 0 ||
			p_ssn > static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
		return out;
	}
	const opennova::world::Entity *entity = kernel_->world.registry.get(
			kernel_->world.registry.find_by_net_id(static_cast<std::uint16_t>(p_ssn)));
	if (entity == nullptr) return out;

	out.resize(EFFECT_STATE_COUNT);
	out.set(EFFECT_STATE_POSITION, Vector3(
			entity->position.x, entity->position.z, -entity->position.y));
	out.set(EFFECT_STATE_ROTATION_DEG, Vector3(
			static_cast<float>(entity->pitch),
			static_cast<float>(entity->yaw),
			static_cast<float>(entity->roll)));
	return out;
}

void Simulation::invalidate_present_effect_pose_cache() const {
	present_effect_pose_cache_valid_ = false;
	present_effect_pose_cache_runtime_ = nullptr;
	present_effect_poses_by_handle_.clear();
	present_effect_handles_by_bms_id_.clear();
	present_effect_handles_by_ssn_.clear();
	present_effect_handles_by_origin_.clear();
	present_effect_missing_handles_.clear();
	present_effect_missing_bms_ids_.clear();
	present_effect_missing_ssns_.clear();
	present_effect_missing_origins_.clear();
}

void Simulation::ensure_present_effect_pose_cache() const {
	if (!kernel_ || !runtime_) {
		if (present_effect_pose_cache_valid_) invalidate_present_effect_pose_cache();
		return;
	}

	const opennova::replication::ClientState &client = runtime_->state();
	const uint32_t logic_tick = kernel_->world.logic_tick;
	if (present_effect_pose_cache_valid_ &&
			present_effect_pose_cache_runtime_ == runtime_.get() &&
			present_effect_pose_cache_logic_tick_ == logic_tick &&
			present_effect_pose_cache_client_frame_ == client.frames_applied) {
		return;
	}

	present_effect_poses_by_handle_.clear();
	present_effect_handles_by_bms_id_.clear();
	present_effect_handles_by_ssn_.clear();
	present_effect_handles_by_origin_.clear();
	present_effect_missing_handles_.clear();
	present_effect_missing_bms_ids_.clear();
	present_effect_missing_ssns_.clear();
	present_effect_missing_origins_.clear();
	present_effect_pose_cache_logic_tick_ = logic_tick;
	present_effect_pose_cache_client_frame_ = client.frames_applied;
	present_effect_pose_cache_runtime_ = runtime_.get();
	present_effect_pose_cache_valid_ = true;
}

bool Simulation::cache_present_effect_pose(
		const opennova::replication::ClientEntityState &p_entity_state) const {
	// Match present_snapshot_from_client_replicas' joiner self-filter: the host's
	// wire echo H is not drawn and therefore cannot own a presented effect.
	// Packed handle zero is a valid pool-0 identity, so presence rides the
	// runtime's explicit validity seam, never a zero sentinel.
	if (joiner_ && runtime_ && runtime_->has_self_handle() &&
			p_entity_state.handle == runtime_->self_handle()) {
		return false;
	}
	if (present_effect_poses_by_handle_.find(p_entity_state.handle) !=
			present_effect_poses_by_handle_.end()) {
		return true;
	}

	const int32_t heading_bam = p_entity_state.heading_bam;
	// Host/listen presentation can recover the authored pitch and roll from the
	// authoritative registry. The compact peer row only carries yaw; joiners
	// therefore retain the wire-only zeroes here.
	const opennova::world::Entity *entity = joiner_ ? nullptr : kernel_->world.registry.get(
			opennova::world::EntityHandle{p_entity_state.handle});
	PresentEffectPose pose;
	pose.position = Vector3(
			static_cast<float>(p_entity_state.x / kFixed16),
			static_cast<float>(p_entity_state.z / kFixed16),
			static_cast<float>(-p_entity_state.y / kFixed16));
	pose.rotation_deg = Vector3(
			entity ? static_cast<float>(entity->pitch) : 0.0f,
			static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(
					heading_bam)),
			entity ? static_cast<float>(entity->roll) : 0.0f);
	present_effect_poses_by_handle_[p_entity_state.handle] = pose;
	present_effect_missing_handles_.erase(p_entity_state.handle);

	// A joiner's decoded handles belong to the host, so only wire identity is
	// meaningful there. Host/listen views can resolve every alias from the same
	// registry entity used by get_present_snapshot().
	if (joiner_ || !entity) return true;
	if (entity->bms_id > 0) {
		const int bms_id = static_cast<int>(entity->bms_id);
		present_effect_handles_by_bms_id_[bms_id] =
				p_entity_state.handle;
		present_effect_missing_bms_ids_.erase(bms_id);
	}
	if (entity->net_id > 0) {
		const int ssn = static_cast<int>(entity->net_id);
		present_effect_handles_by_ssn_[ssn] =
				p_entity_state.handle;
		present_effect_missing_ssns_.erase(ssn);
	}
	const int kind = opennova::world::spawn_origin_kind(entity->spawn_origin);
	const int index = static_cast<int>(opennova::world::spawn_origin_index(entity->spawn_origin));
	const uint64_t origin = present_effect_origin_key(kind, index);
	present_effect_handles_by_origin_[origin] =
			p_entity_state.handle;
	present_effect_missing_origins_.erase(origin);
	return true;
}

bool Simulation::cache_present_effect_pose(
		const opennova::world::Entity &p_entity) const {
	const uint16_t handle = p_entity.handle.packed;
	if (present_effect_poses_by_handle_.find(handle) !=
			present_effect_poses_by_handle_.end()) {
		return true;
	}
	const AiEntity *ae = kernel_->world.ai.for_handle(p_entity.handle);
	PresentEffectPose pose;
	pose.position = Vector3(p_entity.position.x, p_entity.position.z,
			-p_entity.position.y);
	pose.rotation_deg = Vector3(
			static_cast<float>(p_entity.pitch),
			static_cast<float>(pool_present_yaw_deg(
					p_entity, ae, opennova::replication::entity_class_of(p_entity))),
			static_cast<float>(p_entity.roll));
	present_effect_poses_by_handle_[handle] = pose;
	present_effect_missing_handles_.erase(handle);
	if (p_entity.bms_id > 0) {
		const int bms_id = static_cast<int>(p_entity.bms_id);
		present_effect_handles_by_bms_id_[bms_id] = handle;
		present_effect_missing_bms_ids_.erase(bms_id);
	}
	if (p_entity.net_id > 0) {
		const int ssn = static_cast<int>(p_entity.net_id);
		present_effect_handles_by_ssn_[ssn] = handle;
		present_effect_missing_ssns_.erase(ssn);
	}
	const int kind = opennova::world::spawn_origin_kind(p_entity.spawn_origin);
	const int index = static_cast<int>(
			opennova::world::spawn_origin_index(p_entity.spawn_origin));
	const uint64_t origin = present_effect_origin_key(kind, index);
	present_effect_handles_by_origin_[origin] = handle;
	present_effect_missing_origins_.erase(origin);
	return true;
}

PackedVector3Array Simulation::cached_present_effect_state_for_handle(
		uint16_t p_handle) const {
	PackedVector3Array out;
	const auto found = present_effect_poses_by_handle_.find(p_handle);
	if (found == present_effect_poses_by_handle_.end()) return out;
	out.resize(EFFECT_STATE_COUNT);
	out.set(EFFECT_STATE_POSITION, found->second.position);
	out.set(EFFECT_STATE_ROTATION_DEG, found->second.rotation_deg);
	return out;
}

PackedVector3Array Simulation::present_effect_state_for_handle(uint16_t p_handle) const {
	ensure_present_effect_pose_cache();
	PackedVector3Array cached = cached_present_effect_state_for_handle(p_handle);
	if (!cached.is_empty() || !runtime_) return cached;
	if (present_effect_missing_handles_.find(p_handle) !=
			present_effect_missing_handles_.end()) {
		return PackedVector3Array();
	}
	if (!joiner_) {
		const opennova::world::Entity *entity =
				kernel_->world.registry.get(opennova::world::EntityHandle{p_handle});
		if (entity != nullptr && cache_present_effect_pose(*entity)) {
			return cached_present_effect_state_for_handle(p_handle);
		}
		present_effect_missing_handles_.insert(p_handle);
		return PackedVector3Array();
	}
	for (const opennova::replication::ClientEntityState &entity_state :
			runtime_->state().entities) {
		if (entity_state.handle != p_handle) continue;
		if (cache_present_effect_pose(entity_state)) {
			return cached_present_effect_state_for_handle(p_handle);
		}
		break;
	}
	present_effect_missing_handles_.insert(p_handle);
	return PackedVector3Array();
}

PackedVector3Array Simulation::get_present_effect_state_for_ssn(int p_ssn) const {
	if (p_ssn <= 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const auto found = present_effect_handles_by_ssn_.find(p_ssn);
	if (found != present_effect_handles_by_ssn_.end()) {
		return cached_present_effect_state_for_handle(found->second);
	}
	if (!runtime_ || joiner_) return PackedVector3Array();
	if (present_effect_missing_ssns_.find(p_ssn) !=
			present_effect_missing_ssns_.end()) {
		return PackedVector3Array();
	}
	const opennova::world::Entity *match = nullptr;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (match == nullptr && static_cast<int>(e.net_id) == p_ssn) match = &e;
	});
	if (match != nullptr && cache_present_effect_pose(*match)) {
		return cached_present_effect_state_for_handle(match->handle.packed);
	}
	present_effect_missing_ssns_.insert(p_ssn);
	return PackedVector3Array();
}

PackedVector3Array Simulation::get_present_effect_state_for_wire_handle(
		int p_wire_handle) const {
	if (p_wire_handle < 0 ||
			p_wire_handle > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
		return PackedVector3Array();
	}
	return present_effect_state_for_handle(static_cast<uint16_t>(p_wire_handle));
}

PackedVector3Array Simulation::get_present_effect_state_for_bms_id(int p_bms_id) const {
	if (p_bms_id <= 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const auto found = present_effect_handles_by_bms_id_.find(p_bms_id);
	if (found != present_effect_handles_by_bms_id_.end()) {
		return cached_present_effect_state_for_handle(found->second);
	}
	if (!runtime_ || joiner_) return PackedVector3Array();
	if (present_effect_missing_bms_ids_.find(p_bms_id) !=
			present_effect_missing_bms_ids_.end()) {
		return PackedVector3Array();
	}
	const opennova::world::Entity *entity =
			kernel_->world.registry.get(handle_for_bms_id(p_bms_id));
	if (entity != nullptr && cache_present_effect_pose(*entity)) {
		return cached_present_effect_state_for_handle(entity->handle.packed);
	}
	present_effect_missing_bms_ids_.insert(p_bms_id);
	return PackedVector3Array();
}

PackedVector3Array Simulation::get_present_effect_state_for_origin(
		int p_kind, int p_index) const {
	if (p_kind < 0 || p_index < 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const uint64_t requested_origin = present_effect_origin_key(p_kind, p_index);
	const auto found = present_effect_handles_by_origin_.find(requested_origin);
	if (found != present_effect_handles_by_origin_.end()) {
		return cached_present_effect_state_for_handle(found->second);
	}
	if (!runtime_ || joiner_) return PackedVector3Array();
	if (present_effect_missing_origins_.find(requested_origin) !=
			present_effect_missing_origins_.end()) {
		return PackedVector3Array();
	}
	const opennova::world::Entity *match = nullptr;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (match != nullptr) return;
		const int kind = opennova::world::spawn_origin_kind(e.spawn_origin);
		const int index = static_cast<int>(opennova::world::spawn_origin_index(e.spawn_origin));
		if (present_effect_origin_key(kind, index) == requested_origin) match = &e;
	});
	if (match != nullptr && cache_present_effect_pose(*match)) {
		return cached_present_effect_state_for_handle(match->handle.packed);
	}
	present_effect_missing_origins_.insert(requested_origin);
	return PackedVector3Array();
}

// [D-NET-112] entity+0x78 ownerConnectionId (the connection/dcb that owns this entity). A networked
// PLAYER is identified by this + its handle, NOT by an SSN (players carry net_id 0). 0 = unowned (AI /
// mission entity / the host's dedicated reservation).
int Simulation::get_entity_owner_connection_id(int p_index) const {
	if (!kernel_) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return 0;
	const opennova::world::Entity *ent = kernel_->world.registry.get(e->handle);
	return ent ? static_cast<int>(ent->owner_connection_id) : 0;
}

// The entity's wire handle (pool<<12|slot) — the per-entity identity carried on the 0x0A/0x0C wire and
// the decoded present's PF_WIRE_HANDLE. Unique per entity (unlike a player's net_id, which is now 0).
int Simulation::get_entity_wire_handle(int p_index) const {
	if (!kernel_) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	return e ? static_cast<int>(e->handle.packed) : 0;
}

int32_t Simulation::decode_present_part_anim_phase(
		const PackedFloat32Array &p_snapshot, int p_base, int p_channel) {
	if (p_channel < 1 || p_channel > 2 || p_base < 0) return 0;
	const int phase_field = PF_PHASE1 + (p_channel - 1) * 2;
	const int active_field = PF_ACTIVE1 + (p_channel - 1) * 2;
	if (p_base + active_field >= p_snapshot.size()) return 0;
	const float *p = p_snapshot.ptr();
	const int32_t high_code =
			static_cast<int32_t>(p[p_base + active_field]);
	if (!opennova::world::part_anim_phase_active(high_code)) return 0;
	const uint32_t low = static_cast<uint32_t>(
			static_cast<int32_t>(p[p_base + phase_field])) & 0xFFFFu;
	const uint32_t bits =
			(static_cast<uint32_t>(high_code - 1) << 16) | low;
	int32_t value;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

int Simulation::get_entity_part_anim_phase(int p_index, int channel) const {
	if (!kernel_ || channel < 1 || channel > 2) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kPartAnimPhase0 + (channel - 1)];
}

bool Simulation::get_entity_part_anim_active(int p_index, int channel) const {
	if (!kernel_ || channel < 1 || channel > 2) return false;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return false;
	if (channel == 1 && kernel_ != nullptr) {
		const opennova::world::Entity *entity =
				kernel_->world.registry.get(e->handle);
		if (entity != nullptr && (entity->item_attrib & 0x1000u) != 0)
			return false;
	}
	return true;
}

PackedFloat32Array Simulation::get_present_snapshot() const {
	const uint64_t start_us =
			runtime_profiling_enabled_ ? opennova::io::perf_now_us() : 0;
	// ADR 0011 Decision 1 (as amended, D-NET-140 closed): every authoritative live mission is an
	// in-process listen server in standalone MainGame/GameWorld, and the listen host presents
	// from its OWN pools — retail's local client reads process memory and its loopback 0x0A is
	// header-only [orig: serialize_entity_states_to_packet @0x50f07e;
	// collect_visible_entities_for_terrain @0x5c8c60]. A joiner renders the host's stream
	// wire-direct from the state its ClientReplicaPipeline decoded (ClientState).
	// Empty when no runtime is active (a bare sim) — scalar getters (get_entity_*) read the
	// AI pool for tooling.
	PackedFloat32Array out;
	if (runtime_) {
		out = joiner_ ? present_snapshot_from_client_replicas()
		              : present_snapshot_from_world();
	}
	last_present_entity_count_ = static_cast<int>(out.size() / PF_STRIDE);
	std::vector<PresentRowIdentity> next_layout;
	next_layout.reserve(static_cast<std::size_t>(last_present_entity_count_));
	const float *rows = out.ptr();
	for (int i = 0; i < last_present_entity_count_; ++i) {
		const float *row = rows + static_cast<int64_t>(i) * PF_STRIDE;
		next_layout.push_back(PresentRowIdentity{
				static_cast<int32_t>(row[PF_WIRE_HANDLE]),
				static_cast<int32_t>(row[PF_TYPE_ID]),
				static_cast<int32_t>(row[PF_BMS_ID]),
				static_cast<int32_t>(row[PF_KIND]),
				static_cast<int32_t>(row[PF_INDEX])});
	}
	if (next_layout != present_layout_) {
		present_layout_ = std::move(next_layout);
		++present_layout_revision_;
	}
	if (runtime_profiling_enabled_)
		last_present_snapshot_us_ = opennova::io::perf_now_us() - start_us;
	return out;
}
PackedFloat32Array Simulation::present_snapshot_from_client_replicas() const {
	PackedFloat32Array out;
	if (!kernel_ || !runtime_) return out;
	// P7: every path (SP / LAN host / joiner) reads its own npruntime ClientRuntime view's ClientState.
	const opennova::replication::ClientState &cs = runtime_->state();
	const opennova::world::Entity *local_player =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	// Entity_RenderVehicleModel's local UseGun predicate is a render verdict,
	// not Entity.hidden: parent equality + first-person camera + raw UseGun seat.
	// The per-row tail below adds the live EquippedSlot/Def tests.
	// [orig: @0x4407f6..0x44084c; sole submit @0x440918]
	const bool local_first_person_usegun =
			!kernel_->view.third_person && local_player != nullptr &&
			local_player->mounted &&
			local_player->mount_type == opennova::world::SeatType::Gunner;
	const int count = static_cast<int>(cs.entities.size());
	const opennova::inmatch::ClientReplicaPresentContext replica_present_context{
			&kernel_->seat_specs, &kernel_->world.tables.weapons, joiner_};
	out.resize(static_cast<int64_t>(count) * PF_STRIDE);
	float *w = out.ptrw();
	for (int i = 0; i < count; ++i) {
		float *r = w + static_cast<int64_t>(i) * PF_STRIDE;
		const opennova::replication::ClientEntityState &es = cs.entities[i];
		opennova::inmatch::initialize_client_replica_present_row(r);

		// Self-filter (joiner): the host SNAPs our own entity (wire handle H) and streams
		// it back in 0x0A; we draw our local player L via LocalPlayerPresenter, so drop the wire
		// echo here. The row stays at its zero/unresolved defaults (PF_TYPE_ID 0), which the
		// wire render pass skips. [net-re §5.38b two-handle L-vs-H reconciliation]
		if (joiner_ && runtime_->has_self_handle() &&
				es.handle == runtime_->self_handle()) {
			continue;
		}
		// The canonical decoded-client projection owns wire identity, pose,
		// lifecycle, and remote Person appearance for every role. The remainder
		// of this method is role/world enrichment only.
		opennova::inmatch::project_client_replica_present_row(
				r, es, cs, replica_present_context);

		// On the HOST listen server, kind/index/bms_id/net_id resolve from the authored
		// registry entity behind the decoded handle, so EntityIndex can defer
		// that row to MissionPresentPass. A production header-only joiner instead presents
		// every streamed row wire-direct: pools 1-3 have exact-handle native gameplay rows,
		// but those rows carry the spawn-origin sentinel and therefore no authored-node
		// identity. The guarded fill below exists only for an explicit complete-BMS/debug
		// join, where authored promotion supplied a matching type at the same handle.
		// Hidden/alive/animation state still comes from the wire. Remote pool-0 organics
		// remain wire-rendered through their remote-request-shaped body path.
		const opennova::world::EntityHandle h{es.handle};
		const opennova::world::Entity *ent = (!joiner_) ? kernel_->world.registry.get(h) : nullptr;
		// Retail's terrain collector sends pool-1 model rows through
		// render_sector_entity; pool-2 statics and pool-3 marker models join the
		// same sector list through their dedicated collectors. Pool-0 skeletal
		// organics take the general/body list and do not execute this writer.
		// Keep validity independent of whether this client resolves the row to a
		// placed node: a wire-fallback model still executes the same callback,
		// while a failed model build has no CTRL surface on which to apply it.
		const bool sector_model_row = h.pool() >= 1 && h.pool() <= 3;
		if (joiner_ && h.pool() >= 1 && h.pool() <= 3) {
			const opennova::world::Entity *local = kernel_->world.registry.get(h);
			if (local != nullptr && local->spawn_origin != opennova::world::kSpawnOriginNone &&
					static_cast<uint16_t>(local->item_id) == es.type_id) {
				r[PF_KIND] = static_cast<float>(local->spawn_origin >> 24);
				r[PF_INDEX] = static_cast<float>(opennova::world::spawn_origin_index(local->spawn_origin));
				r[PF_BMS_ID] = static_cast<float>(local->bms_id);
				r[PF_NET_ID] = static_cast<float>(local->net_id);
			}
		}
		if (ent) {
			r[PF_KIND] = static_cast<float>(ent->spawn_origin >> 24);
			r[PF_INDEX] = static_cast<float>(opennova::world::spawn_origin_index(ent->spawn_origin));
			r[PF_BMS_ID] = static_cast<float>(ent->bms_id);
			r[PF_NET_ID] = static_cast<float>(ent->net_id);
			r[PF_BODY_ANIM_SLOT] = static_cast<float>(ent->body_anim_slot);
			r[PF_PITCH_DEG] = static_cast<float>(ent->pitch);
			r[PF_ROLL_DEG] = static_cast<float>(ent->roll);
			r[PF_HIDDEN] = ent->hidden ? 1.0f : 0.0f;
			r[PF_ALIVE] = ent->alive ? 1.0f : 0.0f;
			// The authority owns the exact MoveOrder stance latch (bits 8/9 of
			// Player_PackInputStateToEntity @0x4df450; the MATCHTERRAIN tier reads them at
			// Terrain_RenderSectorEntitiesBySide @0x5c7dc2..0x5c7ded - docs/foliage/foliage-re.md). The compact
			// projection above reconstructs this from animation flags for joiners;
			// host/SP must prefer the source byte used by retail's gate.
			r[PF_STANCE_BITS] = static_cast<float>(ent->net_stance_bits & 0x03u);
			write_present_section_mask(r, ent->section_mask);
			r[PF_RIGHT_HAND_COLLAPSED] =
					mount_collapses_right_hand_row(*ent) ? 1.0f : 0.0f;
			// The cveh render callback publishes directly from the live entity
			// motor fields. Do this only for the authoritative registry row:
			// the compact view has no steer/currentSpeed source to reconstruct.
			// [orig: Entity_CacheVehicleHUDStats @ 0x4929B0;
			//  stores @0x4929D7 / @0x4929F1]
			write_present_vehicle_motion_controls(r, kernel_->world, *ent);
			// Only a carrier in the witnessed live UseGun attachment relation
			// publishes its inline MountSlot's HEAT_GLOW, including owned cold
			// zero. A joiner has no heat-window/ownership state in its compact
			// row and must not synthesize one.
			// [orig: attachment call @ 0x546518;
			//  HUD_CacheWeaponSlotInfo stores @ 0x440969 / @ 0x440991]
			write_present_world_model_heat_glow(r, kernel_->world, *ent);
		}
		// The local first-person UseGun parent cull is a render verdict of THIS
		// machine's own mount state, and retail's render walk applies it
		// identically on every role. Resolve the mount row directly: a joiner's
		// exact-handle materialized entity carries the resolved embedded
		// MountSlot (primary_weapon_slot_adm), while `ent` above deliberately
		// stays host-only authored-identity enrichment — keeping this inside it
		// skipped the cull on joiners, drawing the mounted gun TWICE (its wire
		// world model plus the FP model).
		// [orig: Entity_RenderVehicleModel @0x4407d0 predicate
		//  @0x4407f6..0x44084c; sole submit @0x440918]
		if (local_first_person_usegun && local_player->mount_target == h) {
			const opennova::world::Entity *mount_row = kernel_->world.registry.get(h);
			const opennova::world::WeaponTableEntry *mount_def =
					mount_row != nullptr
					? kernel_->world.tables.weapons.by_index(mount_row->primary_weapon_slot_adm)
					: nullptr;
			// Primary retail leg: FP model exists and this exact embedded
			// MountSlot is the live EquippedSlot. flags2 Invisible is the
			// witnessed alternate forced-cull leg and does not require the
			// EquippedSlot comparison.
			// [orig: Def+0x16c @0x440824; EquippedSlot @0x440833;
			//  Def+0x0c & 0x800 @0x44083f]
			const bool equipped_parent_slot = mount_row != nullptr &&
					kernel_->weapon.usegun_slot_active && kernel_->weapon.usegun_mount == h &&
					kernel_->weapon.usegun_weapon_adm ==
							mount_row->primary_weapon_slot_adm;
			if (mount_def != nullptr &&
					((mount_def->has_first_person_model_reference &&
					  kernel_->weapon.first_person_model_adm ==
							  mount_row->primary_weapon_slot_adm &&
					  equipped_parent_slot) ||
					 (mount_def->flags2 &
					  opennova::world::weapon_flag2::kInvisible) != 0))
				r[PF_LOCAL_VIEW_SUPPRESSED] = 1.0f;
		}
		// Two retail callbacks write this three-register family. The sector
		// renderer publishes TEX_TEAM for every placed pool-1/2/3 model that
		// reaches its model callback. The generic-world callback publishes the
		// same TEX_TEAM plus TEAMSWING for a nonzero packed zone byte, and writes
		// LFP only when the client-side shared timer-list entry exists.
		// Values come from the decoded client row for BOTH authority and joiner:
		// this preserves the exact 0x0D/0x10/0x20 bytes and later S2C 0x50 team
		// mutations instead of reaching around the replica pipeline.
		// [orig: render_sector_entity @0x5C424F..0x5C425F;
		//  BoneCallback_gnrc_World @0x4E288B..0x4E28FB]
		const bool zone_ctrl = es.zone_number_rank != 0;
		const int32_t signed_team = es.team < 0x80u
				? static_cast<int32_t>(es.team)
				: static_cast<int32_t>(es.team) - 0x100;
		if (sector_model_row || zone_ctrl) {
			r[PF_TEX_TEAM_VALID] = 1.0f;
			r[PF_TEX_TEAM] = static_cast<float>(signed_team);
		}
		if (zone_ctrl) {
			r[PF_ZONE_CTRL_VALID] = 1.0f;
			const int32_t team_swing = es.team == 1u
					? 0
					: (es.team == 2u ? 0x10000 : 0x8000);
			r[PF_TEAMSWING] = static_cast<float>(team_swing);
			int32_t camp_percent = 0;
			if (runtime_->lfp_cam_percent(es.handle, camp_percent)) {
				r[PF_LFP_CAMPPERCENT_VALID] = 1.0f;
				r[PF_LFP_CAMPPERCENT] = static_cast<float>(camp_percent);
			}
		}
		// Wire lifecycle — PF_RESPAWN_REVISION, the bit0 hide with its
		// carrier-attach exemption, and the class-dependent dead bit (vehicle
		// wrecks are flags&4, not the organic bit 1) — is written by
		// project_client_replica_present_row for every role; the authoritative
		// registry block above overrides hidden/alive on the host. Do not
		// re-derive it here: a pre-ADR-0026 copy of this logic once drifted by
		// testing vehicles against the organic dead bit.
		EmplacedWeaponControls emplaced;
		if (ent != nullptr) {
			if (emplaced_weapon_controls_for(
						kernel_->world, *ent, emplaced))
				write_present_emplaced_controls(r, emplaced);
		}
		const bool authoritative_attachment_pose =
				ent != nullptr && ent->emplacement_parent.valid() &&
				ent->emplacement_pose_metadata_resolved &&
				ent->emplacement_parent.packed == es.parent_handle;
		opennova::world::MountedPose client_attachment_pose;
		const uint32_t attachment_time_ms = kernel_->panm_time_override_ms >= 0
				? static_cast<uint32_t>(kernel_->panm_time_override_ms)
				: kernel_->world.logic_tick * 16u;
		const bool reconstructed_client_attachment_pose = joiner_ &&
				resolve_client_eweap_attachment_pose(
						es, cs, kernel_->seat_specs, kernel_->mounted_graphics,
						kernel_->models, attachment_time_ms, client_attachment_pose);
		if (authoritative_attachment_pose) {
			// NoNetworkCallback addeweap children have only their 0x0D spawn pose in
			// ClientState. The host has already advanced their authoritative userpoint
			// pose through World::update_emplacement_attachments; use that exact result
			// rather than flattening live PANM back to the client's rigid spawn offset.
			r[PF_POS_X] = ent->position.x;
			r[PF_POS_Y] = ent->position.z;
			r[PF_POS_Z] = -ent->position.y;
			r[PF_PITCH_DEG] = static_cast<float>(ent->pitch);
			r[PF_YAW_DEG] = static_cast<float>(ent->yaw);
			r[PF_ROLL_DEG] = static_cast<float>(ent->roll);
		} else if (reconstructed_client_attachment_pose) {
			r[PF_POS_X] = client_attachment_pose.position.x;
			r[PF_POS_Y] = client_attachment_pose.position.z;
			r[PF_POS_Z] = -client_attachment_pose.position.y;
			r[PF_PITCH_DEG] = static_cast<float>(client_attachment_pose.pitch);
			r[PF_YAW_DEG] = static_cast<float>(client_attachment_pose.yaw);
			r[PF_ROLL_DEG] = static_cast<float>(client_attachment_pose.roll);
		}
		// No unattached else: the projection already wrote the decoded wire
		// pose — the chased/snapped position, the vehicle BAM32 euler X/Y, and
		// the full-16-bit-precision heading (net-re §5.38e, D-NET-196). The
		// two attachment branches above and the host's authoritative registry
		// pitch/roll (the `ent` block) override it where a better source
		// exists; re-deriving the wire pose here would re-clobber the host's
		// live vehicle attitude with the stale spawn/dead-pose eulers.
		// Infantry anim from the local AI pool (host only — same registry caveat as above).
		if (!joiner_) {
			const AiEntity *ae = kernel_->world.ai.for_handle(h);
			if (ae != nullptr) {
				for (int slot = 0; slot < 2; ++slot) {
					// HUD_CacheEntityDisplayInfo copies comp[113/114] as raw
					// signed dwords. Do not normalize wrapping zero-time states.
					// [orig: stores @0x4A3E2D/@0x4A3E38]
					const int32_t phase =
							ae->brain.f[AiBrain::kPartAnimPhase0 + slot];
					const bool publish =
							slot != 0 || ent == nullptr ||
							(ent->item_attrib & 0x1000u) == 0;
					uint32_t phase_bits;
					std::memcpy(&phase_bits, &phase, sizeof(phase_bits));
					// The float snapshot transports both 16-bit words as exact
					// integers. ACTIVE zero means unpublished; otherwise it is
					// high16+1. A numeric float32 could lose signed-dword low
					// bits for fast/malformed PLAYPARTANIM rates.
					r[PF_PHASE1 + slot * 2] =
							static_cast<float>(phase_bits & 0xFFFFu);
					r[PF_ACTIVE1 + slot * 2] = publish
							? static_cast<float>((phase_bits >> 16) + 1u)
							: 0.0f;
				}
			}
			if (ae && ae->inf.active) {
				r[PF_ANIM_STATE] = static_cast<float>(ae->inf.anim_state);
				r[PF_ANIM_PHASE_TICKS] = static_cast<float>(ae->inf.clip_phase);
				if (ae->inf.body_blend_active()) {
					r[PF_ANIM_SOURCE_STATE] =
							static_cast<float>(ae->inf.anim_prev);
					r[PF_ANIM_SOURCE_PHASE_TICKS] =
							static_cast<float>(ae->inf.anim_prev_clip_phase);
					r[PF_ANIM_BLEND_WEIGHT] = ae->inf.anim_blend_weight;
				}
				// The upper-body weapon channel this body derived for itself —
				// for the host's OWN player and for every wire peer alike, since
				// remote_player_body_anim now runs the same selection. The gate is
				// the §14.8.6 consumer test; engine_flags bit 0x100 is this file's
				// established "is a player" mirror of entity+0x24, which is what
				// keeps NPCs (who carry no hold ladder in the original either) out.
				if (ent != nullptr &&
						opennova::world::infantry_weapon_channel_visible(
								ae->inf,
								(ent->engine_flags & opennova::world::kEntityFlagPlayer) != 0,
								mount_blocks_weapon_channel(*ent))) {
					r[PF_WPN_ANIM_STATE] =
							static_cast<float>(ae->inf.wpn_state);
					r[PF_WPN_PHASE_TICKS] =
							static_cast<float>(ae->inf.wpn_clip_phase);
					r[PF_WPN_VARIANT] = static_cast<float>(ae->inf.wpn_variant);
					if (ae->inf.weapon_blend_active()) {
						r[PF_WPN_SOURCE_STATE] =
								static_cast<float>(ae->inf.wpn_prev);
						r[PF_WPN_SOURCE_PHASE_TICKS] =
								static_cast<float>(ae->inf.wpn_prev_clip_phase);
						r[PF_WPN_BLEND_WEIGHT] = ae->inf.wpn_blend_weight;
						r[PF_WPN_SOURCE_VARIANT] =
								static_cast<float>(ae->inf.wpn_prev_variant);
					}
				}
				if (ent != nullptr) {
					const opennova::anim::AimOverlayInputs inputs =
							aim_overlay_inputs_for(*ae, *ent);
					opennova::anim::AimOverlayAngles
							angles[opennova::anim::kOverlayClassCount];
					opennova::anim::compute_aim_overlay_angles(inputs, angles);
					write_present_overlay(r, angles);
					// This body's third-person gun. Player rows only: retail's
					// composition gate is the Flags 0x100 player classifier, and
					// placed NPCs carry no equipped index anyway.
					if ((ent->engine_flags & opennova::world::kEntityFlagPlayer) != 0) {
						write_present_held_weapon(
								r, ent->equipped_adm_index,
								(ent->flags & opennova::world::kEntityFlagDead) != 0, inputs,
								ae->inf.wpn_state);
					}
				}
			}
		}
	}
	// Consume-once: each transition pulse dispatches exactly one presented
	// frame (the rows above copied any live pulse into PF_ANIM_STATE_PULSE).
	runtime_->state().clear_anim_pulses();
	return out;
}

PackedFloat32Array Simulation::present_snapshot_from_world() const {
	PackedFloat32Array out;
	if (!kernel_) return out;
	const opennova::world::World &w = kernel_->world;
	const opennova::world::Entity *local_player =
			w.registry.get(w.cached.local_player);
	// Entity_RenderVehicleModel's local UseGun predicate is a render verdict,
	// not Entity.hidden: parent equality + first-person camera + raw UseGun seat.
	// The per-row tail below adds the live EquippedSlot/Def tests.
	// [orig: Entity_RenderVehicleModel @0x4407f6..0x44084c, sole submit @0x440918;
	//  see docs/world/world-wac-ai-re.md]
	const bool local_first_person_usegun =
			!kernel_->view.third_person && local_player != nullptr &&
			local_player->mounted &&
			local_player->mount_type == opennova::world::SeatType::Gunner;
	// One row per live pool slot, in registry order — the set the host's own
	// ClientState held before D-NET-140 closed (every slot the 0x0C/0x0D/0x10/
	// 0x20 spawn batches stream plus every 0x0A record), now read straight
	// from the pools [orig: collect_visible_entities_for_terrain @0x5c8c60
	// walks the pools; see docs/net/novaworld-net-re.md D-NET-140]. A row
	// without a def keeps PF_TYPE_ID 0, which the wire pass skips.
	int count = 0;
	w.registry.for_each([&](const opennova::world::Entity &) { ++count; });
	out.resize(static_cast<int64_t>(count) * PF_STRIDE);
	float *rows = out.ptrw();
	int i = 0;
	w.registry.for_each([&](const opennova::world::Entity &e) {
		float *r = rows + static_cast<int64_t>(i++) * PF_STRIDE;
		const opennova::world::EntityHandle h = e.handle;
		const opennova::EntityClass cls = opennova::replication::entity_class_of(e);
		const AiEntity *ae = w.ai.for_handle(h);
		opennova::inmatch::initialize_client_replica_present_row(r);

		// Wire identity + lifecycle, exactly what project_client_replica_present_row
		// derives for a decoded row, sourced from the authoritative record.
		r[PF_TYPE_ID] = static_cast<float>(static_cast<uint16_t>(e.item_id));
		r[PF_WIRE_HANDLE] = static_cast<float>(h.packed);
		// The groundEntity/mount link the footstep slot reads (mount wins over
		// ground — retail: NetPacket_SerializePlayerState op1 @0x4c0a08, see
		// docs/net/novaworld-net-re.md); -1 = free-standing.
		const uint16_t carrier = e.mounted && e.mount_target.valid()
				? e.mount_target.packed
				: (e.ground_target.valid() ? e.ground_target.packed
				                           : opennova::world::EntityHandle::kInvalid);
		r[PF_CARRIER_HANDLE] = carrier != opennova::world::EntityHandle::kInvalid
				? static_cast<float>(carrier) : -1.0f;
		if (cls == opennova::EntityClass::Player) {
			// A player's wire net_id IS its packed character id (entity+0x15C).
			r[PF_CHARACTER_ID] =
					static_cast<float>(opennova::replication::player_wire_net_id(e));
		}
		r[PF_POS_X] = e.position.x;
		r[PF_POS_Y] = e.position.z;
		r[PF_POS_Z] = -e.position.y;
		r[PF_PITCH_DEG] = static_cast<float>(e.pitch);
		r[PF_YAW_DEG] = static_cast<float>(pool_present_yaw_deg(e, ae, cls));
		r[PF_ROLL_DEG] = static_cast<float>(e.roll);
		// The decoded fold bumps a row's respawn revision on every dead->alive
		// edge of its wire state byte (organic bit 1; vehicle wrecks flag 4).
		// Mirror that edge from the authoritative flags so WirePresentPass
		// re-seeds the same way on the host.
		{
			const uint8_t dead_bit = cls == opennova::EntityClass::Vehicle
					? opennova::replication::kVehicleFlagDeadPose
					: static_cast<uint8_t>(opennova::world::kEntityFlagDead);
			const bool dead = (e.flags & dead_bit) != 0u;
			PoolPresentLifecycle &life = pool_present_lifecycle_[h.packed];
			if (life.registry_spawn_id != e.registry_spawn_id) {
				life.registry_spawn_id = e.registry_spawn_id;
				life.dead_known = false;
			}
			if (life.dead_known && life.dead && !dead) ++life.respawn_revision;
			life.dead_known = true;
			life.dead = dead;
			r[PF_RESPAWN_REVISION] = static_cast<float>(life.respawn_revision);
		}
		r[PF_KIND] = static_cast<float>(e.spawn_origin >> 24);
		r[PF_INDEX] = static_cast<float>(opennova::world::spawn_origin_index(e.spawn_origin));
		r[PF_BMS_ID] = static_cast<float>(e.bms_id);
		r[PF_NET_ID] = static_cast<float>(e.net_id);
		r[PF_BODY_ANIM_SLOT] = static_cast<float>(e.body_anim_slot);
		r[PF_HIDDEN] = e.hidden ? 1.0f : 0.0f;
		r[PF_ALIVE] = e.alive ? 1.0f : 0.0f;
		// The authority owns the exact MoveOrder stance latch (bits 8/9 of
		// Player_PackInputStateToEntity @0x4df450; the MATCHTERRAIN tier reads them at
		// Terrain_RenderSectorEntitiesBySide @0x5c7dc2..0x5c7ded - docs/foliage/foliage-re.md).
		r[PF_STANCE_BITS] = static_cast<float>(e.net_stance_bits & 0x03u);
		write_present_section_mask(r, e.section_mask);
		r[PF_RIGHT_HAND_COLLAPSED] =
				mount_collapses_right_hand_row(e) ? 1.0f : 0.0f;
		// The cveh render callback publishes directly from the live entity
		// motor fields [orig: Entity_CacheVehicleHUDStats @0x4929B0, stores
		// @0x4929D7 / @0x4929F1; see docs/world/vehicle-client-movers-re.md].
		write_present_vehicle_motion_controls(r, w, e);
		// Only a carrier in the witnessed live UseGun attachment relation
		// publishes its inline MountSlot's HEAT_GLOW, including owned cold zero.
		// [orig: attachment call @0x546518; HUD_CacheWeaponSlotInfo stores
		//  @0x440969 / @0x440991; see docs/world/world-wac-ai-re.md]
		write_present_world_model_heat_glow(r, w, e);
		// The local first-person UseGun parent cull is a render verdict of THIS
		// machine's own mount state [orig: Entity_RenderVehicleModel @0x4407d0
		// predicate @0x4407f6..0x44084c, sole submit @0x440918; see
		// docs/world/world-wac-ai-re.md].
		if (local_first_person_usegun && local_player->mount_target == h) {
			const opennova::world::WeaponTableEntry *mount_def =
					w.tables.weapons.by_index(e.primary_weapon_slot_adm);
			// Primary retail leg: FP model exists and this exact embedded
			// MountSlot is the live EquippedSlot. flags2 Invisible is the
			// witnessed alternate forced-cull leg and does not require the
			// EquippedSlot comparison.
			// [orig: Def+0x16c @0x440824; EquippedSlot @0x440833;
			//  Def+0x0c & 0x800 @0x44083f; see docs/world/world-wac-ai-re.md]
			const bool equipped_parent_slot =
					kernel_->weapon.usegun_slot_active && kernel_->weapon.usegun_mount == h &&
					kernel_->weapon.usegun_weapon_adm == e.primary_weapon_slot_adm;
			if (mount_def != nullptr &&
					((mount_def->has_first_person_model_reference &&
					  kernel_->weapon.first_person_model_adm == e.primary_weapon_slot_adm &&
					  equipped_parent_slot) ||
					 (mount_def->flags2 &
					  opennova::world::weapon_flag2::kInvisible) != 0))
				r[PF_LOCAL_VIEW_SUPPRESSED] = 1.0f;
		}
		// Two retail callbacks write this three-register family. The sector
		// renderer publishes TEX_TEAM for every placed pool-1/2/3 model that
		// reaches its model callback. The generic-world callback publishes the
		// same TEX_TEAM plus TEAMSWING for a nonzero packed zone byte, and writes
		// LFP only when the client-side shared timer-list entry exists.
		// [orig: render_sector_entity @0x5C424F..0x5C425F;
		//  BoneCallback_gnrc_World @0x4E288B..0x4E28FB; see docs/world/world-wac-ai-re.md]
		const bool sector_model_row = h.pool() >= 1 && h.pool() <= 3;
		const bool zone_ctrl = e.zone_number != 0 &&
				opennova::world::zone_chain_zone_info_byte(w.zones.chain, e) != 0;
		const int32_t signed_team = e.team < 0x80u
				? static_cast<int32_t>(e.team)
				: static_cast<int32_t>(e.team) - 0x100;
		if (sector_model_row || zone_ctrl) {
			r[PF_TEX_TEAM_VALID] = 1.0f;
			r[PF_TEX_TEAM] = static_cast<float>(signed_team);
		}
		if (zone_ctrl) {
			r[PF_ZONE_CTRL_VALID] = 1.0f;
			const int32_t team_swing = e.team == 1u
					? 0
					: (e.team == 2u ? 0x10000 : 0x8000);
			r[PF_TEAMSWING] = static_cast<float>(team_swing);
			int32_t camp_percent = 0;
			if (runtime_ && runtime_->lfp_cam_percent(h.packed, camp_percent)) {
				r[PF_LFP_CAMPPERCENT_VALID] = 1.0f;
				r[PF_LFP_CAMPPERCENT] = static_cast<float>(camp_percent);
			}
		}
		EmplacedWeaponControls emplaced;
		if (emplaced_weapon_controls_for(w, e, emplaced))
			write_present_emplaced_controls(r, emplaced);
		if (ae == nullptr) return;
		for (int slot = 0; slot < 2; ++slot) {
			// HUD_CacheEntityDisplayInfo copies comp[113/114] as raw
			// signed dwords. Do not normalize wrapping zero-time states.
			// [orig: HUD_CacheEntityDisplayInfo stores @0x4A3E2D/@0x4A3E38;
			//  see docs/world/world-wac-ai-re.md]
			const int32_t phase = ae->brain.f[AiBrain::kPartAnimPhase0 + slot];
			const bool publish = slot != 0 || (e.item_attrib & 0x1000u) == 0;
			uint32_t phase_bits;
			std::memcpy(&phase_bits, &phase, sizeof(phase_bits));
			// The float snapshot transports both 16-bit words as exact
			// integers. ACTIVE zero means unpublished; otherwise it is
			// high16+1.
			r[PF_PHASE1 + slot * 2] = static_cast<float>(phase_bits & 0xFFFFu);
			r[PF_ACTIVE1 + slot * 2] = publish
					? static_cast<float>((phase_bits >> 16) + 1u)
					: 0.0f;
		}
		if (!ae->inf.active) return;
		r[PF_ANIM_STATE] = static_cast<float>(ae->inf.anim_state);
		r[PF_ANIM_PHASE_TICKS] = static_cast<float>(ae->inf.clip_phase);
		if (ae->inf.body_blend_active()) {
			r[PF_ANIM_SOURCE_STATE] = static_cast<float>(ae->inf.anim_prev);
			r[PF_ANIM_SOURCE_PHASE_TICKS] =
					static_cast<float>(ae->inf.anim_prev_clip_phase);
			r[PF_ANIM_BLEND_WEIGHT] = ae->inf.anim_blend_weight;
		}
		// The upper-body weapon channel this body derived for itself; the gate
		// is the §14.8.6 consumer test and engine_flags bit 0x100 is the "is a
		// player" mirror of entity+0x24 (NPCs carry no hold ladder).
		if (opennova::world::infantry_weapon_channel_visible(
					ae->inf,
					(e.engine_flags & opennova::world::kEntityFlagPlayer) != 0,
					mount_blocks_weapon_channel(e))) {
			r[PF_WPN_ANIM_STATE] = static_cast<float>(ae->inf.wpn_state);
			r[PF_WPN_PHASE_TICKS] = static_cast<float>(ae->inf.wpn_clip_phase);
			r[PF_WPN_VARIANT] = static_cast<float>(ae->inf.wpn_variant);
			if (ae->inf.weapon_blend_active()) {
				r[PF_WPN_SOURCE_STATE] = static_cast<float>(ae->inf.wpn_prev);
				r[PF_WPN_SOURCE_PHASE_TICKS] =
						static_cast<float>(ae->inf.wpn_prev_clip_phase);
				r[PF_WPN_BLEND_WEIGHT] = ae->inf.wpn_blend_weight;
				r[PF_WPN_SOURCE_VARIANT] =
						static_cast<float>(ae->inf.wpn_prev_variant);
			}
		}
		const opennova::anim::AimOverlayInputs inputs = aim_overlay_inputs_for(*ae, e);
		opennova::anim::AimOverlayAngles angles[opennova::anim::kOverlayClassCount];
		opennova::anim::compute_aim_overlay_angles(inputs, angles);
		write_present_overlay(r, angles);
		// This body's third-person gun. Player rows only: retail's composition
		// gate is the Flags 0x100 player classifier.
		if ((e.engine_flags & opennova::world::kEntityFlagPlayer) != 0) {
			write_present_held_weapon(
					r, e.equipped_adm_index,
					(e.flags & opennova::world::kEntityFlagDead) != 0, inputs,
					ae->inf.wpn_state);
		}
	});
	return out;
}
