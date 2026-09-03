#include "mission/mission_records.h"

#include "util/record_bind.h"

#include <formats/mission/bms_edit.h> // entity_item_id / entity_transform / entity_name1 / entity_name2

using namespace godot;

namespace {
String gd(const std::string &s) { return String::utf8(s.c_str()); }
} // namespace

// --- MissionEntityRecord ----------------------------------------------------

int MissionEntityRecord::get_item_id() const {
	return opennova::mission::entity_item_id(value_);
}

Vector3 MissionEntityRecord::get_position() const {
	const opennova::mission::EntityTransform t = opennova::mission::entity_transform(value_);
	return Vector3(t.x, t.y, t.z);
}

Vector3 MissionEntityRecord::get_rotation_deg() const {
	const opennova::mission::EntityTransform t = opennova::mission::entity_transform(value_);
	return Vector3(t.pitch, t.yaw, t.roll);
}

String MissionEntityRecord::get_name1() const {
	return String(opennova::mission::entity_name1(value_).c_str());
}

String MissionEntityRecord::get_name2() const {
	return String(opennova::mission::entity_name2(value_).c_str());
}

void MissionEntityRecord::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, kind)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, index)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, item_id)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, type_id)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, bms_id)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::VECTOR3, position)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::VECTOR3, rotation_deg)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, group)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, waypoint_id)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, wp_number)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, team)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, ai_flags)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, perception)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, accuracy)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, alert_state)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, min_engagement_distance)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, max_engagement_distance)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, max_attack_distance)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, spawn_count)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, max_simultaneous)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, no_less_than)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::INT, map_symbol)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::STRING, name1)
	OPENNOVA_RECORD_READ_ONLY(MissionEntityRecord, Variant::STRING, name2)
}

// --- MissionWaypointSummary / MissionWaypointPath / MissionWaypointMarker ---

void MissionWaypointSummary::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(MissionWaypointSummary, Variant::INT, index)
	OPENNOVA_RECORD_READ_ONLY(MissionWaypointSummary, Variant::INT, flags)
	OPENNOVA_RECORD_READ_ONLY(MissionWaypointSummary, Variant::INT, marker_count)
}

PackedInt32Array MissionWaypointPath::get_marker_indices() const {
	PackedInt32Array indices;
	indices.resize(static_cast<int64_t>(value_.marker_indices.size()));
	for (size_t i = 0; i < value_.marker_indices.size(); ++i) {
		indices.set(static_cast<int64_t>(i), value_.marker_indices[i]);
	}
	return indices;
}

void MissionWaypointPath::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(MissionWaypointPath, Variant::INT, index)
	OPENNOVA_RECORD_READ_ONLY(MissionWaypointPath, Variant::INT, flags)
	OPENNOVA_RECORD_READ_ONLY(MissionWaypointPath, Variant::INT, marker_count)
	OPENNOVA_RECORD_READ_ONLY(MissionWaypointPath, Variant::PACKED_INT32_ARRAY, marker_indices)
}

void MissionWaypointMarker::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY_OBJECT(MissionWaypointMarker, marker, MissionEntityRecord)
	OPENNOVA_RECORD_READ_ONLY_OBJECT(MissionWaypointMarker, path, MissionWaypointPath)
}

// --- MissionAreaTrigger -----------------------------------------------------

void MissionAreaTrigger::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(MissionAreaTrigger, Variant::INT, index)
	OPENNOVA_RECORD_READ_ONLY(MissionAreaTrigger, Variant::INT, id)
	OPENNOVA_RECORD_READ_ONLY(MissionAreaTrigger, Variant::VECTOR3, min)
	OPENNOVA_RECORD_READ_ONLY(MissionAreaTrigger, Variant::VECTOR3, max)
	OPENNOVA_RECORD_READ_ONLY(MissionAreaTrigger, Variant::BOOL, active)
	OPENNOVA_RECORD_READ_ONLY(MissionAreaTrigger, Variant::BOOL, constrain_z)
	OPENNOVA_RECORD_READ_ONLY(MissionAreaTrigger, Variant::INT, raw_flags)
}

// --- MissionWeaponLoadoutEntry ----------------------------------------------

Ref<MissionWeaponLoadoutEntry> MissionWeaponLoadoutEntry::make(const String &p_name,
		const String &p_ammo_primary, const String &p_ammo_secondary, const String &p_flags) {
	opennova::mission::WeaponLoadoutEntry v;
	v.name = p_name.utf8().get_data();
	v.ammo_primary = p_ammo_primary.utf8().get_data();
	v.ammo_secondary = p_ammo_secondary.utf8().get_data();
	v.flags = p_flags.utf8().get_data();
	Ref<MissionWeaponLoadoutEntry> out;
	out.instantiate();
	out->assign(v, 0);
	return out;
}

