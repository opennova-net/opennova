// Simulation — presentation reads: entity/pose getters, the present-effect
// pose cache, the packed present snapshots (AI pool + client replicas), HUD views,
// and the drains (effects, fire, destruction, round impacts, tracers).
#include "simulation/simulation_internal.h"
#include "util/color_convert.h"
#include "simulation/hud_view_records.h"
#include "simulation/destruction_events.h"
#include "util/axes.h"

#include "simulation/entity_card.h" // the typed per-entity debug card (ADR 0042 d5)
#include "simulation/entity_row.h"  // one typed entity-directory row
#include "rtxt/rtxt_string_file.h" // the mission text table the objectives fill resolves through

#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/inmatch/client_replica_card.h> // the joiner's decoded replica section
#include <runtime/inmatch/minimap_markers.h> // the retained marker rows (bank walk + local restore)
#include <runtime/hud/hud_minimap_feed.h>  // the feed layout the snapshot carries
#include <runtime/inmatch/present_rows.h> // the PF_* row collectors, both roles (ADR 0043 G3)

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


void Simulation::advance_facial_presentation(const Vector3 &p_camera) {
    if (!kernel_ || !p_camera.is_finite()) return;
    std::vector<opennova::world::FacialDraw> rows;
    kernel_->world.facials.compile_draws(kernel_->world,
            static_cast<int32_t>(p_camera.x * 65536.0),
            static_cast<int32_t>(-p_camera.z * 65536.0),
            kernel_->local.view.camera_mode == 0, rows);
    // JO renders these targets then writes two unused globals. The complete
    // .text/.rdata/.data audit found no texture sink; model materials retain
    // their normal textures. Keep the frame counter/priority state live.
    // [orig: sub_580360 @0x580360; sub_580030 @0x580030;
    //  write-only globals @0x272137C/@0x2721380, world-wac-ai-re §33.30]
}

void Simulation::fill_minefield_draw_rows(
        std::vector<opennova::world::MinefieldDraw> &r_rows) const {
    r_rows.clear();
    if (kernel_) kernel_->world.minefields.compile_draws(kernel_->world, r_rows);
}

