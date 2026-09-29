#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/model/node.h>
#include <editor/preview/menu_preview_state.h>
#include <editor/preview/texture_header.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_frame_assets.h>

namespace opennova::editor {

class MnuDocument;

// One screen of a menu compiled headless the way the game's frame compiles it (ADR 0046
// S9j2): the menu the game would read were it saved now (MnuDocument::saved_image, the
// screen by its row's position), the shell's %VAR% list, and the screen's string tables,
// fonts and textures through a file source (the project's, the open documents standing
// in), textures measured by their headers (TextureHeaderProbe). The render check keeps
// one per screen; the MCP and the tests read its compiler and its notes.
class MenuScreenRender {
public:
	MenuScreenRender();
	~MenuScreenRender();
	MenuScreenRender(const MenuScreenRender &) = delete;
	MenuScreenRender &operator=(const MenuScreenRender &) = delete;

	// Compile the screen `screen_row` of `document`. Ready, or why not: Unserializable (the
	// detail is the first reason) or ScreenMissing (the screen's name).
	MenuPreviewStatus configure(const MnuDocument &document, NodeId screen_row, const FileSource &files,
	                            const std::map<std::string, std::string> &vars);
	MenuPreviewStatus status() const { return status_; }
	const std::string &detail() const { return detail_; }
	// The document revision the render read.
	uint64_t revision() const { return revision_; }

	const menu::MenuFrameCompiler &compiler() const { return compiler_; }
	const menu::MenuFrameState &state() const { return state_; }
	const menu::MenuFrameAssets &assets() const { return assets_; }
	// The frame the game would draw at a device scale (800x600 design units times it).
	const menu::MenuDrawList &compile(float scale_x, float scale_y);
	// What configure noted, then what laying the screen out comes to (nothing hovered).
	std::vector<menu::MenuFrameNote> notes() const;

private:
	std::shared_ptr<const mnu::Document> image_; // the compiler borrows its screen
	menu::MenuFrameCompiler compiler_;
	menu::MenuFrameAssets assets_;
	TextureHeaderProbe decoder_;
	menu::MenuFrameState state_;
	MenuPreviewStatus status_ = MenuPreviewStatus::NoScreen;
	std::string detail_;
	uint64_t revision_ = 0;
};

} // namespace opennova::editor
