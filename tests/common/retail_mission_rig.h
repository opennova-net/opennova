// The retail-mission rig the asset-gated world/mission ctests build on: a
// mounted retail root (a packed install or a loose asset tree), one real
// mission booted through the engine's own boot policy
// (mission::run_mission_boot, the shell's step order with engine-backed step
// bodies), and the no-net authoritative tick the SP listen server's own logic
// tick is (World::run_logic_tick + the local view/weapon pumps). Every read
// and mutation a ported probe needs goes through the same engine seams the
// Godot Simulation binding drives, so a ctest here measures the engine, not a
// harness of its own (tests/mission/ai_path_conformance_test.cpp records why
// the seat specs, the root motion, the item traits, the terrain and the
// collision are not optional: without them the rig measures its omissions).
//
// Gating is the caller's: open() takes the root from retail::install() /
// retail::assets() and reports a missing file so the test can retail::skip.
#ifndef OPENNOVA_TEST_RETAIL_MISSION_RIG_H
#define OPENNOVA_TEST_RETAIL_MISSION_RIG_H

#include <base/io/bam.h>
#include <base/resource_index/resource_index.h>
#include <formats/cpt/cpt.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/trn/trn.h>
#include <net/inmatch/session.h>
#include <net/netsim/idatagram_socket.h>
#include <net/netsim/loopback_channel.h>
#include <net/npruntime/client_runtime.h>
#include <net/npruntime/host_session.h>
#include <runtime/mission/event_runtime.h>
#include <runtime/mission/promote.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/simassets/adm_clip_index.h>
#include <runtime/simassets/adm_root_motion.h>
#include <runtime/simassets/collision_resolve.h>
#include <runtime/simassets/mounted_pose.h>
#include <runtime/simassets/sim_collision_pose.h>
#include <runtime/simassets/sim_model_cache.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/player_input.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_look.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/vehicle_mount.h>
#include <runtime/world/world.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::testrig {

struct BootOptions {
	bool playable = true;   // spawn the host's own player after load
	bool wac = true;        // game.wac / server.wac / <mission>.wac when present
	bool terrain = true;    // the mission's .cpt/.trn height field
	bool collision = true;
	bool seat_specs = true; // the native seat/mount table (S16); off = the bare promote
	// The SP listen server (ADR 0009/0012): the npruntime host session over an
	// in-process loopback, whose Server_TickUpdate owns the logic tick, the
	// WAC 'humans' gate and the SP kill tallies. Off = the bare no-net world
	// tick (what the AI-path and convoy tests drive).
	bool listen_server = true;
	std::string infantry_adm = mission::kDefaultInfantryAdm;
};

// The 62.5 Hz logic-tick clock in mission seconds.
constexpr double kTickSeconds = 1.0 / 62.5;
inline int ticks_for_seconds(double seconds) {
	return static_cast<int>(seconds / kTickSeconds + 0.5);
}

constexpr double kBamPerRad = opennova::io::kBamPerRadian;
constexpr double kBamPerDeg = 4294967296.0 / 360.0;

inline int32_t bam_from_radians(double radians) {
	return static_cast<int32_t>(static_cast<int64_t>(std::llround(radians * kBamPerRad)));
}

