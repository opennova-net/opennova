#include <editor/session/navigation_controller.h>

#include <memory>
#include <optional>

#include <editor/assets/asset_kinds.h>
#include <editor/graph/display_names.h>
#include <editor/model/document.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/text_document.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/selection.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

namespace {

using Pane = NavigationPlace::Pane;

// The open document at exactly `path`, or null.
const DocumentBase *open_at(const SessionView &view, const std::string &path) {
	for (const std::shared_ptr<const DocumentBase> &document : view.documents.open)
		if (document->path() == path) return document.get();
	return nullptr;
}

// Whether the document's view shows one row of it at a time, the selection's, as a page of it (a menu's
// screen: the Preview-role kind that shows its type shows a part of it, a page, preview/viewport_kinds.h; a
// definition table's record, which the definition preview shows, is no page).
bool shows_parts(const DocumentBase &document) {
	const DocumentTypeId type = asset_kind_row(document.kind()).document;
	const ViewportKind kind = preview_kind_of(type);
	return kind != ViewportKind::kCount && viewport_kind_row(kind).part && viewport_kind_row(kind).pages &&
	       viewport_kind_shows(kind, type);
}

} // namespace

NavigationController::NavigationController(SessionCore &core) : core_(core), view_(core.view()) {}

NavigationController::Mark NavigationController::mark(const EditorRequest &request) const {
	Mark mark;
	mark.active = view_.documents.active;
	mark.selection = view_.documents.selection.serial;
	mark.page = view_.documents.page;
	mark.events = view_.events.next_seq();
	if (request_kind_row(request.kind).navigates && view_.project.open) mark.place = here();
	return mark;
}

void NavigationController::follow(const EditorRequest &request, const Mark &before) {
	// What it revealed of a text (a Go to's span, a Problems row's line): that text's place from now on.
	const ViewEvent *revealed_file = nullptr;
	for (const ViewEvent &event : view_.events.held()) {
		if (event.seq < before.events) continue;
		if (event.kind == ViewEventKind::RevealText) text_places_[event.path] = event.locator;
		if (event.kind == ViewEventKind::RevealFile) revealed_file = &event;
	}
	if (!view_.project.open) return;
	tidy();
	// Back and Forward put the person where they went themselves (go).
	if (request.kind == EditorRequestKind::NavigateBack || request.kind == EditorRequestKind::NavigateForward) return;
	// The pane it showed: Files on the file it revealed there; the page of the file an open named; the
	// document, where it moved the document's place (a record picked, another document active) or opened
	// the one active (its tab over a page's). A request refused shows nothing.
	const bool document_moved =
	        view_.documents.active != before.active || view_.documents.selection.serial != before.selection;
	const bool opened = request.kind == EditorRequestKind::OpenDocument && !core_.outcome().refused;
	const AssetEntry *opened_file = opened ? core_.project_file(request.path) : nullptr;
	if (revealed_file) {
		pane_ = Pane::Files;
		pane_path_ = revealed_file->path;
	} else if (opened_file && !view_.documents.page.empty() && opened_file->relative_path == view_.documents.page) {
		pane_ = Pane::Page;
		pane_path_ = view_.documents.page;
	} else if (document_moved || opened) {
		pane_ = Pane::Document;
	}
	// A file picked in Files while the person is there: their place there moves with it.
	const std::string &picked = view_.documents.file_selected.path;
	if (request.kind == EditorRequestKind::SelectFile && pane_ == Pane::Files && !picked.empty()) pane_path_ = picked;
	// A page closed: the person is back at the document.
	if (pane_ == Pane::Page && view_.documents.page != pane_path_) pane_ = Pane::Document;
	if (pane_ == Pane::Document) pane_path_.clear();
	if (!request_kind_row(request.kind).navigates) return;
	const NavigationPlace arrived = here();
	if (!is_step(request, before.place, arrived)) return;
	if (history_.moved(before.place, arrived, core_.platform().now_ms())) publish();
}

bool NavigationController::is_step(const EditorRequest &request, const NavigationPlace &from,
                                   const NavigationPlace &to) const {
	if (request.kind != EditorRequestKind::SelectRecord) return true;
	if (from.pane != to.pane || from.path != to.path) return true;
	// Within the document that shows, a record picked is where the person is; another of a menu's screens
	// is a step, as a page of the menu is.
	const DocumentBase *document = open_at(view_, to.path);
	return document && shows_parts(*document) && from.document == to.document && from.record.row != to.record.row;
}

NavigationPlace NavigationController::here() const {
	if (!view_.project.open) return NavigationPlace();
	if (pane_ == Pane::Files && !pane_path_.empty()) return file_place(Pane::Files, pane_path_);
	if (pane_ == Pane::Page && !pane_path_.empty() && view_.documents.page == pane_path_)
		return file_place(Pane::Page, pane_path_);
	return document_place();
}

NavigationPlace NavigationController::file_place(Pane pane, const std::string &path) const {
	NavigationPlace place;
	place.pane = pane;
	place.path = path;
	if (pane != Pane::Page) {
		place.label = basename_of(path) + " in Files";
		return place;
	}
	// On a page, the record and field the Go to that showed it marked (DI-17).
	place.locator = view_.documents.page_locator;
	place.field = view_.documents.page_field;
	const std::string at = place.locator.empty() ? place.field : place.locator;
	place.label = "About " + basename_of(path) + (at.empty() ? std::string() : ": " + at);
	return place;
}

