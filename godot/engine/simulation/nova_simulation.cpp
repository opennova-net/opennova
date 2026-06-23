#include "simulation/nova_simulation.h"

#include <wac/compiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

#include "netsim/connection.h"

#include <mission/bms.h>
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
	// SP listen server (ADR 0009/0011): NetSystem runs AHEAD of WAC (net-before-logic,
	// [orig: Game_ProcessMainFrame @ 0x5263f0]) and World::net routes through the
	// serializing sink. register_mission_systems appends WAC->BMS->AI after it, so the
	// system order becomes net -> WAC -> BMS -> AI. reset_world recreated a fresh World
	// (empty systems_, net == LocalSink), so this re-wires cleanly on every (re)load.
	if (listen_server_ && net_ && net_sink_ && client_view_) {
		world_->net = net_sink_.get();
		// Reset the connection table to just the host's own loopback (connection 0);
		// co-op peers re-handshake into fresh connections after each (re)load. This
		// keeps the SP listen server byte-identical (one loopback connection) and
		// drops any stale per-peer transports from a prior mission.
		net_->clear_connections();
		net_->add_connection(opennova::netsim::Connection{
				loopback_.get(), opennova::netsim::TransportMode::Loopback, {}, 0});
		remote_peers_.clear();
		world_->add_system(net_.get());
		client_view_->state() = opennova::netsim::ClientState{};
		loopback_->clear();
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
	ClassDB::bind_method(D_METHOD("is_listen_server"), &NovaSimulation::is_listen_server);
	ClassDB::bind_method(D_METHOD("enable_host_listen", "port"), &NovaSimulation::enable_host_listen);
	ClassDB::bind_method(D_METHOD("is_host_listening"), &NovaSimulation::is_host_listening);
	ClassDB::bind_method(D_METHOD("get_host_listen_port"), &NovaSimulation::get_host_listen_port);
	ClassDB::bind_method(D_METHOD("get_host_peer_count"), &NovaSimulation::get_host_peer_count);
	ClassDB::bind_method(D_METHOD("admit_test_remote_peer", "position", "yaw_deg", "team"), &NovaSimulation::admit_test_remote_peer);
	ClassDB::bind_method(D_METHOD("spawn_local_player", "position", "yaw_deg", "team"), &NovaSimulation::spawn_local_player);
	ClassDB::bind_method(D_METHOD("spawn_local_player_at_start"), &NovaSimulation::spawn_local_player_at_start);
	ClassDB::bind_method(D_METHOD("has_local_player"), &NovaSimulation::has_local_player);
	ClassDB::bind_method(D_METHOD("set_player_input", "forward", "back", "left", "right", "run", "crouch", "prone", "jump", "look_yaw_deg", "look_pitch_deg"), &NovaSimulation::set_player_input);
	ClassDB::bind_method(D_METHOD("get_local_player_position"), &NovaSimulation::get_local_player_position);
	ClassDB::bind_method(D_METHOD("get_local_player_yaw_deg"), &NovaSimulation::get_local_player_yaw_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_pitch_deg"), &NovaSimulation::get_local_player_pitch_deg);
	ClassDB::bind_method(D_METHOD("get_local_player_anim_slot"), &NovaSimulation::get_local_player_anim_slot);
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
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_phase", "index", "channel"), &NovaSimulation::get_entity_part_anim_phase);
	ClassDB::bind_method(D_METHOD("get_entity_part_anim_active", "index", "channel"), &NovaSimulation::get_entity_part_anim_active);
	ClassDB::bind_method(D_METHOD("get_entity_anim_slot", "index"), &NovaSimulation::get_entity_anim_slot);
	ClassDB::bind_method(D_METHOD("get_entity_hidden", "index"), &NovaSimulation::get_entity_hidden);
	ClassDB::bind_method(D_METHOD("get_present_snapshot"), &NovaSimulation::get_present_snapshot);
	ClassDB::bind_method(D_METHOD("get_present_stride"), &NovaSimulation::get_present_stride);
	ClassDB::bind_method(D_METHOD("set_terrain_height_field", "terrain"), &NovaSimulation::set_terrain_height_field);
	ClassDB::bind_method(D_METHOD("set_item_seat_specs", "specs"), &NovaSimulation::set_item_seat_specs);
	ClassDB::bind_method(D_METHOD("set_infantry_anim_map", "resource_root", "adm_name"), &NovaSimulation::set_infantry_anim_map);
	ClassDB::bind_method(D_METHOD("resolve_infantry_adm_ids", "resource_root", "item_db"), &NovaSimulation::resolve_infantry_adm_ids);
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
	BIND_ENUM_CONSTANT(PF_ANIM_SLOT);
	BIND_ENUM_CONSTANT(PF_ANIM_STATE);
	BIND_ENUM_CONSTANT(PF_ANIM_PHASE_TICKS);
	BIND_ENUM_CONSTANT(PF_HIDDEN);
	BIND_ENUM_CONSTANT(PF_ALIVE);
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
	return true;
}

bool NovaSimulation::load_mission_file(const String &path) {
	reset_world();
	opennova::bms::File file;
	std::string err;
	if (!opennova::bms::parse_file(std::string(path.utf8().get_data()), file, err)) {
		return false;
	}
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
	return true;
}

void NovaSimulation::build_demo_mission() {
	reset_world();
	opennova::bms::File file = make_demo_mission();
	promo_ = opennova::mission::promote_mission(file, *world_, *ai_, promote_options());
	finish_load(file);
}

void NovaSimulation::step() {
	if (!loaded_) return;
	const uint64_t sim_start = perf_now_us();
	apply_player_input_pre_tick(); // net-before-logic: player input -> body input
	host_net_poll();          // co-op host: drain the socket + handshake/admit/route, before logic
	world_->run_logic_tick(); // one logic tick: cache + WAC + BMS + AI, then ++logic_tick
	last_sim_tick_us_ = perf_now_us() - sim_start;
	const uint64_t net_start = perf_now_us();
	net_tick(); // serialize + loopback-decode when the listen server is on (no-op otherwise)
	host_net_flush();         // co-op host: advance handshakes + ship per-peer S2C (no-op otherwise)
	last_net_tick_us_ = perf_now_us() - net_start;
}

bool NovaSimulation::advance_frame() {
	if (!loaded_) return false;
	// One host frame = one logic tick (the original's 62 Hz engine tick). The WAC VM
	// self-gates to every 62nd tick and the BMS evaluator quarter-passes every 16th,
	// inside their systems — exactly where the original keeps those dividers.
	//
	// Listen-server frame order [orig: Game_ProcessMainFrame @ 0x5263f0]:
	//   input -> net(drain C2S) -> run_logic_tick(WAC/BMS/AI) -> net(emit S2C) -> present.
	// The C2S drain is NetSystem::tick (system index 0, runs at the top of the loop);
	// the S2C emit + the local client's decode happen in net_tick(), after the logic.
	const uint64_t sim_start = perf_now_us();
	apply_player_input_pre_tick(); // input -> the local player's body input, before logic
	host_net_poll();          // co-op host: poll socket -> handshake/admit/route, before logic
	world_->run_logic_tick();
	last_sim_tick_us_ = perf_now_us() - sim_start;
	const uint64_t net_start = perf_now_us();
	net_tick();
	host_net_flush();         // co-op host: advance handshakes + ship per-peer S2C 0x0A
	last_net_tick_us_ = perf_now_us() - net_start;
	return true;
}

// The post-logic half of the listen-server frame: serialize the live world into one
// S2C 0x0A frame, loop it back in-process, and let the local client decode it into the
// ClientState the present pass reads. No-op when the listen server is off.
void NovaSimulation::net_tick() {
	if (!listen_server_ || !net_ || !client_view_ || !loopback_) return;
	net_->emit_s2c(*world_, compute_net_anchor());
	client_view_->pump(*loopback_);
}

void NovaSimulation::apply_player_input_pre_tick() {
	if (!world_ || !world_->ai || !world_->cached.local_player.valid()) return;
	AiEntity *p = world_->ai->for_handle(world_->cached.local_player);
	if (p) opennova::world::apply_player_body_input(*p, opennova::world::pack_player_body_input(player_input_));
}

bool NovaSimulation::spawn_local_player(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!loaded_ || !world_ || !world_->ai) return false;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x, -z, y): the inverse of the present (x,y,z) -> (x, z, -y) remap.
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
	spawn.team = static_cast<uint8_t>(p_team);
	const opennova::world::EntityHandle h = opennova::world::spawn_player(*world_, spawn);
	if (!h.valid()) return false;
	// Seed the look heading to the spawn facing so the body starts aligned. [(90 - yaw) BAM]
	player_input_ = opennova::world::PlayerInput{};
	player_input_.look_heading = opennova::world::bam_heading_from_mission_yaw_deg(p_yaw_deg);
	return true;
}

