#include "simulation/nova_simulation.h"

#include <wac/compiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "netsim/connection.h"
#include "netsim/entity_wire_bridge.h" // build_player_uplink (joiner-side C2S 0x0C body)

#include <npwire/ingame_decode.h> // class_from_tag (§5.10b *_function -> wire class)
#include <npwire/ingame_encode.h> // encode_organic_spawn_batch (+ OrganicSpawnBatch)

#include <npruntime/server_session.h> // set_connection_mode / set_transport_mode / create_session / mark_host_client_in_match
#include <npruntime/server_spawn.h>   // Server_ProcessPendingPlayerSpawns (faithful host-player auto-spawn)
#include <npruntime/server_tick.h>    // Server_TickUpdate (the single C2S drain + logic tick + 0x0A fan)
#include <npruntime/ammo_table_build.h>   // build_ammo_table + round_type resolve (§5.60)
#include <npruntime/weapon_table_build.h> // build_weapon_table (weapon.def -> world armory, D-NET-141)

#include <def/def.h> // def_parse_weapons_memory / def_free_weapons

#include <mission/bms.h>
#include <mission/mission.h>          // kItemIdOffset (wire type id -> items.def id)
#include <mission/mission_systems.h>
#include <anim/aim_overlay.h> // the torso-bend overlay blends [orig: @0x4b1290]
#include <io/bam.h>           // bam_add/bam_sar: the FP roll term composition
#include <world/angle.h>
#include <world/player_spawn.h>
#include <world/spawn_select.h>
#include <world/vehicle_attach.h> // player_toggle_vehicle_mount (the USE-ITEM toggle)

#include "object/nova_item_database.h"
#include "object/nova_object_data.h" // resolve_collision_instances: the .3di collision IR source
#include "resource_index/nova_resource_root.h"
#include "terrain/nova_terrain_data.h"

using namespace godot;
using opennova::world::AiBrain;
using opennova::world::AiEntity;
using opennova::world::AiSystem;
using opennova::world::TickContext;
using opennova::world::World;

namespace {

constexpr double kFixed16 = 65536.0;
constexpr int kPlayerVisualItemId = 105310; // items.def "Player #1, Single player" -> US01/US01.adm
// Canonical definition lives in libs/world/player_spawn.h (shared with the npruntime host).
constexpr uint16_t kRetailPlayerMinEntitySlot = opennova::world::kRetailPlayerMinEntitySlot;

uint64_t present_effect_origin_key(int kind, int index) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(kind)) << 32) |
	       static_cast<uint32_t>(index);
}

uint64_t perf_now_us() {
	using Clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

opennova::world::SeatType seat_type_from_variant(int value) {
	switch (value) {
		case static_cast<int>(opennova::world::SeatType::Passenger): return opennova::world::SeatType::Passenger;
		case static_cast<int>(opennova::world::SeatType::Controller): return opennova::world::SeatType::Controller;
		case static_cast<int>(opennova::world::SeatType::Gunner): return opennova::world::SeatType::Gunner;
		case static_cast<int>(opennova::world::SeatType::Driver): return opennova::world::SeatType::Driver;
		default: return opennova::world::SeatType::None;
	}
}

bool mount_blocks_weapon_channel(const opennova::world::Entity &entity) {
	if (!entity.mounted) return false;
	switch (entity.mount_type) {
		case opennova::world::SeatType::Controller:
		case opennova::world::SeatType::Gunner:
		case opennova::world::SeatType::Driver:
			return true;
		default:
			return false; // passenger seats retain the on-foot upper-body channel
	}
}

int visual_item_id_for_runtime_type(int item_id, const Ref<NovaItemDatabase> &item_db) {
	if (item_id == opennova::world::kPlayerInfantryTypeId && item_db.is_valid() &&
	    item_db->has_item(kPlayerVisualItemId)) {
		return kPlayerVisualItemId;
	}
	return item_id;
}

std::string dictionary_string(const Dictionary &d, const char *key, const std::string &fallback) {
	if (!d.has(key)) return fallback;
	const String value = d.get(key, String());
	return std::string(value.utf8().get_data());
}

void apply_dictionary_string(const Dictionary &d, const char *key, std::string &out) {
	if (d.has(key)) {
		out = dictionary_string(d, key, out);
	}
}

uint16_t dictionary_u16(const Dictionary &d, const char *key, uint16_t fallback) {
	if (!d.has(key)) return fallback;
	const int value = static_cast<int>(d.get(key, static_cast<int>(fallback)));
	return static_cast<uint16_t>(std::clamp(value, 0, 0xFFFF));
}

uint32_t dictionary_u32(const Dictionary &d, const char *key, uint32_t fallback) {
	if (!d.has(key)) return fallback;
	const int64_t value = static_cast<int64_t>(d.get(key, static_cast<int64_t>(fallback)));
	if (value < 0) return 0;
	if (value > 0xFFFFFFFFll) return 0xFFFFFFFFu;
	return static_cast<uint32_t>(value);
}

// Build the same synthetic patrol mission the C++ promote_test uses: 3 markers forming a
// path, one looping waypoint record (channel 1), 2 organics on that route, 1 building.
opennova::bms::File make_demo_mission() {
	opennova::bms::File m{};

	auto marker = [](int32_t x, int32_t y, int32_t z) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Marker;
		e.x = x; e.y = y; e.z = z;
		return e;
	};
	auto organic = [](int32_t x, int32_t y, int32_t z, uint8_t team, uint8_t wp_id) {
		opennova::bms::Entity e{};
		e.type = opennova::bms::ItemType::Organic;
		e.x = x; e.y = y; e.z = z;
		e.yaw = 90;
		e.team = team;
		e.waypoint_id = wp_id;
		e.wp_number = 0;
		e.min_engagement_distance = 50 << 16;
		e.max_engagement_distance = 500 << 16;
		return e;
	};

	m.markers.push_back(marker(100 << 16, 0, 0));
	m.markers.push_back(marker(200 << 16, 0, 0));
	m.markers.push_back(marker(300 << 16, 0, 0));

	opennova::bms::WaypointRecord wr{};
	wr.flags = opennova::bms::WaypointFlags::None; // loops
	wr.marker_count = 3;
	wr.waypoint_numbers = {0, 1, 2};
	m.waypoint_records.push_back(wr);

	m.organics.push_back(organic(0, 0, 0, /*team=*/1, /*wp_id=*/1));
	m.organics.push_back(organic(50 << 16, 0, 0, /*team=*/2, /*wp_id=*/1));

	opennova::bms::Entity bldg{};
	bldg.type = opennova::bms::ItemType::Building;
	bldg.x = 999 << 16;
	m.buildings.push_back(bldg);

	// Authored SSNs (promotion copies record ids verbatim, like the original).
	m.organics[0].id = 1;
	m.organics[1].id = 2;
	m.buildings[0].id = 3;
	m.markers[0].id = 10;
	m.markers[1].id = 11;
	m.markers[2].id = 12;
	return m;
}

} // namespace

NovaSimulation::NovaSimulation() {
	reset_world();
	set_process(true);
}

void NovaSimulation::reset_world() {
	invalidate_present_effect_pose_cache();
	pending_weapon_events_.clear();
	weapon_anim_tick_ = 0;
	local_round_sequence_ = 0;
	weapon_active_ = false;
	weapon_fire_held_ = false;
	weapon_fire_pressed_ = false;
	weapon_reload_pressed_ = false;
	world_ = std::make_unique<World>();
	ai_ = std::make_unique<AiSystem>();
	bms_ = std::make_unique<opennova::mission::BmsEventSystem>();
	wac_ = std::make_unique<opennova::wac::WacSystem>();
	promo_ = opennova::mission::PromoteResult{};
	loaded_ = false;
	playing_ = false;
	have_baseline_ = false;
	last_sim_tick_us_ = 0;
	last_net_tick_us_ = 0;
	last_present_snapshot_us_ = 0;
	last_present_entity_count_ = 0;
	// Collision models/instances are mission-scoped: drop them with the world (the
	// sweep re-registers on the next load) and re-point the fresh ai_ at the container.
	collision_world_ = opennova::world::CollisionWorld{};
	// Occlusion models too — retail reloads the model cache per mission, so the
	// weld pass's shared-record type-5 rewrites never leak across loads.
	occlusion_world_ = opennova::world::OcclusionWorld{};
	occlusion_culled_bms_.clear();
	apply_terrain_to_ai(); // re-point the fresh ai_ at the persisted terrain field (if any)
	apply_root_motion_to_ai(); // ...and at the persisted infantry clip set (if any)
	apply_collision_to_ai();
}

// Re-point the (possibly just-rebuilt) AI system at our owned terrain field. The field's raw
// pointers reference terrain_heightmap_/terrain_sector_grid_, which persist across reset_world.
void NovaSimulation::apply_terrain_to_ai() {
	// The round sim's ground stop shares the same field (world.terrain; §5.60).
	if (world_) world_->terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	if (world_) {
		// The footstep surface pick reads the charmap through this view; the
		// zero-initialized map is the sampler's "no charmap -> surface 1" leg.
		world_->surface_map =
			surface_indices_.empty() ? opennova::terrain::SurfaceTypeMap{} : surface_map_;
	}
	apply_sound_state_to_world();
	if (!ai_) return;
	ai_->terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	ai_->ground_clearance = opennova::world::GroundClearance{};
	// The collision ground probe shares the same field.
	collision_world_.terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
}

// (Re)apply the persisted sound-profile chain state to the current world: the parsed
// SndProf.def table and the water plane. Runs from apply_terrain_to_ai
// (reset_world / load) and from the setters when live. (The mission attrib
// dword the scream's night gate reads is stamped by finish_load from the BMS
// header — not re-applied here.)
void NovaSimulation::apply_sound_state_to_world() {
	if (!world_) return;
	world_->sound_profiles.clear();
	if (!sndprof_text_.empty())
		world_->sound_profiles.parse(reinterpret_cast<const char *>(sndprof_text_.data()),
		                             sndprof_text_.size());
	world_->env.water_z = env_water_z_q16_;
}

// Re-point the (possibly just-rebuilt) AI system at the owned infantry root-motion source.
// A source with no clips counts as none: the selector then resolves every state to "no
// clip" and soldiers stand, exactly the original's relationship between motion and clips.
void NovaSimulation::apply_root_motion_to_ai() {
	if (!ai_) return;
	ai_->root_motion = !infantry_anim_.empty() ? &infantry_anim_ : nullptr;
}

int NovaSimulation::set_infantry_anim_map(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name) {
	// The default clip set (adm_id 0): every infantry entity grounds off this until its own
	// model's .adm is registered (register_infantry_adm + set_infantry_adm_id). Clearing here
	// resets the whole registry on each (re)load.
	infantry_anim_.clear();
	infantry_anim_.register_adm(p_resource_root, p_adm_name);
	apply_root_motion_to_ai();
	return infantry_anim_.clip_count(0);
}

// Per-entity .adm resolution: ground each soldier off its OWN model's clip, not the shared
// default set (adm_id 0). For every active infantry entity, resolve its anim_def from its
// items.def type id, register that .adm (parsed once, cached by name), and store the resulting
// adm_id on its InfantryState. The local player (US01) is covered the same way once spawned.
// Idempotent: re-registering a name returns the cached id, re-setting adm_id is harmless, so the
// host can call this after load and again after spawning the player. [orig: AnimMap_UpdateEntity
// @0x40b5f0 evaluates the entity's own anim map per frame; docs/world/world-wac-ai-re.md D-INF-6.]
void NovaSimulation::resolve_infantry_adm_ids(const Ref<NovaResourceRoot> &p_resource_root,
                                              const Ref<NovaItemDatabase> &p_item_db) {
	if (!world_ || !world_->ai || p_resource_root.is_null() || p_item_db.is_null()) return;
	AiSystem &ai = *world_->ai;
	for (int i = 0; i < ai.count(); ++i) {
		AiEntity *e = ai.at(i);
		if (!e || !e->inf.active) continue;
		const opennova::world::Entity *ent = world_->registry.get(e->handle);
		if (!ent) continue;
		const int visual_item_id = visual_item_id_for_runtime_type(ent->item_id, p_item_db);
		String adm = p_item_db->get_anim_def(visual_item_id);
		if (adm.is_empty()) continue;
		if (!adm.to_lower().ends_with(".adm")) adm += ".adm";
		const int adm_id = infantry_anim_.register_adm(p_resource_root, adm);
		if (adm_id >= 0) e->inf.adm_id = adm_id;
	}
	apply_root_motion_to_ai();
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
	// Cache the Player template's items.def hp at world level so LATE-JOINER spawns (which happen
	// after this sweep) seed full health without an item-db reach-back from libs/ [orig:
	// Entity_InitFromItemDef @0x49e550 — spawn Health = itemDef->healthMax]. (D-NET-144)
	world_->player_item_hp = p_item_db->get_hp(
			static_cast<int>(opennova::world::kPlayerInfantryTypeId) +
			opennova::mission::kItemIdOffset);
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each([&](const opennova::world::Entity &e) { handles.push_back(e.handle); });
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (!e) continue;
		const int def_id = static_cast<int>(e->item_id) + opennova::mission::kItemIdOffset;
		e->is_ai_capable = p_item_db->is_ai_capable(def_id);
		// §5.10b replication class from the *_function tag (ai_function, else move_function).
		const String ai_fn = p_item_db->get_ai_function(def_id);
		const String tag = ai_fn.is_empty() ? p_item_db->get_move_function(def_id) : ai_fn;
		e->net_class_code =
				static_cast<uint8_t>(opennova::class_from_tag(tag.utf8().get_data()));
		// items.def hp -> healthMax; lift spawn-default health to full [orig: @0x49e550].
		const int hp = p_item_db->get_hp(def_id);
		if (hp > 0) {
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
		e->is_capture_trigger = (attrib & 0x20000u) != 0;
		e->is_spawn_point = (attrib & 0x40000u) != 0;
		// Death-presentation traits: LeaveCorpse (attrib 0x400000) keeps the corpse
		// forever; deathtime (def+0x890, parse-scaled ticks) seeds the corpse timer at
		// the death edge. [orig: ItemDef_ParseProperty @0x4a09d3 / @0x49fa6c; consumers
		// Entity_UpdateInfantryAI @0x4b9e54 / @0x4b9c97; world-wac-ai-re §19]
		e->leave_corpse = (attrib & 0x400000u) != 0;
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
				t.team_protect = (attrib & 0x8000u) != 0;
				t.no_die = (attrib & 0x40000000u) != 0;
				t.has_husk = bool(dt.get("has_husk", false));
				t.is_decoration =
						p_item_db->get_item_type(def_id) == NovaItemDatabase::TYPE_DECORATION;
				t.husk_sub_part_count =
						static_cast<uint8_t>(std::clamp(int(dt.get("husk_sub_parts", 0)), 0, 255));
				const PackedInt32Array types = dt.get("husk_sub_part_types", PackedInt32Array());
				for (int s = 0; s < 16 && s < types.size(); ++s)
					t.husk_sub_part_types[s] = static_cast<uint8_t>(types[s]);
				t.debris_scale = float(double(dt.get("debris_scale", 0.0)));
				t.sound_death = String(dt.get("sounddeath", String())).utf8().get_data();
				const Dictionary fx = p_item_db->get_particle_effects(def_id);
				t.particledeath = String(fx.get("particledeath", String())).utf8().get_data();
				t.particleh2odeath =
						String(fx.get("particleh2odeath", String())).utf8().get_data();
				t.particlefire = String(fx.get("particlefire", String())).utf8().get_data();
				t.particleother = String(fx.get("particleother", String())).utf8().get_data();
				// kz_points: the husk-model KZ user-point multi-blast is a tracked
				// refinement (§24) — the fallback single blast at the entity with
				// r = kz ?: boundRadius carries the witnessed gameplay.
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
				vt.player_control = (attrib & 0x40u) != 0;
				world_->vehicle_traits.set(e->item_id, vt);
			}
		}
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

namespace {

// Build the runtime collision model from a parsed .3di collision IR block — the exact
// inverse of the parse scaling (BPLN normals int16 Q14 / 16384, distances + AABBs 16.16;
// libs/threedi/src/threedi_3di3.cpp parse_bpln/parse_bvol). Sections mirror the COBJ
// grouping (volumes are sequential per object in the IR conversion). Returns false when
// the model carries no volumes.
bool collision_model_from_ir(const ThreediIRCollision *col,
	                             opennova::world::CollisionModel &out) {
	if (col == nullptr || col->volume_count == 0 ||
	    !threedi_ir_collision_is_runtime_safe(col))
		return false;
	auto fx = [](float v) { return static_cast<int32_t>(std::lround(v * 65536.0)); };

	out.planes.reserve(col->plane_count);
	for (size_t i = 0; i < col->plane_count; ++i) {
		const ThreediIRCollisionPlane &sp = col->planes[i];
		opennova::world::CollisionPlane p;
		p.nx = static_cast<int16_t>(std::lround(sp.normal[0] * 16384.0f));
		p.ny = static_cast<int16_t>(std::lround(sp.normal[1] * 16384.0f));
		p.nz = static_cast<int16_t>(std::lround(sp.normal[2] * 16384.0f));
		p.dist = fx(sp.distance);
		out.planes.push_back(p);
	}

	out.volumes.reserve(col->volume_count);
	int32_t max_object = 0;
	for (size_t i = 0; i < col->volume_count; ++i) {
		const ThreediIRCollisionVolume &sv = col->volumes[i];
		opennova::world::CollisionVolume v;
		v.type = sv.type;
		v.flags = static_cast<uint32_t>(sv.flags);
		v.min_x = fx(sv.min[0]);
		v.max_x = fx(sv.max[0]);
		v.min_y = fx(sv.min[1]);
		v.max_y = fx(sv.max[1]);
		v.min_z = fx(sv.min[2]);
		v.max_z = fx(sv.max[2]);
		v.plane_start = sv.plane_start;
		v.plane_count = sv.plane_count;
		out.volumes.push_back(v);
		if (sv.object_index > max_object) max_object = sv.object_index;
	}

	// One section per collision object; object_index -1 (no COBJ) folds into
	// section 0. Objects carrying only a face mesh (no volumes) still get a
	// section so the round raycast can walk their faces.
	const int32_t section_count =
			std::max<int32_t>(max_object + 1, static_cast<int32_t>(col->object_count));
	out.sections.assign(static_cast<size_t>(section_count), {});
	// Volumes are contiguous per object; derive the runs.
	int32_t cursor = 0;
	for (int32_t s = 0; s < section_count; ++s) {
		opennova::world::CollisionSection &sec = out.sections[s];
		sec.volume_start = cursor;
		sec.volume_count = 0;
		while (cursor < static_cast<int32_t>(col->volume_count)) {
			const int32_t oi = col->volumes[cursor].object_index;
			if ((oi < 0 ? 0 : oi) != s) break;
			++sec.volume_count;
			++cursor;
		}
		if (s < static_cast<int32_t>(col->object_count))
			sec.part_index = col->objects[s].parent_subobject_index;
	}

	// The face mesh (the bullet LOD): Q8 int16 vertices + the 44-B-equivalent
	// face records, in per-object runs [orig: the runtime CVRT/CNRM/CFAC arrays
	// hung off each COBJ by the collision builder @ 0x5b3bf0; the projectile
	// raycast Physics_RaycastAgainstBoneCollision @ 0x4e4cb0 walks them].
	if (col->face_count > 0 && col->faces != nullptr && col->vertex_count > 0 &&
	    col->vertices != nullptr && col->object_count > 0 && col->objects != nullptr) {
		out.face_vertices.reserve(col->vertex_count);
		for (size_t i = 0; i < col->vertex_count; ++i) {
			opennova::world::CollisionFaceVertex v;
			// The parse divides the disk Q8 int16 by 256 — requantize losslessly.
			v.x = static_cast<int16_t>(std::lround(col->vertices[i].position[0] * 256.0f));
			v.y = static_cast<int16_t>(std::lround(col->vertices[i].position[1] * 256.0f));
			v.z = static_cast<int16_t>(std::lround(col->vertices[i].position[2] * 256.0f));
			out.face_vertices.push_back(v);
		}
		out.faces.reserve(col->face_count);
		for (size_t i = 0; i < col->face_count; ++i) {
			const ThreediIRCollisionFace &sf = col->faces[i];
			opennova::world::CollisionFace f;
			f.v[0] = sf.vert_index[0];
			f.v[1] = sf.vert_index[1];
			f.v[2] = sf.vert_index[2];
			f.normal[0] = sf.normal[0];
			f.normal[1] = sf.normal[1];
			f.normal[2] = sf.normal[2];
			f.axis = sf.dominate_axis;
			f.plane_dist = sf.plane_dist_fp16;
			for (int a = 0; a < 3; ++a) {
				f.min[a] = sf.min_fp16[a];
				f.max[a] = sf.max_fp16[a];
			}
			f.flags = sf.material_flags;
			f.material = sf.poly_type;
			out.faces.push_back(f);
		}
		// Per-object runs; clamp malformed indices to never-hit rather than
		// letting the walk read out of range.
		int32_t vcur = 0, fcur = 0;
		for (int32_t s = 0; s < static_cast<int32_t>(col->object_count); ++s) {
			opennova::world::CollisionSection &sec = out.sections[s];
			const ThreediIRCollisionObject &obj = col->objects[s];
			sec.face_vertex_start = vcur;
			sec.face_vertex_count = obj.num_vertices;
			sec.face_start = fcur;
			sec.face_count = obj.num_faces;
			for (int32_t fi = 0; fi < sec.face_count &&
					fcur + fi < static_cast<int32_t>(out.faces.size()); ++fi) {
				opennova::world::CollisionFace &f = out.faces[fcur + fi];
				if (f.v[0] < 0 || f.v[0] >= obj.num_vertices || f.v[1] < 0 ||
				    f.v[1] >= obj.num_vertices || f.v[2] < 0 || f.v[2] >= obj.num_vertices)
					f.flags |= 0x100u;
			}
			vcur += obj.num_vertices;
			fcur += obj.num_faces;
			if (vcur > static_cast<int32_t>(out.face_vertices.size()) ||
			    fcur > static_cast<int32_t>(out.faces.size())) {
				// Malformed runs — drop the whole face mesh rather than serve
				// a scrambled walk.
				out.face_vertices.clear();
				out.faces.clear();
				for (auto &sc : out.sections) {
					sc.face_start = sc.face_count = 0;
					sc.face_vertex_start = sc.face_vertex_count = 0;
				}
				break;
			}
		}
	}
	return true;
}

// The model bound-sphere radius from the .3di itself — the union of the LOD-0
// part bounding spheres seen from the model origin, with the primitive boxes as
// the degenerate-sphere fallback. This is the entity+0 boundRadius source: the
// original reads it off the MODEL header (gpm[5]) for every placed item,
// collision block or not, and the proximity/hit tests and blast ranges all
// consume it [orig: Entity_InitFromModel @ 0x40dc30; world-wac-ai-re §24].
float model_bound_radius_from_ir(const ThreediModelIR &ir) {
	if (ir.lod_count == 0 || ir.lods == nullptr) return 0.0f;
	const ThreediIRLod &lod = ir.lods[0];
	float r = 0.0f;
	for (size_t i = 0; lod.parts != nullptr && i < lod.part_count; ++i) {
		const ThreediIRPart &p = lod.parts[i];
		const float cx = p.abs_position[0] + p.bounding_center[0];
		const float cy = p.abs_position[1] + p.bounding_center[1];
		const float cz = p.abs_position[2] + p.bounding_center[2];
		const float c = std::sqrt(cx * cx + cy * cy + cz * cz);
		if (c + p.bounding_radius > r) r = c + p.bounding_radius;
	}
	if (r <= 0.0f) {
		for (size_t i = 0; lod.primitives != nullptr && i < lod.primitive_count; ++i) {
			const ThreediIRPrimitive &pr = lod.primitives[i];
			for (int a = 0; a < 3; ++a) {
				r = std::max(r, std::abs(pr.min[a]));
				r = std::max(r, std::abs(pr.max[a]));
			}
		}
	}
	return r;
}

// Build the runtime occlusion model from the parsed occlusion IR — the 60 B
// portal-face records with their sequential slices (the IR conversion already
// mirrors the arena assignment of [orig: load_occlusion_model_data @ 0x5b4a00]).
// The IR face dwords decode as the 12 B OFAC record: bytes 0-2 = vertex
// indices, byte 3 = plane index, then the 3 edge words (bit 15 = winding).
bool occlusion_model_from_ir(const ThreediIROcclusion *occ,
                             opennova::world::OcclusionModel &out) {
	if (occ == nullptr || occ->object_count == 0) return false;
	out.vertices.reserve(occ->vertex_count);
	for (size_t i = 0; i < occ->vertex_count; ++i) {
		opennova::world::OcclusionVertex v;
		v.p[0] = occ->vertices[i].position[0];
		v.p[1] = occ->vertices[i].position[1];
		v.p[2] = occ->vertices[i].position[2];
		out.vertices.push_back(v);
	}
	out.planes.reserve(occ->plane_count);
	for (size_t i = 0; i < occ->plane_count; ++i) {
		opennova::world::OcclusionPlane p;
		p.normal[0] = occ->planes[i].normal[0];
		p.normal[1] = occ->planes[i].normal[1];
		p.normal[2] = occ->planes[i].normal[2];
		p.d = occ->planes[i].radius;
		out.planes.push_back(p);
	}
	out.faces.reserve(occ->face_count);
	for (size_t i = 0; i < occ->face_count; ++i) {
		const ThreediIROcclusionFace &sf = occ->faces[i];
		opennova::world::OcclusionFaceRec f;
		f.v[0] = static_cast<uint8_t>(sf.raw_indices & 0xFF);
		f.v[1] = static_cast<uint8_t>((sf.raw_indices >> 8) & 0xFF);
		f.v[2] = static_cast<uint8_t>((sf.raw_indices >> 16) & 0xFF);
		f.plane = static_cast<uint8_t>((sf.raw_indices >> 24) & 0xFF);
		f.edge[0] = static_cast<uint16_t>(sf.edge_data & 0xFFFF);
		f.edge[1] = static_cast<uint16_t>(sf.edge_data >> 16);
		f.edge[2] = static_cast<uint16_t>(sf.other_edge_data & 0xFFFF);
		out.faces.push_back(f);
	}
	out.records.reserve(occ->object_count);
	for (size_t i = 0; i < occ->object_count; ++i) {
		const ThreediIROcclusionObject &so = occ->objects[i];
		opennova::world::OcclusionPortalFace rec;
		rec.type = static_cast<uint8_t>(so.type);
		rec.section_a = static_cast<uint8_t>(so.parent_subobject_index);
		rec.section_b = static_cast<uint8_t>(so.connecting_subobject);
		rec.pos[0] = so.position[0];
		rec.pos[1] = so.position[1];
		rec.pos[2] = so.position[2];
		rec.radius = so.radius;
		rec.vert_start = so.vertex_start;
		rec.vert_count = so.num_vertices;
		rec.plane_start = so.plane_start;
		rec.plane_count = so.num_planes;
		rec.face_start = so.face_start;
		rec.face_count = so.face_count;
		rec.glow_scale = so.glow_scale;
		out.records.push_back(rec);
	}
	// Slice sanity: reject models whose records point past their arrays, and
	// whose OFAC bytes index outside their record's slice — the engine's hot
	// loops (traverse/build_occluder_planes) read face vertex/plane/edge
	// indices unchecked, so malformed or modded data is rejected here once.
	for (const opennova::world::OcclusionPortalFace &rec : out.records) {
		if (rec.vert_start < 0 || rec.vert_count < 0 ||
		    rec.vert_start + rec.vert_count > static_cast<int32_t>(out.vertices.size()) ||
		    rec.plane_start < 0 || rec.plane_count < 0 ||
		    rec.plane_start + rec.plane_count > static_cast<int32_t>(out.planes.size()) ||
		    rec.face_start < 0 || rec.face_count < 0 ||
		    rec.face_start + rec.face_count > static_cast<int32_t>(out.faces.size()))
			return false;
		for (int32_t f = 0; f < rec.face_count; ++f) {
			const opennova::world::OcclusionFaceRec &face = out.faces[rec.face_start + f];
			if (face.v[0] >= rec.vert_count || face.v[1] >= rec.vert_count ||
			    face.v[2] >= rec.vert_count || face.plane >= rec.plane_count)
				return false;
			for (int k = 0; k < 3; ++k) {
				if ((face.edge[k] & 0xFF) >= rec.vert_count ||
				    ((face.edge[k] >> 8) & 0x7F) >= rec.vert_count)
					return false;
			}
		}
	}
	return true;
}

} // namespace

