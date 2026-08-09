#include "nova_particle_curve_ref.h"

using namespace godot;

void ParticleCurveRef::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_name", "value"), &ParticleCurveRef::set_name);
	ClassDB::bind_method(D_METHOD("get_name"), &ParticleCurveRef::get_name);
	ClassDB::bind_method(D_METHOD("set_reverse", "value"), &ParticleCurveRef::set_reverse);
	ClassDB::bind_method(D_METHOD("get_reverse"), &ParticleCurveRef::get_reverse);
	ClassDB::bind_method(D_METHOD("set_inverse", "value"), &ParticleCurveRef::set_inverse);
	ClassDB::bind_method(D_METHOD("get_inverse"), &ParticleCurveRef::get_inverse);
	ClassDB::bind_method(D_METHOD("set_present", "value"), &ParticleCurveRef::set_present);
	ClassDB::bind_method(D_METHOD("get_present"), &ParticleCurveRef::get_present);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "set_name", "get_name");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reverse"), "set_reverse", "get_reverse");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "inverse"), "set_inverse", "get_inverse");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "present"), "set_present", "get_present");
}

void ParticleCurveRef::set_name(const String &p_value) {
	name = p_value;
	present = present || !name.is_empty();
	emit_changed();
}
String ParticleCurveRef::get_name() const { return name; }

void ParticleCurveRef::set_reverse(bool p_value) {
	reverse = p_value;
	emit_changed();
}
bool ParticleCurveRef::get_reverse() const { return reverse; }

void ParticleCurveRef::set_inverse(bool p_value) {
	inverse = p_value;
	emit_changed();
}
bool ParticleCurveRef::get_inverse() const { return inverse; }

void ParticleCurveRef::set_present(bool p_value) {
	present = p_value;
	emit_changed();
}
bool ParticleCurveRef::get_present() const { return present; }

void ParticleCurveRef::copy_from_native(const opennova::particle::CurveRef &ref) {
	name = String::utf8(ref.name.c_str());
	reverse = ref.reverse;
	inverse = ref.inverse;
	present = ref.present;
}

opennova::particle::CurveRef ParticleCurveRef::to_native() const {
	opennova::particle::CurveRef out;
	out.name = name.utf8().get_data();
	out.reverse = reverse;
	out.inverse = inverse;
	out.present = present;
	return out;
}
