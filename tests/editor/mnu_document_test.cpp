// The menu document (ADR 0046 S6c, S9g, S9h) over the neutral core: every record the
// format holds is a record at its own depth through the menu's table
// (editor/documents/mnu_table, rows of the one table shape since S13 D10). The blank startup menu loads as one screen whose windows hold
// their lists and children; fields read and write by element path; windows and the rows
// of every list are added, duplicated (names made unique), removed and moved (a reparent
// between windows included), each with its undo and the identities kept in the native
// tree's shape; a screen holds every root window and keeps one; a part's toggle leaves it
// out and brings it back; windows copy and paste across screens and files; the saved
// file reparses with every edit in place; serialize issues sit on the record and field
// that cause them; the validator resolves fonts and colors through the stylesheet. S9d:
// the edits that must not lose authored data, and the retail sweep: every shipped menu
// through the document (a Set of every field's own value, then a structural edit of every
// list kind, undone byte for byte and read back after a save), a SKIP-LEG per root. S9p1:
// a parse note on input retail ignores warns, one on input retail crashes or hangs on
// blocks the menu and the build. S13 D5: a screen's or a window's NAME set is its own edit, and
// Rename everywhere rewrites its uses.
#include <editor/documents/mnu_document.h>
#include <editor/documents/document_types.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/display_names.h>
#include <editor/graph/reference_queries.h>
#include <editor/project_build/build_run.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/project/project_files.h>
#include <base/vfs/vfs.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_runtime.h>
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/menu_test_support.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <utility>

using namespace opennova::editor;
namespace mnu = opennova::mnu;
using menu_test::child_of;
using menu_test::depth_of;
using menu_test::image_edits;
using menu_test::window_of;

namespace {

constexpr NodeKind kScreen = node_kind(MenuKind::Screen);
constexpr NodeKind kWindow = node_kind(MenuKind::Window);

using editor_test::NoProcess;

Edit set(NodeAddress address, const char *field, Value value) { return menu_test::set_edit(address, field, std::move(value)); }

Edit op(EditOperation operation, NodeAddress address, NodeId parent = 0, size_t position = SIZE_MAX) {
	Edit edit;
	edit.operation = operation;
	edit.address = address;
	edit.parent = parent;
	edit.position = position;
	return edit;
}

// One keystroke in the inspector: a Set folded into the field's open edit group.
Edit typed(NodeAddress address, const char *field, const char *value) {
	Edit edit = set(address, field, std::string(value));
	edit.coalesce = true;
	return edit;
}

std::string text_of(const Document &document, NodeAddress address, const char *field) {
	Value value;
	if (!document.get(address, field, value)) return "<none>";
	if (const auto *text = std::get_if<std::string>(&value)) return *text;
	return std::to_string(std::get<int64_t>(value));
}

// The screen's windows (roots and children, not the parts) in pre-order.
std::vector<std::string> window_names(const MnuDocument &document, const Node &screen) {
	std::vector<std::string> names;
	document.walk_records(screen, [&](const NodeAddress &record, const Document::Placement &) {
		if (record.kind == kWindow) names.push_back(window_of(document, record)->name);
		return true;
	});
	return names;
}

size_t record_count(const Document &document) {
	size_t count = 0;
	for (const auto &row : document.rows())
		document.walk_records(*row, [&](const NodeAddress &, const Document::Placement &) {
			++count;
			return true;
		});
	return count;
}

// A window's index among its siblings and how many they are.
std::pair<size_t, size_t> sibling_place(const Document &document, const NodeAddress &window) {
	Document::Placement at;
	if (!document.placement(window, at)) return {0, 0};
	for (const Document::Collection &collection : document.collections_of(at.owner))
		if (collection.spec.kind == kWindow) return {at.index, collection.ids.size()};
	return {0, 0};
}

bool has_code(const std::vector<Diagnostic> &diagnostics, const char *code) {
	for (const Diagnostic &d : diagnostics) if (d.code() == code) return true;
	return false;
}

bool load(MnuDocument &document, const std::string &path) {
	Diagnostic error;
	return document.load(path, std::filesystem::path(path).filename().generic_string(), AssetKind::Menu, "jo", error);
}

int structure_and_save() {
	editor_test::TempProjectDir dir("opennova_menu_document_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "John Smith"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.requirements->required_missing == 0);
	TEST_EXPECT(!has_code(view.findings.diagnostics, "reference.missing"));
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	auto *document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document && !document->blocked() && document->rows().size() == 1 && document->identities_match());
	// An edit swaps the row, so the screen is re-read after every edit.
	auto screen = [&]() -> const Node & { return *document->rows()[0]; };
	TEST_EXPECT(screen().name() == "STARTUP");
	TEST_EXPECT(window_names(*document, screen()) == std::vector<std::string>({"MAIN", "TITLE", "EXIT"}));
	NodeAddress title;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "title", title) && title.kind == kWindow &&
			document->window_index(title) == 1);
	TEST_EXPECT(depth_of(*document, title) == 1 && sibling_place(*document, title).first == 0);
	NodeAddress startup;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "STARTUP", startup) &&
			startup.kind == kScreen);
	TEST_EXPECT(text_of(*document, title, "string.value") == "John Smith" && text_of(*document, title, "type") == "static");
	TEST_EXPECT(text_of(*document, title, "string.justify") == "CENTER" && text_of(*document, title, "position.top") == "120");
	const NodeAddress root{screen().id, kWindow, document->window_at(screen(), 0)};
	TEST_EXPECT(text_of(*document, root, "font.name") == "%DEF_FONTNAME_LG%");
	// The John Smith edit: the title text, then a new button under the root window.
	Diagnostic error;
	TEST_EXPECT(document->apply(set(title, "string.value", std::string("John Smith's Game")), error));
	// A screen holds every root window: a window added to the screen is a second root.
	TEST_EXPECT(document->apply(op(EditOperation::Add, {screen().id, kWindow, 0}), error));
	TEST_EXPECT(window_names(*document, screen()) == std::vector<std::string>({"MAIN", "TITLE", "EXIT", "WINDOW1"}));
	TEST_EXPECT(document->collections_of({screen().id, kScreen, 0})[0].ids.size() == 2 &&
	            depth_of(*document, {screen().id, kWindow, document->last_added()}) == 0);
	document->undo();
	TEST_EXPECT(document->apply(op(EditOperation::Add, {screen().id, kWindow, 0}, root.child), error));
	const NodeAddress later{screen().id, kWindow, document->last_added()};
	TEST_EXPECT(window_names(*document, screen()) == std::vector<std::string>({"MAIN", "TITLE", "EXIT", "WINDOW1"}));
	TEST_EXPECT(document->apply(set(later, "name", std::string("LATER")), error));
	TEST_EXPECT(document->apply(set(later, "type", std::string("button")), error));
	TEST_EXPECT(document->apply(set(later, "string.value", std::string("Later")), error));
	TEST_EXPECT(document->apply(set(later, "position.left", int64_t(340)), error) &&
	            document->apply(set(later, "position.top", int64_t(420)), error));
	TEST_EXPECT(document->apply(set(later, "position.right", int64_t(460)), error));
	// Element text takes any character (the writer escapes '<' and '&'); a quote in an
	// attribute value is a blocking serialize issue on the record and field that hold it.
	TEST_EXPECT(document->apply(set(later, "string.value", std::string("<b> & c")), error) && document->serialize().ok());
	TEST_EXPECT(document->apply(set(later, "name", std::string("LA\"TER")), error));
	const SerializeResult quoted = document->serialize();
	TEST_EXPECT(!quoted.ok() && quoted.issues[0].blocks && quoted.issues[0].message.find("quote") != std::string::npos);
	TEST_EXPECT(quoted.issues[0].field == "name" && quoted.issues[0].locator == document->locator(later) &&
	            quoted.issues[0].record == "STARTUP/MAIN/LA\"TER");
	TEST_EXPECT(document->apply(set(later, "name", std::string("LATER")), error));
	TEST_EXPECT(document->apply(set(later, "string.value", std::string("Later")), error) && document->serialize().ok());
	// A nested child (named WINDOW1: no other window has that name now), then the button
	// moved to the front of its siblings.
	TEST_EXPECT(document->apply(op(EditOperation::Add, {0, kWindow, 0}, later.child), error));
	const NodeAddress nested{screen().id, kWindow, document->last_added()};
	TEST_EXPECT(window_names(*document, screen()) == std::vector<std::string>({"MAIN", "TITLE", "EXIT", "LATER", "WINDOW1"}));
	TEST_EXPECT(depth_of(*document, nested) == 2 && document->window_index(nested) == 4);
	TEST_EXPECT(document->apply(op(EditOperation::Move, later, 0, 0), error));
	TEST_EXPECT(window_names(*document, screen()) == std::vector<std::string>({"MAIN", "LATER", "WINDOW1", "TITLE", "EXIT"}));
	TEST_EXPECT(sibling_place(*document, later).first == 0 && document->window_index(nested) == 2);
	// Duplicate carries the subtree, right after the original, every name made unique (B6);
	// Remove takes it away; a screen keeps one root window.
	TEST_EXPECT(document->apply(op(EditOperation::Duplicate, later, 0, 1), error));
	TEST_EXPECT(window_names(*document, screen()) ==
	            std::vector<std::string>({"MAIN", "LATER", "WINDOW1", "LATER2", "WINDOW2", "TITLE", "EXIT"}));
	TEST_EXPECT(document->identities_match());
	TEST_EXPECT(document->apply(op(EditOperation::Remove, {screen().id, kWindow, document->last_added()}), error));
	TEST_EXPECT(window_names(*document, screen()).size() == 5 && document->identities_match());
	TEST_EXPECT(!document->apply(op(EditOperation::Remove, root), error) && error.message.find("one root") != std::string::npos);
	// Undo the removal, redo it, then save and reparse.
	document->undo();
	TEST_EXPECT(window_names(*document, screen())[3] == "LATER2");
	document->redo();
	TEST_EXPECT(document->save(error) && !document->dirty());
	mnu::Document reparsed;
	std::string message;
	TEST_EXPECT(mnu::parse_file(dir.file("project/menus/main.mnu"), reparsed, message));
	TEST_EXPECT(reparsed.screens.size() == 1 && reparsed.screens[0].roots.size() == 1);
	TEST_EXPECT(reparsed.screens[0].roots[0].children.size() == 3);
	const mnu::Window &first = reparsed.screens[0].roots[0].children[0];
	TEST_EXPECT(first.name == "LATER" && first.type == mnu::WindowType::Button && first.string_data.value == "Later");
	TEST_EXPECT(first.position.left == 340 && first.position.top == 420 && first.children.size() == 1);
	TEST_EXPECT(reparsed.screens[0].roots[0].children[1].string_data.value == "John Smith's Game");
	// A second screen from the top-level kind, then a window found on its own screen and gone to
	// by its locator (S12 D3).
	TEST_EXPECT(document->apply(op(EditOperation::Add, {0, kScreen, 0}), error) && document->rows().size() == 2);
	TEST_EXPECT(document->collections_of({document->rows()[1]->id, kScreen, 0})[0].ids.size() == 1);
	NodeAddress exit, elsewhere;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "EXIT", exit,
			menu_window_scope(document->path(), document->rows()[0]->name())));
	TEST_EXPECT(!find_definition(AssetGraph(), *document, "EXIT", elsewhere,
			menu_window_scope(document->path(), document->rows()[1]->name())));
	editor_test::handle_to_end(session, request::open_document("main.mnu", document->locator(exit)));
	TEST_EXPECT(view.documents.selection.primary == exit &&
			window_of(*document, view.documents.selection.primary)->name == "EXIT");
	return 0;
}

int validation() {
	editor_test::TempProjectDir dir("opennova_menu_validation_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Menus"));
	editor_test::create_missing_files(session);
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	auto *document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document);
	const SessionView &view = session.view();
	TEST_EXPECT(view.findings.graph && view.findings.graph->resolve(ReferenceKind::StyleVar, "%DEF_FONTNAME_LG%") == ReferenceStatus::Present);
	TEST_EXPECT(view.findings.graph->resolve_style("%DEF_FONTNAME_LG%") != "%DEF_FONTNAME_LG%");
	NodeAddress exit;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "EXIT", exit));
	auto edit = [&](const std::vector<Edit> &edits) {
		EditorRequest request = request::edit_record(document->path(), edits);
		editor_test::handle_to_end(session, request);
		return session.last_edit_ok();
	};
	// The badge and the validator agree: a font through an undefined variable, a missing
	// texture, an unknown string id and a missing menu file.
	const FieldSchema *font = nullptr;
	for (const FieldSchema &field : document->fields(kWindow))
		if (field.id == "font.name") font = &field;
	TEST_EXPECT(font && font->reference == ReferenceKind::Font);
	if (!font) return 1;
	const FieldUse font_use = document->field_on(exit, *font);
	TEST_EXPECT(edit({set(exit, "font.name", std::string("%NOPE%"))}) && has_code(view.findings.diagnostics, "reference.missing"));
	TEST_EXPECT(reference_status(*view.findings.graph, font_use, std::string("%NOPE%")) ==
			ReferenceStatus::Missing);
	TEST_EXPECT(reference_status(*view.findings.graph, font_use,
						std::string("%DEF_FONTNAME_LG%")) == ReferenceStatus::Present);
	TEST_EXPECT(edit({set(exit, "font.name", std::string("nofont.fnt"))}) && has_code(view.findings.diagnostics, "reference.missing"));
	// An APPEARANCE's value is a texture when its TYPE is IMAGE (field_on).
	const NodeAddress row = child_of(*document, exit, "appearance");
	const FieldSchema *value = nullptr;
	for (const FieldSchema &field : document->fields(row.kind))
		if (field.id == "value") value = &field;
	TEST_EXPECT(value && value->reference == ReferenceKind::None && document->field_on(row, *value).reference == ReferenceKind::None);
	TEST_EXPECT(edit(image_edits(*document, exit, "missing.tga")) && has_code(view.findings.diagnostics, "reference.missing"));
	const FieldUse image = document->field_on(row, *value);
	TEST_EXPECT(image.reference == ReferenceKind::MenuTexture);
	TEST_EXPECT(reference_status(*view.findings.graph, image, std::string("missing.tga")) ==
			ReferenceStatus::Missing);
	TEST_EXPECT(edit({set(exit, "string.type", std::string("ID")), set(exit, "string.value", std::string("NO_SUCH_ID"))}) &&
	            has_code(view.findings.diagnostics, "reference.missing"));
	// An ACTION: a new row is POP_SCREEN; a SCREEN action names a menu file.
	TEST_EXPECT(edit({op(EditOperation::Add, {exit.row, menu_kind("action"), 0}, exit.child)}));
	const NodeAddress action = child_of(*document, exit, "action");
	TEST_EXPECT(text_of(*document, action, "type") == "POP_SCREEN");
	TEST_EXPECT(edit({set(action, "type", std::string("SCREEN")), set(action, "file", std::string("other.mnu"))}) &&
	            has_code(view.findings.diagnostics, "reference.missing"));
	// A SCREEN action with no FILE: an error on that action's file field.
	TEST_EXPECT(edit({set(action, "file", std::string())}));
	bool on_the_field = false;
	for (const Diagnostic &d : view.findings.diagnostics)
		on_the_field = on_the_field || (d.code() == "menu.unserializable" && d.field == "file" && d.child_id == action.child &&
		                                d.row_id == action.row && d.record_kind == action.kind);
	TEST_EXPECT(on_the_field);
	TEST_EXPECT(edit({set(action, "file", std::string("other.mnu"))}));
	TEST_EXPECT(!reference_choices(*view.findings.graph, font_use).empty()); // the project's fonts
	// Build waits on the unsaved prompt over the edited menu; its Save writes the menu and
	// then builds: the missing texture, string id and menu are listed and gate nothing (S14).
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(view.dialogs.unsaved_prompt.open && !view.dialogs.unsaved_prompt.can_discard && !session.view().activity.operation.running() &&
	            view.dialogs.unsaved_prompt.files == std::vector<std::string>{document->path()});
	EditorRequest save = request::resolve_unsaved(UnsavedChoice::Save);
	editor_test::handle_to_end(session, save);
	TEST_EXPECT(!document->dirty() && !view.dialogs.unsaved_prompt.open && view.activity.has_build && view.activity.last_build->ok &&
	            has_code(view.findings.diagnostics, "reference.missing"));
	// A new menu by name and kind.
	editor_test::handle_to_end(session, request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
	auto *extra = session.document_for("extra.mnu");
	TEST_EXPECT(extra && !extra->rows().empty());
	return 0;
}