void NovaSimulation::apply_collision_to_ai() {
	collision_world_.terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	if (ai_) ai_->collision = &collision_world_;
}

int NovaSimulation::resolve_collision_instances(const Ref<NovaItemDatabase> &p_item_db,
                                                Object *p_placer) {
	if (!world_ || p_item_db.is_null() || p_placer == nullptr) return 0;
	apply_collision_to_ai();
	std::unordered_map<std::string, int32_t> model_by_graphic;     // -1 = no collision block
	std::unordered_map<std::string, int32_t> occlusion_by_graphic; // -1 = no occlusion records
	std::unordered_map<std::string, float> radius_by_graphic;      // .3di bound radius (units)
	std::unordered_map<std::string, int32_t> husk_sections_by_graphic; // husk section counts
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each(
			[&](const opennova::world::Entity &e) { handles.push_back(e.handle); });
	int attached = 0;
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (!e || e->kind == opennova::world::EntityKind::Organic ||
		    e->kind == opennova::world::EntityKind::Marker)
			continue;
		const int def_id = static_cast<int>(e->item_id) + opennova::mission::kItemIdOffset;
		const String graphic = p_item_db->get_graphic(def_id);
		if (graphic.is_empty()) continue;
		const std::string key(graphic.utf8().get_data());
		auto it = model_by_graphic.find(key);
		if (it == model_by_graphic.end()) {
			int32_t model_id = -1;
			int32_t occlusion_id = -1;
			float bound_radius = 0.0f;
			// Duck-typed MissionObjectPlacer.object_data_for(graphic) — the placer's
			// per-graphic NovaObjectData cache (the render path loads the same object).
			Ref<NovaObjectData> data = p_placer->call("object_data_for", graphic);
			if (data.is_valid()) {
				opennova::world::CollisionModel model;
				if (collision_model_from_ir(data->native_ir().collision, model))
					model_id = collision_world_.add_model(std::move(model));
				opennova::world::OcclusionModel occ;
				if (occlusion_model_from_ir(data->native_ir().occlusion, occ))
					occlusion_id = occlusion_world_.add_model(std::move(occ));
				bound_radius = model_bound_radius_from_ir(data->native_ir());
			}
			it = model_by_graphic.emplace(key, model_id).first;
			occlusion_by_graphic.emplace(key, occlusion_id);
			radius_by_graphic.emplace(key, bound_radius);
		}
		// The bound-sphere radius (entity+0 boundRadius) comes from the .3di
		// MODEL header bound, not the collision block — every placed item
		// carries one, so collision-less props are still hittable by rounds and
		// reachable by blasts. Raised to the husk model's bound below, then
		// padded +0.0625 [orig: Entity_InitFromModel @ 0x40dc30 — boundRadius =
		// max(gpm[5], husk gpm[5]) + 0x1000; the authored def scale factor is
		// not yet applied (tracked, D-COL-3)].
		float entity_bound = radius_by_graphic[key];
		if (it->second >= 0) {
			collision_world_.assign_entity(h, it->second);
			++attached;
		}
		// The husk-stage collision model: attached beside the graphic instance so
		// every query swaps to the wreck once Flags & 4 sets. The collision pick
		// is the FIRST husk stage (entity+52 huskModel), not huskFinal [orig: the
		// +52 substitution @ 0x538720 / @ 0x413086; D-AI-7 residual closed].
		const String husk_name_s = p_item_db->get_husk(def_id).is_empty()
				? p_item_db->get_huskfinal(def_id)
				: p_item_db->get_husk(def_id);
		if (!husk_name_s.is_empty()) {
			const std::string husk_key(husk_name_s.utf8().get_data());
			auto hit = model_by_graphic.find(husk_key);
			if (hit == model_by_graphic.end()) {
				int32_t husk_model_id = -1;
				Ref<NovaObjectData> hdata = p_placer->call("object_data_for", husk_name_s);
				if (hdata.is_valid()) {
					opennova::world::CollisionModel hmodel;
					if (collision_model_from_ir(hdata->native_ir().collision, hmodel))
						husk_model_id = collision_world_.add_model(std::move(hmodel));
				}
				hit = model_by_graphic.emplace(husk_key, husk_model_id).first;
				occlusion_by_graphic.emplace(husk_key, -1);
			}
			if (hit->second >= 0 && it->second >= 0)
				collision_world_.assign_entity_husk(h, hit->second);
			// The husk RENDER model's section count = the death-piece loop
			// bound [orig: renderObj[8]+52 @ 0x49361a] — most defs author no
			// husk_sub_parts token. Its own cache, independent of the collision
			// cache: a husk graphic can double as some entity's main graphic,
			// which would leave the joint cache without a sections entry.
			auto hs = husk_sections_by_graphic.find(husk_key);
			if (hs == husk_sections_by_graphic.end()) {
				int32_t husk_sections = 0;
				Ref<NovaObjectData> hdata = p_placer->call("object_data_for", husk_name_s);
				if (hdata.is_valid() && hdata->native_ir().lod_count > 0 &&
				    hdata->native_ir().lods != nullptr)
					husk_sections = static_cast<int32_t>(
							hdata->native_ir().lods[0].part_count);
				hs = husk_sections_by_graphic.emplace(husk_key, husk_sections).first;
				if (hdata.is_valid())
					radius_by_graphic.emplace(husk_key,
							model_bound_radius_from_ir(hdata->native_ir()));
			}
			if (opennova::world::ItemDeathTraits *t =
						world_->item_death_traits.get_mutable(e->item_id)) {
				if (t->husk_section_count == 0 && hs->second > 0)
					t->husk_section_count = hs->second;
			}
			// The husk model's bound joins the entity bound max [orig:
			// Entity_InitFromModel @ 0x40dc30, the huskModel[5] compare].
			entity_bound = std::max(entity_bound, radius_by_graphic[husk_key]);
		}
		if (entity_bound > 0.0f && e->bound_radius <= 0.0f)
			e->bound_radius = entity_bound + 0.0625f;  // the +0x1000 16.16 pad
		const int32_t occ_id = occlusion_by_graphic[key];
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

void NovaSimulation::occlusion_init_mission() {
	// [orig: Terrain_InitBuildingPortals @ 0x5c7480 from Game_StartMission
	// @ 0x525e11 — runs over the static prox tables, so make sure they exist
	// before the register pass walks the building prefix.]
	if (!world_) return;
	collision_world_.build_tick_tables(*world_);
	occlusion_world_.init_mission(*world_, collision_world_);
}

void NovaSimulation::run_occlusion_frame(const Transform3D &p_camera, double p_fov_y_deg,
                                         double p_aspect, double p_near,
                                         double p_fog_dist_units, double p_water_z_units,
                                         bool p_force_indoors) {
	if (!world_) return;
	using opennova::world::to_fixed;
	opennova::world::OcclusionFrameCamera cam;

	// Godot world (x, up, z) -> mission fixed (x, -z, up) 16.16.
	const Vector3 gp = p_camera.origin;
	cam.pos_fixed[0] = to_fixed(gp.x);
	cam.pos_fixed[1] = to_fixed(-gp.z);
	cam.pos_fixed[2] = to_fixed(gp.y);
	opennova::world::render_float_from_fixed(cam.pos_fixed, cam.pos_float);

	// Camera axes. Godot camera looks -Z; render float = Godot with X/Z swapped
	// ((-my, mz, mx)/65536 == (gz, gy, gx)); mission dirs = (x, -z, y) of Godot.
	const Vector3 fwd_g = -p_camera.basis.get_column(2).normalized();
	const Vector3 right_g = p_camera.basis.get_column(0).normalized();
	const Vector3 up_g = p_camera.basis.get_column(1).normalized();
	auto render_dir = [](const Vector3 &v) {
		return Vector3(v.z, v.y, v.x);
	};
	const Vector3 f = render_dir(fwd_g);
	const Vector3 r = render_dir(right_g);
	const Vector3 u = render_dir(up_g);
	const Vector3 c(cam.pos_float[0], cam.pos_float[1], cam.pos_float[2]);

	// The 5-plane view frustum (near + 4 sides), inward normals, in render
	// float space — the host stand-in for the retail viewport projector
	// [orig: g_CameraFrustumPlanes5 @ 0xA7849C; D-OCC-12].
	const double half_v = Math::deg_to_rad(p_fov_y_deg) * 0.5;
	const double tan_v = std::tan(half_v);
	const double tan_h = tan_v * (p_aspect > 0.0 ? p_aspect : 1.0);
	Vector3 normals[5];
	normals[0] = f;
	normals[1] = (f * static_cast<real_t>(tan_h) + r).normalized();  // left
	normals[2] = (f * static_cast<real_t>(tan_h) - r).normalized();  // right
	normals[3] = (f * static_cast<real_t>(tan_v) + u).normalized();  // bottom
	normals[4] = (f * static_cast<real_t>(tan_v) - u).normalized();  // top
	cam.frustum_count = 5;
	for (int i = 0; i < 5; ++i) {
		const Vector3 anchor = (i == 0) ? c + f * static_cast<real_t>(p_near) : c;
		cam.frustum[i][0] = normals[i].x;
		cam.frustum[i][1] = normals[i].y;
		cam.frustum[i][2] = normals[i].z;
		cam.frustum[i][3] = -normals[i].dot(anchor);
	}

	// World->view rotation rows (mission axes, Q22): row 0 = forward (the depth
	// cull axis), rows 1/2 = the lateral axes the three-ray probe offsets along.
	// [orig: the fixed view matrix @ 0xA7841C]
	auto mission_dir_q22 = [](const Vector3 &v, int32_t out[3]) {
		out[0] = static_cast<int32_t>(std::lround(v.x * 4194304.0));
		out[1] = static_cast<int32_t>(std::lround(-v.z * 4194304.0));
		out[2] = static_cast<int32_t>(std::lround(v.y * 4194304.0));
	};
	mission_dir_q22(fwd_g, cam.view_rows_q22[0]);
	mission_dir_q22(right_g, cam.view_rows_q22[1]);
	mission_dir_q22(up_g, cam.view_rows_q22[2]);

	cam.fog_dist = to_fixed(p_fog_dist_units);
	cam.water_z = to_fixed(p_water_z_units);
	// The mission-attribute force-indoors override ORs the indoors bit into the
	// frame's accum view. [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8 -> |= 2]
	cam.local_blink_flags =
			collision_world_.local_player_blink_flags | (p_force_indoors ? 0x2u : 0u);

	occlusion_world_.build_frame(*world_, collision_world_, cam);

	// The entity collectors' render gates over the non-building entities the
	// host draws. [orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20 /
	// collect_visible_entities_for_terrain @ 0x5c8c60]
	occlusion_culled_bms_.clear();
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind == opennova::world::EntityKind::Building ||
		    e.kind == opennova::world::EntityKind::Marker)
			return;
		if (e.bms_id == 0) return; // wire avatars ride their own present path
		handles.push_back(e.handle);
	});
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (e == nullptr) continue;
		if (!occlusion_world_.entity_render_visible(*world_, collision_world_, *e, cam))
			occlusion_culled_bms_.push_back(e->bms_id);
	}
}

PackedInt64Array NovaSimulation::get_building_visibility() const {
	PackedInt64Array out;
	if (!world_) return out;
	// Pairs [bms_id, visible<<32 | mask] for every building with an OCCLUSION
	// instance, plus collision-backed de-batched buildings that still entered
	// the retail building batch. OOBJ instances apply their section mask.
	// Without OOBJ there is no safe host part-to-section map, so those buildings
	// keep all render parts while still receiving batch/frustum/TOC visibility.
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building || e.bms_id == 0) return;
		const bool has_occlusion = occlusion_world_.has_instance(e.handle);
		if (!has_occlusion && collision_world_.model_for(e.handle) == nullptr) return;
		const bool visible = occlusion_world_.building_visible(e.handle);
		const uint32_t mask =
		    has_occlusion ? occlusion_world_.section_mask(e.handle) : 0xFFFFFFFFu;
		out.push_back(e.bms_id);
		out.push_back(static_cast<int64_t>(mask) | (visible ? (int64_t(1) << 32) : 0));
	});
	return out;
}

PackedInt32Array NovaSimulation::get_render_culled_bms_ids() const {
	PackedInt32Array out;
	for (const int32_t id : occlusion_culled_bms_) out.push_back(id);
	return out;
}

bool NovaSimulation::occlusion_water_visible() const {
	return occlusion_world_.water_visible();
}

bool NovaSimulation::occlusion_camera_indoors() const {
	return occlusion_world_.camera_indoors();
}

bool NovaSimulation::local_player_indoors() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e != nullptr && (e->flags & opennova::world::kEntityFlagIndoors) != 0;
}

int NovaSimulation::local_player_blink_flags() const {
	return static_cast<int>(collision_world_.local_player_blink_flags);
}

int64_t NovaSimulation::sound_occlusion_distance_q16(const Vector3 &listener_pos,
                                                     const Vector3 &source_pos,
                                                     int64_t distance_q16,
                                                     int source_bms_id) {
	// [orig: Sound_ApplyOcclusionDistance @ 0x529970] — the audio layer feeds
	// the AUDIO listener (camera), emitter/one-shot position, and source
	// identity when known. -1 denotes the local player, positive values are
	// authored BMS ids, and zero keeps the no-entity path. Godot world
	// (x, up, z) -> mission fixed (x, -z, up) 16.16.
	if (!world_) return distance_q16;
	const int32_t lp[3] = {opennova::world::to_fixed(listener_pos.x),
	                       opennova::world::to_fixed(-listener_pos.z),
	                       opennova::world::to_fixed(listener_pos.y)};
	const int32_t sp[3] = {opennova::world::to_fixed(source_pos.x),
	                       opennova::world::to_fixed(-source_pos.z),
	                       opennova::world::to_fixed(source_pos.y)};
	opennova::world::EntityHandle source;
	if (source_bms_id < 0) {
		source = world_->cached.local_player;
	} else if (source_bms_id > 0) {
		world_->registry.for_each([&](const opennova::world::Entity &e) {
			if (!source.valid() && e.bms_id == source_bms_id) source = e.handle;
		});
	}
	// Static/env emitters do not ride the moving-entity collision resolver.
	// Refresh their blink/indoors state at the audio query boundary so the
	// both-indoors terrain bypass sees the source state retail registered.
	if (source.valid() && source != world_->cached.local_player) {
		if (opennova::world::Entity *source_entity = world_->registry.get(source))
			collision_world_.refresh_blink(*world_, *source_entity);
	}
	return collision_world_.sound_occlusion_inflate(*world_, world_->cached.local_player,
	                                                source, lp, sp,
	                                                static_cast<int32_t>(distance_q16));
}

namespace {
// Mission-space 16.16 triple -> Godot world space: (x, y, z) -> (x, z, -y) units.
inline Vector3 godot_from_fixed3(const int32_t p[3]) {
	return Vector3(static_cast<float>(p[0] / 65536.0), static_cast<float>(p[2] / 65536.0),
	               static_cast<float>(-p[1] / 65536.0));
}
} // namespace

Dictionary NovaSimulation::get_collision_debug() const {
	Dictionary out;
	Array instances;
	Dictionary player;
	player["valid"] = false;
	out["instances"] = instances;
	out["player"] = player;
	if (!world_) return out;

	// Anchor the sweep on the local player when one is spawned (150u box, the
	// resolver's own neighborhood scale); an editor preview with no player sweeps
	// the whole table up to the instance cap.
	int32_t anchor[3] = {0, 0, 0};
	int32_t range = -1;
	const opennova::world::Entity *lp =
	    world_->cached.local_player.valid() ? world_->registry.get(world_->cached.local_player)
	                                        : nullptr;
	if (lp != nullptr) {
		anchor[0] = opennova::world::to_fixed(lp->position.x);
		anchor[1] = opennova::world::to_fixed(lp->position.y);
		anchor[2] = opennova::world::to_fixed(lp->position.z);
		range = 150 << 16;
	}
	const std::vector<opennova::world::CollisionWorld::DebugInstance> insts =
	    collision_world_.debug_instances(*world_, anchor, range, 128);
	for (const opennova::world::CollisionWorld::DebugInstance &inst : insts) {
		Dictionary d;
		d["entity_handle"] = static_cast<int>(inst.handle.packed);
		d["pos"] = godot_from_fixed3(inst.pos);
		d["heading"] = static_cast<float>(
		    opennova::world::mission_yaw_deg_from_bam_heading(inst.heading_bam));
		Array vols;
		for (const opennova::world::CollisionWorld::DebugVolume &v : inst.volumes) {
			Dictionary vd;
			vd["type"] = v.type;
			vd["min_x"] = static_cast<float>(v.min[0] / kFixed16);
			vd["max_x"] = static_cast<float>(v.max[0] / kFixed16);
			vd["min_y"] = static_cast<float>(v.min[1] / kFixed16);
			vd["max_y"] = static_cast<float>(v.max[1] / kFixed16);
			vd["min_z"] = static_cast<float>(v.min[2] / kFixed16);
			vd["max_z"] = static_cast<float>(v.max[2] / kFixed16);
			PackedVector3Array corners;
			corners.resize(8);
			Vector3 *cw = corners.ptrw();
			for (int c = 0; c < 8; ++c) cw[c] = godot_from_fixed3(v.corners[c]);
			vd["corners"] = corners;
			vols.push_back(vd);
		}
		d["volumes"] = vols;
		instances.push_back(d);
	}

	// The local player's last full resolve: the capsule test points the resolver
	// queried and the returned foot clearance (CollisionWorld::LocalResolveDebug).
	const opennova::world::CollisionWorld::LocalResolveDebug &lrd =
	    collision_world_.local_resolve_debug;
	if (lrd.valid) {
		player["valid"] = true;
		player["position"] = godot_from_fixed3(lrd.pos);
		PackedVector3Array pts;
		pts.resize(3);
		Vector3 *pw = pts.ptrw();
		PackedFloat32Array radii;
		radii.resize(3);
		float *rw = radii.ptrw();
		for (int i = 0; i < 3; ++i) {
			pw[i] = godot_from_fixed3(lrd.points[i]);
			rw[i] = static_cast<float>(lrd.radii[i] / kFixed16);
		}
		player["points"] = pts;
		player["radii"] = radii;
		player["capsule_bottom"] = static_cast<float>(lrd.capsule_bottom / kFixed16);
		player["capsule_top"] = static_cast<float>(lrd.capsule_top / kFixed16);
		player["foot_clearance"] = static_cast<float>(lrd.foot_clearance / kFixed16);
	}
	return out;
}

namespace {
// Render float world -> Godot world: the render frame is Godot with X/Z
// swapped ((-my, mz, mx)/65536 == (gz, gy, gx)), so the inverse is the same swap.
inline Vector3 godot_from_render_float3(const float p[3]) {
	return Vector3(p[2], p[1], p[0]);
}
} // namespace

