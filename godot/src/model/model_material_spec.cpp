#include "model/model_material_spec.h"

#include "util/string_convert.h"

#include <cmath>
#include <cstring>
#include <string>

using namespace godot;
using namespace opennova::threedi;

namespace {

int round_scaled(float value, double scale) {
	const double v = static_cast<double>(value) * scale;
	return static_cast<int>(v >= 0.0 ? (v + 0.5) : (v - 0.5));
}

int byte_of_unit(float value) {
	int v = round_scaled(value, 255.0);
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// The style <= 112 rule of every generator: the second byte is a phase (Q8)
// below, a CTRL register index above (the MTRL generator layout the engine's
// reader and writer carry, docs/threedi/3di-gp-format-re.md).
constexpr int kGeneratorRegisterStyle = 112;

int phase_or_reg(int style, float phase, int32_t reg) {
	if (style <= kGeneratorRegisterStyle) {
		int v = round_scaled(phase, 256.0);
		return v < 0 ? 0 : (v > 255 ? 255 : v);
	}
	return reg & 0xFF;
}

void split_phase_or_reg(int style, int value, float &r_phase, int32_t &r_reg) {
	if (style <= kGeneratorRegisterStyle) {
		r_phase = static_cast<float>(value) / 256.0f;
		r_reg = -1;
	} else {
		r_phase = 0.0f;
		r_reg = value;
	}
}

} // namespace

void ModelTextureRow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_texture_name", "value"), &ModelTextureRow::set_texture_name);
	ClassDB::bind_method(D_METHOD("get_texture_name"), &ModelTextureRow::get_texture_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "texture_name"), "set_texture_name", "get_texture_name");
	ClassDB::bind_method(D_METHOD("set_slot", "value"), &ModelTextureRow::set_slot);
	ClassDB::bind_method(D_METHOD("get_slot"), &ModelTextureRow::get_slot);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "slot", PROPERTY_HINT_ENUM, "Diffuse:1,Detail:2,Normal:3,Normal B:4"),
			"set_slot", "get_slot");
	ClassDB::bind_method(D_METHOD("set_type", "value"), &ModelTextureRow::set_type);
	ClassDB::bind_method(D_METHOD("get_type"), &ModelTextureRow::get_type);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "type", PROPERTY_HINT_ENUM,
						 "Diffuse:0,Normal from MDT:4,Normal from TGA alpha:5"),
			"set_type", "get_type");
	ClassDB::bind_method(D_METHOD("set_flags", "value"), &ModelTextureRow::set_flags);
	ClassDB::bind_method(D_METHOD("get_flags"), &ModelTextureRow::get_flags);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "flags", PROPERTY_HINT_FLAGS, "Animated:1,Clamped:2"), "set_flags",
			"get_flags");
	ClassDB::bind_method(D_METHOD("set_frame", "value"), &ModelTextureRow::set_frame);
	ClassDB::bind_method(D_METHOD("get_frame"), &ModelTextureRow::get_frame);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frame"), "set_frame", "get_frame");
}

void ModelTextureRow::assign(const ThreediMaterialTexture &p_tex) {
	texture_name_ = opennova::to_gd(std::string(p_tex.name));
	slot_ = p_tex.slot;
	type_ = p_tex.type;
	flags_ = p_tex.flags;
	frame_ = p_tex.frame;
}

bool ModelTextureRow::write(ThreediMaterialTexture &r_tex) const {
	const std::string name = opennova::to_std(texture_name_);
	if (name.size() > sizeof(r_tex.name) - 1) {
		return false;
	}
	memset(&r_tex, 0, sizeof(r_tex));
	snprintf(r_tex.name, sizeof(r_tex.name), "%s", name.c_str());
	r_tex.slot = static_cast<uint8_t>(slot_);
	r_tex.type = static_cast<uint8_t>(type_);
	r_tex.flags = static_cast<uint8_t>(flags_);
	r_tex.frame = static_cast<uint8_t>(frame_);
	return true;
}