String MissionWeaponLoadoutEntry::get_name() const { return gd(value_.name); }
String MissionWeaponLoadoutEntry::get_ammo_primary() const { return gd(value_.ammo_primary); }
String MissionWeaponLoadoutEntry::get_ammo_secondary() const { return gd(value_.ammo_secondary); }
String MissionWeaponLoadoutEntry::get_flags() const { return gd(value_.flags); }

void MissionWeaponLoadoutEntry::_bind_methods() {
	ClassDB::bind_static_method("MissionWeaponLoadoutEntry",
			D_METHOD("make", "name", "ammo_primary", "ammo_secondary", "flags"),
			&MissionWeaponLoadoutEntry::make, DEFVAL(String("-1")), DEFVAL(String("-1")),
			DEFVAL(String("-1")));
	OPENNOVA_RECORD_READ_ONLY(MissionWeaponLoadoutEntry, Variant::INT, index)
	OPENNOVA_RECORD_READ_ONLY(MissionWeaponLoadoutEntry, Variant::STRING, name)
	OPENNOVA_RECORD_READ_ONLY(MissionWeaponLoadoutEntry, Variant::STRING, ammo_primary)
	OPENNOVA_RECORD_READ_ONLY(MissionWeaponLoadoutEntry, Variant::STRING, ammo_secondary)
	OPENNOVA_RECORD_READ_ONLY(MissionWeaponLoadoutEntry, Variant::STRING, flags)
}

// --- MissionGroup -----------------------------------------------------------

void MissionGroup::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(MissionGroup, Variant::INT, index)
	OPENNOVA_RECORD_READ_ONLY(MissionGroup, Variant::INT, field0)
	OPENNOVA_RECORD_READ_ONLY(MissionGroup, Variant::INT, field8)
	OPENNOVA_RECORD_READ_ONLY(MissionGroup, Variant::INT, field12)
}

// --- MissionEvent -----------------------------------------------------------

void MissionEvent::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, index)
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, flags)
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, trigger_index)
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, action_index)
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, trigger_count)
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, action_count)
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, reset_after)
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, delay)
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, unknown5)
	OPENNOVA_RECORD_READ_ONLY(MissionEvent, Variant::INT, unknown6)
}

// --- MissionEventTrigger ----------------------------------------------------

Ref<MissionEventTrigger> MissionEventTrigger::make(int p_main_type, int p_sub_type, int p_param1,
		int p_param2, int p_param3, int p_param4, bool p_negated, bool p_logic_or,
		bool p_logic_xor) {
	opennova::mission::MissionTriggerRecord v;
	v.main_type = p_main_type;
	v.sub_type = p_sub_type;
	v.param1 = p_param1;
	v.param2 = p_param2;
	v.param3 = p_param3;
	v.param4 = p_param4;
	v.negated = p_negated;
	v.logic_or = p_logic_or;
	v.logic_xor = p_logic_xor;
	Ref<MissionEventTrigger> out;
	out.instantiate();
	out->assign(v);
	return out;
}

String MissionEventTrigger::get_main_type_name() const { return gd(value_.main_type_name); }
String MissionEventTrigger::get_sub_type_name() const { return gd(value_.sub_type_name); }
String MissionEventTrigger::get_logic_operator() const { return gd(value_.logic_operator); }

void MissionEventTrigger::_bind_methods() {
	ClassDB::bind_static_method("MissionEventTrigger",
			D_METHOD("make", "main_type", "sub_type", "param1", "param2", "param3", "param4",
					"negated", "logic_or", "logic_xor"),
			&MissionEventTrigger::make, DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(false),
			DEFVAL(false), DEFVAL(false));
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::INT, index)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::INT, condition_flags)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::INT, main_type)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::STRING, main_type_name)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::INT, sub_type)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::STRING, sub_type_name)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::INT, param1)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::INT, param2)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::INT, param3)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::INT, param4)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::INT, unknown7)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::BOOL, negated)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::BOOL, logic_or)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::BOOL, logic_xor)
	OPENNOVA_RECORD_READ_ONLY(MissionEventTrigger, Variant::STRING, logic_operator)
}

// --- MissionEventAction -----------------------------------------------------

Ref<MissionEventAction> MissionEventAction::make(int p_action_type, int p_action_sub_type,
		int p_param1, int p_param2, int p_param3, int p_param4) {
	opennova::mission::MissionActionRecord v;
	v.action_type = p_action_type;
	v.action_sub_type = p_action_sub_type;
	v.param1 = p_param1;
	v.param2 = p_param2;
	v.param3 = p_param3;
	v.param4 = p_param4;
	Ref<MissionEventAction> out;
	out.instantiate();
	out->assign(v);
	return out;
}

