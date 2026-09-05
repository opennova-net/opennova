#include "simulation/fp_viewmodel_spec.h"

using namespace godot;

String FpViewmodelSpec::get_gun() const { return String(value_.gun.c_str()); }
String FpViewmodelSpec::get_arms() const { return String(value_.arms.c_str()); }
String FpViewmodelSpec::get_adm() const { return String(value_.adm.c_str()); }

void FpViewmodelSpec::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_gun"), &FpViewmodelSpec::get_gun);
	ClassDB::bind_method(D_METHOD("get_arms"), &FpViewmodelSpec::get_arms);
	ClassDB::bind_method(D_METHOD("get_adm"), &FpViewmodelSpec::get_adm);
	ClassDB::bind_method(D_METHOD("get_show_arms"), &FpViewmodelSpec::get_show_arms);
#define FP_VIEWMODEL_SPEC_READ_ONLY(m_variant, m_name)                                  \
	ADD_PROPERTY(PropertyInfo(m_variant, #m_name, PROPERTY_HINT_NONE, "",               \
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY),            \
			"", "get_" #m_name);
	FP_VIEWMODEL_SPEC_READ_ONLY(Variant::STRING, gun)
	FP_VIEWMODEL_SPEC_READ_ONLY(Variant::STRING, arms)
	FP_VIEWMODEL_SPEC_READ_ONLY(Variant::STRING, adm)
	FP_VIEWMODEL_SPEC_READ_ONLY(Variant::BOOL, show_arms)
#undef FP_VIEWMODEL_SPEC_READ_ONLY
}
