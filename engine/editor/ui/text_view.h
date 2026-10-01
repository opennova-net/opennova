#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include <editor/model/diagnostic.h>
#include <editor/session/view/findings_index.h>
#include <editor/ui/document_views.h>

namespace opennova::editor {

class TextDocument;

// A text document's lines, read only (ADR 0046 S13 D9): what a text type's tab shows where no script
// device draws it (S13 V10: a headless workspace, the null backend's tests, a frame before the
// Shell's device is made, or one where a popup or another window lies over the tab: ui/script_view
// draws it then). Reload, Undo and Redo, what holds the document read only, then its lines as the
// game reads them (its code page shown as UTF-8): a gutter of line numbers, each line holding a
// finding marked with the worst one's severity (its message the marker's tooltip), and the line's
// text, scrolled sideways when it is wider than the tab. Clipped: a text of many thousand lines
// draws those in sight. A RevealText event (a Go to's span, a Problems row's line) is taken as the
// lines draw: the line is marked and scrolled to. The findings by line are made once per change of
// the findings or of the document.
class TextView final : public DocumentView {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override;
	void rebind(const DocumentBase &document) override;
	// Its parts, as draw draws them: the toolbar (Reload, Undo, Redo) with what holds the document read
	// only, then the lines (the RevealText events it holds taken first).
	void draw_toolbar(Workspace &workspace, const TextDocument &document);
	void draw_lines(Workspace &workspace, const TextDocument &document);

	// The line the last RevealText marked (0 for none), and how many times the lines' markers were
	// made (once per change of the findings or the document, never for a frame).
	size_t marked_line() const { return marked_; }
	size_t markers_made() const { return markers_made_; }
	// How many lines the last frame drew (the clipper's: those in sight, never the whole text).
	size_t lines_drawn() const { return lines_drawn_; }

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
	size_t lines_drawn_ = 0;
	size_t marked_ = 0;   // the line a reveal marked
	bool scroll_ = false; // scroll to it as the lines draw next
};

} // namespace opennova::editor
