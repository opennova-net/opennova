#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "devtools/debug_arg_spec.h"
#include "devtools/debug_control_records.h"
#include "devtools/debug_shell_host.h"

#include <functional>
#include <vector>

namespace godot {

class GameWorld;
class LocalPlayerPresenter;
class MissionRoot;
class Simulation;
class Terrain;
class Viewport;
class Weather;

// The typed debug-control table (ADR 0043 d12, amending ADR 0042 d5): every
// F3 / MCP debug knob as one DebugControlRow bound to its typed owner's member
// functions — Simulation, Terrain, Weather, GameWorld, LocalPlayerPresenter,
// MissionRoot, the Viewport, the AudioServer — and the shell's two verbs
// through DebugShellHost. The F3 windows and MCP's game_debug drive the SAME
// instance: a window's ControlRequest and an op=invoke both land in invoke(),
// which marshals the arguments against the row's schema, applies the two
// gates (per-call confirmation, session authority) and only then reaches the
// owner. Owners resolve live on every call through the host (a world reload
// is picked up without any retained object, and nothing replays across a
// mission: a fresh mission gets fresh debug state). Compiled in every
// flavour: only the ImGui windows are debug-only, so a release --mcp-port
// still serves the whole table.
//
// This table shares the MCP boundary's zero-witness-cite floor
// (scripts/lint/ratchet_counts.py mcp_boundary_cites): a row that needs an
// [orig] cite is re-deriving engine behavior instead of calling it.
class DebugControlTable : public RefCounted {
	GDCLASS(DebugControlTable, RefCounted)

public:
	// The audio rows' bus-volume domain, in whole decibels.
	static constexpr int AUDIO_BUS_VOLUME_MIN_DB = -60;
	static constexpr int AUDIO_BUS_VOLUME_MAX_DB = 6;
	// The teleport action's look domain: one full turn of yaw, the pitch clamp.
	static constexpr double kTeleportYawLimitDeg = 360.0;
	static constexpr double kTeleportPitchLimitDeg = 90.0;
	// A mission variable is a signed 32-bit word.
	static constexpr double kMissionVarValueMin = -2147483648.0;
	static constexpr double kMissionVarValueMax = 2147483647.0;
	// The reason rows the gates report.
	static const char *const kReasonHostOnly;
	static const char *const kReasonConfirm;

	// An action's verdict: the row's Error plus its result value.
	struct Outcome {
		Error error = OK;
		Variant result;
	};

	DebugControlTable();

	// Adopt the shell seam the rows resolve their owners through. The rows
	// register in the constructor regardless (nothing reads an owner until a
	// row is read or invoked), so a table without a host still carries the
	// catalog and its arg schemas, every row unavailable.
	void setup(const Ref<DebugShellHost> &p_host);
	Ref<DebugShellHost> get_host() const { return host_; }
	// Drop the host and every row before the shell is destroyed.
	void clear();

	Ref<DebugControlRow> control(const StringName &p_id) const;
	// The registration order IS the op=list wire order.
	TypedArray<StringName> row_ids() const;
	// The definitions, each paired with THIS caller's live state.
	// `p_allow_authority` mirrors the write path's per-call confirmation: rows
	// report writability for that caller, so an MCP client holding
	// confirm_authority is not told its own successful writes are locked.
	TypedArray<DebugControlRow> list_controls(const StringName &p_page = StringName(),
			const String &p_filter = String(), bool p_allow_authority = false);
	Ref<DebugControlState> get_state(const StringName &p_id, bool p_allow_authority = false);
	Error set_value(const StringName &p_id, const Variant &p_value, bool p_allow_authority = false);
	// Run one action over its typed positional arguments (marshalled here
	// against the row's schema; the refusal is ERR_INVALID_PARAMETER). A
	// non-action row takes its value as the single argument and routes
	// through set_value.
	Ref<DebugInvokeResult> invoke(const StringName &p_id, const Array &p_args = Array(),
			bool p_allow_authority = false);
	// The one argument marshaller for op=invoke: null, a positional Array, a
	// by-name Dictionary or one scalar becomes the typed positional Array the
	// row's owner call reads (a non-action row's value passes through as the
	// single argument); the refusal names the argument.
	Ref<DebugMarshalResult> marshal_invoke_args(const StringName &p_id, const Variant &p_raw) const;
	// The JSON-facing snapshot of the whole table for one caller, at this
	// boundary only: {"edit_unlocked": false, "controls": [row + state...]}.
	// "edit_unlocked" is wire-stable: the F3 Live-edits latch died with the
	// DebugSession family, so it reports false forever. The transport adds
	// the shell's runtime block.
	Variant capture_snapshot(const String &p_filter = String(), bool p_allow_authority = false);
	// Kind-typed value normalization (the write path's argument check): CHECK
	// takes exactly a bool, SLIDER a finite number clamped and snapped to the
	// row's domain, ENUM a valid choice index. NIL = refused.
	static Variant normalize_value(const Ref<DebugControlRow> &p_row, const Variant &p_value);

protected:
	static void _bind_methods();

private:
	using Kind = DebugControlRow::Kind;
	using Target = DebugControlRow::Target;
	using Owner = DebugControlRow::Owner;

