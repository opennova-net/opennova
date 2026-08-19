// Simulation — presentation reads: entity/pose getters, the present-effect
// pose cache, the packed present snapshots (AI pool + client replicas), HUD views,
// and the drains (effects, fire, destruction, round impacts, tracers).
#include "simulation/nova_simulation_internal.h"
#include <npruntime/client_replica_present_projection.h> // the canonical decoded-client projection (ADR 0031)

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <netsim/client_state.h> // minimap_team_argb (the ONE palette home)
#include <npwire/ingame_decode.h> // kRoundEventFlag* (the fire-mode byte)
#include <world/minimap_footprint.h> // the OOBJ occlusion ground-slice footprint mesh
#include <world/minimap_overlay.h>   // classifier + the blip draw policy
#include <renderer/tracer_frame.h> // the styled tracer-ribbon compile
#include <world/entity.h> // kEntityFlag* (the wire state_flags byte IS entity+36 low)

using namespace novasim;

namespace {

inline void write_present_section_mask(float *row, uint32_t hidden_mask) {
	row[Simulation::PF_SECTION_MASK_VALID] = 1.0f;
	row[Simulation::PF_SECTION_MASK_LO] =
			static_cast<float>(hidden_mask & 0xFFFFu);
	row[Simulation::PF_SECTION_MASK_HI] =
			static_cast<float>((hidden_mask >> 16) & 0xFFFFu);
}

} // namespace

Array Simulation::get_throwable_visuals() const {
	Array out;
	if (!world_) return out;
	const double kDegPerBam = opennova::world::kDegreesPerBam;
	auto push_entry = [&](int64_t key, int item_id, const opennova::world::Vec3 &pos,
			int32_t yaw_bam, int32_t pitch_bam, int32_t roll_bam,
			const char *move_effect) {
		Dictionary d;
		d["key"] = key;
		d["item_id"] = item_id;
		d["pos"] = Vector3(pos.x, pos.z, -pos.y);
		// the placer euler convention: rotation_deg = (pitch, MISSION yaw, roll)
		d["rotation_deg"] = Vector3(
				static_cast<float>(double(pitch_bam) * kDegPerBam),
				static_cast<float>(
						opennova::world::mission_yaw_deg_from_bam_heading(yaw_bam)),
				static_cast<float>(double(roll_bam) * kDegPerBam));
		// effects_table tag 1 ("move") is a round-bound particle, not an
		// impact. Retail copies it to AmmoDef+0x70 [orig: @0x409fc2],
		// spawns/updates it through round+0x1cc [orig:
		// @0x4e9f58/@0x4ea8ae/@0x5f7410], then releases it with the round
		// [orig: Projectile_ReleaseEffects @0x4e8280].
		d["move_effect"] = String(move_effect != nullptr ? move_effect : "");
		out.push_back(d);
	};
	for (int i = 0; i < opennova::world::RoundSim::kCapacity; ++i) {
		const opennova::world::LiveRound &r =
				world_->round_sim.rounds[static_cast<size_t>(i)];
		if (!r.active) continue;
		const opennova::world::AmmoTableEntry *ammo =
				world_->ammo.by_index(r.ammo_index);
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
				move_effect);
	}
	uint8_t viewer_team = 0xFF;
	if (const opennova::world::Entity *lp =
			world_->registry.get(world_->cached.local_player))
		viewer_team = static_cast<uint8_t>(lp->team);
	for (const opennova::world::PlacedDevice &d : world_->throwables.devices) {
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
				d.roll_bam, "");
	}
	return out;
}

Dictionary Simulation::get_waypoint_hud_view() const {
	// The current-waypoint slice of the per-frame HUD info rebuild, plus the
	// mission-scripted show gate. [orig: HUD_BuildEntityInfo @ 0x4b88b7..0x4b8914
	// (hudInfo+373 number, +400/404/408 position) + g_showWaypoints @ 0x27238BC]
	Dictionary out;
	const opennova::world::WaypointTrack *track = world_ ? &world_->waypoints : nullptr;
	out["show"] = track != nullptr && track->show;
	out["count"] = track ? static_cast<int>(track->entries.size()) : 0;
	const opennova::world::WaypointEntry *cur = track ? track->current_entry() : nullptr;
	out["current"] = cur ? track->current : -1;
	out["number"] = cur ? track->current + 1 : 0;
	out["name_id"] = cur ? cur->name_id : 0;
	// Fixed 16.16 mission (x,y,z) -> Godot (x, z, -y), like every entity read.
	out["position"] = cur ? Vector3(cur->x / 65536.0f, cur->z / 65536.0f, -(cur->y / 65536.0f))
						  : Vector3();
	out["done"] = cur != nullptr && cur->done;
	return out;
}

Dictionary Simulation::get_hud_map_grid_origin() const {
	// The map grid-label origin: the mission's first type-2043 marker. The
	// host stashes it at promotion from the mission doc; a JOINER promotes a
	// marker-less wire-header BMS (D-NET-194), so its origin resolves from
	// the replicated pool-3 entity in the decoded view instead — the same
	// client-side pool scan retail's HUD init runs (witness at
	// World::map_grid_origin_x / HudMinimapInput::grid_origin_x).
	Dictionary out;
	bool present = world_ != nullptr && world_->map_grid_origin_present;
	int32_t x_q16 = present ? world_->map_grid_origin_x : 0;
	int32_t y_q16 = present ? world_->map_grid_origin_y : 0;
	if (!present && runtime_ != nullptr) {
		present = opennova::netsim::client_minimap_grid_origin(
				runtime_->state(), x_q16, y_q16);
	}
	out["present"] = present;
	out["position"] = present
			? Vector3(x_q16 / 65536.0f, 0.0f, -(y_q16 / 65536.0f))
			: Vector3();
	return out;
}

