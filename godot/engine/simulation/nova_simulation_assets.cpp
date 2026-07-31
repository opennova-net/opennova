// NovaSimulation — shell-side asset resolution: infantry anim maps (.adm),
// item traits/weapons from the item database, collision instances + section
// matrices from the .3di collision IR, and the mission item seat specs.
#include "simulation/nova_simulation_internal.h"

#include <def/def.h> // DEF_ITEM_ATTRIB_* / DEF_ITEM_ATTRIB2_*

using namespace novasim;

void NovaSimulation::reset_infantry_adm_ids() {
	infantry_adm_resolved_ai_count_ = 0;
	if (!world_ || !world_->ai) return;
	AiSystem &ai = *world_->ai;
	for (int i = 0; i < ai.count(); ++i) {
		if (AiEntity *e = ai.at(i)) e->inf.adm_id = 0;
	}
}

int NovaSimulation::set_infantry_anim_map(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name) {
	// The default clip set (adm_id 0): every infantry entity grounds off this until its own
	// model's .adm is registered (register_infantry_adm + set_infantry_adm_id). Clearing here
	// resets the whole registry on each (re)load.
	infantry_anim_.clear();
	reset_infantry_adm_ids();
	const int default_adm_id = infantry_anim_.register_adm(p_resource_root, p_adm_name);
	const int default_clip_count = default_adm_id == 0 ? infantry_anim_.clip_count(0) : 0;
	// Every stored per-entity id indexes this registry; rebuilding it invalidates
	// all prior assignments. Only repopulate once slot 0 is the successfully loaded
	// default map; otherwise a model-specific ADM could usurp the default slot and
	// turn a failed load into false success.
	if (default_adm_id == 0) resolve_new_infantry_adm_ids();
	apply_root_motion_to_ai();
	return default_clip_count;
}

// Assign only newly attached AI entries. AiSystem::attach is append-only, including when
// an entity handle is reused, so the count is a generation-safe high-water mark. This is
// the spawn-time half of AnimMap_RegisterEntity: late joiner-local/remote players must not
// retain the default E_STAND map or configured emplacements fall back to anim_emplaced.
// [orig: AnimMap_RegisterEntity @0x40bb60; AnimMap_UpdateEntity @0x40b5f0.]
void NovaSimulation::resolve_new_infantry_adm_ids() {
	if (!world_ || !world_->ai || infantry_adm_resource_root_.is_null() ||
			infantry_adm_item_db_.is_null() || infantry_anim_.empty())
		return;
	AiSystem &ai = *world_->ai;
	const int count = ai.count();
	if (infantry_adm_resolved_ai_count_ < 0 ||
			infantry_adm_resolved_ai_count_ > count)
		infantry_adm_resolved_ai_count_ = 0;
	for (int i = infantry_adm_resolved_ai_count_; i < count; ++i) {
		AiEntity *e = ai.at(i);
		if (!e) continue;
		e->inf.adm_id = 0;
		if (!e->inf.active) continue;
		const opennova::world::Entity *ent = world_->registry.get(e->handle);
		if (!ent) continue;
		const int visual_item_id =
				visual_item_id_for_runtime_type(ent->item_id, infantry_adm_item_db_);
		String adm = infantry_adm_item_db_->get_anim_def(visual_item_id);
		if (adm.is_empty()) continue;
		if (!adm.to_lower().ends_with(".adm")) adm += ".adm";
		const int adm_id =
				infantry_anim_.register_adm(infantry_adm_resource_root_, adm);
		if (adm_id >= 0) e->inf.adm_id = adm_id;
	}
	infantry_adm_resolved_ai_count_ = count;
}

// Per-entity .adm resolution: ground each soldier off its OWN model's clip, not the shared
// default set (adm_id 0). Retain the shell inputs because multiplayer players are spawned
// after this mission-load sweep; the step/spawn hooks above the world layer resolve each
// later AiSystem entry exactly once.
void NovaSimulation::resolve_infantry_adm_ids(const Ref<NovaResourceRoot> &p_resource_root,
		const Ref<NovaItemDatabase> &p_item_db) {
	if (p_resource_root.is_null() || p_item_db.is_null()) return;
	infantry_adm_resource_root_ = p_resource_root;
	infantry_adm_item_db_ = p_item_db;
	reset_infantry_adm_ids();
	resolve_new_infantry_adm_ids();
}

