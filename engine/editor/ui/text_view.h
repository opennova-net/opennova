#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include <editor/model/diagnostic.h>
#include <editor/session/view/findings_index.h>
#include <editor/session/view/view_events.h>
#include <editor/ui/view_event_mailbox.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

class DocumentBase;
class TextDocument;

// A text document's lines, read only (ADR 0046 S13 D9), and the toolbar above them: what ui/script_view
// draws of a text's tab where no script device draws the document (S13 V10: a headless workspace,
// the null backend's tests, a frame before the Shell's device is made, or one where a popup or
// another window lies over the tab). The toolbar: Reload, Undo and Redo and what holds the document
// read only, their tips above them so they never lie over the rect the device is placed in (the
// toolbar is drawn above the device as well as above the lines). Then the lines as the game reads
// them (its code page shown as UTF-8): a gutter of line numbers, each line holding a finding marked
// with the worst one's severity (its message the marker's tooltip), and the line's text, scrolled
// sideways when it is wider than the tab. Clipped: a text of many thousand lines draws those in
// sight. A RevealText event (a Go to's span, a Problems row's line) the script view hands it is
// taken as the lines draw: the line is marked and scrolled to. The findings by line are made once
// per change of the findings or of the document.
class TextView final {
public:
	// A RevealText event for the lines, held until they draw.
	void receive(const ViewEvent &event) { events_.post(event); }
	// The document at the view's path was read again: what names the old lines goes.
	void rebind(const DocumentBase &document);
	// Its parts, as the script view draws them: the toolbar (Reload, Undo, Redo) with what holds the
	// document read only, then the lines (the RevealText events it holds taken first).
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

	ViewEventMailbox<> events_;
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