int NovaSimulation::spawn_local_player_at_start() {
	if (!loaded_ || !world_ || !world_->ai) return -1;
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

int NovaSimulation::get_local_player_anim_slot() const {
	if (!world_ || !world_->cached.local_player.valid()) return -1;
	// The same Entity.anim_slot the present pass reads for NPC models (written by the infantry
	// motor mirror, infantry.cpp). The avatar is host-managed and not in the present registry,
	// so main_game drives its body clip from this getter.
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e ? e->anim_slot : -1;
}

String NovaSimulation::get_local_player_anim_key() const {
	// The local player's full anim-state clip key ("anim_<name>"), straight from the motor's
	// selected state. Unlike the 8-slot BodyAnim enum (get_local_player_anim_slot), this carries
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
	out["anim_slot"] = ent ? ent->anim_slot : -1;
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

int NovaSimulation::get_entity_anim_slot(int p_index) const {
	if (!ai_ || !world_) return -1;
	AiEntity *e = ai_->at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = world_->registry.get(e->handle);
	return ent ? ent->anim_slot : -1;
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
	// ADR 0011 Decision 1: under the listen server the present pass reads the state the
	// LOCAL CLIENT decoded off the wire, not the authoritative sim directly — so SP
	// renders exactly what a networked peer would. Off (the editor/preview default), the
	// direct AI-pool path below is unchanged.
	if (listen_server_ && client_view_) {
		PackedFloat32Array out = present_snapshot_from_client_view();
		last_present_entity_count_ = static_cast<int>(out.size() / PF_STRIDE);
		last_present_snapshot_us_ = perf_now_us() - start_us;
		return out;
	}
	PackedFloat32Array out;
	if (!ai_ || !world_) {
		last_present_entity_count_ = 0;
		last_present_snapshot_us_ = perf_now_us() - start_us;
		return out;
	}
	const int count = ai_->count();
	last_present_entity_count_ = count;
	out.resize(static_cast<int64_t>(count) * PF_STRIDE);
	float *w = out.ptrw();
	for (int i = 0; i < count; ++i) {
		float *r = w + static_cast<int64_t>(i) * PF_STRIDE;
		// Defaults for a missing/invalid entity: -1 ids, identity transform, inactive, dead/hidden.
		r[PF_KIND] = -1.0f; r[PF_INDEX] = -1.0f; r[PF_BMS_ID] = 0.0f; r[PF_NET_ID] = 0.0f;
		r[PF_POS_X] = 0.0f; r[PF_POS_Y] = 0.0f; r[PF_POS_Z] = 0.0f;
		r[PF_PITCH_DEG] = 0.0f; r[PF_YAW_DEG] = 0.0f; r[PF_ROLL_DEG] = 0.0f;
		r[PF_PHASE1] = 0.0f; r[PF_ACTIVE1] = 0.0f; r[PF_PHASE2] = 0.0f; r[PF_ACTIVE2] = 0.0f;
		r[PF_ANIM_SLOT] = -1.0f; r[PF_ANIM_STATE] = -1.0f; r[PF_ANIM_PHASE_TICKS] = 0.0f;
		r[PF_HIDDEN] = 0.0f; r[PF_ALIVE] = 0.0f;

		AiEntity *e = ai_->at(i);
		if (!e) continue;
		const opennova::world::Entity *ent = world_->registry.get(e->handle);
		if (ent) {
			r[PF_KIND] = static_cast<float>(ent->spawn_origin >> 24);        // [orig promote: (kind<<24)|index]
			r[PF_INDEX] = static_cast<float>(ent->spawn_origin & 0xFFFFFF);
			r[PF_BMS_ID] = static_cast<float>(ent->bms_id);
			r[PF_ANIM_SLOT] = static_cast<float>(ent->anim_slot);
			r[PF_HIDDEN] = ent->hidden ? 1.0f : 0.0f;
			r[PF_ALIVE] = ent->alive ? 1.0f : 0.0f;
		}
		r[PF_NET_ID] = static_cast<float>(e->net_id);
		// mission (x, y, z) 16.16 -> Godot (x, z, -y) world units. [orig render remap: (x, z, -y).]
		r[PF_POS_X] = static_cast<float>(e->pos[0] / kFixed16);
		r[PF_POS_Y] = static_cast<float>(e->pos[2] / kFixed16);
		r[PF_POS_Z] = static_cast<float>(-e->pos[1] / kFixed16);
		// Yaw-only today: MISSION-space yaw degrees for the host basis (bms_to_godot_basis). The brain
		// stores ENGINE-frame heading (90 - yaw); convert back so a moving unit (whose heading is the
		// mover's atan2 bearing) faces its travel direction, not 90 deg off. Pitch/roll reserved (0).
		r[PF_YAW_DEG] = static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(e->heading));
		const int phase1 = e->brain.f[AiBrain::kPartAnimPhase0];
		const int phase2 = e->brain.f[AiBrain::kPartAnimPhase0 + 1];
		r[PF_PHASE1] = static_cast<float>(phase1);
		r[PF_PHASE2] = static_cast<float>(phase2);
		r[PF_ACTIVE1] = (e->brain.f[AiBrain::kPartAnimRate0] != 0 || phase1 != 0) ? 1.0f : 0.0f;
		r[PF_ACTIVE2] = (e->brain.f[AiBrain::kPartAnimRate0 + 1] != 0 || phase2 != 0) ? 1.0f : 0.0f;
		if (e->inf.active) {
			r[PF_ANIM_STATE] = static_cast<float>(e->inf.anim_state);
			r[PF_ANIM_PHASE_TICKS] = static_cast<float>(e->inf.clip_phase);
		}
	}
	last_present_snapshot_us_ = perf_now_us() - start_us;
	return out;
}

void NovaSimulation::enable_listen_server(bool p_enable) {
	listen_server_ = p_enable;
	if (!p_enable) return;
	// Construct the loopback + seam objects once; they persist across reloads (they hold
	// only a LoopbackChannel reference, never a World pointer). finish_load re-wires
	// World::net and re-registers NetSystem on each load.
	if (!loopback_) loopback_ = std::make_unique<opennova::netsim::LoopbackChannel>();
	if (!net_sink_) net_sink_ = std::make_unique<opennova::netsim::SerializingSink>(*loopback_);
	if (!net_) net_ = std::make_unique<opennova::netsim::NetSystem>(*loopback_);
	if (!client_view_) client_view_ = std::make_unique<opennova::netsim::NetClientView>();
}

bool NovaSimulation::enable_host_listen(int p_port) {
	enable_listen_server(true); // the host's own client rides the loopback stack
	if (pump_.is_null()) pump_.instantiate();
	if (pump_->bind_listen(p_port) != 0) {
		host_listen_ = false;
		return false;
	}
	if (!accept_) accept_ = std::make_unique<opennova::HostSessionAccept>();
	accept_->start();
	host_listen_ = true;
	return true;
}

int NovaSimulation::get_host_listen_port() const {
	return (host_listen_ && pump_.is_valid()) ? pump_->local_port() : 0;
}

int NovaSimulation::get_host_peer_count() const {
	return accept_ ? static_cast<int>(accept_->peer_count()) : 0;
}

opennova::PeerAddr NovaSimulation::peer_from_addr(const String &ip, int port) {
	// "a.b.c.d" -> LE octet packing (a | b<<8 | c<<16 | d<<24), the ip_to_le
	// convention PeerAddr uses (octet 0 in the low byte).
	uint32_t packed = 0;
	PackedStringArray parts = ip.split(".");
	if (parts.size() == 4) {
		packed = (static_cast<uint32_t>(parts[0].to_int() & 0xFF)) |
		         (static_cast<uint32_t>(parts[1].to_int() & 0xFF) << 8) |
		         (static_cast<uint32_t>(parts[2].to_int() & 0xFF) << 16) |
		         (static_cast<uint32_t>(parts[3].to_int() & 0xFF) << 24);
	}
	return opennova::PeerAddr{packed, static_cast<uint16_t>(port)};
}

void NovaSimulation::send_datagram(const opennova::PeerAddr &peer,
                                   const std::vector<uint8_t> &dg) {
	if (pump_.is_null() || dg.empty()) return;
	char ipbuf[32];
	std::snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
	              peer.ip & 0xFFu, (peer.ip >> 8) & 0xFFu,
	              (peer.ip >> 16) & 0xFFu, (peer.ip >> 24) & 0xFFu);
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(dg.size()));
	std::memcpy(bytes.ptrw(), dg.data(), dg.size());
	pump_->send_to(String(ipbuf), static_cast<int>(peer.port), bytes);
}

