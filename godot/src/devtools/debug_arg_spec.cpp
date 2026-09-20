#include "devtools/debug_arg_spec.h"

#include <godot_cpp/variant/vector3.hpp>

#include "util/record_bind.h"

#include <cmath>

namespace godot {

// --- DebugMarshalResult ------------------------------------------------------

Ref<DebugMarshalResult> DebugMarshalResult::accepted(const Array &p_args) {
	Ref<DebugMarshalResult> out;
	out.instantiate();
	out->args_ = p_args;
	return out;
}

Ref<DebugMarshalResult> DebugMarshalResult::refusal(const String &p_reason) {
	Ref<DebugMarshalResult> out;
	out.instantiate();
	out->refused_ = true;
	out->reason_ = p_reason;
	return out;
}

void DebugMarshalResult::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY_IS(DebugMarshalResult, refused)
	OPENNOVA_RECORD_READ_ONLY(DebugMarshalResult, Variant::STRING, reason)
	OPENNOVA_RECORD_READ_ONLY(DebugMarshalResult, Variant::ARRAY, args)
}

// --- DebugArgSpec ------------------------------------------------------------

Ref<DebugArgSpec> DebugArgSpec::make(const String &p_name, Kind p_kind) {
	Ref<DebugArgSpec> spec;
	spec.instantiate();
	spec->name_ = p_name;
	spec->kind_ = p_kind;
	return spec;
}

Ref<DebugArgSpec> DebugArgSpec::integer(const String &p_name) {
	return make(p_name, INT);
}

Ref<DebugArgSpec> DebugArgSpec::number(const String &p_name) {
	return make(p_name, FLOAT);
}

Ref<DebugArgSpec> DebugArgSpec::boolean(const String &p_name) {
	return make(p_name, BOOL);
}

Ref<DebugArgSpec> DebugArgSpec::text(const String &p_name) {
	return make(p_name, STRING);
}

Ref<DebugArgSpec> DebugArgSpec::vector3(const String &p_name) {
	return make(p_name, VECTOR3);
}

Ref<DebugArgSpec> DebugArgSpec::between(double p_lo, double p_hi) {
	has_range_ = true;
	minimum_ = p_lo;
	maximum_ = p_hi;
	return Ref<DebugArgSpec>(this);
}

Ref<DebugArgSpec> DebugArgSpec::at_least(double p_lo) {
	return between(p_lo, std::numeric_limits<double>::infinity());
}

Ref<DebugArgSpec> DebugArgSpec::one_of(const PackedStringArray &p_values) {
	choices_ = p_values;
	return Ref<DebugArgSpec>(this);
}

Ref<DebugArgSpec> DebugArgSpec::optional(const Variant &p_default) {
	required_ = false;
	default_value_ = p_default;
	return Ref<DebugArgSpec>(this);
}

String DebugArgSpec::kind_name() const {
	switch (kind_) {
		case INT:
			return "int";
		case FLOAT:
			return "float";
		case BOOL:
			return "bool";
		case STRING:
			return "string";
		case VECTOR3:
			return "vector3";
	}
	return "unknown";
}

String DebugArgSpec::describe() const {
	switch (kind_) {
		case INT:
			return "an integer" + range_text();
		case FLOAT:
			return "a finite number" + range_text();
		case BOOL:
			return "a boolean";
		case STRING:
			if (choices_.is_empty()) {
				return "a non-empty string";
			}
			return "one of " + String(", ").join(choices_);
		case VECTOR3:
			return "a finite [x, y, z] position" + range_text();
	}
	return "a value";
}

Dictionary DebugArgSpec::to_json_value() const {
	Dictionary out;
	out["name"] = name_;
	out["kind"] = kind_name();
	out["required"] = required_;
	if (!required_) {
		out["default"] = default_value_;
	}
	if (has_range_ && std::isfinite(minimum_)) {
		out["minimum"] = minimum_;
	}
	if (has_range_ && std::isfinite(maximum_)) {
		out["maximum"] = maximum_;
	}
	if (!choices_.is_empty()) {
		Array choices;
		for (int i = 0; i < choices_.size(); ++i) {
			choices.push_back(choices_[i]);
		}
		out["choices"] = choices;
	}
	return out;
}

