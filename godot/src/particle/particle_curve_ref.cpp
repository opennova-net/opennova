#include "particle/particle_curve_ref.h"

using namespace godot;

void ParticleCurveRef::_bind_methods() {
	// No GDScript surface: the class stays registered only as the typed
	// Ref<ParticleCurveRef> the C++ particle runtime carries.
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
