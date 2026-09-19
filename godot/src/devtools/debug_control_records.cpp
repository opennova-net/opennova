#include "devtools/debug_control_records.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "util/record_bind.h"

namespace godot {

// --- DebugControlRow ---------------------------------------------------------

String DebugControlRow::kind_name_of(Kind p_kind) {
	switch (p_kind) {
		case CHECK:
			return "check";
		case SLIDER:
			return "slider";
		case ENUM:
			return "enum";
		case ACTION:
			return "action";
	}
	return "unknown";
}

// The wire-stable owner-surface names (the legacy "target" row key).
String DebugControlRow::target_name_of(Target p_target) {
	switch (p_target) {
		case TARGET_WORLD:
			return "world";
		case TARGET_PLAYER:
			return "player";
		case TARGET_SIM:
			return "simulation";
		case TARGET_TERRAIN:
			return "terrain";
		case TARGET_VIEWPORT:
			return "viewport";
		case TARGET_GAME_SHELL:
			return "game_shell";
		case TARGET_ENVIRONMENT:
			return "environment";
		case TARGET_WEATHER:
			return "weather";
	}
	return "unknown";
}

Dictionary DebugControlRow::to_json_value() const {
	Dictionary out;
	out["id"] = String(id_);
	out["page"] = String(page_);
	out["label"] = label_;
	out["description"] = tooltip_;
	out["kind"] = kind_name();
	out["target"] = target_name_of(target_);
	out["minimum"] = minimum_;
	out["maximum"] = maximum_;
	out["step"] = step_;
	Array choices;
	for (int i = 0; i < choices_.size(); ++i) {
		choices.push_back(choices_[i]);
	}
	out["choices"] = choices;
	out["requires_unlock"] = requires_confirm_;
	out["authority"] = authority_ == HOST_ONLY ? "host" : "any";
	Array args;
	for (int i = 0; i < args_.size(); ++i) {
		const Ref<DebugArgSpec> spec = args_[i];
		args.push_back(spec->to_json_value());
	}
	out["args"] = args;
	if (state_.is_valid()) {
		out["state"] = state_->to_json_value();
	}
	return out;
}

Ref<DebugControlRow> DebugControlRow::with_state(const Ref<DebugControlState> &p_state) const {
	Ref<DebugControlRow> out;
	out.instantiate();
	out->id_ = id_;
	out->page_ = page_;
	out->label_ = label_;
	out->tooltip_ = tooltip_;
	out->kind_ = kind_;
	out->target_ = target_;
	out->owner_ = owner_;
	out->default_value_ = default_value_;
	out->minimum_ = minimum_;
	out->maximum_ = maximum_;
	out->step_ = step_;
	out->choices_ = choices_;
	out->requires_confirm_ = requires_confirm_;
	out->authority_ = authority_;
	out->args_ = args_;
	out->state_ = p_state;
	return out;
}

void DebugControlRow::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::STRING_NAME, id)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::STRING_NAME, page)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::STRING, label)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::STRING, tooltip)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::INT, kind)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::INT, target)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::INT, owner)
	ClassDB::bind_method(D_METHOD("get_default_value"), &DebugControlRow::get_default_value);
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "default_value", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_NIL_IS_VARIANT),
			"", "get_default_value");
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::FLOAT, minimum)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::FLOAT, maximum)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::FLOAT, step)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::PACKED_STRING_ARRAY, choices)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::BOOL, requires_confirm)
	OPENNOVA_RECORD_READ_ONLY(DebugControlRow, Variant::INT, authority)
	OPENNOVA_RECORD_READ_ONLY_ROWS(DebugControlRow, args, DebugArgSpec)
	OPENNOVA_RECORD_READ_ONLY_OBJECT(DebugControlRow, state, DebugControlState)
	ClassDB::bind_method(D_METHOD("kind_name"), &DebugControlRow::kind_name);
	ClassDB::bind_method(D_METHOD("to_json_value"), &DebugControlRow::to_json_value);
	BIND_ENUM_CONSTANT(CHECK);
	BIND_ENUM_CONSTANT(SLIDER);
	BIND_ENUM_CONSTANT(ENUM);
	BIND_ENUM_CONSTANT(ACTION);
	BIND_ENUM_CONSTANT(ANY);
	BIND_ENUM_CONSTANT(HOST_ONLY);
	BIND_ENUM_CONSTANT(OWNER_ENGINE);
	BIND_ENUM_CONSTANT(OWNER_DEVICE);
	BIND_ENUM_CONSTANT(TARGET_WORLD);
	BIND_ENUM_CONSTANT(TARGET_PLAYER);
	BIND_ENUM_CONSTANT(TARGET_SIM);
	BIND_ENUM_CONSTANT(TARGET_TERRAIN);
	BIND_ENUM_CONSTANT(TARGET_VIEWPORT);
	BIND_ENUM_CONSTANT(TARGET_GAME_SHELL);
	BIND_ENUM_CONSTANT(TARGET_ENVIRONMENT);
	BIND_ENUM_CONSTANT(TARGET_WEATHER);
}