// --- Windows at any depth (ADR 0046 S9g, S9h) ------------------------------------------

int windows_at_depth() {
	editor_test::TempProjectDir dir("opennova_menu_depth_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Depth"));
	editor_test::create_missing_files(session);
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	auto *document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document && !document->dirty());
	auto screen = [&]() -> const Node & { return *document->rows()[0]; };
	NodeAddress title, exit;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "TITLE", title) &&
			find_definition(AssetGraph(), *document, "EXIT", exit));
	const NodeAddress root{screen().id, kWindow, document->window_at(screen(), 0)};
	const NodeAddress screen_address{screen().id, kScreen, 0};
	// The screen holds its root windows; a window its lists in file order, its child
	// windows last.
	const std::vector<Document::Collection> top = document->collections_of(screen_address);
	TEST_EXPECT(top.size() == 1 && !top[0].spec.fixed && top[0].ids == std::vector<NodeId>{root.child});
	const std::vector<Document::Collection> lists = document->collections_of(root);
	TEST_EXPECT(lists.size() == menu_table().kind(kWindow)->lists().size());
	TEST_EXPECT(std::string(document->kind_token(lists.back().spec.kind)) == "window" &&
	            lists.back().ids == std::vector<NodeId>({title.child, exit.child}));
	TEST_EXPECT(std::string(document->kind_token(lists[2].spec.kind)) == "action" && lists[2].spec.kind == menu_kind("action"));
	// Which lists the root's type reads: a generic window reads no ITEMS, a button no LIST_BOX.
	for (const Document::Collection &collection : lists) {
		const std::string token = document->kind_token(collection.spec.kind);
		if (token == "items.item" || token == "list_box" || token == "datasource")
			TEST_EXPECT(collection.spec.applies == Applicability::Ignored);
		if (token == "appearance" || token == "action" || token == "window")
			TEST_EXPECT(collection.spec.applies == Applicability::Reads);
	}
	TEST_EXPECT(document->ancestors(exit) == std::vector<NodeAddress>({screen_address, root}));
	TEST_EXPECT(document->record_path(exit) == "STARTUP/MAIN/EXIT" && document->locator(exit) == "0/window:0/window:1");
	TEST_EXPECT(document->address_at("0/window:0/window:1") == exit && document->address_at("0/window:1") == NodeAddress());
	const NodeAddress exit_row = child_of(*document, exit, "appearance", 2);
	TEST_EXPECT(document->record_path(exit_row) == "STARTUP/MAIN/EXIT/Appearance 3" &&
	            document->locator(exit_row) == "0/window:0/window:1/appearance:2" &&
	            document->address_at(document->locator(exit_row)) == exit_row);
	// A field as it applies: a static reads STRING; a generic window does not.
	FieldSchema string_value;
	for (const FieldSchema &field : document->fields(kWindow))
		if (field.id == "string.value") string_value = field;
	TEST_EXPECT(document->field_on(title, string_value).applies == Applicability::Reads &&
	            document->field_on(root, string_value).applies == Applicability::Ignored);
	const std::string original = document->serialize().text;
	Diagnostic error;
	// Indent: EXIT into TITLE (B1: the position is an index among TITLE's children).
	Edit move = op(EditOperation::Move, exit, title.child, 0);
	TEST_EXPECT(document->apply(move, error));
	TEST_EXPECT(depth_of(*document, exit) == 2 && document->window_index(exit) == 2);
	TEST_EXPECT(document->ancestors(exit) == std::vector<NodeAddress>({screen_address, root, title}));
	TEST_EXPECT(window_of(*document, title)->children.size() == 1 && window_of(*document, title)->children[0].name == "EXIT");
	// Outdent: back into the root at index 0, before TITLE; then out to the screen, a
	// second root, and back.
	move.parent = root.child;
	TEST_EXPECT(document->apply(move, error));
	TEST_EXPECT(window_names(*document, screen()) == std::vector<std::string>({"MAIN", "EXIT", "TITLE"}));
	move.parent = screen().id;
	move.position = 1;
	TEST_EXPECT(document->apply(move, error) && depth_of(*document, exit) == 0 &&
	            document->collections_of(screen_address)[0].ids == std::vector<NodeId>({root.child, exit.child}));
	TEST_EXPECT(document->identities_match());
	document->undo();
	// Refused: into itself or its own window, the only root into a window, out of its
	// screen, a wrong kind, a stale identity.
	move.parent = exit.child;
	move.position = 0;
	TEST_EXPECT(!document->apply(move, error) && error.code() == "document.collection");
	TEST_EXPECT(!document->apply(op(EditOperation::Move, root, title.child, 0), error));
	TEST_EXPECT(document->apply(op(EditOperation::Add, {0, kScreen, 0}), error));
	const NodeId other_root = document->window_at(*document->rows()[1], 0);
	move.address = exit;
	move.parent = other_root;
	TEST_EXPECT(!document->apply(move, error) && error.message.find("within its own") != std::string::npos);
	document->undo(); // the second screen
	move.parent = 0;
	move.address = {screen().id, kScreen, exit.child};
	TEST_EXPECT(!document->apply(move, error) && error.code() == "document.selection");
	move.address = {screen().id, kWindow, 99999};
	TEST_EXPECT(!document->apply(move, error) && error.code() == "document.selection");
	document->undo(); // the outdent
	document->undo(); // the indent
	TEST_EXPECT(document->serialize().text == original && !document->dirty());
	// An unset edge is left out: Clear unsets it and keeps the value, Write writes that
	// value again (a Set of the same value leaves it out, a Set of another writes it).
	TEST_EXPECT(document->present(exit, "position.left"));
	Edit clear = op(EditOperation::Clear, exit);
	clear.field = "position.left";
	TEST_EXPECT(document->apply(clear, error) && !document->present(exit, "position.left"));
	mnu::Document cleared;
	std::string message;
	const std::string text = document->serialize().text;
	TEST_EXPECT(mnu::parse(reinterpret_cast<const uint8_t *>(text.data()), text.size(), cleared, message));
	TEST_EXPECT(cleared.screens.size() == 1 && cleared.screens[0].roots.size() == 1 &&
	            cleared.screens[0].roots[0].children.size() == 2);
	TEST_EXPECT(cleared.screens[0].roots[0].children[1].name == "EXIT" &&
	            !cleared.screens[0].roots[0].children[1].position.has_left &&
	            cleared.screens[0].roots[0].children[1].position.has_top);
	const int64_t latent = window_of(*document, exit)->position.left;
	TEST_EXPECT(text_of(*document, exit, "position.left") == std::to_string(latent));
	clear.field = "name";
	TEST_EXPECT(!document->apply(clear, error) && error.message == "This field is always written.");
	const uint64_t left_out = document->revision();
	TEST_EXPECT(document->apply(set(exit, "position.left", latent), error) && !document->present(exit, "position.left") &&
	            document->revision() == left_out); // the value it holds: no step
	Edit write = op(EditOperation::Write, exit);
	write.field = "position.left";
	TEST_EXPECT(document->apply(write, error) && document->present(exit, "position.left"));
	TEST_EXPECT(document->serialize().text == original && document->revision() != left_out);
	document->undo(); // the Write: one step
	TEST_EXPECT(!document->present(exit, "position.left") && document->revision() == left_out);
	document->undo(); // the Clear
	TEST_EXPECT(document->serialize().text == original && document->present(exit, "position.left") && !document->dirty());
	// GROUP, never authored on the blank menu's EXIT, written with the value it reads.
	TEST_EXPECT(!document->present(exit, "group") && original.find("GROUP") == std::string::npos);
	write.field = "group";
	TEST_EXPECT(document->apply(write, error) && document->present(exit, "group"));
	TEST_EXPECT(document->serialize().text.find("GROUP") != std::string::npos);
	const uint64_t grouped = document->revision();
	TEST_EXPECT(document->apply(write, error) && document->revision() == grouped); // already written: no step
	document->undo();
	TEST_EXPECT(document->serialize().text == original && !document->dirty());
	write.field = "name";
	TEST_EXPECT(!document->apply(write, error) && error.message == "This field is always written.");
	// The session repairs the selection: a removed window gives way to its owner.
	EditorRequest select = request::select_record(document->path(), exit);
	editor_test::handle_to_end(session, select);
	EditorRequest remove = request::edit_record(document->path(), op(EditOperation::Remove, exit));
	editor_test::handle_to_end(session, remove);
	const SessionView &view = session.view();
	TEST_EXPECT(view.documents.selection.primary == root &&
			view.documents.selection.records == std::vector<NodeAddress>{ root });
	editor_test::handle_to_end(session, request::undo(document->path()));
	TEST_EXPECT(view.documents.selection.primary == root && window_of(*document, exit));
	// A window added inside TITLE is selected.
	EditorRequest add = request::edit_record(
			document->path(), op(EditOperation::Add, { 0, kWindow, 0 }, title.child));
	editor_test::handle_to_end(session, add);
	TEST_EXPECT(session.last_edit_ok() &&
			view.documents.selection.primary.child == document->last_added() &&
			document->ancestors(view.documents.selection.primary).back() == title);
	editor_test::handle_to_end(session, request::undo(document->path()));
	// A drag: edits sharing a gesture are one undo step, and the session validates once,
	// when the gesture ends, however many samples it took.
	const size_t passes = session.validation_stats().passes;
	const uint64_t gesture = next_edit_gesture();
	for (const int64_t left : {130, 140, 150}) {
		EditorRequest drag = request::edit_record(document->path(),
				std::vector<Edit>{ set(title, "position.left", left),
						set(title, "position.right", left + 200) });
		for (Edit &edit : drag.edits) edit.gesture = gesture;
		editor_test::handle_to_end(session, drag);
		TEST_EXPECT(session.last_edit_ok());
	}
	TEST_EXPECT(session.validation_stats().passes == passes && text_of(*document, title, "position.left") == "150");
	editor_test::handle_to_end(session, request::end_edit(document->path()));
	TEST_EXPECT(session.validation_stats().passes == passes + 1);
	editor_test::handle_to_end(session, request::undo(document->path()));
	TEST_EXPECT(document->serialize().text == original && !document->dirty());
	return 0;
}

// --- Every list, at any depth (ADR 0046 S9h) ----------------------------------------------

// A menu laid out the way the shipped ones are (jo_main.mnu): the text table and the
// cursor inside the root window, a button whose states mix a custom hook, a color, a
// typeless placeholder and a sprite-sheet image, and a virtual plus a character hotkey.
const char *const kShippedShape =
        "<SCREEN>\r\n"
        "\t<NAME>OPTIONS</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
        "\t\t<APPEARANCE type=\"custom\" state=\"default\"></APPEARANCE>\r\n"
        "\t\t<POSITION><LEFT>0</LEFT><TOP>75</TOP><RIGHT>800</RIGHT><BOTTOM>525</BOTTOM></POSITION>\r\n"
        "\t\t<TEXT_RSRC>menutxt.BIN</TEXT_RSRC>\r\n"
        "\t\t<CURSOR><FILE>newarow1.tga</FILE><FLAGS>STANDARD_TRANSPARENT</FLAGS></CURSOR>\r\n"
        "\t\t<WINDOW type=\"button\" name=\"BACK\">\r\n"
        "\t\t\t<APPEARANCE type=\"custom\" state=\"default\"></APPEARANCE>\r\n"
        "\t\t\t<APPEARANCE type=\"color\" state=\"mouseover\">FF00FF00</APPEARANCE>\r\n"
        "\t\t\t<APPEARANCE state=\"selected\"></APPEARANCE>\r\n"
        "\t\t\t<APPEARANCE type=\"image\" state=\"disabled\" map_state=\"2\" height=\"20\">back.tga</APPEARANCE>\r\n"
        "\t\t\t<POSITION><LEFT>10</LEFT><TOP>10</TOP></POSITION>\r\n"
        "\t\t\t<HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>\r\n"
        "\t\t\t<HOTKEY>B</HOTKEY>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"spinlist\" name=\"SPIN\">\r\n"
        "\t\t\t<POSITION><LEFT>10</LEFT><TOP>40</TOP></POSITION>\r\n"
        "\t\t\t<ITEMS><ITEM value=\"1\">One</ITEM></ITEMS>\r\n"
        "\t\t\t<SPINUP><APPEARANCE state=\"default\"></APPEARANCE></SPINUP>\r\n"
        "\t\t\t<SPINDOWN><APPEARANCE state=\"default\"></APPEARANCE></SPINDOWN>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"table\" name=\"RESULTS\">\r\n"
        "\t\t\t<POSITION><LEFT>10</LEFT><TOP>80</TOP></POSITION>\r\n"
        "\t\t\t<ITEMS MULTISELECT><ROW><ITEM column=\"0\">A</ITEM><ITEM column=\"1\" type=\"BITMAP\">x.tga</ITEM></ROW></ITEMS>\r\n"
        "\t\t\t<COLUMN count=\"2\"><HEADER column=\"0\" width=\"50\">Name</HEADER><HEADER column=\"1\">Kills</HEADER></COLUMN>\r\n"
        "\t\t\t<WINDOW type=\"static\" name=\"CAPTION\"><POSITION><LEFT>0</LEFT></POSITION></WINDOW>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t</WINDOW>\r\n"
        "</SCREEN>\r\n";

