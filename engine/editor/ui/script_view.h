#pragma once

#include <memory>

#include <editor/ui/document_views.h>
#include <editor/ui/text_view.h>

namespace opennova::editor {

class ScriptViewportView;

// A text document in its Document tab (ADR 0046 S13 V10; every text type's row of
// ui/document_views, the MainViewport role; CONTEXT.md "Script device"): Reload, Undo and Redo and
// what holds the document read only, then its Main view, the script device (ViewportKind::Script,
// the Shell's Godot CodeEdit) in the rest of the tab, where the control owns the input and edits the
// text through requests (preview/script_viewport). Where no device draws it (a workspace with no
// devices, a headless one, the null backend's tests; a frame before the Shell's pump has made the
// device; a popup or a window over the tab), the document's lines read only instead (ui/text_view),
// which take the RevealText events its document is sent (the device takes its reveal from its
// viewport, which the session hands each one). Its toolbar holds back its own tools while an
// operation holds the documents (each tool by the busy gate), never the device: the control is
// read only then (the viewport's editable), and the line under the toolbar says why (a file held
// read only says so in the toolbar's notice).
class ScriptView final : public DocumentView {
public:
	ScriptView();
	~ScriptView() override;

	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &document) override;
	bool main_viewport(Workspace &workspace, const DocumentBase &document) override;
	void end_frame(Workspace &workspace) override;
	bool holds_back_itself() const override { return true; }

	// The document's lines, what it draws where no device does.
	TextView &lines() { return lines_; }
	// Whether the last frame drew the device rather than the lines.
	bool device_drawn() const { return device_drawn_; }

private:
	TextView lines_;
	std::unique_ptr<ScriptViewportView> viewport_;
	bool device_drawn_ = false;
};

} // namespace opennova::editor
