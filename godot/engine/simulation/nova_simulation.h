#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <memory>
#include <vector>

#include <mission/event_runtime.h>
#include <mission/promote.h>
#include <terrain/height_field.h>
#include <wac/wac_system.h>

#include "wac/nova_wac_program.h"
#include <world/ai.h>
#include <world/world.h>

#include "mission/nova_mission_data.h"
#include "simulation/infantry_root_motion.h"

#include <novaworld/replication_min.h>
#include "netsim/loopback_channel.h"
#include "netsim/net_client_view.h"
#include "netsim/net_system.h"
#include "netsim/serializing_sink.h"

namespace godot {

class NovaTerrainData;

// THE mission runtime binding: a thin host shell over the portable libs/world runtime.
// Owns one World + the three logic systems (WAC VM, BMS event evaluator, AI) and drives
// them through World::run_logic_tick — one logic tick per host frame, the original's
// 62 Hz engine tick (current_tick in Game_ProcessMainFrame @0x5263f0). The per-system
// cadences live INSIDE the systems, as in the original: the WAC VM self-gates to every
// 62nd tick (sub_4F81A0 @0x4f81b1) and the BMS evaluator quarter-passes every 16th
// (Server_TickUpdate @0x51d7e0). Both the editor "Play the mission" preview and the game
// runtime go through this one path: promote a parsed BMS mission into the world
// (mission/promote.h), register the systems in the faithful order
// (mission/mission_systems.h), run a pre-mission pass, then tick. Entity transforms
// (mission space -> Godot space) and the part-anim phase are exposed for a scene/renderer
// to draw; host-presentation side effects (text/dialog/win) drain out of the World
// EffectLog each tick. Play/Pause/Step + Stop (snapshot/restore).
class NovaSimulation : public Node3D {
	GDCLASS(NovaSimulation, Node3D)

public:
	enum TickMode {
		// Both modes run one logic tick per process frame today (the 62-divider moved
		// into WacSystem where the original keeps it); the enum survives for API
		// stability and for a future fixed-62 Hz accumulator in DIVIDED.
		TICK_DIVIDED = 0,
		TICK_EVERY_PROCESS = 1
	};

	// Field layout of one entity record in get_present_snapshot()'s flat float buffer. ONE batched
	// PackedFloat32Array call replaces the per-entity scalar getters in the per-tick present loop
	// (the scalar getters box a Variant each; see feedback_dispatcher_callable_perf). Mirrored on the
	// GDScript side via these bound constants so the layout has a single source of truth (C++).
	// Rotation is emitted as mission-space degrees (pitch, yaw, roll) so the host builds the basis
	// through the one placer convention (MissionObjectPlacer.bms_to_godot_basis); position is already
	// in Godot space (x, z, -y). Pitch/roll are 0 today (yaw-only locomotion) — reserved for parity.
	enum PresentField {
		PF_KIND = 0,   // mission ItemType (3 = Organic), -1 if none
		PF_INDEX,      // index within its kind's list
		PF_BMS_ID,     // file entity id; host maps this to a placed node (primary key)
		PF_NET_ID,     // runtime SSN (WAC/BMS addressing)
		PF_POS_X,      // Godot-space position (mission (x,y,z) 16.16 -> (x, z, -y) units)
		PF_POS_Y,
		PF_POS_Z,
		PF_PITCH_DEG,  // mission-space rotation, degrees (0 today; reserved)
		PF_YAW_DEG,
		PF_ROLL_DEG,   // (0 today; reserved)
		PF_PHASE1,     // PANM channel 1 phase 0..65535
		PF_ACTIVE1,    // 1 when channel 1 has a live part-anim to render, else 0
		PF_PHASE2,     // PANM channel 2 phase
		PF_ACTIVE2,
		PF_ANIM_SLOT,  // Entity.anim_slot (main-body .bad/.adm clip; consumed only by the deferred seam)
		PF_HIDDEN,     // 1 when the entity is hidden
		PF_ALIVE,      // 1 when alive
		PF_STRIDE      // record length; also the count of fields above
	};

private:
	std::unique_ptr<opennova::world::World> world_;
	std::unique_ptr<opennova::world::AiSystem> ai_;
	std::unique_ptr<opennova::mission::BmsEventSystem> bms_;
	std::unique_ptr<opennova::wac::WacSystem> wac_;
	// The installed script program. Held as a Ref so it survives reset_world();
	// finish_load() re-applies it onto the fresh WacSystem each (re)load.
	Ref<NovaWacProgram> wac_program_;
	opennova::world::World::Snapshot baseline_; // play-start state, for Stop -> restore
	opennova::mission::PromoteResult promo_;
	bool loaded_ = false;
	bool playing_ = false;
	bool have_baseline_ = false;
	int tick_mode_ = TICK_DIVIDED;