Dictionary NovaSimulation::get_occlusion_debug() const {
	Dictionary out;
	Array buildings;
	Array welds;
	Dictionary counts;
	out["active"] = false;
	out["camera_indoors"] = occlusion_world_.camera_indoors();
	out["exterior_visible"] = occlusion_world_.exterior_visible();
	out["water_visible"] = occlusion_world_.water_visible();
	out["local_blink_flags"] = static_cast<int>(collision_world_.local_player_blink_flags);
	out["counts"] = counts;
	out["buildings"] = buildings;
	out["welds"] = welds;
	if (!world_) return out;

	int instances = 0, batched = 0, visible = 0;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building) return;
		if (!occlusion_world_.has_instance(e.handle)) return;
		++instances;
		const bool is_batched = occlusion_world_.building_batched(e.handle);
		const bool is_visible = occlusion_world_.building_visible(e.handle);
		if (is_batched) ++batched;
		if (is_visible) ++visible;
		if (buildings.size() >= 256) return;
		Dictionary b;
		b["bms_id"] = e.bms_id;
		const int32_t pos_fixed[3] = {opennova::world::to_fixed(e.position.x),
		                              opennova::world::to_fixed(e.position.y),
		                              opennova::world::to_fixed(e.position.z)};
		b["pos"] = godot_from_fixed3(pos_fixed);
		b["batched"] = is_batched;
		b["visible"] = is_visible;
		b["open_flagged"] = occlusion_world_.building_open_flagged(e.handle);
		b["mask"] = static_cast<int64_t>(occlusion_world_.section_mask(e.handle));
		const opennova::world::OcclusionWorld::BuildingFlags flags =
		    occlusion_world_.building_flags(e.handle);
		b["has_open"] = flags.has_open;
		b["has_windows"] = flags.has_windows;
		b["has_links"] = flags.has_links;
		// Record-type census off the (possibly weld-retyped) shared model.
		int windows = 0, portals = 0, links = 0, records = 0;
		const opennova::world::OcclusionModel *m =
		    occlusion_world_.model(occlusion_world_.instance_model_id(e.handle));
		if (m != nullptr) {
			records = static_cast<int>(m->records.size());
			for (const opennova::world::OcclusionPortalFace &rec : m->records) {
				if (rec.type == opennova::world::kOccRecWindow)
					++windows;
				else if (rec.type == opennova::world::kOccRecPortal)
					++portals;
				else if (rec.type == opennova::world::kOccRecWeldedLink)
					++links;
			}
		}
		b["records"] = records;
		b["windows"] = windows;
		b["portals"] = portals;
		b["links"] = links;
		buildings.push_back(b);
	});

	for (const opennova::world::OcclusionWorld::WeldRecord &wr : occlusion_world_.weld_records()) {
		if (welds.size() >= 64) break;
		Dictionary w;
		const opennova::world::Entity *own = world_->registry.get(wr.own_entity);
		const opennova::world::Entity *other = world_->registry.get(wr.other_entity);
		w["own_bms"] = own != nullptr ? own->bms_id : 0;
		w["own_section"] = wr.own_section;
		w["other_bms"] = other != nullptr ? other->bms_id : 0;
		w["other_section"] = wr.other_section;
		welds.push_back(w);
	}

	counts["instances"] = instances;
	counts["batched"] = batched;
	counts["visible"] = visible;
	counts["toc_culled"] = batched - visible;
	counts["slots"] = occlusion_world_.slot_count();
	counts["window_groups"] = occlusion_world_.window_frustum_group_count();
	counts["viewthru_groups"] = occlusion_world_.viewthru_group_count();
	counts["welds"] = static_cast<int>(occlusion_world_.weld_records().size());
	counts["culled_entities"] = static_cast<int>(occlusion_culled_bms_.size());
	out["active"] = instances > 0;
	return out;
}

Dictionary NovaSimulation::get_occlusion_portal_debug(const Vector3 &p_anchor,
                                                      double p_range_units) const {
	Dictionary out;
	Array buildings;
	out["buildings"] = buildings;
	if (!world_) return out;
	// Godot world (x, up, z) -> mission fixed (x, -z, up) 16.16.
	const int64_t anchor_x = opennova::world::to_fixed(p_anchor.x);
	const int64_t anchor_y = opennova::world::to_fixed(-p_anchor.z);
	const int64_t range =
	    p_range_units > 0.0 ? static_cast<int64_t>(p_range_units * kFixed16) : -1;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building) return;
		if (buildings.size() >= 128) return;
		const opennova::world::OcclusionModel *m =
		    occlusion_world_.model(occlusion_world_.instance_model_id(e.handle));
		if (m == nullptr) return;
		const int32_t pos_fixed[3] = {opennova::world::to_fixed(e.position.x),
		                              opennova::world::to_fixed(e.position.y),
		                              opennova::world::to_fixed(e.position.z)};
		if (range >= 0 && (std::abs(pos_fixed[0] - anchor_x) > range ||
		                   std::abs(pos_fixed[1] - anchor_y) > range))
			return;
		// The same full authored building-pose path the engine's frame uses.
		const opennova::world::RenderMatrix mat =
		    opennova::world::render_matrix_from_entity_pose(e);
		Dictionary b;
		b["bms_id"] = e.bms_id;
		b["pos"] = godot_from_fixed3(pos_fixed);
		b["visible"] = occlusion_world_.building_visible(e.handle);
		Array records;
		for (const opennova::world::OcclusionPortalFace &rec : m->records) {
			Dictionary rd;
			rd["type"] = static_cast<int>(rec.type);
			rd["section_a"] = static_cast<int>(rec.section_a);
			rd["section_b"] = static_cast<int>(rec.section_b);
			float wp[3];
			mat.transform_point(rec.pos, wp);
			rd["pos"] = godot_from_render_float3(wp);
			rd["radius"] = rec.radius;
			rd["glow"] = rec.glow_scale;
			// The record's boundary outline: OFAC edge words whose low-15-bit
			// identity appears once (shared interior edges pair up and drop —
			// the same cancellation identity the occluder pass uses).
			std::vector<uint16_t> edges;
			std::vector<int32_t> hits;
			for (int32_t f = 0; f < rec.face_count; ++f) {
				const opennova::world::OcclusionFaceRec &face = m->faces[rec.face_start + f];
				for (int k = 0; k < 3; ++k) {
					const uint16_t w = face.edge[k];
					bool found = false;
					for (size_t x = 0; x < edges.size(); ++x) {
						if ((edges[x] & 0x7FFF) == (w & 0x7FFF)) {
							++hits[x];
							found = true;
							break;
						}
					}
					if (!found) {
						edges.push_back(w);
						hits.push_back(1);
					}
				}
			}
			PackedVector3Array segments;
			for (size_t x = 0; x < edges.size(); ++x) {
				if (hits[x] != 1) continue;
				const int32_t va = edges[x] & 0xFF;
				const int32_t vb = (edges[x] >> 8) & 0x7F;
				if (va >= rec.vert_count || vb >= rec.vert_count) continue;
				float aw[3], bw[3];
				mat.transform_point(m->vertices[rec.vert_start + va].p, aw);
				mat.transform_point(m->vertices[rec.vert_start + vb].p, bw);
				segments.push_back(godot_from_render_float3(aw));
				segments.push_back(godot_from_render_float3(bw));
			}
			rd["segments"] = segments;
			records.push_back(rd);
		}
		b["records"] = records;
		buildings.push_back(b);
	});
	return out;
}

bool NovaSimulation::local_player_in_armory_zone() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	// The use-item armory leg rejects a seated player before consulting the type-6
	// volume bit [orig: Input_HandleActionBinding_0 @0x4e0b3f, parentSlot == 0].
	return e != nullptr && !e->mounted &&
	       (e->flags & opennova::world::kEntityFlagArmoryZone) != 0;
}

bool NovaSimulation::local_player_in_vehicle_loadout_zone() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e != nullptr &&
	       (e->flags & opennova::world::kEntityFlagVehicleLoadoutZone) != 0;
}

bool NovaSimulation::local_player_toggle_mount() {
	// The USE-ITEM mount toggle for the local player — the shell calls this when the
	// armory/vehicle-zone legs of the key don't apply. [orig: Input_ProcessFrame release
	// edge @0x49d6dc -> Entity_ToggleVehicleMount @0x436950]
	if (!world_) return false;
	// Authority-only: the witnessed non-authority path queues C2S 0x26 (attach) /
	// sends 0x27 (detach) and waits for the 0x0A stream to confirm [orig:
	// Entity_RequestVehicleAttach @0x4364a0 / Entity_SendDetachPacket @0x435510].
	// That joiner wire leg is unported (D-AI-11 l) — applying locally on a joiner
	// would silently desync against the host, so the toggle refuses (the armory
	// leg's MP stance).
	if (joiner_) return false;
	// The weapon-busy gate [orig: @0x436958-0x436977 — no EquippedSlot passes;
	// currentAction < 2 (idle/emptyidle) or == 5 (the dry click) passes, as does a
	// pending OVERHEATED (nextAction == 11); an in-flight fire/reload/switch swallows
	// the toggle].
	const int32_t cur = weapon_slot_.current;
	const int32_t next = weapon_slot_.next;
	if (!(cur < 2 || cur == opennova::world::weapon_action::kEmpty ||
	      next == opennova::world::weapon_action::kOverheated))
		return false;
	return opennova::world::player_toggle_vehicle_mount(*world_, world_->cached.local_player);
}

TypedArray<Dictionary> NovaSimulation::get_attach_labels() const {
	TypedArray<Dictionary> out;
	if (!world_) return out;
	const opennova::world::Entity *player = world_->registry.get(world_->cached.local_player);
	if (player == nullptr || !player->alive || player->health <= 0) return out;
	// Armory mode = standing in the type-6 armory volume; the label pass reads the raw
	// flag [orig: is_armory_mode = entity Flags & 0x400000 @0x5a32c4].
	const bool armory_mode =
	    (player->flags & opennova::world::kEntityFlagArmoryZone) != 0;
	// The nearest-only gate [orig: Player_CanFireWeapon @0x5cf780 — EquippedSlot present
	// and parentSlot not 2/5 (ctrl/drvr); the camera-mode/underwater/scope legs live
	// host-side and are unmodeled here: docs/interface/hud-re.md (D-HUD-11)].
	const bool can_fire =
	    player->equipped_adm_index != 0xFF &&
	    !(player->mounted && opennova::world::is_vehicle_control_seat(player->mount_type));
	std::vector<opennova::world::AttachLabel> labels;
	opennova::world::collect_attach_labels(*world_, *player, armory_mode, can_fire, labels);
	for (const opennova::world::AttachLabel &l : labels) {
		Dictionary d;
		d["position"] = Vector3(l.world_pos.x, l.world_pos.y, l.world_pos.z);
		d["seat_type"] = static_cast<int>(l.type);
		d["armory"] = l.armory;
		d["nearest"] = l.nearest;
		String key;
		if (l.type == opennova::world::SeatType::Gunner) {
			// The USEGUN label text: the gun entity's primary weapon -> its weapon.def
			// attachtextid key [orig: Entity_GetWeaponSlots slot0 -> def+0x3A0 @0x5a351d].
			const opennova::world::Entity *cand = world_->registry.get(l.entity);
			if (cand != nullptr && !cand->primary_weapon.empty()) {
				const int wi = world_->weapons.index_of(cand->primary_weapon.c_str());
				if (wi >= 0)
					key = String(world_->weapons.entries[static_cast<size_t>(wi)]
					                     .attach_text_id.c_str());
			}
		}
		d["attach_text_key"] = key;
		out.push_back(d);
	}
	return out;
}

bool NovaSimulation::apply_local_player_loadout(const String &p_weapon_name,
                                                int p_player_class) {
	if (!world_) return false;
	opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (e == nullptr) return false;
	if (p_weapon_name.is_empty()) {
		e->equipped_adm_index = 0xFF;
		if (p_player_class >= 5 && p_player_class <= 9)
			e->player_class = static_cast<uint8_t>(p_player_class);
		return true;
	}
	const int idx = world_->weapons.index_of(p_weapon_name.utf8().get_data());
	if (idx < 0) return false;
	e->equipped_adm_index = static_cast<uint8_t>(idx);
	if (p_player_class >= 5 && p_player_class <= 9)
		e->player_class = static_cast<uint8_t>(p_player_class);
	return true;
}

// weapon.def -> the sim world's armory table. Mirrors the retail load site (Game_StartMission
// parses literally "weapon.def" through WeaponDefs_LoadFile right after AnimDef_InitAll wipes
// the AdmDef table [orig: @0x5254b3/@0x5254bd]); build_weapon_table ports the witnessed
// allocation rule (null@0 + by-name-reuse-else-lowest-free = file order; §5.57, D-NET-141).
Error NovaSimulation::load_weapon_table(const Ref<NovaResourceRoot> &p_resource_root,
                                        const String &p_name) {
	if (!world_) return ERR_UNCONFIGURED;
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty())
		return ERR_INVALID_PARAMETER;
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) return ERR_INVALID_PARAMETER;
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) return ERR_FILE_NOT_FOUND;

	DefWeaponsFile file = {};
	if (def_parse_weapons_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0)
		return ERR_CANT_OPEN;
	world_->weapons = opennova::np::build_weapon_table(file);
	def_free_weapons(&file);

	// The host's own player spawns in finish_load, BEFORE this feed — re-stamp its equipped
	// default now that WPN_M4AUTO resolves by name [orig: PlayerClass_InitEntity @0x4B1116].
	// Joiners spawn after the feed and get the default in Server_BuildPlayerInfoAndAdd.
	// (D-NET-143)
	const int m4 = world_->weapons.index_of("WPN_M4AUTO");
	if (m4 >= 0) {
		std::vector<opennova::world::EntityHandle> handles;
		world_->registry.for_each([&](const opennova::world::Entity &e) {
			if (e.item_id == opennova::world::kPlayerInfantryTypeId &&
			    e.equipped_adm_index == 0xFF)
				handles.push_back(e.handle);
		});
		for (const opennova::world::EntityHandle h : handles) {
			if (opennova::world::Entity *e = world_->registry.get(h))
				e->equipped_adm_index = static_cast<uint8_t>(m4);
		}
	}
	return OK;
}

// ammo.def -> the sim world's ballistics table + the weapon round_type resolve. Mirrors the
// retail load site (Game_StartMission parses literally "ammo.def" through AmmoDef_LoadAll
// @0x40b0b0, the sibling of the weapon.def load [orig: @0x52548a]); the resolve binds each
// adm's fired round to its AmmoTable index (the original's adm+84 pair; §5.60). Call AFTER
// load_weapon_table — an empty armory leaves every round_type unresolved and the fire
// pipeline echoes without spawning sim rounds.
Error NovaSimulation::load_ammo_table(const Ref<NovaResourceRoot> &p_resource_root,
                                      const String &p_name) {
	if (!world_) return ERR_UNCONFIGURED;
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty())
		return ERR_INVALID_PARAMETER;
	const String file_name = p_name.get_file();
	if (file_name.is_empty()) return ERR_INVALID_PARAMETER;
	const PackedByteArray bytes = p_resource_root->read_file(file_name);
	if (bytes.is_empty()) return ERR_FILE_NOT_FOUND;

	DefAmmoFile file = {};
	if (def_parse_ammo_memory(bytes.ptr(), static_cast<size_t>(bytes.size()), &file) != 0)
		return ERR_CANT_OPEN;
	world_->ammo = opennova::np::build_ammo_table(file);
	def_free_ammo(&file);
	opennova::np::resolve_weapon_round_types(world_->weapons, world_->ammo);
	return OK;
}

opennova::mission::PromoteOptions NovaSimulation::promote_options() const {
	opennova::mission::PromoteOptions opts;
	opts.item_seat_specs = item_seat_specs_;
	return opts;
}

