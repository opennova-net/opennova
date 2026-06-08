#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <memory>

#include <mission/promote.h>
#include <world/ai.h>
#include <world/world.h>

#include "mission/nova_mission_data.h"

namespace godot {

// Live mission simulation: owns a libs/world World + the AI system, promotes a parsed BMS
// mission into it (mission/promote.h), and ticks the AI so NPCs walk their authored routes.
// Exposes per-entity transforms (mission space -> Godot space) for a scene / renderer to draw,
// plus Play/Pause/Step transport (ticks on _process when playing).
//
// The AI brain + waypoint mover are byte-exact ports (libs/world, grilled vs Jointops.exe); the
// entity-movement step is a documented kinematic integrator (AiSystem::apply_locomotion) until
// the original physics driver is reverse-engineered. World + AiSystem are heap-held so a reload
// can rebuild them cleanly (World has a self-referential member and is not reassignable).
class NovaSimulation : public Node3D {
	GDCLASS(NovaSimulation, Node3D)

private:
	std::unique_ptr<opennova::world::World> world_;
	std::unique_ptr<opennova::world::AiSystem> ai_;
	opennova::mission::PromoteResult promo_;
	bool loaded_ = false;
	bool playing_ = false;

	void reset_world();

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
	void step(); // advance one AI tick (decision + locomotion)

	// Entity query. The (kind, index) pair lets the editor map a sim entity back to its placed
	// mission record + its already-rendered node (MissionController._pickable).
	int get_entity_count() const;
	int get_entity_kind(int p_index) const;         // mission ItemType (3 = Organic), -1 if none
	int get_entity_index(int p_index) const;        // index within its kind's list
	Vector3 get_entity_position(int p_index) const; // mission (x,y,z) -> Godot (x, z, -y), units
	float get_entity_yaw(int p_index) const;        // BAM heading -> radians
	float get_entity_yaw_deg(int p_index) const;    // heading in mission degrees (for the editor remap)
	int get_entity_state(int p_index) const;        // AI state id (16 = GROUND_FOLLOWWP)

	// The AI-speed -> world-units locomotion factor (see AiSystem::loco_scale).
	void set_loco_scale(int p_scale);
	int get_loco_scale() const;

	int get_spawned_count() const { return promo_.spawned; }
	int get_brain_count() const { return promo_.brains; }
};

} // namespace godot
