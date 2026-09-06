#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>

#include <cstdint>

#include <formats/threedi/threedi_3di3.h>

namespace godot {

// One LGHT record, named "LP##": the attenuation range, the generator
// style/phase/rate, the two colors (BGR bytes; quantized on set), the owning
// subobject, the flag byte, the falloff byte and the raw rotation and
// view-projection words. The node position is the offset in the presentation
// frame. Raw on purpose: the effect-light director reads these words as the
// file carries them (engine ThreediLight), and an export must reproduce them.
class ModelLight3D : public Node3D {
	GDCLASS(ModelLight3D, Node3D)

	float atten_start_ = 0.0f;
	float atten_end_ = 0.0f;
	int style_ = 0;
	int phase_ = 0;
	int rate_ = 0;
	uint8_t color_start_[4] = {255, 255, 255, 0};
	uint8_t color_end_[4] = {255, 255, 255, 0};
	int subobject_ = 0;
	int flags_ = 0;
	int unknown1_ = 0;
	int falloff_byte_ = 0;
	PackedFloat32Array rotation_;
	PackedFloat32Array view_proj_;

protected:
	static void _bind_methods();

public:
	void set_atten_start(float p_value) { atten_start_ = p_value; }
	float get_atten_start() const { return atten_start_; }
	void set_atten_end(float p_value) { atten_end_ = p_value; }
	float get_atten_end() const { return atten_end_; }
	void set_style(int p_value) { style_ = p_value; }
	int get_style() const { return style_; }
	void set_phase(int p_value) { phase_ = p_value; }
	int get_phase() const { return phase_; }
	void set_rate(int p_value) { rate_ = p_value; }
	int get_rate() const { return rate_; }
	void set_color_start(const Color &p_value);
	Color get_color_start() const;
	void set_color_end(const Color &p_value);
	Color get_color_end() const;
	void set_subobject(int p_value) { subobject_ = p_value; }
	int get_subobject() const { return subobject_; }
	void set_flags(int p_value) { flags_ = p_value; }
	int get_flags() const { return flags_; }
	void set_unknown1(int p_value) { unknown1_ = p_value; }
	int get_unknown1() const { return unknown1_; }
	void set_falloff_byte(int p_value) { falloff_byte_ = p_value; }
	int get_falloff_byte() const { return falloff_byte_; }
	void set_rotation_words(const PackedFloat32Array &p_value) { rotation_ = p_value; }
	PackedFloat32Array get_rotation_words() const { return rotation_; }
	void set_view_proj(const PackedFloat32Array &p_value) { view_proj_ = p_value; }
	PackedFloat32Array get_view_proj() const { return view_proj_; }

	// LGHT record <-> node (the offset rides the node position).
	void assign(const opennova::threedi::ThreediLight &p_light);
	bool write(opennova::threedi::ThreediLight &r_light) const;
};

} // namespace godot