void NovaSimulation::set_item_seat_specs(const Array &p_specs) {
	item_seat_specs_.clear();
	for (int64_t i = 0; i < p_specs.size(); ++i) {
		const Variant spec_v = p_specs[i];
		if (spec_v.get_type() != Variant::DICTIONARY) continue;
		const Dictionary spec_d = spec_v;

		opennova::mission::ItemSeatSpec spec;
		spec.type_id = static_cast<int32_t>(spec_d.get("type_id", 0));
		if (spec.type_id == 0) continue;
		spec.emplaced_pose_variant = static_cast<uint8_t>(
		    std::clamp(static_cast<int>(spec_d.get("emplaced_pose_variant", 0)), 0, 8));

		const Variant seats_v = spec_d.get("seats", Array());
		if (seats_v.get_type() != Variant::ARRAY) continue;
		const Array seats_a = seats_v;
		for (int64_t j = 0; j < seats_a.size(); ++j) {
			const Variant seat_v = seats_a[j];
			if (seat_v.get_type() != Variant::DICTIONARY) continue;
			const Dictionary seat_d = seat_v;

			opennova::world::Seat seat;
			seat.type = seat_type_from_variant(static_cast<int>(seat_d.get("type", 0)));
			if (seat.type == opennova::world::SeatType::None) continue;
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
		if (!spec.seats.empty() || !spec.armory_points.empty() ||
		    !spec.primary_weapon.empty())
			item_seat_specs_.push_back(std::move(spec));
	}
}

void NovaSimulation::set_terrain_height_field(const Ref<NovaTerrainData> &p_terrain) {
	// Clear first so a null/unloaded terrain disables grounding.
	terrain_heightmap_.clear();
	terrain_sector_grid_.clear();
	terrain_field_ = opennova::terrain::TerrainHeightField{};

	surface_indices_.clear();
	surface_map_ = opennova::terrain::SurfaceTypeMap{};

	if (p_terrain.is_valid() && p_terrain->is_loaded()) {
		const opennova::CptFile &cpt = p_terrain->get_cpt();
		const opennova::TrnConfig &trn = p_terrain->get_trn();
		if (!cpt.depth_buffer.empty()) {
			terrain_heightmap_ = cpt.depth_buffer; // own a copy (outlives the source resource)
			terrain_sector_grid_.resize(256);
			const int *grid = &trn.sector_grid[0][0];
			for (int i = 0; i < 256; ++i) terrain_sector_grid_[i] = grid[i];

			terrain_field_.heightmap = terrain_heightmap_.data();
			terrain_field_.dim = static_cast<int>(std::sqrt(static_cast<double>(terrain_heightmap_.size())));
			terrain_field_.layout.sector_grid = terrain_sector_grid_.data();
			terrain_field_.layout.origin_x = trn.origin_x;
			terrain_field_.layout.origin_y = trn.origin_y;
			// Water clamp deferred: water_height units (vs the 16.16 worldY @0x26C6454 the original
			// compares) are not yet verified, so leave has_water off rather than float entities onto
			// a wrong plane. The ground-following path (the Phase 1 goal) does not need it.
			terrain_field_.has_water = false;

			// The charmap surface raster for the footstep surface pick (own a
			// copy like the depth buffer; shares the sector grid + origins)
			// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510].
			const std::vector<uint8_t> &charmap = p_terrain->get_charmap_indices();
			if (!charmap.empty() && p_terrain->get_charmap_width() > 0) {
				surface_indices_ = charmap;
				surface_map_.data = surface_indices_.data();
				surface_map_.width = p_terrain->get_charmap_width();
				surface_map_.height = p_terrain->get_charmap_height();
				surface_map_.sector_grid = terrain_sector_grid_.data();
				surface_map_.origin_x = trn.origin_x;
				surface_map_.origin_y = trn.origin_y;
			}
		}
	}
	apply_terrain_to_ai();
}

void NovaSimulation::set_sound_profiles(const PackedByteArray &p_sndprof_text) {
	sndprof_text_.assign(p_sndprof_text.ptr(), p_sndprof_text.ptr() + p_sndprof_text.size());
	apply_sound_state_to_world();
}

void NovaSimulation::set_water_z(double p_water_y) {
	env_water_z_q16_ = static_cast<int32_t>(p_water_y * 65536.0);
	if (world_) world_->env.water_z = env_water_z_q16_;
}

Array NovaSimulation::drain_slot_sounds() {
	Array out;
	if (!loaded_) return out;
	for (const opennova::world::SoundSlotEvent &ev : world_->slot_sounds) {
		Dictionary d;
		d["set"] = String(ev.set_name);
		// Mission-frame 16.16 -> godot (x, z, -y), same mapping as the fire drain.
		d["pos"] = Vector3(static_cast<float>(ev.pos[0]) / 65536.0f,
		                   static_cast<float>(ev.pos[2]) / 65536.0f,
		                   static_cast<float>(-ev.pos[1]) / 65536.0f);
		d["handle"] = ev.source_handle;
		d["slot"] = ev.slot;
		out.push_back(d);
	}
	world_->slot_sounds.clear();
	return out;
}

void NovaSimulation::finish_load(const opennova::bms::File &file) {
	// One world, three systems, the faithful tick order. The AI-change action family reaches
	// brains through World::ai; wire it before registering so the pre-mission pass can dispatch.
	bms_->load(file.events, file.triggers, file.actions);
	// Mission attribute flags -> the world (0x40 = SinglePlayerRespawn gates the SP
	// death auto-lose in check_win_conditions). [orig: Bms_AttribFlags @0xa76258,
	// read by Server_CheckWinConditions @0x51ad6f]
	world_->mission_attrib_flags = static_cast<uint32_t>(file.header.attrib_flags);
	world_->ai = ai_.get();
	// P7 listen server (SP + LAN host): stand up the npruntime in-match runtime (mode-3 HostClient
	// over an in-process loopback, the faithful §5.0 path). Server_TickUpdate owns the logic tick +
	// the C2S drain + the 0x0A fan, so there is NO net ISystem here (D-NET-123/125) — the
	// present reads the host's own ClientRuntime view, and a LAN host adds the socket legs in host_pump.
	if (listen_server_) {
		bringup_host_runtime(file);
	}
	// P7 co-op LAN joiner: a pure non-authority client. Build a fresh np::ClientRuntime (Joiner role)
	// per (re)load — start() fully resets the session, so a reload reconnects cleanly. No net ISystem
	// is registered (the joiner never serializes; run_logic_tick(false) leaves World::net the default
	// LocalSink for WAC/BMS sinks). The local player L is spawned in joiner_pump on the name-match.
	if (joiner_) {
		runtime_ = std::make_unique<opennova::np::ClientRuntime>(
				opennova::ClientSession::Config::jointoperations(), joiner_player_name_);
		joiner_started_ = false;
		joiner_local_spawned_ = false;
		joiner_self_wire_handle_ = 0;
	}
	// Re-arm the fresh runtime's decode view with the items.def class table (built by a
	// prior resolve_item_traits; the shell also re-resolves per load, which re-installs).
	install_item_class_resolver();
	opennova::mission::register_mission_systems(*world_, *wac_, *bms_, *ai_);
	// Re-install the held script program onto the fresh WacSystem (reset_world
	// recreated it). The 62-tick execution divider stays inside the system
	// [orig: dword_C6EAD4 / cmp 0x3E]; a missing program leaves the VM unloaded
	// and its tick early-outs, the BMS-only case.
	if (wac_program_.is_valid() && wac_program_->is_ok()) {
		wac_->set_program(wac_program_->native_program());
	}
	// PreMission events settle initial scripted state before the clock starts (AI is skipped on
	// the pre-mission pass). Snapshot AFTER it so Stop restores the true play-start state.
	world_->run_logic_tick(/*is_authority=*/true, /*pre_mission=*/true);
	baseline_ = world_->snapshot();
	ai_->capture_spawn_baseline();
	have_baseline_ = true;
	loaded_ = true;
}

void NovaSimulation::apply_host_session_mission_header(const opennova::bms::File &file) {
	std::vector<uint8_t> header_blob;
	std::string error;
	if (opennova::bms::encode_header_blob(file, header_blob, error)) {
		host_session_config_.mission_header_blob = std::move(header_blob);
	} else {
		host_session_config_.mission_header_blob.clear();
	}

	const std::string mission_name = file.get_mission_name();
	if (!mission_name.empty()) {
		host_session_config_.mission_name = mission_name;
		if (host_session_config_.spawn_names.empty()) {
			host_session_config_.spawn_names.push_back(mission_name);
		}
	}
	// P7: host_session_config_ is consumed at the next load by bringup_host_runtime
	// (configure_session_runtime + the §5.1 reactive-reply config); nothing to refresh live.
}

void NovaSimulation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_from_mission_data", "mission"), &NovaSimulation::load_from_mission_data);
	ClassDB::bind_method(D_METHOD("load_mission_file", "path"), &NovaSimulation::load_mission_file);
	ClassDB::bind_method(D_METHOD("build_demo_mission"), &NovaSimulation::build_demo_mission);
	ClassDB::bind_method(D_METHOD("is_loaded"), &NovaSimulation::is_loaded);
	ClassDB::bind_method(D_METHOD("set_playing", "playing"), &NovaSimulation::set_playing);
	ClassDB::bind_method(D_METHOD("is_playing"), &NovaSimulation::is_playing);
	ClassDB::bind_method(D_METHOD("step"), &NovaSimulation::step);
	ClassDB::bind_method(D_METHOD("restart"), &NovaSimulation::restart);
	ClassDB::bind_method(D_METHOD("enable_listen_server", "enable"), &NovaSimulation::enable_listen_server);
	ClassDB::bind_method(D_METHOD("set_terrain_til_data", "til_bytes"), &NovaSimulation::set_terrain_til_data);
	ClassDB::bind_method(D_METHOD("is_listen_server"), &NovaSimulation::is_listen_server);
	ClassDB::bind_method(D_METHOD("enable_host_listen", "port"), &NovaSimulation::enable_host_listen);
	ClassDB::bind_method(D_METHOD("is_host_listening"), &NovaSimulation::is_host_listening);
	ClassDB::bind_method(D_METHOD("get_host_listen_port"), &NovaSimulation::get_host_listen_port);
	ClassDB::bind_method(D_METHOD("get_host_peer_count"), &NovaSimulation::get_host_peer_count);
	ClassDB::bind_method(D_METHOD("configure_host_session", "options"), &NovaSimulation::configure_host_session);
	ClassDB::bind_method(D_METHOD("get_host_session_config"), &NovaSimulation::get_host_session_config);
	ClassDB::bind_method(D_METHOD("admit_test_remote_peer", "position", "yaw_deg", "team"), &NovaSimulation::admit_test_remote_peer);
	ClassDB::bind_method(D_METHOD("enable_join", "host_ip", "port", "player_name"), &NovaSimulation::enable_join);
	ClassDB::bind_method(D_METHOD("is_joiner"), &NovaSimulation::is_joiner);
	ClassDB::bind_method(D_METHOD("is_joined_in_match"), &NovaSimulation::is_joined_in_match);
	ClassDB::bind_method(D_METHOD("get_joiner_phase"), &NovaSimulation::get_joiner_phase);
	ClassDB::bind_method(D_METHOD("get_joiner_self_handle"), &NovaSimulation::get_joiner_self_handle);
	ClassDB::bind_method(D_METHOD("spawn_local_player", "position", "yaw_deg", "team"), &NovaSimulation::spawn_local_player);
	ClassDB::bind_method(D_METHOD("spawn_local_player_at_start"), &NovaSimulation::spawn_local_player_at_start);
	ClassDB::bind_method(D_METHOD("has_local_player"), &NovaSimulation::has_local_player);
	ClassDB::bind_method(D_METHOD("get_local_player_wire_handle"), &NovaSimulation::get_local_player_wire_handle);
	ClassDB::bind_method(D_METHOD("set_player_input", "forward", "back", "left", "right", "lean_left", "lean_right", "jump"), &NovaSimulation::set_player_input);
	ClassDB::bind_method(D_METHOD("add_local_player_look", "dx_px", "dy_px"), &NovaSimulation::add_local_player_look);
	ClassDB::bind_method(D_METHOD("set_local_player_mouse", "sensitivity", "invert_y"), &NovaSimulation::set_local_player_mouse);
	ClassDB::bind_method(D_METHOD("request_local_player_stance", "stance"), &NovaSimulation::request_local_player_stance);
	ClassDB::bind_method(D_METHOD("get_local_player_position"), &NovaSimulation::get_local_player_position);
	ClassDB::bind_method(D_METHOD("get_local_player_yaw_deg"), &NovaSimulation::get_local_player_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_pitch_deg"), &NovaSimulation::get_local_player_pitch_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_body_anim_slot"), &NovaSimulation::get_local_player_body_anim_slot);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_key"), &NovaSimulation::get_local_player_anim_key);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_phase_ticks"), &NovaSimulation::get_local_player_anim_phase_ticks);
	ClassDB::bind_method(D_METHOD("get_local_player_aim_overlay"), &NovaSimulation::get_local_player_aim_overlay);
	ClassDB::bind_method(D_METHOD("set_local_player_weapon", "def", "clip_seconds"), &NovaSimulation::set_local_player_weapon);
	ClassDB::bind_method(D_METHOD("clear_local_player_weapon"), &NovaSimulation::clear_local_player_weapon);
	ClassDB::bind_method(D_METHOD("set_local_player_weapon_input", "fire_held", "fire_pressed", "reload_pressed"), &NovaSimulation::set_local_player_weapon_input);
	ClassDB::bind_method(D_METHOD("request_local_player_scope_toggle"), &NovaSimulation::request_local_player_scope_toggle);
	ClassDB::bind_method(D_METHOD("set_local_player_eye", "eye_godot", "valid"), &NovaSimulation::set_local_player_eye);
	ClassDB::bind_method(D_METHOD("set_local_player_camera_third_person", "third_person"), &NovaSimulation::set_local_player_camera_third_person);
	ClassDB::bind_method(D_METHOD("get_local_player_view"), &NovaSimulation::get_local_player_view);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("fov_vertical_from_horizontal", "fov_h_deg", "aspect"), &NovaSimulation::fov_vertical_from_horizontal);
	ClassDB::bind_method(D_METHOD("get_local_player_weapon_state"), &NovaSimulation::get_local_player_weapon_state);
	ClassDB::bind_method(D_METHOD("drain_local_player_weapon_events"), &NovaSimulation::drain_local_player_weapon_events);
	ClassDB::bind_method(D_METHOD("drain_round_impacts"), &NovaSimulation::drain_round_impacts);
	ClassDB::bind_method(D_METHOD("get_local_player_health"), &NovaSimulation::get_local_player_health);
	ClassDB::bind_method(D_METHOD("get_local_player_max_health"), &NovaSimulation::get_local_player_max_health);
	ClassDB::bind_method(D_METHOD("get_local_player_team"), &NovaSimulation::get_local_player_team);
	ClassDB::bind_method(D_METHOD("get_local_player_class"), &NovaSimulation::get_local_player_class);
	ClassDB::bind_method(D_METHOD("get_local_player_weapon_name"), &NovaSimulation::get_local_player_weapon_name);
	ClassDB::bind_method(D_METHOD("drain_effects"), &NovaSimulation::drain_effects);
	ClassDB::bind_method(D_METHOD("drain_fire_presentation_events"),
			&NovaSimulation::drain_fire_presentation_events);
	ClassDB::bind_method(D_METHOD("get_tracer_rounds"), &NovaSimulation::get_tracer_rounds);
	ClassDB::bind_method(D_METHOD("drain_destruction_events"),
			&NovaSimulation::drain_destruction_events);
	ClassDB::bind_method(D_METHOD("get_death_pieces"), &NovaSimulation::get_death_pieces);
	ClassDB::bind_method(D_METHOD("get_destruction_debug", "bms_id"),
			&NovaSimulation::get_destruction_debug);
	ClassDB::bind_method(D_METHOD("set_sound_profiles", "sndprof_text"),
			&NovaSimulation::set_sound_profiles);
	ClassDB::bind_method(D_METHOD("set_water_z", "water_y"), &NovaSimulation::set_water_z);
	ClassDB::bind_method(D_METHOD("drain_slot_sounds"), &NovaSimulation::drain_slot_sounds);
	ClassDB::bind_method(D_METHOD("set_wac_program", "program"), &NovaSimulation::set_wac_program);
	ClassDB::bind_method(D_METHOD("get_wac_program"), &NovaSimulation::get_wac_program);
	ClassDB::bind_method(D_METHOD("compile_and_set_wac", "sources"), &NovaSimulation::compile_and_set_wac);
	ClassDB::bind_method(D_METHOD("get_wac_state"), &NovaSimulation::get_wac_state);
	ClassDB::bind_method(D_METHOD("get_runtime_perf_counters"), &NovaSimulation::get_runtime_perf_counters);
	ClassDB::bind_method(D_METHOD("set_wac_paused", "paused"), &NovaSimulation::set_wac_paused);
	ClassDB::bind_method(D_METHOD("is_wac_paused"), &NovaSimulation::is_wac_paused);
	ClassDB::bind_method(D_METHOD("set_mission_variable", "index", "value"), &NovaSimulation::set_mission_variable);
	ClassDB::bind_method(D_METHOD("get_mission_variable", "index"), &NovaSimulation::get_mission_variable);
	ClassDB::bind_method(D_METHOD("has_event_fired", "index"), &NovaSimulation::has_event_fired);
	ClassDB::bind_method(D_METHOD("get_event_count"), &NovaSimulation::get_event_count);
	ClassDB::bind_method(D_METHOD("get_logic_tick"), &NovaSimulation::get_logic_tick);
	ClassDB::bind_method(D_METHOD("get_mission_variables_snapshot"), &NovaSimulation::get_mission_variables_snapshot);
	ClassDB::bind_method(D_METHOD("get_global_variables_snapshot"), &NovaSimulation::get_global_variables_snapshot);
	ClassDB::bind_method(D_METHOD("get_music_variables_snapshot"), &NovaSimulation::get_music_variables_snapshot);
	ClassDB::bind_method(D_METHOD("set_global_variable", "index", "value"), &NovaSimulation::set_global_variable);
	ClassDB::bind_method(D_METHOD("get_global_variable", "index"), &NovaSimulation::get_global_variable);
	ClassDB::bind_method(D_METHOD("get_fired_events_snapshot"), &NovaSimulation::get_fired_events_snapshot);
	ClassDB::bind_method(D_METHOD("get_entity_debug", "index"), &NovaSimulation::get_entity_debug);
	ClassDB::bind_method(D_METHOD("debug_set_entity_health", "index", "hp"), &NovaSimulation::debug_set_entity_health);
	ClassDB::bind_method(D_METHOD("debug_set_entity_position", "index", "mission_pos"), &NovaSimulation::debug_set_entity_position);
	ClassDB::bind_method(D_METHOD("get_world_entity_debug", "net_id"), &NovaSimulation::get_world_entity_debug);
	ClassDB::bind_method(D_METHOD("debug_set_world_entity_position", "net_id", "mission_pos"), &NovaSimulation::debug_set_world_entity_position);
	ClassDB::bind_method(D_METHOD("set_ai_muzzle_world", "net_id", "godot_pos"), &NovaSimulation::set_ai_muzzle_world);
	ClassDB::bind_method(D_METHOD("get_round_outcome_debug"), &NovaSimulation::get_round_outcome_debug);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("ai_state_name", "state"), &NovaSimulation::ai_state_name);
	ClassDB::bind_static_method("NovaSimulation", D_METHOD("infantry_anim_key", "state"), &NovaSimulation::infantry_anim_key);
	ClassDB::bind_method(D_METHOD("get_entity_count"), &NovaSimulation::get_entity_count);
	ClassDB::bind_method(D_METHOD("get_entity_kind", "index"), &NovaSimulation::get_entity_kind);
	ClassDB::bind_method(D_METHOD("get_entity_index", "index"), &NovaSimulation::get_entity_index);
	ClassDB::bind_method(D_METHOD("get_entity_position", "index"), &NovaSimulation::get_entity_position);
	ClassDB::bind_method(D_METHOD("get_entity_yaw", "index"), &NovaSimulation::get_entity_yaw);
	ClassDB::bind_method(D_METHOD("get_entity_yaw_deg", "index"), &NovaSimulation::get_entity_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_entity_state", "index"), &NovaSimulation::get_entity_state);
	ClassDB::bind_method(D_METHOD("get_entity_net_id", "index"), &NovaSimulation::get_entity_net_id);
	ClassDB::bind_method(D_METHOD("get_foliage_mask_anchor_positions"),
			&NovaSimulation::get_foliage_mask_anchor_positions);
	ClassDB::bind_method(D_METHOD("get_entity_effect_state_for_ssn", "ssn"),
	                     &NovaSimulation::get_entity_effect_state_for_ssn);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_ssn", "ssn"),
	                     &NovaSimulation::get_present_effect_state_for_ssn);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_wire_handle", "wire_handle"),
	                     &NovaSimulation::get_present_effect_state_for_wire_handle);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_bms_id", "bms_id"),
	                     &NovaSimulation::get_present_effect_state_for_bms_id);
	ClassDB::bind_method(D_METHOD("get_present_effect_state_for_origin", "kind", "index"),
	                     &NovaSimulation::get_present_effect_state_for_origin);
	ClassDB::bind_method(D_METHOD("get_entity_bms_id", "index"), &NovaSimulation::get_entity_bms_id);
	ClassDB::bind_method(D_METHOD("get_entity_owner_connection_id", "index"),
	                     &NovaSimulation::get_entity_owner_connection_id);
	ClassDB::bind_method(D_METHOD("get_entity_wire_handle", "index"),
	                     &NovaSimulation::get_entity_wire_handle);
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_phase", "index", "channel"), &NovaSimulation::get_entity_part_anim_phase);
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_active", "index", "channel"), &NovaSimulation::get_entity_part_anim_active);
	ClassDB::bind_method(D_METHOD("get_entity_body_anim_slot", "index"), &NovaSimulation::get_entity_body_anim_slot);
	ClassDB::bind_method(D_METHOD("get_entity_hidden", "index"), &NovaSimulation::get_entity_hidden);
	ClassDB::bind_method(D_METHOD("get_present_snapshot"), &NovaSimulation::get_present_snapshot);
	ClassDB::bind_method(D_METHOD("get_present_stride"), &NovaSimulation::get_present_stride);
	ClassDB::bind_method(D_METHOD("set_terrain_height_field", "terrain"), &NovaSimulation::set_terrain_height_field);
	ClassDB::bind_method(D_METHOD("set_item_seat_specs", "specs"), &NovaSimulation::set_item_seat_specs);
	ClassDB::bind_method(D_METHOD("set_infantry_anim_map", "resource_root", "adm_name"), &NovaSimulation::set_infantry_anim_map);
	ClassDB::bind_method(D_METHOD("resolve_infantry_adm_ids", "resource_root", "item_db"), &NovaSimulation::resolve_infantry_adm_ids);
	ClassDB::bind_method(D_METHOD("resolve_item_traits", "item_db"), &NovaSimulation::resolve_item_traits);
	ClassDB::bind_method(D_METHOD("resolve_ai_weapons", "item_db"), &NovaSimulation::resolve_ai_weapons);
	ClassDB::bind_method(D_METHOD("resolve_collision_instances", "item_db", "placer"),
	                     &NovaSimulation::resolve_collision_instances);
	ClassDB::bind_method(D_METHOD("occlusion_init_mission"),
	                     &NovaSimulation::occlusion_init_mission);
	ClassDB::bind_method(D_METHOD("run_occlusion_frame", "camera", "fov_y_deg", "aspect",
	                              "near", "fog_dist_units", "water_z_units", "force_indoors"),
	                     &NovaSimulation::run_occlusion_frame);
	ClassDB::bind_method(D_METHOD("get_building_visibility"),
	                     &NovaSimulation::get_building_visibility);
	ClassDB::bind_method(D_METHOD("get_render_culled_bms_ids"),
	                     &NovaSimulation::get_render_culled_bms_ids);
	ClassDB::bind_method(D_METHOD("occlusion_water_visible"),
	                     &NovaSimulation::occlusion_water_visible);
	ClassDB::bind_method(D_METHOD("occlusion_camera_indoors"),
	                     &NovaSimulation::occlusion_camera_indoors);
	ClassDB::bind_method(D_METHOD("get_collision_debug"), &NovaSimulation::get_collision_debug);
	ClassDB::bind_method(D_METHOD("get_occlusion_debug"), &NovaSimulation::get_occlusion_debug);
	ClassDB::bind_method(D_METHOD("get_occlusion_portal_debug", "anchor", "range_units"),
	                     &NovaSimulation::get_occlusion_portal_debug);
	ClassDB::bind_method(D_METHOD("local_player_indoors"), &NovaSimulation::local_player_indoors);
	ClassDB::bind_method(D_METHOD("local_player_blink_flags"), &NovaSimulation::local_player_blink_flags);
	ClassDB::bind_method(D_METHOD("sound_occlusion_distance_q16", "listener_pos", "source_pos",
	                              "distance_q16", "source_bms_id"),
	                     &NovaSimulation::sound_occlusion_distance_q16, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("local_player_in_armory_zone"), &NovaSimulation::local_player_in_armory_zone);
	ClassDB::bind_method(D_METHOD("local_player_in_vehicle_loadout_zone"), &NovaSimulation::local_player_in_vehicle_loadout_zone);
	ClassDB::bind_method(D_METHOD("local_player_toggle_mount"), &NovaSimulation::local_player_toggle_mount);
	ClassDB::bind_method(D_METHOD("get_attach_labels"), &NovaSimulation::get_attach_labels);
	ClassDB::bind_method(D_METHOD("apply_local_player_loadout", "weapon_name", "player_class"),
	                     &NovaSimulation::apply_local_player_loadout);
	ClassDB::bind_method(D_METHOD("load_weapon_table", "resource_root", "name"),
	                     &NovaSimulation::load_weapon_table, DEFVAL(String("weapon.def")));
	ClassDB::bind_method(D_METHOD("load_ammo_table", "resource_root", "name"),
	                     &NovaSimulation::load_ammo_table, DEFVAL(String("ammo.def")));
	ClassDB::bind_method(D_METHOD("get_infantry_clip_count"), &NovaSimulation::get_infantry_clip_count);
	ClassDB::bind_method(D_METHOD("set_loco_scale", "scale"), &NovaSimulation::set_loco_scale);
	ClassDB::bind_method(D_METHOD("get_loco_scale"), &NovaSimulation::get_loco_scale);
	ClassDB::bind_method(D_METHOD("get_spawned_count"), &NovaSimulation::get_spawned_count);
	ClassDB::bind_method(D_METHOD("get_brain_count"), &NovaSimulation::get_brain_count);

	// Present-snapshot field layout (single source of truth for the GDScript present pass).
	BIND_ENUM_CONSTANT(PF_KIND);
	BIND_ENUM_CONSTANT(PF_INDEX);
	BIND_ENUM_CONSTANT(PF_BMS_ID);
	BIND_ENUM_CONSTANT(PF_NET_ID);
	BIND_ENUM_CONSTANT(PF_POS_X);
	BIND_ENUM_CONSTANT(PF_POS_Y);
	BIND_ENUM_CONSTANT(PF_POS_Z);
	BIND_ENUM_CONSTANT(PF_PITCH_DEG);
	BIND_ENUM_CONSTANT(PF_YAW_DEG);
	BIND_ENUM_CONSTANT(PF_ROLL_DEG);
	BIND_ENUM_CONSTANT(PF_PHASE1);
	BIND_ENUM_CONSTANT(PF_ACTIVE1);
	BIND_ENUM_CONSTANT(PF_PHASE2);
	BIND_ENUM_CONSTANT(PF_ACTIVE2);
	BIND_ENUM_CONSTANT(PF_BODY_ANIM_SLOT);
	BIND_ENUM_CONSTANT(PF_ANIM_STATE);
	BIND_ENUM_CONSTANT(PF_ANIM_PHASE_TICKS);
	BIND_ENUM_CONSTANT(PF_HIDDEN);
	BIND_ENUM_CONSTANT(PF_ALIVE);
	BIND_ENUM_CONSTANT(PF_TYPE_ID);
	BIND_ENUM_CONSTANT(PF_WIRE_HANDLE);
	BIND_ENUM_CONSTANT(PF_STRIDE);
	BIND_ENUM_CONSTANT(EFFECT_STATE_POSITION);
	BIND_ENUM_CONSTANT(EFFECT_STATE_ROTATION_DEG);
	BIND_ENUM_CONSTANT(EFFECT_STATE_COUNT);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "playing"), "set_playing", "is_playing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loco_scale"), "set_loco_scale", "get_loco_scale");
}

void NovaSimulation::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS && playing_ && loaded_) {
		step();
	}
}

bool NovaSimulation::load_from_mission_data(const Ref<NovaMissionData> &p_mission) {
	if (p_mission.is_null()) return false;
	reset_world();
	// The editor's live, in-memory mission (unsaved edits included).
	const opennova::bms::File &file = p_mission->native_document().bms_file();
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	apply_host_session_mission_header(file);
	return true;
}

bool NovaSimulation::load_mission_file(const String &path) {
	reset_world();
	opennova::bms::File file;
	std::string err;
	if (!opennova::bms::parse_file(std::string(path.utf8().get_data()), file, err)) {
		return false;
	}
	host_session_config_.mission_file = std::string(path.get_file().utf8().get_data());
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	apply_host_session_mission_header(file);
	return true;
}

void NovaSimulation::build_demo_mission() {
	reset_world();
	opennova::bms::File file = make_demo_mission();
	host_session_config_.mission_file = "demo.bms";
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	apply_host_session_mission_header(file);
}

bool NovaSimulation::step() {
	if (!loaded_) return false;
	// ONE logic tick (the original's 62 Hz engine tick). The WAC VM self-gates to every
	// 62nd tick and the BMS evaluator quarter-passes every 16th, inside their systems —
	// exactly where the original keeps those dividers. A host frame runs 0..N of these;
	// the accumulator that decides N lives in MissionRuntime.tick_realtime
	// [orig: Game_MainLoop @ 0x52b630].
	//
	// Listen-server frame order [orig: Game_ProcessMainFrame @ 0x5263f0]:
	//   input -> net(drain C2S) -> run_logic_tick(WAC/BMS/AI) -> net(emit S2C) -> present.
	// Server_TickUpdate owns the C2S drain at the top of the loop and the post-logic S2C fan;
	// host_pump drives it and the local client's decode happens via the host's ClientRuntime.
	const uint64_t sim_start = perf_now_us();
	if (listen_server_) { // P7 listen server (SP + LAN host) -> the npruntime owner loop
		host_pump();
		last_sim_tick_us_ = perf_now_us() - sim_start;
		return true;
	}
	if (joiner_) { // P7 co-op joiner -> the npruntime ClientRuntime (non-authority)
		joiner_pump();
		last_sim_tick_us_ = perf_now_us() - sim_start;
		return true;
	}
	// No-net editor/unit path: one authoritative logic tick, no replication.
	apply_player_input_pre_tick();
	world_->run_logic_tick(/*is_authority=*/true);
	tick_local_player_view();   // retail promotes the per-frame view before weapon actions
	tick_local_player_weapon(); // the equipped-slot FSM pump, after the view promoter
	last_sim_tick_us_ = perf_now_us() - sim_start;
	return true;
}

// The post-logic half of the listen-server frame: serialize the live world into one
// S2C 0x0A frame, loop it back in-process, and let the local client decode it into the
// ClientState the present pass reads. No-op when the listen server is off.
// P7: per-load host bring-up — the faithful §5.0 mode-3 in-process listen server
// [orig: SinglePlayer_StartMission @0x561af0], mirroring apps/nw_server/main.cpp. The host's own
// player AUTO-spawns through the real pipeline (Server_ProcessPendingPlayerSpawns ->
// select_player_spawn start marker), and its own loopback client renders the per-frame 0x0A.
void NovaSimulation::bringup_host_runtime(const opennova::bms::File &file) {
	namespace np = opennova::np;
	// Persist the mission so ctx_.mission (read by the §5.1 0x0B BMS-header burst for LAN joiners)
	// outlives the match — the load-local bms::File would dangle.
	mission_file_ = file;
	host_loop_.clear();
	// Reload: a fresh host_owner_ drops any stale connections / peers from a prior mission. A reload is a
	// new match (Stop -> load), so configure_session_runtime runs once per match (never mid-match,
	// D-NET-124). serve_and_play: host_session_pump must NOT discard the host's own loopback 0x0A — we
	// fold it into ClientState (runtime_) to render the host's own view.
	// Serve-and-play (default) vs dedicated. SP / editor preview are ALWAYS serve-and-play (they render
	// the host's own player); a LAN host honors the UI server-type (host_serve_and_play_, from
	// configure_host_session). A dedicated host (serve_and_play=false) skips the own-player spawn + the
	// local view below and lets host_session_pump discard the host loopback (step 5) — mirroring
	// start_host_session's gating [orig: SinglePlayer_StartMission @0x561af0].
	const bool serve_and_play = host_listen_ ? host_serve_and_play_ : true;
	host_owner_ = np::HostOwner{};
	host_owner_.host_loopback = &host_loop_;
	host_owner_.serve_and_play = serve_and_play;
	ctx_.world = world_.get();
	ctx_.mission = &mission_file_;
	ctx_.terrain_til_data = terrain_til_data_; // S2C 0x45 terrain-tile load source (empty => skipped, §5.37)
	// Server_TickUpdate owns the per-frame C2S drain + S2C fan over connection_list; there is no
	// separate net ISystem (retired P8).

	// The ONE consolidated GameConfig for create_session (ADR 0013): a LAN host takes its lobby name /
	// gametype / mission + the §5.1 reply slice from the GDScript-configured host_session_config_; SP is
	// the faithful "SINGLEPLAYERGAME" / 1 player. game_type (g_GameType) now feeds BOTH the S2C 0x08
	// block dword[3] AND the 0x7B/0x60 bodies (§6.9; NapiNPMsg_0x7B_BuildPayload @0x507740).
	np::GameConfig host_config;
	if (host_listen_) {
		host_config = host_session_config_; // mission/player/spawn + game_type/mp_attributes from the UI
		if (host_config.server_name.empty()) host_config.server_name = "OpenNova LAN Host";
		host_config.max_players = host_max_players_; // the UI player cap (configure_host_session clamped 1..65)
	} else {
		host_config.server_name = "SINGLEPLAYERGAME";
		host_config.max_players = 1;
	}

	// The witnessed §5.0 listen-host bring-up, dedup'd to the ONE shared helper start_host_session
	// (mode 3 -> set_transport_mode -> create_session(&host_loop_) [+ Server_InitNewRoundState] ->
	// configure_session_runtime; then, when serve_and_play, FAITHFUL auto-spawn of the host's own player
	// at the start marker + latch its loopback in-match so Server_TickUpdate fans it the per-frame
	// whole-world 0x0A its local view renders from). host_owner_.host_loopback / .serve_and_play + ctx_.world
	// were set above; this replaces the copy that had drifted out of the helper. [orig: SinglePlayer_StartMission
	// @0x561af0]. The one GameConfig carries the §5.1 reactive-reply config for the joiner replies too.
	np::HostConfig host_cfg;
	host_cfg.config = host_config;
	host_cfg.socket_mode = host_listen_ ? np::SocketMode::Lan : np::SocketMode::Socketless;
	host_cfg.serve_and_play = serve_and_play;
	np::start_host_session(host_owner_, host_cfg);
	if (serve_and_play) {
		// The host's own client view (HostClient role: recv-fold only, 0x0C suppressed). Folds host_loop_
		// each frame into the ClientState the present pass reads.
		runtime_ = std::make_unique<np::ClientRuntime>(host_loop_);

		// Seed the look heading from the auto-spawned player's facing so the body starts aligned (the
		// motor drives entity Yaw from player_input_.look_heading each frame, else input snaps it to 0).
		player_input_ = opennova::world::PlayerInput{};
		stance_latch_ = 0;
		look_px_accum_x_ = look_px_accum_y_ = 0.0f;
		if (world_->ai && world_->cached.local_player.valid()) {
			if (const AiEntity *pe = world_->ai->for_handle(world_->cached.local_player)) {
				player_input_.look_heading = pe->heading;
			}
		}
	} else {
		// Dedicated (UI "serve only"). The witnessed original makes this a true host-only session
		// [orig: HG_SERVEONLY -> CGameSession_SetConnectionMode(1), is_host=1/is_client=0; HostDialog
		// read @0x555940, dispatch @0x556d00, mode switch @0x4c49f0]. We instead keep the single mode-3
		// listen-server path (ADR 0011) with serve_and_play=false — wire-equivalent to the joiner (mode 1
		// vs 3 changes only the host's OWN client bookkeeping, never the S2C stream a peer receives),
		// tracked as a divergence (docs/net/novaworld-net-re.md, D-NET-131). No host player spawns (a slot
		// fills lazily via tick_connections if a peer needs it) and host_session_pump discards the host
		// loopback (step 5): there is no local view, so runtime_ stays null — host_pump's fold and the
		// present snapshot both guard on it.
		runtime_.reset();
	}
}

