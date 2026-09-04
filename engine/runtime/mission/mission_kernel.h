#pragma once

// The engine's ONE mission boot + state + no-net authoritative tick (ADR 0042
// d3): the promoted body of the retail-mission test rig, now the single
// implementation every embedder drives — the Godot Simulation binding (the
// shell's session roles, which bind a kernel), the dedicated host, and the ctests
// (tests/common/retail_mission_files supplies retail paths only). It owns the
// world and its systems (AI, WAC, BMS events, collision, occlusion), the sim
// asset caches (models, collision pose, root motion, clip index), the seat
// specs and ai-profile installs, the terrain field store, the weapon/ammo
// tables, and the local-player frame state (input, weapon, loadout, view,
// stance latch, look accumulators). boot() IS the mission boot order: one
// straight-line sequence with its gates, recorded step by step in boot_trace
// (the ctest-locked order; ADR 0043 slice E9).
//
// Deliberately NOTHING net: the no-net tick has headless consumers that must
// not link the wire stack (the group order — runtime never includes net). The
// net half — the SP listen bring-up, the local C2S drain, the per-tick
// session frame — is inmatch::HostRole (engine/runtime/inmatch/host_role.h),
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
#include <runtime/simassets/item_traits.h>
#include <runtime/simassets/mounted_pose.h>
#include <runtime/simassets/sim_pose_provider.h>
#include <runtime/simassets/sim_model_cache.h>
#include <runtime/terrain_query/terrain_field_store.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/local_player.h>
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
	// The mission's .cpt/.trn(+charmap) height field: when the embedder built
	// no store before the boot (the shell hands its parsed documents over
	// through terrain_field_store_build), the kernel loads it through its own
	// asset index -- the file entry of the one builder (the dedicated host,
	// the ctests). Off = never load (the shell owns the parsed-document entry).
	bool terrain = true;
	// Receives the raw .til bytes beside that load (the S2C 0x45 terrain-tile
	// stream a wire joiner streams, net-re 5.37); null = not wanted.
	std::vector<uint8_t> *terrain_til_bytes = nullptr;
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
	// The net half's session bring-up (inmatch::HostRole::bring_up_singleplayer, or
	// bringup_dedicated for a HostOnly embedder),
	// invoked between the world wiring and the system registration — exactly
	// where the SP listen host stands up inside the load
	// [orig: SinglePlayer_StartMission @0x561af0]. Null = the bare no-net
	// kernel (the AI-path/convoy drives).
	std::function<void()> bringup_net_session;
};

