#include "lights/effect_light_report.h"
#include "util/record_bind.h"

using namespace godot;

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

void EffectLightRow::_bind_methods() {
	EFFECT_LIGHT_ROW_FIELDS(OPENNOVA_RECORD_FIELD)
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
	EFFECT_LIGHT_REPORT_FIELDS(OPENNOVA_RECORD_FIELD)
	ClassDB::bind_method(D_METHOD("get_rows"), &EffectLightReport::get_rows);
	ClassDB::bind_method(D_METHOD("set_rows", "value"), &EffectLightReport::set_rows);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "rows", PROPERTY_HINT_ARRAY_TYPE, "EffectLightRow"),
			"set_rows", "get_rows");
	ClassDB::bind_method(D_METHOD("to_json_value"), &EffectLightReport::to_json_value);
}
