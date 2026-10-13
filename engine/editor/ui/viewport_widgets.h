#pragma once

#include <string>

#include <base/io/json.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

class PreviewClock;
class ViewportModel;
class Workspace;

// The pieces the viewports' toolbars share (ADR 0046 S13 V5: each kind's view, ui/viewport_views).

// A combo as wide as its widest choice, its arrow and its padding: none is cut.
float combo_width(float widest_text);
// A SetViewport of one member of `viewport` (`options`, `camera`, `clock`), its kind's change (viewport_change).
void set_viewport_member(Workspace &workspace, const ViewportModel &viewport, const char *member, io::JsonValue value);
// A SetViewport of the preview clock's `member` (`playing`, `ticks`, `rate`).
void set_clock(Workspace &workspace, const ViewportModel &viewport, const char *member, io::JsonValue value);
// The preview clock's buttons on a viewport's toolbar: Run or Pause (Play is the game's), `run_tip` what the clock
// runs; and, with a `replay_tip`, Replay: the clock at tick 0, run where `replay_runs`.
void clock_buttons(Workspace &workspace, ui_kit::WrapRow &row, const ViewportModel &viewport, const PreviewClock &clock,
		const char *run_tip, const char *replay_tip = nullptr, bool replay_runs = false);
// A tool button lit while `active` (a toggle's: Listen, a mission tool); true when pressed.
bool lit_tool(ui_kit::WrapRow &row, const char *label, bool active, bool enabled, const std::string &tip);

} // namespace opennova::editor