	// --- in-match net seam (ADR 0009/0011): the SP in-process listen server. OFF by
	// default, so the editor / non-net preview path is byte-for-byte unchanged. When
	// enabled (before load), finish_load registers NetSystem ahead of WAC, routes
	// World::net through the serializing sink, and the present pass reads the
	// client-decoded ClientState (ADR 0011 Decision 1) instead of the AI pool.
	bool listen_server_ = false;
	std::unique_ptr<opennova::netsim::LoopbackChannel> loopback_;
	std::unique_ptr<opennova::netsim::SerializingSink> net_sink_;
	std::unique_ptr<opennova::netsim::NetSystem> net_;
	std::unique_ptr<opennova::netsim::NetClientView> client_view_;
	// The per-frame 0x0A anchor: the local player's world position, or (Phase 1, no
	// player yet) the first replicated entity, so compressed deltas stay small.
	opennova::PlayerReplicationState compute_net_anchor() const;
	// Build the PF_* present buffer from the client-decoded ClientState.
	PackedFloat32Array present_snapshot_from_client_view() const;
	// Post-logic listen-server step: emit S2C -> loopback -> client decode. No-op off.
	void net_tick();

	// Terrain the AI grounds on. We own copies of the host's depth buffer + 16x16 sector grid so
	// the portable TerrainHeightField's raw pointers outlive the source NovaTerrainData and survive
	// a reload (reset_world rebuilds ai_; apply_terrain_to_ai re-points it). Empty = no grounding.
	std::vector<uint16_t> terrain_heightmap_;
	std::vector<int> terrain_sector_grid_;
	opennova::terrain::TerrainHeightField terrain_field_;
	void apply_terrain_to_ai();

	// Anim-driven soldier locomotion: the .adm/.bad-backed root-motion source the infantry
	// motor integrates (world/infantry.h). Owned here so it survives reset_world; the fresh
	// ai_ is re-pointed at it like the terrain field. Empty = soldiers hold and stand.
	InfantryRootMotion infantry_anim_;
	void apply_root_motion_to_ai();

	void reset_world();
	// Shared post-promote wiring: load the BMS arrays, register the systems, run the
	// pre-mission pass, capture the restore baseline. Marks the sim loaded.
	void finish_load(const opennova::bms::File &file);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	NovaSimulation();

	// Load + promote the editor's live mission (the in-memory bms::File, including unsaved
	// edits). This is the editor-integration entry: simulate exactly what is on screen.
	bool load_from_mission_data(const Ref<NovaMissionData> &p_mission);
	// Load + promote a .bms mission from disk; false on parse failure.
	bool load_mission_file(const String &path);
	// Build + promote a small synthetic patrol mission (no file) for the headless unit test.
	void build_demo_mission();
	bool is_loaded() const { return loaded_; }

	// Transport.
	void set_playing(bool p_playing) { playing_ = p_playing; }
	bool is_playing() const { return playing_; }
	void step();           // advance exactly one logic tick (editor Step)
	bool advance_frame();  // one host frame = one logic tick (the WAC VM self-gates inside)
	void restart();        // Stop: restore the play-start baseline (rewinds world + AI)
	void set_tick_mode(int p_mode) { tick_mode_ = p_mode; }
	int get_tick_mode() const { return tick_mode_; }

	// Turn the sim into an SP in-process listen server (ADR 0011): the host serializes
	// real entity state through NetSystem onto an in-process loopback, the local client
	// decodes it, and the present pass reads that decoded state. Call BEFORE loading a
	// mission — the next load registers NetSystem ahead of WAC. Disabling reverts to the
	// direct AI-pool present (the editor default).
	void enable_listen_server(bool p_enable);
	bool is_listen_server() const { return listen_server_; }

	// --- WAC scripts ------------------------------------------------------
	// Install a compiled program on the script VM (NovaWacProgram). Applied now if
	// loaded and re-applied on every (re)load. Pass null to uninstall.
	void set_wac_program(const Ref<NovaWacProgram> &p_program);
	Ref<NovaWacProgram> get_wac_program() const { return wac_program_; }
	// Compile `sources` against the LIVE promoted world (symbolic group/area names
	// resolve through the registry) and install on success. False (program not
	// installed) when compilation has errors; inspect via get_wac_program().
	bool compile_and_set_wac(const PackedStringArray &p_sources);
	// { loaded, paused, runs, event_count, code_size } for transport/debug UI.
	Dictionary get_wac_state() const;
	// Script-disable gate [orig: dword_C6EB28].
	void set_wac_paused(bool p_paused);
	bool is_wac_paused() const;

	// Drain the World EffectLog as an Array of Dictionaries {kind, a, b, c, d, str} and clear
	// it. Presentation-only (text/dialog/win/subgoal/show_waypoints/set_light); state mutation
	// is applied in-engine, never here.
	Array drain_effects();

	// Mission scripting state on the shared world (the dword_C6B240 var store + event gates).
	void set_mission_variable(int index, int value);
	int get_mission_variable(int index) const;
	bool has_event_fired(int index) const;
	int get_event_count() const;