// Stamp every live entity's items.def-derived wire traits via the item database:
// - Entity::is_ai_capable from ItemDefAttrib & 0x100000 (AIData): the host's pool-1 0x0D stream
//   emits its AI-trailer iff AI-capable, matching the stock 0x0D decoder's own gate exactly
//   (itemDef.attrib & 0x100000 @0x433327) — byte-faithful AND crash-safe (D-NET-97).
// - Entity::net_class_code from the items.def class tag (ai_function, else move_function — the
//   directive that drives the ItemDef+356 serialize-callback lookup [orig: ingame_decode.h §5.10b])
//   via opennova::class_from_tag. Load-bearing: only witnessed callback classes may be serialized
//   into the 0x0A event loop — classifying a pool-1 ewep emplacement as a vehicle desyncs the
//   retail client mid-frame (retail-join v13, 2026-07-02).
// - Entity::health_max (+ health lift) from items.def hp (itemDef+0x17C healthMax): the original
//   spawns Health = healthMax [orig: Entity_InitFromItemDef @0x49e550]; entities still at the
//   promotion default (100) are lifted to full health. Feeds the §5.13 vehicle health word (a
//   too-small value renders every vehicle burning) and the §5.10 field-17 tier denominator.
//
// The registry's for_each is const-only, so collect the live handles first, then re-fetch each as
// a mutable Entity* — the same mutate-by-handle shape resolve_infantry_adm_ids uses.
//
// ID SPACE (load-bearing): Entity::item_id is the WIRE type id — the small on-disk .bms type that
// build_pool*_batch puts on the wire verbatim (e.g. 0x050E). NovaItemDatabase is keyed by the
// items.def id, which is wire + kItemIdOffset (mission_bms_test: bms_type_id 1291 -> item_id
// 101291; nova_net_client.cpp wire = def_id - 100000). The offset here is mandatory: without it
// every pool-1 lookup misses.
// [orig: NapiNPClientMsg_0x00D @0x432c40; docs/net/novaworld-net-re.md D-NET-97]
void NovaSimulation::resolve_item_traits(const Ref<NovaItemDatabase> &p_item_db) {
	if (!world_ || p_item_db.is_null()) return;
	item_traits_db_ = p_item_db;
	// Cache the Player template's items.def hp at world level so LATE-JOINER spawns (which happen
	// after this sweep) seed full health without an item-db reach-back from libs/ [orig:
	// Entity_InitFromItemDef @0x49e550 — spawn Health = itemDef->healthMax]. (D-NET-144)
	const int player_def_id =
			static_cast<int>(opennova::world::kPlayerInfantryTypeId) +
			opennova::mission::kItemIdOffset;
	world_->player_has_item_def = p_item_db->has_item(player_def_id);
	world_->player_item_hp = opennova::world::retail_signed_i16(
			p_item_db->get_hp(player_def_id));
	world_->player_item_type = static_cast<uint8_t>(p_item_db->get_item_type(player_def_id));
	world_->player_item_attrib = p_item_db->get_attrib(player_def_id);
	world_->player_armor_impact = opennova::world::retail_signed_i16(
			p_item_db->get_armor_impact(player_def_id));
	world_->player_armor_kz = opennova::world::retail_signed_i16(
			p_item_db->get_armor_kz(player_def_id));
	world_->player_damage_reduc_pp = p_item_db->get_damage_reduc_pp(player_def_id);
	world_->player_damage_reduc_max = p_item_db->get_damage_reduc_max(player_def_id);
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each([&](const opennova::world::Entity &e) { handles.push_back(e.handle); });
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (!e) continue;
		const int def_id = static_cast<int>(e->item_id) + opennova::mission::kItemIdOffset;
		e->has_item_def = p_item_db->has_item(def_id);
		e->item_type = static_cast<uint8_t>(p_item_db->get_item_type(def_id));
		e->is_ai_capable = p_item_db->is_ai_capable(def_id);
		// §5.10b replication class from the *_function tag (ai_function, else move_function).
		const String ai_fn = p_item_db->get_ai_function(def_id);
		const String tag = ai_fn.is_empty() ? p_item_db->get_move_function(def_id) : ai_fn;
		e->net_class_code =
				static_cast<uint8_t>(opennova::class_from_tag(tag.utf8().get_data()));
		// items.def hp -> healthMax; lift spawn-default health to full [orig: @0x49e550].
		const int hp = opennova::world::retail_signed_i16(p_item_db->get_hp(def_id));
		e->health = opennova::world::retail_signed_i16(e->health);
		e->health_max = opennova::world::retail_signed_i16(e->health_max);
		e->armor_impact = opennova::world::retail_signed_i16(
				p_item_db->get_armor_impact(def_id));
		e->armor_kz = opennova::world::retail_signed_i16(
				p_item_db->get_armor_kz(def_id));
		e->damage_reduc_pp = p_item_db->get_damage_reduc_pp(def_id);
		e->damage_reduc_max = p_item_db->get_damage_reduc_max(def_id);
		if (hp != 0) {
			e->health_max = hp;
			if (e->health == 100) e->health = hp; // still at the promotion default
		}
		// Indestructible item (def hp == 0): entity Flags |= 0x4000000 and subType = 0xFF —
		// the def-sourced half of the 0x10 static record's flag dword / flag-0x80 byte
		// (D-NET-147; every golden ASH_I5A building carries both). Resolved defs only — a
		// missing items.def id stays untouched. [orig: Entity_InitFromModel @0x40dc8e:
		// !itemDef->healthMax -> Flags |= 0x4000000, Health = 1, subType = -1]
		if (hp == 0 && p_item_db->has_item(def_id)) {
			e->engine_flags |= 0x4000000u;
			e->sub_type = 0xFF;
		}
		// AS zone traits from the attrib dword: 0x20000 "ChangeTeam" = capture trigger,
		// 0x40000 "SpawnPoint" = deploy-selectable (the ASH_I5A "Change Team & Spawn
		// Volume" objects carry both). [orig: def+84 gates in ZoneSlotChain_BuildFromMission
		// @0x4a2de0 / Server_ResolveSpawnTargetHandle @0x4fe110; net-re §5.61]
		const uint32_t attrib = p_item_db->get_attrib(def_id);
		e->item_attrib = attrib;
		e->is_capture_trigger = (attrib & DEF_ITEM_ATTRIB_CHANGETEAM) != 0;
		e->is_spawn_point = (attrib & DEF_ITEM_ATTRIB_SPAWNPOINT) != 0;
		// Death-presentation traits: LeaveCorpse (attrib 0x400000) keeps the corpse
		// forever; deathtime (def+0x890, parse-scaled ticks) seeds the corpse timer at
		// the death edge. [orig: ItemDef_ParseProperty @0x4a09d3 / @0x49fa6c; consumers
		// Entity_UpdateInfantryAI @0x4b9e54 / @0x4b9c97; world-wac-ai-re §19]
		e->leave_corpse = (attrib & DEF_ITEM_ATTRIB_LEAVECORPSE) != 0;
		e->deathtime_ticks = p_item_db->get_deathtime_ticks(def_id);
		// Destruction traits (world/destruction.h; world-wac-ai-re §24): the death
		// chain's def fields, keyed by item id. Fills once per distinct id.
		// [orig: the ItemDef fields Entity_ApplyWeaponDamage / the death dispatch /
		// Entity_InitDeathSounds read — armor +0x190/+0x192, unitType +0x196, kz
		// +0x198, huskSubPart* +0x100.., debrisScale +0x1BC, soundDeath +0x860,
		// the particledeath family +0x412..]
		if (world_->item_death_traits.get(e->item_id) == nullptr &&
		    p_item_db->has_item(def_id)) {
			const Dictionary dt = p_item_db->get_death_traits(def_id);
			if (!dt.is_empty()) {
				opennova::world::ItemDeathTraits t;
				t.unit_type = int(dt.get("unit_type", 0));
				t.kz = float(double(dt.get("kz", 0.0)));
				t.armor_impact = int(dt.get("armor_impact", 0));
				t.armor_blast = int(dt.get("armor_blast", 0));
				t.team_protect = (attrib & 0x8000u) != 0; // 0x8000 is NOT in the witnessed attrib token table — stays raw
				t.no_die = (attrib & DEF_ITEM_ATTRIB_NODIE) != 0;
				t.static_death = (p_item_db->get_attrib2(def_id) & DEF_ITEM_ATTRIB2_STATICDEATH) != 0;
				t.has_husk = bool(dt.get("has_husk", false));
				t.is_decoration =
						p_item_db->get_item_type(def_id) == NovaItemDatabase::TYPE_DECORATION;
				t.husk_sub_part_count =
						static_cast<uint8_t>(std::clamp(int(dt.get("husk_sub_parts", 0)), 0, 255));
				const PackedInt32Array types = dt.get("husk_sub_part_types", PackedInt32Array());
				for (int s = 0; s < 17 && s < types.size(); ++s)
					t.husk_sub_part_types[s] = static_cast<uint8_t>(types[s]);
				t.debris_scale = float(double(dt.get("debris_scale", 0.0)));
				t.sound_death = String(dt.get("sounddeath", String())).utf8().get_data();
				const Dictionary fx = p_item_db->get_particle_effects(def_id);
				t.particledeath = String(fx.get("particledeath", String())).utf8().get_data();
				t.particleh2odeath =
						String(fx.get("particleh2odeath", String())).utf8().get_data();
				t.particlefire = String(fx.get("particlefire", String())).utf8().get_data();
				t.particleother = String(fx.get("particleother", String())).utf8().get_data();
				// resolve_collision_instances enriches this row with live husk-model
				// state and the active first-stage husk's KZ user points.
				world_->item_death_traits.set(e->item_id, std::move(t));
			}
		}
		// Vehicle motor traits: the pre-scaled items.def physics block + the PlayerControl
		// attrib (0x40) gate, keyed by item id in the world table. Fills once per distinct
		// id; the AI tick's vehicle pass drives pool-1 entities whose traits carry a
		// non-zero `physics` selector. [orig: ItemDef_ParsePhysicsProperty @0x49d870;
		// Entity_UpdateVehiclePhysics @0x48af00 attrib & 0x40 gate @0x48b0e6]
		if (e->handle.pool() == 1 &&
		    world_->vehicle_traits.get(e->item_id) == nullptr) {
			const PackedInt32Array vp = p_item_db->get_vehicle_physics(def_id);
			if (vp.size() == 8 && vp[0] != 0) {
				opennova::world::VehicleTraits vt;
				vt.physics = vp[0];
				vt.player_speed = vp[1];
				vt.acceleration = vp[2];
				vt.deceleration = vp[3];
				vt.turn_rate = vp[4];
				vt.turn_rate2 = vp[5];
				vt.unit_type = vp[6];
				vt.torque = vp[7];
				vt.player_control = (attrib & DEF_ITEM_ATTRIB_PLAYERCONTROL) != 0;
				// Vehicle audio belongs to the vehicle ItemDef, not to the
				// mounted NPC's AiProfile. Resolve the profile name and the
				// item-level soundloop overrides once at this portable boundary.
				// [orig: ItemDef_ResolveAllResources @0x49e5f0/@0x49e7f0]
				vt.sound_profile = p_item_db->get_sound_profile(def_id).utf8().get_data();
				const PackedStringArray loops = p_item_db->get_sound_loops(def_id);
				for (int i = 0; i < loops.size() &&
						i < static_cast<int>(vt.sound_loops.size()); ++i) {
					vt.sound_loops[static_cast<size_t>(i)] =
							String(loops[i]).utf8().get_data();
				}
				world_->vehicle_traits.set(e->item_id, vt);
			}
		}
	}
	// Throwable class bindings: every items.def entry whose ai_function /
	// move_function names a throwable class (nade/schl/clym/vmne/lndm) lands a
	// row keyed by type id (id - 100000, the ammo TrcrID space), with the def
	// hp/armor the placed device spawns at. [orig: EntityDef_InitAllCallbacks
	// @ 0x4a5a70 resolves the class tables into every item def at load;
	// world-wac-ai-re §27.]
	world_->throwables.classes.clear();
	const PackedInt32Array all_ids = p_item_db->get_item_ids();
	for (int i = 0; i < all_ids.size(); ++i) {
		const int def_id = all_ids[i];
		const opennova::world::ThrowClass think = opennova::world::throw_class_from_tag(
				p_item_db->get_ai_function(def_id).utf8().get_data());
		const opennova::world::ThrowClass motor = opennova::world::throw_class_from_tag(
				p_item_db->get_move_function(def_id).utf8().get_data());
		if (think == opennova::world::ThrowClass::kNone &&
				motor == opennova::world::ThrowClass::kNone)
			continue;
		opennova::world::ThrowableClassRow row;
		row.item_id = def_id - opennova::mission::kItemIdOffset;
		row.think = think;
		row.motor = motor;
		row.health_max = opennova::world::retail_signed_i16(p_item_db->get_hp(def_id));
		row.armor_impact = opennova::world::retail_signed_i16(
				p_item_db->get_armor_impact(def_id));
		row.armor_kz = opennova::world::retail_signed_i16(
				p_item_db->get_armor_kz(def_id));
		world_->throwables.classes.set(row);
	}
	// The AS zone-slot chain — built AFTER the trait stamp (zone registration keys on
	// is_capture_trigger), then the secure latch seeds each rear zone's control to 1.0.
	// [orig: ZoneSlotChain_BuildFromMission @0x4a2de0 from Game_StartMission @0x526126;
	// the latch is Server_UpdateCaptureZoneEntities' first act @0x519764; net-re §5.61]
	opennova::world::zone_chain_build_from_mission(*world_, world_->zone_chain);
	opennova::world::zone_chain_latch_control(*world_, world_->zone_chain);

	// Decode-side twin of the net_class_code stamp above: the full wire-id ->
	// replication-class table for the LOCAL CLIENT VIEW. The retail client sizes each
	// inbound 0x0A tag-1 record via its OWN items.def serialize callback [orig:
	// itemDef+356 dispatch @0x50f2e2 / ItemList_FindIndexByTypeId]; without this table
	// the view's phase-1 heuristic walks vehicle (15/21 B) and no-callback (0 B)
	// records at the wrong width and desyncs the rest of the frame — every junk record
	// after the desync lands anchor-relative, i.e. scattered around the local player.
	// Same tag rule as the encoder stamp (ai_function, else move_function) so both
	// sides of the in-process wire agree by construction.
	auto table = std::make_shared<std::unordered_map<uint16_t, opennova::EntityClass>>();
	const PackedInt32Array ids = p_item_db->get_item_ids();
	for (int i = 0; i < ids.size(); ++i) {
		const int def_id = ids[i];
		const int wire_id = def_id - opennova::mission::kItemIdOffset;
		if (wire_id < 0 || wire_id > 0xFFFF) continue;
		const String ai_fn = p_item_db->get_ai_function(def_id);
		const String tag = ai_fn.is_empty() ? p_item_db->get_move_function(def_id) : ai_fn;
		const opennova::EntityClass cls =
				opennova::class_from_tag(tag.utf8().get_data());
		if (cls != opennova::EntityClass::Unknown) {
			(*table)[static_cast<uint16_t>(wire_id)] = cls;
		}
	}
	item_class_table_ = std::move(table);
	install_item_class_resolver();
}

