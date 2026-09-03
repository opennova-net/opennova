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
// session frame — is inmatch::listen_host (engine/runtime/inmatch/listen_host.h),
// which drives this kernel through the public per-tick legs below and
// interposes at boot through the bringup_net_session hook.

#include <base/resource_index/resource_index.h>
#include <runtime/world/vehicle_attach.h>
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
#include <runtime/world/player_spawn.h>
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
	// EVERY WAC diagnostic is fatal and fails the boot (the dedicated golden
	// host's policy — running a partial script is a known wire-parity
	// failure). false = the game's lenient policy: only a compile FAILURE
	// blocks, and a blocked program merely disables scripts with a warning.
	// Feeds wac_layered_load's strict_diagnostics flag.
	bool wac_strict_diagnostics = false;
	bool collision = true;
	bool seat_specs = true; // the native seat/mount table (S16); off = the bare promote
	// A joiner world: never spawns its own player here (L spawns on the
	// name-match inside the joiner pump — the joiner ROLE stays with the
	// embedder, ADR 0042 d3; this only gates the boot's spawn step).
	bool joiner = false;
	// The session g_GameType word the spawn-marker select filters on. The
	// embedder derives it (game_type::for_mission_mode over the mission
	// header's mode bit — npwire's mapping) so this kernel stays below net/;
	// 0 is the SP fallback arm.
	uint32_t game_type = 0;
	std::string infantry_adm = kDefaultInfantryAdm;
	// The <name>.wac mission layer when it differs from the mission file's own
	// basename (the shell's loose-mission seam); empty = mission_basename.
	std::string wac_basename;
	// Authored display names for promote's name_index resolve (the embedder's
	// parsed [PeopleNames] STRNAME%03i table; D-HUD-20). Empty = no names.
	std::function<std::string(int32_t)> people_name_resolver;
	// The net half's session bring-up (inmatch::listen_host::bringup, or
	// bringup_dedicated for a HostOnly embedder),
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
	// --- embedder source seams (ADR 0042 d3) --------------------------------
	// The embedder's already-mounted index (the shell's ResourceRoot). When
	// installed it replaces the kernel's own mount for every asset resolve
	// (models, collision pose rigs, .adm registration, the weapon-table
	// texture index) and clears the pose caches like a root switch; null
	// reverts to the kernel's own open() mount.
	void set_asset_index(const ResourceIndex *asset_index);
	// The embedder's already-parsed items.def (the shell's retained rows).
	// Overrides the open_document parse — the caller keeps it alive for the
	// kernel's lifetime; null reverts to the kernel's own parse.
	void set_items_table(const DefItemsFile *items_table);
	// The live items.def rows every kernel leg reads: the override, else the
	// kernel's own parse, else null (no item db — the gated steps skip).
	const DefItemsFile *items_table() const {
		return items_override_ != nullptr ? items_override_
										  : (items_ok ? &items : nullptr);
	}
	// The S9 boot (runtime_boot.h) over the opened mission — the ONE filler
	// of run_mission_boot's step table. The embedder builds terrain_store
	// FIRST when it has terrain (terrain_field_store_build over its parsed
	// cpt/trn documents — the format-typed leg stays on the far side of the
	// ADR 0020 seam); has_terrain is terrain_store.valid().
	bool boot(const KernelBootOptions &options, std::string &error);

	// One bare no-net authoritative logic tick between the local-player pumps
	// (the AI-path and convoy drives; a live session orders the same legs
	// around its session pump — inmatch::listen_host::frame). The tick's phase
	// attribution lands on `profile`.
	void tick_no_net();

	// --- the weather tick (ADR 0042 d2: ONE engine function) ------------------
	// The retail weather tick after the logic tick [orig:
	// Environment_UpdateWeatherTick @ 0x57e9b0 from Game_ProcessMainFrame
	// @ 0x526774, after Entity_UpdateAllEntities @ 0x52674b]: the world's sim
	// legs, the thunder one-shots into world.weather_sounds, the local quake
	// shake arm, then the installed render owner's color legs. Every embedder
	// tick (the no-net tick, the listen frame, the joiner frame, the dedicated
	// host) runs this once per 62.5 Hz quantum.
	void tick_weather();
	// The render owner (the shell's Weather node); null on a headless host.
	world::IWeatherRenderTick *weather_render = nullptr;
	// The mission-start boundary after the eager WAC execution [orig:
	// Environment_MissionStartInit @ 0x57f1e0 then the 255 complete weather
	// ticks @ 0x57f878..0x57f880]: the currents snap to the just-authored
	// targets, the recovered clamps install, and 255 full ticks settle.
	void settle_weather_mission_start();
	// The precipitation pool's per-render update for a camera at (x, y, z)
	// mission 16.16: the wrap into the camera volume and the re-floor of every
	// wrapped drop on terrain / water / the first entity under it
	// [orig: update_weather_particle_positions @ 0x5dec40 from the drawer].
	void update_precipitation(int32_t cam_x, int32_t cam_y, int32_t cam_z);

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
	// The same reset with an explicit heading (the joiner's spawn/redeploy
	// edges hand the authoritative facing in).
	void reset_local_player_input(int32_t look_heading_bam);
	// Restore the post-PreMission snapshot — the play-start world, the WAC
	// runtime state, the local weapon's epoch resets (the borrowed-UseGun
	// reinstall event), the view reset, and the fresh-soldier .adm re-ground;
	// false when no baseline was taken.
	bool restore_baseline();
	// Re-capture the baseline from the CURRENT state (the shell's sealed
	// mission-start point: post-eager-WAC, fully settled play start).
	void capture_baseline();

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
	// One frame of movement keys: packs the keys plus the sim-owned stance
	// latch onto `input`, runs the witnessed movement-held unscope (while
	// SETTLED at scope on a Scoped weapon, any direction key routes through
	// the full unscope; the ForceScoped pin keeps pinned sights raised), and
	// refreshes the view aggregates. [orig: Player_PackInputStateToEntity
	// @0x4df450 — g_movementKeyHeld @0x4df29c; the unscope route
	// @0x4df4c9..0x4df4ec; the ForceScoped pin @0x4df12d]
	void set_movement_keys(bool forward, bool back, bool left, bool right,
			bool lean_left, bool lean_right, bool jump);
	// Stance SELECT request (0 stand / 1 crouch / 2 prone): mutual exclusion
	// at apply, REFUSED while the equipped weapon has ForceCrouch or the
	// player sits in the UseGun seat. Returns whether the latch changed.
	// [orig: input cases 169/170/172 @0x4e0d77.. -> NapiNPServerMsg_HandleStanceChange
	// @0x501c60; the ForceCrouch gate Entity_CheckWeaponSeatFlags(equipped,
	// 0x40000) @0x4e0d8a; the `parentSlot == 3` gate @0x4e0da0..0x4e0db5]
	bool request_stance(int stance);
	// The sim-owned stance latch (0 stand, 1 crouch, 2 prone) — the
	// dword_B76484 prone-latch equivalent the render-slot drape gate reads.
	int stance_latch() const { return stance_latch_; }
	// Mouse pixels onto the look angles (the center-lock accumulator).
	void look(float dx_px, float dy_px);
	// Point the look straight at a mission-space target from a mission-space
	// eye (absolute heading + pitch, engine BAM frame).
	void aim_at(const world::Vec3 &eye, const world::Vec3 &target);
	void teleport_local_player(const world::Vec3 &mission_pos, double yaw_deg,
			double pitch_deg);
	void set_weapon_input(bool fire_held, bool fire_pressed, bool reload_pressed);
	// The by-name weapon install from the retained weapon.def rows. A
	// same-name install is the MOUNT path unless `allow_same_weapon_rebake`
	// asks for the re-bake that keeps the live slot, serials, latches and
	// scope state (the F3 Weapon window's `auto`/ANIM edits).
	bool install_weapon(const std::string &weapon_name,
			bool preserve_slot_state = false, bool allow_same_weapon_rebake = false);
	// The armory table (weapon.def -> world.weapons + the retained rows), the
	// mission loadout-chunk promotion and the spawn-kit rebuild — the boot's
	// load_weapon_table step over an explicit source so the embedder's
	// table-feed seam shares the one body. `index` resolves texture/model
	// references (null = the kernel's own asset index).
	bool load_weapon_table(const BootFileSource &files, const ResourceIndex *index,
			const std::string &name = "weapon.def");
	// ammo.def -> world.ammo + the weapon round_type resolve.
	bool load_ammo_table(const BootFileSource &files,
			const std::string &name = "ammo.def");
	// The infantry clip set (.adm -> .bad root-motion tracks): clear + register
	// the default map through `adm_index` (null = the kernel's asset index) and
	// re-point the AI. Returns the default map's clip count (0 = nothing
	// loaded; soldiers then stand, as in the original).
	int install_infantry_anim(const std::string &adm_name,
			const ResourceIndex *adm_index = nullptr);
	// (Re)arm the per-entity .adm resolution: every soldier grounds on its OWN
	// model .adm from here on (D-INF-6); the boot's resolve_infantry_adm step
	// and the shell's explicit re-arm share this body.
	void rearm_infantry_adm(const ResourceIndex *adm_index = nullptr);
	// Spawn the authoritative side's own player at the mission's player-START
	// marker, selected the way the original engine does (by game type,
	// FARTHEST from the enemy set). 1 = spawned at a real marker, 0 = origin
	// fallback, -1 = failed. [orig: Server_PositionPlayerForSpawn @0x50cf60 ->
	// Entity_FindBestSpawnPoint @0x50ccc0]
	int spawn_local_player_at_start(uint32_t game_type);
	// Spawn the local player at an explicit pose (the shell's authored-pose
	// and legacy LAN-host spawns): the same spawn tail the marker select runs
	// -- the .adm ground, the input reset seeded from the spawn facing, the
	// view reset. False when the registry refuses the spawn.
	bool spawn_local_player(const world::PlayerSpawn &spawn);
	// The USE-ITEM mount toggle [orig: Input_ProcessFrame release edge
	// @0x49d6dc -> Entity_ToggleVehicleMount @0x436950], including the
	// out-of-session UseGun rejection (session_open gates it).
	bool toggle_mount();
	world::LocalPlayerViewFrame view_frame();
	// The Player_CanFireWeapon verdict the body updater and the HUD share
	// [orig: @0x5cf7c7..0x5cf886; Scoped helper @0x4dcc80; Sighted helper
	// @0x4dcd30].
	bool local_player_can_fire(const world::AiEntity *body) const;
	// The seat/armory labels the HUD draws around the local player: nothing
	// for a dead or absent player; armory mode is the raw entity flag [orig:
	// is_armory_mode = entity Flags & 0x400000 @0x5a32c4]; the nearest-only
	// gate is the CanFire verdict above, computed here so camera changes
	// cannot lag one logic tick.
	void collect_attach_labels(std::vector<world::AttachLabel> &out);
	// The authority's read of the local player's dead bit (the entity flags;
	// a joiner reads its replica through inmatch::ClientRuntime::local_player_dead).
	bool local_player_dead() const;

	// --- the medic call (the dead player's C2S 0x2E) -------------------------
	// The retail client medic-call cooldown: 310 ticks stamped at the send
	// [orig: Input_HandleActionBinding case 217 @0x49b511 `dword_B76804 =
	// 0x136`; decremented once per frame in Player_UpdatePerFrame @0x4de73e;
	// cleared on the local death path @0x4b4d06; net-re 0x2E].
	static constexpr int kMedicRequestCooldownTicks = 0x136;
	int medic_request_cooldown_ticks = 0;
	int medic_request_serial = 0;
	// The action gates past the session/entity checks: a dead local player
	// with the cooldown at zero [orig: case 217 @0x49b4b4..0x49b4da].
	bool medic_request_allowed(bool local_dead) const {
		return local_dead && medic_request_cooldown_ticks == 0;
	}
	// The send stamp: the cooldown and the serial the HUD keys its line on.
	void stamp_medic_request();
	// One per tick: the local death edge zeroes the cooldown, else it counts
	// down [orig: Player_UpdatePerFrame @0x4de736..0x4de744; @0x4b4d06].
	void tick_medic_cooldown(bool local_dead);

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
	// The collision debug views around a mission-space anchor; a negative
	// range_units lifts the range gate (every instance / hitbox).
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
	// The retail is_in_session fact: a net session (listen or dedicated) has
	// been brought up over this kernel. The net bring-ups set it; the bare
	// no-net kernel keeps false. Gates the UseGun null-slot rejection
	// [orig: Entity_AttachToUseGunSlot @0x546c07].
	bool session_open = false;
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
	MissionTextSource text_source = MissionTextSource::kNone;
	size_t text_size = 0;
	world::World::Snapshot baseline;
	bool have_baseline = false;
	wac::WacSystem::RuntimeState wac_baseline;
	bool have_wac_baseline = false;

	// --- terrain (the engine's one owning field store, ADR 0042 d4) ----------
	// The same store Simulation::set_terrain_height_field fills, so the
	// kernel grounds and surface-picks on exactly the game's field. The
	// embedder fills it BEFORE boot() from its parsed terrain documents.
	terrain::TerrainFieldStore terrain_store;

	// --- the tick profile (ADR 0043 d5) ---------------------------------------
	// The ONE collector every tick-side span and count lands on (the world,
	// the AI system, the collision resolver, the host session pump, the
	// replication fan, the client frame, the joiner pump). world.profile
	// points here for the kernel's lifetime; the embedder sets it active with
	// its capture window, folds the touched slots onto its board once per
	// render frame, and resets it.
	devtools::TickProfile profile;

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
	// What the view arbiter reads from the embedder's session (death screen,
	// end-round, the death-camera target): a live embedder refreshes this
	// before each session frame; the bare kernel keeps the no-session default.
	world::LocalViewSessionInputs view_session_inputs;
	// The post-tick pump's wire-facing outcomes, overwritten every pump: a
	// serving embedder relays the reload onto its loopback (the witnessed
	// local reload producer -> the S2C 0x49 broadcast) and a joiner ships the
	// fired round; the bare kernel drops both, having already applied them.
	world::LocalWeaponFiredWire last_fired;
	world::LocalWeaponReloadWire last_reload;
	// A non-negative value is the shell's once-per-frame retail presentation
	// DWORD for the PANM pose clock; -1 = deterministic logic time
	// (simassets::mounted_pose_time_ms consumes it).
	int64_t panm_time_override_ms = -1;

	// The native pose counters the soak gates on (the binding's masked-failure
	// signal): queries answered by the engine providers and their declines.
	int collision_queries = 0;
	int collision_declines = 0;
	int mounted_queries = 0;
	int mounted_declines = 0;
	int mounted_evaluations = 0;
	int mounted_cache_hits = 0;
	size_t mounted_rest_cache_size() const { return mounted_rest_cache_.size(); }

	// Re-point the world/AI/collision systems at the terrain field store and
	// the collision world (the embedder re-layers its own device-fed surface
	// extras — placed tiles, sound profiles — after wire_terrain).
	void wire_terrain();
	// Re-feed the terrain store's water plane from World::env.water_z (the
	// occupant water clamp's 16.16 worldY); wire_terrain runs it, and the
	// embedder calls it after every environment change.
	void sync_water_plane();
	void wire_collision();
	// The post-tick "sim wrote the view" fold, exposed for the embedder's
	// mount-change edges (the joiner's authoritative attach echo).
	void sync_local_mounted_input_heading();

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
	std::function<PromoteOptions::AiProfileDefaults(int32_t)> ai_profile_defaults_fn() const;
	bool load_mission_into_world();
	void finish_load();
	// The live asset source: the embedder's installed index, else the kernel's
	// own open() mount, else null (nothing to resolve against).
	const ResourceIndex *asset_index() const {
		if (external_index_ != nullptr) return external_index_;
		return own_mounted_ ? &index : nullptr;
	}

	BootFileSource files_;
	// The per-boot net bring-up hook (KernelBootOptions::bringup_net_session).
	std::function<void()> bringup_net_session_;
	// The embedder source overrides (set_asset_index / set_items_table).
	const ResourceIndex *external_index_ = nullptr;
	const DefItemsFile *items_override_ = nullptr;
	bool own_mounted_ = false;
	// The index the infantry .adm registrations resolve through (the install/
	// re-arm seam's; defaults to the asset index).
	const ResourceIndex *adm_index_ = nullptr;
	bool opened_ = false;
	std::function<std::string(int32_t)> people_name_resolver_;
	bool infantry_adm_retained_ = false;
	int infantry_adm_resolved_ai_count_ = 0;
	float look_accum_x_ = 0.0f;
	float look_accum_y_ = 0.0f;
	int stance_latch_ = 0;
	bool medic_dead_edge_seen_ = false;

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
