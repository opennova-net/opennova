#pragma once

#include <string>

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The font viewport's view (round S23 lane A): its toolbar (the text or a page, the zoom, the colour, bold, italic
// and underline, the text it draws), then the canvas filling the rest (preview/font_viewport's canvas half: the
// glyph under the pointer named, a click selecting it). Every change a SetViewport.
class FontViewportView final : public ViewportView {
public:
	FontViewportView();

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;

private:
	// The text being typed, sent as the options' text once it changes.
	std::string text_;
	std::string text_path_;
};

} // namespace opennova::editor
