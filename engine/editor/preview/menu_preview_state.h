#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/node.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_screen_inputs.h>

namespace opennova::menu {
class MenuFrameAssets;
}

namespace opennova::editor {

class MnuDocument;
struct SessionView;

// What the menu preview can show, and why not (ADR 0046 S9j).
enum class MenuPreviewStatus : uint8_t {
	NoProject,      // no project is open
	NoMenu,         // no menu document holds the preview target
	NoScreen,       // no screen of it is selected
	NoDevice,       // no renderer is attached (an engine-only run)
	Unserializable, // the menu cannot be written as it stands, so the game could not read it
	ScreenMissing,  // the screen is not in the menu the game would read
	Ready,
};
// "no_project", "ready", ...: the token the preview JSON carries.
const char *menu_preview_status_token(MenuPreviewStatus status);
// The line the preview shows for a status (`detail`: the first serialize issue, the
// screen's name).
std::string menu_preview_status_message(MenuPreviewStatus status, const std::string &detail);

// How the preview draws the screen: the device size, every window shown, and one window
// of the screen held the way the game's pump and the author's clicks would leave it (a
// preview never pumps): under the mouse or pressed, disabled, checked, its list open,
// focused with its caret showing.
struct MenuPreviewOptions {
	int width = menu::kMenuDesignWidth;
	int height = menu::kMenuDesignHeight;
	bool show_hidden = false;
	NodeId force_window = 0; // a window record of the previewed screen (0 = none)
	int force_state = -1;    // menu::kStateMouseover / kStateSelected / kStateDisabled (-1 = none)
	bool checked = false;    // the forced window checked (a check box, a radio button)
	bool popup_open = false; // its list open (a combo box)
	bool focused = false;    // it has the keyboard focus, the caret in its shown phase (an edit)
	bool operator==(const MenuPreviewOptions &other) const {
		return width == other.width && height == other.height && show_hidden == other.show_hidden &&
		       force_window == other.force_window && force_state == other.force_state &&
		       checked == other.checked && popup_open == other.popup_open && focused == other.focused;
	}
	bool operator!=(const MenuPreviewOptions &other) const { return !(*this == other); }
	// True when the forced window is held some way.
	bool forcing() const { return force_state >= 0 || checked || popup_open || focused; }
};
// "normal", "mouseover", "selected", "disabled": a forced state's token (-1 = normal), and
// back; false for another token.
const char *menu_preview_state_token(int state);
bool menu_preview_state_from_token(const std::string &token, int &out);

// The options on a configured screen's frame state, after each configure (the device's
// fresh state): every window shown, and the window at `forced_index` (-1 none) held as
// the options say.
void apply_menu_preview_options(const MenuPreviewOptions &options, int forced_index, const menu::MenuFrameCompiler &compiler,
                                menu::MenuFrameState &state);

// The names a configure could not load, split into the files the source lacks and the
// files it has that did not load (a font or string table that does not parse, a texture
// that does not decode), each once.
void split_unloaded(const menu::MenuFrameAssets &assets, std::vector<std::string> &missing,
                    std::vector<std::string> &unreadable);

// What the preview's device does after a follow().
enum class MenuPreviewAction : uint8_t {
	Keep,      // nothing changed that it draws
	Configure, // configure screen() again, then report configured()
	Clear,     // drop what it configured (the status says why there is nothing)
};

// The preview's portable half, the device's (the shell's MenuFrame) guide: which screen
// the view previews (SessionView::preview), the menu the game would read were it saved
// now (MnuDocument::saved_image, the screen by its row's position), the shell's %VAR% list
// read through the project's files (the open documents standing in), and when to
// configure again: the menu's identity, revision or screen row moved, the options moved,
// or a file the last configure read (the stylesheets, the string tables, the fonts, the
// textures) moved its stamp. A menu that cannot be read keeps its status until it
// changes (no retry every frame).
class MenuPreviewModel {
public:
	MenuPreviewAction follow(const SessionView &view);
	// The device configured screen() through `assets`: the files it read or looked for
	// (besides the stylesheets this model read), and the names that did not load, split
	// into the files the project lacks and the files it has that did not load.
	void configured(const menu::MenuFrameAssets &assets);
	// Options apply on the next follow (a configure).
	void set_options(const MenuPreviewOptions &options);
	const MenuPreviewOptions &options() const { return options_; }

	MenuPreviewStatus status() const { return status_; }
	const std::string &detail() const { return detail_; }
	// The document and screen the device configures (valid until the next follow).
	const mnu::Document *image() const { return image_.get(); }
	const mnu::Screen *screen() const { return screen_; }
	const std::map<std::string, std::string> &style_vars() const { return style_vars_; }
	// The files the last configure could not find in the project, each once (the
	// preview's banner).
	const std::vector<std::string> &missing() const { return missing_; }
	// The files the last configure found but could not load (a font or string table that
	// does not parse, a texture that does not decode or of a kind the game does not
	// load), each once.
	const std::vector<std::string> &unreadable() const { return unreadable_; }
	// The document state the device shows: its path and revision, and the screen row.
	const std::string &shown_path() const { return shown_path_; }
	uint64_t shown_revision() const { return shown_.revision; }
	NodeId shown_row() const { return shown_.row; }
	// The pre-order index of the window the options hold in a state (-1 none).
	int forced_index() const { return forced_index_; }

private:
	struct Key {
		uint64_t identity = 0, revision = 0, options = 0;
		NodeId row = 0;
		bool operator==(const Key &o) const {
			return identity == o.identity && revision == o.revision && options == o.options && row == o.row;
		}
	};
	MenuPreviewAction stop_(MenuPreviewStatus status, const std::string &detail);
	bool dependencies_moved_(const FileSource &files) const;

	MenuPreviewOptions options_;
	uint64_t options_serial_ = 0;
	MenuPreviewStatus status_ = MenuPreviewStatus::NoProject;
	std::string detail_;
	bool have_shown_ = false, failed_ = false, device_configured_ = false;
	Key shown_;
	std::string shown_path_;
	uint64_t generation_ = 0;
	std::shared_ptr<const mnu::Document> image_, retired_;
	const mnu::Screen *screen_ = nullptr;
	menu::MenuStyleSource style_;
	std::map<std::string, std::string> style_vars_;
	std::vector<menu::MenuDependency> dependencies_;
	std::vector<std::string> missing_;
	std::vector<std::string> unreadable_;
	int forced_index_ = -1;
};

} // namespace opennova::editor