namespace {
// NovaUdpPump-backed netsim::IDatagramSocket — the Godot adapter the shared host owner loop pumps. A
// null/closed pump (pure SP) yields recv 0 / send no-op, so the loop's socket legs go inert exactly as
// the old host_listen_-gated code did. PeerAddr <-> "a.b.c.d" uses the LE octet packing PeerAddr
// documents (octet 0 in the low byte; 127.0.0.1 -> 0x0100007F) — the conversion formerly in
// peer_from_addr / send_datagram.
class NovaUdpPumpDatagramSocket : public opennova::netsim::IDatagramSocket {
public:
	explicit NovaUdpPumpDatagramSocket(NovaUdpPump *pump) : pump_(pump) {}

	int recv_from(uint8_t *buf, std::size_t cap, opennova::PeerAddr &from) override {
		if (pump_ == nullptr || !pump_->is_open()) return 0;
		if (!pump_->has_inbound()) {
			pump_->poll();
			if (!pump_->has_inbound()) return 0;
		}
		const Dictionary d = pump_->take_inbound();
		const String ip = d.get("ip", String());
		const int port = d.get("port", 0);
		const PackedByteArray bytes = d.get("bytes", PackedByteArray());
		uint32_t packed = 0;
		const PackedStringArray parts = ip.split(".");
		if (parts.size() == 4) {
			packed = static_cast<uint32_t>(parts[0].to_int() & 0xFF) |
			         (static_cast<uint32_t>(parts[1].to_int() & 0xFF) << 8) |
			         (static_cast<uint32_t>(parts[2].to_int() & 0xFF) << 16) |
			         (static_cast<uint32_t>(parts[3].to_int() & 0xFF) << 24);
		}
		from = opennova::PeerAddr{packed, static_cast<uint16_t>(port)};
		const std::size_t n = std::min(cap, static_cast<std::size_t>(bytes.size()));
		if (n > 0) std::memcpy(buf, bytes.ptr(), n);
		return static_cast<int>(n);
	}

	void send_to(const opennova::PeerAddr &to, const uint8_t *data, std::size_t len) override {
		if (pump_ == nullptr || !pump_->is_open() || len == 0) return;
		char ipbuf[32];
		std::snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", to.ip & 0xFFu, (to.ip >> 8) & 0xFFu,
		              (to.ip >> 16) & 0xFFu, (to.ip >> 24) & 0xFFu);
		PackedByteArray bytes;
		bytes.resize(static_cast<int64_t>(len));
		std::memcpy(bytes.ptrw(), data, len);
		pump_->send_to(String(ipbuf), to.port, bytes);
	}

private:
	NovaUdpPump *pump_;
};
} // namespace

// P7/A5: the per-frame host owner loop is now a THIN delegation to the shared core host_session_pump
// (libs/npruntime) — the SAME loop apps/nw_server runs, so the headless server and the Godot host can no
// longer drift. NovaSimulation supplies the socket (a NovaUdpPump adapter; SP passes a null pump and the
// loop's socket legs go inert) and folds the host's own loopback 0x0A into ClientState for the present
// pass (serve_and_play: host_session_pump skips the loopback discard so we can read it here).
void NovaSimulation::host_pump() {
	namespace np = opennova::np;
	const uint32_t now = host_owner_.now_tick;
	apply_player_input_pre_tick(); // input -> the host player's body input, before logic (ADR 0009/0012)
	NovaUdpPumpDatagramSocket sock(host_listen_ ? pump_.ptr() : nullptr);
	np::host_session_pump(host_owner_, sock); // recv-drain -> tick_connections -> Server_TickUpdate -> S2C flush
	tick_local_player_view();   // retail promotes the per-frame view before weapon actions
	tick_local_player_weapon(); // the equipped-slot FSM pump, after the view promoter
	if (runtime_) runtime_->Client_ProcessNetworkFrame(now); // fold host_loop_ -> ClientState (HostClient view)
}

// host_pump's dispatch_event + admit_peer were promoted into libs/npruntime (np::dispatch_event /
// np::admit_peer over host_owner_, driven by host_session_pump) — the SAME code apps/nw_server runs, so
// the Godot host and the headless server can no longer drift.

// P7: the per-frame non-authority client loop — the Godot equivalent of the joiner half of
// Client_ProcessNetworkFrame (§5.44). The recv-fold + the C2S 0x0C uplink are fused inside the
// runtime; run_logic_tick(false) runs first so the uplink reflects L's just-integrated pose
// (matches the legacy "uplink-after-tick"). The recv-fold writes only ClientState (read after the
// frame), so folding it after the motor is harmless.
void NovaSimulation::joiner_pump() {
	namespace np = opennova::np;
	if (!runtime_) return;
	const uint32_t now = now_tick_;
	// ClientHello once (Idle -> Hello) the first armed frame.
	if (!joiner_started_) {
		const std::vector<uint8_t> hello = runtime_->start();
		if (!hello.empty()) ship_to_host(hello);
		joiner_started_ = true;
	}
	// Deposit received framed datagrams for this frame's recv pump.
	if (pump_.is_valid()) {
		pump_->poll();
		while (pump_->has_inbound()) {
			const Dictionary d = pump_->take_inbound();
			const PackedByteArray bytes = d.get("bytes", PackedByteArray());
			runtime_->receive(bytes.ptr(), static_cast<std::size_t>(bytes.size()));
		}
	}
	apply_player_input_pre_tick();                  // input -> L's body input
	world_->run_logic_tick(/*is_authority=*/false); // local World tick: moves L's motor ONLY (never Server_TickUpdate)
	tick_local_player_view();   // retail promotes the per-frame view before weapon actions
	tick_local_player_weapon(); // the equipped-slot FSM pump, after the view promoter

	// Run the client frame: recv-fold (-> ClientState) + connect-drive + the C2S 0x0C uplink (gated
	// InMatch && deployed inside the runtime). Build the uplink from L once it exists.
	std::vector<std::vector<uint8_t>> outs;
	const bool have_L = joiner_local_spawned_ && world_->ai && world_->cached.local_player.valid();
	const opennova::world::Entity *e = have_L ? world_->registry.get(world_->cached.local_player) : nullptr;
	const opennova::world::AiEntity *ae = have_L ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
	if (e && ae) {
		const opennova::PlayerExtendedUplink up = opennova::netsim::build_player_uplink(*e, *ae);
		outs = runtime_->Client_ProcessNetworkFrame(up, now);
	} else {
		outs = runtime_->Client_ProcessNetworkFrame(now);
	}
	for (const std::vector<uint8_t> &dg : outs) ship_to_host(dg);

	// On reaching in-match (detected by the recv-fold above): learn H + spawn L at the host-advertised
	// pose. L is the joiner's OWN motor-driven pool-0 entity (publishes cached.local_player); H is the
	// wire identity the host knows us by — the two stay distinct, reconciled by the name-match (§5.38b).
	if (runtime_->in_match() && !joiner_local_spawned_ && world_->ai) {
		joiner_self_wire_handle_ = runtime_->self_handle();
		const np::JoinerConnection::SelfSpawn &sp = runtime_->spawn_pose();
		const opennova::world::PlayerSpawn spawn = spawn_from_self(sp);
		const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
		joiner_local_spawned_ = h.valid();
		player_input_ = opennova::world::PlayerInput{};
		player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(spawn.yaw);
		stance_latch_ = 0;
		look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	}
	++now_tick_;
}

void NovaSimulation::apply_player_input_pre_tick() {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return;
	AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return;
	opennova::world::apply_player_body_input(*p, opennova::world::pack_player_body_input(player_input_));
	// The local-player weapon-channel inputs, refreshed before the body updater runs —
	// the per-tick re-read of the held AdmDefs record kind + the Flags-bit refresh
	// (Flags|0x10 from g_weaponScopeActive; the binoculars bit stays false until a host
	// binoculars input exists). [orig: @ 0x4b5d7f..0x4b5dc0]
	if (p->inf.active) {
		if (weapon_active_) {
			opennova::world::infantry_weapon_switch_stamp(
					p->inf, weapon_anim_map_serial_);
		}
		p->inf.wpn_hold_kind = weapon_active_ ? weapon_hold_kind_ : 0;
		p->inf.scope_raised = weapon_active_ && player_view_.scope_engaged;
		// The run-gait class + ForceCrouch mirror, same per-tick re-read pattern as the
		// hold kind [orig: the selection reads AdmDefs[+0x2B0]+0xAC each pass @ 0x4b72cf;
		// the ForceCrouch checks read the equipped def flags @ 0x4b7245/@ 0x4e0d8a].
		p->inf.wpn_run_anim = weapon_active_ ? weapon_run_anim_ : 0;
		p->inf.wpn_force_crouch = weapon_active_ && weapon_force_crouch_;
	}
}

bool NovaSimulation::spawn_local_player(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!loaded_ || !world_ || !world_->ai) return false;
	// P7: the npruntime listen server auto-spawns the host's own player at bring-up (the faithful §5.0
	// mode-3 path), so an explicit spawn is a no-op success there. The legacy LAN host + any non-listen
	// caller (no auto-spawn) still spawn at the requested pose below.
	if (has_local_player()) return true;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x, -z, y): the inverse of the present (x,y,z) -> (x, z, -y) remap.
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
	spawn.team = static_cast<uint8_t>(p_team);
	if (listen_server_) spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
	if (!h.valid()) return false;
	// Seed the look heading to the spawn facing so the body starts aligned. [(90 - yaw) BAM]
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(p_yaw_deg);
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	return true;
}

int NovaSimulation::spawn_local_player_at_start() {
	if (!loaded_ || !world_ || !world_->ai) return -1;
	// P7: the npruntime listen server auto-spawns the host's own player at bring-up via the SAME
	// select_player_spawn start-marker scan (Server_BuildPlayerInfoAndAdd), so when a player already
	// exists this is a no-op success (the player is at its start, input seeded by bringup_host_runtime).
	if (has_local_player()) return 1;
	// Pick the player-start marker the original would — scan the 60xx start-marker family (SP/DM,
	// coop, team), FARTHEST from the enemy set — instead of the first NPC's position. Finds the
	// authored start whatever the mission mode (e.g. a 6001-only SP training mission like 00TRa).
	// [orig: Server_PositionPlayerForSpawn @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0; net-re §5.2c]
	const opennova::world::SpawnPointResult sel = opennova::world::select_player_spawn(*world_);
	opennova::world::PlayerSpawn spawn;
	if (sel.found) {
		spawn.position = sel.position; // mission space, straight from the chosen marker
		spawn.yaw = sel.yaw;
	} else {
		// No player-start marker authored: spawn at the mission origin (the terrain clamp grounds
		// it). NEVER fall back to an NPC's position — that is the bug this replaces.
		spawn.position = {0.0f, 0.0f, 0.0f};
		spawn.yaw = 0;
	}
	// SP keeps the player's own team; the marker's team is not copied [orig: §5.2c]. Team 1 mirrors
	// the prior placeholder until the MP team path lands.
	spawn.team = 1;
	if (listen_server_) spawn.min_entity_slot = kRetailPlayerMinEntitySlot;
	const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
	if (!h.valid()) return -1;
	// Seed the look heading to the spawn facing so the body starts aligned. [(90 - yaw) BAM]
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(spawn.yaw);
	stance_latch_ = 0;
	look_px_accum_x_ = look_px_accum_y_ = 0.0f;
	return sel.found ? 1 : 0;
}

bool NovaSimulation::has_local_player() const {
	return world_ && world_->cached.local_player.valid();
}

int NovaSimulation::get_local_player_wire_handle() const {
	// The handle the wire stream knows the local player by. On a JOINER that is H (the
	// host-assigned wire identity), NOT the local sim handle L — L lives in the joiner's
	// own pool and collides with a host-side slot (e.g. the host player), so excluding L
	// from the wire present would wrongly hide a remote entity. On the host, the local
	// player's own pool-0 handle IS its wire handle.
	if (joiner_) return static_cast<int>(joiner_self_wire_handle_);
	return (world_ && world_->cached.local_player.valid())
			? static_cast<int>(world_->cached.local_player.packed) : 0;
}

void NovaSimulation::set_player_input(bool p_forward, bool p_back, bool p_left, bool p_right,
                                      bool p_lean_left, bool p_lean_right, bool p_jump) {
	player_input_.forward = p_forward;
	player_input_.back = p_back;
	player_input_.left = p_left;
	player_input_.right = p_right;
	// Lean keys -> MoveOrder bits 6/7 [orig: g_inputFlags 0x2000/0x4000 packed
	// @ 0x4df708-0x4df741]; jump is a per-frame edge the motor consumes once grounded.
	player_input_.lean_left = p_lean_left;
	player_input_.lean_right = p_lean_right;
	player_input_.jump = p_jump;
	// Stance comes from the sim-owned SELECT latches (request_local_player_stance —
	// the C2S 0x1D apply semantics [orig: @ 0x501c60]).
	player_input_.crouch = (stance_latch_ == 1);
	player_input_.prone = (stance_latch_ == 2);
	// The movement-held latch and the unscope-on-move [orig:
	// Player_PackInputStateToEntity @ 0x4df450 — any of the four direction keys
	// sets byte_B7653B (blocks scope-UP on Scoped weapons @ 0x4df29c) and, while
	// SETTLED at scope on a Scoped (flags 1) weapon, routes through
	// Player_ToggleWeaponScope @ 0x4df4c9..0x4df4ec = the full unscope. The
	// toggle's ForceScoped pin (@ 0x4df12d) keeps pinned sights raised].
	const bool move_held = p_forward || p_back || p_left || p_right;
	if (opennova::world::player_view_move_input(player_view_, move_held,
			weapon_active_ ? weapon_def_.flags : 0) &&
			(weapon_def_.flags & 0x20000000) == 0) {
		if (opennova::world::player_view_set_engaged(player_view_, false,
				(weapon_def_.flags2 & 0x200) != 0))
			opennova::world::weapon_fsm_queue_scope_down(weapon_slot_);
	}
}

void NovaSimulation::add_local_player_look(float p_dx_px, float p_dy_px) {
	// The scoped sensitivity reduction divides by the CURRENT zoom magnification —
	// the slot zoom seeded from the def's scope_max_mag [orig: sens /
	// Player_GetClampedWeaponElevation() @ 0x499714, applied while scoped and the
	// binocular view is down; no binoculars input exists yet]. Engaged-at-scope is
	// the sim's own bit; the zoom-adjust keys are an unported tail, so the seed
	// (scope_max_mag) IS the current zoom.
	int32_t scoped_zoom = 0;
	if (weapon_active_ && player_view_.scope_engaged && weapon_scope_max_mag_ > 1.0f)
		scoped_zoom = static_cast<int32_t>(weapon_scope_max_mag_);
	const bool prone = (stance_latch_ == 2); // [orig: MoveOrder & 0x100 @ 0x4e0ff7]
	// Godot supplies float relative motion; the original consumes whole center-lock
	// pixels. Accumulate the fraction so slow motion is not truncated away.
	look_px_accum_x_ += p_dx_px;
	look_px_accum_y_ += p_dy_px;
	const int32_t dx = static_cast<int32_t>(look_px_accum_x_);
	const int32_t dy = static_cast<int32_t>(look_px_accum_y_);
	look_px_accum_x_ -= static_cast<float>(dx);
	look_px_accum_y_ -= static_cast<float>(dy);
	if (dx == 0 && dy == 0) return;
	opennova::world::player_look_apply(player_input_.look_heading, player_input_.look_pitch,
	                                   look_settings_, dx, dy, scoped_zoom, prone);
}

void NovaSimulation::set_local_player_mouse(int p_sensitivity, bool p_invert_y) {
	// The mousescale clamp [orig: @ 0x49b19b-0x49b1b9: >= 0x200 -> 0x1FF, <= 0 -> 1].
	int s = p_sensitivity;
	if (s < opennova::world::kMouseSensitivityMin) s = opennova::world::kMouseSensitivityMin;
	if (s > opennova::world::kMouseSensitivityMax) s = opennova::world::kMouseSensitivityMax;
	look_settings_.sensitivity = s;
	look_settings_.invert_y = p_invert_y;
}

bool NovaSimulation::request_local_player_stance(int p_stance) {
	if (p_stance < 0 || p_stance > 2) return false;
	// ForceCrouch weapons refuse stance changes [orig: the case-169/170/172 gate
	// Entity_CheckWeaponSeatFlags(equipped, 0x40000) @ 0x4e0d8a; the seat-kind-3
	// mount refusal rides the unported mounting slice].
	if (weapon_active_ && weapon_force_crouch_) return false;
	if (stance_latch_ == p_stance) return false;
	// SELECT with mutual exclusion — the 0x1D apply writes one stance bit and clears
	// the other [orig: NapiNPServerMsg_HandleStanceChange @ 0x501c60: 169 -> crouch,
	// 170 -> prone, 172 -> clear both].
	stance_latch_ = p_stance;
	player_input_.crouch = (stance_latch_ == 1);
	player_input_.prone = (stance_latch_ == 2);
	return true;
}

Vector3 NovaSimulation::get_local_player_position() const {
	if (!world_ || !world_->cached.local_player.valid()) return Vector3();
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return Vector3();
	// mission (x,y,z) -> Godot (x, z, -y).
	return Vector3(e->position.x, e->position.z, -e->position.y);
}

float NovaSimulation::get_local_player_yaw_deg() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return 0.0f;
	// Engine heading (BAM32) -> mission yaw degrees, the (90 - heading) convention.
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(p->heading));
}

float NovaSimulation::get_local_player_pitch_deg() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0.0f;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return 0.0f;
	return static_cast<float>(static_cast<double>(p->pitch) * opennova::world::kDegreesPerBam);
}

int NovaSimulation::get_local_player_body_anim_slot() const {
	if (!world_ || !world_->cached.local_player.valid()) return -1;
	// The same Entity.body_anim_slot the present pass reads for NPC models (written by the
	// infantry motor mirror, infantry.cpp). The avatar is host-managed and not in the present
	// registry, so main_game drives its body clip from this getter.
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? e->body_anim_slot : -1;
}

String NovaSimulation::get_local_player_anim_key() const {
	// The local player's full anim-state clip key ("anim_<name>"), straight from the motor's
	// selected state. Unlike the 8-slot BodyAnim enum (get_local_player_body_anim_slot), this carries
	// stance + jump (anim_idle_crouch / anim_walk_prone_forward / anim_jump_loop / ...), so
	// main_game drives the 3rd-person avatar via play_body_clip(key) for full stance fidelity.
	// [orig: off_8135F0 names ARE the .adm keys without the "anim_" prefix]
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return String();
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return String();
	return infantry_anim_key(p->inf.anim_state);
}

int NovaSimulation::get_local_player_anim_phase_ticks() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 0;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	return p ? p->inf.clip_phase : 0;
}

Dictionary NovaSimulation::get_local_player_aim_overlay() const {
	// The torso-bend overlay state: the nine per-segment orientations from the exact BAM
	// blends [orig: Entity_BuildBoneTransformMatrices @0x4b1290; world-wac-ai-re.md §14],
	// converted once here to mission-euler degrees — yaw via the canonical (90 - heading),
	// pitch NEGATED (engine BAM pitch is up-positive, BMS euler pitch is nose-down-positive
	// per MissionObjectPlacer.bms_to_godot_basis). The host builds Godot bases from these
	// with that single-sourced conversion; delta(body class) is identity by construction.
	Dictionary out;
	out["valid"] = false;
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return out;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p) return out;

	opennova::anim::AimOverlayInputs in;
	in.aim_yaw = p->heading;
	in.aim_pitch = p->pitch;
	in.body_yaw = p->inf.body_heading;
	in.leg_yaw_r = p->inf.leg_yaw[0];
	in.leg_yaw_l = p->inf.leg_yaw[1];
	// The head-look decay term carries the arms-dip feed (the +0x371 weapon-switch
	// window drops it 0x2800000/tick; infantry_weapon_channel owns the decay)
	// [orig: @ 0x4b5cab..0x4b5cd5]. The lean term is the sim's lean angle
	// (entity+0xB0; ramp/decay in infantry_lean_tick); roll is the slope-conform
	// visual roll (entity+0x18) and torso_roll its sixteenth-step chaser
	// (entity+0x2DC, infantry_torso_roll_tick); body_pitch is the slope-conform
	// body pitch (entity+0x90; both fed by infantry_slope_pass). pitch_blend
	// stays 0 until its sim source (recoil impulses) is ported — the formulas
	// carry the term so it drops in without touching this seam.
	in.head_look_decay = p->inf.head_look_decay;
	in.lean = p->inf.lean_angle;
	in.roll = p->roll;
	in.body_pitch = p->body_pitch;
	in.torso_roll = p->inf.torso_roll;
	in.aim_state = (opennova::world::infantry_anim_flags(p->inf.anim_state) & 0x40u) != 0;
	in.rolling = (p->inf.anim_state == opennova::world::anim_state::kRollLeft ||
	              p->inf.anim_state == opennova::world::anim_state::kRollRight);

	opennova::anim::AimOverlayAngles angles[opennova::anim::kOverlayClassCount];
	opennova::anim::compute_aim_overlay_angles(in, angles);

	const auto to_mission = [](const opennova::anim::AimOverlayAngles &a) {
		return Vector3(
				static_cast<float>(-static_cast<double>(a.pitch) * opennova::world::kDegreesPerBam),
				static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(a.yaw)),
				static_cast<float>(static_cast<double>(a.roll) * opennova::world::kDegreesPerBam));
	};

	PackedVector3Array packed;
	packed.resize(opennova::anim::kOverlayClassCount);
	for (int i = 0; i < opennova::anim::kOverlayClassCount; ++i) {
		packed[i] = to_mission(angles[i]);
	}
	out["valid"] = true;
	out["aim_state"] = in.aim_state;
	out["body"] = to_mission(angles[opennova::anim::kOverlayBody]);
	out["angles"] = packed;
	return out;
}

// --- the local player's equipped-weapon FSM (net-re §5.62) --------------------------

NovaSimulation::WeaponClipRing *NovaSimulation::weapon_ring_for(const String &p_key_lower) {
	for (std::pair<String, WeaponClipRing> &kv : weapon_clip_rings_) {
		if (kv.first == p_key_lower) return &kv.second;
	}
	return nullptr;
}