Variant DebugArgSpec::coerce(const Variant &p_raw) const {
	switch (kind_) {
		case INT: {
			if (!is_finite_number(p_raw)) {
				return Variant();
			}
			const double value = p_raw;
			if (value != std::floor(value)) {
				return Variant();
			}
			return in_range(value) ? Variant(static_cast<int64_t>(value)) : Variant();
		}
		case FLOAT: {
			if (!is_finite_number(p_raw)) {
				return Variant();
			}
			const double value = p_raw;
			return in_range(value) ? Variant(value) : Variant();
		}
		case BOOL:
			return p_raw.get_type() == Variant::BOOL ? p_raw : Variant();
		case STRING: {
			if (p_raw.get_type() != Variant::STRING) {
				return Variant();
			}
			const String value = p_raw;
			if (value.strip_edges().is_empty()) {
				return Variant();
			}
			if (!choices_.is_empty() && !choices_.has(value)) {
				return Variant();
			}
			return value;
		}
		case VECTOR3: {
			const Variant vector = vector3_of(p_raw);
			if (vector.get_type() == Variant::NIL) {
				return vector;
			}
			const Vector3 v = vector;
			if (!(in_range(v.x) && in_range(v.y) && in_range(v.z))) {
				return Variant();
			}
			return v;
		}
	}
	return Variant();
}

Ref<DebugMarshalResult> DebugArgSpec::marshal(const TypedArray<DebugArgSpec> &p_specs,
		const Variant &p_raw) {
	Array positional;
	if (p_raw.get_type() == Variant::DICTIONARY) {
		const Dictionary by_name = p_raw;
		for (int i = 0; i < p_specs.size(); ++i) {
			const Ref<DebugArgSpec> spec = p_specs[i];
			if (by_name.has(spec->name_)) {
				positional.push_back(by_name[spec->name_]);
			} else if (spec->required_) {
				return DebugMarshalResult::refusal(
						vformat("requires %s (%s)", spec->name_, spec->describe()));
			} else {
				positional.push_back(spec->default_value_);
			}
		}
	} else if (p_raw.get_type() == Variant::ARRAY) {
		positional = p_raw;
	} else if (p_raw.get_type() != Variant::NIL) {
		positional.push_back(p_raw);
	}
	if (positional.size() > p_specs.size()) {
		return DebugMarshalResult::refusal(vformat("takes %d argument(s), got %d",
				static_cast<int64_t>(p_specs.size()), static_cast<int64_t>(positional.size())));
	}
	Array out;
	for (int i = 0; i < p_specs.size(); ++i) {
		const Ref<DebugArgSpec> spec = p_specs[i];
		if (i >= positional.size()) {
			if (spec->required_) {
				return DebugMarshalResult::refusal(
						vformat("requires %s (%s)", spec->name_, spec->describe()));
			}
			out.push_back(spec->default_value_);
			continue;
		}
		const Variant value = spec->coerce(positional[i]);
		if (value.get_type() == Variant::NIL) {
			return DebugMarshalResult::refusal(
					vformat("%s must be %s", spec->name_, spec->describe()));
		}
		out.push_back(value);
	}
	return DebugMarshalResult::accepted(out);
}

bool DebugArgSpec::in_range(double p_value) const {
	return !has_range_ || (p_value >= minimum_ && p_value <= maximum_);
}

String DebugArgSpec::range_text() const {
	if (!has_range_) {
		return String();
	}
	if (std::isfinite(minimum_) && std::isfinite(maximum_)) {
		return vformat(" in %s..%s", number_text(minimum_), number_text(maximum_));
	}
	if (std::isfinite(minimum_)) {
		return " >= " + number_text(minimum_);
	}
	return " <= " + number_text(maximum_);
}

String DebugArgSpec::number_text(double p_value) {
	if (p_value == std::floor(p_value)) {
		return String::num_int64(static_cast<int64_t>(p_value));
	}
	return String::num(p_value);
}