opennova::world::PlayerSpawn NovaSimulation::spawn_from_pose(
		const opennova::HostJoinerPose &pose) const {
	opennova::world::PlayerSpawn spawn;
	// Pose position is mission i32 16.16; PlayerSpawn.position is float mission units.
	spawn.position = {static_cast<float>(pose.pos_x / kFixed16),
	                  static_cast<float>(pose.pos_y / kFixed16),
	                  static_cast<float>(pose.pos_z / kFixed16)};
	// Heading is the joiner's wire heading (i16, sign-ext << 16 = 32-bit BAM); the
	// spawn yaw is mission degrees (90 - heading), the same convention as the host
	// player's spawn and apply_player_intent.
	const int32_t heading_bam = static_cast<int32_t>(pose.heading) << 16;
	spawn.yaw = static_cast<int16_t>(
			std::lround(opennova::world::mission_yaw_deg_from_bam_heading(heading_bam)));
	spawn.team = pose.team;
	return spawn;
}

opennova::world::EntityHandle NovaSimulation::admit_remote_peer(
		const opennova::PeerAddr &peer, const opennova::HostJoinerPose &pose) {
	if (!net_ || !world_) return {};
	auto it = remote_peers_.find(peer);
	if (it == remote_peers_.end()) {
		RemotePeer fresh;
		fresh.transport = std::make_unique<opennova::netsim::UdpSessionTransport>(
				opennova::netsim::UdpSessionTransport::Role::Host);
		const opennova::netsim::Connection conn{fresh.transport.get(),
				opennova::netsim::TransportMode::Client, {}, 0};
		fresh.conn_index = net_->add_connection(conn);
		it = remote_peers_.emplace(peer, std::move(fresh)).first;
	}
	RemotePeer &rp = it->second;
	if (rp.admitted) return net_->connection(rp.conn_index).owned_entity;
	opennova::world::PlayerSpawn spawn = spawn_from_pose(pose);
	spawn.net_id = next_joiner_net_id_++; // distinct SSN per joiner (host is 0xFFF0)
	const opennova::world::EntityHandle h = net_->admit_peer(*world_, rp.conn_index, spawn);
	rp.admitted = h.valid();
	return h;
}