void NovaSimulation::install_item_class_resolver() {
	if (!runtime_ || !item_class_table_) return;
	runtime_->view().set_item_class_resolver(
			[table = item_class_table_](uint16_t type_id) {
				const auto it = table->find(type_id);
				return it != table->end() ? it->second : opennova::EntityClass::Unknown;
			});
}

void NovaSimulation::install_charattr_challenge_table() {
	if (!runtime_) return;
	if (charattr_challenge_loaded_) {
		runtime_->set_charattr_challenge_table(charattr_challenge_table_);
	} else {
		runtime_->clear_charattr_challenge_table();
	}
}

void NovaSimulation::install_character_join_vars() {
	if (!runtime_ || !join_character_vars_set_) return;
	runtime_->set_character_join_vars(join_character_vars_);
}

// The D-AI-5 host weapon seed. The original resolves the items.def ammo_closeattack/
// easyrocket/advancedrocket/marker3 names into ammo-def ids on the def and block-copies
// them onto the entity (+0x358..0x35B; the copy site is the open world-wac-ai-re §17.7
// item 1 — no per-field writer exists). Until that copy is witnessed, the port carries
// ONE ammo id + clipsize per NPC (AiProfile — JO riflemen author all four slots to the
// same rifle round), stamped here from the item database against the loaded ammo table.
// Also seeds the spawn magazine: word entity+0x35C = itemDef+0x894 clipsize [orig:
// Entity_ResetToSpawnState @ 0x4b97a9/0x4b97b5]. Consumption stays motor-gated: only
// the infantry fire pass reads ammo_primary (host-side NPCs; never the local player).
// [orig: ItemDef_ParseProperty @ 0x4a1823 (-> def+0x56B) / @ 0x49fa1c (-> def+0x894);
// docs/divergence-ledger.md D-AI-5]
int NovaSimulation::resolve_ai_weapons(const Ref<NovaItemDatabase> &p_item_db) {
	if (!world_ || !world_->ai || p_item_db.is_null()) return 0;
	int armed = 0;
	// Bind every body's sound profile first — persons AND vehicles carry one
	// (e.g. DBuggy01 -> SP_DuneBuggy), and unarmed defs (the player) must not
	// skip it. An unauthored key resolves to "default" via the emit-side
	// fallback (index stays -1). [orig: the def+0x268 parse binding
	// @ 0x49fb0f-0x49fb64; alloc seed @ 0x49e3f5]
	if (!world_->sound_profiles.empty()) {
		for (int i = 0; i < world_->ai->count(); ++i) {
			opennova::world::AiEntity *ae = world_->ai->at(i);
			if (ae == nullptr) continue;
			const opennova::world::Entity *e = world_->registry.get(ae->handle);
			if (e == nullptr) continue;
			const int def_id = static_cast<int>(e->item_id) + opennova::mission::kItemIdOffset;
			const String prof = p_item_db->get_sound_profile(def_id);
			if (prof.is_empty()) continue;
			ae->profile.sound_profile = static_cast<int16_t>(
				world_->sound_profiles.index_of(prof.utf8().get_data()));
		}
	}
	if (world_->ammo.empty()) return 0; // no ammo.def loaded — NPCs stay unarmed
	for (int i = 0; i < world_->ai->count(); ++i) {
		opennova::world::AiEntity *ae = world_->ai->at(i);
		if (ae == nullptr) continue;
		const opennova::world::Entity *e = world_->registry.get(ae->handle);
		if (e == nullptr) continue;
		const int def_id = static_cast<int>(e->item_id) + opennova::mission::kItemIdOffset;
		const String ammo_name = p_item_db->get_ammo_closeattack(def_id);
		if (ammo_name.is_empty()) continue; // def authors no anim-fire round (e.g. the player)
		const int ammo = world_->ammo.index_of(ammo_name.utf8().get_data());
		if (ammo < 0) continue; // name not in this mission's ammo.def — stay unarmed
		ae->profile.ammo_primary = ammo;
		ae->profile.clip_size = p_item_db->get_clipsize(def_id);
		ae->inf.magazine = static_cast<int16_t>(ae->profile.clip_size);
		++armed;
	}
	return armed;
}

