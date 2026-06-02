#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <mission/mission.h>

namespace godot {

class NovaResourceRoot;

// Thin GDExtension wrapper over opennova::mission::MissionDocument (libs/mission).
// Parses a NovaLogic .bms mission file and exposes its header (terrain/env refs,
// metadata) and its placed entities as Godot dictionaries. The read surface is
// loading + getters; the mutate surface (Phase 1 authoring) is set_entity_transform
// + save, calling the already byte-faithful writer in libs/mission.
class NovaMissionData : public RefCounted {
	GDCLASS(NovaMissionData, RefCounted)

private:
	opennova::mission::MissionDocument document;
	String source_path;
	String last_error;
	// True once an in-memory mutation lands and before the next successful save/load.
	// MissionDocument carries no dirty bit of its own, so the wrapper owns it.
	bool modified = false;

	Dictionary entity_to_dictionary(const opennova::mission::EntityRecord &record) const;
	Dictionary waypoint_path_to_dictionary(const opennova::mission::WaypointPath &path) const;

protected:
	static void _bind_methods();

public:
	// Mirrors opennova::mission::EntityKind. Bound as plain constants.
	enum {
		KIND_MARKER = 0,
		KIND_ITEM = 1,
		KIND_BUILDING = 2,
		KIND_ORGANIC = 3,
		// Mirrors opennova::mission::bms::WaypointFlags (libs/mission). Bound as constants so
		// GDScript composes a path's flags without magic numbers. A path with DOES_NOT_LOOP
		// clear loops back to its first marker; BLUE_TEAM / RED_TEAM scope it to a side.
		WP_FLAG_DOES_NOT_LOOP = 1,
		WP_FLAG_BLUE_TEAM = 2,
		WP_FLAG_RED_TEAM = 4,
	};

	Error open_file(const String &path);
	// Load a .bms by flat name through the mounted resource root (VFS), so missions packed in
	// PFF archives load at runtime. Mirrors NovaObjectData::open_from_resource_root.
	Error open_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;

	String get_mission_name() const;
	String get_designer() const;
	// Header references (basenames, no extension): e.g. "dvxi5", "full_00".
	String get_terrain_ref() const;
	String get_environment_ref() const;
	Dictionary get_info() const;

	int get_entity_count(int kind) const;
	// Array of dictionaries; see entity_to_dictionary() for the fields.
	Array get_entities(int kind) const;
	// One entity by (kind, index) as a dictionary (same fields as get_entities), or {}
	// if out of range. O(1) — prefer this over scanning get_entities for a single hit.
	Dictionary get_entity(int kind, int index) const;
	Array get_all_entities() const;

	// --- Authoring (Phase 1) --------------------------------------------------
	// Move an existing entity. `position` is mission-space (x, y, z); `rotation_deg`
	// is (pitch, yaw, roll) in degrees, rounded to the int fields the format stores.
	// Returns false if (kind, index) is out of range. Sets the dirty flag on success.
	bool set_entity_transform(int kind, int index, const Vector3 &position, const Vector3 &rotation_deg);
	// Mutate a single editable scalar property on the entity at (kind, index). The
	// property name matches the entity dictionary key it edits; the editable set is:
	// "team", "group", "waypoint_id", "wp_number", "perception", "accuracy",
	// "alert_state", "min_engagement_distance", "max_engagement_distance",
	// "max_attack_distance", "spawn_count", "max_simultaneous", "ai_flags". The
	// underlying lib setter (set_entity_properties) replaces all property fields at
	// once, so this reads the entity's current properties, overwrites only the named
	// one, then writes the lot back. Returns false if (kind, index) is out of range or
	// `property` is not a known editable name. Sets the dirty flag on success.
	bool set_entity_property_int(int kind, int index, const String &property, int value);
	// Place a new entity of `kind` for `item_id` (an items.def id) at `position`
	// (mission-space x, y, z) with `rotation_deg` (pitch, yaw, roll), rounded to the
	// int fields the format stores. The lib seeds the rest of the record with sane
	// defaults (see make_default_entity). Returns the new entity as a dictionary
	// (same fields as get_entity, including its assigned "index"), or {} if no mission
	// is loaded or the kind is invalid. Sets the dirty flag on success.
	Dictionary add_entity(int kind, int item_id, const Vector3 &position, const Vector3 &rotation_deg);
	// Remove the entity at (kind, index). The lib erases it from its kind's list, so
	// every later entity of that kind shifts down by one index (callers holding indices
	// must re-fetch). Markers also repair the waypoint paths that referenced them.
	// Returns false if (kind, index) is out of range. Sets the dirty flag on success.
	bool remove_entity(int kind, int index);

