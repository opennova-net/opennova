#include "document_toolbar.h"

#include <cstdint>
#include <map>
#include <string>

#include <editor/graph/asset_graph.h>
#include <editor/session/file_card.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// How many uses a file has (file_use_count), counted again only when the graph moved (its generation: no
// two graphs and no two states of one graph share one, so a count kept by it is never another graph's) or
// the files did; a toolbar draws every frame, a string table's uses are thousands. The documents shown at
// once are few: the counts of the last few kept.
size_t use_count(const SessionView &view, const std::string &path) {
	struct Count {
		uint64_t generation = 0;
		RevisionKey files;
		size_t uses = 0;
	};
	static std::map<std::string, Count> counts;
	const uint64_t generation = view.findings.graph ? view.findings.graph->generation() : 0;
	const RevisionKey files = revision_key(view.revisions, {ViewConcern::Files});
	const auto kept = counts.find(path);
	if (kept != counts.end() && kept->second.generation == generation && kept->second.files == files)
		return kept->second.uses;
	if (counts.size() >= 16) counts.clear();
	Count &count = counts[path];
	count.generation = generation;
	count.files = files;
	count.uses = file_use_count(view, path);
	return count.uses;
}

} // namespace

// Each tool enabled while the busy gate takes its request too (SessionView::allows).
void draw_document_toolbar(Workspace &workspace, const DocumentBase &document, const char *blocked_notice) {
	const SessionView &view = workspace.view();
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Reload", view.allows(EditorRequestKind::ReloadDocument),
	                 "Read the file again (asks first when it has unsaved changes)."))
		workspace.request(request::reload_document(document.path()));
	// S25: in this view, its own step or one its terrain tools made in another file (DocumentsView::undo_next).
	const bool undoes = document.can_undo() || (view.documents.active == document.path() && view.documents.undo_next.any());
	const bool redoes = document.can_redo() || (view.documents.active == document.path() && view.documents.redo_next.any());
	if (ui_kit::tool(row, "Undo", undoes && view.allows(EditorRequestKind::Undo),
	                 undoes ? "Undo the last change in this view (Ctrl+Z)." : "Nothing to undo in this file."))
		workspace.request(request::undo(document.path()));
	if (ui_kit::tool(row, "Redo", redoes && view.allows(EditorRequestKind::Redo),
	                 redoes ? "Redo what Undo took back (Ctrl+Y)." : "Nothing to redo in this file."))
		workspace.request(request::redo(document.path()));
	// Who names this file (the deep-integration plan's DI-05): its uses, in the Inspector with nothing
	// selected, one hop further where a use is a definition itself (an item naming a model: the missions
	// placing that item); each a Go to there.
	const bool reading = !view.activity.validation.read || view.activity.validation.files_unread;
	const size_t uses = use_count(view, document.path());
	const std::string label = "Used by (" + (reading && !uses ? std::string("...") : std::to_string(uses)) + ")###used_by";
	const std::string tip = reading && !uses ? std::string(ui_kit::reading_references())
	                        : uses ? std::string("Who names this file or what it defines, listed in the Inspector.")
	                               : std::string(ui_kit::nothing_names_it(true));
	if (ui_kit::tool(row, label.c_str(), (uses || reading) && view.allows(EditorRequestKind::SelectRecord), tip)) {
		workspace.request(request::select_record(document.path(), NodeAddress()));
		window_requests::focus(workspace, "inspector");
	}
	if (document.blocked()) {
		if (blocked_notice) {
			ui_kit::refusal_text(blocked_notice);
			return;
		}
		// Read only while input its Save cannot write stands: how many lines, on one line, Problems' fix in the
		// tooltip; input on no line (a table's whole file) said as such, with no fix to point to.
		size_t lines = 0;
		for (const SourceIssue &issue : document.issues()) lines += issue.blocks && issue.line > 0 ? 1 : 0;
		const std::string line = lines ? "Read only: " + std::to_string(lines) + (lines == 1 ? " line" : " lines") +
		                                     " the editor cannot carry (Problems)"
		                               : std::string("Read only: the editor cannot carry this file (Problems)");
		row.next(ui_kit::text_width(line.c_str()));
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, ui_kit::refusal_color());
		ui_kit::clipped_text(line, lines ? "A save cannot write these lines as the game reads them. Problems' Correct "
		                                   "the line... rewrites one in the file and reads it again."
		                                 : "A save cannot write this file as the game reads it: Problems says why.");
		ImGui::PopStyleColor();
		return;
	}
	// What a save changes beyond the edits, on the tools' row, its words in the tooltip (while there are
	// edits to save, or lines the game skips to leave out): amber while the save leaves lines out.
	const std::string words = document.save_words();
	const size_t ignored = document.ignored_lines();
	if (words.empty() || (!document.dirty() && !ignored)) return;
	const std::string line = ignored ? "Save leaves out " + std::to_string(ignored) + (ignored == 1 ? " line" : " lines")
	                                 : std::string("Save rewrites the file");
	row.next(ui_kit::text_width(line.c_str()));
	ImGui::AlignTextToFramePadding();
	if (ignored) ui_kit::warning_line(line, words);
	else ui_kit::quiet_text(line, words);
}

} // namespace opennova::editor