void NovaSimulation::apply_collision_to_ai() {
	collision_world_.terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	collision_world_.set_section_matrix_provider(this);
	if (world_) {
		world_->collision = &collision_world_;
		world_->mounted_pose_provider = this;
	}
	if (ai_) ai_->collision = &collision_world_;
}

bool NovaSimulation::resolve_mounted_pose(
		opennova::world::World &p_world,
		const opennova::world::Entity &p_carrier,
		const opennova::world::Seat &p_seat,
		opennova::world::MountedPose &r_out) {
	if (!world_ || &p_world != world_.get() ||
			p_seat.type != opennova::world::SeatType::Gunner ||
			p_seat.bone_index == 0)
		return false;
	const auto data_found =
			mounted_pose_data_by_type_.find(p_carrier.item_id);
	if (data_found == mounted_pose_data_by_type_.end() ||
			data_found->second.is_null())
		return false;
	const Ref<NovaObjectData> &data = data_found->second;

	Dictionary controls;
	const ThreediModelIR &ir = data->native_ir();
	if (ir.control_register_count > 0 && ir.control_registers == nullptr)
		return false;
	AiEntity *carrier_ai = ai_ ? ai_->for_handle(p_carrier.handle) : nullptr;
	assign_part_anim_phases(
			controls, (p_carrier.item_attrib & 0x1000u) == 0,
			[carrier_ai](int p_channel) {
		return carrier_ai != nullptr
				? carrier_ai->brain.f[
						AiBrain::kPartAnimPhase0 + p_channel]
				: 0;
	});
	// This is the exact witnessed publisher scope: a live UseGun child is being
	// posed from the parent carrier's PANM/bone transform, so cache the
	// carrier's inline MountSlot before evaluating that parent model.
	// [orig: Entity_AttachToBoneAndUpdateTransform @ 0x546518..0x54652B;
	//  HUD_CacheWeaponSlotInfo @ 0x44095B..0x440991]
	assign_world_model_heat_glow(controls, p_world, p_carrier);
	EmplacedWeaponControls emplaced;
	if (emplaced_weapon_controls_for(
			p_world, ai_.get(), p_carrier, emplaced)) {
		controls[String(kEmplacedGunYawRegister)] =
				static_cast<int>(emplaced.gun_yaw);
		controls[String(kEmplacedGunPitchRegister)] =
				static_cast<int>(emplaced.gun_pitch);
	}
	const uint32_t time_ms = panm_time_override_ms_ >= 0
			? static_cast<uint32_t>(panm_time_override_ms_)
			: p_world.logic_tick * 16u;
	// Keep authority and joiner attachment reconstruction on one matrix path:
	// both consume the same authored userpoint and rest/live bone transforms.
	return resolve_model_mounted_pose(
			data, p_carrier, p_seat, controls, time_ms, r_out);
}

bool NovaSimulation::ensure_collision_instance(
		opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity) {
	if (!world_ || &p_world != world_.get() ||
			collision_item_db_.is_null() || collision_placer_.is_null())
		return false;
	const opennova::world::Entity *entity = p_world.registry.get(p_entity);
	if (entity == nullptr) {
		collision_world_.remove_entity_instance(p_entity);
		collision_skeletal_sources_.erase(p_entity.packed);
		collision_resolution_attempted_.erase(p_entity.packed);
		return false;
	}
	const auto attempted =
			collision_resolution_attempted_.find(p_entity.packed);
	if (attempted != collision_resolution_attempted_.end()) {
		if (attempted->second == entity->registry_spawn_id)
			return collision_world_.has_instance(p_world, p_entity);
		collision_world_.remove_entity_instance(p_entity);
		collision_skeletal_sources_.erase(p_entity.packed);
		collision_resolution_attempted_.erase(attempted);
	}

	// Re-run the idempotent attach sweep against the retained mission caches.
	// It resolves every entity that appeared since the previous sweep, including
	// a player deployed after load, without registering another graphic model.
	resolve_collision_instances(collision_item_db_, collision_placer_.ptr());
	return collision_world_.has_instance(p_world, p_entity);
}

