// The undo, save and close scope across documents (ADR 0046 S25, the shared seams): DocumentSet's part that lets a
// view's terrain tools edit other documents (a stroke's .cpt or map, a tile's .til, the missions that follow the
// ground) and keeps them one person's work: one Ctrl+Z, one Save, one close prompt.
#include <editor/session/document_set.h>
#include <editor/session/file_chores.h>

#include <algorithm>

#include <editor/model/document_base.h>
#include <editor/project/project_files.h>
#include <editor/session/problems_service.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

namespace {

void add_unique(std::vector<std::string> &paths, const std::string &path) {
	if (std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
}

} // namespace

DocumentBase *DocumentSet::open_quietly(const std::string &path, const std::string &why) {
	if (DocumentBase *open = document_for(path)) return open;
	if (!view_.project.open) return nullptr;
	const std::string was = view_.documents.active;
	open_document(request::open_document(path));
	DocumentBase *made = document_for(path);
	if (!was.empty() && document_for(was)) activate(was);
	core_.touch(ViewConcern::Selection);
	if (!made) return nullptr;
	quiet_.insert(made->path());
	if (!why.empty()) {
		view_.activity.status = "Opened " + made->path() + ": " + why + ".";
		core_.note("Opened " + made->path() + " in the background: " + why);
		core_.touch(ViewConcern::Output);
	}
	return made;
}

void DocumentSet::note_edited_from(const std::string &view, const std::string &edited) {
	if (view.empty() || edited.empty() || view == edited) return;
	std::vector<std::string> &scope = edited_from_[view];
	if (std::find(scope.begin(), scope.end(), edited) != scope.end()) return;
	scope.push_back(edited);
	update_step_next();
}

std::vector<std::string> DocumentSet::scope_of(const std::string &view) const {
	std::vector<std::string> out;
	if (!open_at(view)) return out;
	out.push_back(view);
	if (const auto found = edited_from_.find(view); found != edited_from_.end())
		for (const std::string &path : found->second)
			if (open_at(path)) add_unique(out, path);
	return out;
}

std::vector<std::string> DocumentSet::save_scope(const std::string &view) const {
	std::vector<std::string> out = scope_of(view);
	if (out.empty()) return {view};
	std::vector<std::string> scope = out;
	out.resize(1);
	for (size_t i = 1; i < scope.size(); ++i)
		if (const DocumentBase *document = open_at(scope[i]); document && document->unsaved()) add_unique(out, scope[i]);
	// The documents a gesture of the scope changed (a stroke's followers), where they keep unsaved edits.
	for (const auto &[token, documents] : links_) {
		const std::vector<std::string> paths = linked(token);
		const bool ours = std::any_of(paths.begin(), paths.end(), [&scope](const std::string &path) {
			return std::find(scope.begin(), scope.end(), path) != scope.end();
		});
		if (!ours || paths.size() < 2) continue;
		for (const std::string &path : paths)
			if (const DocumentBase *document = open_at(path); document && document->unsaved()) add_unique(out, path);
	}
	return out;
}

std::vector<std::string> DocumentSet::scope_unsaved(const std::string &path) const {
	std::vector<std::string> out;
	const std::vector<std::string> scope = save_scope(path);
	for (size_t i = 1; i < scope.size(); ++i) out.push_back(scope[i]);
	return out;
}

bool DocumentSet::apply_terrain_edits(DocumentBase &document, const std::vector<Edit> &edits, const std::string &view,
                                      const std::string &words) {
	const uint64_t before = document.revision();
	if (!apply_edits(document, edits, false)) return false;
	note_edited_from(view, document.path());
	if (document.revision() != before && !words.empty()) {
		view_.activity.status = words;
		name_step(document);
		core_.touch(ViewConcern::Output);
	}
	update_step_next();
	return true;
}

bool DocumentSet::apply_bake_landing(DocumentBase &document, const std::vector<Edit> &edits,
                                     const std::string &alone_words) {
	// The step keeps the stroke's words (what Undo names), and the status line stays the user's: a bake lands
	// under whatever they do next. A landing that stands alone takes its own words.
	const std::string words = document.undo_words();
	const std::string status = view_.activity.status;
	const StepMark mark = document.undo_mark();
	if (!apply_edits(document, edits, false)) return false;
	const bool amended = document.undo_mark().serial == mark.serial && mark.serial != 0;
	document.name_step(amended ? words : alone_words);
	view_.activity.status = status;
	update_step_next();
	return true;
}

std::vector<std::string> DocumentSet::linked(uint64_t gesture, std::vector<std::string> *gone) const {
	std::vector<std::string> out;
	const auto found = links_.find(gesture);
	if (found == links_.end()) return out;
	for (const LinkedDocument &each : found->second) {
		const DocumentBase *document = open_at(each.path);
		if (document && document->identity() == each.identity) add_unique(out, each.path);
		else if (gone) add_unique(*gone, each.path);
	}
	return out;
}

DocumentsView::StepNext DocumentSet::next_step(const std::string &view, bool undo) const {
	DocumentsView::StepNext out;
	const auto can = [undo](const DocumentBase &document) { return undo ? document.can_undo() : document.can_redo(); };
	const auto mark = [undo](const DocumentBase &document) { return undo ? document.undo_mark() : document.redo_mark(); };
	// The step made last (Undo: the highest serial) or undone last (Redo: the lowest), the view's own first: a step
	// of a kind whose history keeps no marks (serial 0) goes only where no other does.
	const DocumentBase *best = nullptr;
	uint64_t best_serial = 0;
	for (const std::string &path : scope_of(view)) {
		const DocumentBase *document = open_at(path);
		// A blocked document (its file changed outside the editor under its edits) takes no step: the others' go on.
		if (!document || !can(*document) || document->blocked()) continue;
		const uint64_t serial = mark(*document).serial;
		const bool better = !best || (serial != 0 && (best_serial == 0 || (undo ? serial > best_serial : serial < best_serial)));
		if (better) {
			best = document;
			best_serial = serial;
		}
	}
	if (!best) return out;
	out.path = best->path();
	out.words = undo ? best->undo_words() : best->redo_words();
	// Every other open document whose top step carries the same link goes with it.
	const uint64_t link = mark(*best).link;
	if (link)
		for (const std::string &path : linked(link)) {
			const DocumentBase *document = open_at(path);
			if (document && document != best && can(*document) && mark(*document).link == link) out.linked.push_back(path);
		}
	return out;
}

void DocumentSet::update_step_next() {
	view_.documents.undo_next = next_step(view_.documents.active, true);
	view_.documents.redo_next = next_step(view_.documents.active, false);
	view_.documents.active_scope_unsaved = view_.documents.active.empty() ? std::vector<std::string>()
	                                                                        : scope_unsaved(view_.documents.active);
	view_.documents.edited_from.clear();
	for (const auto &[view, edited] : edited_from_) {
		std::vector<std::string> open;
		for (const std::string &path : edited)
			if (open_at(path)) open.push_back(path);
		if (!open.empty()) view_.documents.edited_from[view] = std::move(open);
	}
}

void DocumentSet::undo_redo(const EditorRequest &request) {
	DocumentBase *document = document_for(request.path);
	if (!document) {
		if (view_.project.open) core_.refuse_request(CoreFinding::DocumentNotOpen, "Open the file before undoing or redoing in it.", request.path);
		return;
	}
	const bool undo = request.kind == EditorRequestKind::Undo;
	const std::string view = document->path();
	const DocumentsView::StepNext next = next_step(view, undo);
	const uint64_t serial = view_.documents.selection.serial;
	// The documents the step is taken in: its own, then each linked one whose top step carries its link.
	std::vector<std::string> taken;
	if (next.any()) {
		taken.push_back(next.path);
		for (const std::string &path : next.linked) taken.push_back(path);
	}
	const DocumentBase *own = open_at(next.path);
	const uint64_t link = own ? (undo ? own->undo_mark() : own->redo_mark()).link : 0;
	// A redo makes again what its link's undo took back together, or nothing: a document of that undo that no longer
	// holds the step (an edit after the undo dropped its redo) leaves the others' steps alone, so entities never follow a
	// stroke that is gone.
	if (!undo && link) {
		std::vector<std::string> lost;
		if (const auto found = undone_.find(link); found != undone_.end())
			for (const LinkedDocument &each : found->second) {
				const DocumentBase *other = open_at(each.path);
				if (other && other->identity() == each.identity && !other->holds_link(link)) lost.push_back(basename_of(each.path));
			}
		if (!lost.empty()) {
			view_.activity.status = "Nothing redone: " + (next.words.empty() ? std::string("the step") : next.words) +
			                        " goes with " + names_in_words(lost) + "'s, which " + (lost.size() == 1 ? "was" : "were") +
			                        " edited after the undo and no longer " + (lost.size() == 1 ? "holds" : "hold") + " it.";
			core_.outcome().stepped.clear();
			core_.touch(ViewConcern::Output);
			return;
		}
	}
	std::vector<std::string> said, moved, stuck;
	for (const std::string &path : taken) {
		DocumentBase *each = document_for(path);
		if (!each) continue;
		const uint64_t before = each->revision(), generation = each->load_generation();
		// What the step did, said as it is taken back or made again (the UX round's problems lane).
		const std::string words = undo ? each->undo_words() : each->redo_words();
		if (undo) each->undo(); else each->redo();
		if (each->revision() == before) {
			stuck.push_back(basename_of(each->path()));
			continue;
		}
		if (each->path() == view_.documents.active) repair_selection(*each, generation, before, NodeAddress());
		moved.push_back(each->path());
		said.push_back(words.empty() ? "an edit of " + basename_of(each->path()) : words);
	}
	// The documents the link changed that this step does not take: edited since (they keep their later edit), or closed.
	std::vector<std::string> kept, closed;
	if (link) {
		std::vector<std::string> gone;
		for (const std::string &path : linked(link, &gone)) {
			const DocumentBase *other = open_at(path);
			if (other && other->holds_link(link) && std::find(taken.begin(), taken.end(), path) == taken.end())
				kept.push_back(basename_of(path));
		}
		for (const std::string &path : gone) closed.push_back(basename_of(path));
	}
	if (view_.documents.selection.serial != serial) core_.touch(ViewConcern::Selection);
	const std::string file = basename_of(view);
	if (moved.empty()) {
		view_.activity.status = std::string(undo ? "Nothing to undo in " : "Nothing to redo in ") + file + ".";
	} else {
		std::string status = std::string(undo ? "Undid: " : "Redid: ");
		for (size_t i = 0; i < said.size(); ++i) status += (i ? "; " : "") + said[i];
		if (!kept.empty())
			status += "; " + names_in_words(kept) + (kept.size() == 1 ? " was" : " were") + " edited since and " +
			          (kept.size() == 1 ? "keeps its" : "keep their") + " later edits";
		if (!closed.empty())
			status += "; " + names_in_words(closed) + (closed.size() == 1 ? " was" : " were") + " closed or read again since and " +
			          (closed.size() == 1 ? "keeps its" : "keep their") + " edits as saved";
		if (!stuck.empty())
			status += "; " + names_in_words(stuck) + " did not take " + (stuck.size() == 1 ? "its step" : "their steps") +
			          " (blocked: " + (stuck.size() == 1 ? "its file" : "their files") + " changed outside the editor)";
		view_.activity.status = status + ".";
	}
	// What a link's undo took back together, for its redo; a redo made it again.
	if (link && !moved.empty()) {
		if (undo) {
			std::vector<LinkedDocument> together;
			for (const std::string &path : moved)
				if (const DocumentBase *each = open_at(path)) together.push_back(LinkedDocument{path, each->identity()});
			undone_[link] = std::move(together);
			constexpr size_t kUndoneKept = 4096;
			while (undone_.size() > kUndoneKept) undone_.erase(undone_.begin());
		} else {
			undone_.erase(link);
		}
	}
	core_.outcome().stepped = moved;
	core_.touch(ViewConcern::Output);
	update_view();
	// An undo or a redo ends the documents' gestures: the validation their edits left waiting runs now.
	if (!moved.empty()) core_.problems().validate_later();
	end_gesture_in(view, true);
	for (const std::string &path : moved) end_gesture_in(path, true);
}

// The coalesced group, or the gesture, of the document at `path` ends; a gesture open in another
// document stays open (its canvas ends it as it lets go, the wire's as its last sample comes), but for the gesture
// a token names (S25), which ends wherever its batches went.
void DocumentSet::end_edit(const std::string &path, uint64_t gesture, bool cancel) {
	DocumentBase *document = (path.empty() && gesture) ? nullptr : document_for(path);
	// A cancel naming no gesture cancels the one open in the document.
	if (!gesture && cancel && document) gesture = view_.documents.gesture_in(document->path()).token;
	std::vector<std::string> paths;
	if (document) paths.push_back(document->path());
	if (gesture) {
		for (const OpenGesture &open : view_.documents.gestures)
			if (open.token == gesture) add_unique(paths, open.path);
		for (const std::string &each : linked(gesture))
			if (open_at(each)) add_unique(paths, each);
	}
	std::vector<std::string> dropped;
	for (const std::string &each : paths) {
		DocumentBase *in = document_for(each);
		if (!in) continue;
		const OpenGesture &open = view_.documents.gesture_in(each);
		const bool ours = in == document || (gesture && open.token == gesture);
		if (cancel && gesture) {
			const uint64_t before = in->revision(), generation = in->load_generation();
			if (in->cancel_gesture(gesture)) {
				dropped.push_back(basename_of(each));
				if (each == view_.documents.active) repair_selection(*in, generation, before, NodeAddress());
			}
		}
		if (!ours) continue;
		in->end_edit_group();
		end_gesture_in(each, true);
	}
	if (!dropped.empty()) {
		view_.activity.status = "Cancelled: " + names_in_words(dropped) + (dropped.size() == 1 ? " is" : " are") + " as the stroke found " +
		                        (dropped.size() == 1 ? "it." : "them.");
		core_.touch(ViewConcern::Output);
		core_.touch(ViewConcern::Selection);
		update_view();
	}
}

void DocumentSet::release_scope(const std::string &path) {
	const auto found = edited_from_.find(path);
	std::vector<std::string> scope = found == edited_from_.end() ? std::vector<std::string>() : found->second;
	if (found != edited_from_.end()) edited_from_.erase(found);
	// The clean files opened quietly for it close with it, unless another view's scope holds them.
	bool closed = false;
	for (const std::string &each : scope) {
		if (!quiet_.count(each)) continue;
		const DocumentBase *document = open_at(each);
		if (!document || document->unsaved() || each == view_.documents.active) continue;
		const bool held = std::any_of(edited_from_.begin(), edited_from_.end(), [&each](const auto &other) {
			return std::find(other.second.begin(), other.second.end(), each) != other.second.end();
		});
		if (held) continue;
		for (auto it = documents_.begin(); it != documents_.end(); ++it)
			if ((*it)->path() == each) {
				documents_.erase(it);
				break;
			}
		quiet_.erase(each);
		remembered_.erase(each);
		forget_file_state(each);
		end_gesture_in(each, false);
		forget_document_workspace(view_.workspace, each);
		closed = true;
	}
	quiet_.erase(path);
	for (auto &[view, edited] : edited_from_) edited.erase(std::remove(edited.begin(), edited.end(), path), edited.end());
	// The links keep it: an undo of a step it shared says it was closed (DocumentSet::linked's `gone`).
	if (closed) core_.touch(ViewConcern::Workspace);
}

} // namespace opennova::editor
