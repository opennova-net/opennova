#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <formats/mission/bms.h>
#include <formats/mission/mission.h>

#include <cstddef>
#include <cstdint>

// The mission document's typed views as the document API reads
// them (ADR 0043 d10 over slice E11): each record is a value wrapper over the
// engine's bms::Entity or a formats/mission view struct — forwarding getters
// bound as read-only properties, never mirrored members. A static make()
// exists only for the records a test or the editor authors as an INPUT
// (the loadout entry, the trigger, the action); every other record is
// minted by MissionData from the file.

namespace godot {

// One placed record (a bms::Entity) with its (kind, index) address in the
// document. `item_id` is the items.def key (bms type_id + ITEM_ID_OFFSET);
// `position` is mission space (16.16 already converted to float) and
// `rotation_deg` the authored eulers (pitch, yaw, roll) — the placement
// layer converts to Godot space where terrain context is available.
class MissionEntityRecord : public RefCounted {
	GDCLASS(MissionEntityRecord, RefCounted)

	opennova::bms::Entity value_{};
	int kind_ = 0;
	int index_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::bms::Entity &p_value, opennova::mission::EntityKind p_kind,
			size_t p_index) {
		value_ = p_value;
		kind_ = static_cast<int>(p_kind);
		index_ = static_cast<int>(p_index);
	}
	const opennova::bms::Entity &value() const { return value_; }

	int get_kind() const { return kind_; }
	int get_index() const { return index_; }
	int get_item_id() const;
	int get_type_id() const { return static_cast<int>(value_.type_id); }
	int get_bms_id() const { return static_cast<int>(value_.id); }
	Vector3 get_position() const;
	Vector3 get_rotation_deg() const;
	int get_group() const { return static_cast<int>(value_.group_id); }
	int get_waypoint_id() const { return static_cast<int>(value_.waypoint_id); }
	int get_wp_number() const { return static_cast<int>(value_.wp_number); }
	int get_team() const { return static_cast<int>(value_.team); }
	int get_ai_flags() const { return static_cast<int>(value_.bmsi_attributes); }
	int get_perception() const { return static_cast<int>(value_.perception2); }
	int get_accuracy() const { return static_cast<int>(value_.w_accuracy1); }
	int get_alert_state() const { return static_cast<int>(value_.alert_state); }
	int get_min_engagement_distance() const {
		return static_cast<int>(value_.min_engagement_distance);
	}
	int get_max_engagement_distance() const {
		return static_cast<int>(value_.max_engagement_distance);
	}
	int get_max_attack_distance() const { return static_cast<int>(value_.max_attack_distance); }
	int get_spawn_count() const { return static_cast<int>(value_.spawns); }
	// = no_more_than (byte 74).
	int get_max_simultaneous() const { return static_cast<int>(value_.no_more_than); }
	int get_no_less_than() const { return static_cast<int>(value_.no_less_than); } // byte 75
	int get_map_symbol() const { return static_cast<int>(value_.map_symbol); }     // byte 81
	String get_name1() const; // AI class (iai_name)
	String get_name2() const; // AI script (ai_textfile)
};

// One populated waypoint path's summary: index, flags and the marker count
// (the cheap read for a path list; the indices are get_waypoint_path's).
class MissionWaypointSummary : public RefCounted {
	GDCLASS(MissionWaypointSummary, RefCounted)

	opennova::mission::WaypointSummary value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::WaypointSummary &p_value) { value_ = p_value; }

	int get_index() const { return static_cast<int>(value_.index); }
	int get_flags() const { return value_.flags; }
	int get_marker_count() const { return value_.marker_count; }
};

// One waypoint path: its index, flags (WP_FLAG_*) and the ordered KIND_MARKER
// entity indices it references.
class MissionWaypointPath : public RefCounted {
	GDCLASS(MissionWaypointPath, RefCounted)

	opennova::mission::WaypointPath value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::WaypointPath &p_value) { value_ = p_value; }

	int get_index() const { return static_cast<int>(value_.index); }
	int get_flags() const { return value_.flags; }
	int get_marker_count() const { return static_cast<int>(value_.marker_indices.size()); }
	PackedInt32Array get_marker_indices() const;
};

// add_waypoint_marker's result: the new marker record and the path it now
// references it from.
class MissionWaypointMarker : public RefCounted {
	GDCLASS(MissionWaypointMarker, RefCounted)

	Ref<MissionEntityRecord> marker_;
	Ref<MissionWaypointPath> path_;

protected:
	static void _bind_methods();

public:
	void assign(const Ref<MissionEntityRecord> &p_marker, const Ref<MissionWaypointPath> &p_path) {
		marker_ = p_marker;
		path_ = p_path;
	}