// Every list of BACK: added to, duplicated, moved, reparented into MAIN and back and
// removed, each undone, the identities in the native tree's shape after every step (a
// part moved to a window that has none, refused where one is); a leaf added into an
// absent block authors it; the parts' toggles.
int every_list() {
	editor_test::TempProjectDir dir("opennova_menu_every_list_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path) && document.identities_match());
	NodeAddress back, main, spin, results;
	TEST_EXPECT(find_definition(AssetGraph(), document, "BACK", back) &&
			find_definition(AssetGraph(), document, "MAIN", main) &&
			find_definition(AssetGraph(), document, "SPIN", spin) &&
			find_definition(AssetGraph(), document, "RESULTS", results));
	const std::string original = document.serialize().text;
	Diagnostic error;
	for (const Document::Collection &collection : document.collections_of(back)) {
		const std::string token = document.kind_token(collection.spec.kind);
		const NodeKind kind = collection.spec.kind;
		const bool part = token == "list_box" || token == "spinup" || token == "spindown" || token == "scrollbar";
		auto step = [&](const Edit &edit) { return document.apply(edit, error) && document.identities_match(); };
		TEST_EXPECT(step(op(EditOperation::Add, {back.row, kind, 0}, back.child)));
		const NodeAddress first = child_of(document, back, token.c_str());
		TEST_EXPECT(first.child && document.address_at(document.locator(first)) == first);
		if (part) {
			// One part at most; the toggle leaves it out and keeps it; Remove takes it.
			TEST_EXPECT(!document.apply(op(EditOperation::Duplicate, first, 0, 1), error));
			const std::string written = document.serialize().text;
			TEST_EXPECT(step(set(back, token.c_str(), int64_t(0))) && document.serialize().text != written &&
			            !document.present(first, std::string()) && text_of(document, back, token.c_str()) == "0");
			TEST_EXPECT(step(set(back, token.c_str(), int64_t(1))) && document.serialize().text == written);
			// Moved to a window that has none (MAIN) and back; never into one that has one.
			TEST_EXPECT(step(op(EditOperation::Move, first, main.child, 0)) && child_of(document, main, token.c_str()) == first &&
			            !child_of(document, back, token.c_str()).child && document.serialize().text != written);
			if (token == "spinup" || token == "spindown")
				TEST_EXPECT(!document.apply(op(EditOperation::Move, first, spin.child, 0), error) &&
				            error.message.find("already holds") != std::string::npos);
			TEST_EXPECT(step(op(EditOperation::Move, first, back.child, 0)) && child_of(document, back, token.c_str()) == first &&
			            document.serialize().text == written);
			TEST_EXPECT(step(op(EditOperation::Remove, first)) && !child_of(document, back, token.c_str()).child);
		} else {
			TEST_EXPECT(step(op(EditOperation::Duplicate, first, 0, 1)));
			const NodeAddress copy{back.row, kind, document.last_added()};
			TEST_EXPECT(step(op(EditOperation::Move, first, 0, SIZE_MAX)));
			std::vector<NodeId> ids;
			for (const Document::Collection &now : document.collections_of(back))
				if (now.spec.kind == kind) ids = now.ids;
			TEST_EXPECT(!ids.empty() && ids.front() == copy.child && ids.back() == first.child);
			// Reparented into MAIN and back into BACK.
			TEST_EXPECT(step(op(EditOperation::Move, copy, main.child, 0)) && document.ancestors(copy).back() == main);
			TEST_EXPECT(step(op(EditOperation::Move, copy, back.child, 0)) && document.ancestors(copy).back() == back);
			TEST_EXPECT(step(op(EditOperation::Remove, first)) && document.address_at(document.locator(copy)) == copy);
		}
		TEST_EXPECT(document.serialize().ok());
		while (document.can_undo()) document.undo();
		TEST_EXPECT(document.serialize().text == original && document.identities_match());
	}
	// A leaf reparented: BACK's first HOTKEY into MAIN, then back.
	const NodeAddress hotkey = child_of(document, back, "hotkey");
	TEST_EXPECT(document.apply(op(EditOperation::Move, hotkey, main.child, 0), error) && document.identities_match());
	TEST_EXPECT(document.ancestors(hotkey).back() == main && text_of(document, hotkey, "value") == "VK_ESCAPE" &&
	            text_of(document, hotkey, "virtual") == "1");
	TEST_EXPECT(window_of(document, main)->hotkeys.size() == 1 && window_of(document, back)->hotkeys.size() == 1);
	// A leaf goes only into a collection of its own kind: a HOTKEY is not an ACTION, a
	// table cell only into a row.
	TEST_EXPECT(!document.apply(op(EditOperation::Add, {back.row, menu_kind("item"), 0}, back.child), error));
	document.undo();
	TEST_EXPECT(document.serialize().text == original);
	// A leaf added into an absent block authors it: an ITEM on BACK writes an ITEMS.
	TEST_EXPECT(!window_of(document, back)->items.present);
	TEST_EXPECT(document.apply(op(EditOperation::Add, {back.row, menu_kind("items.item"), 0}, back.child), error));
	const NodeAddress item{back.row, menu_kind("items.item"), document.last_added()};
	TEST_EXPECT(window_of(document, back)->items.present && document.present(item, std::string()));
	// Where the new record sits is the core's index of the committed screen (Document::path_in, S13
	// D8): the window's path, then the ITEMS row's list and index.
	const Document::RecordPath item_path = document.path_in(*document.row(item.row), item.child);
	const Document::RecordPath back_path = document.path_in(*document.row(back.row), back.child);
	TEST_EXPECT(!back_path.empty() && item_path.size() == back_path.size() + 1 &&
	            item_path[back_path.size()].index == 0);
	// ... which a button does not read (flagged, still written).
	FieldSchema text;
	for (const FieldSchema &field : document.fields(item.kind))
		if (field.id == "text") text = field;
	TEST_EXPECT(document.field_on(item, text).applies == Applicability::Ignored);
	TEST_EXPECT(document.apply(set(back, "items", int64_t(0)), error) && !document.present(item, std::string()) &&
	            window_of(document, back)->items.items.size() == 1);
	document.undo();
	document.undo();
	TEST_EXPECT(document.serialize().text == original);
	// The spin list's arrows are parts; a table's cells sit in its rows.
	const NodeAddress up = child_of(document, spin, "spinup");
	TEST_EXPECT(up.child && window_of(document, up) && window_of(document, up)->type == mnu::WindowType::Button &&
	            document.window_index(up) == -1 && depth_of(document, up) == 2);
	const NodeAddress row = child_of(document, results, "items.row");
	const NodeAddress cell = child_of(document, row, "item", 1);
	TEST_EXPECT(cell.child && text_of(document, cell, "text") == "x.tga" && document.record_path(cell) ==
	                                                                             "OPTIONS/MAIN/RESULTS/Row 1/Cell 2");
	FieldSchema cell_text;
	for (const FieldSchema &field : document.fields(cell.kind))
		if (field.id == "text") cell_text = field;
	TEST_EXPECT(document.field_on(cell, cell_text).reference == ReferenceKind::MenuTexture &&
	            document.field_on(cell, cell_text).applies == Applicability::Reads);
	// A table cell edit survives a save and a reload (the rows are the ITEMS as written).
	TEST_EXPECT(document.apply(set(cell, "text", std::string("y.tga")), error) && document.save(error));
	MnuDocument reloaded;
	TEST_EXPECT(load(reloaded, path) && reloaded.serialize().text == document.serialize().text);
	NodeAddress results_again;
	TEST_EXPECT(find_definition(AssetGraph(), reloaded, "RESULTS", results_again));
	TEST_EXPECT(text_of(reloaded, child_of(reloaded, child_of(reloaded, results_again, "items.row"), "item", 1), "text") ==
	            "y.tga");
	return 0;
}

// Every list's default record, the window's and the nested ones (a row's cell, an
// element's attribute and element), survives a save and a reload.
int defaults_survive() {
	editor_test::TempProjectDir dir("opennova_menu_defaults_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	NodeAddress back;
	TEST_EXPECT(find_definition(AssetGraph(), document, "BACK", back));
	Diagnostic error;
	for (const Document::Collection &collection : document.collections_of(back))
		if (collection.ids.empty())
			TEST_EXPECT(document.apply(op(EditOperation::Add, {back.row, collection.spec.kind, 0}, back.child), error));
	const NodeAddress row = child_of(document, back, "items.row");
	const NodeAddress element = child_of(document, back, "element");
	TEST_EXPECT(document.apply(op(EditOperation::Add, {back.row, menu_kind("item"), 0}, row.child), error));
	TEST_EXPECT(document.apply(op(EditOperation::Add, {back.row, menu_kind("attribute"), 0}, element.child), error));
	TEST_EXPECT(document.apply(op(EditOperation::Add, {back.row, menu_kind("element"), 0}, element.child), error));
	// The defaults: a new ACTION is POP_SCREEN, a SOUND is MOUSEIN over MOUSE_OVER, a window
	// is named uniquely.
	TEST_EXPECT(text_of(document, child_of(document, back, "action"), "type") == "POP_SCREEN");
	TEST_EXPECT(text_of(document, child_of(document, back, "sound"), "trigger") == "MOUSE_OVER");
	TEST_EXPECT(text_of(document, child_of(document, back, "window"), "name") == "WINDOW1");
	TEST_EXPECT(text_of(document, child_of(document, back, "attribute"), "name") == "PLAYERLIST" &&
	            text_of(document, child_of(document, element, "attribute"), "name") == "NAME");
	// A window keeps only PLAYERLIST and SERVERLIST, read by presence, and its known extras.
	TEST_EXPECT(!document.apply(set(child_of(document, back, "attribute"), "name", std::string("NAME")), error));
	TEST_EXPECT(!document.apply(set(child_of(document, back, "attribute"), "value", std::string("1")), error));
	TEST_EXPECT(!document.apply(set(element, "tag", std::string("NOT_READ")), error));
	TEST_EXPECT(document.apply(set(child_of(document, element, "element"), "tag", std::string("ANY_NAME")), error));
	TEST_EXPECT(document.identities_match() && document.serialize().ok() && document.save(error));
	MnuDocument reloaded;
	TEST_EXPECT(load(reloaded, path) && reloaded.issues().empty() && reloaded.identities_match());
	TEST_EXPECT(reloaded.serialize().text == document.serialize().text && record_count(reloaded) == record_count(document));
	return 0;
}

// window_index numbers windows as the frame compiler does (the spin arrows after them).
int window_index_matches_the_compiler() {
	editor_test::TempProjectDir dir("opennova_menu_index_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	const mnu::Document native = document.native();
	opennova::menu::MenuFrameCompiler compiler;
	compiler.configure(&native.screens[0]);
	size_t windows = 0;
	const Node &screen = *document.rows()[0];
	document.walk_records(screen, [&](const NodeAddress &record, const Document::Placement &) {
		if (record.kind != kWindow) return true;
		const int index = document.window_index(record);
		if (index < 0 || compiler.widget_name(index) != window_of(document, record)->name ||
		    document.window_at(screen, size_t(index)) != record.child)
			windows = SIZE_MAX / 2;
		++windows;
		return true;
	});
	// The spin arrows are widgets past the document's index space, not counted here.
	TEST_EXPECT(windows == 5 && compiler.widget_count() == 5);
	TEST_EXPECT(document.record_at(screen, 1, 1, 1) == child_of(document, {screen.id, kWindow, document.window_at(screen, 1)},
	                                                                      "hotkey", 1));
	return 0;
}

// Windows copied and pasted: across screens, into another file, names made unique;
// anything else cannot be copied; Cut is a copy and a Remove; windows of two screens copied and
// cut together (the polish).
int copy_and_paste() {
	editor_test::TempProjectDir dir("opennova_menu_clipboard_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Clip"));
	editor_test::create_missing_files(session);
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	auto *document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document);
	const SessionView &view = session.view();
	NodeAddress title, exit;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "TITLE", title) &&
			find_definition(AssetGraph(), *document, "EXIT", exit));
	auto select = [&](const NodeAddress &address, SelectMode mode) {
		editor_test::handle_to_end(session, request::select_record(document->path(), address, mode));
	};
	auto paste_into = [&](Document &target, NodeId parent, size_t position) {
		editor_test::handle_to_end(session, request::paste(target.path(), PasteAt{target.rows()[0]->id, parent, position}));
		return session.last_edit_ok();
	};
	// A row cannot be copied as windows.
	select(child_of(*document, exit, "appearance"), SelectMode::Replace);
	editor_test::handle_to_end(session, request::copy(document->path()));
	TEST_EXPECT(!session.last_edit_ok() && view.documents.clipboard.empty());
	// TITLE and EXIT, in document order whatever the selection order.
	select(exit, SelectMode::Replace);
	select(title, SelectMode::Add);
	editor_test::handle_to_end(session, request::copy(document->path()));
	TEST_EXPECT(
			session.last_edit_ok() && view.documents.clipboard.compare(0, 3, "\xEF\xBB\xBF") == 0);
	// Into a second screen's root: the names are free there.
	Diagnostic error;
	TEST_EXPECT(document->apply(op(EditOperation::Add, {0, kScreen, 0}), error));
	const Node *second = document->rows()[1].get();
	const NodeId second_root = document->window_at(*second, 0);
	editor_test::handle_to_end(session, request::paste(document->path(), PasteAt{second->id, second_root, 0}));
	TEST_EXPECT(session.last_edit_ok() && document->identities_match());
	second = document->rows()[1].get();
	TEST_EXPECT(window_names(*document, *second) == std::vector<std::string>({"MAIN", "TITLE", "EXIT"}));
	TEST_EXPECT(view.documents.selection.records.size() == 2 && window_of(*document, view.documents.selection.records[0])->name == "TITLE");
	// Again into the first screen: every name taken, so each is made unique.
	TEST_EXPECT(paste_into(*document, 0, SIZE_MAX));
	TEST_EXPECT(window_names(*document, *document->rows()[0]) ==
	            std::vector<std::string>({"MAIN", "TITLE", "EXIT", "TITLE2", "EXIT2"}));
	TEST_EXPECT(document->collections_of({document->rows()[0]->id, kScreen, 0})[0].ids.size() == 3);
	TEST_EXPECT(window_of(*document, view.documents.selection.primary)->name == "TITLE2" && depth_of(*document, view.documents.selection.primary) == 0);
	editor_test::handle_to_end(session, request::undo(document->path()));
	// Into another menu file.
	editor_test::handle_to_end(session, request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
	auto *extra = dynamic_cast<MnuDocument *>(session.document_for("extra.mnu"));
	TEST_EXPECT(extra);
	const NodeId extra_root = extra->window_at(*extra->rows()[0], 0);
	TEST_EXPECT(paste_into(*extra, extra_root, SIZE_MAX));
	NodeAddress pasted;
	TEST_EXPECT(find_definition(AssetGraph(), *extra, "EXIT", pasted) &&
			extra->ancestors(pasted).back().child == extra_root &&
			text_of(*extra, pasted, "string.value") == "Exit" && extra->identities_match());
	// Cut: a copy, then one Remove of the selection.
	select(title, SelectMode::Replace);
	editor_test::handle_to_end(session, request::cut(document->path()));
	TEST_EXPECT(session.last_edit_ok() && !window_of(*document, title));
	editor_test::handle_to_end(session, request::undo(document->path()));
	TEST_EXPECT(window_of(*document, title) != nullptr);
	// Windows of two screens (the polish; S13 D7 refused them): the second screen's TITLE and the
	// first's EXIT copied together, in the file's order whatever the selection's (EXIT, then TITLE),
	// and pasted by the tree's rule in one step: right after the primary of the menu pasted into.
	NodeAddress second_title;
	document->walk_records(*document->rows()[1], [&](const NodeAddress &record, const Document::Placement &) {
		if (record.kind == kWindow && window_of(*document, record)->name == "TITLE") second_title = record;
		return !second_title.child;
	});
	TEST_EXPECT(second_title.child != 0 && second_title.row != exit.row);
	select(second_title, SelectMode::Replace);
	select(exit, SelectMode::Add);
	editor_test::handle_to_end(session, request::copy(document->path()));
	TEST_EXPECT(session.last_edit_ok());
	TEST_EXPECT(find_definition(AssetGraph(), *extra, "TITLE", pasted));
	editor_test::handle_to_end(session, request::select_record(extra->path(), pasted, SelectMode::Replace));
	const std::string extra_before = extra->serialize().text;
	editor_test::handle_to_end(session, request::paste(extra->path()));
	TEST_EXPECT(session.last_edit_ok() && extra->identities_match());
	TEST_EXPECT(window_names(*extra, *extra->rows()[0]) == std::vector<std::string>({"MAIN", "TITLE", "EXIT2", "TITLE2", "EXIT"}));
	editor_test::handle_to_end(session, request::undo(extra->path()));
	TEST_EXPECT(extra->serialize().text == extra_before);
	// Cut across the two screens: one step removing both, undone to the bytes.
	const std::string two_screens = document->serialize().text;
	select(second_title, SelectMode::Replace);
	select(exit, SelectMode::Add);
	editor_test::handle_to_end(session, request::cut(document->path()));
	TEST_EXPECT(session.last_edit_ok() && !window_of(*document, exit) && !window_of(*document, second_title));
	editor_test::handle_to_end(session, request::undo(document->path()));
	TEST_EXPECT(document->serialize().text == two_screens);
	// A paste that is not a menu's clipboard is refused.
	Edit foreign = op(EditOperation::Paste, {document->rows()[0]->id, kWindow, 0}, document->window_at(*document->rows()[0], 0));
	foreign.value = std::string("key=value");
	TEST_EXPECT(!document->apply(foreign, error));
	return 0;
}