void NovaSimulation::host_net_poll() {
	if (!host_listen_ || pump_.is_null() || !accept_ || !net_ || !world_) return;
	pump_->poll();
	while (pump_->has_inbound()) {
		const Dictionary d = pump_->take_inbound();
		const String ip = d.get("ip", String());
		const int port = d.get("port", 0);
		const PackedByteArray bytes = d.get("bytes", PackedByteArray());
		const opennova::PeerAddr peer = peer_from_addr(ip, port);

		opennova::HostSessionAccept::HandleResult r = accept_->handle_datagram(
				peer, bytes.ptr(), static_cast<size_t>(bytes.size()), net_frame_counter_);
		for (const std::vector<uint8_t> &dg : r.outbound) {
			send_datagram(peer, dg);
		}
		for (const opennova::HostAcceptEvent &ev : r.events) {
			switch (ev.kind) {
				case opennova::HostAcceptEvent::Kind::PeerSpawned:
					admit_remote_peer(peer, ev.pose);
					break;
				case opennova::HostAcceptEvent::Kind::PeerC2SInMatch: {
					auto rp = remote_peers_.find(peer);
					if (rp != remote_peers_.end() && rp->second.transport) {
						for (const opennova::ProtocolMessage &m : ev.in_match_c2s) {
							// Identity-reframe [tag][payload] into the transport inbound
							// FIFO so NetSystem::tick's host_recv drains it next tick.
							std::vector<uint8_t> framed;
							framed.reserve(1 + m.payload.size());
							framed.push_back(m.tag);
							framed.insert(framed.end(), m.payload.begin(), m.payload.end());
							rp->second.transport->push_inbound(framed);
						}
					}
					break;
				}
				case opennova::HostAcceptEvent::Kind::PeerGoodbye: {
					auto rp = remote_peers_.find(peer);
					if (rp != remote_peers_.end()) {
						// Retire the connection in place (nulled transport is skipped by
						// emit_s2c/tick) before destroying the transport it points at.
						if (rp->second.conn_index < net_->connection_count()) {
							net_->connection(rp->second.conn_index).transport = nullptr;
							net_->connection(rp->second.conn_index).owned_entity = {};
						}
						remote_peers_.erase(rp);
					}
					break;
				}
				case opennova::HostAcceptEvent::Kind::PeerHandshakeAdvanced:
				default:
					break;
			}
		}
	}
	++net_frame_counter_;
}

