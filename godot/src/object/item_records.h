#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/audio/envs_markers.h>

namespace godot {

// One anchored items.def particle-effect slot ({effect, userpoint[, secondary]}
// as authored; empty = key absent). The runtime effect-attach pass consumes
// slot A ("particlefx") through ItemDatabase.get_particle_fx.
// [orig: ItemDef_ParseProperty @ 0x49eb00; slot-A runtime attach witness
// resolve_item_materials_and_spawn_bone_trails @ 0x522ee0 ->
// Entity_SpawnBoneTrailEffect @ 0x43bef0]
class ItemParticleFx : public RefCounted {
	GDCLASS(ItemParticleFx, RefCounted)

	String effect_;
	String userpoint_;
	String secondary_effect_;

protected:
	static void _bind_methods();

public:
	void assign(const String &p_effect, const String &p_userpoint,
			const String &p_secondary_effect);

	String get_effect() const { return effect_; }
	String get_userpoint() const { return userpoint_; }
	String get_secondary_effect() const { return secondary_effect_; }
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
	int right_limit_bam_ = 0;
	int left_limit_bam_ = 0;
	bool designated_g_ = false;
	bool designated_c_ = false;

protected:
	static void _bind_methods();

public:
	void assign(int p_kind, const String &p_userpoint, int p_item_id, int p_stored_slot,
			int p_angle_count, int p_down, int p_up, int p_right, int p_left,
			bool p_designated_g, bool p_designated_c);

	// ItemDatabase.EMPLACEMENT_ADDEWEAP / _G / _C.
	int get_kind() const { return kind_; }
	String get_userpoint() const { return userpoint_; }
	int get_item_id() const { return item_id_; }
	int get_stored_slot() const { return stored_slot_; }
	int get_angle_count() const { return angle_count_; }
	// Four authored limits (retail packs 0 or 4); explicit all-zero limits stay
	// distinct from an omitted fallback.
	bool has_explicit_limits() const { return angle_count_ == 4; }
	int get_down_limit_bam() const { return down_limit_bam_; }
	int get_up_limit_bam() const { return up_limit_bam_; }
	int get_right_limit_bam() const { return right_limit_bam_; }
	int get_left_limit_bam() const { return left_limit_bam_; }
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

} // namespace godot
