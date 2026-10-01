#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include <editor/model/diagnostic.h>
#include <editor/session/view/findings_index.h>
#include <editor/ui/document_views.h>

namespace opennova::editor {

// A text document in its Document tab (ADR 0046 S13 D9; the view every text type's row makes until
// S13 V10's script device, a Godot CodeEdit, takes the tab): Reload, Undo and Redo, what holds the
// document read only, then its lines as the game reads them (its code page shown as UTF-8), read
// only, an edit being a span a request sends (the editor MCP's edit_record): a gutter of line
// numbers, each line holding a finding marked with the worst one's severity (its message the
// marker's tooltip), and the line's text, scrolled sideways when it is wider than the tab. Clipped:
// a text of many thousand lines draws those in sight. A RevealText event (a Go to's span, a Problems
// row's line) is taken as it draws: the line is marked and scrolled to. The findings by line are
// made once per change of the findings or of the document.
class TextView final : public DocumentView {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &document) override;

	// The line the last RevealText marked (0 for none), and how many times the lines' markers were
	// made (once per change of the findings or the document, never for a frame).
	size_t marked_line() const { return marked_; }
	size_t markers_made() const { return markers_made_; }

private:
	struct Marker {
		DiagnosticSeverity severity = DiagnosticSeverity::Info;
		std::string tip;
	};
	void follow_markers(const SessionView &view, const DocumentBase &document);

	FindingsIndex findings_;
	// The worst finding on each line that holds one, by line.
	std::map<size_t, Marker> markers_;
	RevisionKey markers_key_;
	uint64_t markers_document_ = 0, markers_load_ = 0, markers_revision_ = 0;
	bool markers_valid_ = false;
	size_t markers_made_ = 0;
	size_t marked_ = 0;   // the line a reveal marked
	bool scroll_ = false; // scroll to it as the lines draw next
};

} // namespace opennova::editor
