#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/model/node.h>
#include <editor/preview/texture_header.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_frame_assets.h>

namespace opennova::editor {

class MnuDocument;

// What a menu screen's render shows, and why not (ADR 0046 S9j, S13 V5): the menu viewport's
// reason and the render check's status of a screen.
enum class MenuScreenStatus : uint8_t {
	NoProject, // no project is open
	NoMenu, // no menu is open to show
	NoScreen, // a menu is open, none of its screens selected
	Unserializable, // the menu cannot be written as it stands, so the game could not read it
	ScreenMissing, // the screen is not in the menu the game would read
	Ready,
};
// "no_project", "ready", ...: its token on the wire.
const char *menu_screen_status_token(MenuScreenStatus status);
// The line a menu viewport shows for a status (`detail`: the first serialize issue, the screen's
// name); "" when ready.
std::string menu_screen_status_message(MenuScreenStatus status, const std::string &detail);

// The names a configure could not load, split into the files the source lacks and the files it
// has that did not load (a font or string table that does not parse, a texture that does not
// decode), each once.
void split_unloaded(const menu::MenuFrameAssets &assets, std::vector<std::string> &missing,
		std::vector<std::string> &unreadable);

// One screen of a menu compiled headless the way the game's frame compiles it (ADR 0046
// S9j2): the menu the game would read were it saved now (MnuDocument::saved_image, the
// screen by its row's position), the shell's %VAR% list, and the screen's string tables,
// fonts and textures through a file source (the project's, the open documents standing
// in), textures measured by their headers (TextureHeaderProbe). The render check keeps
// one per screen; the menu viewport keeps its own (S13 V5: the geometry its hit tests, handles
// and drags read, with its options' frame state); the queries and the tests read its compiler and
// its notes.
class MenuScreenRender {
public:
	MenuScreenRender();
	~MenuScreenRender();
	MenuScreenRender(const MenuScreenRender &) = delete;
	MenuScreenRender &operator=(const MenuScreenRender &) = delete;

	// Compile the screen `screen_row` of `document`. Ready, or why not: Unserializable (the
	// detail is the first reason) or ScreenMissing (the screen's name).
	MenuScreenStatus configure(const MnuDocument &document, NodeId screen_row, const FileSource &files,
	                           const std::map<std::string, std::string> &vars);
	MenuScreenStatus status() const { return status_; }
	const std::string &detail() const { return detail_; }
	// The document revision the render read.
	uint64_t revision() const { return revision_; }

	// The menu image it compiled and its screen (null unless ready): what a device configures.
	const mnu::Document *image() const { return image_.get(); }
	const mnu::Screen *screen() const { return screen_; }
	const menu::MenuFrameCompiler &compiler() const { return compiler_; }
	const menu::MenuFrameState &state() const { return state_; }
	// The frame state the screen is laid out at from now on (a viewport's options held on it).
	void set_state(const menu::MenuFrameState &state) { state_ = state; }
	const menu::MenuFrameAssets &assets() const { return assets_; }
	// The frame the game would draw at a device scale (800x600 design units times it).
	const menu::MenuDrawList &compile(float scale_x, float scale_y);
	// What configure noted, then what laying the screen out comes to (its frame state's).
	std::vector<menu::MenuFrameNote> notes() const;

private:
	std::shared_ptr<const mnu::Document> image_; // the compiler borrows its screen
	const mnu::Screen *screen_ = nullptr;
	menu::MenuFrameCompiler compiler_;
	menu::MenuFrameAssets assets_;
	TextureHeaderProbe decoder_;
	menu::MenuFrameState state_;
	MenuScreenStatus status_ = MenuScreenStatus::NoScreen;
	std::string detail_;
	uint64_t revision_ = 0;
};

} // namespace opennova::editor
