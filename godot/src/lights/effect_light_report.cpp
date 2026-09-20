#include "lights/effect_light_report.h"
#include "util/variant_type_of.h"

using namespace godot;

#define EFFECT_LIGHT_BIND_FIELD(m_type, m_name, m_default)                                         \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

Dictionary EffectLightRow::to_json_value() const {
	Dictionary out;
	out["position"] = position_;
	out["color"] = color_;
	out["range"] = range_;
	out["atten2"] = attenuation_quadratic_;
	out["handle"] = handle_;
	out["retail_handle"] = retail_handle_;
	return out;
}

void CoronaRow::_bind_methods() {
	CORONA_ROW_FIELDS(EFFECT_LIGHT_BIND_FIELD)
}

void EffectLightRow::_bind_methods() {
	EFFECT_LIGHT_ROW_FIELDS(EFFECT_LIGHT_BIND_FIELD)
	ClassDB::bind_method(D_METHOD("to_json_value"), &EffectLightRow::to_json_value);
}

Dictionary EffectLightReport::to_json_value() const {
	Dictionary out;
#define EFFECT_LIGHT_REPORT_JSON(m_type, m_name, m_default) out[#m_name] = m_name##_;
	EFFECT_LIGHT_REPORT_FIELDS(EFFECT_LIGHT_REPORT_JSON)
#undef EFFECT_LIGHT_REPORT_JSON
	Array rows;
	for (int64_t i = 0; i < rows_.size(); ++i) {
		const Ref<EffectLightRow> row = rows_[i];
		if (row.is_valid()) rows.push_back(row->to_json_value());
	}
	out["rows"] = rows;
	return out;
}

void EffectLightReport::_bind_methods() {
	EFFECT_LIGHT_REPORT_FIELDS(EFFECT_LIGHT_BIND_FIELD)
	ClassDB::bind_method(D_METHOD("get_rows"), &EffectLightReport::get_rows);
	ClassDB::bind_method(D_METHOD("set_rows", "value"), &EffectLightReport::set_rows);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "rows", PROPERTY_HINT_ARRAY_TYPE, "EffectLightRow"),
			"set_rows", "get_rows");
	ClassDB::bind_method(D_METHOD("add_row", "row"), &EffectLightReport::add_row);
	ClassDB::bind_method(D_METHOD("to_json_value"), &EffectLightReport::to_json_value);
}

#undef EFFECT_LIGHT_BIND_FIELD
