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
	if (ui_kit::tool(row, "Undo", document.can_undo() && view.allows(EditorRequestKind::Undo),
	                 document.can_undo() ? "Undo the last change to this file (Ctrl+Z)." : "Nothing to undo in this file."))
		workspace.request(request::undo(document.path()));
	if (ui_kit::tool(row, "Redo", document.can_redo() && view.allows(EditorRequestKind::Redo),
	                 document.can_redo() ? "Redo what Undo took back (Ctrl+Y)." : "Nothing to redo in this file."))
		workspace.request(request::redo(document.path()));
	// Who names this file (the deep-integration plan's DI-05): its uses, in the Inspector with nothing
	// selected, one hop further where a use is a definition itself (an item naming a model: the missions
	// placing that item); each a Go to there.
	const bool reading = !view.activity.validation.read || view.activity.validation.files_unread;
	const size_t uses = use_count(view, document.path());
	const std::string label = "Used by (" + (reading && !uses ? std::string("...") : std::to_string(uses)) + ")###used_by";
	const std::string tip = reading && !uses ? "The project's references are still being read."
	                        : uses ? "Who names this file or what it defines: " + std::to_string(uses) +
	                                         (uses == 1 ? " use" : " uses") +
	                                         ". Shown in the Inspector, the selection cleared, each a click away."
	                               : "No file of the project names it or what it defines.";
	if (ui_kit::tool(row, label.c_str(), (uses || reading) && view.allows(EditorRequestKind::SelectRecord), tip)) {
		workspace.request(request::select_record(document.path(), NodeAddress()));
		window_requests::focus(workspace, "inspector");
	}
	if (document.blocked())
		ImGui::TextWrapped("%s", blocked_notice ? blocked_notice
		                                        : "This file has unsupported or malformed input. See Problems, correct "
		                                          "the source, then Reload.");
	// What a save changes beyond the edits, said before it (while there are edits to save, or what the
	// game skips to leave out).
	else if (const std::string words = document.save_words(); !words.empty() && (document.dirty() || document.ignored_lines()))
		ImGui::TextWrapped("%s", words.c_str());
}

} // namespace opennova::editor