void NovaSimulation::host_net_flush() {
	if (!host_listen_ || pump_.is_null() || !accept_ || !net_) return;
	// Advance the handshake for pre-Spawned peers (drives GameSession::tick so the
	// spawn gate opens) and ship their replies. ~16ms per host frame.
	for (opennova::HostSessionAccept::TickOut &t : accept_->tick_handshakes(16, net_frame_counter_)) {
		for (const std::vector<uint8_t> &dg : t.outbound) {
			send_datagram(t.peer, dg);
		}
	}
	// Ship each admitted peer's per-frame S2C 0x0A (staged by emit_s2c into its
	// transport's outbound FIFO) as a framed SESSION datagram.
	for (auto &kv : remote_peers_) {
		RemotePeer &rp = kv.second;
		if (!rp.admitted || !rp.transport) continue;
		std::vector<uint8_t> raw;
		while (rp.transport->pop_outbound(raw)) {
			if (raw.empty()) continue;
			const uint8_t tag = raw[0];
			const std::vector<uint8_t> body(raw.begin() + 1, raw.end());
			std::vector<uint8_t> dg;
			if (accept_->frame_in_match_s2c(kv.first, tag, body, dg)) {
				send_datagram(kv.first, dg);
			}
		}
	}
}

bool NovaSimulation::admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!host_listen_ || !net_ || !world_) return false;
	opennova::HostJoinerPose pose;
	pose.pos_valid = true;
	// Godot (x,y,z) -> mission (x,-z,y) -> i32 16.16, the inverse of the present remap.
	pose.pos_x = static_cast<int32_t>(std::lround(static_cast<double>(p_position.x) * kFixed16));
	pose.pos_y = static_cast<int32_t>(std::lround(static_cast<double>(-p_position.z) * kFixed16));
	pose.pos_z = static_cast<int32_t>(std::lround(static_cast<double>(p_position.y) * kFixed16));
	const int32_t bam = opennova::world::bam_heading_from_mission_yaw_deg(p_yaw_deg);
	pose.heading = static_cast<int16_t>(bam >> 16);
	pose.team = static_cast<uint8_t>(p_team);
	// A synthetic loopback peer; distinct port per call so repeated admits don't alias.
	const opennova::PeerAddr peer{0x0100007Fu,
			static_cast<uint16_t>(40000 + remote_peers_.size())};
	return admit_remote_peer(peer, pose).valid();
}