// The session's Duplicate (S9k2): each selected window right after itself in one step
// (a window inside another selected one goes with that one), the copies selected; a screen
// on its own after itself; nothing selected is refused.
int duplicate_selection() {
	editor_test::TempProjectDir dir("opennova_menu_duplicate_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Dup"));
	editor_test::create_missing_files(session);
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	auto *document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document);
	const SessionView &view = session.view();
	const std::string original = document->serialize().text;
	NodeAddress main, title, exit;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "MAIN", main) &&
			find_definition(AssetGraph(), *document, "TITLE", title) &&
			find_definition(AssetGraph(), *document, "EXIT", exit));
	auto select = [&](const NodeAddress &address, SelectMode mode) {
		EditorRequest request = request::select_record(document->path(), address);
		request.mode = mode;
		editor_test::handle_to_end(session, request);
	};
	auto duplicate = [&] {
		editor_test::handle_to_end(session, request::duplicate(document->path()));
		return session.last_edit_ok() && document->identities_match();
	};
	auto undo = [&] {
		editor_test::handle_to_end(session, request::undo(document->path()));
		return document->serialize().text == original && !document->dirty();
	};
	select(NodeAddress(), SelectMode::Replace);
	TEST_EXPECT(!duplicate() && document->serialize().text == original);
	// EXIT then TITLE: each copy after its own original, the copies selected in the order their
	// originals were, the primary's copy the primary.
	select(exit, SelectMode::Replace);
	select(title, SelectMode::Add);
	TEST_EXPECT(duplicate());
	const Node *screen = document->rows()[0].get();
	TEST_EXPECT(window_names(*document, *screen) == std::vector<std::string>({"MAIN", "TITLE", "TITLE2", "EXIT", "EXIT2"}));
	TEST_EXPECT(view.documents.selection.records.size() == 2 && window_of(*document, view.documents.selection.records[0])->name == "EXIT2" &&
	            window_of(*document, view.documents.selection.records[1])->name == "TITLE2" &&
	            window_of(*document, view.documents.selection.primary)->name == "TITLE2");
	TEST_EXPECT(undo());
	// MAIN with TITLE inside it: MAIN once, with everything it holds, after itself among the roots.
	select(main, SelectMode::Replace);
	select(title, SelectMode::Add);
	TEST_EXPECT(duplicate());
	screen = document->rows()[0].get();
	TEST_EXPECT(window_names(*document, *screen) ==
	            std::vector<std::string>({"MAIN", "TITLE", "EXIT", "MAIN2", "TITLE2", "EXIT2"}));
	TEST_EXPECT(view.documents.selection.records.size() == 1 &&
			depth_of(*document, view.documents.selection.primary) == 0);
	TEST_EXPECT(undo());
	// Two of EXIT's four APPEARANCE rows (the first and the third): each copy after its own.
	select(child_of(*document, exit, "appearance", 0), SelectMode::Replace);
	select(child_of(*document, exit, "appearance", 2), SelectMode::Add);
	TEST_EXPECT(duplicate());
	std::vector<std::string> states;
	for (const Document::Collection &collection : document->collections_of(exit))
		if (std::string(document->kind_token(collection.spec.kind)) == "appearance")
			for (const NodeId id : collection.ids) states.push_back(text_of(*document, {exit.row, collection.spec.kind, id}, "state"));
	TEST_EXPECT(states.size() == 6 && states[0] == states[1] && states[3] == states[4] && states[1] != states[2] &&
	            states[4] != states[5]);
	TEST_EXPECT(undo());
	// The screen: after itself among the screens, under a name of its own.
	select({screen->id, kScreen, 0}, SelectMode::Replace);
	TEST_EXPECT(duplicate());
	TEST_EXPECT(document->rows().size() == 2 && document->rows()[1]->name() == "STARTUP2");
	TEST_EXPECT(undo());
	return 0;
}

// A code-page menu's windows pasted into a Unicode one keep their characters.
int copy_between_encodings() {
	editor_test::TempProjectDir dir("opennova_menu_clipboard_encoding_test");
	const std::string code_page = dir.file("a.mnu"), unicode = dir.file("b.mnu");
	TEST_EXPECT(editor_test::write_text(code_page, "<SCREEN><NAME>A</NAME><WINDOW type=\"static\" name=\"CAFE\">"
	                                               "<POSITION><LEFT>0</LEFT></POSITION><STRING>Caf\xE9</STRING></WINDOW></SCREEN>"));
	TEST_EXPECT(editor_test::write_text(unicode, "\xEF\xBB\xBF<SCREEN><NAME>B</NAME><WINDOW type=\"window\" name=\"MAIN\">"
	                                             "<POSITION><LEFT>0</LEFT></POSITION></WINDOW></SCREEN>"));
	MnuDocument from, to;
	TEST_EXPECT(load(from, code_page) && load(to, unicode));
	NodeAddress cafe, main;
	TEST_EXPECT(find_definition(AssetGraph(), from, "CAFE", cafe) &&
			find_definition(AssetGraph(), to, "MAIN", main));
	const std::string payload = from.copy({cafe});
	TEST_EXPECT(!payload.empty() && payload.find("Caf\xC3\xA9") != std::string::npos);
	Edit paste = op(EditOperation::Paste, {main.row, kWindow, 0}, main.child, 0);
	paste.value = payload;
	Diagnostic error;
	TEST_EXPECT(to.apply(paste, error));
	NodeAddress copied;
	TEST_EXPECT(find_definition(AssetGraph(), to, "CAFE", copied) &&
	            text_of(to, copied, "string.value") == "Caf\xC3\xA9");
	// And back into the code-page menu's roots, as its own byte (its name taken there).
	const std::string back = to.copy({copied});
	Edit again = op(EditOperation::Paste, {from.rows()[0]->id, kWindow, 0}, 0, 0);
	again.value = back;
	TEST_EXPECT(from.apply(again, error));
	NodeAddress returned;
	TEST_EXPECT(find_definition(AssetGraph(), from, "CAFE2", returned) &&
	            text_of(from, returned, "string.value") == "Caf\xC3\xA9" &&
	            window_of(from, returned)->string_data.value == "Caf\xE9" &&
	            depth_of(from, returned) == 0 && from.window_index(returned) == 0);
	// A character the code page cannot hold refuses the paste.
	Edit foreign = again;
	foreign.value = std::string("\xEF\xBB\xBF<SCREEN><NAME>X</NAME><WINDOW type=\"static\" name=\"W\"><POSITION><LEFT>0"
	                            "</LEFT></POSITION><STRING>\xE2\x82\xAC\xE4\xB8\xAD</STRING></WINDOW></SCREEN>");
	TEST_EXPECT(!from.apply(foreign, error) && error.message.find("code page") != std::string::npos);
	return 0;
}

// A menu's texts at the widget (the found-bugs round): a code-page menu's model holds Windows-1252
// bytes, which a read gives as UTF-8 (never the raw byte a widget would show as another character)
// and a set takes back in the code page, a character it has no byte for refused; a field's width
// counts characters in either encoding, as the game's narrowed buffers count them (a Unicode menu's
// text of two-byte characters past half the width in bytes is taken, and refused at the width).
int texts_in_their_code_page() {
	editor_test::TempProjectDir dir("opennova_menu_code_page_test");
	const std::string code_page = dir.file("a.mnu"), unicode = dir.file("b.mnu");
	TEST_EXPECT(editor_test::write_text(code_page, "<SCREEN><NAME>A</NAME><WINDOW type=\"static\" name=\"CAFE\">"
	                                               "<POSITION><LEFT>0</LEFT></POSITION><STRING>Caf\xE9</STRING></WINDOW></SCREEN>"));
	TEST_EXPECT(editor_test::write_text(unicode, "\xEF\xBB\xBF<SCREEN><NAME>B</NAME><WINDOW type=\"static\" name=\"MAIN\">"
	                                             "<POSITION><LEFT>0</LEFT></POSITION><STRING>Caf\xC3\xA9</STRING></WINDOW></SCREEN>"));
	MnuDocument cp, wide;
	TEST_EXPECT(load(cp, code_page) && load(wide, unicode));
	NodeAddress cafe, main;
	TEST_EXPECT(find_definition(AssetGraph(), cp, "CAFE", cafe) && find_definition(AssetGraph(), wide, "MAIN", main));
	if (!cafe.row || !main.row) return 1;
	TEST_EXPECT(text_of(cp, cafe, "string.value") == "Caf\xC3\xA9" && window_of(cp, cafe)->string_data.value == "Caf\xE9");
	TEST_EXPECT(text_of(wide, main, "string.value") == "Caf\xC3\xA9");
	Diagnostic error;
	TEST_EXPECT(cp.apply(set(cafe, "string.value", std::string("Na\xC3\xAFve")), error) &&
	            window_of(cp, cafe)->string_data.value == "Na\xEFve" && text_of(cp, cafe, "string.value") == "Na\xC3\xAFve");
	TEST_EXPECT(!cp.apply(set(cafe, "string.value", std::string("\xE2\x9C\x93 done")), error) &&
	            error.message.find("Windows-1252") != std::string::npos);
	// The window NAME is 128 bytes with its terminator: 127 characters in either encoding.
	std::string e_acute;
	for (int i = 0; i < 127; ++i) e_acute += "\xC3\xA9";
	TEST_EXPECT(cp.apply(set(cafe, "name", e_acute), error) && window_of(cp, cafe)->name.size() == 127);
	TEST_EXPECT(!cp.apply(set(cafe, "name", e_acute + "\xC3\xA9"), error) && error.message == "The text is too long.");
	TEST_EXPECT(wide.apply(set(main, "name", e_acute), error) && window_of(wide, main)->name.size() == 254);
	TEST_EXPECT(!wide.apply(set(main, "name", e_acute + "\xC3\xA9"), error));
	// The box over a menu's text counts characters too.
	const FieldSchema *name = nullptr;
	for (const FieldSchema &field : MnuDocument::schema(kWindow))
		if (field.id == "name") name = &field;
	TEST_EXPECT(name && name->code_page && name->width == 128);
	return 0;
}