// --- DebugControlState -------------------------------------------------------

Variant DebugControlState::json_value(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::VECTOR2: {
			const Vector2 v = p_value;
			Dictionary out;
			out["x"] = v.x;
			out["y"] = v.y;
			return out;
		}
		case Variant::VECTOR3: {
			const Vector3 v = p_value;
			Dictionary out;
			out["x"] = v.x;
			out["y"] = v.y;
			out["z"] = v.z;
			return out;
		}
		case Variant::COLOR: {
			const Color c = p_value;
			Dictionary out;
			out["r"] = c.r;
			out["g"] = c.g;
			out["b"] = c.b;
			out["a"] = c.a;
			return out;
		}
		case Variant::STRING_NAME:
			return String(p_value);
		case Variant::DICTIONARY: {
			const Dictionary in = p_value;
			Dictionary out;
			const Array keys = in.keys();
			for (int i = 0; i < keys.size(); ++i) {
				out[String(keys[i])] = json_value(in[keys[i]]);
			}
			return out;
		}
		case Variant::ARRAY: {
			const Array in = p_value;
			Array out;
			for (int i = 0; i < in.size(); ++i) {
				out.push_back(json_value(in[i]));
			}
			return out;
		}
		case Variant::PACKED_STRING_ARRAY: {
			const PackedStringArray in = p_value;
			Array out;
			for (int i = 0; i < in.size(); ++i) {
				out.push_back(in[i]);
			}
			return out;
		}
		case Variant::PACKED_INT32_ARRAY: {
			const PackedInt32Array in = p_value;
			Array out;
			for (int i = 0; i < in.size(); ++i) {
				out.push_back(in[i]);
			}
			return out;
		}
		case Variant::PACKED_INT64_ARRAY: {
			const PackedInt64Array in = p_value;
			Array out;
			for (int i = 0; i < in.size(); ++i) {
				out.push_back(in[i]);
			}
			return out;
		}
		case Variant::PACKED_FLOAT32_ARRAY: {
			const PackedFloat32Array in = p_value;
			Array out;
			for (int i = 0; i < in.size(); ++i) {
				out.push_back(in[i]);
			}
			return out;
		}
		case Variant::PACKED_FLOAT64_ARRAY: {
			const PackedFloat64Array in = p_value;
			Array out;
			for (int i = 0; i < in.size(); ++i) {
				out.push_back(in[i]);
			}
			return out;
		}
		case Variant::OBJECT:
			return Variant();
		default:
			return p_value;
	}
}

Dictionary DebugControlState::to_json_value() const {
	Dictionary out;
	out["id"] = String(id_);
	out["kind"] = DebugControlRow::kind_name_of(kind_);
	out["value"] = json_value(value_);
	out["desired_value"] = json_value(desired_value_);
	out["available"] = available_;
	out["writable"] = writable_;
	out["authoritative"] = authoritative_;
	out["reason"] = reason_;
	return out;
}

void DebugControlState::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(DebugControlState, Variant::STRING_NAME, id)
	OPENNOVA_RECORD_READ_ONLY(DebugControlState, Variant::INT, kind)
	ClassDB::bind_method(D_METHOD("get_value"), &DebugControlState::get_value);
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "value", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_NIL_IS_VARIANT),
			"", "get_value");
	ClassDB::bind_method(D_METHOD("get_desired_value"), &DebugControlState::get_desired_value);
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "desired_value", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_NIL_IS_VARIANT),
			"", "get_desired_value");
	OPENNOVA_RECORD_READ_ONLY_IS(DebugControlState, available)
	OPENNOVA_RECORD_READ_ONLY_IS(DebugControlState, writable)
	OPENNOVA_RECORD_READ_ONLY_IS(DebugControlState, authoritative)
	OPENNOVA_RECORD_READ_ONLY(DebugControlState, Variant::STRING, reason)
	ClassDB::bind_method(D_METHOD("to_json_value"), &DebugControlState::to_json_value);
}

// --- DebugInvokeResult -------------------------------------------------------

Dictionary DebugInvokeResult::to_json_value() const {
	Dictionary out;
	out["error"] = static_cast<int>(error_);
	out["result"] = DebugControlState::json_value(result_);
	out["state"] = state_.is_valid() ? Variant(state_->to_json_value()) : Variant();
	return out;
}

void DebugInvokeResult::_bind_methods() {
	OPENNOVA_RECORD_READ_ONLY(DebugInvokeResult, Variant::INT, error)
	ClassDB::bind_method(D_METHOD("get_result"), &DebugInvokeResult::get_result);
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "result", PROPERTY_HINT_NONE, "",
						 PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY | PROPERTY_USAGE_NIL_IS_VARIANT),
			"", "get_result");
	OPENNOVA_RECORD_READ_ONLY_OBJECT(DebugInvokeResult, state, DebugControlState)
	ClassDB::bind_method(D_METHOD("to_json_value"), &DebugInvokeResult::to_json_value);
}

} // namespace godot