bool NovaSimulation::build_section_matrices(opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity, int32_t p_model_id,
		const opennova::world::CollisionMatrix &p_entity_world,
		const opennova::world::CollisionModel &p_model,
		std::vector<opennova::world::CollisionMatrix> &r_out) {
	const auto skeletal_found =
			collision_skeletal_sources_.find(p_entity.packed);
	const opennova::world::Entity *entity =
			p_world.registry.get(p_entity);
	if (skeletal_found != collision_skeletal_sources_.end() &&
			skeletal_found->second.model_id == p_model_id &&
			entity != nullptr && skeletal_found->second.registry_spawn_id ==
					entity->registry_spawn_id) {
		const SkeletalCollisionSource &source = skeletal_found->second;
		AiEntity *ai_entity = ai_ ? ai_->for_handle(p_entity) : nullptr;
		const size_t section_count = p_model.sections.size();
		if (source.anim.is_null() || ai_entity == nullptr || entity == nullptr ||
				source.parents.size() < section_count ||
				source.rest_global.size() < section_count ||
				source.overlay_classes.size() < static_cast<int64_t>(section_count))
			return false;

		const String reset_key("anim_reset");
		auto resolve_primary_key = [&](const String &p_key) {
			if (source.anim->has_clip(p_key)) return p_key;
			return source.anim->has_clip(reset_key) ? reset_key : p_key;
		};
		const String primary_key =
				resolve_primary_key(infantry_anim_key(ai_entity->inf.anim_state));
		if (primary_key.is_empty()) return false;
		const float primary_fps = source.anim->get_clip_fps(primary_key, 0);
		const double primary_seconds = primary_fps > 0.0f
				? static_cast<double>(std::max(ai_entity->inf.clip_phase, 0)) /
						(2.0 * primary_fps)
				: 0.0;
		String source_key;
		double source_seconds = 0.0;
		const bool primary_blend = ai_entity->inf.body_blend_active();
		if (primary_blend) {
			source_key = resolve_primary_key(
					infantry_anim_key(ai_entity->inf.anim_prev));
			const float source_fps = source.anim->get_clip_fps(source_key, 0);
			if (source_fps > 0.0f)
				source_seconds =
						static_cast<double>(
								std::max(ai_entity->inf.anim_prev_clip_phase, 0)) /
						(2.0 * source_fps);
		}

		const opennova::anim::AimOverlayInputs inputs =
				aim_overlay_inputs_for(*ai_entity, *entity);
		opennova::anim::AimOverlayAngles
				angles[opennova::anim::kOverlayClassCount];
		opennova::anim::compute_aim_overlay_angles(inputs, angles);
		const Array deltas = aim_overlay_deltas_for(angles);

		String weapon_key;
		double weapon_seconds = 0.0;
		const bool collapse_right_hand =
				mount_collapses_right_hand_row(*entity);
		if (p_world.cached.local_player.valid() &&
				p_entity.packed == p_world.cached.local_player.packed &&
				opennova::world::infantry_weapon_channel_visible(
						ai_entity->inf, weapon_active_,
						mount_blocks_weapon_channel(*entity))) {
			weapon_key = infantry_anim_key(ai_entity->inf.wpn_state);
			const float weapon_fps = source.anim->get_clip_fps(weapon_key, 0);
			if (weapon_fps > 0.0f)
				weapon_seconds =
						static_cast<double>(
								std::max(ai_entity->inf.wpn_clip_phase, 0)) /
						(2.0 * weapon_fps);
		}

		const Array pose = primary_blend
				? source.anim->eval_pose_blended_overlay(
						source_key, source_seconds,
						primary_key, primary_seconds,
						ai_entity->inf.anim_blend_weight,
						source.overlay_classes, deltas,
						weapon_key, weapon_seconds, collapse_right_hand)
				: source.anim->eval_pose_overlay(
						primary_key, primary_seconds,
						source.overlay_classes, deltas,
						weapon_key, weapon_seconds, collapse_right_hand);
		if (pose.size() < static_cast<int64_t>(section_count)) return false;

		// The callback result is FINAL world-space. Build the body placement from
		// the overlay's body class (not the aim heading), then apply the skinned
		// deformation exactly once. At bind pose pose_global*rest_global^-1 is
		// identity, which guards against both double-rest and double-entity
		// translation. COBJ parent/offset/CXLT are deliberately not selectors:
		// COBJ[i] pairs strictly with this output slot i.
		const int32_t position[3] = {
				p_entity_world.m[3], p_entity_world.m[7], p_entity_world.m[11]};
		const opennova::world::CollisionMatrix body_world =
				opennova::world::collision_matrix_from_euler(
						angles[opennova::anim::kOverlayBody].yaw,
						angles[opennova::anim::kOverlayBody].pitch,
						angles[opennova::anim::kOverlayBody].roll, position);
		std::vector<Transform3D> pose_global(section_count);
		r_out.resize(section_count);
		for (size_t i = 0; i < section_count; ++i) {
			const Variant value = pose[static_cast<int64_t>(i)];
			if (value.get_type() != Variant::TRANSFORM3D) return false;
			const Transform3D local = static_cast<Transform3D>(value);
			const int32_t parent = source.parents[i];
			// eval_pose_overlay emits BN17's zero-scale local clip pose.
			// Retail zeroes the FINAL collision row after overlay/re-anchor. Preserve
			// that literal collision result for COBJ 16: composing body_world here
			// would incorrectly reintroduce the entity translation.
			// [orig: special row @0x4b1290]
			const bool collapsed_right_hand =
					collapse_right_hand && i == 16;
			if (collapsed_right_hand) {
				pose_global[i] = local;
				r_out[i] = opennova::world::CollisionMatrix{};
				continue;
			}
			pose_global[i] = parent >= 0
					? pose_global[static_cast<size_t>(parent)] * local
					: local;
			const Transform3D deformation =
					pose_global[i] * source.rest_global[i].affine_inverse();
			float render_pose[16];
			panm_render_matrix_from_godot(deformation, render_pose);
			if (!opennova::world::collision_matrix_apply_render_pose(
						body_world, render_pose, r_out[i]))
				return false;
		}
		return true;
	}

	const auto found = collision_pose_data_.find(p_model_id);
	if (found == collision_pose_data_.end() || found->second.is_null()) return false;
	const Ref<NovaObjectData> &data = found->second;
	// Retail Generic collision always transforms the canonical first RLOD. It
	// never follows the render-selected LOD or scans for another live PANM.
	constexpr int lod_index = 0;
	if (!data->has_live_panm_for_lod(lod_index)) return false;
	const PackedInt32Array targets =
			data->get_effective_panm_targets(lod_index);
	if (targets.is_empty()) return false;
	const ThreediModelIR &ir = data->native_ir();
	if (ir.control_register_count > 0 && ir.control_registers == nullptr)
		return false;

	// PLAYPARTANIM publishes its two phase accumulators to the fixed retail
	// VEHICLE_SPECIAL1/2 registers. A brainless static still evaluates
	// free-running PANM with zero phase values.
	Dictionary controls;
	AiEntity *ai_entity = ai_ ? ai_->for_handle(p_entity) : nullptr;
	assign_part_anim_phases(
			controls, entity == nullptr || (entity->item_attrib & 0x1000u) == 0,
			[ai_entity](int p_channel) {
		return ai_entity != nullptr
				? ai_entity->brain.f[
						AiBrain::kPartAnimPhase0 + p_channel]
				: 0;
	});
	// The generic collision frame receives HEAT_GLOW only when this model is the
	// carrier in that same live UseGun attachment relation. The helper omits it
	// for every other entity; a scoped cold slot still writes literal zero.
	// [orig: attachment caller @ 0x546518;
	//  HUD_CacheWeaponSlotInfo cold/hot stores @ 0x440969/@0x440991]
	if (entity != nullptr)
		assign_world_model_heat_glow(controls, p_world, *entity);
	// EWEAP yaw/pitch are independent semantic CTRL writers. B50Cal consumes
	// this pair and is unaffected by the VEHICLE_SPECIAL publication above.
	EmplacedWeaponControls emplaced;
	if (entity != nullptr &&
			emplaced_weapon_controls_for(p_world, ai_.get(), *entity, emplaced)) {
		controls[String(kEmplacedGunYawRegister)] =
				static_cast<int>(emplaced.gun_yaw);
		controls[String(kEmplacedGunPitchRegister)] =
				static_cast<int>(emplaced.gun_pitch);
	}
	const uint32_t time_ms = panm_time_override_ms_ >= 0
			? static_cast<uint32_t>(panm_time_override_ms_)
			: (world_ != nullptr ? world_->logic_tick * 16u : 0u);
	const Dictionary transforms =
			data->evaluate_panm(lod_index, time_ms, controls);
	if (transforms.is_empty()) return false;

	// Default every COBJ slot to the Simple callback. Override only PANM nodes
	// whose target part ordinal exists as a collision section. This intentionally
	// ignores COBJ parent metadata and CXLT/offset records: CVRT is model-space.
	r_out.assign(p_model.sections.size(), p_entity_world);
	bool matched_section = false;
	for (int i = 0; i < targets.size(); ++i) {
		const int section = targets[i];
		if (static_cast<size_t>(section) >= r_out.size() ||
				!transforms.has(section))
			continue;
		const Variant value = transforms[section];
		if (value.get_type() != Variant::TRANSFORM3D) return false;
		float pose[16];
		panm_render_matrix_from_godot(static_cast<Transform3D>(value), pose);
		if (!opennova::world::collision_matrix_apply_render_pose(
					p_entity_world, pose, r_out[static_cast<size_t>(section)]))
			return false;
		matched_section = true;
	}
	return matched_section;
}

