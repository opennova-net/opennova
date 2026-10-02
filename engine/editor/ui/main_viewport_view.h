#pragma once

#include <memory>

#include <editor/preview/viewport_kinds.h>
#include <editor/ui/document_views.h>
#include <editor/ui/outline_model.h>

namespace opennova::editor {

class OutlineView;
class ViewportView;

// The view of a document whose type's row plays the MainViewport role (ADR 0046 S13 V3, V5; decision
// 11 as S13 amends it): the document's outline (its row's OutlineSpec) in a column that resizes,
// beside a viewport of the Main-role kind that shows the type, whose canvas fills the rest of the
// tab with its ImGui overlays; the Inspector stays the generic one beside the Document window. The
// viewport is the session's, kept while the document is open (Viewports::track), and drawn through
// the Shell's device of (document, kind). The mission's 3D view plugs in as rows: its type's view
// row with this role and a Main-role ViewportKind that shows it; no type has one yet. While an
// operation holds the documents (S13 A3) its outline column is held back, never its canvas: the
// camera and the picture stay live (a SetViewport runs beside any operation), the canvas holding
// back its own edits (CanvasWindowRequests).
class MainViewportView final : public DocumentView {
public:
	MainViewportView(const OutlineSpec &outline, ViewportKind kind);
	~MainViewportView() override;

	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &document) override;
	bool main_viewport(Workspace &workspace, const DocumentBase &document) override;
	OutlineModel *outline() override;
	void end_frame(Workspace &workspace) override;
	bool holds_back_itself() const override { return true; }
	ViewportKind kind() const { return kind_; }

private:
	std::unique_ptr<OutlineView> outline_;
	std::unique_ptr<ViewportView> viewport_;
	ViewportKind kind_;
};

} // namespace opennova::editor