float NovaSimulation::weapon_ring_take_length(const char *p_key) {
	// Serve the ring head's duration, then advance the head — the consuming read
	// [orig: Anim_GetDurationTicks @ 0x53ee10: currentEntry = *slot;
	//  *slot = *(currentEntry + 36); duration from currentEntry's data].
	WeaponClipRing *ring = weapon_ring_for(String::utf8(p_key).to_lower());
	if (ring == nullptr || ring->lengths.is_empty()) return -1.0f;
	const float served = ring->lengths[ring->head];
	ring->head = (ring->head + 1) % static_cast<int>(ring->lengths.size());
	return served;
}

int NovaSimulation::weapon_ring_take_variant(const String &p_key) {
	// Serve the head as the PLAYED variant and advance — the play latch: playback
	// follows the served entry while the ring moves on [orig: AnimMap_PlayAnimBySlot
	// @ 0x40bda0: animEntry = slot[i]; slot[i] = next; animState+68 = animEntry].
	WeaponClipRing *ring = weapon_ring_for(p_key.to_lower());
	if (ring == nullptr || ring->lengths.is_empty()) return 0;
	const int served = ring->head;
	ring->head = (ring->head + 1) % static_cast<int>(ring->lengths.size());
	return served;
}

void NovaSimulation::set_local_player_weapon(const Dictionary &p_def,
		const Dictionary &p_clip_seconds) {
	using opennova::world::WeaponFsmActionRow;
	// A mount is a new presentation epoch: no payload from the previous weapon may
	// cross this seam, even though its strings were copied into the pending records.
	pending_weapon_events_.clear();
	// Mirror the weapon dict's ACTION rows into the def-agnostic bake inputs.
	std::vector<WeaponFsmActionRow> rows;
	const Array actions = p_def.get("actions", Array());
	rows.reserve(static_cast<size_t>(actions.size()));
	for (int i = 0; i < actions.size(); ++i) {
		const Dictionary a = actions[i];
		WeaponFsmActionRow row;
		const CharString name = String(a.get("name", "")).utf8();
		const CharString anim = String(a.get("anim", "")).utf8();
		const CharString function = String(a.get("function", "")).utf8();
		snprintf(row.name, sizeof(row.name), "%s", name.get_data());
		snprintf(row.anim, sizeof(row.anim), "%s", anim.get_data());
		snprintf(row.function, sizeof(row.function), "%s", function.get_data());
		row.delaystart = static_cast<int32_t>(int64_t(a.get("delaystart", -1)));
		row.delayend = static_cast<int32_t>(int64_t(a.get("delayend", -1)));
		// The audio/effect legs ride the bake into the pool entries
		// [orig: ActionDef_ParseScriptLine @ 0x4023c0 rows].
		const CharString soundset = String(a.get("soundset", "")).utf8();
		const CharString soundsetend = String(a.get("soundsetend", "")).utf8();
		const CharString particle = String(a.get("particle", "")).utf8();
		const CharString userpoint = String(a.get("particleuserpoint", "")).utf8();
		snprintf(row.soundset, sizeof(row.soundset), "%s", soundset.get_data());
		snprintf(row.soundsetend, sizeof(row.soundsetend), "%s", soundsetend.get_data());
		snprintf(row.particle, sizeof(row.particle), "%s", particle.get_data());
		snprintf(row.particleuserpoint, sizeof(row.particleuserpoint), "%s", userpoint.get_data());
		rows.push_back(row);
	}
	// Clip lengths come from the loaded viewmodel's .adm (seconds) as per-key VARIANT
	// arrays; they seed the slot rings the bake and the play events consume
	// serve-then-advance [orig: the animState slot heads (+72) built by
	// AnimMap_RegisterBoneNode @ 0x40c2d0; Anim_GetDurationTicks @ 0x53ee10].
	weapon_clip_rings_.clear();
	weapon_anim_variant_ = 0;
	{
		const Array keys = p_clip_seconds.keys();
		for (int i = 0; i < keys.size(); ++i) {
			WeaponClipRing ring;
			const Variant v = p_clip_seconds[keys[i]];
			if (v.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
				ring.lengths = v;
			} else {
				// Single-variant convenience: a plain number is a one-entry ring.
				ring.lengths.push_back(static_cast<float>(double(v)));
			}
			if (ring.lengths.is_empty()) continue;
			weapon_clip_rings_.emplace_back(String(keys[i]).to_lower(), ring);
		}
	}
	// The bake probes existence as a pure lookup and reads durations ring-wise —
	// one consuming read per 'auto' field [orig: Anim_InitActions @ 0x541fa0;
	// the lookup @ 0x5421ae, the reads @ 0x5421c5 / @ 0x5421d8].
	const auto resolve_fn = [](void *p_ctx, const char *key) -> int {
		NovaSimulation *self = static_cast<NovaSimulation *>(p_ctx);
		return self->weapon_ring_for(String::utf8(key).to_lower()) != nullptr ? 1 : 0;
	};
	const auto clip_fn = [](void *p_ctx, const char *key) -> float {
		return static_cast<NovaSimulation *>(p_ctx)->weapon_ring_take_length(key);
	};
	weapon_def_ = opennova::world::WeaponFsmDef{};
	opennova::world::weapon_fsm_bake(rows.data(), rows.size(), resolve_fn, clip_fn, this,
			weapon_def_);
	const int flags = int(p_def.get("flags", 0));
	weapon_def_.auto_fire = (flags & 0x100) != 0; // [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0]
	weapon_def_.burst3 = (flags & 0x20) != 0;     // [orig: WeaponAction_Fire @ 0x542c8a]
	weapon_def_.flags = flags;                    // raw mask: the scope gate + fov policy read it
	weapon_def_.flags2 = int(p_def.get("flags2", 0)); // Inset (0x200) picks the 7-step ease
	weapon_scope_max_mag_ = float(double(p_def.get("scope_max_mag", 0.0)));
	// The 3P body-channel kinds [orig: weapon.def special_hold/attack_anim -> the
	// AdmDefs record +0xA4/+0xA8; world-wac-ai-re.md §14.8.4].
	weapon_hold_kind_ = int(int64_t(p_def.get("special_hold", 0)));
	weapon_attack_kind_ = int(int64_t(p_def.get("attack_anim", 0)));
	// The run-gait class [orig: 'run_anim' -> AdmDefs +0xAC; promotion @ 0x4b729d] and
	// ForceCrouch (0x40000): idle_mortar promotion + stance-change refusal.
	weapon_run_anim_ = int(int64_t(p_def.get("run_anim", 0)));
	weapon_force_crouch_ = (flags & 0x40000) != 0;
	// A held-AnimMap CHANGE advances a host serial; the local InfantryState observes
	// that edge pre-tick and stamps its own 20-tick arms-dip window. Compare the
	// resolved map identity, not the weapon name: two weapon records sharing one
	// AnimMap do NOT dip. A fresh mount advances even when the map key is empty.
	// [orig: previous/current AdmDefs record +0 comparison @0x4b46d0..0x4b4701].
	const String anim_map = p_def.get("animadm", String());
	if (!weapon_active_ || anim_map.nocasecmp_to(weapon_anim_map_) != 0) {
		weapon_anim_map_ = anim_map;
		++weapon_anim_map_serial_;
		if (weapon_anim_map_serial_ == 0) ++weapon_anim_map_serial_; // reserve 0 = none
	}
	const int clipsize = int(p_def.get("clipsize", 0));
	weapon_def_.clip_capacity = clipsize > 0 ? clipsize : -1; // no clipsize key = no clip tracking
	// Fresh slot: full magazine + the def's carried reserve (the interim ammo default
	// until PLAYER_INFO loadout resolution lands — D-WPN-7).
	weapon_slot_ = opennova::world::WeaponSlotState{};
	weapon_slot_.clip = clipsize > 0 ? clipsize : 0;
	weapon_slot_.reserve = int(p_def.get("startrounds", 0));
	weapon_play_serial_ = 0;
	weapon_anim_key_ = String();
	weapon_anim_variant_ = 0;
	weapon_anim_tick_ = 0;
	weapon_fired_serial_ = weapon_dry_serial_ = weapon_reload_serial_ = 0;
	weapon_unscope_serial_ = weapon_rescope_serial_ = 0;
	weapon_action_serial_ = 0;
	weapon_action_started_ = -1;
	weapon_action_end_serial_ = 0;
	weapon_action_finished_ = -1;
	weapon_fire_held_ = weapon_fire_pressed_ = weapon_reload_pressed_ = false;
	// A fresh mount starts at the hip with the interp cleared and the hipfire
	// latch reset [orig: Player_MountWeaponSlot zeroes the view biases @ 0x4dfbcf].
	player_view_.scope_engaged = false;
	player_view_.scope_step = 0;
	player_view_.ease_steps = opennova::world::kScopeEaseSteps;
	player_view_.scope_hipfire = true;
	weapon_active_ = true;
}

void NovaSimulation::clear_local_player_weapon() {
	weapon_active_ = false;
	pending_weapon_events_.clear();
	weapon_fire_held_ = false;
	weapon_fire_pressed_ = false;
	weapon_reload_pressed_ = false;
	weapon_clip_rings_.clear();
	weapon_anim_variant_ = 0;
	weapon_anim_tick_ = 0;
	player_view_.scope_engaged = false;
	player_view_.scope_step = 0;
	player_view_.ease_steps = opennova::world::kScopeEaseSteps;
	player_view_.scope_hipfire = true;
	weapon_hold_kind_ = 0;
	weapon_attack_kind_ = 0;
	weapon_run_anim_ = 0;
	weapon_force_crouch_ = false;
	weapon_anim_map_ = String();
}

void NovaSimulation::set_local_player_weapon_input(bool p_fire_held, bool p_fire_pressed,
		bool p_reload_pressed) {
	weapon_fire_held_ = p_fire_held;
	weapon_fire_pressed_ = weapon_fire_pressed_ || p_fire_pressed; // latch until consumed
	weapon_reload_pressed_ = weapon_reload_pressed_ || p_reload_pressed;
}

bool NovaSimulation::request_local_player_scope_toggle() {
	// [orig: input case 6 @ 0x4e0420 gates currentAction not in {RELOAD, SWITCHFROM};
	//  Player_ToggleWeaponScope @ 0x4df0c0 gates def Flags & 3, flips g_scopeEngaged
	//  @ 0x82CE94, and queues the scopeup/scopedown FSM state @ 0x53f050/0x53f080]
	if (!weapon_active_) return false;
	if (!opennova::world::weapon_fsm_scope_toggle_allowed(weapon_def_, weapon_slot_)) return false;
	// Scope-UP is refused while a movement key is held on a Scoped weapon
	// [orig: byte_B7653B && (flags & 1) -> return @ 0x4df29c].
	if (!player_view_.scope_engaged &&
			opennova::world::player_view_scope_up_blocked(player_view_, weapon_def_.flags))
		return false;
	// ForceScoped pins the raised sight: un-scoping is refused once settled
	// [orig: (flags1 & 0x20000000) == 0 || !g_weaponScopeActive @ 0x4df12d].
	if (player_view_.scope_engaged && (weapon_def_.flags & 0x20000000) != 0 &&
			!opennova::world::player_view_scope_ease_active(player_view_))
		return false;
	// The toggle latches this ease's step count (7 for Inset weapons, else 15;
	// 1 on the hipfire-return leg) and REFUSES while the previous ease runs
	// [orig: Player_ToggleWeaponScope @ 0x4df177 !activeFlag; Setup @ 0x4df1b3..0x4df36e].
	if (!opennova::world::player_view_set_engaged(player_view_, !player_view_.scope_engaged,
			(weapon_def_.flags2 & 0x200) != 0))
		return false;
	if (player_view_.scope_engaged)
		opennova::world::weapon_fsm_queue_scope_up(weapon_slot_);
	else
		opennova::world::weapon_fsm_queue_scope_down(weapon_slot_);
	return true;
}

void NovaSimulation::set_local_player_camera_third_person(bool p_third_person) {
	player_view_.third_person = p_third_person; // [orig: g_camera_mode @ 0xA890C8]
}

// One 62.5 Hz tick of the view state, before the weapon pump: the ADS ease and the
// third-person anchor chase run at the WORLD cadence, so camera lag is identical at
// any render rate. Retail's Player_UpdatePerFrame call precedes the later
// WeaponAction_ProcessAllEntities call, so this tick's settle promoter is visible to
// action routing while an action's unscope/rescope begins easing on the next tick
// [orig: call sites @ 0x42c18e / @ 0x526786; promoter @ 0x4de4f7].
void NovaSimulation::tick_local_player_view() {
	if (!world_ || !world_->cached.local_player.valid()) {
		player_view_ = opennova::world::PlayerViewState{};
		return;
	}
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return;
	// The anchor-chase target is Position + CameraOffset — the posed head-bone eye
	// [orig: ThirdPersonCamera_Update @ 0x437b70..76], fed by the host's per-frame
	// skeleton sample (see local_eye_mission_). Without a sample: Position + 1.0,
	// the witnessed NON-person bump [orig: @ 0x437e8f].
	const float eye[3] = {
		local_eye_valid_ ? local_eye_mission_[0] : e->position.x,
		local_eye_valid_ ? local_eye_mission_[1] : e->position.y,
		local_eye_valid_ ? local_eye_mission_[2] : e->position.z + 1.0f,
	};
	opennova::world::player_view_tick(player_view_, eye);
}

void NovaSimulation::set_local_player_eye(const Vector3 &p_eye_godot, bool p_valid) {
	// Godot (x, y, z) -> mission (x, -z, y), the get_local_player_position inverse.
	local_eye_mission_[0] = p_eye_godot.x;
	local_eye_mission_[1] = -p_eye_godot.z;
	local_eye_mission_[2] = p_eye_godot.y;
	local_eye_valid_ = p_valid;
}

Dictionary NovaSimulation::get_local_player_view() const {
	Dictionary out;
	out["scope_engaged"] = player_view_.scope_engaged;
	const opennova::world::Entity *local = world_ != nullptr
			? world_->registry.get(world_->cached.local_player)
			: nullptr;
	out["mounted"] = local != nullptr && local->mounted;
	// Structural proxy for Player_IsVehicleHasAttackCapability until mounted
	// weapon inventory is modeled: these seat classes replace the on-foot
	// upper-body weapon channel; passenger seats do not.
	out["vehicle_attack_context"] = local != nullptr && mount_blocks_weapon_channel(*local);
	out["scope_fraction"] = opennova::world::player_view_scope_fraction(player_view_);
	// The NoCardSwitch reload rule: while the equipped slot is mid-RELOAD on a
	// weapon WITHOUT NoCardSwitch (flags 0x2000000), the FP camera drops the ADS
	// view bias for the frame — the host reads the eased fraction as 0.
	// [orig: Player_UpdateFirstPersonCamera @ 0x4dd439/@ 0x4dd4cc; the same
	//  predicate is Player_IsReloadingCardSwitchWeapon @ 0x4dcdd0 (ex kong
	//  "Player_IsDriverInVehicle"), whose one caller refuses fire @ 0x5cf7be]
	out["suppress_view_bias"] = weapon_active_ &&
			weapon_slot_.current == opennova::world::weapon_action::kReload &&
			(weapon_def_.flags & 0x2000000) == 0;
	// The scope-card switch: a Scoped (flags 0x1) weapon at FULL raise in first person
	// draws the SIGHTS card INSTEAD of the FP viewmodel — the frame draws one or the
	// other, never both. [orig: Player_IsEquippedWeaponScoped @ 0x4dcc80 (Flags & 1 &&
	// g_weaponScopeActive) routes the frame to draw_weapon_sight_overlays @ 0x4dce00;
	// the FP model call @ 0x5d822c requires both scope gates CLEAR @ 0x5d8212..0x5d8218]
	out["scope_card_active"] = weapon_active_ && (weapon_def_.flags & 1) != 0 &&
			player_view_.scope_engaged && !player_view_.third_person &&
			!opennova::world::player_view_scope_ease_active(player_view_);
	out["fov_h_deg"] = opennova::world::player_view_fov_h_deg(player_view_,
			weapon_active_ ? weapon_def_.flags : 0,
			weapon_active_ ? weapon_scope_max_mag_ : 0.0f);
	// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position map.
	out["tp_anchor"] = Vector3(player_view_.tp_anchor[0], player_view_.tp_anchor[2],
			-player_view_.tp_anchor[1]);
	out["tp_anchor_valid"] = player_view_.tp_anchor_valid;
	// The FP camera roll in degrees: roll = torsoRoll + lean/4 [orig: the on-foot
	// person leg @ 0x437fe6 — g_view_rot_roll = entity+0x2DC + (entity+0xB0 >> 2)].
	{
		float fp_roll_deg = 0.0f;
		if (world_ && world_->ai && world_->cached.local_player.valid()) {
			if (const AiEntity *p = world_->ai->for_handle(world_->cached.local_player)) {
				const int32_t roll_bam = opennova::io::bam_add(
						p->inf.torso_roll, opennova::io::bam_sar(p->inf.lean_angle, 2));
				fp_roll_deg = static_cast<float>(
						static_cast<double>(roll_bam) * opennova::world::kDegreesPerBam);
			}
		}
		out["fp_roll_deg"] = fp_roll_deg;
	}
	return out;
}

float NovaSimulation::fov_vertical_from_horizontal(float p_fov_h_deg, float p_aspect) {
	return opennova::world::fov_vertical_from_horizontal_deg(p_fov_h_deg, p_aspect);
}

// One 62.5 Hz pump of the local player's slot, after the world logic tick
// [orig: WeaponAction_ProcessAllEntities @ 0x542690 pumps every pooled entity in the
// frame loop; local-player-only here — D-WPN-6].
void NovaSimulation::tick_local_player_weapon() {
	if (!weapon_active_ || !world_ || !world_->cached.local_player.valid()) return;
	opennova::world::WeaponFsmInputs in;
	in.fire_held = weapon_fire_held_;
	in.fire_pressed = weapon_fire_pressed_;
	// The dispatch gate runs here now: the raw reload edge is refused on a full
	// magazine or an empty reserve [orig: input case 0xD3 @ 0x4e0420].
	in.reload_pressed = weapon_reload_pressed_ &&
			opennova::world::weapon_fsm_reload_allowed(weapon_def_, weapon_slot_);
	in.is_local = true;
	in.is_authority = !joiner_; // the joiner defers the refill to the §5.58 round-trip
	in.auto_reload = true;      // [orig: g_autoReloadEnabled @ 0x24D2118, default on]
	// The weapon FSM consumes the promoted/settled scope bit, not the raw
	// requested-engagement bit. Player_UpdatePerFrame runs before the weapon
	// pump in retail and only promotes g_weaponScopeActive after the ease has
	// completed [orig: promoter @ 0x4de4f7; weapon pump @ 0x526786].
	in.scope_active = player_view_.scope_engaged &&
			!opennova::world::player_view_scope_ease_active(player_view_);
	opennova::world::WeaponFsmEvents ev;
	opennova::world::weapon_fsm_tick(weapon_def_, weapon_slot_, in, ev);
	weapon_fire_pressed_ = false; // edges consume on the first tick of the frame
	weapon_reload_pressed_ = false;
	PendingWeaponEvent pending;
	pending.tick = world_->logic_tick;
	bool has_presentation_event = false;
	if (ev.play_anim) {
		++weapon_play_serial_;
		weapon_anim_key_ = String::utf8(ev.anim_key);
		weapon_anim_tick_ = world_->logic_tick;
		// The play consumes the slot ring and latches the served variant — the host
		// plays exactly this variant on every viewmodel part
		// [orig: AnimMap_PlayAnimBySlot @ 0x40bda0 advances the head and latches
		//  the served entry at animState+68].
		weapon_anim_variant_ = weapon_ring_take_variant(weapon_anim_key_);
		pending.anim_key = weapon_anim_key_;
		pending.anim_variant = weapon_anim_variant_;
		has_presentation_event = true;
	}
	if (ev.action_started >= 0) {
		// Copy the begin leg while this def is mounted; a later weapon switch cannot
		// change the queued sound/effect payload.
		// [orig: ActionSlot_ExecuteActionWithEffect @ 0x541860].
		++weapon_action_serial_;
		weapon_action_started_ = ev.action_started;
		pending.action_started = ev.action_started;
		if (ev.action_started < opennova::world::weapon_action::kCount) {
			const opennova::world::WeaponFsmAction &act = weapon_def_.actions[ev.action_started];
			pending.action_soundset = String::utf8(act.soundset);
			pending.action_particle = String::utf8(act.particle);
			pending.action_particle_userpoint = String::utf8(act.particle_userpoint);
		}
		has_presentation_event = true;
	}
	if (ev.action_finished >= 0) {
		// The END leg: the finished action's soundsetend — fire rows carry the gunshot
		// here, reload rows the completion sound
		// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim @ 0x401100].
		++weapon_action_end_serial_;
		weapon_action_finished_ = ev.action_finished;
		pending.action_finished = ev.action_finished;
		if (ev.action_finished < opennova::world::weapon_action::kCount) {
			pending.action_end_soundset =
					String::utf8(weapon_def_.actions[ev.action_finished].soundsetend);
		}
		has_presentation_event = true;
	}
	if (ev.action_effect >= 0 && ev.action_effect < opennova::world::weapon_action::kCount) {
		// The recoil-row DIRECT effect leg — casing eject / bolt smoke at the arbiter
		// tick. Copied like the begin leg so a weapon switch cannot swap the payload.
		// [orig: WeaponAction_Recoil @ 0x542dd0 spawn @ 0x542f64]
		const opennova::world::WeaponFsmAction &act = weapon_def_.actions[ev.action_effect];
		pending.action_effect = ev.action_effect;
		pending.effect_particle = String::utf8(act.particle);
		pending.effect_particle_userpoint = String::utf8(act.particle_userpoint);
		has_presentation_event = true;
	}
	// Preserve the retail call order within one pump: clip start, begin leg, then
	// finish leg. Records themselves stay in logic-tick order until the host drains.
	if (has_presentation_event) {
		pending.world_position = get_local_player_position();
		pending.scope_settled = player_view_.scope_engaged &&
				!opennova::world::player_view_scope_ease_active(player_view_);
		pending.third_person = player_view_.third_person;
		const opennova::world::Entity *local =
				world_->registry.get(world_->cached.local_player);
		pending.vehicle_attack_context =
				local != nullptr && mount_blocks_weapon_channel(*local);
		pending_weapon_events_.push_back(std::move(pending));
	}
	if (ev.fired) {
		++weapon_fired_serial_;
		// The 3P body attack stamp — knife/grenade kinds only; rifle fire stamps NO body
		// state (the FP clip plays on the weapon adm channel, and the fire path's only
		// other anim side effect drives the .3di control registers)
		// [orig: WeaponAction_Fire @ 0x542bbc..0x542bea; ActionSlot_TryAllocCtrlRegAnim
		//  @ 0x401f00 -> dword_83FCE8].
		AiEntity *p = world_->ai ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
		if (p && p->inf.active)
			opennova::world::infantry_weapon_attack_stamp(p->inf, weapon_attack_kind_);
		// Local/SP fire already passed the same FSM/ammo authority that the remote
		// C2S 0x06 handler validates. Append the host's round-ring record and spawn
		// the authoritative projectile here; the loopback server handler correctly
		// ignores this player because retail's local action has already done both.
		// [orig: WeaponAction_Fire @ 0x542c5e ->
		// Entity_FireWeaponAndSendPacket @ 0x42bd80 local re-entry ->
		// RoundData_AddRound @ 0x4fdb40 inline RoundData_SpawnRound @ 0x4ec0d0]
		opennova::world::Entity *shooter =
				world_->registry.get(world_->cached.local_player);
		if (!joiner_ && shooter != nullptr && p != nullptr) {
			const uint8_t adm_index = shooter->equipped_adm_index;
			const opennova::world::WeaponTableEntry *adm =
					world_->weapons.by_index(adm_index);
			if (adm != nullptr && adm->ammo_index >= 0) {
				opennova::world::Vec3 origin = shooter->position;
				if (local_eye_valid_) {
					origin.x = local_eye_mission_[0];
					origin.y = local_eye_mission_[1];
					origin.z = local_eye_mission_[2];
				} else {
					origin.z += 1.0f;
				}
				// The round bearing frame IS the engine heading frame: RoundSim's
				// (cos, sin) mission-axis mapping is wire-validated on the 0x06 yaw
				// BAM (round_sim.cpp spawn, D-NET-153), and the retail spawner runs
				// raw descriptor angles through sin/cos [orig: RoundData_SpawnRound
				// trig @ 0x4ec511..0x4ec5fb]. Applying the (90 - heading)
				// mission-yaw flip here mirrored every local shot across the NE
				// diagonal (impacts landed 90 deg off the aim ray - the
				// fp_impact_probe pin; the same mistake D-NET-153 records for the
				// wire leg).
				const int32_t dir_yaw = p->heading;
				const int32_t dir_pitch = p->pitch;
				local_round_sequence_ =
						static_cast<uint16_t>(local_round_sequence_ + 1u);
				const uint16_t shot_seq = local_round_sequence_;

				opennova::world::RoundEvent round_event;
				round_event.shooter_handle = world_->cached.local_player.packed;
				round_event.origin_x = static_cast<int32_t>(
						std::lround(double(origin.x) * kFixed16));
				round_event.origin_y = static_cast<int32_t>(
						std::lround(double(origin.y) * kFixed16));
				round_event.origin_z = static_cast<int32_t>(
						std::lround(double(origin.z) * kFixed16));
				round_event.dir_yaw = dir_yaw;
				round_event.dir_pitch = dir_pitch;
				round_event.shot_seq = shot_seq;
				const uint32_t clip_before_consume = static_cast<uint32_t>(
						std::max(0, ev.fired_clip_before_consume));
				round_event.mode_flags = static_cast<uint8_t>(
						((clip_before_consume & 0x3u) << 4u) | 0x02u);
				const bool vehicle_attack_context =
						mount_blocks_weapon_channel(*shooter);
				const bool scope_settled = player_view_.scope_engaged &&
						!opennova::world::player_view_scope_ease_active(player_view_);
				// The ordinary on-foot hip-fire leg is exact: retail passes
				// Weapon_GetScopeZoomLevel(false, 12), which returns 12, and the
				// server's bit-6-clearing composite preserves it. The predicate
				// reads the PROMOTED scope bit, so ADS raise and third-person use
				// the same 12. Settled-FP/mounted zoom levels remain D-WPN-8.
				if (player_view_.third_person ||
						(!scope_settled && !vehicle_attack_context &&
								(weapon_def_.flags & 0x20000000) == 0)) {
					round_event.subtype = 12;
				}
				round_event.adm_index = adm_index;
				world_->rounds.add(round_event);

				opennova::world::RoundSpawnParams round;
				round.owner = world_->cached.local_player;
				round.shooter_handle = world_->cached.local_player.packed;
				round.origin = origin;
				round.dir_yaw_bam = dir_yaw;
				round.dir_pitch_bam = dir_pitch;
				round.ammo_index = adm->ammo_index;
				round.adm_index = adm_index;
				round.shot_seq = shot_seq;
				world_->round_sim.spawn(*world_, round);
			}
		}
	}
	if (ev.dry_fired) ++weapon_dry_serial_;
	if (ev.reload_requested) ++weapon_reload_serial_;
	if (ev.reload_applied) {
		// The refill stamps the 3P body reload-anim window on the entity — 80 ticks; the
		// infantry weapon channel then wants state 65 reload until it expires (and the
		// locked clip plays to its end). In the original the stamp lives inside the
		// refill itself; the SP/listen-host loopback applies it at reload start.
		// [orig: WeaponSlot_ReloadAmmo @ 0x54173c; world-wac-ai-re.md §14.8.5]
		AiEntity *p = world_->ai ? world_->ai->for_handle(world_->cached.local_player) : nullptr;
		if (p && p->inf.active) p->inf.reload_anim_ticks = 80;
	}
	// The FSM's scope side effects land on the sim-owned engaged bit: forced
	// unscope (one-shot / reload stash) and the pump's rescope-after-reload
	// [orig: g_weaponScopeActive writes; the rescope block @ 0x54139e].
	if (ev.unscope) {
		++weapon_unscope_serial_;
		// The forced paths run the same refusing toggle — a mid-ease unscope keeps
		// the scope (rare: a reload requested inside the raise ease)
		// [orig: @ 0x543136 calls Player_ToggleWeaponScope, activeFlag-gated].
		opennova::world::player_view_set_engaged(player_view_, false,
				(weapon_def_.flags2 & 0x200) != 0);
	}
	if (ev.rescope) {
		++weapon_rescope_serial_;
		opennova::world::player_view_set_engaged(player_view_, true,
				(weapon_def_.flags2 & 0x200) != 0);
	}
}

