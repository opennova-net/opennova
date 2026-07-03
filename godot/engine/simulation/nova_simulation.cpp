#include "simulation/nova_simulation.h"

#include <wac/compiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

#include "netsim/connection.h"
#include "netsim/entity_wire_bridge.h" // build_player_uplink (joiner-side C2S 0x0C body)

#include <novaworld/ingame_decode.h> // class_from_tag (§5.10b *_function -> wire class)
#include <novaworld/ingame_encode.h> // encode_organic_spawn_batch (+ OrganicSpawnBatch)

#include <npruntime/server_session.h> // set_connection_mode / set_transport_mode / create_session / mark_host_client_in_match
#include <npruntime/server_spawn.h>   // Server_ProcessPendingPlayerSpawns (faithful host-player auto-spawn)
#include <npruntime/server_tick.h>    // Server_TickUpdate (the single C2S drain + logic tick + 0x0A fan)
#include <npruntime/weapon_table_build.h> // build_weapon_table (weapon.def -> world armory, D-NET-141)

#include <def/def.h> // def_parse_weapons_memory / def_free_weapons

#include <mission/bms.h>
#include <mission/mission.h>          // kItemIdOffset (wire type id -> items.def id)
#include <mission/mission_systems.h>
#include <world/angle.h>
#include <world/player_spawn.h>
#include <world/spawn_select.h>

#include "object/nova_item_database.h"
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
	apply_terrain_to_ai(); // re-point the fresh ai_ at the persisted terrain field (if any)
	apply_root_motion_to_ai(); // ...and at the persisted infantry clip set (if any)
}

// Re-point the (possibly just-rebuilt) AI system at our owned terrain field. The field's raw
// pointers reference terrain_heightmap_/terrain_sector_grid_, which persist across reset_world.
void NovaSimulation::apply_terrain_to_ai() {
	if (!ai_) return;
	ai_->terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	ai_->ground_clearance = opennova::world::GroundClearance{};
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
	}
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
		if (!spec.seats.empty()) item_seat_specs_.push_back(std::move(spec));
	}
}

void NovaSimulation::set_terrain_height_field(const Ref<NovaTerrainData> &p_terrain) {
	// Clear first so a null/unloaded terrain disables grounding.
	terrain_heightmap_.clear();
	terrain_sector_grid_.clear();
	terrain_field_ = opennova::terrain::TerrainHeightField{};

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
		}
	}
	apply_terrain_to_ai();
}

void NovaSimulation::finish_load(const opennova::bms::File &file) {
	// One world, three systems, the faithful tick order. The AI-change action family reaches
	// brains through World::ai; wire it before registering so the pre-mission pass can dispatch.
	bms_->load(file.events, file.triggers, file.actions);
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
	ClassDB::bind_method(D_METHOD("advance_frame"), &NovaSimulation::advance_frame);
	ClassDB::bind_method(D_METHOD("restart"), &NovaSimulation::restart);
	ClassDB::bind_method(D_METHOD("set_tick_mode", "mode"), &NovaSimulation::set_tick_mode);
	ClassDB::bind_method(D_METHOD("get_tick_mode"), &NovaSimulation::get_tick_mode);
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
	ClassDB::bind_method(D_METHOD("set_player_input", "forward", "back", "left", "right", "run", "crouch", "prone", "jump", "look_yaw_deg", "look_pitch_deg"), &NovaSimulation::set_player_input);
	ClassDB::bind_method(D_METHOD("get_local_player_position"), &NovaSimulation::get_local_player_position);
	ClassDB::bind_method(D_METHOD("get_local_player_yaw_deg"), &NovaSimulation::get_local_player_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_pitch_deg"), &NovaSimulation::get_local_player_pitch_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_body_anim_slot"), &NovaSimulation::get_local_player_body_anim_slot);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_key"), &NovaSimulation::get_local_player_anim_key);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_phase_ticks"), &NovaSimulation::get_local_player_anim_phase_ticks);
	ClassDB::bind_method(D_METHOD("get_local_player_health"), &NovaSimulation::get_local_player_health);
	ClassDB::bind_method(D_METHOD("get_local_player_max_health"), &NovaSimulation::get_local_player_max_health);
	ClassDB::bind_method(D_METHOD("get_local_player_team"), &NovaSimulation::get_local_player_team);
	ClassDB::bind_method(D_METHOD("drain_effects"), &NovaSimulation::drain_effects);
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
	ClassDB::bind_method(D_METHOD("load_weapon_table", "resource_root", "name"),
	                     &NovaSimulation::load_weapon_table, DEFVAL(String("weapon.def")));
	ClassDB::bind_method(D_METHOD("get_infantry_clip_count"), &NovaSimulation::get_infantry_clip_count);
	ClassDB::bind_method(D_METHOD("set_loco_scale", "scale"), &NovaSimulation::set_loco_scale);
	ClassDB::bind_method(D_METHOD("get_loco_scale"), &NovaSimulation::get_loco_scale);
	ClassDB::bind_method(D_METHOD("get_spawned_count"), &NovaSimulation::get_spawned_count);
	ClassDB::bind_method(D_METHOD("get_brain_count"), &NovaSimulation::get_brain_count);

	BIND_ENUM_CONSTANT(TICK_DIVIDED);
	BIND_ENUM_CONSTANT(TICK_EVERY_PROCESS);

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

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "playing"), "set_playing", "is_playing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tick_mode"), "set_tick_mode", "get_tick_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loco_scale"), "set_loco_scale", "get_loco_scale");
}