class RetailMissionRig : public inmatch::TickTarget,
						 public world::IMountedPoseProvider,
						 public world::ICollisionSectionMatrixProvider {
public:
	RetailMissionRig();
	~RetailMissionRig() override;
	RetailMissionRig(const RetailMissionRig &) = delete;
	RetailMissionRig &operator=(const RetailMissionRig &) = delete;

	// Mount `root_dir` (packed install first, loose tree otherwise), read
	// `mission_name` through the mount and parse it; items.def when present.
	// `expansion` names the expansion mount to prefer (e.g. "revx02"); the caller
	// checks index.mounted_expansion() when the leg depends on it.
	bool open(const std::string &root_dir, const std::string &mission_name, std::string &error,
			const std::string &expansion = std::string());
	// The S9 boot (runtime_boot.h) over the opened mission.
	bool boot(const BootOptions &options, std::string &error);

	// One authoritative logic tick: the listen server's owner pump (the input
	// pre-tick, host_session_pump's Server_TickUpdate, the local view promoter,
	// the equipped-slot FSM pump, the host's own ClientState fold), or with
	// listen_server off the bare World::run_logic_tick between the same pumps.
	void tick();
	void tick(int count);

	// --- the local player ----------------------------------------------------
	bool has_local_player() const;
	world::Entity *player();
	const world::Entity *player() const;
	world::AiEntity *player_ai();
	world::Vec3 player_position() const; // mission space (Z-up)
	std::string player_anim_key() const;  // "anim_<state>"
	int32_t player_health() const;
	// The movement keys the pre-tick packs onto the body (look rides look()).
	world::PlayerInput input;
	// Mouse pixels onto the look angles (the center-lock accumulator).
	void look(float dx_px, float dy_px);
	// Point the look straight at a mission-space target from a mission-space
	// eye (absolute heading + pitch, engine BAM frame).
	void aim_at(const world::Vec3 &eye, const world::Vec3 &target);
	void teleport_local_player(const world::Vec3 &mission_pos, double yaw_deg, double pitch_deg);
	void set_weapon_input(bool fire_held, bool fire_pressed, bool reload_pressed);
	// The by-name weapon install from the retained weapon.def rows.
	bool install_weapon(const std::string &weapon_name, bool preserve_slot_state = false);
	// The USE-ITEM mount toggle.
	bool toggle_mount();
	world::LocalPlayerViewFrame view_frame();

	// --- entities ------------------------------------------------------------
	world::Entity *by_net_id(uint16_t ssn);
	world::Entity *by_bms_id(int32_t bms_id);
	world::AiEntity *ai_for(world::EntityHandle h);
	// Both stores: the registry position and the AI 16.16 mirror.
	void set_entity_position(world::EntityHandle h, const world::Vec3 &mission_pos);
	void set_entity_health(world::EntityHandle h, int32_t hp);

	// --- terrain queries -----------------------------------------------------
	bool has_terrain() const { return terrain.valid(); }
	// The renderer-accurate column height under a mission x/y, world units.
	float ground_height(float mission_x, float mission_y) const;

	// --- observation -----------------------------------------------------------
	std::vector<world::Effect> drain_effects();
	std::vector<world::CollisionWorld::DebugInstance> collision_instances(
			const world::Vec3 &anchor, float range_units, int32_t max_instances);
	std::vector<world::CollisionWorld::DebugHitboxEntity> hitboxes(
			const world::Vec3 &anchor, float range_units, int32_t max_entities, int32_t max_faces);

	// --- the mounted root and the mission ------------------------------------------
	ResourceIndex index;
	std::string root_dir;
	std::string mission_name;
	std::string mission_basename;
	bms::File mission;
	DefItemsFile items{};
	bool items_ok = false;

	// --- the world and its systems ---------------------------------------------------
	world::World world;
	world::AiSystem ai;
	mission::BmsEventSystem events;
	wac::WacSystem wac;
	bool wac_loaded = false;
	world::CollisionWorld collision;
	world::OcclusionWorld occlusion;
	simassets::SimCollisionPoseProvider collision_pose;
	simassets::SimModelCache models;
	simassets::CollisionResolveState collision_state;
	simassets::AdmRootMotion root_motion;
	std::vector<mission::ItemSeatSpec> seat_specs;
	std::unordered_map<int32_t, std::string> mounted_graphics;
	std::vector<mission::PromoteOptions::AiProfileRow> ai_profiles;
	mission::PromoteResult promo;
	int collision_attached = 0;
	int text_source = 0; // mission::MissionTextSource
	size_t text_size = 0;
	world::World::Snapshot baseline;
	bool have_baseline = false;

	// --- the SP listen server (npruntime) ----------------------------------------------
	bool listen_server = false;
	netsim::LoopbackChannel host_loop; // the host's own dcb-2 client; declared before the runtime
	np::HostOwner host_owner;
	std::unique_ptr<np::ClientRuntime> client_runtime;

	// --- terrain data (the mounted cpt/trn) -----------------------------------
	CptFile cpt;
	TrnConfig trn;
	std::vector<uint16_t> heightmap;
	std::vector<int> sector_grid;
	terrain::TerrainHeightField terrain;

	// --- the local player's weapon and view --------------------------------------------
	DefWeaponsFile weapon_defs{};
	bool weapon_defs_ok = false;
	bool ammo_ok = false;
	world::LocalPlayerWeapon weapon;
	// The resident kit: the mission's loadout/availability chunks promoted
	// through the SP gate at load_weapon_table, then the Player_InitPlayer
	// weapon leg (the spawn-default select + the slot pool the reloads refill).
	world::LocalPlayerLoadout loadout;
	world::WeaponInventory inventory;
	bool inventory_valid = false;
	world::PlayerViewState view;
	world::LocalPlayerViewTracker view_tracker;
	world::PlayerLookSettings look_settings;
	simassets::AdmClipIndex clip_index;

	// The native pose counters the soak gates on (the binding's masked-failure
	// signal): queries answered by the engine providers and their declines.
	int collision_queries = 0;
	int collision_declines = 0;
	int mounted_queries = 0;
	int mounted_declines = 0;
	int mounted_evaluations = 0;
	int mounted_cache_hits = 0;

	// inmatch::TickTarget
	inmatch::TickOutcome advance_mission_tick(const inmatch::TickInput &input) override;
	bool reset_mission_to_baseline(inmatch::SessionError &error) override;
	void close_mission() override;
	// world::IMountedPoseProvider
	bool resolve_mounted_pose(world::World &w, const world::Entity &carrier,
			const world::Seat &seat, world::MountedPose &out) override;
	// world::ICollisionSectionMatrixProvider
	bool ensure_collision_instance(world::World &w, world::EntityHandle entity) override;
	bool build_section_matrices(world::World &w, world::EntityHandle entity, int32_t model_id,
			const world::CollisionMatrix &entity_world, const world::CollisionModel &model,
			std::vector<world::CollisionMatrix> &out) override;

private:
	bool load_terrain(std::string &error);
	void wire_terrain();
	void wire_collision();
	uint32_t mission_game_type() const;
	void bringup_host_runtime();
	void host_pump();
	void drain_host_client_gameplay_requests();
	static void before_server_tick(void *context);
	void apply_player_input_pre_tick();
	bool local_player_can_fire(const world::AiEntity *body) const;
	void sync_local_mounted_input_heading();
	void resolve_new_infantry_adm_ids();
	int spawn_local_player_at_start();
	bool load_mission_into_world();
	void finish_load();
	bool load_weapon_table();
	bool load_ammo_table();

	mission::BootFileSource files_;
	bool infantry_adm_retained_ = false;
	int infantry_adm_resolved_ai_count_ = 0;
	float look_accum_x_ = 0.0f;
	float look_accum_y_ = 0.0f;
	int stance_latch_ = 0;

	struct MountedPoseRest {
		simassets::MountedPosePartMatrices parts;
	};
	struct MountedPoseLive {
		uint32_t time_ms = 0;
		std::array<int32_t, THREEDI_CTRL_REGISTER_COUNT> controls{};
		bool valid = false;
		simassets::MountedPosePartMatrices parts;
	};
	std::map<const Threedi3di3 *, MountedPoseRest> mounted_rest_cache_;
	std::map<const Threedi3di3 *, std::vector<MountedPoseLive>> mounted_live_cache_;
	uint32_t mounted_cache_tick_ = 0xFFFFFFFFu;
};

// Mission-space helpers.
inline float planar_distance(const world::Vec3 &a, const world::Vec3 &b) {
	const float dx = a.x - b.x, dy = a.y - b.y;
	return std::sqrt(dx * dx + dy * dy);
}
inline float distance(const world::Vec3 &a, const world::Vec3 &b) {
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline world::Vec3 ai_position(const world::AiEntity &e) {
	return world::Vec3{e.pos[0] / 65536.0f, e.pos[1] / 65536.0f, e.pos[2] / 65536.0f};
}

} // namespace opennova::testrig

#endif // OPENNOVA_TEST_RETAIL_MISSION_RIG_H
