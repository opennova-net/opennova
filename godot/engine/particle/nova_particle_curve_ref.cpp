#include "nova_particle_curve_ref.h"

using namespace godot;

void NovaParticleCurveRef::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_name", "value"), &NovaParticleCurveRef::set_name);
	ClassDB::bind_method(D_METHOD("get_name"), &NovaParticleCurveRef::get_name);
	ClassDB::bind_method(D_METHOD("set_reverse", "value"), &NovaParticleCurveRef::set_reverse);
	ClassDB::bind_method(D_METHOD("get_reverse"), &NovaParticleCurveRef::get_reverse);
	ClassDB::bind_method(D_METHOD("set_inverse", "value"), &NovaParticleCurveRef::set_inverse);
	ClassDB::bind_method(D_METHOD("get_inverse"), &NovaParticleCurveRef::get_inverse);
	ClassDB::bind_method(D_METHOD("set_present", "value"), &NovaParticleCurveRef::set_present);
	ClassDB::bind_method(D_METHOD("get_present"), &NovaParticleCurveRef::get_present);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "set_name", "get_name");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reverse"), "set_reverse", "get_reverse");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "inverse"), "set_inverse", "get_inverse");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "present"), "set_present", "get_present");
}

void NovaParticleCurveRef::set_name(const String &p_value) {
	name = p_value;
	present = present || !name.is_empty();
	emit_changed();
}
String NovaParticleCurveRef::get_name() const { return name; }

void NovaParticleCurveRef::set_reverse(bool p_value) {
	reverse = p_value;
	emit_changed();
}
bool NovaParticleCurveRef::get_reverse() const { return reverse; }

void NovaParticleCurveRef::set_inverse(bool p_value) {
	inverse = p_value;
	emit_changed();
}
bool NovaParticleCurveRef::get_inverse() const { return inverse; }

void NovaParticleCurveRef::set_present(bool p_value) {
	present = p_value;
	emit_changed();
}
bool NovaParticleCurveRef::get_present() const { return present; }

void NovaParticleCurveRef::copy_from_native(const opennova::particle::CurveRef &ref) {
	name = String::utf8(ref.name.c_str());
	reverse = ref.reverse;
	inverse = ref.inverse;
	present = ref.present;
}

opennova::particle::CurveRef NovaParticleCurveRef::to_native() const {
	opennova::particle::CurveRef out;
	out.name = name.utf8().get_data();
	out.reverse = reverse;
	out.inverse = inverse;
	out.present = present;
	return out;
}
