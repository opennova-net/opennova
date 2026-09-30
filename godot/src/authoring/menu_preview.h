#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/menu_preview_json.h>
#include <editor/preview/menu_preview_state.h>
#include <editor/preview/menu_preview_viewport.h>

#include "mnu/menu_frame.h"

namespace opennova::editor {
struct SessionView;
}

namespace godot {

// The editor's menu preview (ADR 0046 S6c, S9j): the device half of the Preview window's
// menu pane seam. The portable model (editor/preview) says which screen the view
// previews, the menu the game would read were it saved now, and when to configure
// again; this renders it with the runtime's own MenuFrame, reading the project's files
// through the session's asset source (the open documents standing in for theirs), into
// an offscreen SubViewport the pane draws. Created in every editor run, headless
// included: the MCP and the tests read its JSON.
class MenuPreview : public opennova::editor::MenuPreviewViewport {
public:
	explicit MenuPreview(Node &owner);
	~MenuPreview() override;

	// Once per pump: follow the session's preview target.
	void refresh(const opennova::editor::SessionView &view);
	// What it shows now, for the JSON.
	opennova::editor::MenuPreviewSnapshot snapshot(const opennova::editor::SessionView &view) const;

	opennova::editor::MenuPreviewStatus status(std::string *detail) const override;
	const std::vector<std::string> &missing() const override { return model_.missing(); }
	const std::vector<std::string> &unreadable() const override { return model_.unreadable(); }
	void draw(int device_width, int device_height) override;
	// Options apply on the next refresh (a configure), then after every configure.
	void set_options(const opennova::editor::MenuPreviewOptions &options) override;
	const opennova::editor::MenuPreviewOptions &options() const override { return model_.options(); }
	const opennova::menu::MenuFrameCompiler *compiler() const override;
	const opennova::menu::MenuFrameState *frame_state() const override;
	uint64_t shown_revision() const override { return model_.shown_revision(); }

private:
	void apply_options_();

	Node &owner_;
	SubViewport *viewport_ = nullptr;
	MenuFrame *frame_ = nullptr;
	opennova::editor::MenuPreviewModel model_;
	// The files the frame reads through (it borrows them until its next configure).
	std::shared_ptr<const opennova::editor::ProjectAssetSource> assets_;
	int width_ = 0, height_ = 0; // the size the window last drew it at (0: never drawn)
};

} // namespace godot
