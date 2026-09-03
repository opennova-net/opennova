#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <limits>

namespace godot {

// The outcome of DebugArgSpec::marshal (ADR 0043 d12): the typed positional
// Array an action row's owner call reads, or the refusal naming the argument
// that was missing or outside its domain. One record for both, replacing the
// Variant union the GDScript table returned.
class DebugMarshalResult : public RefCounted {
	GDCLASS(DebugMarshalResult, RefCounted)

	bool refused_ = false;
	String reason_;
	Array args_;

protected:
	static void _bind_methods();

public:
	static Ref<DebugMarshalResult> accepted(const Array &p_args);
	static Ref<DebugMarshalResult> refusal(const String &p_reason);

	bool is_refused() const { return refused_; }
	String get_reason() const { return reason_; }
	Array get_args() const { return args_; }
};

// One positional argument of a debug-control action row: its wire name,
// kind, default and the domain a value must lie in. `marshal` is the one
// validator the table's invoke path and the MCP boundary both run, so an
// argument shape is declared once on its row, published by op=list, and
// refused identically everywhere (ADR 0042 d5: every action validates before
// any engine call).
class DebugArgSpec : public RefCounted {
	GDCLASS(DebugArgSpec, RefCounted)

public:
	enum Kind {
		INT,
		FLOAT,
		BOOL,
		STRING,
		VECTOR3,
	};

private:
	String name_;
	Kind kind_ = INT;
	bool required_ = true;
	Variant default_value_;
	// Numeric domain (per component for VECTOR3); unset = any finite value.
	bool has_range_ = false;
	double minimum_ = -std::numeric_limits<double>::infinity();
	double maximum_ = std::numeric_limits<double>::infinity();
	// STRING: the accepted values; empty = any non-blank string.
	PackedStringArray choices_;

	static Ref<DebugArgSpec> make(const String &p_name, Kind p_kind);
	bool in_range(double p_value) const;
	String range_text() const;
	static String number_text(double p_value);
	static bool is_finite_number(const Variant &p_value);
	// A finite Vector3 from a Vector3, an [x, y, z] Array or an {x, y, z}
	// Dictionary; NIL otherwise.
	static Variant vector3_of(const Variant &p_value);

protected:
	static void _bind_methods();

public:
	static Ref<DebugArgSpec> integer(const String &p_name);
	static Ref<DebugArgSpec> number(const String &p_name);
	static Ref<DebugArgSpec> boolean(const String &p_name);
	static Ref<DebugArgSpec> text(const String &p_name);
	// A Vector3, accepted on the wire as Vector3, [x, y, z] or {x, y, z}.
	static Ref<DebugArgSpec> vector3(const String &p_name);

	Ref<DebugArgSpec> between(double p_lo, double p_hi);
	Ref<DebugArgSpec> at_least(double p_lo);
	Ref<DebugArgSpec> one_of(const PackedStringArray &p_values);
	Ref<DebugArgSpec> optional(const Variant &p_default);

	String get_name() const { return name_; }
	Kind get_kind() const { return kind_; }
	bool is_required() const { return required_; }
	Variant get_default_value() const { return default_value_; }
	bool get_has_range() const { return has_range_; }
	double get_minimum() const { return minimum_; }
	double get_maximum() const { return maximum_; }
	PackedStringArray get_choices() const { return choices_; }

	String kind_name() const;
	// The human domain, for refusal messages.
	String describe() const;
	// The JSON-facing schema row published with the control (op=list).
	Dictionary to_json_value() const;
	// The typed value for one raw wire value, or NIL when it lies outside the
	// spec: numbers are never coerced from strings, floats must be finite,
	// booleans must be JSON booleans.
	Variant coerce(const Variant &p_raw) const;
	// Marshal one action's wire arguments against `p_specs`: null, a
	// positional Array, a by-name Dictionary or a single scalar. The accepted
	// result carries the typed positional Array the row's owner call reads;
	// the refusal names the argument.
	static Ref<DebugMarshalResult> marshal(const TypedArray<DebugArgSpec> &p_specs,
			const Variant &p_raw);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::DebugArgSpec::Kind);