int NovaSimulation::resolve_collision_instances(const Ref<NovaItemDatabase> &p_item_db,
                                                Object *p_placer) {
	if (!world_ || p_item_db.is_null() || p_placer == nullptr) return 0;
	RefCounted *placer_ref = Object::cast_to<RefCounted>(p_placer);
	if (placer_ref == nullptr) return 0;
	collision_item_db_ = p_item_db;
	collision_placer_ = placer_ref;
	apply_collision_to_ai();
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each(
			[&](const opennova::world::Entity &e) { handles.push_back(e.handle); });
	int attached = 0;
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (!e || e->kind == opennova::world::EntityKind::Marker)
			continue;
		const auto previous_attempt =
				collision_resolution_attempted_.find(h.packed);
		if (previous_attempt != collision_resolution_attempted_.end() &&
				previous_attempt->second != e->registry_spawn_id) {
			collision_world_.remove_entity_instance(h);
			collision_skeletal_sources_.erase(h.packed);
		}
		collision_resolution_attempted_[h.packed] = e->registry_spawn_id;
		const bool is_organic =
				e->kind == opennova::world::EntityKind::Organic;
		const int def_id = is_organic
				? visual_item_id_for_runtime_type(e->item_id, p_item_db)
				: static_cast<int>(e->item_id) +
						opennova::mission::kItemIdOffset;
		const String graphic = p_item_db->get_graphic(def_id);
		if (graphic.is_empty()) continue;
		const std::string key(graphic.utf8().get_data());
		auto it = collision_model_by_graphic_.find(key);
		if (it == collision_model_by_graphic_.end()) {
			int32_t model_id = -1;
			int32_t occlusion_id = -1;
			float bound_radius = 0.0f;
			// Duck-typed MissionObjectPlacer.object_data_for(graphic) — the placer's
			// per-graphic NovaObjectData cache (the render path loads the same object).
			Ref<NovaObjectData> data = p_placer->call("object_data_for", graphic);
			if (data.is_valid()) {
				opennova::world::CollisionModel model;
				if (collision_model_from_ir(
						data->native_ir().collision, model, data->has_collision())) {
					model_id = collision_world_.add_model(std::move(model));
					if (data->has_live_panm_for_lod(0))
						collision_pose_data_[model_id] = data;
				}
				opennova::world::OcclusionModel occ;
				if (occlusion_model_from_ir(data->native_ir().occlusion, occ))
					occlusion_id = occlusion_world_.add_model(std::move(occ));
				bound_radius = model_bound_radius_from_ir(data->native_ir());
			}
			it = collision_model_by_graphic_.emplace(key, model_id).first;
			collision_occlusion_by_graphic_.emplace(key, occlusion_id);
			collision_radius_by_graphic_.emplace(key, bound_radius);
		}
		// The bound-sphere radius (entity+0 boundRadius) comes from the .3di
		// MODEL header bound, not the collision block — every placed item
		// carries one, so collision-less props are still hittable by rounds and
		// reachable by blasts. Raised to the husk model's bound below, then
		// padded +0.0625 [orig: Entity_InitFromModel @ 0x40dc30 — boundRadius =
		// max(gpm[5], husk gpm[5]) + 0x1000; the authored def scale factor is
		// not yet applied (tracked, D-COL-3)].
		float entity_bound = collision_radius_by_graphic_[key];
		if (it->second >= 0) {
			collision_world_.assign_entity(
					h, it->second, e->registry_spawn_id);
			++attached;
			if (is_organic) {
				collision_skeletal_sources_.erase(h.packed);
				Ref<NovaSkeletalAnim> skeletal =
						p_placer->call("skeletal_anim_for", def_id, graphic);
				const opennova::world::CollisionModel *person_model =
						collision_world_.model(it->second);
				if (skeletal.is_valid() && skeletal->is_loaded() &&
						person_model != nullptr) {
					const Array bones = skeletal->get_skeleton_bones();
					const size_t section_count = person_model->sections.size();
					if (bones.size() >= static_cast<int64_t>(section_count)) {
						SkeletalCollisionSource source;
						source.model_id = it->second;
						source.registry_spawn_id = e->registry_spawn_id;
						source.anim = skeletal;
						source.overlay_classes =
								skeletal->get_overlay_classes();
						source.parents.resize(section_count, -1);
						source.rest_global.resize(section_count);
						bool valid_rig =
								source.overlay_classes.size() >=
								static_cast<int64_t>(section_count);
						for (size_t i = 0; valid_rig && i < section_count; ++i) {
							const Variant bone_value =
									bones[static_cast<int64_t>(i)];
							if (bone_value.get_type() != Variant::DICTIONARY) {
								valid_rig = false;
								break;
							}
							const Dictionary bone = bone_value;
							const int32_t parent =
									static_cast<int32_t>(bone.get(
											"parent_index", -1));
							const Variant rest_value =
									bone.get("rest", Transform3D());
							if (parent < -1 ||
									parent >= static_cast<int32_t>(i) ||
									rest_value.get_type() != Variant::TRANSFORM3D) {
								valid_rig = false;
								break;
							}
							const Transform3D rest =
									static_cast<Transform3D>(rest_value);
							source.parents[i] = parent;
							source.rest_global[i] = parent >= 0
									? source.rest_global[
											static_cast<size_t>(parent)] * rest
									: rest;
						}
						if (valid_rig)
							collision_skeletal_sources_[h.packed] =
									std::move(source);
					}
				}
			}
		}
		// The husk-stage collision model: attached beside the graphic instance so
		// every query swaps to the wreck once Flags & 4 sets. The collision pick
		// is the FIRST husk stage (entity+52 huskModel), not huskFinal [orig: the
		// +52 substitution @ 0x538720 / @ 0x413086; D-AI-7 residual closed].
		const String first_husk_name_s = p_item_db->get_husk(def_id);
		const String final_husk_name_s = p_item_db->get_huskfinal(def_id);
		const String husk_name_s = first_husk_name_s.is_empty()
				? final_husk_name_s
				: first_husk_name_s;
		if (!husk_name_s.is_empty()) {
			// Retail keeps live huskModel and huskFinalModel pointers on the
			// entity. A successfully opened model supplies that pointer even when
			// it has no collision block; missing/corrupt assets leave it null.
			// [orig: Entity_ProcessBuildingDeath @ 0x49442c]
			Ref<NovaObjectData> first_husk_data;
			Ref<NovaObjectData> final_husk_data;
			if (!first_husk_name_s.is_empty())
				first_husk_data = p_placer->call("object_data_for", first_husk_name_s);
			if (!final_husk_name_s.is_empty())
				final_husk_data = p_placer->call("object_data_for", final_husk_name_s);
			if (opennova::world::ItemDeathTraits *t =
						world_->item_death_traits.get_mutable(e->item_id))
				t->husk_model_loaded =
						first_husk_data.is_valid() || final_husk_data.is_valid();
			const std::string husk_key(husk_name_s.utf8().get_data());
			Ref<NovaObjectData> husk_data = first_husk_name_s.is_empty()
					? final_husk_data
					: first_husk_data;
			auto hit = collision_model_by_graphic_.find(husk_key);
			if (hit == collision_model_by_graphic_.end()) {
				int32_t husk_model_id = -1;
				if (husk_data.is_valid()) {
					opennova::world::CollisionModel hmodel;
					if (collision_model_from_ir(
							husk_data->native_ir().collision, hmodel,
							husk_data->has_collision())) {
						husk_model_id = collision_world_.add_model(std::move(hmodel));
						if (husk_data->has_live_panm_for_lod(0))
							collision_pose_data_[husk_model_id] = husk_data;
					}
					collision_radius_by_graphic_.emplace(husk_key,
							model_bound_radius_from_ir(husk_data->native_ir()));
				}
				hit = collision_model_by_graphic_.emplace(
						husk_key, husk_model_id).first;
				collision_occlusion_by_graphic_.emplace(husk_key, -1);
			}
			if (hit->second >= 0 && it->second >= 0)
				collision_world_.assign_entity_husk(h, hit->second);
			// Retail's death-sound tail walks exact, case-insensitive "KZ"
			// user points on the active FIRST husk, not the huskFinal piece
			// model, and queues a radius-5 blast at every match. Cache this
			// metadata separately from collision registration: the same graphic
			// may already be resident as another entity's main model.
			// Unlike collision's legacy final-only fallback, the retail KZ walker
			// reads entity+52 huskModel. A def with only huskFinal has no KZ source
			// and therefore takes the entity-origin fallback blast.
			if (!first_husk_name_s.is_empty()) {
				auto kz_it = collision_husk_kz_points_by_graphic_.find(husk_key);
				if (kz_it == collision_husk_kz_points_by_graphic_.end()) {
					std::vector<opennova::world::Vec3> kz_points;
					if (husk_data.is_valid()) {
						const ThreediModelIR &ir = husk_data->native_ir();
						for (size_t up_index = 0;
								ir.userpoints != nullptr && up_index < ir.userpoint_count;
								++up_index) {
							const ThreediIRUserPoint &point = ir.userpoints[up_index];
							if (String::utf8(point.name).nocasecmp_to("KZ") != 0)
								continue;
							// IR is (-source y, source z, source x); destruction's
							// placement math consumes mission-local (x, y, z).
							kz_points.push_back(opennova::world::Vec3{
									point.position[2],
									-point.position[0],
									point.position[1]});
						}
					}
					kz_it = collision_husk_kz_points_by_graphic_.emplace(
							husk_key, std::move(kz_points)).first;
				}
				if (opennova::world::ItemDeathTraits *t =
							world_->item_death_traits.get_mutable(e->item_id);
						t != nullptr && t->kz_points.empty() && !kz_it->second.empty())
					t->kz_points = kz_it->second;
			}
			// The PIECE model is huskFINAL first [orig: @ 0x4934af
			// huskFinalModel ?: huskModel] — the opposite preference from the
			// collision husk pick above. Its LOD-0 part table feeds the
			// death-piece loop bound [orig: renderObj[8]+52 @ 0x49361a], the
			// per-section centers [orig: the section-row center @ 0x4938bf],
			// and section 0's z extents (the wreck ground-rest offset
			// [orig: @ 0x461e23-0x461e4b]). Its own cache, independent of the
			// collision cache: a husk graphic can double as some entity's main
			// graphic, which would leave the joint cache without an entry.
			const String piece_name_s = final_husk_name_s.is_empty()
					? first_husk_name_s
					: final_husk_name_s;
			const std::string piece_key(piece_name_s.utf8().get_data());
			auto hs = collision_husk_pieces_by_graphic_.find(piece_key);
			if (hs == collision_husk_pieces_by_graphic_.end()) {
				CollisionHuskPieceInfo info;
				Ref<NovaObjectData> hdata = final_husk_name_s.is_empty()
						? first_husk_data
						: final_husk_data;
				if (hdata.is_valid() && hdata->native_ir().lod_count > 0 &&
				    hdata->native_ir().lods != nullptr) {
					const ThreediIRLod &lod = hdata->native_ir().lods[0];
					info.sections = static_cast<int32_t>(lod.part_count);
					for (size_t pi = 0; lod.parts != nullptr && pi < lod.part_count;
							++pi) {
						const ThreediIRPart &part = lod.parts[pi];
						info.centers.push_back(opennova::world::Vec3{
								part.abs_position[0] + part.bounding_center[0],
								part.abs_position[1] + part.bounding_center[1],
								part.abs_position[2] + part.bounding_center[2]});
					}
					if (lod.parts != nullptr && lod.part_count > 0 &&
					    lod.primitives != nullptr) {
						const ThreediIRPart &p0 = lod.parts[0];
						bool any = false;
						for (int32_t pr = 0; pr < p0.primitive_count; ++pr) {
							const size_t idx =
									static_cast<size_t>(p0.primitive_start) + pr;
							if (idx >= lod.primitive_count) break;
							const ThreediIRPrimitive &prim = lod.primitives[idx];
							info.rest_min_z =
									any ? std::min(info.rest_min_z, prim.min[2])
									    : prim.min[2];
							info.rest_max_z =
									any ? std::max(info.rest_max_z, prim.max[2])
									    : prim.max[2];
							any = true;
						}
					}
				}
				hs = collision_husk_pieces_by_graphic_.emplace(
						piece_key, std::move(info)).first;
				if (hdata.is_valid())
					collision_radius_by_graphic_.emplace(piece_key,
							model_bound_radius_from_ir(hdata->native_ir()));
			}
			if (opennova::world::ItemDeathTraits *t =
						world_->item_death_traits.get_mutable(e->item_id)) {
				const CollisionHuskPieceInfo &info = hs->second;
				if (t->husk_section_count == 0 && info.sections > 0)
					t->husk_section_count = info.sections;
				if (t->husk_section_centers.empty() && !info.centers.empty())
					t->husk_section_centers = info.centers;
				t->husk_rest_min_z = info.rest_min_z;
				t->husk_rest_max_z = info.rest_max_z;
			}
			// The husk model's bound joins the entity bound max [orig:
			// Entity_InitFromModel @ 0x40dc30, the huskModel[5] compare].
			entity_bound = std::max(
					entity_bound, collision_radius_by_graphic_[husk_key]);
		}
		if (entity_bound > 0.0f && e->bound_radius <= 0.0f)
			e->bound_radius = entity_bound + 0.0625f;  // the +0x1000 16.16 pad
		const int32_t occ_id = collision_occlusion_by_graphic_[key];
		if (occ_id >= 0 && e->kind == opennova::world::EntityKind::Building) {
			// The def bits the occlusion engine reads: attrib2 bit 6 "weldable"
			// [orig: itemDef+88 >> 6 @ 0x5c5cce], attrib bit 27 recurse-windows
			// [orig: itemDef+84 >> 27 @ 0x5c7456]; the destruction bone-map
			// bytes (+2193/+2194) stay 0 until the destruction system lands
			// (D-COL-2 / D-OCC-9).
			opennova::world::OcclusionWorld::EntityDefBits bits;
			bits.weldable = (p_item_db->get_attrib2(def_id) & (1u << 6)) != 0;
			bits.recurse_windows = (p_item_db->get_attrib(def_id) & (1u << 27)) != 0;
			occlusion_world_.assign_entity(h, occ_id, bits);
		}
	}
	return attached;
}

