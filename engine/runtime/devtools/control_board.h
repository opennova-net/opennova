// The control board (ADR 0043 d12): the read side of the ONE debug-control
// table F3 and MCP share. ControlRequest is how a window writes a row; the
// board is how it reads one — the row's definition (label, tooltip, kind,
// range, enum choices) from the table's catalog, pushed once, and its live
// state (value, writable, the reason when not) for the rows the visible
// windows declare, pushed on a short cadence. A window draws a row through
// draw_control and never hand-copies a row's range, choices or refusal text,
// so F3 shows exactly what MCP's `game_debug op=list` reports, joiner
// authority gate included.
//
// Plain values only: the embedder converts the table's rows and states; the
// board never reaches into Godot or a live World.
#pragma once

#include <runtime/devtools/control_request.h>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::devtools {

// DebugControlRow::Kind, in its order (the embedder static_asserts it).
enum class ControlKind : uint8_t {
	Check,
	Slider,
	Enum,
	Action,
};

// One row's definition, as the table's catalog lists it.
struct ControlSpec {
	std::string id;
	std::string label;
	std::string tooltip;
	ControlKind kind = ControlKind::Check;
	double minimum = 0.0;
	double maximum = 1.0;
	double step = 0.0;
	std::vector<std::string> choices; // Enum rows: the value is an index
};

// One row's live state for the F3 caller. `value` is Bool for a Check,
// Float for a Slider, Int (the choice index) for an Enum; `has_value` is
// false while the row's owner is away (between missions).
struct ControlState {
	std::string id;
	bool writable = false;
	bool has_value = false;
	ControlArg value;
	std::string reason;
};

class ControlBoard {
public:
	void set_catalog(std::vector<ControlSpec> catalog);
	bool has_catalog() const { return !catalog_.empty(); }
	int catalog_size() const { return static_cast<int>(catalog_.size()); }
	const ControlSpec *spec(const char *id) const;

	// A push replaces the states of the rows it names and keeps the rest (a
	// row no window shows any more keeps its last reading until it is
	// wanted again).
	void set_states(const std::vector<ControlState> &states);
	void clear_states() { states_.clear(); }
	const ControlState *state(const char *id) const;
	// A widget's optimistic write: the value the click sent, shown until the
	// next push confirms (or corrects) it.
	void set_local_value(const char *id, const ControlArg &value);

	// A Slider's drag buffer: the widget edits this while the item is
	// active and queues the value on release, so a push mid-drag never
	// yanks the handle.
	float &slider_edit(const char *id) { return slider_edits_[id]; }

private:
	std::vector<ControlSpec> catalog_;
	std::unordered_map<std::string, size_t> index_;
	std::unordered_map<std::string, ControlState> states_;
	std::unordered_map<std::string, float> slider_edits_;
};

// Draw one row as its kind's widget (a checkbox, a slider queued on release,
// an enum combo, an action button) and queue a ControlRequest on an edit.
// An unknown row draws its id greyed; a row that is not writable draws
// disabled with the table's reason as its tooltip. `label` overrides the
// row's own label. Returns true when a request was queued.
bool draw_control(ControlBoard &board, const char *id, std::deque<ControlRequest> &queue,
		const char *label = nullptr);

}  // namespace opennova::devtools
