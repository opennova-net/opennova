#pragma once

// The engine's ONE mission boot + state + no-net authoritative tick (ADR 0042
// d3): the promoted body of the retail-mission test rig, now the single
// implementation every embedder drives — the Godot Simulation binding (the
// shell TickTarget, which owns a kernel), the dedicated host, and the ctests
// (tests/common/retail_mission_files supplies retail paths only). It owns the
// world and its systems (AI, WAC, BMS events, collision, occlusion), the sim
// asset caches (models, collision pose, root motion, clip index), the seat
// specs and ai-profile installs, the terrain field store, the weapon/ammo
// tables, and the local-player frame state (input, weapon, loadout, view,
// stance latch, look accumulators). boot() is THE one filler of
// run_mission_boot's functor table (runtime_boot.h — the ctest-locked order).
//
// Deliberately NOTHING net: the no-net tick has headless consumers that must
// not link the wire stack (the group order — runtime never includes net). The
// net half — the SP listen bring-up, the local C2S drain, the per-tick
// session frame — is inmatch::listen_host (engine/net/inmatch/listen_host.h),
// which drives this kernel through the public per-tick legs below and
// interposes at boot through the bringup_net_session hook.

#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/mission/event_runtime.h>
#include <runtime/mission/promote.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/simassets/adm_clip_index.h>
#include <runtime/simassets/adm_root_motion.h>
#include <runtime/simassets/collision_resolve.h>
#include <runtime/simassets/mounted_pose.h>
#include <runtime/simassets/sim_collision_pose.h>
#include <runtime/simassets/sim_model_cache.h>
#include <runtime/terrain_query/terrain_field_store.h>
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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::mission {

struct KernelBootOptions {
	bool playable = true;   // spawn the authoritative side's own player after load
	bool wac = true;        // game.wac / server.wac / <mission>.wac when present
	bool collision = true;
	bool seat_specs = true; // the native seat/mount table (S16); off = the bare promote
	// The session g_GameType word the spawn-marker select filters on. The
	// embedder derives it (game_type::for_mission_mode over the mission
	// header's mode bit — npwire's mapping) so this kernel stays below net/;
	// 0 is the SP fallback arm.
	uint32_t game_type = 0;
	std::string infantry_adm = kDefaultInfantryAdm;
	// The net half's session bring-up (inmatch::listen_host::bringup),
	// invoked between the world wiring and register_mission_systems — exactly
	// where the SP listen host stands up inside the load
	// [orig: SinglePlayer_StartMission @0x561af0]. Null = the bare no-net
	// kernel (the AI-path/convoy drives).
	std::function<void()> bringup_net_session;
};

class MissionKernel : public world::IMountedPoseProvider,
					  public world::ICollisionSectionMatrixProvider {
public:
	MissionKernel();
	~MissionKernel() override;
	MissionKernel(const MissionKernel &) = delete;
	MissionKernel &operator=(const MissionKernel &) = delete;

	// Mount `root` (packed install first, loose tree otherwise), read
	// `mission_file_name` through the mount and parse it; items.def when
	// present. `expansion` names the expansion mount to prefer (e.g.
	// "revx02"); the caller checks index.mounted_expansion() when the leg
	// depends on it.
	bool open(const std::string &root, const std::string &mission_file_name,
			std::string &error, const std::string &expansion = std::string());
	// Adopt an already-parsed mission document over the embedder's own file
	// source (the shell's mounted-resource reader; a ctest's in-memory map).
	// open() is this plus the mount + read + parse legs.
	void open_document(bms::File mission_doc, std::string mission_file_basename,
			BootFileSource files);
	// The S9 boot (runtime_boot.h) over the opened mission — the ONE filler
	// of run_mission_boot's step table. The embedder builds terrain_store
	// FIRST when it has terrain (terrain_field_store_build over its parsed
	// cpt/trn documents — the format-typed leg stays on the far side of the
	// ADR 0020 seam); has_terrain is terrain_store.valid().
	bool boot(const KernelBootOptions &options, std::string &error);

	// One bare no-net authoritative logic tick between the local-player pumps
	// (the AI-path and convoy drives; a live session orders the same legs
	// around its session pump — inmatch::listen_host::frame).
	void tick_no_net();

	// --- the per-tick legs a session frame orders around its pump -----------
	// Pack the frame input onto the local player's body before the logic tick
	// (the view-flag stamps ride along); no local player = no-op.
	void apply_player_input_pre_tick();
	// The post-tick local pumps in retail order: the sim-wrote-the-view fold,
	// the per-frame view promoter, then the equipped-slot FSM pump.
	void run_local_player_post_tick();
	// Ground every soldier that appeared since the previous sweep on its OWN
	// model .adm (D-INF-6); also the session pump's before-server-tick hook.
	void resolve_new_infantry_adm_ids();
	// Reset the frame-input state and seed the look heading from the (auto-)
	// spawned local player's facing — the session bring-up's tail.
	void reset_local_player_input_to_player_facing();
	// Restore the post-PreMission world snapshot; false when none was taken.
	bool restore_baseline();

	// --- the local player ----------------------------------------------------
	bool has_local_player() const;
	world::Entity *player();
	const world::Entity *player() const;
	world::AiEntity *player_ai();
	world::Vec3 player_position() const; // mission space (Z-up)
	std::string player_anim_key() const; // "anim_<state>"
	int32_t player_health() const;
	// The movement keys the pre-tick packs onto the body (look rides look()).
	world::PlayerInput input;
	// Mouse pixels onto the look angles (the center-lock accumulator).
	void look(float dx_px, float dy_px);
	// Point the look straight at a mission-space target from a mission-space
	// eye (absolute heading + pitch, engine BAM frame).
	void aim_at(const world::Vec3 &eye, const world::Vec3 &target);
	void teleport_local_player(const world::Vec3 &mission_pos, double yaw_deg,
			double pitch_deg);
	void set_weapon_input(bool fire_held, bool fire_pressed, bool reload_pressed);
	// The by-name weapon install from the retained weapon.def rows.
	bool install_weapon(const std::string &weapon_name,
			bool preserve_slot_state = false);
	// The USE-ITEM mount toggle.
	bool toggle_mount();
	world::LocalPlayerViewFrame view_frame();
	// The Player_CanFireWeapon verdict the body updater and the HUD share
	// [orig: @0x5cf7c7..0x5cf886; Scoped helper @0x4dcc80; Sighted helper
	// @0x4dcd30].
	bool local_player_can_fire(const world::AiEntity *body) const;

	// --- entities ------------------------------------------------------------
	world::Entity *by_net_id(uint16_t ssn);
	world::Entity *by_bms_id(int32_t bms_id);
	world::AiEntity *ai_for(world::EntityHandle h);
	// Both stores: the registry position and the AI 16.16 mirror.
	void set_entity_position(world::EntityHandle h, const world::Vec3 &mission_pos);
	void set_entity_health(world::EntityHandle h, int32_t hp);

	// --- terrain queries -----------------------------------------------------
	bool has_terrain() const { return terrain_store.valid(); }
	// The renderer-accurate column height under a mission x/y, world units.
	float ground_height(float mission_x, float mission_y) const;

	// --- observation ---------------------------------------------------------
	std::vector<world::Effect> drain_effects();
	std::vector<world::CollisionWorld::DebugInstance> collision_instances(
			const world::Vec3 &anchor, float range_units, int32_t max_instances);
	std::vector<world::CollisionWorld::DebugHitboxEntity> hitboxes(
			const world::Vec3 &anchor, float range_units, int32_t max_entities,
			int32_t max_faces);

	// --- the mounted root and the mission ------------------------------------
	ResourceIndex index;
	std::string root_dir;
	std::string mission_name;
	std::string mission_basename;
	bms::File mission;
	DefItemsFile items{};
	bool items_ok = false;

	// --- the world and its systems -------------------------------------------
	world::World world;
	world::AiSystem ai;
	BmsEventSystem events;
	wac::WacSystem wac;
	bool wac_loaded = false;
	world::CollisionWorld collision;
	world::OcclusionWorld occlusion;
	simassets::SimCollisionPoseProvider collision_pose;
	simassets::SimModelCache models;
	simassets::CollisionResolveState collision_state;
	simassets::AdmRootMotion root_motion;
	std::vector<ItemSeatSpec> seat_specs;
	std::unordered_map<int32_t, std::string> mounted_graphics;
	std::vector<PromoteOptions::AiProfileRow> ai_profiles;
	PromoteResult promo;
	int collision_attached = 0;
	int text_source = 0; // mission::MissionTextSource
	size_t text_size = 0;
	world::World::Snapshot baseline;
	bool have_baseline = false;

	// --- terrain (the engine's one owning field store, ADR 0042 d4) ----------
	// The same store Simulation::set_terrain_height_field fills, so the
	// kernel grounds and surface-picks on exactly the game's field. The
	// embedder fills it BEFORE boot() from its parsed terrain documents.
	terrain::TerrainFieldStore terrain_store;

	// --- the local player's weapon and view ----------------------------------
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

	// world::IMountedPoseProvider
	bool resolve_mounted_pose(world::World &w, const world::Entity &carrier,
			const world::Seat &seat, world::MountedPose &out) override;
	// world::ICollisionSectionMatrixProvider
	bool ensure_collision_instance(world::World &w, world::EntityHandle entity) override;
	bool build_section_matrices(world::World &w, world::EntityHandle entity,
			int32_t model_id, const world::CollisionMatrix &entity_world,
			const world::CollisionModel &model,
			std::vector<world::CollisionMatrix> &out) override;

private:
	void wire_terrain();
	void wire_collision();
	std::function<PromoteOptions::AiProfileDefaults(int32_t)> ai_profile_defaults_fn() const;
	void sync_local_mounted_input_heading();
	int spawn_local_player_at_start(uint32_t game_type);
	bool load_mission_into_world();
	void finish_load();
	bool load_weapon_table();
	bool load_ammo_table();

	BootFileSource files_;
	// The per-boot net bring-up hook (KernelBootOptions::bringup_net_session).
	std::function<void()> bringup_net_session_;
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

} // namespace opennova::mission