	Ref<MissionEntityRecord> get_marker() const { return marker_; }
	Ref<MissionWaypointPath> get_path() const { return path_; }
};

// One 32-byte axis-aligned box zone: `id` is the off-0 dword (Phase-5
// UNKNOWN; carried raw), min/max the mission-space corners, active /
// constrain_z the two known flag bits and raw_flags the whole flags dword.
class MissionAreaTrigger : public RefCounted {
	GDCLASS(MissionAreaTrigger, RefCounted)

	opennova::mission::AreaTriggerRecord value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::AreaTriggerRecord &p_value) { value_ = p_value; }
	const opennova::mission::AreaTriggerRecord &value() const { return value_; }
	// The one authoring seam (ADR 0043 d10): a test builds a zone by hand for
	// EntityIndex.build; the document path fills the record from the .bms.
	static Ref<MissionAreaTrigger> make(int p_index, int p_id, const Vector3 &p_min,
			const Vector3 &p_max, bool p_active = true, bool p_constrain_z = false);

	int get_index() const { return static_cast<int>(value_.index); }
	int get_id() const { return value_.wp_number; }
	Vector3 get_min() const { return Vector3(value_.min_x, value_.min_y, value_.min_z); }
	Vector3 get_max() const { return Vector3(value_.max_x, value_.max_y, value_.max_z); }
	bool get_active() const { return value_.active; }
	bool get_constrain_z() const { return value_.constrain_z; }
	int get_raw_flags() const { return value_.reserved; }
};

// One weapon-loadout record — the kit tuple {name, ammoPri, ammoSec, flags}
// (net-re §5.63) as four strings; flags is the per-ammo damage-class input
// (1 = x0.9, 2 = x1.1, otherwise neutral). Authorable: set_weapon_loadout
// takes a list of these; omitted values default to "-1".
class MissionWeaponLoadoutEntry : public RefCounted {
	GDCLASS(MissionWeaponLoadoutEntry, RefCounted)

	opennova::mission::WeaponLoadoutEntry value_;
	int index_ = 0;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::WeaponLoadoutEntry &p_value, int p_index) {
		value_ = p_value;
		index_ = p_index;
	}
	const opennova::mission::WeaponLoadoutEntry &value() const { return value_; }
	static Ref<MissionWeaponLoadoutEntry> make(const String &p_name,
			const String &p_ammo_primary = "-1", const String &p_ammo_secondary = "-1",
			const String &p_flags = "-1");

	int get_index() const { return index_; }
	String get_name() const;
	String get_ammo_primary() const;
	String get_ammo_secondary() const;
	String get_flags() const;
};

// One of the 64 fixed group records: field0 is flags, field8 is value,
// field12 is the canonical constant (mission.h GroupFields).
class MissionGroup : public RefCounted {
	GDCLASS(MissionGroup, RefCounted)

	opennova::mission::GroupFields value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::GroupFields &p_value) { value_ = p_value; }

	int get_index() const { return static_cast<int>(value_.index); }
	int get_field0() const { return value_.field0; }
	int get_field8() const { return value_.field8; }
	int get_field12() const { return value_.field12; }
};

// One event: the EventFlags bitfield, its contiguous trigger/action runs and
// the 10-bit reset_after / delay counters.
class MissionEvent : public RefCounted {
	GDCLASS(MissionEvent, RefCounted)

	opennova::mission::MissionEventRecord value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::MissionEventRecord &p_value) { value_ = p_value; }

	int get_index() const { return static_cast<int>(value_.index); }
	int get_flags() const { return value_.flags; }
	int get_trigger_index() const { return value_.trigger_index; }
	int get_action_index() const { return value_.action_index; }
	int get_trigger_count() const { return value_.trigger_count; }
	int get_action_count() const { return value_.action_count; }
	int get_reset_after() const { return value_.reset_after; }
	int get_delay() const { return value_.delay; }
	int get_unknown5() const { return value_.unknown5; }
	int get_unknown6() const { return value_.unknown6; }
};

// One trigger (condition). Authorable: the editable fields (main/sub type,
// the four params, the negated / OR / XOR logic bits) through make(); the
// unmodeled condition_flags high bits and unknown7 ride the existing record
// on an edit.
class MissionEventTrigger : public RefCounted {
	GDCLASS(MissionEventTrigger, RefCounted)