void NovaSimulation::set_item_seat_specs(const Array &p_specs) {
	item_seat_specs_.clear();
	mounted_pose_data_by_type_.clear();
	for (int64_t i = 0; i < p_specs.size(); ++i) {
		const Variant spec_v = p_specs[i];
		if (spec_v.get_type() != Variant::DICTIONARY) continue;
		const Dictionary spec_d = spec_v;

		opennova::mission::ItemSeatSpec spec;
		spec.type_id = static_cast<int32_t>(spec_d.get("type_id", 0));
		if (spec.type_id == 0) continue;
		const Variant model_data_value =
				spec_d.get("model_data", Variant());
		if (model_data_value.get_type() == Variant::OBJECT) {
			Ref<NovaObjectData> model_data(model_data_value);
			if (model_data.is_valid() && model_data->has_document())
				mounted_pose_data_by_type_[spec.type_id] = model_data;
		}
		if (spec_d.has("mount_config_valid")) {
			spec.mount_config_valid = static_cast<bool>(spec_d.get("mount_config_valid", false));
			spec.mount_config = spec.mount_config_valid
			                        ? static_cast<int32_t>(spec_d.get("mount_config", 0))
			                        : 0;
		}

		const Variant seats_v = spec_d.get("seats", Array());
		if (seats_v.get_type() != Variant::ARRAY) continue;
		const Array seats_a = seats_v;
		bool retail_slots_used[10] = {};
		int inferred_passenger_slot = 0;
		for (int64_t j = 0; j < seats_a.size(); ++j) {
			const Variant seat_v = seats_a[j];
			if (seat_v.get_type() != Variant::DICTIONARY) continue;
			const Dictionary seat_d = seat_v;

			opennova::world::Seat seat;
			seat.type = seat_type_from_variant(static_cast<int>(seat_d.get("type", 0)));
			if (seat.type == opennova::world::SeatType::None) continue;
			int retail_slot = -1;
			if (seat_d.has("retail_slot")) {
				retail_slot = static_cast<int>(seat_d.get("retail_slot", -1));
			} else {
				// Compatibility for tests/tools that construct seat dictionaries
				// directly. Production extraction supplies the explicit slot.
				switch (seat.type) {
					case opennova::world::SeatType::Passenger:
						while (inferred_passenger_slot < 8 &&
						       retail_slots_used[inferred_passenger_slot])
							++inferred_passenger_slot;
						if (inferred_passenger_slot < 8)
							retail_slot = inferred_passenger_slot++;
						break;
					case opennova::world::SeatType::Controller:
					case opennova::world::SeatType::Driver:
						retail_slot = 8;
						break;
					case opennova::world::SeatType::Gunner:
						retail_slot = 9;
						break;
					default:
						break;
				}
			}
			const bool slot_matches_type =
					(retail_slot >= 0 && retail_slot < 8 &&
					 seat.type == opennova::world::SeatType::Passenger) ||
					(retail_slot == 8 &&
					 opennova::world::is_vehicle_control_seat(seat.type)) ||
					(retail_slot == 9 &&
					 seat.type == opennova::world::SeatType::Gunner);
			if (slot_matches_type && !retail_slots_used[retail_slot]) {
				seat.retail_slot = static_cast<uint8_t>(retail_slot);
				retail_slots_used[retail_slot] = true;
			}
			seat.bone_index = static_cast<uint8_t>(
			    std::clamp(static_cast<int>(seat_d.get("bone_index", 0)), 0, 255));
			seat.pose_index = static_cast<uint8_t>(
			    std::clamp(static_cast<int>(seat_d.get("pose_index", 0)), 0, 30));
			seat.source_name = String(seat_d.get("source_name", String())).utf8().get_data();
			const Vector3 pos = seat_d.get("position", Vector3());
			seat.seat_local = {static_cast<float>(pos.x), static_cast<float>(pos.y),
			                   static_cast<float>(pos.z)};
			seat.yaw_offset = static_cast<int16_t>(
			    std::clamp(static_cast<int>(seat_d.get("yaw_offset", 0)), -32768, 32767));
			spec.seats.push_back(seat);
		}
		const Variant attachments_v =
				spec_d.get("emplacement_attachments", Array());
		if (attachments_v.get_type() == Variant::ARRAY) {
			const Array attachments_a = attachments_v;
			for (int64_t j = 0; j < attachments_a.size(); ++j) {
				const Variant attachment_v = attachments_a[j];
				if (attachment_v.get_type() != Variant::DICTIONARY) continue;
				const Dictionary attachment_d = attachment_v;
				const int full_child_id =
						static_cast<int>(attachment_d.get("item_id", 0));
				const int child_type_id = full_child_id - 100000;
				if (child_type_id <= 0) continue;
				opennova::mission::ItemEmplacementAttachmentSpec attachment;
				attachment.child_type_id =
						static_cast<int32_t>(child_type_id);
				attachment.kind =
						static_cast<opennova::mission::EmplacementAttachmentKind>(
								std::clamp(static_cast<int>(
										attachment_d.get("kind", 0)), 0, 2));
				attachment.stored_slot = static_cast<uint8_t>(
						std::clamp(static_cast<int>(
								attachment_d.get("stored_slot", 0)), 0, 4));
				if (static_cast<bool>(
						attachment_d.get("designated_c", false)))
					attachment.attachment_flags |= 1;
				if (static_cast<bool>(
						attachment_d.get("designated_g", false)))
					attachment.attachment_flags |= 2;
				attachment.anchor_found = static_cast<bool>(
						attachment_d.get("anchor_found", false));
				attachment.anchor.type =
						opennova::world::SeatType::Gunner;
				attachment.anchor.attachment_frame = true;
				attachment.anchor.bone_index = static_cast<uint8_t>(
						std::clamp(static_cast<int>(
								attachment_d.get("bone_index", 0)), 0, 255));
				attachment.anchor.source_name =
						String(attachment_d.get(
								"source_name", String())).utf8().get_data();
				const Vector3 local =
						attachment_d.get("local", Vector3());
				attachment.anchor.seat_local = {
						static_cast<float>(local.x),
						static_cast<float>(local.y),
						static_cast<float>(local.z)};
				attachment.anchor.yaw_offset = static_cast<int16_t>(
						std::clamp(static_cast<int>(
								attachment_d.get("yaw_offset", 0)),
								-32768, 32767));
				attachment.angle_count = static_cast<uint8_t>(
						static_cast<int>(
								attachment_d.get("angle_count", 0)) == 4
								? 4 : 0);
				attachment.down_limit_bam = static_cast<int32_t>(
						attachment_d.get("down_limit_bam", 0));
				attachment.up_limit_bam = static_cast<int32_t>(
						attachment_d.get("up_limit_bam", 0));
				attachment.right_limit_bam = static_cast<int32_t>(
						attachment_d.get("right_limit_bam", 0));
				attachment.left_limit_bam = static_cast<int32_t>(
						attachment_d.get("left_limit_bam", 0));
				spec.emplacement_attachments.push_back(
						std::move(attachment));
			}
		}
		// The attach-label sources: "armory*" userpoint locals (Armory-attrib items
		// only — the host gates on itemdef attrib 0x80000) + the ewep primary_weapon
		// link [orig: @0x4361ee/@0x5a36f5; ItemDef+0x54B].
		const Variant armory_v = spec_d.get("armory_points", Array());
		if (armory_v.get_type() == Variant::ARRAY) {
			const Array armory_a = armory_v;
			for (int64_t j = 0; j < armory_a.size(); ++j) {
				if (armory_a[j].get_type() != Variant::VECTOR3) continue;
				const Vector3 p = armory_a[j];
				spec.armory_points.push_back({static_cast<float>(p.x),
				                              static_cast<float>(p.y),
				                              static_cast<float>(p.z)});
			}
		}
		spec.primary_weapon =
		    String(spec_d.get("primary_weapon", String())).utf8().get_data();
		if (spec.mount_config_valid || !spec.seats.empty() || !spec.armory_points.empty() ||
		    !spec.primary_weapon.empty() || !spec.emplacement_attachments.empty())
			item_seat_specs_.push_back(std::move(spec));
	}
	// Lookup table, ordered for the binary search in item_seat_spec_for_type —
	// a joiner probes it once per present row per frame.
	std::sort(item_seat_specs_.begin(), item_seat_specs_.end(),
			[](const opennova::mission::ItemSeatSpec &a,
					const opennova::mission::ItemSeatSpec &b) {
				return a.type_id < b.type_id;
			});
}