Dictionary NovaSimulation::get_local_player_weapon_state() const {
	Dictionary out;
	out["active"] = weapon_active_;
	if (!weapon_active_) return out;
	out["current"] = weapon_slot_.current;
	out["anim_key"] = weapon_anim_key_;
	out["anim_variant"] = weapon_anim_variant_;
	const uint32_t anim_age_ticks = world_ && !weapon_anim_key_.is_empty()
			? world_->logic_tick - weapon_anim_tick_ : 0;
	out["anim_age_ticks"] = static_cast<int64_t>(anim_age_ticks);
	out["play_serial"] = static_cast<int64_t>(weapon_play_serial_);
	// The last-started action's audio/effect legs remain useful snapshot diagnostics;
	// ordered delivery uses drain_local_player_weapon_events().
	// [orig: ActionSlot_ExecuteActionWithEffect
	// @ 0x541860 -> ActionSlot_SpawnEffect @ 0x401f20].
	out["action_serial"] = static_cast<int64_t>(weapon_action_serial_);
	if (weapon_action_started_ >= 0 &&
			weapon_action_started_ < opennova::world::weapon_action::kCount) {
		const opennova::world::WeaponFsmAction &act = weapon_def_.actions[weapon_action_started_];
		out["action_started"] = weapon_action_started_;
		out["action_soundset"] = String::utf8(act.soundset);
		out["action_particle"] = String::utf8(act.particle);
		out["action_particle_userpoint"] = String::utf8(act.particle_userpoint);
	} else {
		out["action_started"] = -1;
		out["action_soundset"] = String();
		out["action_particle"] = String();
		out["action_particle_userpoint"] = String();
	}
	// The latest END-leg snapshot diagnostic: fire rows carry the per-shot gunshot
	// here (GS_*), reload rows the completion sound. Ordered delivery uses the batch.
	// [orig: ActionSlot_FinishActivePhase @ 0x53f7b0 -> the end shim @ 0x401100 plays
	//  ActionDef+12 at the owner entity].
	out["action_end_serial"] = static_cast<int64_t>(weapon_action_end_serial_);
	if (weapon_action_finished_ >= 0 &&
			weapon_action_finished_ < opennova::world::weapon_action::kCount) {
		out["action_end_soundset"] =
				String::utf8(weapon_def_.actions[weapon_action_finished_].soundsetend);
	} else {
		out["action_end_soundset"] = String();
	}
	out["fired_serial"] = static_cast<int64_t>(weapon_fired_serial_);
	out["dry_serial"] = static_cast<int64_t>(weapon_dry_serial_);
	out["reload_serial"] = static_cast<int64_t>(weapon_reload_serial_);
	out["unscope_serial"] = static_cast<int64_t>(weapon_unscope_serial_);
	out["rescope_serial"] = static_cast<int64_t>(weapon_rescope_serial_);
	out["clip"] = weapon_slot_.clip;
	out["reserve"] = weapon_slot_.reserve;
	out["kick"] = static_cast<int>(weapon_slot_.kick);
	// Read-only diagnostics for the local FIRE -> RoundData_AddRound seam. The last
	// row lets parity tests pin the observed tag-2 mode byte without exposing mutable
	// ring state. [orig: ((MountSlot.clip & 3) << 4) | 2 sampled before consume
	// @ WeaponAction_Fire 0x542c11 / 0x542c75].
	out["round_ring_count"] = world_ ? world_->rounds.count : 0;
	if (world_ && world_->rounds.count > 0) {
		const int last = world_->rounds.cursor == 0
				? opennova::world::RoundRing::kCapacity - 1
				: world_->rounds.cursor - 1;
		const opennova::world::RoundEvent &round = world_->rounds.records[
				static_cast<std::size_t>(last)];
		out["last_round_flags"] = round.mode_flags;
		out["last_round_subtype"] = round.subtype;
		out["last_round_slot_byte"] = round.slot_byte;
		out["last_round_seq"] = round.shot_seq;
	}
	// The 3P body's weapon channel (the entity's secondary AnimMap channel): the clip key
	// + its own playhead for the host's mask-bone override. The key remains populated
	// when the state id matches the primary because the two playheads are independent.
	// Empty means the override gate is off (weapon in hands + allowed mount class +
	// primary state flag 0x40).
	// [orig: gate @ 0x4b14a7; producer @ 0x4b5dad; world-wac-ai-re.md §14.8].
	out["body_anim_key"] = String();
	out["body_anim_phase"] = 0;
	if (world_ && world_->ai && world_->cached.local_player.valid()) {
		const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
		const opennova::world::Entity *entity =
				world_->registry.get(world_->cached.local_player);
		const bool blocked_mount =
				entity != nullptr && mount_blocks_weapon_channel(*entity);
		if (p && entity && opennova::world::infantry_weapon_channel_visible(
					p->inf, weapon_active_, blocked_mount)) {
			out["body_anim_key"] = infantry_anim_key(p->inf.wpn_state);
			out["body_anim_phase"] = p->inf.wpn_clip_phase;
		}
	}
	return out;
}

Array NovaSimulation::drain_local_player_weapon_events() {
	Array out;
	const uint32_t now = world_ ? world_->logic_tick : 0;
	for (const PendingWeaponEvent &event : pending_weapon_events_) {
		Dictionary row;
		// Unsigned subtraction intentionally preserves age across logic-tick wrap.
		row["age_ticks"] = static_cast<int64_t>(now - event.tick);
		row["world_position"] = event.world_position;
		row["anim_key"] = event.anim_key;
		row["anim_variant"] = event.anim_variant;
		row["action_started"] = event.action_started;
		row["action_soundset"] = event.action_soundset;
		row["action_particle"] = event.action_particle;
		row["action_particle_userpoint"] = event.action_particle_userpoint;
		row["scope_settled"] = event.scope_settled;
		row["third_person"] = event.third_person;
		row["vehicle_attack_context"] = event.vehicle_attack_context;
		row["action_finished"] = event.action_finished;
		row["action_end_soundset"] = event.action_end_soundset;
		row["action_effect"] = event.action_effect;
		row["effect_particle"] = event.effect_particle;
		row["effect_particle_userpoint"] = event.effect_particle_userpoint;
		out.push_back(row);
	}
	pending_weapon_events_.clear();
	return out;
}

// Drain the round impacts the flight sim resolved since the last call, each row already
// resolved through the ammo effects_table (canonical tag -> {effect, sound}); rows whose
// tag has neither an effect nor a sound are dropped, matching the original impact
// presenter [orig: Projectile_SpawnImpactEffect @ 0x4e9b80; selection witness on
// world/round_sim.h RoundImpact].
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
		if (row.effect.empty() && row.sound.empty()) continue;
		Dictionary d;
		// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position convention.
		d["position"] = Vector3(imp.position.x, imp.position.z, -imp.position.y);
		d["direction"] = Vector3(imp.direction.x, imp.direction.z, -imp.direction.y);
		d["effect"] = String::utf8(row.effect.c_str());
		d["sound"] = String::utf8(row.sound.c_str());
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

// HUD health/team. The original rebuilds these into its per-frame HUD info struct every frame
// (health ratio at +92 = currentHealth/maxHealth, team byte at +374). We surface the raw values
// and let the HUD compute the ratio. [orig: HUD_BuildEntityInfo @0x4b8440]
int NovaSimulation::get_local_player_health() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? e->health : 0;
}

int NovaSimulation::get_local_player_max_health() const {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return 100;
	const AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (!p || p->inf.max_health <= 0) return 100;
	return p->inf.max_health;
}

int NovaSimulation::get_local_player_team() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? static_cast<int>(e->team) : 0;
}

int NovaSimulation::get_local_player_class() const {
	if (!world_ || !world_->cached.local_player.valid()) return 0;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? static_cast<int>(e->player_class) : 0;
}

String NovaSimulation::get_local_player_weapon_name() const {
	if (!world_ || !world_->cached.local_player.valid()) return String();
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	if (!e) return String();
	const opennova::world::WeaponTableEntry *weapon =
			world_->weapons.by_index(e->equipped_adm_index);
	return weapon ? String(weapon->name.c_str()) : String();
}

void NovaSimulation::restart() {
	if (!loaded_ || !have_baseline_) return;
	pending_weapon_events_.clear();
	world_->restore(baseline_); // rewinds registry/vars/env/clock + re-inits systems (incl. AI;
	                            // WacSystem::on_load also resets its 62-tick accumulator)
	weapon_anim_tick_ = world_->logic_tick;
	// The restored world can share a tick number with a previously cached view.
	// Force the next FollowOwner query to rebuild against the post-restart epoch.
	invalidate_present_effect_pose_cache();
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
		d["str"] = String(e.str.c_str());
		out.push_back(d);
	}
	world_->effects.clear();
	return out;
}