String MissionEventAction::get_action_type_name() const { return gd(value_.action_type_name); }
String MissionEventAction::get_action_sub_type_name() const {
	return gd(value_.action_sub_type_name);
}

void MissionEventAction::_bind_methods() {
	ClassDB::bind_static_method("MissionEventAction",
			D_METHOD("make", "action_type", "action_sub_type", "param1", "param2", "param3",
					"param4"),
			&MissionEventAction::make, DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(0), DEFVAL(0));
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::INT, index)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::INT, action_type)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::STRING, action_type_name)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::INT, action_sub_type)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::STRING, action_sub_type_name)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::INT, param1)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::INT, param2)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::INT, param3)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::INT, param4)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::INT, reserved0)
	OPENNOVA_RECORD_READ_ONLY(MissionEventAction, Variant::INT, reserved1)
}

// --- MissionLogicReference / MissionLogicDiagnostic -------------------------

String MissionLogicReference::get_source_kind() const { return gd(value_.source_kind); }
String MissionLogicReference::get_target_kind() const { return gd(value_.target_kind); }
String MissionLogicReference::get_label() const { return gd(value_.label); }

void MissionLogicReference::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(MissionLogicReference, Variant::STRING, source_kind)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicReference, Variant::INT, source_index)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicReference, Variant::STRING, target_kind)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicReference, Variant::INT, target_index)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicReference, Variant::INT, param_slot)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicReference, Variant::INT, raw_value)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicReference, Variant::STRING, label)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicReference, Variant::BOOL, valid)
}

String MissionLogicDiagnostic::get_severity() const { return gd(value_.severity); }
String MissionLogicDiagnostic::get_code() const { return gd(value_.code); }
String MissionLogicDiagnostic::get_message() const { return gd(value_.message); }
String MissionLogicDiagnostic::get_subject_kind() const { return gd(value_.subject_kind); }

void MissionLogicDiagnostic::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(MissionLogicDiagnostic, Variant::STRING, severity)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicDiagnostic, Variant::STRING, code)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicDiagnostic, Variant::STRING, message)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicDiagnostic, Variant::STRING, subject_kind)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicDiagnostic, Variant::INT, subject_index)
}

// --- MissionEventChain ------------------------------------------------------

Ref<MissionEvent> MissionEventChain::get_event() const {
	Ref<MissionEvent> out;
	out.instantiate();
	out->assign(value_.event);
	return out;
}

TypedArray<MissionEventTrigger> MissionEventChain::get_triggers() const {
	TypedArray<MissionEventTrigger> out;
	for (const opennova::mission::MissionTriggerRecord &t : value_.triggers) {
		Ref<MissionEventTrigger> row;
		row.instantiate();
		row->assign(t);
		out.push_back(row);
	}
	return out;
}

TypedArray<MissionEventAction> MissionEventChain::get_actions() const {
	TypedArray<MissionEventAction> out;
	for (const opennova::mission::MissionActionRecord &a : value_.actions) {
		Ref<MissionEventAction> row;
		row.instantiate();
		row->assign(a);
		out.push_back(row);
	}
	return out;
}

TypedArray<MissionLogicReference> MissionEventChain::get_references() const {
	TypedArray<MissionLogicReference> out;
	for (const opennova::mission::MissionLogicReference &r : value_.references) {
		Ref<MissionLogicReference> row;
		row.instantiate();
		row->assign(r);
		out.push_back(row);
	}
	return out;
}

TypedArray<MissionLogicDiagnostic> MissionEventChain::get_diagnostics() const {
	TypedArray<MissionLogicDiagnostic> out;
	for (const opennova::mission::MissionLogicDiagnostic &d : value_.diagnostics) {
		Ref<MissionLogicDiagnostic> row;
		row.instantiate();
		row->assign(d);
		out.push_back(row);
	}
	return out;
}

void MissionEventChain::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY_OBJECT(MissionEventChain, event, MissionEvent)
	OPENNOVA_RECORD_READ_ONLY_ROWS(MissionEventChain, triggers, MissionEventTrigger)
	OPENNOVA_RECORD_READ_ONLY_ROWS(MissionEventChain, actions, MissionEventAction)
	OPENNOVA_RECORD_READ_ONLY_ROWS(MissionEventChain, references, MissionLogicReference)
	OPENNOVA_RECORD_READ_ONLY_ROWS(MissionEventChain, diagnostics, MissionLogicDiagnostic)
}

// --- MissionLogicSummary ----------------------------------------------------

void MissionLogicSummary::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(MissionLogicSummary, Variant::INT, events)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicSummary, Variant::INT, triggers)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicSummary, Variant::INT, actions)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicSummary, Variant::INT, area_triggers)
	OPENNOVA_RECORD_READ_ONLY(MissionLogicSummary, Variant::INT, diagnostics)
}