PackedInt32Array Simulation::get_hud_minimap_snapshot() const {
	const opennova::world::Entity *local_player = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
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
	const uint64_t tick = world_ != nullptr
			? static_cast<uint64_t>(world_->logic_tick) : 0;
	if (minimap_snapshot_valid_ && revision == minimap_snapshot_revision_ &&
			tick == minimap_snapshot_tick_ &&
			local_marker_handle == minimap_snapshot_local_handle_) {
		return minimap_snapshot_cache_;
	}
	PackedInt32Array out;
	bool retained_local_player = false;
	int count = 0;
	if (runtime_) {
		const opennova::netsim::ClientMinimapState &map =
				runtime_->state().minimap;
		auto count_bank = [&](const auto &bank, bool regular) {
			for (const auto &slot : bank) {
				if (!slot.active) continue;
				++count;
				if (regular && slot.entity_known &&
						slot.handle == local_marker_handle)
					retained_local_player = true;
			}
		};
		count_bank(map.transient, true);
		count_bank(map.persistent, true);
		count_bank(map.special, false);
	}
	// Retail registers the locally deployed player in a regular retained bank.
	// The loopback client does not receive that client-local registration, so
	// restore it here unless a decoded regular row already covers the same wire
	// handle. The draw-call probe confirms cell 3, team-table blue, and the
	// ordinary 6px-floor path at map center.
	// Retail witness: render_minimap_slot_blip @0x5BE240 ->
	// draw_minimap_blip, regular TSDicon submit @0x597F73. See hud-re.md.
	const bool append_local_player = local_player != nullptr &&
			local_marker_handle != opennova::world::EntityHandle::kInvalid &&
			!retained_local_player;
	if (append_local_player) ++count;

	out.resize(HUD_MINIMAP_HEADER_SIZE + count * HUD_MINIMAP_STRIDE);
	int32_t *write = out.ptrw();
	write[0] = HUD_MINIMAP_SNAPSHOT_VERSION;
	write[1] = HUD_MINIMAP_STRIDE;
	write[2] = count;

	int row = 0;
	auto append_overlay_bank = [&](const auto &bank, int bank_id) {
		for (const opennova::netsim::ClientMinimapOverlaySlot &slot : bank) {
			if (!slot.active) continue;
			int32_t *dst = write + HUD_MINIMAP_HEADER_SIZE +
					row++ * HUD_MINIMAP_STRIDE;
			dst[0] = bank_id;
			dst[1] = slot.handle;
			dst[2] = slot.x;
			dst[3] = slot.y;
			dst[4] = slot.z;
			dst[5] = slot.heading_bam;
			dst[6] = slot.param;
			dst[7] = static_cast<int32_t>(slot.argb);
			dst[8] = slot.flags;
			dst[9] = slot.source;
			dst[10] = slot.remaining_ticks;
			dst[11] = slot.entity_known ? 1 : 0;
			// The draw policy resolves against the LOCAL entity (host: the
			// live registry; joiner: the materialized twin) — retail reads
			// the pool slot's def at draw time the same way (witness at
			// world::minimap_blip_draw_policy). Absent entity -> the
			// rotated fallback on the class table.
			opennova::world::MinimapBlipDrawPolicy policy;
			if (world_) {
				const opennova::world::Entity *entity = world_->registry.get(
						opennova::world::EntityHandle{slot.handle});
				if (entity != nullptr) {
					policy = opennova::world::minimap_blip_draw_policy(
							*entity, slot.param);
				} else {
					policy.half_x_q16 = 0;
					policy.half_y_q16 = 0;
				}
			} else {
				policy.half_x_q16 = 0;
				policy.half_y_q16 = 0;
			}
			dst[12] = (policy.rotate ? 1 : 0) | (policy.footprint ? 2 : 0);
			dst[13] = policy.half_x_q16;
			dst[14] = policy.half_y_q16;
			dst[15] = policy.floor_px;
		}
	};
	if (runtime_) {
		const opennova::netsim::ClientMinimapState &map = runtime_->state().minimap;
		append_overlay_bank(map.transient,
				static_cast<int>(opennova::hud::HudMinimapBank::kTransient));
		append_overlay_bank(map.persistent,
				static_cast<int>(opennova::hud::HudMinimapBank::kPersistent));
		append_overlay_bank(map.special,
				static_cast<int>(opennova::hud::HudMinimapBank::kSpecial));
	}
	if (append_local_player) {
		int32_t *dst = write + HUD_MINIMAP_HEADER_SIZE +
				row++ * HUD_MINIMAP_STRIDE;
		const opennova::world::MinimapBlipDrawPolicy policy =
				opennova::world::minimap_blip_draw_policy(*local_player, 3);
		dst[0] = static_cast<int>(
				opennova::hud::HudMinimapBank::kPersistent);
		dst[1] = local_marker_handle;
		dst[2] = opennova::world::to_fixed(local_player->position.x);
		dst[3] = opennova::world::to_fixed(local_player->position.y);
		dst[4] = opennova::world::to_fixed(local_player->position.z);
		dst[5] = static_cast<int32_t>(get_local_player_heading_bam());
		dst[6] = 3; // live Person classification -> TSDicon cell 3
		dst[7] = static_cast<int32_t>(
				opennova::netsim::minimap_team_argb(local_player->team));
		dst[8] = 0x10; // regular persistent bank
		dst[9] = local_player->zone_number;
		dst[10] = 0; // regular slots draw at zero lifetime
		dst[11] = 1; // the local entity is necessarily resolved
		dst[12] = (policy.rotate ? 1 : 0) | (policy.footprint ? 2 : 0);
		dst[13] = policy.half_x_q16;
		dst[14] = policy.half_y_q16;
		dst[15] = policy.floor_px;
	}
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
	if (!world_) return out;
	int count = 0;
	std::unordered_map<int32_t, opennova::world::MinimapFootprintMesh> meshes;
	world_->registry.for_each([&](const opennova::world::Entity &entity) {
		if (!opennova::world::minimap_overlay_entity_enabled(entity)) return;
		const opennova::world::MinimapOverlayClassification row =
				opennova::world::classify_minimap_overlay(entity);
		if (!row.visible) return;
		const opennova::world::MinimapBlipDrawPolicy policy =
				opennova::world::minimap_blip_draw_policy(entity, row.icon);
		if (!policy.footprint) return;
		const int32_t model_id = occlusion_world_.instance_model_id(
				entity.handle);
		if (model_id < 0) return;
		auto mesh_it = meshes.find(model_id);
		if (mesh_it == meshes.end()) {
			const opennova::world::OcclusionModel *model =
					occlusion_world_.model(model_id);
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

Array Simulation::get_objectives_view() const {
	// The SP objectives panel's row walk: slots 1..8 until a 0/255 win id.
	// [orig: HUD_DrawWinConditions @0x5ba9e0 — byte_A7628B[slot] 0/255 break;
	//  row gate = show-win bit @0x5ba9ff; checkmark = won bit @0x5bab35]
	Array out;
	if (!world_) return out;
	const auto &sg = world_->subgoals;
	for (int slot = 1; slot <= 8; ++slot) {
		const uint8_t id = sg.win_text_ids[slot];
		if (id == 0 || id == 255) break;
		Dictionary row;
		row["slot"] = slot;
		row["text_id"] = static_cast<int>(id);
		row["shown"] = (sg.show_win & (1u << slot)) != 0;
		row["done"] = (sg.won & (1u << slot)) != 0;
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
Array Simulation::drain_round_impacts() {
	Array out;
	if (!world_) return out;
	const uint32_t now = world_->logic_tick;
	for (const opennova::world::RoundImpact &imp : world_->round_sim.impacts) {
		const opennova::world::AmmoTableEntry *ammo = world_->ammo.by_index(imp.ammo_index);
		if (ammo == nullptr) continue;
		if (imp.effect_tag < 0 || imp.effect_tag >= opennova::world::kImpactEffectTagCount)
			continue;
		const opennova::world::AmmoImpactEffectRow &row = ammo->impact_effects[imp.effect_tag];
		const bool has_effect = imp.present_effect && !row.effect.empty();
		const bool has_sound = imp.present_sound && !row.sound.empty();
		if (!has_effect && !has_sound) continue;
		Dictionary d;
		// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position convention.
		d["position"] = Vector3(imp.position.x, imp.position.z, -imp.position.y);
		d["direction"] = Vector3(imp.direction.x, imp.direction.z, -imp.direction.y);
		d["effect"] = has_effect ? String::utf8(row.effect.c_str()) : String();
		d["sound"] = has_sound ? String::utf8(row.sound.c_str()) : String();
		// A lifecycle rewind must never turn a future/stale source tick into an
		// unsigned multi-billion-tick particle pre-age request.
		const uint32_t age_ticks = now >= imp.tick ? now - imp.tick : 0u;
		d["age_ticks"] = static_cast<int64_t>(age_ticks);
		d["source_tick"] = static_cast<int64_t>(imp.tick);
		d["source_order"] = static_cast<int64_t>(imp.source_order);
		// The impact flash light rides the effect leg's own gate — retail
		// requires the effect entry AND the ammo light_impact radius (the
		// witness map on renderer/light_scene.h).
		if (has_effect && ammo->light_impact_radius > 0.0f) {
			d["light_radius"] = ammo->light_impact_radius;
			d["light_color"] = Color(
					static_cast<float>((ammo->light_impact_color >> 16) & 0xFF) / 255.0f,
					static_cast<float>((ammo->light_impact_color >> 8) & 0xFF) / 255.0f,
					static_cast<float>(ammo->light_impact_color & 0xFF) / 255.0f);
			d["light_ticks"] = ammo->light_impact_ticks;
		}
		out.push_back(d);
	}
	world_->round_sim.impacts.clear();
	return out;
}

Array Simulation::drain_effects() {
	Array out;
	if (!world_installed_) return out;
	for (const opennova::world::Effect &e : world_->effects.entries()) {
		Dictionary d;
		d["kind"] = String(e.kind.c_str());
		d["a"] = e.a;
		d["b"] = e.b;
		d["c"] = e.c;
		d["d"] = e.d;
		if (e.kind == "vehicle_control_started" ||
				e.kind == "vehicle_control_stopped")
			d["wire_handle"] = e.d;
		d["str"] = String(e.str.c_str());
		out.push_back(d);
	}
	world_->effects.clear();
	return out;
}

// The shell fire-presentation drain — see the header note. Direction math mirrors
// the round spawn's mission-frame forward (cos yaw * cp, sin yaw * cp, sin pitch)
// [orig: RoundData_SpawnRound @0x4ec5e9], axis-mapped mission -> godot (x, z, -y).
Array Simulation::drain_fire_presentation_events() {
	Array out;
	if (!world_installed_) return out;
	constexpr double kRadPerBam = (2.0 * 3.14159265358979323846) / 4294967296.0;
	const bool have_local = world_->cached.local_player.valid();
	for (const opennova::world::FireEvent &fe : world_->round_sim.fired) {
		Dictionary d;
		d["origin"] = Vector3(fe.origin.x, fe.origin.z, -fe.origin.y);
		// Retail's two receive arms are mutually exclusive and present differently.
		// Bit 0 is tested first; only when it is CLEAR and bit 1 is set does the
		// adm-indexed arm run, and that arm spawns no ammo-def sound or effect.
		// A zero flags byte is host/AI-originated fire, which keeps the ammo-def
		// legs because retail presents those inline at the shooter instead.
		// [orig: @0x42f521 / @0x42f6ce; ammo legs @0x42f5dc / @0x42f6c2]
		d["adm_arm"] = (fe.wire_round_flags & opennova::kRoundEventFlagAltFire) == 0 &&
				(fe.wire_round_flags & opennova::kRoundEventFlagAdmIndexed) != 0;
		d["adm_index"] = fe.adm_index;
		const double bearing = static_cast<double>(fe.yaw_bam) * kRadPerBam;
		const double pitch = static_cast<double>(fe.pitch_bam) * kRadPerBam;
		const double cp = std::cos(pitch);
		d["forward"] = Vector3(static_cast<real_t>(std::cos(bearing) * cp),
				static_cast<real_t>(std::sin(pitch)),
				static_cast<real_t>(-std::sin(bearing) * cp));
		d["shooter_handle"] = static_cast<int>(fe.shooter_handle);
		const opennova::world::Entity *shooter = world_->registry.get(fe.shooter);
		d["source_bms_id"] = shooter != nullptr ? shooter->bms_id : 0;
		d["is_local_player"] = have_local && fe.shooter == world_->cached.local_player;
		d["ammo_index"] = fe.ammo_index;
		const opennova::world::AmmoTableEntry *ammo = world_->ammo.by_index(fe.ammo_index);
		d["effect"] = ammo ? String(ammo->ai_launch_effect.c_str()) : String();
		d["mf_light"] = ammo ? ammo->mf_light : 0;
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
				world_->weapons.by_index(fe.adm_index);
		const opennova::world::WeaponFsmAction *fire_row =
				fired_def != nullptr
						? &fired_def->action_fsm.actions[opennova::world::weapon_action::kFire]
						: nullptr;
		d["action_effect"] = fire_row ? String(fire_row->particle) : String();
		// Resolved against the THIRD-PERSON model (gfx3): ActionDef+57 is the gfx3
		// userpoint index and +56 the gfx1 one — the opposite way round from three
		// currently-tracked doc lines. [orig: loader @0x54506c/@0x545092, resolver
		//  @0x54039e/@0x54040f]
		d["action_userpoint"] =
				fire_row ? String(fire_row->particle_userpoint) : String();
		// The 3P adm-arm anchor is the SHELL's: the rendered held-weapon node's
		// own userpoint (WirePresentPass.muzzle_world_for), which is what retail
		// spawns at — the muzzle-authority decision that closed the S12a
		// sim-posed shadow seam. The event carries the row's userpoint name; the
		// presentation layer resolves it against the node it renders.
		out.push_back(d);
	}
	world_->round_sim.fired.clear();
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
	world_->fire_sounds.set_listener(opennova::world::Vec3{
			p_listener_godot.x, -p_listener_godot.z, p_listener_godot.y});
}

// The ready fire-sound drain: immediate near shots, the adm-arm action-row
// sets, and expired propagation-delayed slots, in play order on the logic
// clock. The shell plays each row positionally; the set's max-range cull
// stays at play time in the audio bank (D-AI-8).
// [orig: Entity_PlaySound3D_FullVolume @ 0x528e20 / the pending drain
//  Sound_TickPendingSlots @ 0x529310]
Array Simulation::drain_fire_sounds() {
	Array out;
	if (!world_installed_) return out;
	for (const opennova::world::ReadyFireSound &sound :
			world_->fire_sounds.drain()) {
		Dictionary d;
		d["set"] = String(sound.set_name.c_str());
		d["pos"] = Vector3(sound.pos.x, sound.pos.z, -sound.pos.y);
		d["source_bms_id"] = sound.source_bms_id;
		out.push_back(d);
	}
	return out;
}

// The destruction presentation drain (world/destruction.h; §24): one call per
// present, converting the sim's events into godot-space dictionaries. Mission
// (x, y, z-up) -> Godot (x, z, -y), the drain_fire_presentation_events rule.
Dictionary Simulation::drain_destruction_events() {
	Dictionary out;
	if (!world_installed_) return out;
	opennova::world::DestructionEvents &ev = world_->destruction;
	auto to_godot = [](const opennova::world::Vec3 &v) {
		return Vector3(v.x, v.z, -v.y);
	};
	Array effects;
	for (const opennova::world::DestructionEffectEvent &e : ev.effects) {
		Dictionary d;
		d["effect"] = String(e.effect.c_str());
		d["pos"] = to_godot(e.pos);
		d["dir"] = to_godot(e.dir);
		d["attach_net_id"] = static_cast<int>(e.attach_net_id);
		d["attach_bms_id"] = e.attach_bms_id;
		d["attach_wire_handle"] = static_cast<int>(e.attach_wire_handle);
		d["attach_spawn_origin"] =
				static_cast<int64_t>(e.attach_spawn_origin);
		d["family"] = static_cast<int>(e.family);
		effects.push_back(d);
	}
	Array sounds;
	for (const opennova::world::DestructionSoundEvent &s : ev.sounds) {
		Dictionary d;
		d["sound"] = String(s.sound.c_str());
		d["pos"] = to_godot(s.pos);
		sounds.push_back(d);
	}
	Array husks;
	for (const opennova::world::HuskSwapEvent &h : ev.husk_swaps) {
		Dictionary d;
		d["net_id"] = static_cast<int>(h.net_id);
		d["wire_handle"] = static_cast<int>(h.wire_handle);
		d["bms_id"] = h.bms_id;
		d["spawn_origin"] = static_cast<int64_t>(h.spawn_origin);
		d["item_id"] = h.item_id;
		d["spawned_piece_mask"] = static_cast<int64_t>(h.spawned_piece_mask);
		d["pos"] = to_godot(h.pos);
		husks.push_back(d);
	}
	Array death_lights;
	for (const opennova::world::DeathLightEvent &l : ev.death_lights) {
		Dictionary d;
		d["pos"] = to_godot(l.pos);
		d["radius"] = l.radius;
		death_lights.push_back(d);
	}
	out["effects"] = effects;
	out["sounds"] = sounds;
	out["husk_swaps"] = husks;
	out["death_lights"] = death_lights;
	out["explosions_processed"] = ev.explosions_processed;
	out["items_destroyed"] = ev.items_destroyed;
	out["crackles"] = ev.crackles; // wreck-fire crackle rolls fired (S12b)
	out["debris_triangles"] = ev.debris_triangles;
	out["glass_points"] = ev.glass_points;
	ev.clear();
	return out;
}

// The live death-piece pool snapshot — the present pass renders each piece as
// its single husk-model section [orig: the piece render mask piece[31]; §24].
Array Simulation::get_death_pieces() const {
	Array out;
	if (!world_installed_) return out;
	for (size_t slot = 0; slot < world_->death_pieces.pieces.size(); ++slot) {
		const opennova::world::DeathPiece &p = world_->death_pieces.pieces[slot];
		if (!p.active) continue;
		Dictionary d;
		d["slot"] = static_cast<int>(slot);
		d["generation"] = static_cast<int64_t>(p.generation);
		d["item_id"] = p.item_id;
		d["section"] = static_cast<int>(p.section);
		d["type_index"] = static_cast<int>(p.type_index);
		// The debris-type trail effect, from the ONE native table [orig:
		// g_death_piece_types @ 0x8404f0 +0x2C]; "" = no trail authored.
		d["trail"] = String(
				opennova::world::death_piece_trail_effect(p.type_index));
		d["scale"] = p.render_scale;
		d["pos"] = Vector3(p.pos.x, p.pos.z, -p.pos.y);
		d["heading"] = p.heading;
		d["pitch"] = p.pitch;
		d["settled"] = p.settled;
		out.push_back(d);
	}
	return out;
}

// Per-entity destruction diagnostics (probe/F3 seam): the gate inputs the
// damage chain reads, resolved by bms_id. {} = no such entity.
Dictionary Simulation::get_destruction_debug(int p_bms_id) const {
	Dictionary out;
	if (!world_) return out;
	const opennova::world::Entity *found = nullptr;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (found == nullptr && e.bms_id == p_bms_id) found = &e;
	});
	if (found == nullptr) return out;
	out["bms_id"] = found->bms_id;
	out["net_id"] = static_cast<int>(found->net_id);
	out["kind"] = static_cast<int>(found->kind);
	out["pool"] = found->handle.pool();
	out["item_id"] = found->item_id;
	out["health"] = found->health;
	out["health_max"] = found->health_max;
	out["alive"] = found->alive;
	out["bound_radius"] = found->bound_radius;
	out["engine_flags"] = static_cast<int64_t>(found->engine_flags);
	out["is_ai_capable"] = found->is_ai_capable;
	out["has_collision_instance"] =
			collision_world_.has_instance(*world_, found->handle);
	const opennova::world::ItemDeathTraits *t =
			world_->item_death_traits.get(found->item_id);
	out["has_death_traits"] = t != nullptr;
	if (t != nullptr) {
		out["armor_impact"] = t->armor_impact;
		out["armor_blast"] = t->armor_blast;
		out["unit_type"] = t->unit_type;
		out["kz"] = t->kz;
		out["has_husk"] = t->has_husk;
		out["husk_model_loaded"] = t->husk_model_loaded;
		PackedVector3Array kz_points;
		for (const opennova::world::Vec3 &point : t->kz_points)
			kz_points.push_back(Vector3(point.x, point.y, point.z));
		out["kz_point_count"] = static_cast<int64_t>(t->kz_points.size());
		out["kz_points"] = kz_points;
		PackedVector3Array bridge_dead_points;
		for (const opennova::world::Vec3 &point : t->bridge_dead_points)
			bridge_dead_points.push_back(Vector3(point.x, point.y, point.z));
		out["bridge_dead_point_count"] =
				static_cast<int64_t>(t->bridge_dead_points.size());
		out["bridge_dead_points"] = bridge_dead_points;
		PackedVector3Array glass_positions;
		PackedVector3Array glass_directions;
		for (const opennova::world::GlassPointTrait &point : t->glass_points) {
			glass_positions.push_back(Vector3(
					point.local_pos.x, point.local_pos.y, point.local_pos.z));
			glass_directions.push_back(Vector3(
					point.local_dir.x, point.local_dir.y, point.local_dir.z));
		}
		out["glass_point_count"] =
				static_cast<int64_t>(t->glass_points.size());
		out["glass_point_positions"] = glass_positions;
		out["glass_point_directions"] = glass_directions;
	}
	out["pos"] = Vector3(found->position.x, found->position.z, -found->position.y);
	return out;
}

// The live tracer trail channels for the ribbon layer — see the header note.
// Mission -> godot axis map (x, z, -y), matching the other presentation drains.
PackedFloat32Array Simulation::get_tracer_trails() const {
	PackedFloat32Array out;
	if (!world_installed_) return out;
	for (const opennova::world::TracerTrailChannel &c : world_->round_sim.trails.channels) {
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
Array Simulation::get_round_glow_rows() const {
	Array out;
	if (!world_installed_) return out;
	for (const opennova::world::LiveRound &r : world_->round_sim.rounds) {
		if (!r.active || r.ammo_index < 0) continue;
		const opennova::world::AmmoTableEntry *ammo =
				world_->ammo.by_index(r.ammo_index);
		if (ammo == nullptr || ammo->light_move_radius <= 0.0f) continue;
		Dictionary d;
		d["id"] = static_cast<int64_t>(r.presentation_generation);
		// The spawn rides radius/2 above the round and the per-tick follow
		// re-centers at the round position (retail: @0x4ec8d6 / @0x4eaa9f,
		// see renderer/light_scene.h).
		d["pos"] = Vector3(r.pos.x, r.pos.z, -r.pos.y);
		d["radius"] = ammo->light_move_radius;
		d["color"] = Color(
				static_cast<float>((ammo->light_move_color >> 16) & 0xFF) / 255.0f,
				static_cast<float>((ammo->light_move_color >> 8) & 0xFF) / 255.0f,
				static_cast<float>(ammo->light_move_color & 0xFF) / 255.0f);
		out.push_back(d);
	}
	return out;
}

// The styled half of the trail split: rows in, per-family strip runs out.
// Family packing (positions + colors arrays) is transport shape only; the
// geometry/color math lives in renderer/tracer_frame.cpp with its citations.
Dictionary Simulation::compile_tracer_ribbons(const PackedFloat32Array &rows,
		const Vector3 &camera) {
	std::vector<renderer::TracerChannelInput> channels;
	const float *r = rows.ptr();
	const int64_t size = rows.size();
	int64_t i = 0;
	while (r != nullptr && i + 2 < size) {
		renderer::TracerChannelInput c;
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
	renderer::TracerRibbonFrame frame;
	renderer::compile_tracer_ribbons(channels.data(), channels.size(),
			{static_cast<float>(camera.x), static_cast<float>(camera.y),
					static_cast<float>(camera.z)},
			frame);
	auto pack_family = [](const std::vector<float> &run) {
		Dictionary family;
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
		family["positions"] = positions;
		family["colors"] = colors;
		return family;
	};
	Dictionary out;
	out["additive"] = pack_family(frame.additive);
	out["alpha"] = pack_family(frame.alpha);
	out["channels"] = frame.channels;
	return out;
}

Dictionary Simulation::get_entity_debug(int p_index) const {
	Dictionary out;
	if (!ai_ || !world_) return out;
	AiEntity *e = ai_->at(p_index);
	if (!e) return out;
	// A scripted remove (VaporizeSingle / removeSSN) despawns the registry slot
	// while the AiEntity stays in the AI pool, so the registry block emits TYPED
	// DEFAULTS rather than dropping keys - the card's shape is stable whether
	// the entity is whole or registry-despawned.
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	out["kind"] = ent ? opennova::world::spawn_origin_kind(ent->spawn_origin) : -1;
	out["index"] = ent ? static_cast<int>(opennova::world::spawn_origin_index(ent->spawn_origin)) : -1;
	out["bms_id"] = ent ? ent->bms_id : 0;
	out["item_id"] = ent ? ent->item_id : 0;
	// NOTE: retail BMS names are Windows-1252; non-ASCII bytes will read as
	// invalid UTF-8 here. Names are ASCII in practice; revisit if mojibake shows.
	out["name"] = ent ? String(ent->name.c_str()) : String();
	out["group_id"] = ent ? static_cast<int>(ent->group_id) : 0;
	out["team"] = ent ? static_cast<int>(ent->team) : -1;
	out["pool"] = ent ? ent->handle.pool() : -1;
	out["engine_flags"] = ent ? static_cast<int64_t>(ent->engine_flags) : 0;
	out["waypoint_id"] = ent ? static_cast<int>(ent->waypoint_id) : 0;
	out["wp_number"] = ent ? ent->wp_number : 0;
	out["health"] = ent ? ent->health : 0;
	out["alive"] = ent ? ent->alive : false;
	out["hidden"] = ent ? ent->hidden : false;
	out["held"] = ent ? ent->held : false;
	out["disabled"] = ent ? ent->disabled : false;
	out["vehicle_family"] = -1;
	if (ent != nullptr) {
		if (const opennova::world::VehicleTraits *traits =
					world_->vehicle_traits.get(ent->item_id)) {
			out["vehicle_family"] = static_cast<int>(traits->family);
		}
	}
	out["body_anim_slot"] = ent ? ent->body_anim_slot : -1;
	out["character_anim_slot"] = ent ? static_cast<int>(ent->anim_slot) : -1;
	out["minimap_net_id"] = ent ? static_cast<int>(ent->minimap_net_id) : 0;
	out["mounted"] = ent ? ent->mounted : false;
	out["mount_target_net_id"] = 0;
	out["mount_seat"] = ent ? static_cast<int>(ent->mount_seat) : -1;
	out["mount_type"] = ent ? static_cast<int>(ent->mount_type) : 0;
	out["mount_config_valid"] = ent ? ent->mounted_config_valid : false;
	out["mount_config"] =
			(ent && ent->mounted_config_valid) ? static_cast<int>(ent->mounted_config) : 0;
	out["mount_seat_bone"] = 0;
	out["mount_seat_pose_index"] = 0;
	out["mount_seat_source_name"] = String();
	out["mount_seat_local"] = Vector3();
	out["mount_seat_yaw_offset"] = 0;
	out["mount_target_config_valid"] = false;
	out["mount_target_config"] = 0;
	out["mount_target_seat_count"] = 0;
	out["mount_target_seats"] = Array();
	if (ent && ent->mounted) {
		const opennova::world::Entity *target = world_->registry.get(ent->mount_target);
		if (target) {
			out["mount_target_net_id"] = static_cast<int>(target->net_id);
			out["mount_target_config_valid"] = target->emplaced_config_valid;
			out["mount_target_config"] =
					target->emplaced_config_valid ? static_cast<int>(target->emplaced_config) : 0;
			out["mount_target_seat_count"] = static_cast<int>(target->seats.size());
			Array target_seats;
			for (int i = 0; i < static_cast<int>(target->seats.size()); ++i) {
				const opennova::world::Seat &seat = target->seats[i];
				Dictionary d;
				d["index"] = i;
				d["type"] = static_cast<int>(seat.type);
				d["retail_slot"] = static_cast<int>(seat.retail_slot);
				d["bone_index"] = static_cast<int>(seat.bone_index);
				d["pose_index"] = static_cast<int>(seat.pose_index);
				d["source_name"] = String(seat.source_name.c_str());
				d["local"] = Vector3(seat.seat_local.x, seat.seat_local.y, seat.seat_local.z);
				d["yaw_offset"] = static_cast<int>(seat.yaw_offset);
				d["occupied"] = seat.occupant.valid();
				target_seats.push_back(d);
			}
			out["mount_target_seats"] = target_seats;
			if (ent->mount_seat >= 0 && ent->mount_seat < static_cast<int>(target->seats.size())) {
				const opennova::world::Seat &seat = target->seats[ent->mount_seat];
				out["mount_type"] = static_cast<int>(seat.type);
				out["mount_seat_bone"] = static_cast<int>(seat.bone_index);
				out["mount_seat_pose_index"] = static_cast<int>(seat.pose_index);
				out["mount_seat_source_name"] = String(seat.source_name.c_str());
				out["mount_seat_local"] = Vector3(seat.seat_local.x, seat.seat_local.y, seat.seat_local.z);
				out["mount_seat_yaw_offset"] = static_cast<int>(seat.yaw_offset);
			}
		}
	}
	out["net_id"] = e->net_id;
	out["wire_handle"] = static_cast<int>(e->handle.packed);
	out["team"] = static_cast<int>(e->team);
	// The AI-side entity+286 mirror; diverges from the registry health under
	// some damage paths, so the card shows both.
	out["ai_health"] = static_cast<int>(e->health);
	out["position"] = get_entity_position(p_index);
	out["yaw_deg"] = get_entity_yaw_deg(p_index);
	const int state = e->brain.f[AiBrain::kCurState];
	out["state"] = state;
	out["state_name"] = ai_state_name(state);
	out["pending_state"] = e->brain.f[AiBrain::kPendState];
	out["alert"] = e->brain.f[AiBrain::kAlert];
	out["wp_channel"] = e->brain.f[AiBrain::kWpChannel];
	out["wp_node"] = e->brain.f[AiBrain::kWpNode];
	out["wp_distance"] = e->brain.f[AiBrain::kWpDistance];
	out["out_speed"] = e->brain.f[AiBrain::kOutSpeed];
	out["infantry"] = e->inf.active;
	out["adm_id"] = e->inf.active ? e->inf.adm_id : -1;
	out["adm_name"] = e->inf.active
			? String::utf8(infantry_anim_.adm_name(e->inf.adm_id).c_str())
			: String();
	out["infantry_move_mode"] = e->inf.move_mode;
	out["anim_state"] = e->inf.active ? e->inf.anim_state : -1;
	out["anim_key"] = e->inf.active ? infantry_anim_key(e->inf.anim_state) : String();
	// Infantry combat diagnostics (the P1 threat-loop bring-up surface): the
	// perception/attack ranges the scan reads (AiSlot +68/+60, world units), the
	// D-AI-5 weapon seed (AiProfile ammo index + clip, the live magazine word),
	// and the current combat target.
	out["sight_range_u"] =
			e->slot.f[opennova::world::AiSlot::kSightRange] / 65536.0;
	out["attack_range_u"] =
			e->slot.f[opennova::world::AiSlot::kAttackRange] / 65536.0;
	out["ammo_primary"] = e->profile.ammo_primary;
	out["clip_size"] = e->profile.clip_size;
	out["magazine"] = static_cast<int>(e->inf.magazine);
	out["combat_target_valid"] = e->inf.combat_target.valid();
	// The D-AI-6 muzzle seam readback (probe surface): the shell-fed posed muzzle.
	out["muzzle_valid"] = e->muzzle_valid;
	out["muzzle"] = godot_from_fixed3(e->muzzle_world);
	// Death presentation (P1c): the damage-time selection still pending consume,
	// the live corpse countdown, and the def traits behind them (world-wac-ai-re §19).
	out["death_anim_state"] = ent ? ent->death_anim_state : 0;
	out["corpse_timer"] = ent ? ent->corpse_timer : 0;
	out["deathtime_ticks"] = ent ? ent->deathtime_ticks : 0;
	out["leave_corpse"] = ent ? ent->leave_corpse : false;
	return out;
}

String Simulation::ai_state_name(int p_state) {
	return String(opennova::world::ai_state_name(p_state));
}

String Simulation::infantry_anim_key(int p_state) {
	if (p_state < 0 || p_state >= opennova::world::kInfantryAnimStateCount) return String();
	const char *name = opennova::world::kInfantryAnimNames[p_state];
	if (!name || !name[0]) return String();
	return String("anim_") + String(name);
}

int64_t Simulation::infantry_anim_flags(int p_state) {
	if (p_state < 0 || p_state >= opennova::world::kInfantryAnimStateCount) return 0;
	return static_cast<int64_t>(opennova::world::kInfantryAnimFlags[p_state]);
}

bool Simulation::remote_body_state_defers(int64_t p_current_flags, int64_t p_next_flags) {
	return opennova::world::remote_body_state_defers(
			static_cast<uint32_t>(p_current_flags), static_cast<uint32_t>(p_next_flags));
}

int Simulation::get_entity_count() const {
	return ai_ ? ai_->count() : 0;
}

int Simulation::get_entity_kind(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return opennova::world::spawn_origin_kind(ent->spawn_origin); // [orig promote: (kind<<24)|index]
}

int Simulation::get_entity_index(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return static_cast<int>(opennova::world::spawn_origin_index(ent->spawn_origin));
}

Vector3 Simulation::get_entity_position(int p_index) const {
	if (!ai_) return Vector3();
	AiEntity *e = ai_->at(p_index);
	if (!e) return Vector3();
	// mission (x, y, z) 16.16 -> Godot (x, z, -y) world units. [orig render remap: (x, z, -y).]
	return Vector3(static_cast<float>(e->pos[0] / kFixed16),
	               static_cast<float>(e->pos[2] / kFixed16),
	               static_cast<float>(-e->pos[1] / kFixed16));
}

// The AI brain stores heading in the ENGINE frame (90 - mission yaw): the spawn seed and the
// waypoint mover (atan2(dY,dX) bearing) both use it, so a unit faces consistently whether parked or
// moving. The shell basis (MissionObjectPlacer.bms_to_godot_basis) takes the MISSION yaw and internally
// applies the faithful (90 - yaw) engine heading, so the present converts engine -> mission here:
// mission_yaw = 90 - engine_heading. (Stationary units still report their authored yaw.)
float Simulation::get_entity_yaw(int p_index) const {
	if (!ai_) return 0.0f;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0.0f;
	const double mission_yaw_deg = opennova::world::mission_yaw_deg_from_bam_heading(e->heading);
	return static_cast<float>(mission_yaw_deg * 0.017453292519943295);
}

float Simulation::get_entity_yaw_deg(int p_index) const {
	if (!ai_) return 0.0f;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0.0f;
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(e->heading));
}

int Simulation::get_entity_state(int p_index) const {
	if (!ai_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kCurState];
}

int Simulation::get_entity_net_id(int p_index) const {
	if (!ai_) return 0;
	AiEntity *e = ai_->at(p_index);
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
	if (!ai_ || !world_) return out;
	for (int i = 0; i < ai_->count(); ++i) {
		AiEntity *e = ai_->at(i);
		if (!e) continue;
		const opennova::world::Entity *ent = world_->registry.get(e->handle);
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
	if (world_ == nullptr || p_ssn <= 0 ||
			p_ssn > static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
		return out;
	}
	const opennova::world::Entity *entity = world_->registry.get(
			world_->registry.find_by_net_id(static_cast<std::uint16_t>(p_ssn)));
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
	if (!world_ || !runtime_) {
		if (present_effect_pose_cache_valid_) invalidate_present_effect_pose_cache();
		return;
	}

	const opennova::netsim::ClientState &client = runtime_->state();
	const uint32_t logic_tick = world_->logic_tick;
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
		const opennova::netsim::ClientEntityState &p_entity_state) const {
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
	const opennova::world::Entity *entity = joiner_ ? nullptr : world_->registry.get(
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
	for (const opennova::netsim::ClientEntityState &entity_state :
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
	for (const opennova::netsim::ClientEntityState &entity_state :
			runtime_->state().entities) {
		const opennova::world::Entity *entity = world_->registry.get(
				opennova::world::EntityHandle{entity_state.handle});
		if (!entity || static_cast<int>(entity->net_id) != p_ssn) continue;
		if (cache_present_effect_pose(entity_state)) {
			return cached_present_effect_state_for_handle(entity_state.handle);
		}
		break;
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
	for (const opennova::netsim::ClientEntityState &entity_state :
			runtime_->state().entities) {
		const opennova::world::Entity *entity = world_->registry.get(
				opennova::world::EntityHandle{entity_state.handle});
		if (!entity || static_cast<int>(entity->bms_id) != p_bms_id) continue;
		if (cache_present_effect_pose(entity_state)) {
			return cached_present_effect_state_for_handle(entity_state.handle);
		}
		break;
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
	for (const opennova::netsim::ClientEntityState &entity_state :
			runtime_->state().entities) {
		const opennova::world::Entity *entity = world_->registry.get(
				opennova::world::EntityHandle{entity_state.handle});
		if (!entity) continue;
		const int kind = opennova::world::spawn_origin_kind(entity->spawn_origin);
		const int index = static_cast<int>(opennova::world::spawn_origin_index(entity->spawn_origin));
		if (present_effect_origin_key(kind, index) != requested_origin) continue;
		if (cache_present_effect_pose(entity_state)) {
			return cached_present_effect_state_for_handle(entity_state.handle);
		}
		break;
	}
	present_effect_missing_origins_.insert(requested_origin);
	return PackedVector3Array();
}

int Simulation::get_entity_bms_id(int p_index) const {
	if (!ai_ || !world_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->bms_id : 0;
}

// [D-NET-112] entity+0x78 ownerConnectionId (the connection/dcb that owns this entity). A networked
// PLAYER is identified by this + its handle, NOT by an SSN (players carry net_id 0). 0 = unowned (AI /
// mission entity / the host's dedicated reservation).
int Simulation::get_entity_owner_connection_id(int p_index) const {
	if (!ai_ || !world_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? static_cast<int>(ent->owner_connection_id) : 0;
}

// The entity's wire handle (pool<<12|slot) — the per-entity identity carried on the 0x0A/0x0C wire and
// the decoded present's PF_WIRE_HANDLE. Unique per entity (unlike a player's net_id, which is now 0).
int Simulation::get_entity_wire_handle(int p_index) const {
	if (!ai_) return 0;
	AiEntity *e = ai_->at(p_index);
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
	if (!ai_ || channel < 1 || channel > 2) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kPartAnimPhase0 + (channel - 1)];
}

bool Simulation::get_entity_part_anim_active(int p_index, int channel) const {
	if (!ai_ || channel < 1 || channel > 2) return false;
	AiEntity *e = ai_->at(p_index);
	if (!e) return false;
	if (channel == 1 && world_ != nullptr) {
		const opennova::world::Entity *entity =
				world_->registry.get(e->handle);
		if (entity != nullptr && (entity->item_attrib & 0x1000u) != 0)
			return false;
	}
	return true;
}

int Simulation::get_entity_body_anim_slot(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->body_anim_slot : -1;
}

bool Simulation::get_entity_hidden(int p_index) const {
	if (!ai_ || !world_) return false;
	AiEntity *e = ai_->at(p_index);
	if (!e) return false;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->hidden : false;
}

PackedFloat32Array Simulation::get_present_snapshot() const {
	const uint64_t start_us =
			runtime_profiling_enabled_ ? perf_now_us() : 0;
	// P7 (ADR 0011 Decision 1): every authoritative live mission is an in-process listen server in
	// standalone MainGame/GameWorld. The present pass reads the state the LOCAL CLIENT decoded off the
	// wire (ClientState), not the authoritative sim directly. Standalone SP and LAN hosts therefore
	// render exactly what a networked peer would; a joiner renders remote entities wire-direct.
	// ADR 0025 retired ONED's live editor preview, while bare sims remain available to tests/tooling.
	// The old no-net AI-pool present is retired. Empty when no runtime is active (a bare sim) — scalar
	// getters (get_entity_*) read the AI pool for tooling.
	PackedFloat32Array out;
	if (runtime_) {
		out = present_snapshot_from_client_replicas();
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
		last_present_snapshot_us_ = perf_now_us() - start_us;
	return out;
}
PackedFloat32Array Simulation::present_snapshot_from_client_replicas() const {
	PackedFloat32Array out;
	if (!world_ || !runtime_) return out;
	// P7: every path (SP / LAN host / joiner) reads its own npruntime ClientRuntime view's ClientState.
	const opennova::netsim::ClientState &cs = runtime_->state();
	const opennova::world::Entity *local_player =
			world_->registry.get(world_->cached.local_player);
	// Entity_RenderVehicleModel's local UseGun predicate is a render verdict,
	// not Entity.hidden: parent equality + first-person camera + raw UseGun seat.
	// The per-row tail below adds the live EquippedSlot/Def tests.
	// [orig: @0x4407f6..0x44084c; sole submit @0x440918]
	const bool local_first_person_usegun =
			!player_view_.third_person && local_player != nullptr &&
			local_player->mounted &&
			local_player->mount_type == opennova::world::SeatType::Gunner;
	const int count = static_cast<int>(cs.entities.size());
	int dismemberment_piece_count = 0;
	if (!joiner_) {
		world_->registry.for_each([&](const opennova::world::Entity &entity) {
			if (entity.dismemberment_piece) ++dismemberment_piece_count;
		});
	}
	const opennova::np::ClientReplicaPresentContext replica_present_context{
			&item_seat_specs_, &world_->weapons, joiner_};
	out.resize(static_cast<int64_t>(count + dismemberment_piece_count) * PF_STRIDE);
	float *w = out.ptrw();
	for (int i = 0; i < count; ++i) {
		float *r = w + static_cast<int64_t>(i) * PF_STRIDE;
		const opennova::netsim::ClientEntityState &es = cs.entities[i];
		opennova::np::initialize_client_replica_present_row(r);

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
		opennova::np::project_client_replica_present_row(
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
		const opennova::world::Entity *ent = (!joiner_) ? world_->registry.get(h) : nullptr;
		// Retail's terrain collector sends pool-1 model rows through
		// render_sector_entity; pool-2 statics and pool-3 marker models join the
		// same sector list through their dedicated collectors. Pool-0 skeletal
		// organics take the general/body list and do not execute this writer.
		// Keep validity independent of whether this client resolves the row to a
		// placed node: a wire-fallback model still executes the same callback,
		// while a failed model build has no CTRL surface on which to apply it.
		const bool sector_model_row = h.pool() >= 1 && h.pool() <= 3;
		if (joiner_ && h.pool() >= 1 && h.pool() <= 3) {
			const opennova::world::Entity *local = world_->registry.get(h);
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
			write_present_section_mask(r, ent->section_mask);
			r[PF_RIGHT_HAND_COLLAPSED] =
					mount_collapses_right_hand_row(*ent) ? 1.0f : 0.0f;
			// The cveh render callback publishes directly from the live entity
			// motor fields. Do this only for the authoritative registry row:
			// the compact view has no steer/currentSpeed source to reconstruct.
			// [orig: Entity_CacheVehicleHUDStats @ 0x4929B0;
			//  stores @0x4929D7 / @0x4929F1]
			write_present_vehicle_motion_controls(r, *world_, *ent);
			// Only a carrier in the witnessed live UseGun attachment relation
			// publishes its inline MountSlot's HEAT_GLOW, including owned cold
			// zero. A joiner has no heat-window/ownership state in its compact
			// row and must not synthesize one.
			// [orig: attachment call @ 0x546518;
			//  HUD_CacheWeaponSlotInfo stores @ 0x440969 / @ 0x440991]
			write_present_world_model_heat_glow(r, *world_, *ent);
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
			const opennova::world::Entity *mount_row = world_->registry.get(h);
			const opennova::world::WeaponTableEntry *mount_def =
					mount_row != nullptr
					? world_->weapons.by_index(mount_row->primary_weapon_slot_adm)
					: nullptr;
			// Primary retail leg: FP model exists and this exact embedded
			// MountSlot is the live EquippedSlot. flags2 Invisible is the
			// witnessed alternate forced-cull leg and does not require the
			// EquippedSlot comparison.
			// [orig: Def+0x16c @0x440824; EquippedSlot @0x440833;
			//  Def+0x0c & 0x800 @0x44083f]
			const bool equipped_parent_slot = mount_row != nullptr &&
					local_weapon_.usegun_slot_active && local_weapon_.usegun_mount == h &&
					local_weapon_.usegun_weapon_adm ==
							mount_row->primary_weapon_slot_adm;
			if (mount_def != nullptr &&
					((mount_def->has_first_person_model_reference &&
					  local_weapon_.first_person_model_adm ==
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
						*world_, ai_.get(), *ent, emplaced))
				write_present_emplaced_controls(r, emplaced);
		}
		const bool authoritative_attachment_pose =
				ent != nullptr && ent->emplacement_parent.valid() &&
				ent->emplacement_pose_metadata_resolved &&
				ent->emplacement_parent.packed == es.parent_handle;
		opennova::world::MountedPose client_attachment_pose;
		const uint32_t attachment_time_ms = panm_time_override_ms_ >= 0
				? static_cast<uint32_t>(panm_time_override_ms_)
				: world_->logic_tick * 16u;
		const bool reconstructed_client_attachment_pose = joiner_ &&
				resolve_client_eweap_attachment_pose(
						es, cs, item_seat_specs_, mounted_pose_native_graphics_,
						sim_models_, attachment_time_ms, client_attachment_pose);
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
		if (world_->ai && !joiner_) {
			const AiEntity *ae = world_->ai->for_handle(h);
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
	// Dismemberment clones have no LAN spawn/compact record in this port:
	// entity_wire_bridge excludes them, and whether the retail clone reaches
	// the compact/spawn wire is an open D-AI-9 residual (retail memcpy-inherits
	// NetId/Ssn but severs the connection id). A listen server or standalone
	// game appends them as synthetic-origin rows, which the existing
	// runtime-model WirePresentPass owns. Their packed local handle is only a
	// presenter key.
	int piece_row = count;
	if (!joiner_) {
		world_->registry.for_each([&](const opennova::world::Entity &ent) {
			if (!ent.dismemberment_piece) return;
			float *r = w + static_cast<int64_t>(piece_row++) * PF_STRIDE;
			opennova::np::initialize_client_replica_present_row(r);
			r[PF_KIND] = static_cast<float>(
					opennova::world::kSpawnOriginKindNone);
			r[PF_INDEX] = static_cast<float>(
					opennova::world::kSpawnOriginIndexNone);
			r[PF_BMS_ID] = 0.0f;
			r[PF_NET_ID] = 0.0f;
			r[PF_TYPE_ID] = static_cast<float>(ent.item_id);
			r[PF_WIRE_HANDLE] = static_cast<float>(ent.handle.packed);
			r[PF_BODY_ANIM_SLOT] = static_cast<float>(ent.body_anim_slot);
			r[PF_HIDDEN] = ent.hidden ? 1.0f : 0.0f;
			r[PF_ALIVE] = ent.alive ? 1.0f : 0.0f;
			write_present_section_mask(r, ent.section_mask);

			const AiEntity *ae = world_->ai != nullptr
					? world_->ai->for_handle(ent.handle)
					: nullptr;
			if (ae == nullptr) {
				r[PF_POS_X] = ent.position.x;
				r[PF_POS_Y] = ent.position.z;
				r[PF_POS_Z] = -ent.position.y;
				r[PF_PITCH_DEG] = static_cast<float>(ent.pitch);
				r[PF_YAW_DEG] = static_cast<float>(ent.yaw);
				r[PF_ROLL_DEG] = static_cast<float>(ent.roll);
				return;
			}

			constexpr double kFixed16 = 65536.0;
			r[PF_POS_X] = static_cast<float>(ae->pos[0] / kFixed16);
			r[PF_POS_Y] = static_cast<float>(ae->pos[2] / kFixed16);
			r[PF_POS_Z] = static_cast<float>(-ae->pos[1] / kFixed16);
			r[PF_PITCH_DEG] = static_cast<float>(
					double(ae->pitch) * opennova::world::kDegreesPerBam);
			r[PF_YAW_DEG] = static_cast<float>(
					opennova::world::mission_yaw_deg_from_bam_heading(
							ae->heading));
			r[PF_ROLL_DEG] = static_cast<float>(
					double(ae->roll) * opennova::world::kDegreesPerBam);
			for (int slot = 0; slot < 2; ++slot) {
				const int32_t phase =
						ae->brain.f[AiBrain::kPartAnimPhase0 + slot];
				const bool publish = slot != 0 ||
						(ent.item_attrib & 0x1000u) == 0;
				uint32_t phase_bits = 0;
				std::memcpy(&phase_bits, &phase, sizeof(phase_bits));
				r[PF_PHASE1 + slot * 2] =
						static_cast<float>(phase_bits & 0xFFFFu);
				r[PF_ACTIVE1 + slot * 2] = publish
						? static_cast<float>((phase_bits >> 16) + 1u)
						: 0.0f;
			}
			if (ae->inf.active) {
				r[PF_ANIM_STATE] = static_cast<float>(ae->inf.anim_state);
				r[PF_ANIM_PHASE_TICKS] =
						static_cast<float>(ae->inf.clip_phase);
				if (ae->inf.body_blend_active()) {
					r[PF_ANIM_SOURCE_STATE] =
							static_cast<float>(ae->inf.anim_prev);
					r[PF_ANIM_SOURCE_PHASE_TICKS] =
							static_cast<float>(ae->inf.anim_prev_clip_phase);
					r[PF_ANIM_BLEND_WEIGHT] = ae->inf.anim_blend_weight;
				}
				const opennova::anim::AimOverlayInputs inputs =
						aim_overlay_inputs_for(*ae, ent);
				opennova::anim::AimOverlayAngles
						angles[opennova::anim::kOverlayClassCount];
				opennova::anim::compute_aim_overlay_angles(inputs, angles);
				write_present_overlay(r, angles);
			}
		});
	}
	// Consume-once: each transition pulse dispatches exactly one presented
	// frame (the rows above copied any live pulse into PF_ANIM_STATE_PULSE).
	runtime_->state().clear_anim_pulses();
	return out;
}