void NovaSimulation::_notification(int p_what) {
	if (p_what == NOTIFICATION_PROCESS && playing_ && loaded_) {
		if (tick_mode_ == TICK_EVERY_PROCESS) {
			step();
		} else {
			advance_frame();
		}
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

void NovaSimulation::step() {
	if (!loaded_) return;
	const uint64_t sim_start = perf_now_us();
	if (listen_server_) { // P7 listen server (SP + LAN host) -> the npruntime owner loop
		host_pump();
		last_sim_tick_us_ = perf_now_us() - sim_start;
		return;
	}
	if (joiner_) { // P7 co-op joiner -> the npruntime ClientRuntime (non-authority)
		joiner_pump();
		last_sim_tick_us_ = perf_now_us() - sim_start;
		return;
	}
	// No-net editor/unit path: one authoritative logic tick, no replication.
	apply_player_input_pre_tick();
	world_->run_logic_tick(/*is_authority=*/true);
	last_sim_tick_us_ = perf_now_us() - sim_start;
}

bool NovaSimulation::advance_frame() {
	if (!loaded_) return false;
	// One host frame = one logic tick (the original's 62 Hz engine tick). The WAC VM
	// self-gates to every 62nd tick and the BMS evaluator quarter-passes every 16th,
	// inside their systems — exactly where the original keeps those dividers.
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
	}
	++now_tick_;
}

void NovaSimulation::apply_player_input_pre_tick() {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return;
	AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (p) opennova::world::apply_player_body_input(*p, opennova::world::pack_player_body_input(player_input_));
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
	// [orig: CMap_SetupSpawnCamera @0x50cf60 -> Entity_FindBestSpawnPoint @0x50ccc0; net-re §5.2c]
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
                                      bool p_run, bool p_crouch, bool p_prone, bool p_jump,
                                      float p_look_yaw_deg, float p_look_pitch_deg) {
	player_input_.forward = p_forward;
	player_input_.back = p_back;
	player_input_.left = p_left;
	player_input_.right = p_right;
	player_input_.run = p_run;
	// Stance is the host's already-resolved posture (the host owns the key-edge toggle); jump is a
	// per-frame edge the motor consumes once when grounded. [orig: entity+0x12C stance/jump bits]
	player_input_.crouch = p_crouch;
	player_input_.prone = p_prone;
	player_input_.jump = p_jump;
	// Look yaw (mission degrees) -> engine BAM heading, the (90 - yaw) convention used at spawn.
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(p_look_yaw_deg);
	// Look pitch (mission degrees, up positive) -> entity Pitch@+0x14 (BAM32). No 90-offset.
	player_input_.look_pitch =
	    static_cast<int32_t>(static_cast<double>(p_look_pitch_deg) * opennova::world::kBamPerDegree);
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

void NovaSimulation::restart() {
	if (!loaded_ || !have_baseline_) return;
	world_->restore(baseline_); // rewinds registry/vars/env/clock + re-inits systems (incl. AI;
	                            // WacSystem::on_load also resets its 62-tick accumulator)
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

int NovaSimulation::get_mission_variable(int index) const {
	return world_ ? world_->vars.get_mission(index) : 0;
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