void ModelMaterialSpec::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_material_name", "value"), &ModelMaterialSpec::set_material_name);
	ClassDB::bind_method(D_METHOD("get_material_name"), &ModelMaterialSpec::get_material_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "material_name"), "set_material_name", "get_material_name");
	ClassDB::bind_method(D_METHOD("set_shader_tag", "value"), &ModelMaterialSpec::set_shader_tag);
	ClassDB::bind_method(D_METHOD("get_shader_tag"), &ModelMaterialSpec::get_shader_tag);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "shader_tag"), "set_shader_tag", "get_shader_tag");
	ClassDB::bind_method(D_METHOD("set_alpha_strips", "value"), &ModelMaterialSpec::set_alpha_strips);
	ClassDB::bind_method(D_METHOD("get_alpha_strips"), &ModelMaterialSpec::get_alpha_strips);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "alpha_strips"), "set_alpha_strips", "get_alpha_strips");
	ClassDB::bind_method(D_METHOD("set_textures", "value"), &ModelMaterialSpec::set_textures);
	ClassDB::bind_method(D_METHOD("get_textures"), &ModelMaterialSpec::get_textures);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "textures", PROPERTY_HINT_ARRAY_TYPE,
						 String::num_int64(Variant::OBJECT) + "/" + String::num_int64(PROPERTY_HINT_RESOURCE_TYPE) +
								 ":ModelTextureRow"),
			"set_textures", "get_textures");
