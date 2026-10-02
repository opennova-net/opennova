#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/node.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_model.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_screen_inputs.h>

namespace opennova::editor {

class MenuViewport;
class MnuDocument;
struct MenuCanvasFrame;

// How a menu viewport holds its screen (ADR 0046 S9j, S13 V5): every window shown, and one window
// of the screen held the way the game's pump and the author's clicks would leave it (a picture never
// pumps): under the mouse or pressed, disabled, checked, its list open, focused (its caret blinking
// on the preview clock, S13 V8). The size the device draws at is the viewport's state
// (ViewportState).
struct MenuViewportOptions {
	bool show_hidden = false;
	NodeId force_window = 0; // a window record of the shown screen (0 = none)
	int force_state = -1; // menu::kStateMouseover / kStateSelected / kStateDisabled (-1 = none)
	bool checked = false; // the forced window checked (a check box, a radio button)
	bool popup_open = false; // its list open (a combo box)
	bool focused = false; // it has the keyboard focus, its caret blinking on the clock (an edit)
	bool operator==(const MenuViewportOptions &other) const {
		return show_hidden == other.show_hidden && force_window == other.force_window &&
				force_state == other.force_state && checked == other.checked &&
				popup_open == other.popup_open && focused == other.focused;
	}
	bool operator!=(const MenuViewportOptions &other) const { return !(*this == other); }
	// True when the forced window is held some way.
	bool forcing() const { return force_state >= 0 || checked || popup_open || focused; }
};
// The options on the wire (the envelope's `options`, a SetViewport's): {show_hidden, force_id,
// force_state, checked, popup_open, focus}.
io::JsonValue menu_options_to_json(const MenuViewportOptions &options);

// "normal", "mouseover", "selected", "disabled": a forced state's token (-1 = normal), and back;
// false for another token.
const char *menu_force_state_token(int state);
bool menu_force_state_from_token(const std::string &token, int &out);

// The options on a configured screen's frame state (the viewport's headless compile's, and the
// device's after each configure): every window shown, and the window at `forced_index` (-1 none)
// held as the options say.
void apply_menu_options(const MenuViewportOptions &options, int forced_index,
		const menu::MenuFrameCompiler &compiler, menu::MenuFrameState &state);

// A menu's clock on the preview clock (ADR 0046 S13 V8; CONTEXT.md "Preview clock"): the frame's
// clock (MenuFrameState::time_ms) is the preview clock's milliseconds, where the game reads
// GetTickCount. What reads it in a preview is a focused edit box's caret, which the game draws while
// (GetTickCount() & 0x3FF) > 0x200 [orig: CEditWnd_Render @ 0x661c63] (menu_caret_shown, the one gate
// the edit and the multiline edit draw by; docs/mnu/menu-re.md, the edit's caret and the multiline
// edit's): it blinks as the clock plays, stands while it is paused and seeks with it.
uint32_t menu_frame_time(const PreviewClock &clock);
// Whether a device's frame drawn at `frame_ms` (its MenuFrameState::time_ms) draws otherwise at the
// preview clock's time, and that time (S13 V8): only while a focused edit box of the shown screen
// reads the clock (the options' focus on the window they hold, an edit box) and the caret's half of
// the blink at the one differs from the other's. So the device draws its frame again twice a blink,
// never once a frame as the clock plays, and never with no focused edit box; a frame configured anew
// (its time the configure's) takes the clock's time at its first tick that would show otherwise.
bool menu_frame_clock(const MenuViewport &menu, uint32_t frame_ms, const PreviewClock &clock,
		uint32_t &time);

// A window's TYPE as the factory matches it (the generic window for none), and what the options
// can hold it in by its type: checked (a check box, a radio button), its list open (a combo box),
// focused (an edit box).
mnu::WindowType menu_window_type(const MnuDocument &document, const NodeAddress &window);
bool menu_type_checkable(mnu::WindowType type);
bool menu_type_has_list(mnu::WindowType type);
bool menu_type_editable(mnu::WindowType type);

// A menu's viewport (ADR 0046 S6c, S9j, S13 V5; ViewportKind::Menu): one screen of the menu at its
// path as the game would draw the menu were it saved now (MnuDocument::saved_image, the screen by
// its row's position), with the Shell's %VAR% list read through the project's files (the open
// documents standing in). The screen is the Preview window's target's while it is this menu, else
// the one it showed. It compiles the screen headless itself (MenuScreenRender, textures measured by
// their headers), its options held on the compile's frame state: the geometry its hit tests, its
// handles and its drags read, and its compiler's notes, made once per configure; the Shell's device
// configures the same image with the runtime's MenuFrame and Godot's decoders, and reports where it
// placed each widget. It configures again only when what feeds its screen moved (S13 V8): its screen
// or its options; of the menu's change set, the screen's own row (its fields, a window of it and what
// they hold) or a screen before it in the file (the first load of a texture, which any screen may
// make, fixes the band every later user of it draws: menu-re.md "The IMAGE pass"), the screens
// reordered or one removed, the file-wide state; a file the compile read (the string tables, the
// fonts, the textures) moving its stamp; or a stylesheet's moving where a variable the text of the
// screens up to it names came, went or took another value. An edit of a screen after it, or a
// stylesheet variable only later screens name, leaves the picture as it is (Keep). A menu the game
// could not read keeps its reason until it changes (no retry every frame). Its held window follows
// the selection: a window of the screen newly selected is the one held, what its type cannot hold let
// go.
class MenuViewport final : public ViewportModel {
public:
	explicit MenuViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	MenuScreenStatus screen_status() const { return reason_; }
	const MenuViewportOptions &options() const { return options_; }
	// The screen row it shows (0 none).
	NodeId screen_row() const { return part_; }
	// What the device configures (null unless ready): the menu image, which the device holds while
	// its frame borrows the screen (the viewport's next configure lets go of its own), and its screen;
	// the Shell's %VAR% list; the pre-order index of the window the options hold (-1 none).
	const std::shared_ptr<const mnu::Document> &image() const { return render_.image(); }
	const mnu::Screen *screen() const { return render_.screen(); }
	const std::map<std::string, std::string> &style_vars() const { return style_vars_; }
	int forced_index() const { return forced_index_; }
	// Its headless compile (the game's rects and hit test, its options held), and the compiler's
	// notes on the screen, made once per configure.
	const MenuScreenRender &render() const { return render_; }
	const std::vector<menu::MenuFrameNote> &notes() const { return notes_; }
	// The files the screen names that the project lacks, and those it has that did not load (a
	// font or string table that does not parse, a texture that does not decode), each once.
	const std::vector<std::string> &missing() const { return missing_; }
	const std::vector<std::string> &unreadable() const { return unreadable_; }
	// Where the device placed each widget at the document revision it last configured (its report:
	// empty before one).
	const std::vector<ViewportDeviceReport::Rect> &device_rects() const { return device_rects_; }
	// How many times it compiled its screen.
	uint64_t configures() const { return configures_; }
	// What a canvas maps of it in a frame (menu_canvas.h): the menu, its screen, its compile, its
	// notes and the session's selection on the screen while the menu is the active document.
	MenuCanvasFrame canvas_frame(const ViewportContext &context) const;