bool DebugArgSpec::is_finite_number(const Variant &p_value) {
	const Variant::Type type = p_value.get_type();
	if (type != Variant::INT && type != Variant::FLOAT) {
		return false;
	}
	const double value = p_value;
	return std::isfinite(value);
}

Variant DebugArgSpec::vector3_of(const Variant &p_value) {
	if (p_value.get_type() == Variant::VECTOR3) {
		const Vector3 v = p_value;
		return v.is_finite() ? p_value : Variant();
	}
	Array parts;
	if (p_value.get_type() == Variant::ARRAY) {
		const Array array = p_value;
		if (array.size() != 3) {
			return Variant();
		}
		parts = array;
	} else if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary dict = p_value;
		if (!(dict.has("x") && dict.has("y") && dict.has("z"))) {
			return Variant();
		}
		parts.push_back(dict["x"]);
		parts.push_back(dict["y"]);
		parts.push_back(dict["z"]);
	} else {
		return Variant();
	}
	for (int i = 0; i < 3; ++i) {
		if (!is_finite_number(parts[i])) {
			return Variant();
		}
	}
	return Vector3(static_cast<float>(static_cast<double>(parts[0])),
			static_cast<float>(static_cast<double>(parts[1])),
			static_cast<float>(static_cast<double>(parts[2])));
}

void DebugArgSpec::_bind_methods() {
	ClassDB::bind_static_method("DebugArgSpec", D_METHOD("integer", "name"), &DebugArgSpec::integer);
	ClassDB::bind_static_method("DebugArgSpec", D_METHOD("number", "name"), &DebugArgSpec::number);
	ClassDB::bind_static_method("DebugArgSpec", D_METHOD("boolean", "name"), &DebugArgSpec::boolean);
	ClassDB::bind_static_method("DebugArgSpec", D_METHOD("text", "name"), &DebugArgSpec::text);
	ClassDB::bind_static_method("DebugArgSpec", D_METHOD("vector3", "name"), &DebugArgSpec::vector3);
	ClassDB::bind_static_method("DebugArgSpec", D_METHOD("marshal", "specs", "raw"),
			&DebugArgSpec::marshal);
	ClassDB::bind_method(D_METHOD("between", "lo", "hi"), &DebugArgSpec::between);
	ClassDB::bind_method(D_METHOD("optional", "default"), &DebugArgSpec::optional);
	ClassDB::bind_method(D_METHOD("kind_name"), &DebugArgSpec::kind_name);
	ClassDB::bind_method(D_METHOD("describe"), &DebugArgSpec::describe);
	ClassDB::bind_method(D_METHOD("to_json_value"), &DebugArgSpec::to_json_value);
	ClassDB::bind_method(D_METHOD("coerce", "raw"), &DebugArgSpec::coerce);
	OPENNOVA_RECORD_READ_ONLY(DebugArgSpec, Variant::STRING, name)
	OPENNOVA_RECORD_READ_ONLY(DebugArgSpec, Variant::INT, kind)
	OPENNOVA_RECORD_READ_ONLY_IS(DebugArgSpec, required)
	ClassDB::bind_method(D_METHOD("get_default_value"), &DebugArgSpec::get_default_value);
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "default_value", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_NIL_IS_VARIANT),
			"", "get_default_value");
	OPENNOVA_RECORD_READ_ONLY(DebugArgSpec, Variant::BOOL, has_range)
	OPENNOVA_RECORD_READ_ONLY(DebugArgSpec, Variant::FLOAT, minimum)
	OPENNOVA_RECORD_READ_ONLY(DebugArgSpec, Variant::FLOAT, maximum)
	OPENNOVA_RECORD_READ_ONLY(DebugArgSpec, Variant::PACKED_STRING_ARRAY, choices)
	BIND_ENUM_CONSTANT(INT);
	BIND_ENUM_CONSTANT(FLOAT);
	BIND_ENUM_CONSTANT(BOOL);
	BIND_ENUM_CONSTANT(STRING);
	BIND_ENUM_CONSTANT(VECTOR3);
}

} // namespace godot
