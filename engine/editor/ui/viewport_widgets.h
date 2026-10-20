#pragma once

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <base/io/json.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

class PreviewClock;
class ViewportModel;
class Workspace;

// The pieces the viewports' toolbars share (ADR 0046 S13 V5: each kind's view, ui/viewport_views).

// A combo as wide as its widest choice, its arrow and its padding: none is cut (every window's combos too: the menu
// view's, the map view's, Problems', the mission card's).
float combo_width(float widest_text);
// A SetViewport of one member of `viewport` (`options`, `camera`, `clock`), its kind's change (viewport_change).
void set_viewport_member(Workspace &workspace, const ViewportModel &viewport, const char *member, io::JsonValue value);
// A SetViewport of the preview clock's `member` (`playing`, `ticks`, `rate`).
void set_clock(Workspace &workspace, const ViewportModel &viewport, const char *member, io::JsonValue value);
// The preview clock's buttons on a viewport's toolbar: Run or Pause (Play is the game's), `run_tip` what the clock
// runs; and, with a `replay_tip`, Replay: the clock at tick 0, run where `replay_runs`.
void clock_buttons(Workspace &workspace, ui_kit::WrapRow &row, const ViewportModel &viewport, const PreviewClock &clock,
		const char *run_tip, const char *replay_tip = nullptr, bool replay_runs = false);
// The preview clock at tick 0, run where `runs` (Replay's; a death or a clip played from its start).
void replay_clock(Workspace &workspace, const ViewportModel &viewport, bool runs);
// A tool button lit while `active` (a toggle's: Listen, a mission tool); true when pressed.
bool lit_tool(ui_kit::WrapRow &row, const char *label, bool active, bool enabled, const std::string &tip);
// A picture's Show on `row` and its popup of the layers it draws, a check box each over its value: true when one
// changed (its value written), the caller then setting them.
bool show_layers(ui_kit::WrapRow &row, const char *tip, std::initializer_list<std::pair<const char *, bool *>> layers);
// The ground under the pointer as a canvas's one line (DI-07): the class's legend colour (`surface`, -1 none) then
// "Ground: <ground>", or with nothing under the pointer "Ground: point at the terrain." and what it reads
// (`reads`) in its tooltip.
void ground_line(const std::string &ground, int surface, const char *reads);
// The files a picture lacks as one amber line, the first three named: every one, and that Problems imports them,
// in its tooltip. Nothing while none is.
void missing_files_line(const std::vector<std::string> &missing);

} // namespace opennova::editor
