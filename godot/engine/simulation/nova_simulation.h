#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <memory>

#include <mission/event_runtime.h>
#include <mission/promote.h>
#include <wac/wac_system.h>
#include <world/ai.h>
#include <world/world.h>

#include "mission/nova_mission_data.h"

namespace godot {

// THE mission runtime binding: a thin host shell over the portable libs/world runtime.
// Owns one World + the three logic systems (WAC VM, BMS event evaluator, AI) and drives
// them through World::run_logic_tick — the same tick the original engine runs inside its
// server tick (sub_4F81A0). Both the editor "Play the mission" preview and the game runtime
// go through this one path: promote a parsed BMS mission into the world (mission/promote.h),
// register the systems in the faithful order (mission/mission_systems.h), run a pre-mission
// pass, then tick. Entity transforms (mission space -> Godot space) and the part-anim phase
// are exposed for a scene/renderer to draw; host-presentation side effects (text/dialog/win)
// drain out of the World EffectLog each tick. Play/Pause/Step + Stop (snapshot/restore).
//
// Tick cadence: DIVIDED (62 render frames per logic tick, faithful to the 0x3E divider) for
// the game; EVERY_PROCESS (one logic tick per frame) for the snappy editor preview.
class NovaSimulation : public Node3D {
	GDCLASS(NovaSimulation, Node3D)

public:
	enum TickMode {
		TICK_DIVIDED = 0,      // one logic tick per 62 render frames (game; faithful sub_4F81A0)
		TICK_EVERY_PROCESS = 1 // one logic tick per render frame (editor preview)
	};

private:
	std::unique_ptr<opennova::world::World> world_;
	std::unique_ptr<opennova::world::AiSystem> ai_;
	std::unique_ptr<opennova::mission::BmsEventSystem> bms_;
	std::unique_ptr<opennova::wac::WacSystem> wac_;
	opennova::world::TickService tick_service_;
	opennova::world::World::Snapshot baseline_; // play-start state, for Stop -> restore
	opennova::mission::PromoteResult promo_;
	bool loaded_ = false;
	bool playing_ = false;
	bool have_baseline_ = false;
	int tick_mode_ = TICK_DIVIDED;

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
	void step();           // advance exactly one logic tick (editor Step / EVERY_PROCESS)
	bool advance_frame();  // feed one host frame to the 62-frame divider (game / DIVIDED)
	void restart();        // Stop: restore the play-start baseline (rewinds world + AI)
	void set_tick_mode(int p_mode) { tick_mode_ = p_mode; }
	int get_tick_mode() const { return tick_mode_; }

	// Drain the World EffectLog as an Array of Dictionaries {kind, a, b, c, d, str} and clear
	// it. Presentation-only (text/dialog/win/subgoal/show_waypoints/set_light); state mutation
	// is applied in-engine, never here.
	Array drain_effects();

	// Mission scripting state on the shared world (the dword_C6B240 var store + event gates).
	void set_mission_variable(int index, int value);
	int get_mission_variable(int index) const;
	bool has_event_fired(int index) const;
	int get_event_count() const;

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

	// The AI-speed -> world-units locomotion factor (see AiSystem::loco_scale).
	void set_loco_scale(int p_scale);
	int get_loco_scale() const;

	int get_spawned_count() const { return promo_.spawned; }
	int get_brain_count() const { return promo_.brains; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaSimulation::TickMode);
