#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/audio/envs_markers.h>
#include <runtime/mission/promote.h>

#include "simulation/entity_card.h"

namespace godot {

// One anchored items.def particle-effect slot ({effect, userpoint[, secondary]}
// as authored; empty = key absent). The runtime effect-attach pass consumes
// slot A ("particlefx") through ItemDatabase.get_particle_fx.
// [orig: ItemDef_ParseProperty @ 0x49eb00; slot-A runtime attach witness
// resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 ->
// Entity_SpawnBoneTrailEffect @ 0x43bef0]
struct ItemParticleFx {
	bool valid = false; // false = no such item row
	String effect;
	String userpoint;
	String secondary_effect;
};

// One child-emplacement record (addeweap / addeweapG / addeweapC): the source
// userpoint, the child item id, the optional down/up/right/left limits (retail-
// scaled BAM: one authored degree = 11930464), its 1-based stored slot and
// whether it is the designated G / C attachment (the last stored of its kind).
class ItemEmplacementAttachment : public RefCounted {
	GDCLASS(ItemEmplacementAttachment, RefCounted)

	int kind_ = 0;
	String userpoint_;
	int item_id_ = 0;
	int stored_slot_ = 0;
	int angle_count_ = 0;
	int down_limit_bam_ = 0;
	int up_limit_bam_ = 0;
	bool designated_g_ = false;
	bool designated_c_ = false;

protected:
	static void _bind_methods();

public:
	void assign(int p_kind, const String &p_userpoint, int p_item_id, int p_stored_slot,
			int p_angle_count, int p_down, int p_up,
			bool p_designated_g, bool p_designated_c);

	// ItemDatabase.EMPLACEMENT_ADDEWEAP / _G / _C.
	int get_kind() const { return kind_; }
	String get_userpoint() const { return userpoint_; }
	int get_item_id() const { return item_id_; }
	int get_stored_slot() const { return stored_slot_; }
	// Four authored limits (retail packs 0 or 4); explicit all-zero limits stay
	// distinct from an omitted fallback.
	bool has_explicit_limits() const { return angle_count_ == 4; }
	int get_down_limit_bam() const { return down_limit_bam_; }
	int get_up_limit_bam() const { return up_limit_bam_; }
	bool is_designated_g() const { return designated_g_; }
	bool is_designated_c() const { return designated_c_; }
};

// One resolved envs-class ambient marker (audio/envs_markers.h): the authored
// BMS position, the placed entity's id, and the four time-of-day slot set
// names ("" = silent slot).
class EnvsMarkerRow : public RefCounted {
	GDCLASS(EnvsMarkerRow, RefCounted)

	Vector3 position_;
	int bms_id_ = 0;
	PackedStringArray slot_sets_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::audio::EnvsMarker &p_marker);

	Vector3 get_position() const { return position_; }
	int get_bms_id() const { return bms_id_; }
	PackedStringArray get_slot_sets() const { return slot_sets_; }
};

// One resolved child-emplacement anchor of an item's seat spec
// (mission::ItemEmplacementAttachmentSpec): the child type, whether the parent
// model authored its userpoint anchor, and the authored limits.
class ItemSeatAttachmentRow : public RefCounted {
	GDCLASS(ItemSeatAttachmentRow, RefCounted)

	opennova::mission::ItemEmplacementAttachmentSpec value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::mission::ItemEmplacementAttachmentSpec &p_value) { value_ = p_value; }

	// The full items.def id (the raw BMS type id + 100000).
	int get_item_id() const;
	// ItemDatabase.EMPLACEMENT_ADDEWEAP / _G / _C.
	int get_kind() const { return static_cast<int>(value_.kind); }
	int get_stored_slot() const { return value_.stored_slot; }
	int get_down_limit_bam() const { return value_.down_limit_bam; }
	int get_up_limit_bam() const { return value_.up_limit_bam; }
};

// The static mount analysis of ONE item (ItemDatabase.extract_seat_specs_for_item):
// the native seat-spec extraction over its items.def row + its model
// (mission::extract_item_seat_specs), shaped for inspection. `error` names
// why nothing resolved ("missing_resource_root_or_item_db", "item_not_found");
// an item without runtime metadata yields an empty card with no error.
class ItemSeatCard : public RefCounted {
	GDCLASS(ItemSeatCard, RefCounted)

	int item_id_ = 0;
	int type_id_ = 0;
	String display_name_;
	String graphic_;
	String model_;
	String error_;
	String primary_weapon_;
	bool mount_config_valid_ = false;
	int mount_config_ = 0;
	TypedArray<EntityCardSeat> seats_;
	TypedArray<ItemSeatAttachmentRow> emplacement_attachments_;

protected:
	static void _bind_methods();

public:
	void set_identity(int p_item_id, int p_type_id);
	void set_error(const String &p_error) { error_ = p_error; }
	void set_model(const String &p_display_name, const String &p_graphic, const String &p_model);
	void assign_spec(const opennova::mission::ItemSeatSpec &p_spec);

	int get_item_id() const { return item_id_; }
	// The raw BMS type id (item id minus 100000).
	int get_type_id() const { return type_id_; }
	String get_display_name() const { return display_name_; }
	String get_graphic() const { return graphic_; }
	// The model file the graphic names ("<graphic>.3di"), empty without one.
	String get_model() const { return model_; }
	String get_error() const { return error_; }
	// The ewep primary_weapon link (weapon.def entry), empty if none.
	String get_primary_weapon() const { return primary_weapon_; }
	// The target definition's phrase_set: presence separate from the value.
	bool is_mount_config_valid() const { return mount_config_valid_; }
	int get_mount_config() const { return mount_config_; }
	// The seat rows (static data: never occupied).
	TypedArray<EntityCardSeat> get_seats() const { return seats_; }
	TypedArray<ItemSeatAttachmentRow> get_emplacement_attachments() const { return emplacement_attachments_; }
};

} // namespace godot