// A window selected with one that holds it comes with that one; a window a part holds
// copies; the windows a part holds count among the names a new window avoids.
int copy_selection_shapes() {
	editor_test::TempProjectDir dir("opennova_menu_clipboard_shapes_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	NodeAddress results, caption, spin;
	TEST_EXPECT(find_definition(AssetGraph(), document, "RESULTS", results) &&
			find_definition(AssetGraph(), document, "CAPTION", caption) &&
			find_definition(AssetGraph(), document, "SPIN", spin));
	const std::string alone = document.copy({results});
	TEST_EXPECT(!alone.empty() && document.copy({results, caption}) == alone && document.copy({caption, results}) == alone);
	Diagnostic error;
	const NodeAddress up = child_of(document, spin, "spinup");
	TEST_EXPECT(document.apply(op(EditOperation::Add, {up.row, kWindow, 0}, up.child), error));
	const NodeAddress held{up.row, kWindow, document.last_added()};
	TEST_EXPECT(window_of(document, held) && window_of(document, held)->name == "WINDOW1" && document.ancestors(held).back() == up);
	const std::string payload = document.copy({held});
	TEST_EXPECT(!payload.empty() && payload.find("\"WINDOW1\"") != std::string::npos);
	TEST_EXPECT(document.copy({spin, held}) == document.copy({spin}) && !document.copy({spin}).empty());
	// The name the part's window holds is taken: a new root is WINDOW2, a pasted copy WINDOW2
	// too (the new root undone first).
	TEST_EXPECT(document.apply(op(EditOperation::Add, {up.row, kWindow, 0}), error) &&
	            window_of(document, {up.row, kWindow, document.last_added()})->name == "WINDOW2");
	document.undo();
	Edit paste = op(EditOperation::Paste, {up.row, kWindow, 0}, 0, SIZE_MAX);
	paste.value = payload;
	TEST_EXPECT(document.apply(paste, error) && document.identities_match() &&
	            window_of(document, {up.row, kWindow, document.last_added()})->name == "WINDOW2");
	return 0;
}

// A screen duplicated after its records were looked up: the copy's records answer by their
// own identities (read, set, add into, found by name), and the original's still do.
int duplicate_screen() {
	editor_test::TempProjectDir dir("opennova_menu_duplicate_screen_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	NodeAddress back;
	TEST_EXPECT(find_definition(
			AssetGraph(), document, "BACK", back)); // the original's places are built
	Diagnostic error;
	TEST_EXPECT(document.apply(op(EditOperation::Duplicate, {document.rows()[0]->id, kScreen, 0}, 0, 1), error));
	TEST_EXPECT(document.rows().size() == 2 && document.identities_match());
	auto second = [&]() -> const Node & { return *document.rows()[1]; };
	const NodeAddress root{second().id, kWindow, document.window_at(second(), 0)};
	TEST_EXPECT(root.child && root.child != document.window_at(*document.rows()[0], 0));
	TEST_EXPECT(text_of(document, root, "name") == "MAIN" && window_of(document, root) && !document.collections_of(root).empty() &&
	            document.present(root, std::string()));
	TEST_EXPECT(window_names(document, second()) == window_names(document, *document.rows()[0]));
	const NodeAddress hotkey = child_of(document, {second().id, kWindow, document.window_at(second(), 1)}, "hotkey");
	TEST_EXPECT(hotkey.child && text_of(document, hotkey, "value") == "VK_ESCAPE" &&
	            document.address_at(document.locator(hotkey)) == hotkey);
	TEST_EXPECT(document.apply(set(root, "name", std::string("COPY")), error));
	NodeAddress found;
	TEST_EXPECT(find_definition(AssetGraph(), document, "COPY", found) && found == root);
	TEST_EXPECT(document.apply(op(EditOperation::Add, {second().id, kWindow, 0}, root.child), error) &&
	            document.ancestors({second().id, kWindow, document.last_added()}).back() == root && document.identities_match());
	TEST_EXPECT(text_of(document, back, "name") == "BACK" && text_of(document, root, "name") == "COPY");
	return 0;
}

// S9h2, what the menu window asks for: a window added with its type is one step (the
// Add names the field), and a duplicated screen takes a name no other screen has (retail
// finds the last of two screens of one name, so a same-named copy would stand in for the
// original), its windows keeping theirs.
int typed_add_and_screen_copy() {
	editor_test::TempProjectDir dir("opennova_menu_typed_add_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	const std::string original = document.serialize().text;
	const NodeId row = document.rows()[0]->id;
	NodeAddress main;
	TEST_EXPECT(find_definition(AssetGraph(), document, "MAIN", main));
	Diagnostic error;
	Edit add = op(EditOperation::Add, {row, kWindow, 0}, main.child);
	add.field = "type";
	add.value = std::string("button");
	TEST_EXPECT(document.apply(add, error) && document.identities_match());
	const NodeAddress made{row, kWindow, document.last_added()};
	TEST_EXPECT(window_of(document, made) && window_of(document, made)->type == mnu::WindowType::Button &&
	            text_of(document, made, "type") == "button" && document.ancestors(made).back() == main);
	document.undo();
	TEST_EXPECT(document.serialize().text == original && !document.can_undo());
	TEST_EXPECT(document.apply(op(EditOperation::Duplicate, {row, kScreen, 0}, 0, 1), error));
	TEST_EXPECT(document.apply(op(EditOperation::Duplicate, {row, kScreen, 0}, 0, 2), error));
	TEST_EXPECT(document.rows().size() == 3 && document.rows()[0]->name() == "OPTIONS" &&
	            document.rows()[1]->name() == "OPTIONS2" && document.rows()[2]->name() == "OPTIONS3");
	TEST_EXPECT(window_names(document, *document.rows()[1]) == window_names(document, *document.rows()[0]));
	TEST_EXPECT(document.identities_match());
	document.undo();
	document.undo();
	TEST_EXPECT(document.serialize().text == original && !document.dirty());
	// The two copies in one batch (S13 D7): the second is named beside the first, as the batch left
	// the rows (prepare_duplicate), and two new screens take two names.
	TEST_EXPECT(document.apply({op(EditOperation::Duplicate, {row, kScreen, 0}, 0, 1),
	                            op(EditOperation::Duplicate, {row, kScreen, 0}, 0, 2)},
	                           error));
	TEST_EXPECT(document.rows().size() == 3 && document.rows()[1]->name() == "OPTIONS2" &&
	            document.rows()[2]->name() == "OPTIONS3" && document.identities_match());
	document.undo();
	TEST_EXPECT(document.apply({op(EditOperation::Add, {0, kScreen, 0}), op(EditOperation::Add, {0, kScreen, 0})}, error));
	TEST_EXPECT(document.rows().size() == 3 && document.rows()[1]->name() != document.rows()[2]->name());
	document.undo();
	TEST_EXPECT(document.serialize().text == original && !document.dirty());
	return 0;
}

// A menu keeps its screens found: removing the last screen is refused and commits
// nothing (the document's veto, not only the menu view's), and a new screen takes a name
// no other screen has, even the SCREEN<n> its identity would give it, which a file read
// again may already hold (retail finds the last of two screens of one name).
int screens_stay_found() {
	editor_test::TempProjectDir dir("opennova_menu_screens_found_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	const NodeAddress only{document.rows()[0]->id, kScreen, 0};
	const uint64_t revision = document.revision();
	Diagnostic error;
	TEST_EXPECT(!document.apply(op(EditOperation::Remove, only), error) && error.code() == "document.structure" &&
	            error.message.find("at least one screen") != std::string::npos);
	TEST_EXPECT(document.rows().size() == 1 && document.revision() == revision && !document.can_undo() && !document.dirty());
	// A second screen: the name its identity gives it while free, and then the first may go.
	TEST_EXPECT(document.apply(op(EditOperation::Add, {0, kScreen, 0}), error) && document.rows().size() == 2);
	const std::string by_identity = "SCREEN" + std::to_string(document.last_added());
	TEST_EXPECT(document.rows()[1]->name() == by_identity);
	TEST_EXPECT(document.apply(op(EditOperation::Remove, only), error) && document.rows().size() == 1);
	TEST_EXPECT(!document.apply(op(EditOperation::Remove, {document.rows()[0]->id, kScreen, 0}), error));
	document.undo();
	TEST_EXPECT(document.rows().size() == 2 && document.rows()[0]->name() == "OPTIONS");

	// The same file with its screen named what the next new screen's identity would name it
	// (the same records, so the same identities): the new screen takes another name.
	// Written in lower case: screens are compared as the lookups compare names.
	std::string lowered = by_identity;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::string text = kShippedShape;
	const std::string authored = "<NAME>OPTIONS</NAME>";
	text.replace(text.find(authored), authored.size(), "<NAME>" + lowered + "</NAME>");
	const std::string again = dir.file("again.mnu");
	TEST_EXPECT(editor_test::write_text(again, text));
	MnuDocument reread;
	TEST_EXPECT(load(reread, again));
	TEST_EXPECT(reread.apply(op(EditOperation::Add, {0, kScreen, 0}), error) && reread.rows().size() == 2);
	TEST_EXPECT(reread.last_added() == document.rows()[1]->id);
	TEST_EXPECT(reread.rows()[1]->name() == "SCREEN2" && reread.rows()[0]->name() == lowered);
	return 0;
}

// The nested lists: a table cell moved between rows, an element's attribute and element
// between extra elements, each and back; into a window only what a window keeps (a save
// would lose anything else), so the edited menu reads back as written.
int nested_lists() {
	editor_test::TempProjectDir dir("opennova_menu_nested_lists_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	NodeAddress back, results;
	TEST_EXPECT(find_definition(AssetGraph(), document, "BACK", back) &&
			find_definition(AssetGraph(), document, "RESULTS", results));
	const std::string original = document.serialize().text;
	Diagnostic error;
	auto step = [&](const Edit &edit) { return document.apply(edit, error) && document.identities_match(); };
	auto owner = [&](const NodeAddress &record) { return document.ancestors(record).back(); };
	// A table cell between rows.
	const NodeAddress row = child_of(document, results, "items.row");
	TEST_EXPECT(step(op(EditOperation::Add, {results.row, menu_kind("items.row"), 0}, results.child)));
	const NodeAddress other_row{results.row, menu_kind("items.row"), document.last_added()};
	const NodeAddress cell = child_of(document, row, "item", 1);
	TEST_EXPECT(step(op(EditOperation::Move, cell, other_row.child, 0)) && owner(cell) == other_row &&
	            text_of(document, cell, "text") == "x.tga" && window_of(document, results)->items.rows[0].cells.size() == 1);
	TEST_EXPECT(step(op(EditOperation::Move, cell, row.child, SIZE_MAX)) && owner(cell) == row &&
	            child_of(document, row, "item", 1) == cell && step(op(EditOperation::Remove, other_row)));
	// An element's attribute and element between two extra elements.
	const NodeKind element_kind = menu_kind("element"), attribute_kind = menu_kind("attribute");
	TEST_EXPECT(step(op(EditOperation::Add, {back.row, element_kind, 0}, back.child)));
	const NodeAddress one{back.row, element_kind, document.last_added()};
	TEST_EXPECT(step(op(EditOperation::Add, {back.row, element_kind, 0}, back.child)));
	const NodeAddress two{back.row, element_kind, document.last_added()};
	TEST_EXPECT(step(op(EditOperation::Add, {back.row, attribute_kind, 0}, one.child)));
	const NodeAddress attribute{back.row, attribute_kind, document.last_added()};
	TEST_EXPECT(step(op(EditOperation::Add, {back.row, element_kind, 0}, one.child)));
	const NodeAddress inner{back.row, element_kind, document.last_added()};
	for (const NodeAddress &record : {attribute, inner}) {
		TEST_EXPECT(step(op(EditOperation::Move, record, two.child, 0)) && owner(record) == two);
		TEST_EXPECT(step(op(EditOperation::Move, record, one.child, 0)) && owner(record) == one);
	}
	// Into a window: an attribute NAME, a PLAYERLIST with a value and an element tagged
	// ANY_NAME are refused (the tag kept as the reader keeps it, upper case) ...
	TEST_EXPECT(text_of(document, attribute, "name") == "NAME");
	TEST_EXPECT(!document.apply(op(EditOperation::Move, attribute, back.child, 0), error) &&
	            error.message.find("PLAYERLIST and SERVERLIST attributes") != std::string::npos);
	TEST_EXPECT(step(set(attribute, "name", std::string("PLAYERLIST"))) && step(set(attribute, "value", std::string("1"))));
	TEST_EXPECT(!document.apply(op(EditOperation::Move, attribute, back.child, 0), error) &&
	            error.message.find("no value") != std::string::npos);
	TEST_EXPECT(step(set(inner, "tag", std::string("any_name"))) && text_of(document, inner, "tag") == "ANY_NAME");
	TEST_EXPECT(!document.apply(op(EditOperation::Move, inner, back.child, 0), error) &&
	            error.message.find("keeps only these elements") != std::string::npos);
	TEST_EXPECT(owner(attribute) == one && owner(inner) == one);
	// ... a PLAYERLIST with none and a TARGET move in.
	Edit clear = op(EditOperation::Clear, attribute);
	clear.field = "value";
	TEST_EXPECT(step(clear) && step(op(EditOperation::Move, attribute, back.child, 0)) && owner(attribute) == back);
	TEST_EXPECT(step(set(inner, "tag", std::string("TARGET"))) && step(op(EditOperation::Move, inner, back.child, 0)) &&
	            owner(inner) == back);
	const SerializeResult edited = document.serialize();
	TEST_EXPECT(edited.ok());
	mnu::Document reread;
	std::string message;
	std::vector<mnu::ParseNote> notes;
	TEST_EXPECT(mnu::parse(edited.text, reread, message, &notes) && notes.empty() && mnu::serialize(reread) == edited.text);
	while (document.can_undo()) document.undo();
	TEST_EXPECT(document.serialize().text == original && document.identities_match());
	return 0;
}

// --- Edits that must not lose authored data (ADR 0046 S9d, S9h) -----------------------

int shipped_shape_edits() {
	editor_test::TempProjectDir dir("opennova_menu_shipped_shape_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	const Node &row = *document.rows()[0];
	const NodeAddress screen{row.id, kScreen, 0};
	NodeAddress root, back;
	TEST_EXPECT(find_definition(AssetGraph(), document, "MAIN", root) &&
			find_definition(AssetGraph(), document, "BACK", back));
	auto appearances = [&]() { return window_of(document, back)->appearances; };
	auto hotkeys = [&]() { return window_of(document, back)->hotkeys; };
	Diagnostic error;

	// B2: the APPEARANCE rows are records. The sprite-sheet image row's texture edited in
	// place keeps its layout; an image for the default state is a row of its own beside
	// the custom hook, the color and the typeless placeholder, which nothing rewrites.
	const NodeAddress disabled = child_of(document, back, "appearance", 3);
	TEST_EXPECT(document.apply(set(disabled, "value", std::string("back2.tga")), error));
	TEST_EXPECT(appearances()[3].value == "back2.tga" && appearances()[3].has_map_state && appearances()[3].map_state == 2);
	TEST_EXPECT(document.apply(op(EditOperation::Add, {back.row, menu_kind("appearance"), 0}, back.child), error));
	const NodeAddress added{back.row, menu_kind("appearance"), document.last_added()};
	TEST_EXPECT(document.apply(std::vector<Edit>{set(added, "type", std::string("IMAGE")),
	                                             set(added, "value", std::string("video.tga"))},
	                           error));
	TEST_EXPECT(appearances().size() == 5 && appearances()[0].type == "custom" && appearances()[4].value == "video.tga");
	TEST_EXPECT(document.apply(op(EditOperation::Remove, added), error));
	TEST_EXPECT(appearances().size() == 4 && appearances()[0].type == "custom" && appearances()[1].type == "color" &&
	            appearances()[2].type.empty());

	// B4: the HOTKEY rows are records with their own VIRTUAL flag.
	const NodeAddress escape = child_of(document, back, "hotkey");
	TEST_EXPECT(text_of(document, escape, "value") == "VK_ESCAPE" && text_of(document, escape, "virtual") == "1");
	TEST_EXPECT(document.apply(set(escape, "value", std::string("VK_RETURN")), error));
	TEST_EXPECT(hotkeys().size() == 2 && hotkeys()[0].value == "VK_RETURN" && hotkeys()[0].virtual_key);
	TEST_EXPECT(hotkeys()[1].value == "B" && !hotkeys()[1].virtual_key);
	TEST_EXPECT(document.apply(op(EditOperation::Remove, escape), error) && hotkeys().size() == 1 && hotkeys()[0].value == "B");
	TEST_EXPECT(document.apply(op(EditOperation::Add, {back.row, menu_kind("hotkey"), 0}, back.child, 0), error));
	TEST_EXPECT(document.apply(set({back.row, menu_kind("hotkey"), document.last_added()}, "value", std::string("Q")), error));
	TEST_EXPECT(hotkeys().size() == 2 && hotkeys()[0].value == "Q" && !hotkeys()[0].virtual_key);
	document.undo(); document.undo(); document.undo(); // back to VK_RETURN (virtual) + B
	TEST_EXPECT(hotkeys().size() == 2 && hotkeys()[0].virtual_key && hotkeys()[1].value == "B");

	// B3: the text table and the cursor are the windows' own (retail reads neither on a
	// SCREEN [orig: CUIScene_ParseNodeAttributes @ 0x639630]): the screen has no such field,
	// every window does, and a save writes what the window holds.
	Value none;
	TEST_EXPECT(!document.get(screen, "text_rsrc", none) && !document.get(screen, "cursor.file", none));
	TEST_EXPECT(text_of(document, root, "text_rsrc") == "menutxt.BIN" && text_of(document, root, "cursor.file") == "newarow1.tga");
	TEST_EXPECT(text_of(document, root, "cursor.flags") == "STANDARD_TRANSPARENT");
	TEST_EXPECT(document.apply(set(root, "text_rsrc", std::string("options.bin")), error));
	TEST_EXPECT(document.apply(set(root, "cursor.flags", std::string("STANDARD")), error));
	TEST_EXPECT(document.apply(set(root, "cursor.file", std::string("arrow2.tga")), error));
	// The text table takes the schema's width (64 with the terminator).
	TEST_EXPECT(!document.apply(set(root, "text_rsrc", std::string(64, 'a')), error));
	TEST_EXPECT(document.apply(set(root, "text_rsrc", std::string(63, 'a')), error));
	document.undo();
	TEST_EXPECT(document.save(error));
	mnu::Document reparsed;
	std::string message;
	TEST_EXPECT(mnu::parse_file(path, reparsed, message) && reparsed.screens.size() == 1);
	const mnu::Screen &saved = reparsed.screens[0];
	TEST_EXPECT(saved.roots.size() == 1 && saved.roots[0].text_rsrc == "options.bin");
	TEST_EXPECT(saved.roots[0].cursor.file == "arrow2.tga" && saved.roots[0].cursor.flags == "STANDARD");
	const mnu::Window &saved_back = saved.roots[0].children[0];
	TEST_EXPECT(saved_back.appearances.size() == 4 && saved_back.appearances[0].type == "custom" &&
	            saved_back.appearances[3].value == "back2.tga");
	TEST_EXPECT(saved_back.hotkeys.size() == 2 && saved_back.hotkeys[0].value == "VK_RETURN" && saved_back.hotkeys[0].virtual_key);
	// A Set of the value an unauthored field reads authors nothing: BACK has no RIGHT, no
	// GROUP, no STRING.
	const std::string before = document.serialize().text;
	TEST_EXPECT(document.apply(set(back, "position.right", std::stoll(text_of(document, back, "position.right"))), error));
	TEST_EXPECT(document.serialize().text == before);
	TEST_EXPECT(document.apply(set(back, "group", int64_t(0)), error) && document.serialize().text == before);
	TEST_EXPECT(document.apply(set(back, "string.value", std::string()), error) && document.serialize().text == before);
	return 0;
}

// The inspector sends a coalesced Set per keystroke: a field's typing is one undo step,
// each Set applied to the record as the group found it.
int typed_clear_and_retype() {
	editor_test::TempProjectDir dir("opennova_menu_typed_retype_test");
	const std::string path = dir.file("options.mnu");
	TEST_EXPECT(editor_test::write_text(path, kShippedShape));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	NodeAddress back;
	TEST_EXPECT(find_definition(AssetGraph(), document, "BACK", back));
	const NodeAddress image = child_of(document, back, "appearance", 3);
	const NodeAddress escape = child_of(document, back, "hotkey");
	auto appearances = [&]() { return window_of(document, back)->appearances; };
	auto hotkeys = [&]() { return window_of(document, back)->hotkeys; };
	const std::string original = document.serialize().text;
	Diagnostic error;

	for (const char *value : {"back.tg", "", "t", "tab_new.tga"}) TEST_EXPECT(document.apply(typed(image, "value", value), error));
	TEST_EXPECT(appearances().size() == 4 && appearances()[3].type == "image" && appearances()[3].value == "tab_new.tga");
	TEST_EXPECT(appearances()[3].has_map_state && appearances()[3].map_state == 2 && appearances()[3].height == 20);
	document.end_edit_group();
	document.undo(); // the typing was one step
	TEST_EXPECT(document.serialize().text == original && !document.can_undo());

	for (const char *value : {"VK_ESCAP", "", "V", "VK_RETURN"}) TEST_EXPECT(document.apply(typed(escape, "value", value), error));
	TEST_EXPECT(hotkeys().size() == 2 && hotkeys()[0].value == "VK_RETURN" && hotkeys()[0].virtual_key);
	TEST_EXPECT(hotkeys()[1].value == "B" && !hotkeys()[1].virtual_key);
	document.end_edit_group();
	document.undo();
	TEST_EXPECT(document.serialize().text == original && !document.can_undo());

	// A gesture's edits build on each other instead (S9g), the whole gesture one step.
	const uint64_t gesture = next_edit_gesture();
	for (const char *value : {"back.tg", "", "tab_new.tga"}) {
		Edit edit = set(image, "value", std::string(value));
		edit.gesture = gesture;
		TEST_EXPECT(document.apply(edit, error));
	}
	TEST_EXPECT(appearances()[3].value == "tab_new.tga");
	document.end_edit_group();
	document.undo();
	TEST_EXPECT(document.serialize().text == original && !document.can_undo());
	// Coalesced batches take the group's base too: two fields per keystroke, one step.
	for (const char *value : {"VK_ESCAP", "", "VK_RETURN"}) {
		Edit hotkey = typed(escape, "value", value), texture = typed(image, "value", value[0] ? "t.tga" : "");
		TEST_EXPECT(document.apply(std::vector<Edit>{hotkey, texture}, error));
	}
	TEST_EXPECT(hotkeys()[0].value == "VK_RETURN" && hotkeys()[0].virtual_key && appearances()[3].value == "t.tga");
	document.end_edit_group();
	document.undo();
	TEST_EXPECT(document.serialize().text == original && !document.can_undo());
	return 0;
}

// Parse notes (S9p1): input retail ignores is a warning and the menu stays editable;
// input retail crashes or hangs on blocks the menu, and its error stops the build
// (docs/mnu/menu-re.md, "Crash and hang cases"); so does a number holding a stylesheet
// variable, which retail reads through the variable and a save would write as 0 (S12 B2).
int parse_notes() {
	editor_test::TempProjectDir dir("opennova_menu_parse_notes_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Notes"));
	editor_test::create_missing_files(session);
	const std::string window = "<WINDOW type=\"button\" name=\"B\" SCREENX=\"1\"><POSITION><LEFT>0</LEFT></POSITION>";
	TEST_EXPECT(editor_test::write_text(dir.file("project/menus/ignored.mnu"),
	                                    "<SCREEN><NAME>I</NAME>" + window + "</WINDOW></SCREEN>"));
	TEST_EXPECT(editor_test::write_text(dir.file("project/menus/crash.mnu"),
	                                    "<SCREEN><NAME>C</NAME>" + window + "<ACTION type=\"\">X</ACTION></WINDOW></SCREEN>"));
	// The text ends inside a tag: retail's attribute loop never ends.
	TEST_EXPECT(editor_test::write_text(dir.file("project/menus/hang.mnu"), "<SCREEN><NAME>H</NAME><WINDOW type=\"button\""));
	TEST_EXPECT(editor_test::write_text(dir.file("project/menus/variable.mnu"),
	                                    "<SCREEN><NAME>V</NAME><WINDOW type=\"button\" name=\"B\"><POSITION><LEFT>%X%"
	                                    "</LEFT></POSITION></WINDOW></SCREEN>"));
	// S12 review: B's ignored attribute after a window A inside MAIN, for a move below.
	TEST_EXPECT(editor_test::write_text(dir.file("project/menus/moved.mnu"),
	                                    "<SCREEN><NAME>M</NAME><WINDOW type=\"window\" name=\"MAIN\"><WINDOW type=\"button\" "
	                                    "name=\"A\"><POSITION><LEFT>0</LEFT></POSITION></WINDOW>" +
	                                            window + "</WINDOW></WINDOW></SCREEN>"));
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &view = session.view();
	auto finding = [&](const char *asset, const char *code) -> const Diagnostic * {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == code && d.asset.find(asset) != std::string::npos) return &d;
		return nullptr;
	};
	const Diagnostic *ignored = finding("ignored.mnu", "menu.ignored_input");
	TEST_EXPECT(ignored && ignored->severity == DiagnosticSeverity::Warning && ignored->field.find("@SCREENX") != std::string::npos);
	TEST_EXPECT(!finding("ignored.mnu", "menu.invalid_input"));
	const Diagnostic *crash = finding("crash.mnu", "menu.invalid_input");
	TEST_EXPECT(crash && crash->severity == DiagnosticSeverity::Error && crash->field.find("/ACTION@TYPE") != std::string::npos);
	const Diagnostic *hang = finding("hang.mnu", "menu.invalid_input");
	TEST_EXPECT(hang && hang->severity == DiagnosticSeverity::Error);
	const Diagnostic *variable = finding("variable.mnu", "menu.invalid_input");
	TEST_EXPECT(variable && variable->severity == DiagnosticSeverity::Error &&
	            variable->field.find("/POSITION/LEFT") != std::string::npos);
	editor_test::handle_to_end(session, request::open_document("variable.mnu"));
	const Document *held = session.document_for("variable.mnu");
	TEST_EXPECT(held && held->blocked() && !held->serialize().ok());
	editor_test::handle_to_end(session, request::open_document("ignored.mnu"));
	editor_test::handle_to_end(session, request::open_document("crash.mnu"));
	const Document *editable = session.document_for("ignored.mnu");
	const Document *blocked = session.document_for("crash.mnu");
	TEST_EXPECT(editable && !editable->blocked() && editable->ignored_lines() == 1 && editable->serialize().ok());
	TEST_EXPECT(blocked && blocked->blocked() && !blocked->serialize().ok());
	// An explicit Save rewrites the one without the input the game ignores, and refuses the
	// one that does not serialize: never "no changes" (S11b).
	TEST_EXPECT(editable->rewrite_need() == Document::RewriteNeed::Rewrite);
	TEST_EXPECT(blocked->rewrite_need() == Document::RewriteNeed::Unserializable);
	// S12 review: a source finding stays on its window while the window moves (its locator
	// names the file as loaded, Document::source_address), and goes to the file once removed.
	editor_test::handle_to_end(session, request::open_document("moved.mnu"));
	Document *moved = session.document_for("moved.mnu");
	NodeAddress b;
	TEST_EXPECT(moved && find_definition(AssetGraph(), *moved, "B", b) && b.child != 0);
	if (!moved || !b.child) return 1;
	const auto on_b = [&]() {
		const Diagnostic *d = finding("moved.mnu", "menu.ignored_input");
		return d ? d->child_id : NodeId(-1);
	};
	TEST_EXPECT(on_b() == b.child);
	EditorRequest move = request::edit_record(moved->path(), Edit());
	move.edits[0].operation = EditOperation::Move;
	move.edits[0].address = b;
	move.edits[0].position = 0;
	editor_test::handle_to_end(session, move);
	Document::Placement at;
	TEST_EXPECT(moved->placement(b, at) && at.index == 0 && on_b() == b.child);
	EditorRequest remove = request::edit_record(moved->path(), Edit());
	remove.edits[0].operation = EditOperation::Remove;
	remove.edits[0].address = b;
	editor_test::handle_to_end(session, remove);
	TEST_EXPECT(on_b() == 0);
	return 0;
}

// The blank project's STARTUP menu: the moves that change nothing, its root's custom
// row, and a string table renamed under a menu that names it.
int blank_menu_edits() {
	editor_test::TempProjectDir dir("opennova_menu_blank_edits_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Edits"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	auto *document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document && !document->dirty());
	const Node &row = *document->rows()[0];
	const NodeAddress screen{row.id, kScreen, 0};
	const NodeAddress root{row.id, kWindow, document->window_at(row, 0)};
	NodeAddress exit;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "EXIT", exit));
	Diagnostic error;

	// B5: EXIT is the last of MAIN's two children; Down (and a Move to where it is)
	// changes nothing, so nothing is recorded. Likewise the only root and the only screen.
	TEST_EXPECT(sibling_place(*document, exit) == std::make_pair(size_t(1), size_t(2)));
	TEST_EXPECT(sibling_place(*document, root) == std::make_pair(size_t(0), size_t(1)));
	TEST_EXPECT(document->apply(op(EditOperation::Move, exit, 0, 2), error) && !document->dirty() && !document->can_undo());
	TEST_EXPECT(document->apply(op(EditOperation::Move, exit, 0, 1), error) && !document->dirty() && !document->can_undo());
	TEST_EXPECT(document->apply(op(EditOperation::Move, root, 0, 1), error) && !document->dirty() && !document->can_undo());
	TEST_EXPECT(document->apply(op(EditOperation::Move, screen, 0, 1), error) && !document->dirty() && !document->can_undo());
	TEST_EXPECT(document->apply(op(EditOperation::Move, exit, 0, 0), error) && document->dirty() && document->can_undo());
	document->undo();
	TEST_EXPECT(!document->dirty() && window_names(*document, *document->rows()[0])[2] == "EXIT");

	// B2 on the blank root: MAIN's only row is the custom hook; an image row added beside it
	// and removed leaves it as it was.
	TEST_EXPECT(document->apply(op(EditOperation::Add, {row.id, menu_kind("appearance"), 0}, root.child), error));
	const NodeAddress image{row.id, menu_kind("appearance"), document->last_added()};
	TEST_EXPECT(document->apply(std::vector<Edit>{set(image, "type", std::string("IMAGE")),
	                                              set(image, "value", std::string("logo.tga"))},
	                            error));
	TEST_EXPECT(window_of(*document, root)->appearances.size() == 2 && window_of(*document, root)->appearances[0].type == "custom");
	TEST_EXPECT(document->apply(op(EditOperation::Remove, image), error));
	const auto &kept = window_of(*document, root)->appearances;
	TEST_EXPECT(kept.size() == 1 && kept[0].type == "custom" && kept[0].state == "default" && kept[0].value.empty());

	// B3 through the rename transaction: the menu names a string table; renaming the table
	// rewrites the menu's saved TEXT_RSRC, so nothing is left dangling.
	editor_test::handle_to_end(session, request::create_file("mytext.bin", asset_kind_token(AssetKind::Strings)));
	TEST_EXPECT(session.document_for("mytext.bin"));
	document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document);
	EditorRequest edit = request::edit_record(document->path(), Edit());
	const NodeAddress main_window{document->rows()[0]->id, kWindow, document->window_at(*document->rows()[0], 0)};
	edit.edits = {set(main_window, "text_rsrc", std::string("mytext.bin"))};
	editor_test::handle_to_end(session, edit);
	editor_test::handle_to_end(session, request::save_all());
	TEST_EXPECT(!has_code(view.findings.diagnostics, "reference.missing"));
	const std::string menu_path = view.project.root + "/" + document->path();
	editor_test::handle_to_end(session, request::rename_asset("mytext.bin", "newtext.bin"));
	TEST_EXPECT(session.outcome().done());
	mnu::Document reparsed;
	std::string message;
	TEST_EXPECT(mnu::parse_file(menu_path, reparsed, message) && reparsed.screens.size() == 1);
	TEST_EXPECT(reparsed.screens[0].roots.size() == 1 && reparsed.screens[0].roots[0].text_rsrc == "newtext.bin");
	TEST_EXPECT(view.findings.graph && view.findings.graph->missing().empty() && !has_code(view.findings.diagnostics, "reference.missing"));
	document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document && text_of(*document, {document->rows()[0]->id, kWindow, document->window_at(*document->rows()[0], 0)},
	                                "text_rsrc") == "newtext.bin");
	return 0;
}