void Simulation::fill_throwable_visual_rows(
		std::vector<opennova::world::ThrowableVisualRow> &r_rows) const {
	r_rows.clear();
	if (!kernel_) return;
	const double kDegPerBam = opennova::world::kDegreesPerBam;
	auto push_entry = [&](int64_t key, int item_id, const opennova::world::Vec3 &pos,
			int32_t yaw_bam, int32_t pitch_bam, int32_t roll_bam,
			const char *move_effect, bool move_effect_live) {
		opennova::world::ThrowableVisualRow d;
		d.key = key;
		d.item_id = item_id;
		d.pos = pos;
		// the placer euler convention: rotation_deg = (pitch, MISSION yaw, roll)
		d.pitch_deg = static_cast<float>(double(pitch_bam) * kDegPerBam);
		d.yaw_deg = static_cast<float>(
				opennova::world::mission_yaw_deg_from_bam_heading(yaw_bam));
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
}

TypedArray<ThrowableVisualRow> Simulation::get_throwable_visuals() const {
	TypedArray<ThrowableVisualRow> out;
	std::vector<opennova::world::ThrowableVisualRow> rows;
	fill_throwable_visual_rows(rows);
	for (const opennova::world::ThrowableVisualRow &row : rows) {
		Ref<ThrowableVisualRow> d;
		d.instantiate();
		d->assign(row);
		out.push_back(d);
	}
	return out;
}

const opennova::particle::ParticleForceField *Simulation::particle_force_field() const {
	return kernel_ ? &kernel_->world.rotor_wash : nullptr;
}

void Simulation::fill_vehicle_trail_visual_rows(
		std::vector<opennova::world::VehicleTrailVisualRow> &r_rows) const {
	r_rows.clear();
	if (!kernel_)
		return;
	const auto &world = kernel_->world;
	world.registry.for_each([&](const opennova::world::Entity &entity) {
		if (!entity.alive || entity.veh.movement_effects_disabled ||
				((entity.flags | entity.engine_flags) & opennova::world::kEntityFlagDead) != 0)
			return;
		const auto *traits = world.vehicles.traits.get(entity.item_id);
		if (traits == nullptr)
			return;
		for (uint8_t i = 0; i < 16; ++i) {
			const auto &point = entity.veh.trails.points[i];
			if (point.definition == 0 || point.definition > 4)
				continue;
			opennova::world::VehicleTrailVisualRow row;
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

TypedArray<VehicleTrailVisualRow> Simulation::get_vehicle_trail_visuals() const {
	TypedArray<VehicleTrailVisualRow> out;
	std::vector<opennova::world::VehicleTrailVisualRow> rows;
	fill_vehicle_trail_visual_rows(rows);
	for (const opennova::world::VehicleTrailVisualRow &row : rows) {
		Ref<VehicleTrailVisualRow> wrapped;
		wrapped.instantiate();
		wrapped->assign(row);
		out.push_back(wrapped);
	}
	return out;
}

Ref<WaypointHudView> Simulation::get_waypoint_hud_view() const {
	// The current-waypoint slice of the per-frame HUD info rebuild, plus the
	// mission-scripted show gate. [orig: HUD_BuildEntityInfo @ 0x4b88b7..0x4b8914
	// (hudInfo+373 number, +400/404/408 position) + g_showWaypoints @ 0x27238BC]
	opennova::world::WaypointHudView v;
	const opennova::world::WaypointTrack *track = kernel_ ? &kernel_->world.script.waypoints : nullptr;
	v.show = track != nullptr && track->show;
	v.count = track ? static_cast<int>(track->entries.size()) : 0;
	const opennova::world::WaypointEntry *cur = track ? track->current_entry() : nullptr;
	v.current = cur ? static_cast<int>(track->current) : -1;
	// The record converts the entry's fixed 16.16 mission (x,y,z) to Godot
	// (x, z, -y), like every entity read.
	if (cur != nullptr) v.entry = *cur;
	Ref<WaypointHudView> out;
	out.instantiate();
	out->assign(v);
	return out;
}

Ref<HudMapGridOrigin> Simulation::get_hud_map_grid_origin() const {
	// The map grid-label origin: the mission's first type-2043 marker. The
	// host stashes it at promotion from the mission doc; a JOINER promotes a
	// marker-less wire-header BMS (D-NET-194), so its origin resolves from
	// the replicated pool-3 entity in the decoded view instead — the same
	// client-side pool scan retail's HUD init runs (witness at
	// World::map_grid_origin_x / HudMinimapInput::grid_origin_x).
	opennova::hud::HudMapGridOrigin v;
	v.present = kernel_ != nullptr && kernel_->world.tables.map_grid_origin_present;
	v.x_q16 = v.present ? kernel_->world.tables.map_grid_origin_x : 0;
	v.y_q16 = v.present ? kernel_->world.tables.map_grid_origin_y : 0;
	if (!v.present && runtime_ != nullptr) {
		v.present = opennova::replication::client_minimap_grid_origin(
				runtime_->state(), v.x_q16, v.y_q16);
	}
	Ref<HudMapGridOrigin> out;
	out.instantiate();
	out->assign(v);
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
	if (present_.minimap_snapshot_valid && revision == present_.minimap_snapshot_revision &&
			tick == present_.minimap_snapshot_tick &&
			local_marker_handle == present_.minimap_snapshot_local_handle) {
		return present_.minimap_snapshot_cache;
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
	present_.minimap_snapshot_cache = out;
	present_.minimap_snapshot_revision = revision;
	present_.minimap_snapshot_tick = tick;
	present_.minimap_snapshot_local_handle = local_marker_handle;
	present_.minimap_snapshot_valid = true;
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

void Simulation::fill_objectives(const Ref<RtxtStringFile> &p_mission_text,
		std::vector<opennova::hud::HudObjectiveRow> &r_rows) const {
	// The SP objectives panel's row walk: slots 1..8 until a 0/255 win id.
	// [orig: HUD_DrawWinConditions @0x5ba9e0 — byte_A7628B[slot] 0/255 break;
	//  row gate = show-win bit @0x5ba9ff; checkmark = won bit @0x5bab35]
	// The panel's resolved rows: the shown win-condition slots with their
	// mission-text lines and completed state (an empty row set hides the
	// panel — the retail toggle's off state). [orig: HUD_DrawWinConditions
	// @0x5ba940 — rows from the header table walk, text = mission
	// WinConditions/STRWINCOND%03i]
	r_rows.clear();
	if (!kernel_) return;
	const auto &sg = kernel_->world.script.subgoals;
	for (int slot = 1; slot <= 8; ++slot) {
		const uint8_t id = sg.win_text_ids[slot];
		if (id == 0 || id == 255) break;
		if ((sg.show_win & (1u << slot)) == 0) continue;
		opennova::hud::HudObjectiveRow row;
		const String key = vformat("STRWINCOND%03d", static_cast<int>(id));
		if (p_mission_text.is_valid() &&
				p_mission_text->has_string_in_section("WinConditions", StringName(key))) {
			row.text = p_mission_text->get_string_in_section("WinConditions", StringName(key))
							   .utf8()
							   .get_data();
		}
		row.done = (sg.won & (1u << slot)) != 0;
		r_rows.push_back(std::move(row));
	}
}
// Drain the round impacts the flight sim resolved since the last call, each row already
// resolved through the ammo effects_table (canonical tag -> {effect, sound}) and its
// per-leg presentation mask; rows with no enabled authored leg are dropped, matching
// the original impact presenter [orig: AmmoDef_ProcessImpactEffect @ 0x40a170;
// ballistic wrapper Projectile_SpawnImpactEffect @ 0x4e9b80; selection witness
// on world/round_sim.h RoundImpact].
void Simulation::drain_round_impact_rows(
		std::vector<opennova::world::RoundImpactPresentation> &r_rows) {
	r_rows.clear();
	if (!kernel_) return;
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
		opennova::world::RoundImpactPresentation d;
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
	kernel_->world.round_sim.impacts.clear();
}

TypedArray<RoundImpactRow> Simulation::drain_round_impacts() {
	TypedArray<RoundImpactRow> out;
	std::vector<opennova::world::RoundImpactPresentation> rows;
	drain_round_impact_rows(rows);
	for (const opennova::world::RoundImpactPresentation &row : rows) {
		Ref<RoundImpactRow> d;
		d.instantiate();
		d->assign(row);
		out.push_back(d);
	}
	return out;
}

void Simulation::drain_terrain_scorches(
		std::vector<opennova::world::TerrainScorchEvent> &r_events) {
	r_events.clear();
	if (!kernel_) return;
	// The events carry the mission 16.16 bounds; the terrain consumer folds
	// mission (x,y,z) -> Godot (x,z,-y) as it inserts them.
	r_events = kernel_->world.out.terrain_scorches.pending();
	kernel_->world.out.terrain_scorches.clear_pending();
}

std::vector<std::string> Simulation::script_effect_names() const {
    return world_installed_ ? kernel_->script_effect_catalog.interned_names() : std::vector<std::string>{};
}

void Simulation::drain_script_effects(std::vector<opennova::world::ScriptEffectEvent> &events) {
    events.clear();
    if (!world_installed_) return;
    events.swap(kernel_->world.out.script_effects);
}

TypedArray<MissionEffect> Simulation::drain_effects() {
	TypedArray<MissionEffect> out;
	if (!world_installed_) return out;
	for (const opennova::world::Effect &e : kernel_->world.out.effects.entries()) {
		// The vehicle_control_* lifecycle edges carry the wire handle in d;
		// every other kind leaves the alias absent.
		const bool control_edge = e.kind == "vehicle_control_started" ||
				e.kind == "vehicle_control_stopped";
		Ref<MissionEffect> d;
		d.instantiate();
		d->assign(e, control_edge ? e.d : -1);
		out.push_back(d);
	}
	kernel_->world.out.effects.clear();
	return out;
}

// The shell fire-presentation drain — see the header note. Direction math mirrors
// the round spawn's mission-frame forward (cos yaw * cp, sin yaw * cp, sin pitch)
// [orig: RoundData_SpawnRound @0x4ec5e9]; the rows cross in the mission frame
// and the consumer axis-maps mission -> godot (x, z, -y).
void Simulation::drain_fire_presentation_rows(
		std::vector<opennova::world::FirePresentationRow> &r_rows) {
	r_rows.clear();
	if (!world_installed_) return;
	constexpr double kRadPerBam = (2.0 * 3.14159265358979323846) / 4294967296.0;
	const bool have_local = kernel_->world.cached.local_player.valid();
	for (const opennova::world::FireEvent &fe : kernel_->world.round_sim.fired) {
		opennova::world::FirePresentationRow d;
		d.origin = fe.origin;
		// Retail's two receive arms are mutually exclusive and present differently.
		// Bit 0 is tested first; only when it is CLEAR and bit 1 is set does the
		// adm-indexed arm run, and that arm spawns no ammo-def sound or effect.
		// A zero flags byte is host/AI-originated fire, which keeps the ammo-def
		// legs because retail presents those inline at the shooter instead.
		// [orig: @0x42f521 / @0x42f6ce; ammo legs @0x42f5dc / @0x42f6c2]
		d.adm_arm = (fe.wire_round_flags & opennova::kRoundEventFlagAltFire) == 0 &&
				(fe.wire_round_flags & opennova::kRoundEventFlagAdmIndexed) != 0;
		d.adm_index = fe.adm_index;
		const double bearing = static_cast<double>(fe.yaw_bam) * kRadPerBam;
		const double pitch = static_cast<double>(fe.pitch_bam) * kRadPerBam;
		const double cp = std::cos(pitch);
		d.forward = opennova::world::Vec3{static_cast<float>(std::cos(bearing) * cp),
				static_cast<float>(std::sin(bearing) * cp), static_cast<float>(std::sin(pitch))};
		d.shooter_handle = static_cast<int32_t>(fe.shooter_handle);
		const opennova::world::Entity *shooter = kernel_->world.registry.get(fe.shooter);
		d.source_bms_id = shooter != nullptr ? shooter->bms_id : 0;
		d.is_local_player = have_local && fe.shooter == kernel_->world.cached.local_player;
		d.ammo_index = fe.ammo_index;
		const opennova::world::AmmoTableEntry *ammo = kernel_->world.tables.ammo.by_index(fe.ammo_index);
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
		//  g_weaponActionTable @0x830B90; the +684 call @0x42f777/@0x42f98f; the glow
		//  gate @0x40205e/@0x402080 with the context stamped 2 @0x42f8a0]
		const opennova::world::WeaponTableEntry *fired_def =
				kernel_->world.tables.weapons.by_index(fe.adm_index);
		const opennova::world::WeaponFsmAction *fire_row =
				fired_def != nullptr
						? &fired_def->action_fsm.actions[opennova::world::weapon_action::kFire]
						: nullptr;
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
	kernel_->world.round_sim.fired.clear();
}

TypedArray<FirePresentationEvent> Simulation::drain_fire_presentation_events() {
	TypedArray<FirePresentationEvent> out;
	std::vector<opennova::world::FirePresentationRow> rows;
	drain_fire_presentation_rows(rows);
	for (const opennova::world::FirePresentationRow &row : rows) {
		Ref<FirePresentationEvent> d;
		d.instantiate();
		d->assign(row);
		out.push_back(d);
	}
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
void Simulation::drain_fire_sounds(std::vector<opennova::world::ReadyFireSound> &r_sounds) {
	r_sounds.clear();
	if (!world_installed_) return;
	// Mission-space rows; the fire pass axis-maps (x, z, -y) as it plays them.
	r_sounds = kernel_->world.out.fire_sounds.drain();
}

// The destruction presentation drain (world/destruction.h; §24): one call per
// present, handing the sim's events to the C++ pass whole (the counters ride
// along: `crackles` is the wreck-fire crackle rolls fired, S12b). The events
// cross in mission space; the pass axis-maps mission (x, y, z-up) -> Godot
// (x, z, -y), the drain_fire_presentation_rows rule.
void Simulation::drain_vehicle_effects(std::vector<opennova::world::VehicleEffectEvent> &r_events) {
	r_events.clear();
	if (world_installed_)
		r_events.swap(kernel_->world.out.vehicle_effects);
}

void Simulation::drain_destruction_events(opennova::world::DestructionEvents &r_events) {
	r_events.clear();
	if (!world_installed_) return;
	opennova::world::DestructionEvents &ev = kernel_->world.out.destruction;
	r_events.effects.swap(ev.effects);
	r_events.sounds.swap(ev.sounds);
	r_events.husk_swaps.swap(ev.husk_swaps);
	r_events.death_lights.swap(ev.death_lights);
	r_events.explosions_processed = ev.explosions_processed;
	r_events.items_destroyed = ev.items_destroyed;
	r_events.crackles = ev.crackles;
	r_events.debris_triangles = ev.debris_triangles;
	r_events.glass_points = ev.glass_points;
	ev.clear();
}

// The live death-piece pool snapshot — the present pass renders each piece as
// its single husk-model section [orig: the piece render mask piece[31]; §24].
void Simulation::fill_death_pieces(std::vector<opennova::world::DeathPieceRow> &r_pieces) const {
	r_pieces.clear();
	if (!world_installed_) return;
	for (size_t slot = 0; slot < kernel_->world.death_pieces.pieces.size(); ++slot) {
		const opennova::world::DeathPiece &p = kernel_->world.death_pieces.pieces[slot];
		if (!p.active) continue;
		opennova::world::DeathPieceRow d;
		d.slot = static_cast<int32_t>(slot);
		d.generation = p.generation;
		d.item_id = p.item_id;
		d.section = static_cast<int32_t>(p.section);
		// The debris-type row names the trail effect through the ONE native
		// table (death_piece_trail_effect) [orig: g_death_piece_types
		// @ 0x8404f0 +0x2C]; "" = no trail authored.
		d.type_index = static_cast<int32_t>(p.type_index);
		d.scale = p.render_scale;
		d.pos = p.pos;
		d.heading = p.heading;
		d.pitch = p.pitch;
		d.settled = p.settled;
		r_pieces.push_back(std::move(d));
	}
}

// Whether the collision world holds an instance for the placed entity: the
// one destruction-gate fact the GUT collision cases read by bms_id.
bool Simulation::has_collision_instance(int p_bms_id) const {
	if (!kernel_) return false;
	const opennova::world::Entity *found = nullptr;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (found == nullptr && e.bms_id == p_bms_id) found = &e;
	});
	if (found == nullptr) return false;
	return kernel_->collision.has_instance(kernel_->world, found->handle);
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
void Simulation::fill_round_glows(std::vector<opennova::world::RoundGlowRow> &r_rows) const {
	r_rows.clear();
	if (!world_installed_) return;
	for (const opennova::world::LiveRound &r : kernel_->world.round_sim.rounds) {
		if (!r.active || r.ammo_index < 0) continue;
		const opennova::world::AmmoTableEntry *ammo =
				kernel_->world.tables.ammo.by_index(r.ammo_index);
		if (ammo == nullptr || ammo->light_move_radius <= 0.0f) continue;
		opennova::world::RoundGlowRow d;
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
	return opennova::world::inspect::entity_directory(kernel_->world, /*with_brains=*/!is_joiner());
}

opennova::world::inspect::EntityCard Simulation::native_entity_card(int p_handle) const {
	if (!kernel_ || p_handle < 0 || p_handle > 0xFFFF) return {};
	// The directory's rule: a joiner's non-authoritative tooling AI pool never
	// joins the decoded view, so the F3 card shows no AI half for a row the
	// list beside it calls brainless.
	return opennova::world::inspect::build_entity_card(
			kernel_->world, /*with_brains=*/!is_joiner(),
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
	if (is_joiner() && runtime_ != nullptr) {
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
	present_.effect_pose_cache_valid = false;
	present_.effect_pose_cache_runtime = nullptr;
	present_.effect_poses_by_handle.clear();
	present_.effect_handles_by_bms_id.clear();
	present_.effect_handles_by_ssn.clear();
	present_.effect_handles_by_origin.clear();
	present_.effect_missing_handles.clear();
	present_.effect_missing_bms_ids.clear();
	present_.effect_missing_ssns.clear();
	present_.effect_missing_origins.clear();
}

void Simulation::ensure_present_effect_pose_cache() const {
	if (!kernel_ || !runtime_) {
		if (present_.effect_pose_cache_valid) invalidate_present_effect_pose_cache();
		return;
	}

	const opennova::replication::ClientState &client = runtime_->state();
	const uint32_t logic_tick = kernel_->world.logic_tick;
	if (present_.effect_pose_cache_valid &&
			present_.effect_pose_cache_runtime == runtime_ &&
			present_.effect_pose_cache_logic_tick == logic_tick &&
			present_.effect_pose_cache_client_frame == client.frames_applied) {
		return;
	}

	present_.effect_poses_by_handle.clear();
	present_.effect_handles_by_bms_id.clear();
	present_.effect_handles_by_ssn.clear();
	present_.effect_handles_by_origin.clear();
	present_.effect_missing_handles.clear();
	present_.effect_missing_bms_ids.clear();
	present_.effect_missing_ssns.clear();
	present_.effect_missing_origins.clear();
	present_.effect_pose_cache_logic_tick = logic_tick;
	present_.effect_pose_cache_client_frame = client.frames_applied;
	present_.effect_pose_cache_runtime = runtime_;
	present_.effect_pose_cache_valid = true;
}

bool Simulation::cache_present_effect_pose(
		const opennova::replication::ClientEntityState &p_entity_state) const {
	// Match build_client_replica_present_rows' joiner self-filter: the host's
	// wire echo H is not drawn and therefore cannot own a presented effect.
	// Packed handle zero is a valid pool-0 identity, so presence rides the
	// runtime's explicit validity seam, never a zero sentinel.
	if (is_joiner() && runtime_ && runtime_->has_self_handle() &&
			p_entity_state.handle == runtime_->self_handle()) {
		return false;
	}
	if (present_.effect_poses_by_handle.find(p_entity_state.handle) !=
			present_.effect_poses_by_handle.end()) {
		return true;
	}

	const int32_t heading_bam = p_entity_state.heading_bam;
	// Host/listen presentation can recover the authored pitch and roll from the
	// authoritative registry. The compact peer row only carries yaw; joiners
	// therefore retain the wire-only zeroes here.
	const opennova::world::Entity *entity = is_joiner() ? nullptr : kernel_->world.registry.get(
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
	present_.effect_poses_by_handle[p_entity_state.handle] = pose;
	present_.effect_missing_handles.erase(p_entity_state.handle);

	// A joiner's decoded handles belong to the host, so only wire identity is
	// meaningful there. Host/listen views can resolve every alias from the same
	// registry entity used by get_present_snapshot().
	if (is_joiner() || !entity) return true;
	if (entity->bms_id > 0) {
		const int bms_id = static_cast<int>(entity->bms_id);
		present_.effect_handles_by_bms_id[bms_id] =
				p_entity_state.handle;
		present_.effect_missing_bms_ids.erase(bms_id);
	}
	if (entity->net_id > 0) {
		const int ssn = static_cast<int>(entity->net_id);
		present_.effect_handles_by_ssn[ssn] =
				p_entity_state.handle;
		present_.effect_missing_ssns.erase(ssn);
	}
	const int kind = opennova::world::spawn_origin_kind(entity->spawn_origin);
	const int index = static_cast<int>(opennova::world::spawn_origin_index(entity->spawn_origin));
	const uint64_t origin = present_effect_origin_key(kind, index);
	present_.effect_handles_by_origin[origin] =
			p_entity_state.handle;
	present_.effect_missing_origins.erase(origin);
	return true;
}

bool Simulation::cache_present_effect_pose(
		const opennova::world::Entity &p_entity) const {
	const uint16_t handle = p_entity.handle.packed;
	if (present_.effect_poses_by_handle.find(handle) !=
			present_.effect_poses_by_handle.end()) {
		return true;
	}
	const AiEntity *ae = kernel_->world.ai.for_handle(p_entity.handle);
	PresentEffectPose pose;
	pose.position = Vector3(p_entity.position.x, p_entity.position.z,
			-p_entity.position.y);
	pose.rotation_deg = Vector3(
			static_cast<float>(p_entity.pitch),
			static_cast<float>(opennova::inmatch::pool_present_yaw_deg(
					p_entity, ae, opennova::replication::entity_class_of(p_entity))),
			static_cast<float>(p_entity.roll));
	present_.effect_poses_by_handle[handle] = pose;
	present_.effect_missing_handles.erase(handle);
	if (p_entity.bms_id > 0) {
		const int bms_id = static_cast<int>(p_entity.bms_id);
		present_.effect_handles_by_bms_id[bms_id] = handle;
		present_.effect_missing_bms_ids.erase(bms_id);
	}
	if (p_entity.net_id > 0) {
		const int ssn = static_cast<int>(p_entity.net_id);
		present_.effect_handles_by_ssn[ssn] = handle;
		present_.effect_missing_ssns.erase(ssn);
	}
	const int kind = opennova::world::spawn_origin_kind(p_entity.spawn_origin);
	const int index = static_cast<int>(
			opennova::world::spawn_origin_index(p_entity.spawn_origin));
	const uint64_t origin = present_effect_origin_key(kind, index);
	present_.effect_handles_by_origin[origin] = handle;
	present_.effect_missing_origins.erase(origin);
	return true;
}

PackedVector3Array Simulation::cached_present_effect_state_for_handle(
		uint16_t p_handle) const {
	PackedVector3Array out;
	const auto found = present_.effect_poses_by_handle.find(p_handle);
	if (found == present_.effect_poses_by_handle.end()) return out;
	out.resize(EFFECT_STATE_COUNT);
	out.set(EFFECT_STATE_POSITION, found->second.position);
	out.set(EFFECT_STATE_ROTATION_DEG, found->second.rotation_deg);
	return out;
}

PackedVector3Array Simulation::present_effect_state_for_handle(uint16_t p_handle) const {
	ensure_present_effect_pose_cache();
	PackedVector3Array cached = cached_present_effect_state_for_handle(p_handle);
	if (!cached.is_empty() || !runtime_) return cached;
	if (present_.effect_missing_handles.find(p_handle) !=
			present_.effect_missing_handles.end()) {
		return PackedVector3Array();
	}
	if (!is_joiner()) {
		const opennova::world::Entity *entity =
				kernel_->world.registry.get(opennova::world::EntityHandle{p_handle});
		if (entity != nullptr && cache_present_effect_pose(*entity)) {
			return cached_present_effect_state_for_handle(p_handle);
		}
		present_.effect_missing_handles.insert(p_handle);
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
	present_.effect_missing_handles.insert(p_handle);
	return PackedVector3Array();
}

PackedVector3Array Simulation::get_present_effect_state_for_ssn(int p_ssn) const {
	if (p_ssn <= 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const auto found = present_.effect_handles_by_ssn.find(p_ssn);
	if (found != present_.effect_handles_by_ssn.end()) {
		return cached_present_effect_state_for_handle(found->second);
	}
	if (!runtime_ || is_joiner()) return PackedVector3Array();
	if (present_.effect_missing_ssns.find(p_ssn) !=
			present_.effect_missing_ssns.end()) {
		return PackedVector3Array();
	}
	const opennova::world::Entity *match = nullptr;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (match == nullptr && static_cast<int>(e.net_id) == p_ssn) match = &e;
	});
	if (match != nullptr && cache_present_effect_pose(*match)) {
		return cached_present_effect_state_for_handle(match->handle.packed);
	}
	present_.effect_missing_ssns.insert(p_ssn);
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
	const auto found = present_.effect_handles_by_bms_id.find(p_bms_id);
	if (found != present_.effect_handles_by_bms_id.end()) {
		return cached_present_effect_state_for_handle(found->second);
	}
	if (!runtime_ || is_joiner()) return PackedVector3Array();
	if (present_.effect_missing_bms_ids.find(p_bms_id) !=
			present_.effect_missing_bms_ids.end()) {
		return PackedVector3Array();
	}
	const opennova::world::Entity *entity =
			kernel_->world.registry.get(handle_for_bms_id(p_bms_id));
	if (entity != nullptr && cache_present_effect_pose(*entity)) {
		return cached_present_effect_state_for_handle(entity->handle.packed);
	}
	present_.effect_missing_bms_ids.insert(p_bms_id);
	return PackedVector3Array();
}

PackedVector3Array Simulation::get_present_effect_state_for_origin(
		int p_kind, int p_index) const {
	if (p_kind < 0 || p_index < 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const uint64_t requested_origin = present_effect_origin_key(p_kind, p_index);
	const auto found = present_.effect_handles_by_origin.find(requested_origin);
	if (found != present_.effect_handles_by_origin.end()) {
		return cached_present_effect_state_for_handle(found->second);
	}
	if (!runtime_ || is_joiner()) return PackedVector3Array();
	if (present_.effect_missing_origins.find(requested_origin) !=
			present_.effect_missing_origins.end()) {
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
	present_.effect_missing_origins.insert(requested_origin);
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
	if (runtime_ && kernel_) {
		const opennova::inmatch::PresentRowsContext context{*kernel_, runtime_, is_joiner()};
		if (is_joiner()) {
			opennova::inmatch::build_client_replica_present_rows(context, present_.rows_scratch);
			// Consume-once: each transition pulse dispatches exactly one presented
			// frame (the rows copied any live pulse into PF_ANIM_STATE_PULSE).
			runtime_->state().clear_anim_pulses();
		} else {
			opennova::inmatch::build_world_present_rows(
					context, present_.pool_lifecycle, present_.rows_scratch);
		}
		out.resize(static_cast<int64_t>(present_.rows_scratch.size()));
		if (!present_.rows_scratch.empty())
			std::memcpy(out.ptrw(), present_.rows_scratch.data(),
					present_.rows_scratch.size() * sizeof(float));
	}
	present_.last_entity_count = static_cast<int>(out.size() / PF_STRIDE);
	std::vector<PresentRowIdentity> next_layout;
	next_layout.reserve(static_cast<std::size_t>(present_.last_entity_count));
	const float *rows = out.ptr();
	for (int i = 0; i < present_.last_entity_count; ++i) {
		const float *row = rows + static_cast<int64_t>(i) * PF_STRIDE;
		next_layout.push_back(PresentRowIdentity{
				static_cast<int32_t>(row[PF_WIRE_HANDLE]),
				static_cast<int32_t>(row[PF_TYPE_ID]),
				static_cast<int32_t>(row[PF_BMS_ID]),
				static_cast<int32_t>(row[PF_KIND]),
				static_cast<int32_t>(row[PF_INDEX])});
	}
	if (next_layout != present_.layout) {
		present_.layout = std::move(next_layout);
		++present_.layout_revision;
	}
	if (runtime_profiling_enabled_)
		present_.last_snapshot_us = opennova::io::perf_now_us() - start_us;
	return out;
}

void Simulation::fill_water_wake_frame(
		const Vector3 &camera, opennova::renderer::WaterWakeFrame &frame) const {
	frame.clear();
	if (!kernel_)
		return;
	const int32_t position[3] = { opennova::world::to_fixed(camera.x),
		opennova::world::to_fixed(-camera.z), opennova::world::to_fixed(camera.y) };
	const auto &world = kernel_->world;
	opennova::renderer::compile_water_wakes(
			world.rotor_wash.water_wakes(), world.env.water_z, world.logic_tick, position, frame);
}