	ViewportStatus status() const override;
	const char *reason() const override { return menu_screen_status_token(reason_); }
	std::string message() const override { return menu_screen_status_message(reason_, detail_); }
	const std::string &detail() const override { return detail_; }
	std::string caption() const override;
	const char *units() const override { return "design"; }
	ViewportLayout layout() const override;
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	bool command(const ViewportContext &context, const std::string &name,
			const std::vector<NodeId> &ids, CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue body_json(const ViewportInput &input) const override;
	io::JsonValue items_json(const ViewportInput &input) const override;
	io::JsonValue notes_json(const ViewportInput &input) const override;
	// A screen of the menu by its row, as the render check compiled it (menu_render_to_json).
	io::JsonValue render_json(
			const ViewportInput &input, NodeId row, const JsonPage &page, std::string &error) const override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	void report_(const ViewportDeviceReport &report) override;

private:
	ViewportAction stop_(MenuScreenStatus reason, const std::string &detail);
	// The held window follows the selection (the primary record's window on the shown screen).
	void follow_selection_(const ViewportInput &input, const MnuDocument &document);
	// Whether what changed in the document since the last follow reaches the picture of the screen
	// `row` (S13 V8): everything the document cannot say, any change while the picture failed (it is
	// tried again once the document changes), and of a change set what feeds the screen (the screen,
	// the screens before it, their order) or leaves the menu with no image the game would read (none
	// of its screens).
	bool moves_(const ViewportInput &input, const MnuDocument &document, NodeId row) const;
	// Whether what moved of the files the picture read is the shell's stylesheets alone, with no
	// variable the screens up to the shown one name come, gone or of another value (S13 V8): the
	// picture stands, the variables read again kept for the device's next configure.
	bool styles_alone_(const FileSource &files);
	// The picture stands for the document as it is now (Keep).
	ViewportAction kept_(const MnuDocument &document);

	MenuViewportOptions options_;
	uint64_t options_serial_ = 0;
	MenuScreenStatus reason_ = MenuScreenStatus::NoProject;
	std::string detail_;
	NodeId part_ = 0;
	std::string screen_name_; // the shown screen's name (the caption)
	NodeId followed_window_ = 0; // the selected window the held state last followed
	PreviewFollow picture_;
	MenuScreenRender render_;
	menu::MenuStyleSource style_;
	std::map<std::string, std::string> style_vars_;
	// The variables the text of the screens up to the shown one names (menu_variables_named of them as
	// the menu's writer writes them: a screen before it makes first loads its textures take), sorted:
	// made when a stylesheet first moves after a configure.
	std::vector<std::string> screen_variables_;
	bool screen_variables_made_ = false;
	std::vector<menu::MenuFrameNote> notes_;
	std::vector<std::string> missing_;
	std::vector<std::string> unreadable_;
	int forced_index_ = -1;
	std::vector<ViewportDeviceReport::Rect> device_rects_;
	uint64_t configures_ = 0;
};

} // namespace opennova::editor