opennova::PlayerReplicationState NovaSimulation::compute_net_anchor() const {
	opennova::PlayerReplicationState anchor; // sensible defaults if the world is empty
	if (!world_) return anchor;
	const opennova::world::Entity *subj = nullptr;
	// Prefer the local player handle (Phase 2+); else the first replicated entity so the
	// per-record compressed deltas stay small (the codec is lossy with magnitude).
	if (world_->cached.local_player.valid()) {
		subj = world_->registry.get(world_->cached.local_player);
	}
	if (!subj) {
		world_->registry.for_each([&](const opennova::world::Entity &e) {
			if (subj) return;
			if (opennova::netsim::entity_class_of(e) != opennova::EntityClass::Unknown) {
				subj = &e;
			}
		});
	}
	if (subj) {
		anchor.spawn_x = static_cast<uint32_t>(opennova::world::to_fixed(subj->position.x));
		anchor.spawn_y = static_cast<uint32_t>(opennova::world::to_fixed(subj->position.y));
		anchor.spawn_z = static_cast<uint32_t>(opennova::world::to_fixed(subj->position.z));
	}
	return anchor;
}

PackedFloat32Array NovaSimulation::present_snapshot_from_client_view() const {
	PackedFloat32Array out;
	if (!client_view_ || !world_) return out;
	const opennova::netsim::ClientState &cs = client_view_->state();
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
		r[PF_ANIM_SLOT] = -1.0f; r[PF_ANIM_STATE] = -1.0f; r[PF_ANIM_PHASE_TICKS] = 0.0f;
		r[PF_HIDDEN] = 0.0f; r[PF_ALIVE] = 1.0f;

		// kind/index/bms_id/net_id resolve from the registry entity behind the decoded
		// handle: the host is authoritative, so the placed-node mapping still resolves
		// through MissionEntityRegistry exactly as the AI-pool path does.
		const opennova::world::EntityHandle h{es.handle};
		const opennova::world::Entity *ent = world_->registry.get(h);
		if (ent) {
			r[PF_KIND] = static_cast<float>(ent->spawn_origin >> 24);
			r[PF_INDEX] = static_cast<float>(ent->spawn_origin & 0xFFFFFF);
			r[PF_BMS_ID] = static_cast<float>(ent->bms_id);
			r[PF_NET_ID] = static_cast<float>(ent->net_id);
			r[PF_ANIM_SLOT] = static_cast<float>(ent->anim_slot);
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
		if (world_->ai) {
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