// The host fire-presentation drain — see the header note. Direction math mirrors
// the round spawn's mission-frame forward (cos yaw * cp, sin yaw * cp, sin pitch)
// [orig: RoundData_SpawnRound @0x4ec5e9], axis-mapped mission -> godot (x, z, -y).
Array NovaSimulation::drain_fire_presentation_events() {
	Array out;
	if (!loaded_) return out;
	constexpr double kRadPerBam = (2.0 * 3.14159265358979323846) / 4294967296.0;
	const bool have_local = world_->cached.local_player.valid();
	const uint16_t local_packed = have_local ? world_->cached.local_player.packed : 0xFFFF;
	for (const opennova::world::FireEvent &fe : world_->round_sim.fired) {
		Dictionary d;
		d["origin"] = Vector3(fe.origin.x, fe.origin.z, -fe.origin.y);
		const double bearing = static_cast<double>(fe.yaw_bam) * kRadPerBam;
		const double pitch = static_cast<double>(fe.pitch_bam) * kRadPerBam;
		const double cp = std::cos(pitch);
		d["forward"] = Vector3(static_cast<real_t>(std::cos(bearing) * cp),
				static_cast<real_t>(std::sin(pitch)),
				static_cast<real_t>(-std::sin(bearing) * cp));
		d["shooter_handle"] = static_cast<int>(fe.shooter_handle);
		opennova::world::EntityHandle shooter_handle;
		shooter_handle.packed = fe.shooter_handle;
		const opennova::world::Entity *shooter = world_->registry.get(shooter_handle);
		d["source_bms_id"] = shooter != nullptr ? shooter->bms_id : 0;
		d["is_local_player"] = have_local && fe.shooter_handle == local_packed;
		d["ammo_index"] = fe.ammo_index;
		const opennova::world::AmmoTableEntry *ammo = world_->ammo.by_index(fe.ammo_index);
		d["sound_set"] = ammo ? String(ammo->ai_launch_set.c_str()) : String();
		d["effect"] = ammo ? String(ammo->ai_launch_effect.c_str()) : String();
		d["mf_light"] = ammo ? ammo->mf_light : 0;
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
	out["has_collision_instance"] = collision_world_.has_instance(found->handle);
	const opennova::world::ItemDeathTraits *t =
			world_->item_death_traits.get(found->item_id);
	out["has_death_traits"] = t != nullptr;
	if (t != nullptr) {
		out["armor_impact"] = t->armor_impact;
		out["armor_blast"] = t->armor_blast;
		out["unit_type"] = t->unit_type;
		out["kz"] = t->kz;
		out["has_husk"] = t->has_husk;
	}
	out["pos"] = Vector3(found->position.x, found->position.z, -found->position.y);
	return out;
}

// Live tracer rounds for the streak layer — see the header note.
PackedFloat32Array NovaSimulation::get_tracer_rounds() const {
	PackedFloat32Array out;
	if (!loaded_) return out;
	for (const opennova::world::LiveRound &r : world_->round_sim.rounds) {
		if (!r.active || !r.tracer) continue;
		const opennova::world::AmmoTableEntry *ammo = world_->ammo.by_index(r.ammo_index);
		const int64_t base = out.size();
		out.resize(base + 9);
		float *w = out.ptrw() + base;
		w[0] = r.pos.x;
		w[1] = r.pos.z;
		w[2] = -r.pos.y;
		w[3] = r.vel.x;
		w[4] = r.vel.z;
		w[5] = -r.vel.y;
		w[6] = static_cast<float>(r.team);
		w[7] = ammo ? static_cast<float>(ammo->tracer_type_friendly) : 0.0f;
		w[8] = ammo ? static_cast<float>(ammo->tracer_type_enemy) : 0.0f;
	}
	return out;
}

void NovaSimulation::set_wac_program(const Ref<NovaWacProgram> &p_program) {
	wac_program_ = p_program;
	if (!loaded_ || !wac_) {
		return; // finish_load applies it on the next load
	}
	if (wac_program_.is_valid() && wac_program_->is_ok()) {
		wac_->set_program(wac_program_->native_program());
	} else {
		wac_->set_program(opennova::wac::Program());
	}
}

bool NovaSimulation::compile_and_set_wac(const PackedStringArray &p_sources) {
	ERR_FAIL_COND_V_MSG(!loaded_, false, "compile_and_set_wac needs a loaded world (the registry resolves symbolic names).");
	std::vector<std::string> sources;
	sources.reserve(static_cast<size_t>(p_sources.size()));
	for (int64_t i = 0; i < p_sources.size(); ++i) {
		const CharString utf8 = p_sources[i].utf8();
		sources.emplace_back(utf8.get_data(), static_cast<size_t>(utf8.length()));
	}
	opennova::wac::CompileEnv env;
	env.registry = &world_->registry;
	opennova::wac::Program program = opennova::wac::compile_program(sources, env);
	Ref<NovaWacProgram> holder;
	holder.instantiate();
	// Adopt the registry-compiled program into the holder so get_wac_program()
	// exposes its diagnostics either way.
	holder->adopt(std::move(program));
	wac_program_ = holder;
	if (!wac_program_->is_ok()) {
		return false;
	}
	wac_->set_program(wac_program_->native_program());
	return true;
}

Dictionary NovaSimulation::get_wac_state() const {
	Dictionary out;
	out["loaded"] = wac_ != nullptr && wac_->vm().loaded();
	out["paused"] = wac_ != nullptr && wac_->paused;
	out["runs"] = wac_ != nullptr ? static_cast<int64_t>(wac_->runs()) : 0;
	out["event_count"] = wac_ != nullptr ? wac_->program().event_count : 0;
	out["code_size"] = wac_ != nullptr ? static_cast<int>(wac_->program().code.size()) : 0;
	return out;
}

Dictionary NovaSimulation::get_runtime_perf_counters() const {
	Dictionary out;
	out["loaded"] = loaded_;
	out["listen_server"] = listen_server_;
	out["ai_count"] = ai_ ? ai_->count() : 0;
	out["present_entity_count"] = last_present_entity_count_;
	out["sim_tick_us"] = static_cast<int64_t>(last_sim_tick_us_);
	out["net_tick_us"] = static_cast<int64_t>(last_net_tick_us_);
	out["present_snapshot_us"] = static_cast<int64_t>(last_present_snapshot_us_);
	return out;
}

void NovaSimulation::set_wac_paused(bool p_paused) {
	if (wac_) {
		wac_->paused = p_paused; // [orig: dword_C6EB28]
	}
}

bool NovaSimulation::is_wac_paused() const {
	return wac_ != nullptr && wac_->paused;
}

void NovaSimulation::set_mission_variable(int index, int value) {
	if (world_) world_->vars.set_mission(index, value);
}

// Probe/diagnostic seam beside get_entity_debug: write an AI entity's health through
// the same stores the scripted SETHP path touches (registry + the motor copy)
// [orig: the WAC SETHP op writes entity+286]. Lets in-game probes shorten a fight
// without bypassing the damage/death chain under test.
void NovaSimulation::debug_set_entity_health(int p_index, int p_hp) {
	if (!ai_ || !world_) return;
	AiEntity *e = ai_->at(p_index);
	if (!e) return;
	e->health = static_cast<int16_t>(p_hp);
	if (opennova::world::Entity *ent = world_->registry.get(e->handle)) {
		ent->health = p_hp;
		ent->alive = p_hp > 0;
	}
}

// The D-AI-6 muzzle seam: the present layer pushes each posed model's gun-flash
// userpoint world position back to the sim once per frame; the AI fire pass spawns
// rounds from it while fresh. Godot (x, up, z) -> mission (x, -gz, gy) in 16.16
// fixed — the inverse of the present mapping godot = (mx, mz, -my).
// [orig: Entity_GetAttachmentWorldPosition @0x4b2670 from the anim-event fire block
// @0x4bf326 — computed inline against the engine-side skeleton; ours is host-fed.]
void NovaSimulation::set_ai_muzzle_world(int p_net_id, const Vector3 &p_godot_pos) {
	if (!ai_ || !world_) return;
	// Keyed by the row's PF_NET_ID (the authored SSN) — the wire handle is
	// 0-ambiguous for pool-0 slot 0, and the row order is not the AI index.
	// for_handle inside set_entity_muzzle drops non-AI entities.
	if (p_net_id <= 0 || p_net_id > 0xFFFF) return;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	if (!h.valid()) return;
	const int32_t pos[3] = {
		static_cast<int32_t>(p_godot_pos.x * 65536.0f),
		static_cast<int32_t>(-p_godot_pos.z * 65536.0f),
		static_cast<int32_t>(p_godot_pos.y * 65536.0f),
	};
	ai_->set_entity_muzzle(h, pos, world_->logic_tick);
}

// Probe seam beside debug_set_entity_health: teleport an AI entity through both
// position stores (registry + motor copy) — mission-space coordinates. Lets
// in-game probes bring a reachable victim to the player when the mission
// geography (interiors, fences) defeats straight-line navigation.
void NovaSimulation::debug_set_entity_position(int p_index, const Vector3 &p_mission_pos) {
	if (!ai_ || !world_) return;
	AiEntity *e = ai_->at(p_index);
	if (!e) return;
	e->pos[0] = static_cast<int32_t>(p_mission_pos.x * 65536.0f);
	e->pos[1] = static_cast<int32_t>(p_mission_pos.y * 65536.0f);
	e->pos[2] = static_cast<int32_t>(p_mission_pos.z * 65536.0f);
	if (opennova::world::Entity *ent = world_->registry.get(e->handle)) {
		ent->position.x = p_mission_pos.x;
		ent->position.y = p_mission_pos.y;
		ent->position.z = p_mission_pos.z;
	}
}

// World-registry probe seams keyed by SSN — pool-1 vehicles (and anything else
// without an AI brain) are invisible to the AI-index seams above; vehicle probes
// need to find and place them. Mission-space coordinates, same convention as
// debug_set_entity_position.
Dictionary NovaSimulation::get_world_entity_debug(int p_net_id) const {
	Dictionary out;
	if (!world_ || p_net_id <= 0 || p_net_id > 0xFFFF) return out;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	const opennova::world::Entity *ent = world_->registry.get(h);
	if (!ent) return out;
	out["net_id"] = static_cast<int>(ent->net_id);
	out["bms_id"] = ent->bms_id;
	out["pool"] = h.pool();
	out["alive"] = ent->alive;
	out["health"] = ent->health;
	out["mission_position"] = Vector3(ent->position.x, ent->position.y, ent->position.z);
	out["position"] = Vector3(ent->position.x, ent->position.z, -ent->position.y);
	out["yaw"] = static_cast<int>(ent->yaw);
	out["seat_count"] = static_cast<int>(ent->seats.size());
	Array seats;
	for (const opennova::world::Seat &s : ent->seats) {
		Dictionary sd;
		sd["type"] = static_cast<int>(s.type);
		sd["occupied"] = s.occupant.valid();
		sd["local"] = Vector3(s.seat_local.x, s.seat_local.y, s.seat_local.z);
		sd["name"] = String(s.source_name.c_str());
		seats.push_back(sd);
	}
	out["seats"] = seats;
	return out;
}

void NovaSimulation::debug_set_world_entity_position(int p_net_id,
                                                     const Vector3 &p_mission_pos) {
	if (!world_ || p_net_id <= 0 || p_net_id > 0xFFFF) return;
	const opennova::world::EntityHandle h =
			world_->registry.find_by_net_id(static_cast<uint16_t>(p_net_id));
	opennova::world::Entity *ent = world_->registry.get(h);
	if (!ent) return;
	ent->position.x = p_mission_pos.x;
	ent->position.y = p_mission_pos.y;
	ent->position.z = p_mission_pos.z;
	// Keep the AI mirror in step when the entity carries a brain (harmless otherwise).
	if (ai_) {
		if (AiEntity *ae = ai_->for_handle(h)) {
			ae->pos[0] = static_cast<int32_t>(p_mission_pos.x * 65536.0f);
			ae->pos[1] = static_cast<int32_t>(p_mission_pos.y * 65536.0f);
			ae->pos[2] = static_cast<int32_t>(p_mission_pos.z * 65536.0f);
		}
	}
}

int NovaSimulation::get_mission_variable(int index) const {
	return world_ ? world_->vars.get_mission(index) : 0;
}

Dictionary NovaSimulation::get_round_outcome_debug() const {
	Dictionary out;
	if (!world_) return out;
	out["ended"] = world_->round_end.ended;
	out["winner_team"] = world_->round_end.winner_team;
	out["bluekills"] = world_->kill_stats.bluekills_by_player;
	out["greenkills"] = world_->kill_stats.greenkills_by_player;
	out["enemy_kills"] = world_->kill_stats.enemy_kills_by_player;
	out["team_kills_by_others"] = world_->kill_stats.team_kills_by_others;
	out["friendly_kills_by_others"] = world_->kill_stats.friendly_kills_by_others;
	out["enemy_kills_by_others"] = world_->kill_stats.enemy_kills_by_others;
	out["humans"] = world_->cached.humans;
	out["mp_session"] = world_->mp_session;
	return out;
}

bool NovaSimulation::has_event_fired(int index) const {
	if (!bms_ || index < 0) return false;
	// The active latch + delay-elapsed window — the same read the original's Event
	// trigger category makes [orig: EventTrigger_EvaluateCondition @0x453620 case 3].
	return bms_->event_fired(static_cast<size_t>(index));
}

int NovaSimulation::get_event_count() const {
	return bms_ ? static_cast<int>(bms_->events().size()) : 0;
}

int64_t NovaSimulation::get_logic_tick() const {
	// uint32 -> int64 keeps long sessions sign-safe on the GDScript side.
	return world_ ? static_cast<int64_t>(world_->logic_tick) : 0;
}

namespace {
PackedInt32Array snapshot_bank(const opennova::world::World *world, int count,
                               int32_t (opennova::world::ScriptVarStore::*getter)(int) const) {
	PackedInt32Array out;
	out.resize(count);
	int32_t *w = out.ptrw();
	for (int i = 0; i < count; ++i) {
		w[i] = world ? (world->vars.*getter)(i) : 0;
	}
	return out;
}
} // namespace

PackedInt32Array NovaSimulation::get_mission_variables_snapshot() const {
	return snapshot_bank(world_.get(), opennova::world::ScriptVarStore::kMissionVars,
	                     &opennova::world::ScriptVarStore::get_mission);
}

PackedInt32Array NovaSimulation::get_global_variables_snapshot() const {
	return snapshot_bank(world_.get(), opennova::world::ScriptVarStore::kGlobalVars,
	                     &opennova::world::ScriptVarStore::get_global);
}

PackedInt32Array NovaSimulation::get_music_variables_snapshot() const {
	return snapshot_bank(world_.get(), opennova::world::ScriptVarStore::kMusicVars,
	                     &opennova::world::ScriptVarStore::get_music);
}

void NovaSimulation::set_global_variable(int index, int value) {
	if (world_) world_->vars.set_global(index, value);
}

int NovaSimulation::get_global_variable(int index) const {
	return world_ ? world_->vars.get_global(index) : 0;
}

PackedByteArray NovaSimulation::get_fired_events_snapshot() const {
	PackedByteArray out;
	if (!bms_) return out;
	const size_t count = bms_->events().size();
	out.resize(static_cast<int64_t>(count));
	uint8_t *w = out.ptrw();
	for (size_t i = 0; i < count; ++i) {
		w[i] = bms_->event_fired(i) ? 1 : 0;
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
	out["kind"] = ent ? static_cast<int>(ent->spawn_origin >> 24) : -1;
	out["index"] = ent ? static_cast<int>(ent->spawn_origin & 0xFFFFFF) : -1;
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
	out["mounted"] = ent ? ent->mounted : false;
	out["mount_target_net_id"] = 0;
	out["mount_seat"] = ent ? static_cast<int>(ent->mount_seat) : -1;
	out["mount_type"] = ent ? static_cast<int>(ent->mount_type) : 0;
	out["mount_seat_bone"] = 0;
	out["mount_seat_pose_index"] = 0;
	out["mount_seat_source_name"] = String();
	out["mount_seat_local"] = Vector3();
	out["mount_seat_yaw_offset"] = 0;
	out["mount_target_emplaced_pose_variant"] = 0;
	out["mount_target_seat_count"] = 0;
	out["mount_target_seats"] = Array();
	if (ent && ent->mounted) {
		const opennova::world::Entity *target = world_->registry.get(ent->mount_target);
		if (target) {
			out["mount_target_net_id"] = static_cast<int>(target->net_id);
			out["mount_target_emplaced_pose_variant"] = static_cast<int>(target->emplaced_pose_variant);
			out["mount_target_seat_count"] = static_cast<int>(target->seats.size());
			Array target_seats;
			for (int i = 0; i < static_cast<int>(target->seats.size()); ++i) {
				const opennova::world::Seat &seat = target->seats[i];
				Dictionary d;
				d["index"] = i;
				d["type"] = static_cast<int>(seat.type);
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
	// The D-AI-6 muzzle seam readback (probe surface): the host-fed posed muzzle.
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

int NovaSimulation::get_entity_count() const {
	return ai_ ? ai_->count() : 0;
}

int NovaSimulation::get_entity_kind(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return static_cast<int>(ent->spawn_origin >> 24); // [orig promote: (kind<<24)|index]
}

int NovaSimulation::get_entity_index(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	if (!ent) return -1;
	return static_cast<int>(ent->spawn_origin & 0xFFFFFF);
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
// moving. The host basis (MissionObjectPlacer.bms_to_godot_basis) takes the MISSION yaw and internally
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
	present_effect_poses_by_handle_.reserve(client.entities.size());
	present_effect_handles_by_bms_id_.reserve(client.entities.size());
	present_effect_handles_by_ssn_.reserve(client.entities.size());
	present_effect_handles_by_origin_.reserve(client.entities.size());

	for (const opennova::netsim::ClientEntityState &entity_state : client.entities) {
		// Match present_snapshot_from_client_view's joiner self-filter: the host's
		// wire echo H is not drawn and therefore cannot own a presented effect.
		if (joiner_ && joiner_self_wire_handle_ != 0 &&
				entity_state.handle == joiner_self_wire_handle_) {
			continue;
		}

		const int32_t heading_bam = static_cast<int32_t>(
				static_cast<uint32_t>(entity_state.yaw_byte) << 24);
		PresentEffectPose pose;
		pose.position = Vector3(
				static_cast<float>(entity_state.x / kFixed16),
				static_cast<float>(entity_state.z / kFixed16),
				static_cast<float>(-entity_state.y / kFixed16));
		pose.rotation_deg = Vector3(
				0.0f,
				static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(
						heading_bam)),
				0.0f);
		present_effect_poses_by_handle_[entity_state.handle] = pose;

		// A joiner's decoded handles belong to the host, so only wire identity is
		// meaningful there. Host/listen views can resolve the same registry entity
		// used by get_present_snapshot() for BMS origin and SSN identity.
		if (joiner_) continue;
		const opennova::world::Entity *entity = world_->registry.get(
				opennova::world::EntityHandle{entity_state.handle});
		if (!entity) continue;
		if (entity->bms_id > 0) {
			present_effect_handles_by_bms_id_[static_cast<int>(entity->bms_id)] =
					entity_state.handle;
		}
		if (entity->net_id > 0) {
			present_effect_handles_by_ssn_[static_cast<int>(entity->net_id)] =
					entity_state.handle;
		}
		const int kind = static_cast<int>(entity->spawn_origin >> 24);
		const int index = static_cast<int>(entity->spawn_origin & 0xFFFFFFu);
		present_effect_handles_by_origin_[present_effect_origin_key(kind, index)] =
				entity_state.handle;
	}

	present_effect_pose_cache_logic_tick_ = logic_tick;
	present_effect_pose_cache_client_frame_ = client.frames_applied;
	present_effect_pose_cache_runtime_ = runtime_.get();
	present_effect_pose_cache_valid_ = true;
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
	return cached_present_effect_state_for_handle(p_handle);
}

PackedVector3Array NovaSimulation::get_present_effect_state_for_ssn(int p_ssn) const {
	if (p_ssn <= 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const auto found = present_effect_handles_by_ssn_.find(p_ssn);
	if (found == present_effect_handles_by_ssn_.end()) return PackedVector3Array();
	return cached_present_effect_state_for_handle(found->second);
}

PackedVector3Array NovaSimulation::get_present_effect_state_for_wire_handle(
		int p_wire_handle) const {
	if (p_wire_handle <= 0 ||
			p_wire_handle > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
		return PackedVector3Array();
	}
	return present_effect_state_for_handle(static_cast<uint16_t>(p_wire_handle));
}

PackedVector3Array NovaSimulation::get_present_effect_state_for_bms_id(int p_bms_id) const {
	if (p_bms_id <= 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const auto found = present_effect_handles_by_bms_id_.find(p_bms_id);
	if (found == present_effect_handles_by_bms_id_.end()) return PackedVector3Array();
	return cached_present_effect_state_for_handle(found->second);
}

PackedVector3Array NovaSimulation::get_present_effect_state_for_origin(
		int p_kind, int p_index) const {
	if (p_kind < 0 || p_index < 0) return PackedVector3Array();
	ensure_present_effect_pose_cache();
	const auto found = present_effect_handles_by_origin_.find(
			present_effect_origin_key(p_kind, p_index));
	if (found == present_effect_handles_by_origin_.end()) return PackedVector3Array();
	return cached_present_effect_state_for_handle(found->second);
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
	const int slot = channel - 1;
	return e->brain.f[AiBrain::kPartAnimRate0 + slot] != 0 ||
	       e->brain.f[AiBrain::kPartAnimPhase0 + slot] != 0;
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
	const uint64_t start_us = perf_now_us();
	// P7 (ADR 0011 Decision 1): every play path is the in-process listen server — the present pass
	// reads the state the LOCAL CLIENT decoded off the wire (ClientState), not the authoritative sim
	// directly. SP, a LAN host, and the editor preview all render exactly what a networked peer would;
	// a joiner renders remote entities wire-direct. The old no-net AI-pool present is retired. Empty
	// when no runtime is active (a bare sim) — scalar getters (get_entity_*) read the AI pool for tooling.
	PackedFloat32Array out;
	if (runtime_) {
		out = present_snapshot_from_client_view();
	}
	last_present_entity_count_ = static_cast<int>(out.size() / PF_STRIDE);
	last_present_snapshot_us_ = perf_now_us() - start_us;
	return out;
}

void NovaSimulation::enable_listen_server(bool p_enable) {
	listen_server_ = p_enable;
	// P7: the SP listen server now rides the npruntime in-match runtime (ctx_ / host_loop_ /
	// runtime_), stood up per-load in bringup_host_runtime — there is no net ISystem and
	// no legacy loopback seam here. (The LAN host still builds the legacy seam in enable_host_listen
	// until A3; a sim is SP listen XOR LAN host XOR joiner.)
}

void NovaSimulation::set_terrain_til_data(const PackedByteArray &p_til_bytes) {
	terrain_til_data_.assign(p_til_bytes.ptr(), p_til_bytes.ptr() + p_til_bytes.size());
}

bool NovaSimulation::enable_host_listen(int p_port) {
	listen_server_ = true;
	if (pump_.is_null()) pump_.instantiate();
	if (pump_->bind_listen(p_port) != 0) {
		host_listen_ = false;
		return false;
	}
	host_listen_ = true;
	// P7: the LAN host rides the npruntime runtime (ctx_ over a real UDP socket), stood up per-load in
	// bringup_host_runtime with SocketMode::Lan. NovaUdpPump owns the socket; all protocol/crypto/
	// framing stays in libs (ADR 0010). host_session_config_ keeps the GDScript-facing session options
	// (the Dictionary getter + the §5.1 reactive-reply config fed to configure_session_runtime).
	host_bind_port_ = static_cast<uint16_t>(std::clamp(p_port, 0, 0xFFFF));
	return true;
}

int NovaSimulation::get_host_listen_port() const {
	return (host_listen_ && pump_.is_valid()) ? pump_->local_port() : 0;
}

int NovaSimulation::get_host_peer_count() const {
	// Count the type-1 (remote-joiner) connections in the npruntime table. The host's own type-2
	// loopback is excluded; a pre-Hello garbage datagram registers no node (handle_server_datagram
	// drops bad envelopes), so it stays 0 until a real JointOperations peer handshakes.
	int n = 0;
	for (const opennova::np::NapiNPConnection &c : ctx_.np_protocol.connection_list) {
		if (c.type == 1) ++n;
	}
	return n;
}

void NovaSimulation::configure_host_session(Dictionary p_options) {
	opennova::np::GameConfig config = host_session_config_;
	host_bind_port_ = dictionary_u16(p_options, "bind_port", host_bind_port_);
	apply_dictionary_string(p_options, "server_name", config.server_name);
	apply_dictionary_string(p_options, "mission_name", config.mission_name);
	apply_dictionary_string(p_options, "mission_file", config.mission_file);
	apply_dictionary_string(p_options, "player_name", config.player_name);
	apply_dictionary_string(p_options, "expansion", config.expansion);
	if (p_options.has("gametype")) {
		config.game_type = dictionary_u32(p_options, "gametype", config.game_type);
	} else if (p_options.has("game_type")) {
		config.game_type = dictionary_u32(p_options, "game_type", config.game_type);
	}
	config.mp_attributes = dictionary_u32(p_options, "mpattrib", config.mp_attributes);
	if (p_options.has("spawn_x") || p_options.has("spawn_y") || p_options.has("spawn_z")) {
		config.spawn_x = dictionary_u32(p_options, "spawn_x", config.spawn_x);
		config.spawn_y = dictionary_u32(p_options, "spawn_y", config.spawn_y);
		config.spawn_z = dictionary_u32(p_options, "spawn_z", config.spawn_z);
	}
	if (p_options.has("spawn_names")) {
		config.spawn_names.clear();
		const Variant names_v = p_options.get("spawn_names", Array());
		if (names_v.get_type() == Variant::ARRAY) {
			const Array names = names_v;
			for (int64_t i = 0; i < names.size(); ++i) {
				const String name = names[i];
				if (!name.is_empty()) {
					config.spawn_names.emplace_back(name.utf8().get_data());
				}
			}
		}
	}
	// Server type + player cap (UI host config): serve_and_play gates the host's own-player spawn +
	// loopback fold at bring-up; max_players is the lobby-advertised cap, clamped to the witnessed 1..65.
	if (p_options.has("serve_and_play")) {
		host_serve_and_play_ = static_cast<bool>(p_options["serve_and_play"]);
	}
	if (p_options.has("max_players")) {
		uint32_t mp = dictionary_u32(p_options, "max_players", host_max_players_);
		if (mp < 1u) {
			mp = 1u;
		} else if (mp > 65u) {
			mp = 65u;
		}
		host_max_players_ = mp;
	}
	host_session_config_ = std::move(config);
}

Dictionary NovaSimulation::get_host_session_config() const {
	const opennova::np::GameConfig &session = host_session_config_;
	Dictionary out;
	out["bind_port"] = static_cast<int>(host_bind_port_);
	out["server_name"] = String(session.server_name.c_str());
	out["mission_name"] = String(session.mission_name.c_str());
	out["mission_file"] = String(session.mission_file.c_str());
	out["player_name"] = String(session.player_name.c_str());
	out["expansion"] = String(session.expansion.c_str());
	out["gametype"] = static_cast<int64_t>(session.game_type);
	out["mpattrib"] = static_cast<int64_t>(session.mp_attributes);
	out["spawn_x"] = static_cast<int64_t>(session.spawn_x);
	out["spawn_y"] = static_cast<int64_t>(session.spawn_y);
	out["spawn_z"] = static_cast<int64_t>(session.spawn_z);
	out["mission_header_size"] = static_cast<int64_t>(session.mission_header_blob.size());
	Array spawn_names;
	for (const std::string &name : session.spawn_names) {
		spawn_names.push_back(String(name.c_str()));
	}
	out["spawn_names"] = spawn_names;
	return out;
}

// ---- co-op LAN joiner (D.2) -------------------------------------------------

bool NovaSimulation::enable_join(const String &p_host_ip, int p_port, const String &p_player_name) {
	// P7: the joiner is a non-authority np::ClientRuntime (Joiner role) built per-load in finish_load;
	// it owns the connect-leg state machine + the S2C->ClientState fold internally. Here we only dial
	// the socket + store the player name (the ClientHello.co the host echoes for the name-match). Leave
	// listen_server_ false (the present gate adds || joiner_); a sim is host XOR joiner.
	if (pump_.is_null()) pump_.instantiate();
	if (pump_->dial(p_host_ip, p_port) != 0) {
		joiner_ = false;
		return false;
	}
	joiner_player_name_ = std::string(p_player_name.utf8().get_data());
	// Build the Joiner runtime now so get_joiner_phase reads Idle before the first load (the contract
	// the legacy joiner_session_ held); finish_load rebuilds it fresh on each (re)load.
	runtime_ = std::make_unique<opennova::np::ClientRuntime>(
			opennova::ClientSession::Config::jointoperations(), joiner_player_name_);
	install_item_class_resolver();
	joiner_ = true;
	joiner_started_ = false;
	joiner_local_spawned_ = false;
	joiner_self_wire_handle_ = 0;
	return true;
}

bool NovaSimulation::is_joined_in_match() const {
	return joiner_ && runtime_ && runtime_->in_match();
}

int NovaSimulation::get_joiner_phase() const {
	return (joiner_ && runtime_) ? static_cast<int>(runtime_->phase()) : -1;
}

int NovaSimulation::get_joiner_self_handle() const {
	return joiner_ ? static_cast<int>(joiner_self_wire_handle_) : 0;
}

void NovaSimulation::ship_to_host(const std::vector<uint8_t> &dg) {
	if (pump_.is_null() || dg.empty()) return;
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(dg.size()));
	std::memcpy(bytes.ptrw(), dg.data(), dg.size());
	pump_->send_to_host(bytes);
}

opennova::world::PlayerSpawn NovaSimulation::spawn_from_self(
		const opennova::np::JoinerConnection::SelfSpawn &s) const {
	opennova::world::PlayerSpawn spawn;
	// SelfSpawn position is mission i32 16.16; PlayerSpawn.position is float mission units.
	spawn.position = {static_cast<float>(s.pos_x / kFixed16),
	                  static_cast<float>(s.pos_y / kFixed16),
	                  static_cast<float>(s.pos_z / kFixed16)};
	// orientation is ALREADY a full 32-bit BAM (unlike HostJoinerPose.heading, an i16
	// the host shifts << 16) -> pass it straight to the (90 - heading) mission-degree map.
	spawn.yaw = static_cast<int16_t>(
			std::lround(opennova::world::mission_yaw_deg_from_bam_heading(s.orientation)));
	spawn.team = s.team;
	spawn.net_id = s.net_id; // the host-assigned joiner SSN (NOT the host's own 0xFFF0)
	return spawn;
}

// (P7 A4: joiner_net_poll / joiner_net_flush deleted — the joiner now runs through joiner_pump
//  over an np::ClientRuntime; the legacy JoinerSession path is retired here.)

bool NovaSimulation::admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!host_listen_ || !world_ || !world_->ai) return false;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x,-z,y), the inverse of the present remap (same as spawn_local_player).
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
	spawn.team = static_cast<uint8_t>(p_team);
	// A synthetic loopback peer; distinct port per call so repeated admits don't alias. Own a transport
	// so the connection is well-formed. The synthetic admit (no handshake) mirrors the post-PeerSpawned
	// state; with the host already at net_id 0xFFF0 the joiner allocates 0xFFF1.
	const opennova::PeerAddr peer{0x0100007Fu, static_cast<uint16_t>(40000 + host_owner_.peers.size())};
	opennova::np::PeerLink &link = host_owner_.peers[peer];
	if (!link.transport) {
		link.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
				opennova::netsim::UdpSessionTransport::Role::Host);
	}
	return opennova::np::admit_synthetic_peer(ctx_, *world_, peer, spawn, link.transport.get()).valid();
}

PackedFloat32Array NovaSimulation::present_snapshot_from_client_view() const {
	PackedFloat32Array out;
	if (!world_ || !runtime_) return out;
	// P7: every path (SP / LAN host / joiner) reads its own npruntime ClientRuntime view's ClientState.
	const opennova::netsim::ClientState &cs = runtime_->state();
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
		r[PF_BODY_ANIM_SLOT] = -1.0f; r[PF_ANIM_STATE] = -1.0f; r[PF_ANIM_PHASE_TICKS] = 0.0f;
		r[PF_HIDDEN] = 0.0f; r[PF_ALIVE] = 1.0f;
		r[PF_TYPE_ID] = 0.0f; r[PF_WIRE_HANDLE] = 0.0f;

		// Self-filter (joiner): the host SNAPs our own entity (wire handle H) and streams
		// it back in 0x0A; we draw our local player L via LocalPlayerHost, so drop the wire
		// echo here. The row stays at its zero/unresolved defaults (PF_TYPE_ID 0), which the
		// wire render pass skips. [net-re §5.38b two-handle L-vs-H reconciliation]
		if (joiner_ && joiner_self_wire_handle_ != 0 && es.handle == joiner_self_wire_handle_) {
			continue;
		}
		// Wire identity for the render pass. The joiner has no authoritative registry for
		// the host's entities, so it renders from the wire type id + handle, not a node.
		r[PF_TYPE_ID] = static_cast<float>(es.type_id);
		r[PF_WIRE_HANDLE] = static_cast<float>(es.handle);

		// On the HOST listen server, kind/index/bms_id/net_id resolve from the registry
		// entity behind the decoded handle (host == authoritative client, so the placed-node
		// mapping still resolves through MissionEntityRegistry exactly as the AI-pool path
		// does). A joiner's decoded handles live in the HOST's handle space and would resolve
		// to the wrong local entity, so the joiner skips this and renders wire-direct (b2).
		const opennova::world::EntityHandle h{es.handle};
		const opennova::world::Entity *ent = (!joiner_) ? world_->registry.get(h) : nullptr;
		if (ent) {
			r[PF_KIND] = static_cast<float>(ent->spawn_origin >> 24);
			r[PF_INDEX] = static_cast<float>(ent->spawn_origin & 0xFFFFFF);
			r[PF_BMS_ID] = static_cast<float>(ent->bms_id);
			r[PF_NET_ID] = static_cast<float>(ent->net_id);
			r[PF_BODY_ANIM_SLOT] = static_cast<float>(ent->body_anim_slot);
			r[PF_HIDDEN] = ent->hidden ? 1.0f : 0.0f;
			r[PF_ALIVE] = ent->alive ? 1.0f : 0.0f;
		}
		// Decoded wire position is mission (x,y,z) 16.16 -> Godot (x, z, -y) world units,
		// the SAME remap the AI-pool path uses. Position is post-compression (lossy) —
		// exactly what the original client renders for its decoded peers.
		r[PF_POS_X] = static_cast<float>(es.x / kFixed16);
		r[PF_POS_Y] = static_cast<float>(es.z / kFixed16);
		r[PF_POS_Z] = static_cast<float>(-es.y / kFixed16);
		// Coarse heading: rebuild the 32-bit engine BAM from the compact high byte, then
		// engine -> mission yaw (90 - heading), matching the AI-pool present.
		const int32_t heading_bam = static_cast<int32_t>(static_cast<uint32_t>(es.yaw_byte) << 24);
		r[PF_YAW_DEG] = static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(heading_bam));
		// Infantry anim from the local AI pool (host only — same registry caveat as above).
		if (world_->ai && !joiner_) {
			const AiEntity *ae = world_->ai->for_handle(h);
			if (ae && ae->inf.active) {
				r[PF_ANIM_STATE] = static_cast<float>(ae->inf.anim_state);
				r[PF_ANIM_PHASE_TICKS] = static_cast<float>(ae->inf.clip_phase);
			}
		}
	}
	return out;
}

void NovaSimulation::set_loco_scale(int p_scale) {
	if (ai_) ai_->loco_scale = p_scale;
}

int NovaSimulation::get_loco_scale() const {
	return ai_ ? ai_->loco_scale : 0;
}