class MissionKernel : public world::IPoseProvider {
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
	void set_items_table(const opennova::def::DefItemsFile *items_table);
	// The live items.def rows every kernel leg reads: the override, else the
	// kernel's own parse, else null (no item db — the gated steps skip).
	const opennova::def::DefItemsFile *items_table() const {
		return items_override_ != nullptr ? items_override_
										  : (items_ok ? &items : nullptr);
	}
	// The embedder's item-trait sweep over items_table() (simassets
	// resolve_item_traits with the embedder's wire-class classifier: the
	// callback class and health every registry row carries). The baseline is
	// captured before the embedder supplies these traits, so restore_baseline
	// re-runs the sweep with the retained classifier before any view is
	// rebuilt from the restored rows.
	void resolve_item_traits(simassets::ItemWireClassFn wire_class);
	// Re-run the embedder's sweep with the retained classifier (a streamed
	// topology change, the baseline restore); no-op before the embedder ran it.
	void resweep_item_traits();
	// The embedder's world-object collision instance sweep (BVOL/BPLN) over
	// items_table() [orig: the movement collision resolver @0x4b2bd0 + the
	// query set; §15]; returns the attached count. refresh_collision_instances
	// re-runs it for rows that appeared since (a joiner's streamed world), and
	// stays a no-op until the embedder ran the sweep once — exactly the gate
	// the wire collision shapes below sit behind.
	int resolve_collision_instances();
	int refresh_collision_instances();
	// The authored collision shape for one decoded runtime type id (items.def
	// graphic -> the parse-once model cache): model, exact effective
	// bound/scale and bbox center stay inseparable for movement, projectiles
	// and lighting. Cached per type; the default shape before the embedder's
	// collision sweep ran.
	world::ResolvedCollisionShape wire_collision_shape_for_type(uint16_t type_id);
	// Retire a streamed row's collision/occlusion instances and pose caches
	// (a topology change dropped the row). A later allocation at the same
	// packed handle may already have had its caches rebuilt; the retired
	// lifetime never erases those.
	void retire_replica_entity(const world::EntityLifetime &lifetime);
	// The mission-start portal init over the static prox tables [orig:
	// Terrain_InitBuildingPortals @ 0x5c7480 from Game_StartMission @ 0x525e11;
	// the tables must exist before the register pass walks the building prefix].
	void occlusion_init_mission();
	// The .adm registry id a decoded Player/Infantry row of `type_id` grounds
	// on (items.def anim_def through the infantry adm index) — the netsim twin
	// of resolve_new_infantry_adm_ids (AnimMap_RegisterEntity's spawn half
	// [orig: @0x40bb60]); -1 = no adm (the row stays chase-only), 0 = the
	// default set when the model names none. Cached per type so late-joining
	// peers and respawns cost one map lookup; -1 before the infantry sources
	// are armed (rearm_infantry_adm) so nothing is stamped early.
	int adm_id_for_runtime_type(uint16_t type_id);
	// The cached id only (no registration): -1 when the type never resolved.
	int cached_adm_id_for_runtime_type(uint16_t type_id) const;
	// The S9 boot over the opened mission: the terrain field (the embedder's
	// parsed documents when it built the store, else the kernel's own load),
	// then the ordered step sequence with its gates (a missing file source
	// skips every file-fed step, a missing item db the trait/collision steps,
	// a joiner never spawns its own player). Every step that ran lands in
	// boot_trace, in order — the ctest-locked contract.
	bool boot(const KernelBootOptions &options, std::string &error);
	std::vector<std::string> boot_trace;


	// --- the weather tick (ADR 0042 d2: ONE engine function) ------------------
	// The retail weather tick after the logic tick [orig:
	// Environment_UpdateWeatherTick @ 0x57e9b0 from Game_ProcessMainFrame
	// @ 0x526774, after Entity_UpdateAllEntities @ 0x52674b]: the world's sim
	// legs, the thunder one-shots into world.out.weather_sounds, the local quake
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
	// Ground every soldier that appeared since the previous sweep on its OWN
	// model .adm (D-INF-6); also the session pump's before-server-tick hook.
	void resolve_new_infantry_adm_ids();
	// Restore the post-PreMission snapshot — the play-start world, the WAC
	// runtime state, the local weapon's epoch resets (the borrowed-UseGun
	// reinstall event), the view reset, and the fresh-soldier .adm re-ground;
	// false when no baseline was taken.
	bool restore_baseline();
	// Re-capture the baseline from the CURRENT state (the shell's sealed
	// mission-start point: post-eager-WAC, fully settled play start).
	void capture_baseline();
	// --- the local player ----------------------------------------------------
	// The by-name weapon install from the retained weapon.def rows. A
	// same-name install is the MOUNT path unless `allow_same_weapon_rebake`
	// asks for the re-bake that keeps the live slot, serials, latches and
	// scope state (the F3 Weapon window's `auto`/ANIM edits).
	bool install_weapon(const std::string &weapon_name,
			bool preserve_slot_state = false, bool allow_same_weapon_rebake = false);
	// The armory table (weapon.def -> world.tables.weapons + the retained rows), the
	// mission loadout-chunk promotion and the spawn-kit rebuild — the boot's
	// load_weapon_table step over an explicit source so the embedder's
	// table-feed seam shares the one body. `index` resolves texture/model
	// references (null = the kernel's own asset index).
	bool load_weapon_table(const BootFileSource &files, const ResourceIndex *index,
			const std::string &name = "weapon.def");
	// ammo.def -> world.tables.ammo + the weapon round_type resolve.
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
	opennova::def::DefItemsFile items{};
	bool items_ok = false;

