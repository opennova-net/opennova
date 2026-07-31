// NovaSimulation — presentation reads: entity/pose getters, the present-effect
// pose cache, the packed present snapshots (AI pool + client view), HUD views,
// and the drains (effects, fire, destruction, round impacts, tracers).
#include "simulation/nova_simulation_internal.h"

#include <cstring>

#include <world/entity.h> // kEntityFlag* (the wire state_flags byte IS entity+36 low)

using namespace novasim;

Array NovaSimulation::get_throwable_visuals() const {
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

Dictionary NovaSimulation::get_waypoint_hud_view() const {
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

Array NovaSimulation::get_objectives_view() const {
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
Array NovaSimulation::drain_round_impacts() {
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
		out.push_back(d);
	}
	world_->round_sim.impacts.clear();
	return out;
}

Array NovaSimulation::drain_effects() {
	Array out;
	if (!loaded_) return out;
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
Array NovaSimulation::drain_fire_presentation_events() {
	Array out;
	if (!loaded_) return out;
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
		d["adm_arm"] = (fe.wire_round_flags & 0x01u) == 0 &&
				(fe.wire_round_flags & 0x02u) != 0;
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
		d["sound_set"] = ammo ? String(ammo->ai_launch_set.c_str()) : String();
		d["effect"] = ammo ? String(ammo->ai_launch_effect.c_str()) : String();
		d["mf_light"] = ammo ? ammo->mf_light : 0;
		// The adm arm's replacement legs. Retail executes the ADDRESSED def's action
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
		d["action_sound_set"] =
				fire_row ? String(fire_row->soundset) : String();
		d["action_effect"] = fire_row ? String(fire_row->particle) : String();
		// Resolved against the THIRD-PERSON model (gfx3): ActionDef+57 is the gfx3
		// userpoint index and +56 the gfx1 one — the opposite way round from three
		// currently-tracked doc lines. [orig: loader @0x54506c/@0x545092, resolver
		//  @0x54039e/@0x54040f]
		d["action_userpoint"] =
				fire_row ? String(fire_row->particle_userpoint) : String();
		out.push_back(d);
	}
	world_->round_sim.fired.clear();
	return out;
}

// The destruction presentation drain (world/destruction.h; §24): one call per
// present, converting the sim's events into godot-space dictionaries. Mission
// (x, y, z-up) -> Godot (x, z, -y), the drain_fire_presentation_events rule.
Dictionary NovaSimulation::drain_destruction_events() {
	Dictionary out;
	if (!loaded_) return out;
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
	Array bursts;
	for (const opennova::world::SectionDebrisEvent &b : ev.debris_bursts) {
		Dictionary d;
		d["net_id"] = static_cast<int>(b.net_id);
		d["bms_id"] = b.bms_id;
		d["spawn_origin"] = static_cast<int64_t>(b.spawn_origin);
		d["item_id"] = b.item_id;
		d["pos"] = to_godot(b.pos);
		d["blast_center"] = to_godot(b.blast_center);
		d["has_blast_center"] = b.blast_center.x != 0.0f || b.blast_center.y != 0.0f ||
				b.blast_center.z != 0.0f;
		bursts.push_back(d);
	}
	Array glass;
	for (const opennova::world::GlassBreakEvent &g : ev.glass_breaks) {
		Dictionary d;
		d["net_id"] = static_cast<int>(g.net_id);
		d["bms_id"] = g.bms_id;
		d["spawn_origin"] = static_cast<int64_t>(g.spawn_origin);
		d["item_id"] = g.item_id;
		d["blast_pos"] = to_godot(g.blast_pos);
		d["radius"] = g.radius;
		glass.push_back(d);
	}
	out["effects"] = effects;
	out["sounds"] = sounds;
	out["husk_swaps"] = husks;
	out["debris_bursts"] = bursts;
	out["glass_breaks"] = glass;
	out["explosions_processed"] = ev.explosions_processed;
	out["items_destroyed"] = ev.items_destroyed;
	ev.clear();
	return out;
}

// The live death-piece pool snapshot — the present pass renders each piece as
// its single husk-model section [orig: the piece render mask piece[31]; §24].
Array NovaSimulation::get_death_pieces() const {
	Array out;
	if (!loaded_) return out;
	for (size_t slot = 0; slot < world_->death_pieces.pieces.size(); ++slot) {
		const opennova::world::DeathPiece &p = world_->death_pieces.pieces[slot];
		if (!p.active) continue;
		Dictionary d;
		d["slot"] = static_cast<int>(slot);
		d["generation"] = static_cast<int64_t>(p.generation);
		d["item_id"] = p.item_id;
		d["section"] = static_cast<int>(p.section);
		d["type_index"] = static_cast<int>(p.type_index);
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
Dictionary NovaSimulation::get_destruction_debug(int p_bms_id) const {
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
	}
	out["pos"] = Vector3(found->position.x, found->position.z, -found->position.y);
	return out;
}

// The live tracer trail channels for the ribbon layer — see the header note.
// Mission -> godot axis map (x, z, -y), matching the other presentation drains.
PackedFloat32Array NovaSimulation::get_tracer_trails() const {
	PackedFloat32Array out;
	if (!loaded_) return out;
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
Dictionary NovaSimulation::get_entity_debug(int p_index) const {
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
	out["adm_name"] = e->inf.active ? infantry_anim_.adm_name(e->inf.adm_id) : String();
	out["infantry_move_mode"] = e->inf.move_mode;
	out["anim_state"] = e->inf.active ? e->inf.anim_state : -1;
	out["anim_key"] = e->inf.active ? infantry_anim_key(e->inf.anim_state) : String();
	// Infantry combat diagnostics (the P1 threat-loop bring-up surface): the
	// perception/attack ranges the scan reads (AiSlot +68/+60, world units), the
	// D-AI-5 weapon seed (AiProfile ammo index + clip, the live magazine word),
	// and the current combat target.
	out["sight_range_u"] = e->slot.f[17] / 65536.0;
	out["attack_range_u"] = e->slot.f[15] / 65536.0;
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

String NovaSimulation::ai_state_name(int p_state) {
	return String(opennova::world::ai_state_name(p_state));
}

String NovaSimulation::infantry_anim_key(int p_state) {
	if (p_state < 0 || p_state >= opennova::world::kInfantryAnimStateCount) return String();
	const char *name = opennova::world::kInfantryAnimNames[p_state];
	if (!name || !name[0]) return String();
	return String("anim_") + String(name);
}

int64_t NovaSimulation::infantry_anim_flags(int p_state) {
	if (p_state < 0 || p_state >= opennova::world::kInfantryAnimStateCount) return 0;
	return static_cast<int64_t>(opennova::world::kInfantryAnimFlags[p_state]);
}

int NovaSimulation::get_entity_count() const {
	return ai_ ? ai_->count() : 0;
}

int NovaSimulation::get_entity_kind(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return opennova::world::spawn_origin_kind(ent->spawn_origin); // [orig promote: (kind<<24)|index]
}

int NovaSimulation::get_entity_index(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return static_cast<int>(opennova::world::spawn_origin_index(ent->spawn_origin));
}

Vector3 NovaSimulation::get_entity_position(int p_index) const {
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
float NovaSimulation::get_entity_yaw(int p_index) const {
	if (!ai_) return 0.0f;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0.0f;
	const double mission_yaw_deg = opennova::world::mission_yaw_deg_from_bam_heading(e->heading);
	return static_cast<float>(mission_yaw_deg * 0.017453292519943295);
}

float NovaSimulation::get_entity_yaw_deg(int p_index) const {
	if (!ai_) return 0.0f;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0.0f;
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(e->heading));
}

int NovaSimulation::get_entity_state(int p_index) const {
	if (!ai_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kCurState];
}

int NovaSimulation::get_entity_net_id(int p_index) const {
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
PackedVector3Array NovaSimulation::get_foliage_mask_anchor_positions() const {
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

PackedVector3Array NovaSimulation::get_entity_effect_state_for_ssn(int p_ssn) const {
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

void NovaSimulation::invalidate_present_effect_pose_cache() const {
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

void NovaSimulation::ensure_present_effect_pose_cache() const {
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

bool NovaSimulation::cache_present_effect_pose(
		const opennova::netsim::ClientEntityState &p_entity_state) const {
	// Match present_snapshot_from_client_view's joiner self-filter: the host's
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

PackedVector3Array NovaSimulation::cached_present_effect_state_for_handle(
		uint16_t p_handle) const {
	PackedVector3Array out;
	const auto found = present_effect_poses_by_handle_.find(p_handle);
	if (found == present_effect_poses_by_handle_.end()) return out;
	out.resize(EFFECT_STATE_COUNT);
	out.set(EFFECT_STATE_POSITION, found->second.position);
	out.set(EFFECT_STATE_ROTATION_DEG, found->second.rotation_deg);
	return out;
}

PackedVector3Array NovaSimulation::present_effect_state_for_handle(uint16_t p_handle) const {
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

PackedVector3Array NovaSimulation::get_present_effect_state_for_ssn(int p_ssn) const {
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

PackedVector3Array NovaSimulation::get_present_effect_state_for_wire_handle(
		int p_wire_handle) const {
	if (p_wire_handle < 0 ||
			p_wire_handle > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
		return PackedVector3Array();
	}
	return present_effect_state_for_handle(static_cast<uint16_t>(p_wire_handle));
}

PackedVector3Array NovaSimulation::get_present_effect_state_for_bms_id(int p_bms_id) const {
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

PackedVector3Array NovaSimulation::get_present_effect_state_for_origin(
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

int NovaSimulation::get_entity_bms_id(int p_index) const {
	if (!ai_ || !world_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->bms_id : 0;
}

// [D-NET-112] entity+0x78 ownerConnectionId (the connection/dcb that owns this entity). A networked
// PLAYER is identified by this + its handle, NOT by an SSN (players carry net_id 0). 0 = unowned (AI /
// mission entity / the host's dedicated reservation).
int NovaSimulation::get_entity_owner_connection_id(int p_index) const {
	if (!ai_ || !world_) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? static_cast<int>(ent->owner_connection_id) : 0;
}

// The entity's wire handle (pool<<12|slot) — the per-entity identity carried on the 0x0A/0x0C wire and
// the decoded present's PF_WIRE_HANDLE. Unique per entity (unlike a player's net_id, which is now 0).
int NovaSimulation::get_entity_wire_handle(int p_index) const {
	if (!ai_) return 0;
	AiEntity *e = ai_->at(p_index);
	return e ? static_cast<int>(e->handle.packed) : 0;
}

int32_t NovaSimulation::decode_present_part_anim_phase(
		const PackedFloat32Array &p_snapshot, int p_base, int p_channel) {
	if (p_channel < 1 || p_channel > 2 || p_base < 0) return 0;
	const int phase_field = PF_PHASE1 + (p_channel - 1) * 2;
	const int active_field = PF_ACTIVE1 + (p_channel - 1) * 2;
	if (p_base + active_field >= p_snapshot.size()) return 0;
	const float *p = p_snapshot.ptr();
	const int32_t high_code =
			static_cast<int32_t>(p[p_base + active_field]);
	if (high_code <= 0 || high_code > 0x10000) return 0;
	const uint32_t low = static_cast<uint32_t>(
			static_cast<int32_t>(p[p_base + phase_field])) & 0xFFFFu;
	const uint32_t bits =
			(static_cast<uint32_t>(high_code - 1) << 16) | low;
	int32_t value;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

int NovaSimulation::get_entity_part_anim_phase(int p_index, int channel) const {
	if (!ai_ || channel < 1 || channel > 2) return 0;
	AiEntity *e = ai_->at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kPartAnimPhase0 + (channel - 1)];
}

bool NovaSimulation::get_entity_part_anim_active(int p_index, int channel) const {
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

int NovaSimulation::get_entity_body_anim_slot(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->body_anim_slot : -1;
}

bool NovaSimulation::get_entity_hidden(int p_index) const {
	if (!ai_ || !world_) return false;
	AiEntity *e = ai_->at(p_index);
	if (!e) return false;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->hidden : false;
}

PackedFloat32Array NovaSimulation::get_present_snapshot() const {
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
		out = present_snapshot_from_client_view();
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
PackedFloat32Array NovaSimulation::present_snapshot_from_client_view() const {
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
	out.resize(static_cast<int64_t>(count) * PF_STRIDE);
	float *w = out.ptrw();
	for (int i = 0; i < count; ++i) {
		float *r = w + static_cast<int64_t>(i) * PF_STRIDE;
		const opennova::netsim::ClientEntityState &es = cs.entities[i];
		r[PF_KIND] = -1.0f; r[PF_INDEX] = -1.0f; r[PF_BMS_ID] = 0.0f; r[PF_NET_ID] = 0.0f;
		r[PF_POS_X] = 0.0f; r[PF_POS_Y] = 0.0f; r[PF_POS_Z] = 0.0f;
		r[PF_PITCH_DEG] = 0.0f; r[PF_YAW_DEG] = 0.0f; r[PF_ROLL_DEG] = 0.0f;
		r[PF_PHASE1] = 0.0f; r[PF_ACTIVE1] = 0.0f; r[PF_PHASE2] = 0.0f; r[PF_ACTIVE2] = 0.0f;
		r[PF_BODY_ANIM_SLOT] = -1.0f; r[PF_ANIM_STATE] = -1.0f; r[PF_ANIM_PHASE_TICKS] = -1.0f;
		r[PF_ANIM_SOURCE_STATE] = -1.0f;
		r[PF_ANIM_SOURCE_PHASE_TICKS] = -1.0f;
		r[PF_ANIM_BLEND_WEIGHT] = 1.0f;
		r[PF_ANIM_REMOTE_REQUEST] = 0.0f;
		r[PF_ANIM_STATE_PULSE] = -1.0f; r[PF_ANIM_PULSE_TICKS] = -1.0f;
		r[PF_WPN_ANIM_STATE] = -1.0f; r[PF_WPN_PHASE_TICKS] = -1.0f;
		r[PF_HIDDEN] = 0.0f; r[PF_LOCAL_VIEW_SUPPRESSED] = 0.0f;
		r[PF_ALIVE] = 1.0f; r[PF_RESPAWN_REVISION] = 0.0f;
		r[PF_TYPE_ID] = 0.0f; r[PF_WIRE_HANDLE] = 0.0f;
		for (int field = PF_AIM_OVERLAY_VALID; field < PF_STRIDE; ++field)
			r[field] = 0.0f;

		// Self-filter (joiner): the host SNAPs our own entity (wire handle H) and streams
		// it back in 0x0A; we draw our local player L via LocalPlayerPresenter, so drop the wire
		// echo here. The row stays at its zero/unresolved defaults (PF_TYPE_ID 0), which the
		// wire render pass skips. [net-re §5.38b two-handle L-vs-H reconciliation]
		if (joiner_ && runtime_->has_self_handle() &&
				es.handle == runtime_->self_handle()) {
			continue;
		}
		// Wire identity for the render pass. The joiner has no authoritative registry for
		// the host's entities, so it renders from the wire type id + handle, not a node.
		r[PF_TYPE_ID] = static_cast<float>(es.type_id);
		r[PF_WIRE_HANDLE] = static_cast<float>(es.handle);

		// On the HOST listen server, kind/index/bms_id/net_id resolve from the registry
		// entity behind the decoded handle (host == authoritative client, so the placed-node
		// mapping still resolves through MissionEntityRegistry exactly as the AI-pool path
		// does). A joiner resolves the DEFER IDENTITY the same way for mission pools 1-3:
		// its locally promoted world shares the host's pool/slot handle space for .bms
		// entities — promote order mirrors Mission_LoadBMSFile @0x40f4e0 on both sides, the
		// same identity assumption sync_joiner_authoritative_mount already relies on — so a
		// streamed vehicle/building/marker row maps onto its locally PLACED node and renders
		// batched + occludable through MissionPresentPass instead of wire-direct. The fill is
		// type-guarded (a drifted slot must never adopt a wrong node), skips synthetic
		// children (spawn_origin sentinel), and fills ONLY the identity: hidden/alive/anim
		// state keep coming from the wire bytes below, because the joiner's local sim is not
		// authoritative for any of them. Pool-0 organics (players + streamed AI) stay
		// wire-rendered — their body-anim path is remote-request-shaped, which the mission
		// pass does not model.
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
			if (local_first_person_usegun &&
					local_player->mount_target == h) {
				const opennova::world::WeaponTableEntry *mount_def =
						world_->weapons.by_index(ent->primary_weapon_slot_adm);
				// Primary retail leg: FP model exists and this exact embedded
				// MountSlot is the live EquippedSlot. flags2 Invisible is the
				// witnessed alternate forced-cull leg and does not require the
				// EquippedSlot comparison.
				// [orig: Def+0x16c @0x440824; EquippedSlot @0x440833;
				//  Def+0x0c & 0x800 @0x44083f]
				const bool equipped_parent_slot =
						local_usegun_slot_active_ && local_usegun_mount_ == h &&
						local_usegun_weapon_adm_ == ent->primary_weapon_slot_adm;
				if (mount_def != nullptr &&
						((mount_def->has_first_person_model_reference &&
						  local_first_person_model_adm_ ==
								  ent->primary_weapon_slot_adm &&
						  equipped_parent_slot) ||
						 (mount_def->flags2 &
						  opennova::world::weapon_flag2::kInvisible) != 0))
					r[PF_LOCAL_VIEW_SUPPRESSED] = 1.0f;
			}
		}
		// Two retail callbacks write this three-register family. The sector
		// renderer publishes TEX_TEAM for every placed pool-1/2/3 model that
		// reaches its model callback. The generic-world callback publishes the
		// same TEX_TEAM plus TEAMSWING for a nonzero packed zone byte, and writes
		// LFP only when the client-side shared timer-list entry exists.
		// Values come from the decoded client row for BOTH authority and joiner:
		// this preserves the exact 0x0D/0x10/0x20 bytes and later S2C 0x50 team
		// mutations instead of reaching around the client view.
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
		// A joiner cannot resolve host wire handles through its local registry.
		// Organic lifecycle therefore comes straight from the raw compact byte:
		// bit 0 hides, bit 1 is dead/undeployed. The revision survives multiple
		// decoded frames between render passes and gives presentation a stable
		// signal to reset one-shot/body-channel state on respawn.
		// Both roles fold compact lifecycle records. Preserve the epoch on the
		// host too: WirePresentPass renders admitted remote players that have no
		// placed mission node.
		r[PF_RESPAWN_REVISION] = static_cast<float>(es.respawn_revision);
		if (joiner_) {
			if (es.state_flags_known) {
				r[PF_HIDDEN] = (es.state_flags & 0x01u) != 0u ? 1.0f : 0.0f;
				r[PF_ALIVE] = (es.state_flags & opennova::world::kEntityFlagDead) == 0u ? 1.0f : 0.0f;
			}
		}
		EmplacedWeaponControls emplaced;
		if (ent != nullptr) {
			if (emplaced_weapon_controls_for(
						*world_, ai_.get(), *ent, emplaced))
				write_present_emplaced_controls(r, emplaced);
		} else if (joiner_ &&
				emplaced_weapon_controls_for_client(
						es, cs, item_seat_specs_, emplaced)) {
			write_present_emplaced_controls(r, emplaced);
		}
		const bool authoritative_attachment_pose =
				ent != nullptr && ent->emplacement_parent.valid() &&
				ent->emplacement_parent.packed == es.parent_handle;
		opennova::world::MountedPose client_attachment_pose;
		const uint32_t attachment_time_ms = panm_time_override_ms_ >= 0
				? static_cast<uint32_t>(panm_time_override_ms_)
				: world_->logic_tick * 16u;
		const bool reconstructed_client_attachment_pose = joiner_ &&
				resolve_client_eweap_attachment_pose(
						es, cs, item_seat_specs_, mounted_pose_data_by_type_,
						attachment_time_ms, client_attachment_pose);
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
		} else {
			// Decoded wire position is mission (x,y,z) 16.16 -> Godot (x, z, -y)
			// world units, the SAME remap the AI-pool path uses. Position is
			// post-compression (lossy), exactly what retail renders for decoded peers.
			r[PF_POS_X] = static_cast<float>(es.x / kFixed16);
			r[PF_POS_Y] = static_cast<float>(es.z / kFixed16);
			r[PF_POS_Z] = static_cast<float>(-es.y / kFixed16);
			// The decoded body keeps a full client-side heading: each wire sample
			// re-seeds its high byte, then sub-byte body effects such as recoil apply.
			const int32_t heading_bam = es.heading_bam;
			r[PF_YAW_DEG] = static_cast<float>(
					opennova::world::mission_yaw_deg_from_bam_heading(heading_bam));
		}
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
								ae->inf, (ent->engine_flags & 0x100u) != 0,
								mount_blocks_weapon_channel(*ent))) {
					r[PF_WPN_ANIM_STATE] =
							static_cast<float>(ae->inf.wpn_state);
					r[PF_WPN_PHASE_TICKS] =
							static_cast<float>(ae->inf.wpn_clip_phase);
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
					if ((ent->engine_flags & 0x100u) != 0) {
						write_present_held_weapon(
								r, ent->equipped_adm_index,
								(ent->flags & 2u) != 0, inputs,
								ae->inf.wpn_state);
					}
				}
			}
		}
		if (joiner_) {
			opennova::anim::AimOverlayInputs inputs;
			bool collapse_right_hand = false;
			if (aim_overlay_inputs_for_client(
						es, cs, item_seat_specs_, inputs,
						&collapse_right_hand)) {
				r[PF_ANIM_STATE] =
						static_cast<float>(es.anim_state_id);
				r[PF_ANIM_REMOTE_REQUEST] = 1.0f;
				// A transition state that arrived and was overwritten within
				// this fold window (a tapped prone roll rides the wire for 1-2
				// ticks). Presentation dispatches it BEFORE the current state,
				// replaying retail's per-record apply order [orig: @0x4c1153].
				if (es.anim_state_pulse >= 0) {
					r[PF_ANIM_STATE_PULSE] =
							static_cast<float>(es.anim_state_pulse);
					if (es.cls == opennova::EntityClass::Player) {
						r[PF_ANIM_PULSE_TICKS] =
								static_cast<float>(es.anim_pulse_ratio);
					}
				}
				// The player compact's byte 15 is the authority's elapsed
				// half-frame ticks in the current body loop. Retail applies it
				// to remote players as the anim-channel phase seed. Infantry
				// compacts carry only the state byte, so their -1 sentinel tells
				// presentation to advance the selected clip locally.
				// [orig: player write @0x4c0cf2; remote apply @0x4c11a6;
				//  AnimMap_UpdateEntity consumes entity+0x377 @0x40b74b]
				if (es.cls == opennova::EntityClass::Player) {
					r[PF_ANIM_PHASE_TICKS] =
							static_cast<float>(es.anim_channel_ratio);
				}
				r[PF_RIGHT_HAND_COLLAPSED] =
						collapse_right_hand ? 1.0f : 0.0f;
				// The peer's upper-body weapon pose, re-derived here exactly as
				// every retail observer re-derives it: the hold kind from the ADM
				// table by the peer's own equipped index (wire off-16), and the
				// scoped/binocular conditions from its own Flags byte (wire off-13,
				// which the remote read-mask 0xFD preserves). Nothing about this
				// crosses the wire — there is no scope message and no scoped anim
				// id — so a joiner that ignores these two bytes shows every peer
				// holding a rifle at rest whatever they are actually carrying.
				// [orig: kind read @0x4b5dba, scope test @0x4b5deb; the selection
				//  has no ownership gate — only the local Flags refresh does,
				//  @0x4b5d77]
				// The peer's hold state is derived once, OUTSIDE the channel-visibility
				// gate below: the upper-body pose is gated, but the held weapon's attach
				// FRAME is selected from the same state whenever the weapon is drawn, so
				// gating this would silently hand every knife and grenade the wrong frame.
				int wpn_hold_state = -1;
				if (es.cls == opennova::EntityClass::Player) {
					int hold_kind = 0;
					if (world_) {
						if (const opennova::world::WeaponTableEntry *held =
									world_->weapons.by_index(
											es.equipped_adm_index))
							hold_kind = held->special_hold;
					}
					wpn_hold_state = opennova::world::infantry_weapon_hold_state(
							hold_kind, es.anim_state_id,
							(es.state_flags & opennova::world::kEntityFlagScopeRaised) != 0,
							(es.state_flags & opennova::world::kEntityFlagBinoculars) != 0,
							/*reloading=*/false);
				}
				if (es.cls == opennova::EntityClass::Player && !collapse_right_hand &&
						(opennova::world::infantry_anim_flags(es.anim_state_id) &
						 0x40u) != 0) {
					r[PF_WPN_ANIM_STATE] = static_cast<float>(wpn_hold_state);
					// -1 = presentation free-runs the secondary clip. The playhead
					// is not replicated (the compact carries only the PRIMARY
					// ratio), and retail's client advances it locally; this is the
					// same wire-state/local-phase split the primary clip already
					// uses for rows whose phase arrives as -1.
					r[PF_WPN_PHASE_TICKS] = -1.0f;
				}
				opennova::anim::AimOverlayAngles
						angles[opennova::anim::kOverlayClassCount];
				opennova::anim::compute_aim_overlay_angles(inputs, angles);
				write_present_overlay(r, angles);
				// The peer's third-person gun, from the equipped ADM index its own
				// compact record carries. Infantry rows are not players and carry
				// no index. The dead bit is the record's state byte 0x02.
				if (es.cls == opennova::EntityClass::Player) {
					write_present_held_weapon(
							r, es.equipped_adm_index,
							(es.state_flags & opennova::world::kEntityFlagDead) != 0, inputs,
							wpn_hold_state);
				}
			}
		}
	}
	// Consume-once: each transition pulse dispatches exactly one presented
	// frame (the rows above copied any live pulse into PF_ANIM_STATE_PULSE).
	runtime_->state().clear_anim_pulses();
	return out;
}
