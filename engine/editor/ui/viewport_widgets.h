#pragma once

#include <cstdint>
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
struct SessionView;

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
// The ground under the pointer as a canvas's one line (DI-07; the mission's and the terrain's): the class's legend
// colour (`surface`, -1 none) then "Ground: <ground>", or with nothing under the pointer "Ground: point at the
// terrain." and what the game reads there in its tooltip.
void ground_line(const std::string &ground, int surface);
// The files a picture lacks or could not read as one amber line, what they are after their count ("2 files missing:
// logo.tga, gunpl22b.fnt"), the first three named: every one, then `then` (what brings them), in its tooltip. Nothing
// while none is.
void files_line(const std::vector<std::string> &names, const char *what, const char *then);
// What a picture says of itself (a viewport's notes, its `notes_json`: no mission, a file the project lacks), a
// line each, cut, whole in its tooltip; a note of the code `skip` left to a line of its own (the missing files').
void picture_notes(const io::JsonValue &notes, const char *skip = nullptr);

// A line of a toolbar's popup (Sound, Fire, First person, Damage, Heard; the kit's prose rule): `text` cut to what is
// left of the popup's line, or to `ems` font sizes from its left edge where that is wider (a popup sizes to its widest
// item, so a line cut to the room left alone would cut to its controls'). Hovered, `tip` when one is given, else the
// whole text when it was cut; muted for Quiet, amber for Warning.
enum class LineTone { Plain, Quiet, Warning };
void popup_line(const std::string &text, const std::string &tip = std::string(), LineTone tone = LineTone::Plain,
		float ems = 26.0f);

// Listen on `row` (the mission's DI-36, the environment's S23 C), lit while `on`: its tooltip what it hears where the
// camera stands (`hears`, "the mission"), as the game plays it, then `detail`. True when pressed.
bool listen_tool(ui_kit::WrapRow &row, bool on, const char *hears, const char *detail);
// The Terrain pane's button on `row` (S25; a mission's 3D view, a terrain's): lit while the pane shows beside `path`,
// named after the brush's tool while it brushes; pressed, the pane shown or hidden (Ctrl+E).
void terrain_pane_tool(Workspace &workspace, ui_kit::WrapRow &row, const std::string &path);
// What Play from here does (DI-26), the player on the ground `where` ("under the camera", "here"), `key` its key
// (none for ""): the tooltip of the 3D view's tool, of the picture's menu and of the Build menu's item.
std::string play_from_here_words(const char *where, const char *key = "");

// The items that draw a model, a Selectable each by its name and its id (Place in mission's choice, a model let go
// over a mission's picture; DI-12): the one picked (-1: none).
int64_t item_choice(const SessionView &view, const std::vector<int64_t> &items);

} // namespace opennova::editor
