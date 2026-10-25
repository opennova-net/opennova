#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/inmatch/loaded_model_list.h>

#include <cstdint>

namespace godot {

// One node of the mission start's frozen model list (Simulation::debug_mission_start_model_nodes;
// engine runtime/inmatch/loaded_model_list.h loaded_model_list_entries, a debug instrument): a
// typed row (ADR 0017, ADR 0042 d5); to_json_value() is the model_list_dump probe's JSON edge.
class ModelListNodeRow : public RefCounted {
	GDCLASS(ModelListNodeRow, RefCounted)

	opennova::inmatch::LoadedModelListEntry value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::inmatch::LoadedModelListEntry &p_value) { value_ = p_value; }

	String get_name() const;
	bool get_foliage() const { return value_.foliage; }
	int64_t get_load_index() const { return value_.load_index; }
	int get_page_index() const { return value_.page_index; }

	Dictionary to_json_value() const;
};

} // namespace godot