	// One registered row and its owner bindings. The closures capture the
	// member-function pointers of the typed owner; the owner instance itself
	// is resolved inside the closure on every call.
	struct Entry {
		Ref<DebugControlRow> row;
		// The live value; NIL while the owner is away.
		std::function<Variant()> read;
		// Apply one normalized value.
		std::function<Error(const Variant &)> write;
		// Run the action over the marshalled positional arguments.
		std::function<Outcome(const Array &)> invoke;
	};

	std::vector<Entry> entries_;
	Ref<DebugShellHost> host_;

	const Entry *find(const StringName &p_id) const;
	Entry *find(const StringName &p_id);
	Ref<DebugControlState> state_of(const Entry &p_entry, bool p_allow_authority);
	Ref<DebugInvokeResult> invoke_result(Error p_error, const Variant &p_result,
			const StringName &p_id, bool p_allow_authority);

	// --- live owner resolution (per call, never retained) --------------------
	// The world rows draw over a LOADED mission: the shell's GameWorld node
	// outlives the mission, so an unloaded one reads as no world.
	GameWorld *world() const;
	MissionRoot *runtime() const;
	Simulation *sim() const;
	LocalPlayerPresenter *player() const;
	Terrain *terrain() const;
	Weather *weather() const;
	Viewport *viewport() const;
	// "" while the target's owner resolves, else the reason row.
	String availability(Target p_target) const;
	bool has_host_authority() const;
	bool write_allowed(const Entry &p_entry, bool p_allow_authority) const;
	String policy_reason(const Entry &p_entry, bool p_allow_authority) const;

	// --- row registration ------------------------------------------------------
	Entry &add(Kind p_kind, const char *p_id, const char *p_page, const String &p_label,
			const String &p_tooltip, Target p_target, Owner p_owner);
	Entry &check(const char *p_id, const char *p_page, const String &p_label,
			const String &p_tooltip, Target p_target, Owner p_owner);
	Entry &slider(const char *p_id, const char *p_page, const String &p_label,
			const String &p_tooltip, Target p_target, Owner p_owner, double p_default,
			double p_minimum, double p_maximum, double p_step);
	Entry &enum_row(const char *p_id, const char *p_page, const String &p_label,
			const String &p_tooltip, Target p_target, Owner p_owner, int p_default,
			const PackedStringArray &p_choices);
	Entry &action(const char *p_id, const char *p_page, const String &p_label,
			const String &p_tooltip, Target p_target, Owner p_owner,
			const TypedArray<DebugArgSpec> &p_args = TypedArray<DebugArgSpec>());
	static void authoritative(Entry &p_entry);

	// Bind a value row to its owner's getter / setter member pointers; the
	// owner is resolved per call through `p_resolve`.
	template <typename OwnerT, typename Get, typename Set>
	void bind_value(Entry &p_entry, OwnerT *(DebugControlTable::*p_resolve)() const, Get p_get,
			Set p_set);
	// Bind an action row to one owner member: the marshalled positional
	// arguments unpack into the member's parameters; a void member answers a
	// null result, an Error member its verdict, any other return the result.
	template <typename OwnerT, typename R, typename... P>
	void bind_action(Entry &p_entry, OwnerT *(DebugControlTable::*p_resolve)() const,
			R (OwnerT::*p_fn)(P...));

	void register_option_rows();
	void register_terrain_rows();
	void register_rendering_rows();
	void register_edit_actions();
	void register_audio_actions();
	void register_runtime_rows();
	void register_environment_rows();
	void register_automation_actions();
	void register_spectator_row();
};

} // namespace godot