// A screen's or a window's NAME set in the Inspector is its own edit (S13 D5 cut the same-file
// follow of S9l): the ACTIONs naming it by the old NAME keep it, the graph reporting each that no
// longer resolves, and one undo gives the NAME back; Rename everywhere (RenameSymbol) is what
// rewrites the uses, the definition and each ACTION reaching it.
int name_is_its_own_edit() {
	editor_test::TempProjectDir dir("opennova_menu_name_edit_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Names"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string place = "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT>"
	                          "<BOTTOM>20</BOTTOM></POSITION>\r\n";
	const auto window = [&](const char *type, const char *name, const std::string &body) {
		return std::string("<WINDOW TYPE=\"") + type + "\" NAME=\"" + name + "\">\r\n" + place +
		       body + "</WINDOW>\r\n";
	};
	const std::string go = "<ACTION TYPE=\"SCREEN\" FILE=\"flow.mnu\">AWAY</ACTION>\r\n"
	                       "<ACTION TYPE=\"WINDOW\" STATE=\"SHOW\">TITLE</ACTION>\r\n";
	const std::string back = "<ACTION TYPE=\"SCREEN\" FILE=\"flow.mnu\">HOME</ACTION>\r\n";
	const std::string panel =
	        window("STATIC", "PANEL", window("BUTTON", "GO", go) + window("STATIC", "TITLE", ""));
	TEST_EXPECT(editor_test::write_text(view.project.root + "/flow.mnu",
	                                    "<SCREEN>\r\n<NAME>HOME</NAME>\r\n" + panel +
	                                            "</SCREEN>\r\n<SCREEN>\r\n<NAME>AWAY</NAME>\r\n" +
	                                            window("BUTTON", "BACK", back) + "</SCREEN>\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("flow.mnu"));
	auto *menu = dynamic_cast<MnuDocument *>(session.document_for("flow.mnu"));
	TEST_EXPECT(menu && menu->rows().size() == 2);
	if (!menu || menu->rows().size() != 2) return 1;
	const NodeAddress home{menu->rows()[0]->id, kScreen, 0};
	NodeAddress go_window, back_window, title;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "GO", go_window) &&
			find_definition(AssetGraph(), *menu, "BACK", back_window));
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "TITLE", title));
	const NodeAddress show_title = child_of(*menu, go_window, "action", 1);
	const NodeAddress go_home = child_of(*menu, back_window, "action", 0);
	const auto rename = [&](const NodeAddress &record, const char *name) {
		EditorRequest request = request::edit_record(
				menu->path(), typed(record, "name", name)); // as the Inspector sets it
		editor_test::handle_to_end(session, request);
		return session.outcome().done();
	};
	const auto missing = [&](const char *record) {
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.code() == "reference.missing" && d.record == record) return true;
		return false;
	};
	TEST_EXPECT(!missing("HOME/PANEL/GO/Action 2") && !missing("AWAY/BACK/Action 1"));
	// The window: GO's WINDOW target keeps TITLE, which nothing defines now.
	TEST_EXPECT(rename(title, "HEADLINE") && text_of(*menu, title, "name") == "HEADLINE");
	TEST_EXPECT(text_of(*menu, show_title, "target") == "TITLE");
	TEST_EXPECT(missing("HOME/PANEL/GO/Action 2"));
	editor_test::handle_to_end(session, request::undo(menu->path()));
	TEST_EXPECT(text_of(*menu, title, "name") == "TITLE" && !menu->can_undo());
	TEST_EXPECT(!missing("HOME/PANEL/GO/Action 2"));
	// The screen: BACK's SCREEN target keeps HOME.
	TEST_EXPECT(rename(home, "START") && text_of(*menu, home, "name") == "START");
	TEST_EXPECT(text_of(*menu, go_home, "target") == "HOME" && missing("AWAY/BACK/Action 1"));
	editor_test::handle_to_end(session, request::undo(menu->path()));
	TEST_EXPECT(text_of(*menu, home, "name") == "HOME" && !menu->can_undo());
	// Rename everywhere: the window and the ACTION reaching it, on disk.
	editor_test::handle_to_end(session, request::rename_symbol(menu->path(), menu->locator(title), "name", "HEADLINE"));
	TEST_EXPECT(session.outcome().done());
	menu = dynamic_cast<MnuDocument *>(session.document_for("flow.mnu"));
	NodeAddress headline, go_again;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "HEADLINE", headline) &&
			find_definition(AssetGraph(), *menu, "GO", go_again));
	if (!menu || !go_again.child) return 1;
	TEST_EXPECT(text_of(*menu, child_of(*menu, go_again, "action", 1), "target") == "HEADLINE" &&
	            !missing("HOME/PANEL/GO/Action 2"));
	return 0;
}

// --- The lookup: the editor's by-name finds are the game's (S12 A1) ----------------------
//
// MnuDocument::lookup_names (the duplicate-name Problems rows, the graph's menu names) and
// the game's MenuRuntime (a screen by name; find_control, a window by name on a screen)
// answer every NAME of a menu alike: the screen each screen NAME shows, and the window each
// window NAME finds on its screen (a duplicate finds the first, a window under one with no
// NAME is never found, a screen a later one of its name shadows is never searched). The
// editor's windows meet the runtime's doc ids by their place among the named windows of
// their screen in pre-order. Empty when the two agree, else the first difference.
std::string lookup_disagreement(const MnuDocument &document) {
	using Found = MenuLookupName::Found;
	using opennova::strutil::iequals;
	using opennova::strutil::to_upper;
	const mnu::Document native = document.native();
	if (native.screens.empty()) return std::string();
	opennova::menu::MenuRuntime runtime;
	if (!runtime.open_document(&native, "lookup.mnu", std::string())) return "the runtime shows no screen";
	const opennova::menu::MenuDocIndex &index = runtime.index();
	if (index.screen_ids().size() != native.screens.size() || document.rows().size() != native.screens.size())
		return "one screen each";
	std::map<NodeId, size_t> row_index;
	for (size_t i = 0; i < document.rows().size(); ++i) row_index[document.rows()[i]->id] = i;
	// The runtime's named windows of each screen in pre-order (through windows with no NAME).
	std::vector<std::vector<int>> named(native.screens.size());
	std::function<void(size_t, int)> walk = [&](size_t screen, int id) {
		const opennova::menu::MenuDocIndex::Node *node = index.node(id);
		if (node == nullptr || node->window == nullptr) return;
		if (!node->window->name.empty()) named[screen].push_back(id);
		for (int child : node->child_ids) walk(screen, child);
	};
	for (size_t i = 0; i < named.size(); ++i)
		for (int root : index.node(index.screen_ids()[i])->child_ids) walk(i, root);
	const std::vector<MenuLookupName> names = document.lookup_names();
	std::map<NodeId, int> id_of; // each editor window's doc id
	std::vector<size_t> placed(named.size(), 0);
	for (const MenuLookupName &name : names) {
		if (name.address.kind != kWindow) continue;
		const size_t screen = row_index[name.screen];
		const std::vector<int> &ids = named[screen];
		if (placed[screen] >= ids.size() || index.window(ids[placed[screen]])->name != name.name)
			return "screen " + std::to_string(screen) + ": the editor's window " + name.name +
			       " is not the runtime's next named window";
		id_of[name.address.child] = ids[placed[screen]++];
	}
	for (size_t i = 0; i < named.size(); ++i)
		if (placed[i] != named[i].size())
			return "screen " + std::to_string(i) + ": the runtime holds a named window the editor does not list";
	std::map<std::string, size_t> last; // the screen each screen NAME shows, as the editor reads it
	for (size_t i = 0; i < native.screens.size(); ++i) last[to_upper(native.screens[i].name)] = i;
	// The window a NAME finds on a screen as the editor reads it: its Yes row there, else none.
	const auto finds = [&](size_t screen, const std::string &window) {
		for (const MenuLookupName &name : names)
			if (name.address.kind == kWindow && name.found == Found::Yes && row_index[name.screen] == screen &&
			    iequals(name.name, window))
				return id_of[name.address.child];
		return -1;
	};
	for (const MenuLookupName &name : names) {
		const size_t screen = row_index[name.screen];
		const std::string &screen_name = native.screens[screen].name;
		// (An empty screen argument is the current screen.)
		if (!runtime.show_screen(screen_name)) return "the runtime shows no screen named " + screen_name;
		if (name.address.kind == kScreen) {
			const size_t expected = name.found == Found::LaterScreen ? row_index[name.found_instead.row] : screen;
			const int shown = index.node(runtime.current_screen_id())->screen_index;
			if (shown < 0 || size_t(shown) != expected)
				return "screen NAME " + screen_name + ": the runtime shows screen " + std::to_string(shown) +
				       ", the editor reads screen " + std::to_string(expected);
			continue;
		}
		int expected = -1;
		switch (name.found) {
		case Found::Yes: expected = id_of[name.address.child]; break;
		case Found::EarlierWindow: expected = id_of[name.found_instead.child]; break;
		case Found::UnderNameless: expected = finds(screen, name.name); break;
		case Found::ShadowedScreen: expected = finds(last[to_upper(screen_name)], name.name); break;
		case Found::LaterScreen: return "a window read as a screen";
		}
		const int found = runtime.find_control(screen_name, name.name);
		if (found != expected)
			return "window NAME " + name.name + " on screen " + std::to_string(screen) + ": the runtime finds doc id " +
			       std::to_string(found) + ", the editor reads doc id " + std::to_string(expected);
	}
	return std::string();
}