	// --- the world and its systems -------------------------------------------
	world::World world;
	BmsEventSystem events;
	wac::WacSystem wac;
	bool wac_loaded = false;
	world::CollisionWorld collision;
	world::OcclusionWorld occlusion;
	simassets::SimPoseProvider collision_pose;
	simassets::SimModelCache models;
	simassets::CollisionResolveState collision_state;
	simassets::AdmRootMotion root_motion;
	std::vector<ItemSeatSpec> seat_specs;
	std::unordered_map<int32_t, std::string> mounted_graphics;
	// The loaded document is a true S2C 0x0B mission: the 616-byte BMS header
	// alone. Retail allocates pools 1..3 while consuming 0x0D/0x10/0x20; the
	// joiner role's materializer gives local world consumers the same exact
	// packed rows, and the seat-spec refresh keys on this flag for every
	// role. Full-BMS joiners never enter that path and keep ordinary
	// promotion untouched. Per-load state the embedder sets after the
	// document opens (a fresh kernel reads false).
	bool wire_header_world = false;
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
	// The local player (world/local_player.h): its input, weapon, loadout,
	// view and medic-call state plus the verbs over them; the kernel keeps the
	// asset-bound legs (the def tables, the .adm clips, the spawn entries).
	world::LocalPlayer local;
	opennova::def::DefWeaponsFile weapon_defs{};
	bool weapon_defs_ok = false;
	bool ammo_ok = false;
	simassets::AdmClipIndex clip_index;
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

	// world::IPoseProvider: the seat leg is the kernel's own; the muzzle and
	// userpoint legs ride collision_pose (the sim-clock skeleton / PANM pose).
	bool resolve_mounted_pose(world::World &w, const world::Entity &carrier,
			const world::Seat &seat, world::MountedPose &out) override;
	bool ensure_collision_instance(world::World &w, world::EntityHandle entity) override;
	bool build_section_matrices(world::World &w, world::EntityHandle entity,
			int32_t model_id, const world::CollisionMatrix &entity_world,
			const world::CollisionModel &model,
			std::vector<world::CollisionMatrix> &out) override;
	bool resolve_muzzle_pose(world::World &w, world::EntityHandle entity,
			int32_t out[3]) override;
	bool resolve_userpoint_transform(world::World &w, world::EntityHandle entity,
			int userpoint_index, int32_t out[6]) override;
	bool resolve_userpoint_rigid(world::World &w, world::EntityHandle entity,
			int userpoint_index, int32_t out[3]) override;

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
	const opennova::def::DefItemsFile *items_override_ = nullptr;
	// The embedder's trait sweep classifier (resolve_item_traits); unset until
	// the embedder ran the sweep, so a restore re-stamps only what it stamped.
	simassets::ItemWireClassFn item_wire_class_;
	// The embedder's collision sweep ran (resolve_collision_instances): the
	// gate for the re-sweeps and the wire collision shapes.
	bool collision_items_resolved_ = false;
	std::unordered_map<uint16_t, world::ResolvedCollisionShape> wire_collision_shape_by_type_;
	std::unordered_map<uint16_t, int> adm_by_runtime_type_;
	bool own_mounted_ = false;
	// The index the infantry .adm registrations resolve through (the install/
	// re-arm seam's; defaults to the asset index).
	const ResourceIndex *adm_index_ = nullptr;
	bool opened_ = false;
	std::function<std::string(int32_t)> people_name_resolver_;
	bool infantry_adm_retained_ = false;
	int infantry_adm_resolved_ai_count_ = 0;

	struct MountedPoseRest {
		simassets::MountedPosePartMatrices parts;
	};
	struct MountedPoseLive {
		uint32_t time_ms = 0;
		std::array<int32_t, opennova::threedi::THREEDI_CTRL_REGISTER_COUNT> controls{};
		bool valid = false;
		simassets::MountedPosePartMatrices parts;
	};
	std::map<const opennova::threedi::Threedi3di3 *, MountedPoseRest> mounted_rest_cache_;
	std::map<const opennova::threedi::Threedi3di3 *, std::vector<MountedPoseLive>> mounted_live_cache_;
	uint32_t mounted_cache_tick_ = 0xFFFFFFFFu;
};

} // namespace opennova::mission