#define MODEL_MATERIAL_INT_BIND(m_name)                                                                   \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &ModelMaterialSpec::set_##m_name);           \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ModelMaterialSpec::get_##m_name);                    \
	ADD_PROPERTY(PropertyInfo(Variant::INT, #m_name), "set_" #m_name, "get_" #m_name);
	MODEL_MATERIAL_INT_FIELDS(MODEL_MATERIAL_INT_BIND)
#undef MODEL_MATERIAL_INT_BIND
#define MODEL_MATERIAL_COLOR_BIND(m_name)                                                                 \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &ModelMaterialSpec::set_##m_name);           \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &ModelMaterialSpec::get_##m_name);                    \
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, #m_name), "set_" #m_name, "get_" #m_name);
	MODEL_MATERIAL_COLOR_FIELDS(MODEL_MATERIAL_COLOR_BIND)
#undef MODEL_MATERIAL_COLOR_BIND
	ClassDB::bind_method(D_METHOD("get_diffuse_texture"), &ModelMaterialSpec::get_diffuse_texture);
}

Color ModelMaterialSpec::color_from_bytes(const uint8_t p_rgba[4]) {
	return Color(p_rgba[0] / 255.0f, p_rgba[1] / 255.0f, p_rgba[2] / 255.0f, p_rgba[3] / 255.0f);
}

void ModelMaterialSpec::bytes_from_color(const Color &p_color, uint8_t r_rgba[4]) {
	r_rgba[0] = static_cast<uint8_t>(byte_of_unit(p_color.r));
	r_rgba[1] = static_cast<uint8_t>(byte_of_unit(p_color.g));
	r_rgba[2] = static_cast<uint8_t>(byte_of_unit(p_color.b));
	r_rgba[3] = static_cast<uint8_t>(byte_of_unit(p_color.a));
}

String ModelMaterialSpec::get_diffuse_texture() const {
	for (int i = 0; i < textures_.size(); ++i) {
		const Ref<ModelTextureRow> row = textures_[i];
		if (row.is_valid() && row->get_slot() == THREEDI_TEX_SLOT_DIFFUSE) {
			return row->get_texture_name();
		}
	}
	return String();
}

void ModelMaterialSpec::assign(const ThreediMaterial &p_material) {
	shader_tag_ = opennova::to_gd(std::string(p_material.shader_name));
	material_flags_ = p_material.material_flags;
	alpha_test_byte_ = p_material.alpha_test_value_byte;
	emissive_type_ = p_material.emissive_type;
	emissive_type2_ = p_material.emissive_type2;
	is_glass_ = p_material.is_glass;
	glass_type2_ = p_material.glass_type2;
	alpha_gen_style_ = p_material.alpha_gen.style;
	alpha_gen_phase_or_reg_ = phase_or_reg(alpha_gen_style_, p_material.alpha_gen.phase, p_material.alpha_gen.reg);
	alpha_gen_rate_q8_ = round_scaled(p_material.alpha_gen.rate, 256.0);
	alpha_gen_start_ = p_material.alpha_gen.start;
	alpha_gen_end_ = p_material.alpha_gen.end;
	rgb_gen_style_ = p_material.rgb_gen.style;
	rgb_gen_phase_or_reg_ = phase_or_reg(rgb_gen_style_, p_material.rgb_gen.phase, p_material.rgb_gen.reg);
	rgb_gen_rate_q8_ = round_scaled(p_material.rgb_gen.rate, 256.0);
	rgb_gen2_style_ = p_material.rgb_gen2.style;
	rgb_gen2_phase_or_reg_ = phase_or_reg(rgb_gen2_style_, p_material.rgb_gen2.phase, p_material.rgb_gen2.reg);
	rgb_gen2_rate_q8_ = round_scaled(p_material.rgb_gen2.rate, 256.0);
	u_style_ = p_material.u_params.style;
	u_phase_or_reg_ = phase_or_reg(u_style_, p_material.u_params.phase, p_material.u_params.reg);
	u_rate_q8_ = round_scaled(p_material.u_params.gen_rate, 256.0);
	u_start_q8_ = round_scaled(p_material.u_params.start, 256.0);
	u_end_q8_ = round_scaled(p_material.u_params.end, 256.0);
	v_style_ = p_material.v_params.style;
	v_phase_or_reg_ = phase_or_reg(v_style_, p_material.v_params.phase, p_material.v_params.reg);
	v_rate_q8_ = round_scaled(p_material.v_params.gen_rate, 256.0);
	v_start_q8_ = round_scaled(p_material.v_params.start, 256.0);
	v_end_q8_ = round_scaled(p_material.v_params.end, 256.0);
	animation_frames_ = p_material.animation.num_frames;
	animation_type_ = p_material.animation.animation_type;
	animation_cycle_ = p_material.animation.cycle_frame_time;
	for (int k = 0; k < 4; ++k) {
		rgb_gen_start_color_[k] = static_cast<uint8_t>(byte_of_unit(p_material.rgb_gen.start_color[k]));
		rgb_gen_end_color_[k] = static_cast<uint8_t>(byte_of_unit(p_material.rgb_gen.end_color[k]));
		rgb_gen2_start_color_[k] = static_cast<uint8_t>(byte_of_unit(p_material.rgb_gen2.start_color[k]));
		rgb_gen2_end_color_[k] = static_cast<uint8_t>(byte_of_unit(p_material.rgb_gen2.end_color[k]));
		reflect_color_[k] = static_cast<uint8_t>(byte_of_unit(p_material.reflect_color[k]));
		reflect_color2_[k] = static_cast<uint8_t>(byte_of_unit(p_material.reflect_color2[k]));
	}
	textures_.clear();
	for (uint32_t i = 0; i < p_material.texture_count && i < 24u; ++i) {
		Ref<ModelTextureRow> row;
		row.instantiate();
		row->assign(p_material.textures[i]);
		textures_.push_back(row);
	}
}

bool ModelMaterialSpec::write(ThreediMaterial &r_material, String &r_error) const {
	memset(&r_material, 0, sizeof(r_material));
	const std::string tag = opennova::to_std(shader_tag_);
	if (tag.empty() || tag.size() > sizeof(r_material.shader_name) - 1) {
		r_error = "material '" + material_name_ + "': shader tag missing or longer than 32 characters";
		return false;
	}
	snprintf(r_material.shader_name, sizeof(r_material.shader_name), "%s", tag.c_str());
	if (textures_.size() > 24) {
		r_error = "material '" + material_name_ + "': more than 24 texture rows";
		return false;
	}
	r_material.texture_count = static_cast<uint32_t>(textures_.size());
	for (int i = 0; i < textures_.size(); ++i) {
		const Ref<ModelTextureRow> row = textures_[i];
		if (row.is_null() || !row->write(r_material.textures[i])) {
			r_error = "material '" + material_name_ + "': texture row " + String::num_int64(i) +
					" is empty or its name exceeds 16 characters";
			return false;
		}
	}
	r_material.material_flags = static_cast<uint8_t>(material_flags_);
	r_material.alpha_test_value_byte = static_cast<uint8_t>(alpha_test_byte_);
	r_material.emissive_type = static_cast<uint8_t>(emissive_type_);
	r_material.emissive_type2 = static_cast<uint8_t>(emissive_type2_);
	r_material.is_glass = static_cast<uint8_t>(is_glass_);
	r_material.glass_type2 = static_cast<uint8_t>(glass_type2_);
	r_material.alpha_gen.style = static_cast<uint8_t>(alpha_gen_style_);
	split_phase_or_reg(alpha_gen_style_, alpha_gen_phase_or_reg_, r_material.alpha_gen.phase, r_material.alpha_gen.reg);
	r_material.alpha_gen.rate = static_cast<float>(alpha_gen_rate_q8_) / 256.0f;
	r_material.alpha_gen.start = static_cast<int16_t>(alpha_gen_start_);
	r_material.alpha_gen.end = static_cast<int16_t>(alpha_gen_end_);
	r_material.rgb_gen.style = static_cast<uint8_t>(rgb_gen_style_);
	split_phase_or_reg(rgb_gen_style_, rgb_gen_phase_or_reg_, r_material.rgb_gen.phase, r_material.rgb_gen.reg);
	r_material.rgb_gen.rate = static_cast<float>(rgb_gen_rate_q8_) / 256.0f;
	r_material.rgb_gen2.style = static_cast<uint8_t>(rgb_gen2_style_);
	split_phase_or_reg(rgb_gen2_style_, rgb_gen2_phase_or_reg_, r_material.rgb_gen2.phase, r_material.rgb_gen2.reg);
	r_material.rgb_gen2.rate = static_cast<float>(rgb_gen2_rate_q8_) / 256.0f;
	r_material.u_params.style = static_cast<uint8_t>(u_style_);
	split_phase_or_reg(u_style_, u_phase_or_reg_, r_material.u_params.phase, r_material.u_params.reg);
	r_material.u_params.gen_rate = static_cast<float>(u_rate_q8_) / 256.0f;
	r_material.u_params.start = static_cast<float>(u_start_q8_) / 256.0f;
	r_material.u_params.end = static_cast<float>(u_end_q8_) / 256.0f;
	r_material.v_params.style = static_cast<uint8_t>(v_style_);
	split_phase_or_reg(v_style_, v_phase_or_reg_, r_material.v_params.phase, r_material.v_params.reg);
	r_material.v_params.gen_rate = static_cast<float>(v_rate_q8_) / 256.0f;
	r_material.v_params.start = static_cast<float>(v_start_q8_) / 256.0f;
	r_material.v_params.end = static_cast<float>(v_end_q8_) / 256.0f;
	r_material.animation.num_frames = static_cast<uint8_t>(animation_frames_);
	r_material.animation.animation_type = static_cast<uint8_t>(animation_type_);
	r_material.animation.cycle_frame_time = static_cast<int16_t>(animation_cycle_);
	for (int k = 0; k < 4; ++k) {
		r_material.rgb_gen.start_color[k] = rgb_gen_start_color_[k] / 255.0f;
		r_material.rgb_gen.end_color[k] = rgb_gen_end_color_[k] / 255.0f;
		r_material.rgb_gen2.start_color[k] = rgb_gen2_start_color_[k] / 255.0f;
		r_material.rgb_gen2.end_color[k] = rgb_gen2_end_color_[k] / 255.0f;
		r_material.reflect_color[k] = reflect_color_[k] / 255.0f;
		r_material.reflect_color2[k] = reflect_color2_[k] / 255.0f;
	}
	return true;
}