// Two screens named HOME (the first shadowed, its windows never searched); on AWAY a GO
// under a window with no NAME, then two more GOs (the first found), and a window of the
// same name as a root on the next root.
const char *const kLookupShape =
        "<SCREEN>\r\n"
        "\t<NAME>HOME</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\"><POSITION><LEFT>0</LEFT></POSITION>\r\n"
        "\t\t<WINDOW type=\"button\" name=\"GO\"><POSITION><LEFT>0</LEFT></POSITION></WINDOW>\r\n"
        "\t</WINDOW>\r\n"
        "</SCREEN>\r\n"
        "<SCREEN>\r\n"
        "\t<NAME>AWAY</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\"><POSITION><LEFT>0</LEFT></POSITION>\r\n"
        "\t\t<WINDOW type=\"window\"><POSITION><LEFT>0</LEFT></POSITION>\r\n"
        "\t\t\t<WINDOW type=\"button\" name=\"GO\"><POSITION><LEFT>0</LEFT></POSITION></WINDOW>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"button\" name=\"GO\"><POSITION><LEFT>1</LEFT></POSITION></WINDOW>\r\n"
        "\t\t<WINDOW type=\"button\" name=\"go\"><POSITION><LEFT>2</LEFT></POSITION></WINDOW>\r\n"
        "\t</WINDOW>\r\n"
        "\t<WINDOW type=\"window\" name=\"SECOND\"><POSITION><LEFT>0</LEFT></POSITION>\r\n"
        "\t\t<WINDOW type=\"button\" name=\"MAIN\"><POSITION><LEFT>0</LEFT></POSITION></WINDOW>\r\n"
        "\t</WINDOW>\r\n"
        "</SCREEN>\r\n"
        "<SCREEN>\r\n"
        "\t<NAME>home</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\"><POSITION><LEFT>0</LEFT></POSITION></WINDOW>\r\n"
        "</SCREEN>\r\n";

// The editor's and the game's lookups agree over the menus of fixtures/ and a menu that
// meets every kind of find (the retail menus: the sweep below).
int lookup_matches_the_runtime() {
	using Found = MenuLookupName::Found;
	editor_test::TempProjectDir dir("opennova_menu_lookup_test");
	const std::string shape = dir.file("lookup.mnu");
	TEST_EXPECT(editor_test::write_text(shape, kLookupShape));
	const std::string fixtures = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/mnu/";
	for (const std::string &path : {shape, fixtures + "all_widgets.mnu", fixtures + "widgets.mnu"}) {
		MnuDocument document;
		TEST_EXPECT(load(document, path) && !document.blocked());
		const std::string why = lookup_disagreement(document);
		if (!why.empty()) std::printf("  %s: %s\n", path.c_str(), why.c_str());
		TEST_EXPECT(why.empty());
	}
	MnuDocument document;
	TEST_EXPECT(load(document, shape));
	std::map<Found, int> kinds;
	for (const MenuLookupName &name : document.lookup_names()) ++kinds[name.found];
	TEST_EXPECT(kinds[Found::LaterScreen] == 1 && kinds[Found::ShadowedScreen] == 2 && kinds[Found::UnderNameless] == 1 &&
	            kinds[Found::EarlierWindow] == 2);
	return 0;
}

// --- The retail sweep: every shipped menu through the editor's MnuDocument ------------
//
// A retail menu loads through Document::load (the decode included), is not blocked, and
// serializes exactly as mnu::serialize of the format's own parse, with its identities in
// the native tree's shape. Every field of every record (the rows and every record the
// document's walk meets at any depth) takes a Set of the value it reads, which leaves the
// bytes as they were, and whose undo restores them. Then every list kind the menu holds
// takes structural edits (a record duplicated and moved to its list's end, or moved into
// the first owner whose list of that kind is empty; a default added into an empty list; a
// part left out and written again), the identities checked after each; the edited menu
// reads back after a save as it was written, and undoing it all restores the bytes. A leg
// per root, each a SKIP-LEG without it.

struct SweepTotals {
	int menus = 0, repeats = 0, screens = 0, windows = 0, records = 0, sets = 0, structural = 0, kinds = 0, failures = 0;
	// Fields whose same-value Set changed the bytes, by field id: each is a failure.
	std::map<std::string, int> changed_by_set;
};

void sweep_fail(SweepTotals &totals, const std::string &label, const std::string &why) {
	std::printf("  FAIL %s: %s\n", label.c_str(), why.c_str());
	++totals.failures;
}

// Every field of one record Set to what it reads.
bool own_values(const MnuDocument &document, const NodeAddress &address, const std::string &label,
                std::vector<Edit> &edits, SweepTotals &totals) {
	for (const FieldSchema &field : document.fields(address.kind)) {
		Value value;
		if (!document.get(address, field.id, value)) {
			sweep_fail(totals, label + " " + field.id, "the field does not read");
			return false;
		}
		edits.push_back(set(address, field.id.c_str(), value));
	}
	return true;
}

// Every field of one record Set to what it reads in one batch, the file compared, the
// batch undone and the file compared again. A changed file is traced to its fields one
// Set at a time.
void sweep_record(MnuDocument &document, const NodeAddress &address, const std::string &label,
                  const std::string &original, SweepTotals &totals) {
	std::vector<Edit> edits;
	if (!own_values(document, address, label, edits, totals) || edits.empty()) return;
	Diagnostic error;
	if (!document.apply(edits, error)) {
		sweep_fail(totals, label + " " + error.field, "a Set of its own value is refused: " + error.message);
		return;
	}
	const bool changed = document.serialize().text != original;
	document.undo();
	if (document.serialize().text != original || document.can_undo()) sweep_fail(totals, label, "the undo left the file changed");
	if (!changed) return;
	for (const Edit &edit : edits) {
		if (!document.apply(edit, error)) continue;
		if (document.serialize().text != original) {
			++totals.changed_by_set[edit.field];
			sweep_fail(totals, label + " " + edit.field, "a Set of its own value changed the file");
		}
		document.undo();
	}
}

bool is_part(const std::string &token) {
	return token == "list_box" || token == "spinup" || token == "spindown" || token == "scrollbar";
}

// Whether `address` is `record` or sits inside it.
bool inside(const Document &document, const NodeAddress &address, const NodeAddress &record) {
	if (address == record) return true;
	for (const NodeAddress &owner : document.ancestors(address))
		if (owner.child == record.child) return true;
	return false;
}

// Structural edits over every list kind the menu holds, read back after a save, undone.
void sweep_structure(MnuDocument &document, const std::string &label, const std::string &original,
                     const std::string &scratch, SweepTotals &totals) {
	// The first owner holding a record of each kind, and the first holding none of it.
	struct Owner {
		NodeAddress address;
		NodeId first = 0;
	};
	std::map<std::string, Owner> full, empty;
	std::map<std::string, NodeKind> kinds;
	for (const auto &row : document.rows()) {
		std::vector<NodeAddress> owners{{row->id, row->kind, 0}};
		document.walk_records(*row, [&](const NodeAddress &record, const Document::Placement &) {
			owners.push_back(record);
			return true;
		});
		for (const NodeAddress &owner : owners)
			for (const Document::Collection &collection : document.collections_of(owner)) {
				const std::string token = document.kind_token(collection.spec.kind);
				kinds[token] = collection.spec.kind;
				if (collection.ids.empty()) empty.emplace(token, Owner{owner, 0});
				else full.emplace(token, Owner{owner, collection.ids.front()});
			}
	}
	Diagnostic error;
	auto step = [&](const std::string &what, const Edit &edit) {
		++totals.structural;
		if (!document.apply(edit, error)) { sweep_fail(totals, label + " " + what, "refused: " + error.message); return false; }
		if (!document.identities_match()) { sweep_fail(totals, label + " " + what, "the identities left the native shape"); return false; }
		return true;
	};
	for (const auto &[token, kind] : kinds) {
		++totals.kinds;
		const auto has = full.find(token);
		const auto lacks = empty.find(token);
		if (has != full.end()) {
			const Owner &owner = has->second;
			const NodeAddress record{owner.address.row, kind, owner.first};
			if (is_part(token)) {
				// Left out and written again, then moved to a window of its screen that has
				// none (never into the part itself).
				if (!step(token + " off", set(owner.address, token.c_str(), int64_t(0)))) continue;
				if (!step(token + " on", set(owner.address, token.c_str(), int64_t(1)))) continue;
				if (lacks != empty.end() && lacks->second.address.row == owner.address.row &&
				    window_of(document, lacks->second.address) && !inside(document, lacks->second.address, record))
					step(token + " moved", op(EditOperation::Move, record, lacks->second.address.child, 0));
				continue;
			}
			if (!step(token + " duplicated", op(EditOperation::Duplicate, record, 0, 1))) continue;
			const NodeAddress copy{record.row, kind, document.last_added()};
			if (lacks != empty.end() && lacks->second.address.row == record.row && lacks->second.address.child &&
			    lacks->second.address.child != record.child)
				step(token + " reparented", op(EditOperation::Move, copy, lacks->second.address.child, 0));
			else
				step(token + " moved", op(EditOperation::Move, copy, 0, SIZE_MAX));
		} else if (lacks != empty.end()) {
			const NodeAddress owner = lacks->second.address;
			step(token + " added", op(EditOperation::Add, {owner.row, kind, 0}, owner.child ? owner.child : owner.row));
		}
	}
	// The edited menu reads back after a save as it was written.
	const SerializeResult edited = document.serialize();
	if (!edited.ok()) {
		sweep_fail(totals, label, "the edits leave the menu unwritable: " + edited.issues.front().message);
	} else {
		const std::string copy = scratch + "/edited.mnu";
		MnuDocument reloaded;
		if (!editor_test::write_text(copy, edited.text) || !load(reloaded, copy) || reloaded.blocked() ||
		    reloaded.serialize().text != edited.text || !reloaded.identities_match() ||
		    record_count(reloaded) != record_count(document))
			sweep_fail(totals, label, "the edited menu does not read back as it was written");
	}
	while (document.can_undo()) document.undo();
	if (document.serialize().text != original || !document.identities_match())
		sweep_fail(totals, label, "undoing the structural edits left the file changed");
}

// One menu: `absolute` holds its stored bytes, `decoded` what the game's loader reads.
void sweep_menu(const std::string &label, const std::string &absolute, const std::vector<uint8_t> &decoded,
                const std::string &scratch, SweepTotals &totals) {
	++totals.menus;
	MnuDocument document;
	if (!load(document, absolute)) {
		sweep_fail(totals, label, "does not load");
		return;
	}
	if (document.blocked()) { sweep_fail(totals, label, "loads blocked"); return; }
	mnu::Document native;
	std::string message;
	if (!mnu::parse(decoded.data(), decoded.size(), native, message)) { sweep_fail(totals, label, "mnu::parse: " + message); return; }
	std::vector<uint8_t> expected;
	if (!mnu::serialize_bytes(native, expected, message)) { sweep_fail(totals, label, "mnu::serialize: " + message); return; }
	const SerializeResult serialized = document.serialize();
	if (!serialized.ok()) { sweep_fail(totals, label, "does not serialize"); return; }
	const std::string original = serialized.text;
	if (original != std::string(expected.begin(), expected.end())) {
		sweep_fail(totals, label, "serializes unlike mnu::serialize(parse())");
		return;
	}
	if (document.rows().size() != native.screens.size()) { sweep_fail(totals, label, "one row per screen"); return; }
	if (!document.identities_match()) { sweep_fail(totals, label, "identities unlike the native records"); return; }
	const std::string lookup = lookup_disagreement(document);
	if (!lookup.empty()) sweep_fail(totals, label, "the lookup unlike the game's: " + lookup);
	// The identities are captured first: every edit swaps the row it touches.
	struct Record { NodeAddress address; std::string label; };
	std::vector<Record> records;
	for (const auto &row : document.rows()) {
		++totals.screens;
		records.push_back({{row->id, row->kind, 0}, label + " " + row->name()});
		document.walk_records(*row, [&](const NodeAddress &record, const Document::Placement &) {
			records.push_back({record, label + " " + document.record_path(record)});
			++totals.records;
			if (record.kind == kWindow) ++totals.windows;
			return true;
		});
	}
	// Every field of every record of a screen Set to its own value in one batch (one row,
	// one step), the file compared and the batch undone; a changed file is traced to the
	// record, then to the field.
	for (const auto &row : document.rows()) {
		std::vector<Edit> edits;
		for (const Record &record : records)
			if (record.address.row == row->id && !own_values(document, record.address, record.label, edits, totals)) return;
		totals.sets += int(edits.size());
		Diagnostic error;
		const bool applied = document.apply(edits, error);
		const bool changed = applied && document.serialize().text != original;
		if (applied) document.undo();
		if (applied && !changed && !document.can_undo()) continue;
		if (!applied) sweep_fail(totals, label + " " + row->name(), "the batch of own values is refused: " + error.message);
		for (const Record &record : records)
			if (record.address.row == row->id) sweep_record(document, record.address, record.label, original, totals);
	}
	if (document.serialize().text != original) sweep_fail(totals, label, "the undo left the file changed");
	if (document.can_undo() || document.dirty()) sweep_fail(totals, label, "the sweep left history behind");
	sweep_structure(document, label, original, scratch, totals);
}

bool is_menu_name(const std::string &name) {
	if (name.size() < 4) return false;
	std::string ext = name.substr(name.size() - 4);
	for (char &c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return ext == ".mnu";
}

// Every .mnu one mount layer of the packed install serves (base, then each expansion),
// the way mnu_compat mounts it: the stored bytes go through Document::load's own decode.
// A menu an earlier layer already served byte for byte is counted, not swept again.
void sweep_install_mount(const std::string &install, const std::string &expansion, const std::string &scratch,
                         std::map<std::string, std::vector<uint8_t>> &seen, SweepTotals &totals) {
	opennova::Vfs vfs;
	if (!vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed)) {
		sweep_fail(totals, expansion.empty() ? "<install>" : expansion, "mount_game: " + vfs.last_error());
		return;
	}
	for (const auto &location : vfs.list_files()) {
		if (!is_menu_name(location.logical_name)) continue;
		const std::string label = (expansion.empty() ? std::string("<install>/") : expansion + "/") + location.logical_name;
		std::vector<uint8_t> stored, decoded;
		if (!vfs.read_file_raw(location.logical_name, stored) || !vfs.read_file(location.logical_name, decoded)) {
			sweep_fail(totals, label, "unreadable");
			continue;
		}
		std::vector<uint8_t> &earlier = seen[location.logical_name];
		if (earlier == stored) { ++totals.repeats; continue; }
		earlier = stored;
		const std::string absolute = scratch + "/" + location.logical_name;
		if (!editor_test::write_bytes(absolute, stored)) { sweep_fail(totals, label, "the scratch copy failed"); continue; }
		sweep_menu(label, absolute, decoded, scratch, totals);
	}
}

