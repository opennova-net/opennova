#pragma once

#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// One emitted static population of MissionObjectPlacer: the MultiMesh draw
// carries its own placement facts (global blended strip or 512-unit bin, RLOD
// level, bin coordinates) and, for the populations the static terrain-shadow
// attribution reads (`shadow_tagged`), the slot identity arrays (bms/item ids,
// attrib2, cast eligibility, graphic) plus the live row -> slot map the placer
// republishes on every carve or level switch. Typed properties, so the placer
// tests and the shadow-attribution probe read the node instead of metadata.
class StaticPopulationInstance : public MultiMeshInstance3D {
	GDCLASS(StaticPopulationInstance, MultiMeshInstance3D)

public:
	enum PopulationKind {
		POPULATION_GLOBAL = 0,
		POPULATION_BIN = 1,
	};

	void set_population_kind(PopulationKind p_kind) { population_kind_ = p_kind; }
	PopulationKind get_population_kind() const { return population_kind_; }
	void set_lod_index(int p_index) { lod_index_ = p_index; }
	int get_lod_index() const { return lod_index_; }
	void set_bin_x(int p_x) { bin_x_ = p_x; }
	int get_bin_x() const { return bin_x_; }
	void set_bin_z(int p_z) { bin_z_ = p_z; }
	int get_bin_z() const { return bin_z_; }
	void set_shadow_tagged(bool p_tagged) { shadow_tagged_ = p_tagged; }
	bool is_shadow_tagged() const { return shadow_tagged_; }
	void set_graphic(const String &p_graphic) { graphic_ = p_graphic; }
	String get_graphic() const { return graphic_; }
	void set_slot_bms_ids(const PackedInt32Array &p_ids) { slot_bms_ids_ = p_ids; }
	PackedInt32Array get_slot_bms_ids() const { return slot_bms_ids_; }
	void set_slot_item_ids(const PackedInt32Array &p_ids) { slot_item_ids_ = p_ids; }
	PackedInt32Array get_slot_item_ids() const { return slot_item_ids_; }
	void set_slot_attrib2(const PackedInt64Array &p_values) { slot_attrib2_ = p_values; }
	PackedInt64Array get_slot_attrib2() const { return slot_attrib2_; }
	void set_slot_casts_shadow(const PackedByteArray &p_casts) { slot_casts_shadow_ = p_casts; }
	PackedByteArray get_slot_casts_shadow() const { return slot_casts_shadow_; }
	// The population-local slot each live MultiMesh row draws, dense over
	// [0, visible_instance_count) in the placer's swap-remove order.
	void set_row_slots(const PackedInt32Array &p_rows) { row_slots_ = p_rows; }
	PackedInt32Array get_row_slots() const { return row_slots_; }

protected:
	static void _bind_methods();

private:
	PopulationKind population_kind_ = POPULATION_GLOBAL;
	int lod_index_ = 0;
	int bin_x_ = 0;
	int bin_z_ = 0;
	bool shadow_tagged_ = false;
	String graphic_;
	PackedInt32Array slot_bms_ids_;
	PackedInt32Array slot_item_ids_;
	PackedInt64Array slot_attrib2_;
	PackedByteArray slot_casts_shadow_;
	PackedInt32Array row_slots_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::StaticPopulationInstance::PopulationKind);
