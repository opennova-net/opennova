#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "devtools/debug_arg_spec.h"

namespace godot {

class DebugControlState;

// One typed control row of the debug-control table (ADR 0043 d12): the wire
// definition `game_debug op=list` publishes (the thirteen legacy keys of
// to_json_value; "description" carries the tooltip, "requires_unlock" the
// confirm gate) plus the action's argument schema. The table fills the row
// and owns its owner bindings; a row listed for a caller carries that
// caller's live `state` (null on the catalog row itself).
//
// `owner` classifies the row: an ENGINE row ends in a Simulation / Terrain /
// Weather / environment engine call; a DEVICE row mutates what exists only
// because a Godot viewport, node or audio bus exists. `target` is the
// wire-stable owner-surface name (the legacy "target" row key).
class DebugControlRow : public RefCounted {
	GDCLASS(DebugControlRow, RefCounted)

public:
	enum Kind {
		CHECK,
		SLIDER,
		ENUM,
		ACTION,
	};

	enum Authority {
		ANY,
		HOST_ONLY,
	};

	enum Owner {
		OWNER_ENGINE,
		OWNER_DEVICE,
	};

	enum Target {
		TARGET_WORLD,
		TARGET_PLAYER,
		TARGET_SIM,
		TARGET_TERRAIN,
		TARGET_VIEWPORT,
		TARGET_GAME_SHELL,
		TARGET_ENVIRONMENT,
		TARGET_WEATHER,
	};

	static String kind_name_of(Kind p_kind);
	static String target_name_of(Target p_target);
	static String owner_name_of(Owner p_owner);

private:
	friend class DebugControlTable;

	StringName id_;
	StringName page_;
	String label_;
	String tooltip_;
	Kind kind_ = CHECK;
	Target target_ = TARGET_WORLD;
	Owner owner_ = OWNER_DEVICE;
	Variant default_value_;
	double minimum_ = 0.0;
	double maximum_ = 1.0;
	double step_ = 0.1;
	PackedStringArray choices_;
	// Mutation needs the caller's per-call confirm_authority (the wire key
	// stays "requires_unlock"; the F3 Live-edits latch died with the
	// DebugSession family).
	bool requires_confirm_ = false;
	Authority authority_ = ANY;
	// ACTION rows: the positional argument schema DebugArgSpec::marshal
	// validates against (the owner call reads the typed values).
	TypedArray<DebugArgSpec> args_;
	Ref<DebugControlState> state_;

protected:
	static void _bind_methods();

public:
	StringName get_id() const { return id_; }
	StringName get_page() const { return page_; }
	String get_label() const { return label_; }
	String get_tooltip() const { return tooltip_; }
	Kind get_kind() const { return kind_; }
	Target get_target() const { return target_; }
	Owner get_owner() const { return owner_; }
	Variant get_default_value() const { return default_value_; }
	double get_minimum() const { return minimum_; }
	double get_maximum() const { return maximum_; }
	double get_step() const { return step_; }
	PackedStringArray get_choices() const { return choices_; }
	bool get_requires_confirm() const { return requires_confirm_; }
	Authority get_authority() const { return authority_; }
	TypedArray<DebugArgSpec> get_args() const { return args_; }
	Ref<DebugControlState> get_state() const { return state_; }

	String kind_name() const { return kind_name_of(kind_); }
	// The MCP boundary conversion: the legacy wire row keys, plus "state"
	// when this row was listed for a caller.
	Dictionary to_json_value() const;
	// A copy of the definition carrying one caller's live state.
	Ref<DebugControlRow> with_state(const Ref<DebugControlState> &p_state) const;
};

// One authoritative observation of a DebugControlRow for one caller: the
// live value (null while the owner is away), whether the row is available
// and writable for THAT caller, and the reason row when it is not.
class DebugControlState : public RefCounted {
	GDCLASS(DebugControlState, RefCounted)

	friend class DebugControlTable;

	StringName id_;
	DebugControlRow::Kind kind_ = DebugControlRow::CHECK;
	Variant value_;
	Variant desired_value_;
	bool available_ = false;
	bool writable_ = false;
	bool authoritative_ = false;
	String reason_;

protected:
	static void _bind_methods();

public:
	StringName get_id() const { return id_; }
	DebugControlRow::Kind get_kind() const { return kind_; }
	Variant get_value() const { return value_; }
	Variant get_desired_value() const { return desired_value_; }
	bool is_available() const { return available_; }
	bool is_writable() const { return writable_; }
	bool is_authoritative() const { return authoritative_; }
	String get_reason() const { return reason_; }

	// The MCP boundary conversion (the eight wire keys).
	Dictionary to_json_value() const;
	// The JSON-safe form of any value the table hands the wire: vectors and
	// colors as {x, y, z} / {r, g, b, a} objects, StringNames as text,
	// collections recursively, packed arrays as arrays, objects as null.
	static Variant json_value(const Variant &p_value);
};

// What DebugControlTable::invoke answers: the row's verdict (an Error), the
// action's result value and the row's state for the caller afterwards.
class DebugInvokeResult : public RefCounted {
	GDCLASS(DebugInvokeResult, RefCounted)

	friend class DebugControlTable;

	Error error_ = OK;
	Variant result_;
	Ref<DebugControlState> state_;

protected:
	static void _bind_methods();

public:
	Error get_error() const { return error_; }
	Variant get_result() const { return result_; }
	Ref<DebugControlState> get_state() const { return state_; }

	// The MCP boundary conversion: {error, result, state}.
	Dictionary to_json_value() const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::DebugControlRow::Kind);
VARIANT_ENUM_CAST(godot::DebugControlRow::Authority);
VARIANT_ENUM_CAST(godot::DebugControlRow::Owner);
VARIANT_ENUM_CAST(godot::DebugControlRow::Target);