void print_totals(const char *leg, const SweepTotals &totals) {
	std::printf("%s: %d menu(s) through MnuDocument (%d more served unchanged by a later layer): %d screen(s), "
	            "%d record(s) (%d window(s)), %d same-value Set(s) each undone, %d structural edit(s) over %d "
	            "list kind(s), %d failure(s)\n",
	            leg, totals.menus, totals.repeats, totals.screens, totals.records, totals.windows, totals.sets,
	            totals.structural, totals.kinds, totals.failures);
	for (const auto &entry : totals.changed_by_set)
		std::printf("  a same-value Set of %s changed the file %d time(s)\n", entry.first.c_str(), entry.second);
}

int retail_sweep() {
	// The fifteen shipped revx02 menus of the reference fixture set, read loose.
	static const char *const kFixtures[] = {
		"jo_main", "jo_sp", "jo_mp", "jo_options", "jo_game", "jo_player", "jo_weapon", "jo_loadout",
		"jo_color", "jo_cmap", "jo_stat", "jo_death", "jo_vehicle", "jo_item_db", "jo_splash",
	};
	int failures = 0;
	if (retail::reference_fixture("mnu/jo_main.mnu").empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mnu/jo_*.mnu (the shipped revx02 menus through MnuDocument)");
	} else {
		editor_test::TempProjectDir scratch("opennova_menu_fixture_sweep");
		SweepTotals totals;
		for (const char *name : kFixtures) {
			const std::string path = retail::reference_fixture((std::string("mnu/") + name + ".mnu").c_str());
			std::vector<uint8_t> decoded;
			std::string message;
			if (path.empty() || !opennova::io::read_file_bytes(path, decoded, message)) { sweep_fail(totals, name, "missing fixture"); continue; }
			opennova::vfs_decode_payload(decoded);
			sweep_menu(std::string("fixtures/mnu/") + name + ".mnu", path, decoded, scratch.root(), totals);
		}
		print_totals("fixture leg", totals);
		failures += totals.failures;
	}
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (every .mnu the packed install serves, through MnuDocument)");
	} else {
		editor_test::TempProjectDir scratch("opennova_menu_retail_sweep");
		SweepTotals totals;
		std::map<std::string, std::vector<uint8_t>> seen;
		sweep_install_mount(install, std::string(), scratch.root(), seen, totals);
		for (const std::string &expansion : retail::expansions())
			sweep_install_mount(install, expansion, scratch.root(), seen, totals);
		if (totals.menus == 0) sweep_fail(totals, "<install>", "the packed install served no .mnu");
		print_totals("retail leg", totals);
		failures += totals.failures;
	}
	return failures ? 1 : 0;
}

} // namespace

// S11a: a field's value edited is a change, and a Set gives it back. An optional field's
// presence is a change of its own: EXIT's left edge left out (the same value) is changed
// and a Write gives it back; GROUP, left out in the file, written is changed and a Clear
// takes it back. A window's children reordered mark the window that holds them, not the
// children or the screen.
int changes_since_save() {
	editor_test::TempProjectDir dir("opennova_menu_changes_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Changes"));
	editor_test::create_missing_files(session);
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	auto *document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document);
	if (!document) return 1;
	using Change = Document::RecordChange;
	const Node &screen = *document->rows()[0];
	const NodeAddress screen_address{screen.id, kScreen, 0};
	const NodeAddress root{screen.id, kWindow, document->window_at(screen, 0)};
	NodeAddress exit, title;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "EXIT", exit) &&
			find_definition(AssetGraph(), *document, "TITLE", title));
	Diagnostic error;
	const std::string saved_text = text_of(*document, title, "string.value");
	TEST_EXPECT(document->apply(set(title, "string.value", std::string("Another title")), error));
	TEST_EXPECT(document->field_changed(title, "string.value") && !document->field_changed(title, "name"));
	TEST_EXPECT(document->record_change(title) == Change::Changed && document->record_change(root) == Change::Unchanged);
	std::vector<Edit> back = document->revert_edits(title, "string.value");
	TEST_EXPECT(back.size() == 1 && back[0].operation == EditOperation::Set &&
	            std::get<std::string>(back[0].value) == saved_text);
	TEST_EXPECT(document->apply(back, error) && !document->field_changed(title, "string.value") &&
	            text_of(*document, title, "string.value") == saved_text && document->record_change(title) == Change::Unchanged);
	document->undo();
	document->undo();
	TEST_EXPECT(!document->dirty());
	Edit clear = op(EditOperation::Clear, exit);
	clear.field = "position.left";
	TEST_EXPECT(document->apply(clear, error) && document->field_changed(exit, "position.left"));
	TEST_EXPECT(document->record_change(exit) == Change::Changed && document->record_change(root) == Change::Unchanged);
	back = document->revert_edits(exit, "position.left");
	TEST_EXPECT(back.size() == 1 && back[0].operation == EditOperation::Write);
	TEST_EXPECT(document->apply(back, error) && !document->field_changed(exit, "position.left") &&
	            document->present(exit, "position.left") && document->record_change(exit) == Change::Unchanged);
	document->undo();
	document->undo();
	TEST_EXPECT(!document->dirty());
	Edit write = op(EditOperation::Write, exit);
	write.field = "group";
	TEST_EXPECT(document->apply(write, error) && document->field_changed(exit, "group"));
	back = document->revert_edits(exit, "group");
	TEST_EXPECT(back.size() == 1 && back[0].operation == EditOperation::Clear);
	TEST_EXPECT(document->apply(back, error) && !document->field_changed(exit, "group") && !document->present(exit, "group"));
	document->undo();
	document->undo();
	TEST_EXPECT(document->apply(op(EditOperation::Move, exit, 0, 0), error));
	TEST_EXPECT(document->record_change(root) == Change::Changed && document->record_change(exit) == Change::Unchanged &&
	            document->record_change(title) == Change::Unchanged && document->record_change(screen_address) == Change::Unchanged);
	return 0;
}

// S12 D6: a style colour (a FONT colour, an APPEARANCE of type COLOR or OUTLINE, a COLOR ITEM)
// is a hex word the Inspector draws a swatch for, a %VAR% taken as well; an IMAGE's value is
// none; a CURSOR's and an APPEARANCE's FLAGS offer "Not written" (no text: the element left
// out) and the material flag tokens the parse knows, and take any text; a BODY's
// BITMAP_FLAGS and the header's sort stay free text.
int colours_and_flags() {
	editor_test::TempProjectDir dir("opennova_menu_colours_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Colours"));
	editor_test::create_missing_files(session);
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	auto *document = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(document);
	if (!document) return 1;
	NodeAddress exit;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "EXIT", exit));
	auto schema = [&](const NodeAddress &at, const char *id) {
		for (const FieldSchema &field : document->fields(at.kind))
			if (field.id == id) return document->field_on(at, field);
		return FieldUse();
	};
	for (const char *id : {"font.default_fg", "font.default_bg", "font.mouseover_fg", "font.mouseover_bg", "font.selected_fg",
	                       "font.selected_bg", "font.disabled_fg", "font.disabled_bg"})
		TEST_EXPECT(schema(exit, id).color == FieldColor::HexArgb && schema(exit, id).reference == ReferenceKind::StyleVar);
	Diagnostic error;
	TEST_EXPECT(document->apply(set(exit, "font.default_fg", std::string("%DEF_TEXTCOLOR%")), error));
	const NodeAddress appearance = child_of(*document, exit, "appearance");
	TEST_EXPECT(document->apply({set(appearance, "type", std::string("COLOR")), set(appearance, "value", std::string("FF102030"))},
	                            error));
	TEST_EXPECT(schema(appearance, "value").color == FieldColor::HexArgb);
	TEST_EXPECT(document->apply(set(appearance, "type", std::string("IMAGE")), error));
	TEST_EXPECT(schema(appearance, "value").color == FieldColor::None &&
	            schema(appearance, "value").reference == ReferenceKind::MenuTexture);
	const FieldSchema &flags = *schema(appearance, "flags").schema, &cursor = *schema(exit, "cursor.flags").schema;
	// No text first (the element left out), then the table's 43 tokens.
	TEST_EXPECT(flags.open_choices && flags.choices.size() == 44 && cursor.open_choices && cursor.choices.size() == 44);
	TEST_EXPECT(flags.choices[0].name.empty() && flags.choices[0].label == "Not written" && cursor.choices[0].name.empty() &&
	            cursor.choices[0].label == "Not written");
	bool transparent = false;
	for (const FieldChoice &choice : cursor.choices)
		transparent = transparent || (choice.name == "STANDARD_TRANSPARENT" && choice.value == 0x300651);
	TEST_EXPECT(transparent);
	TEST_EXPECT(document->apply(set(exit, "cursor.flags", std::string("STANDARD|NATIVE2X")), error) &&
	            text_of(*document, exit, "cursor.flags") == "STANDARD|NATIVE2X");
	// Picking "Not written" clears it: the file leaves the element out again.
	TEST_EXPECT(document->apply({set(exit, "cursor.file", std::string("cursor.tga")),
	                             set(appearance, "flags", std::string("NOCHECKDEPTH"))},
	                            error));
	std::string text = document->serialize().text;
	TEST_EXPECT(text.find("STANDARD|NATIVE2X") != std::string::npos && text.find("NOCHECKDEPTH") != std::string::npos);
	TEST_EXPECT(document->apply({set(exit, "cursor.flags", cursor.choices[0].name), set(appearance, "flags", flags.choices[0].name)},
	                            error));
	text = document->serialize().text;
	TEST_EXPECT(text_of(*document, exit, "cursor.flags").empty() && text_of(*document, appearance, "flags").empty() &&
	            text.find("STANDARD|NATIVE2X") == std::string::npos && text.find("NOCHECKDEPTH") == std::string::npos);
	for (const FieldSchema &field : document->fields(menu_kind("column.body")))
		if (field.id == "bitmap_flags") TEST_EXPECT(field.choices.empty() && !field.open_choices);
	return 0;
}

// S13 D6: a load in place starts the revisions again at 0 and gives the rows their identities from
// 1 again, so the menu's own memos (the menu the game would read were it saved, the names no
// lookup returns) key on the load generation as well: two screens named A (the first shadowed),
// then the file loaded again in place as A and B, at the same revision and with the same row
// identities, answer from the new file.
int memos_follow_a_load_in_place() {
	editor_test::TempProjectDir dir("opennova_menu_load_in_place");
	const std::string path = dir.file("twice.mnu");
	const auto screen = [](const char *name) {
		return std::string("<SCREEN>\r\n\t<NAME>") + name +
		       "</NAME>\r\n\t<WINDOW type=\"window\" name=\"MAIN\">"
		       "<POSITION><LEFT>0</LEFT></POSITION></WINDOW>\r\n</SCREEN>\r\n";
	};
	TEST_EXPECT(editor_test::write_text(path, screen("A") + screen("A")));
	MnuDocument document;
	TEST_EXPECT(load(document, path) && document.rows().size() == 2);
	const NodeAddress first{document.rows()[0]->id, kScreen, 0};
	SymbolFacts shadowed;
	document.refine_symbol(first, shadowed);
	std::shared_ptr<const mnu::Document> image = document.saved_image();
	TEST_EXPECT(shadowed.inert && image && image->screens.size() == 2 &&
	            image->screens[1].name == "A");
	const uint64_t generation = document.load_generation();
	TEST_EXPECT(editor_test::write_text(path, screen("A") + screen("B")));
	TEST_EXPECT(load(document, path) && document.revision() == 0 &&
	            document.load_generation() != generation);
	TEST_EXPECT(document.rows().size() == 2 && document.rows()[0]->id == first.row);
	SymbolFacts found;
	document.refine_symbol(first, found);
	image = document.saved_image();
	TEST_EXPECT(!found.inert && image && image->screens.size() == 2 &&
	            image->screens[1].name == "B");
	return 0;
}

// A window's parts in words (the plain-words lane, the audit's 5.1): each by what it does or shows, as
// the outline and the Inspector title it (record_display over the menu row's record_label), never "Action 2".
int parts_in_words() {
	editor_test::TempProjectDir dir("opennova_menu_parts_in_words");
	const std::string path = dir.file("words.mnu");
	TEST_EXPECT(editor_test::write_text(
			path, "<SCREEN>\r\n\t<NAME>MAIN</NAME>\r\n\t<WINDOW type=\"button\" name=\"GO\">\r\n"
			      "\t\t<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">btn.tga</APPEARANCE>\r\n"
			      "\t\t<APPEARANCE STATE=\"MOUSEOVER\" TYPE=\"COLOR\">%TRIM_COLOR%</APPEARANCE>\r\n"
			      "\t\t<APPEARANCE STATE=\"SELECTED\"></APPEARANCE>\r\n"
			      "\t\t<SOUND STATE=\"MOUSEIN\" TRIGGER=\"MOUSE_OVER\">click.lwf</SOUND>\r\n"
			      "\t\t<ACTION TYPE=\"SCREEN\" FILE=\"options.mnu\">OPTIONS</ACTION>\r\n"
			      "\t\t<ACTION TYPE=\"WINDOW\" STATE=\"SHOW\" TOGGLE>PANEL</ACTION>\r\n"
			      "\t\t<ACTION TYPE=\"WINDOW\" STATE=\"HIDE\">PANEL</ACTION>\r\n"
			      "\t\t<ACTION TYPE=\"POP_SCREEN\"></ACTION>\r\n"
			      "\t\t<ACTION TYPE=\"JUMP\">TITLE</ACTION>\r\n"
			      "\t\t<HOTKEY>O</HOTKEY>\r\n"
			      "\t</WINDOW>\r\n</SCREEN>\r\n"));
	MnuDocument document;
	TEST_EXPECT(load(document, path));
	NodeAddress go;
	TEST_EXPECT(find_definition(AssetGraph(), document, "GO", go));
	const auto words = [&](const char *token, size_t index) {
		return record_display(document, child_of(document, go, token, index), nullptr);
	};
	TEST_EXPECT(words("appearance", 0) == "Normal: image btn.tga");
	TEST_EXPECT(words("appearance", 1) == "Mouse over: colour %TRIM_COLOR%");
	TEST_EXPECT(words("appearance", 2) == "Selected (marks the state only)");
	TEST_EXPECT(words("sound", 0) == "Mouse enters: MOUSE_OVER (click.lwf)");
	TEST_EXPECT(words("action", 0) == "Go to OPTIONS in options.mnu");
	TEST_EXPECT(words("action", 1) == "Show or hide PANEL");
	TEST_EXPECT(words("action", 2) == "Hide PANEL");
	TEST_EXPECT(words("action", 3) == "Go back");
	// A type the game's parse knows no code of does nothing [orig: CUIElement_ParseXMLDefinition @ 0x648ee2].
	TEST_EXPECT(words("action", 4) == "Does nothing (JUMP is no action type)");
	TEST_EXPECT(words("hotkey", 0) == "Key O");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	return parts_in_words() || memos_follow_a_load_in_place() || colours_and_flags() || changes_since_save() || structure_and_save() || validation() || windows_at_depth() || every_list() || defaults_survive() ||
	       window_index_matches_the_compiler() || copy_and_paste() || duplicate_selection() || copy_between_encodings() ||
	       texts_in_their_code_page() ||
	       copy_selection_shapes() || duplicate_screen() || typed_add_and_screen_copy() || screens_stay_found() ||
	       nested_lists() || shipped_shape_edits() ||
	       typed_clear_and_retype() || parse_notes() || blank_menu_edits() || name_is_its_own_edit() ||
	       lookup_matches_the_runtime() ||
	       retail_sweep();
}