	// --- Waypoints ------------------------------------------------------------
	// A mission carries 128 fixed waypoint paths; a path is an ordered list of marker
	// indices (each an index into the KIND_MARKER entity list) plus flags (WP_FLAG_*).
	// Units follow a path via their per-entity "waypoint_id". The lib (libs/mission)
	// already parses and round-trips all of this byte-faithfully; this is the binding.
	//
	// Every populated path summary as { index, flags, marker_count } -- the cheap read
	// for a path list (does not materialize the marker indices).
	Array get_waypoint_summaries() const;
	// One path as { index, flags, marker_count, marker_indices: PackedInt32Array } (the
	// indices are KIND_MARKER entity indices), or {} if `index` is out of range.
	Dictionary get_waypoint_path(int index) const;
	// All 128 paths in the same shape as get_waypoint_path (for the in-world overlay).
	Array get_waypoint_paths() const;
	// Replace a path's ordered marker list and flags. `marker_indices` are KIND_MARKER
	// entity indices; an empty list clears the path. Does NOT create or delete marker
	// entities -- it only rewrites which markers (and in what order) the path references,
	// so it is the call for reorder / flag-only edits. Returns false if `index` is out of
	// range or any marker index is invalid. Sets the dirty flag on success.
	bool set_waypoint_path(int index, const PackedInt32Array &marker_indices, int flags);
	// Empty a path (drop all its marker references; the marker entities are left in place).
	// Returns false if `index` is out of range. Sets the dirty flag on success.
	bool clear_waypoint_path(int index);
	// Create a new marker entity for `marker_item_id` at the given transform AND link it
	// into path `path_index` at `insert_index` (-1 = append). One call both adds the
	// KIND_MARKER entity and references it from the path. Returns { "marker": <entity dict>,
	// "path": <path dict> } or {} if no mission is loaded or the path / insert index is
	// invalid. Rotation is rounded to integer degrees (same contract as add_entity). Sets
	// the dirty flag on success.
	Dictionary add_waypoint_marker(int path_index, int marker_item_id, const Vector3 &position, const Vector3 &rotation_deg, int insert_index);

	// Write the document back to disk. save_file() targets the path it was opened
	// from; save_as() targets a new path and adopts it. Both clear the dirty flag and
	// go through the byte-faithful writer in libs/mission. save_file() returns
	// ERR_INVALID_PARAMETER when there is no current path (shell then offers Save As).
	Error save_file();
	Error save_as(const String &path);
	bool is_modified() const;

	// --- Snapshot / restore (undo/redo support) -------------------------------
	// Serialize the whole document to a byte buffer through the same byte-faithful
	// writer as save (write_bms_bytes), not the bytes it was opened from. The result
	// is a valid .bms regardless of how the document was built, so callers (the
	// editor's undo stack) hold re-serialized states, never raw input passed through.
	// Returns an empty array when no mission is loaded. Non-const: write_bms_bytes
	// syncs the header counts on the underlying document before serializing.
	PackedByteArray snapshot();
	// Replace the whole in-memory document by parsing `bytes` (load_bms_bytes). Used
	// to restore an undo/redo snapshot without touching the filesystem. Leaves the
	// dirty flag untouched: the editor owns its own dirty state and recomputes it.
	// Returns false (and leaves the document empty: load_bms_bytes clears first) when
	// the bytes fail to parse, so callers must not assume a valid document on false.
	bool restore_snapshot(const PackedByteArray &bytes);
};

} // namespace godot