	opennova::mission::MissionTriggerRecord value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::MissionTriggerRecord &p_value) { value_ = p_value; }
	const opennova::mission::MissionTriggerRecord &value() const { return value_; }
	static Ref<MissionEventTrigger> make(int p_main_type, int p_sub_type, int p_param1 = 0,
			int p_param2 = 0, int p_param3 = 0, int p_param4 = 0, bool p_negated = false,
			bool p_logic_or = false, bool p_logic_xor = false);

	int get_index() const { return static_cast<int>(value_.index); }
	int get_condition_flags() const { return value_.condition_flags; }
	int get_main_type() const { return value_.main_type; }
	String get_main_type_name() const;
	int get_sub_type() const { return value_.sub_type; }
	String get_sub_type_name() const;
	int get_param1() const { return value_.param1; }
	int get_param2() const { return value_.param2; }
	int get_param3() const { return value_.param3; }
	int get_param4() const { return value_.param4; }
	int get_unknown7() const { return value_.unknown7; }
	bool get_negated() const { return value_.negated; }
	bool get_logic_or() const { return value_.logic_or; }
	bool get_logic_xor() const { return value_.logic_xor; }
	String get_logic_operator() const;
};

// One action (effect). Authorable: the type pair and the four params through
// make(); the reserved words ride the existing record on an edit.
class MissionEventAction : public RefCounted {
	GDCLASS(MissionEventAction, RefCounted)

	opennova::mission::MissionActionRecord value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::MissionActionRecord &p_value) { value_ = p_value; }
	const opennova::mission::MissionActionRecord &value() const { return value_; }
	static Ref<MissionEventAction> make(int p_action_type, int p_action_sub_type = 0,
			int p_param1 = 0, int p_param2 = 0, int p_param3 = 0, int p_param4 = 0);

	int get_index() const { return static_cast<int>(value_.index); }
	int get_action_type() const { return value_.action_type; }
	String get_action_type_name() const;
	int get_action_sub_type() const { return value_.action_sub_type; }
	String get_action_sub_type_name() const;
	int get_param1() const { return value_.param1; }
	int get_param2() const { return value_.param2; }
	int get_param3() const { return value_.param3; }
	int get_param4() const { return value_.param4; }
	int get_reserved0() const { return value_.reserved0; }
	int get_reserved1() const { return value_.reserved1; }
};

// One resolved cross-link of an event chain (a trigger pointing at an area
// zone, a ResetEvent action pointing at an event).
class MissionLogicReference : public RefCounted {
	GDCLASS(MissionLogicReference, RefCounted)

	opennova::mission::MissionLogicReference value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::MissionLogicReference &p_value) { value_ = p_value; }

	String get_source_kind() const;
	int get_source_index() const { return value_.source_index; }
	String get_target_kind() const;
	int get_target_index() const { return value_.target_index; }
	int get_param_slot() const { return value_.param_slot; }
	int get_raw_value() const { return value_.raw_value; }
	String get_label() const;
	bool get_valid() const { return value_.valid; }
};

// One diagnostic of an event chain.
class MissionLogicDiagnostic : public RefCounted {
	GDCLASS(MissionLogicDiagnostic, RefCounted)

	opennova::mission::MissionLogicDiagnostic value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::MissionLogicDiagnostic &p_value) { value_ = p_value; }

	String get_severity() const;
	String get_code() const;
	String get_message() const;
	String get_subject_kind() const;
	int get_subject_index() const { return value_.subject_index; }
};

// One event with its chains resolved: the event, its triggers and actions
// in chain order, the cross-links and the diagnostics.
class MissionEventChain : public RefCounted {
	GDCLASS(MissionEventChain, RefCounted)

	opennova::mission::MissionEventChain value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::MissionEventChain &p_value) { value_ = p_value; }

	Ref<MissionEvent> get_event() const;
	TypedArray<MissionEventTrigger> get_triggers() const;
	TypedArray<MissionEventAction> get_actions() const;
	TypedArray<MissionLogicReference> get_references() const;
	TypedArray<MissionLogicDiagnostic> get_diagnostics() const;
};

// The document's logic counts.
class MissionLogicSummary : public RefCounted {
	GDCLASS(MissionLogicSummary, RefCounted)

	opennova::mission::MissionLogicSummary value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::MissionLogicSummary &p_value) { value_ = p_value; }

	int get_events() const { return static_cast<int>(value_.event_count); }
	int get_triggers() const { return static_cast<int>(value_.trigger_count); }
	int get_actions() const { return static_cast<int>(value_.action_count); }
	int get_area_triggers() const { return static_cast<int>(value_.area_trigger_count); }
	int get_diagnostics() const { return static_cast<int>(value_.diagnostic_count); }
};

} // namespace godot