	// --- Read-only introspection (debug overlay / tooling) -----------------
	// The world's logic tick counter [orig: current_tick @0x24c1968]. The
	// pre-mission pass in finish_load already advanced it once, so a freshly
	// loaded mission reads 1 — consumers should track deltas, not absolutes.
	int64_t get_logic_tick() const;
	// Whole-bank snapshots of the script variable stores (V0..V511 / G0..G255 /
	// M0..M15 [orig: dword_C6B240 / dword_C6BA40 / music bank]): ONE packed call
	// for a low-Hz overlay refresh instead of hundreds of boxed scalar reads.
	// Always bank-sized; all zeros when no mission is loaded.
	PackedInt32Array get_mission_variables_snapshot() const;
	PackedInt32Array get_global_variables_snapshot() const;
	PackedInt32Array get_music_variables_snapshot() const;
	// Globals (G#) round out the scalar var API (mission V# already bound).
	void set_global_variable(int index, int value);
	int get_global_variable(int index) const;
	// [i] = 1 when event i has fired (active latch + delay elapsed): the bulk
	// form of has_event_fired for an event readout. Empty when unloaded.
	PackedByteArray get_fired_events_snapshot() const;
	// Scalars-only detail card for ONE selected entity ({} when the index is
	// invalid). The key set is STABLE: a registry-despawned entity (scripted
	// remove) still carries every key, with typed defaults for the registry
	// half (kind/index -1, alive false, empty name, ...). Dictionary/String
	// allocation is fine at selected-entity-only low-Hz use; the per-tick
	// present loop has get_present_snapshot instead.
	Dictionary get_entity_debug(int p_index) const;
	// Human-readable AI state name, "?" for the id gaps
	// [orig: Entity_LookupAIStateName @0x455cc0].
	static String ai_state_name(int p_state);

	// Entity query. The (kind, index) pair lets the editor map a sim entity back to its placed
	// mission record + its already-rendered node (MissionController._pickable).
	int get_entity_count() const;
	int get_entity_kind(int p_index) const;         // mission ItemType (3 = Organic), -1 if none
	int get_entity_index(int p_index) const;        // index within its kind's list
	Vector3 get_entity_position(int p_index) const; // mission (x,y,z) -> Godot (x, z, -y), units
	float get_entity_yaw(int p_index) const;        // BAM heading -> radians
	float get_entity_yaw_deg(int p_index) const;    // heading in mission degrees (for the editor remap)
	int get_entity_state(int p_index) const;        // AI state id (16 = GROUND_FOLLOWWP)
	int get_entity_net_id(int p_index) const;       // runtime SSN (WAC/BMS addressing), 0 if none
	int get_entity_bms_id(int p_index) const;       // file entity id; the host maps this to a placed node
	// Part-anim channel phase 0..65535 (PLAYPARTANIM); channel is 1 or 2. The host renders the
	// model part from this (the engine computes it; the host only reads it).
	int get_entity_part_anim_phase(int p_index, int channel) const;
	// True when channel has a live part-anim to render (rate set or phase moved off rest), so the
	// host only poses commanded channels and leaves untouched parts at their default.
	bool get_entity_part_anim_active(int p_index, int channel) const;
	// Entity.anim_slot: the main-body skeletal clip (.bad via .adm) the AI requested. Written by
	// EntityCommands::set_ssn_anim; consumed only by the host's deferred apply_body_anim seam today
	// (skeletal runtime not yet built — AnimMap_PlayAnimBySlot @0x40bda0 / off_8135F0). -1 = none.
	int get_entity_anim_slot(int p_index) const;
	// True when the entity is flagged hidden (HideSingle / held). The present pass maps
	// (not hidden and alive) -> Node3D.visible.
	bool get_entity_hidden(int p_index) const;

	// ONE batched present snapshot for the per-tick render pass: a flat PackedFloat32Array of
	// get_entity_count() records, PF_STRIDE floats each, fields per the PresentField enum. Avoids the
	// ~10 Variant-boxed scalar getter calls per entity the present loop would otherwise make.
	PackedFloat32Array get_present_snapshot() const;
	int get_present_stride() const { return PF_STRIDE; }

	// Wire the terrain the AI grounds on (the host's loaded NovaTerrainData). Copies the depth
	// buffer + sector layout so the portable height field outlives the source and survives reload.
	// Null/unloaded clears grounding (entities keep their authored Z). The editor preview and the
	// game runtime both call this once in MissionRuntime.setup() so they ground identically.
	void set_terrain_height_field(const Ref<NovaTerrainData> &p_terrain);

	// The AI-speed -> world-units locomotion factor (see AiSystem::loco_scale).
	void set_loco_scale(int p_scale);
	int get_loco_scale() const;

	// Wire the infantry root-motion source: resolve a model's .adm (e.g. "E_STAND.adm")
	// through the host's resource root and keep its clips' root tracks. Returns the number
	// of anim states with a usable clip (0 = nothing loaded; org1 soldiers then stand —
	// motion comes from clips, as in the original). Survives reset_world like the terrain.
	int set_infantry_anim_map(const Ref<class NovaResourceRoot> &p_resource_root, const String &p_adm_name);
	int get_infantry_clip_count() const { return infantry_anim_.clip_count(); }

	int get_spawned_count() const { return promo_.spawned; }
	int get_brain_count() const { return promo_.brains; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaSimulation::TickMode);
VARIANT_ENUM_CAST(godot::NovaSimulation::PresentField);