NavigationPlace NavigationController::document_place() const {
	NavigationPlace place;
	const std::string &path = view_.documents.active;
	if (path.empty()) return place;
	place.path = path;
	const std::string name = basename_of(path);
	place.label = name;
	const DocumentBase *document = open_at(view_, path);
	if (!document) return place;
	if (const Document *records = records_of(*document)) {
		const Selection &selection = view_.documents.selection;
		if (selection.document != path || !selection.primary.row || !has_record(*records, selection.primary))
			return place;
		place.record = selection.primary;
		place.document = document->identity();
		place.locator = records->locator(selection.primary);
		std::optional<GraphNameSource> names;
		if (view_.findings.graph) names.emplace(*view_.findings.graph);
		const std::string title = record_display(*records, selection.primary, names ? &*names : nullptr);
		if (!title.empty()) place.label = name + ": " + title;
		return place;
	}
	if (text_of(*document)) {
		const auto shown = text_places_.find(path);
		size_t line = 0, column = 0;
		if (shown != text_places_.end() && TextDocument::read_locator(shown->second, line, column)) {
			place.locator = shown->second;
			place.label = name + ", line " + std::to_string(line);
		}
	}
	return place;
}

void NavigationController::go(bool back, size_t steps) {
	const std::string way = back ? "back" : "forward";
	if (!view_.project.open)
		return core_.refuse_quietly(CoreFinding::NavigationNone, "No project is open: there is nothing to go " + way + " to.");
	tidy();
	NavigationPlace to;
	if (!history_.step(back, steps, here(), to)) {
		const std::string away = steps > 1 ? " " + std::to_string(steps) + " places away." : ".";
		return core_.refuse_quietly(CoreFinding::NavigationNone, "There is no place to go " + way + " to" + away);
	}
	show(to);
	publish();
	view_.activity.status = std::string(back ? "Back to " : "Forward to ") + to.label + ".";
	core_.touch(ViewConcern::Output);
}

void NavigationController::show(const NavigationPlace &place) {
	DocumentSet &documents = core_.documents();
	if (place.pane == Pane::Files) {
		documents.show_in_files(request::show_in_files(place.path));
		pane_ = Pane::Files;
		pane_path_ = place.path;
		return;
	}
	EditorRequest open = request::open_document(place.path);
	if (place.pane == Pane::Page) {
		// The page with the line it marked (the open brings the Document window forward with its tab).
		open.locator = place.locator;
		open.field = place.field;
	} else {
		// Its record by the address it was kept by while that read of the document stands, else by its
		// locator (read again since: closed and opened, renamed, changed on disk); a text at its line.
		const DocumentBase *document = documents.document_for(place.path);
		const Document *records = document ? records_of(*document) : nullptr;
		if (records && place.record.row && document->identity() == place.document && has_record(*records, place.record))
			open.address = place.record;
		else
			open.locator = place.locator;
	}
	documents.open_document(open);
	pane_ = place.pane;
	pane_path_ = place.pane == Pane::Page ? place.path : std::string();
	// The Document window comes forward with its tab, whichever tab showed before (the active document's
	// over a page's; a page's own open asks it for the page's, DocumentSet::open_document).
	if (place.pane == Pane::Document) {
		ViewEvent shown;
		shown.kind = ViewEventKind::ShowDocument;
		shown.path = place.path;
		view_.events.post(std::move(shown));
	}
	core_.touch(ViewConcern::Selection);
}

void NavigationController::tidy() {
	const uint64_t files = view_.revisions.of(ViewConcern::Files);
	if (files == files_seen_ || !view_.project.open) return;
	files_seen_ = files;
	if (history_.drop([this](const NavigationPlace &place) { return core_.project_file(place.path) == nullptr; }))
		publish();
}

void NavigationController::publish() {
	view_.navigation.back.assign(history_.back().begin(), history_.back().end());
	view_.navigation.forward.assign(history_.forward().begin(), history_.forward().end());
	core_.touch(ViewConcern::Navigation);
}

void NavigationController::clear() {
	history_.clear();
	pane_ = Pane::Document;
	pane_path_.clear();
	text_places_.clear();
	files_seen_ = view_.revisions.of(ViewConcern::Files);
	publish();
}

bool navigation_offered(const SessionView &view, bool back) {
	if (!view.project.open || !(back ? view.navigation.can_back() : view.navigation.can_forward())) return false;
	return view.allows(back ? EditorRequestKind::NavigateBack : EditorRequestKind::NavigateForward) &&
	       shown_modal(view).modal == HeldModal::None;
}

void NavigationController::follow_moves(const std::vector<std::pair<std::string, std::string>> &moved) {
	for (const auto &[from, to] : moved) {
		if (pane_path_ == from) pane_path_ = to;
		const auto shown = text_places_.find(from);
		if (shown != text_places_.end()) {
			std::string locator = shown->second;
			text_places_.erase(shown);
			text_places_[to] = std::move(locator);
		}
	}
	if (history_.follow_moves(moved)) publish();
}

} // namespace opennova::editor
