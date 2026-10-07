// The session's wire form (ADR 0046 d10, the editor MCP): every request kind, edit
// operation, pick purpose, unsaved choice, selection mode, severity and Problems scope and
// grouping has a token that reads back; a request survives the JSON round trip with its
// edits in the batch form (S13 A5: each record by its identity in the document the request
// acts on, or by the label an earlier add of the batch gave; an add's kind by its token; a
// record added straight into a row naming the row; a fix's add in a closed table naming its
// kind by the type its path opens), its paths, names and imports, and so does the request of
// every kind of fix; a malformed request or edit is refused with a reason naming its place;
// the project settings are one request whose settings are each optional, and the dialogs
// section says what the last one came to; the view is written by section (view_json: the
// project, the requirements, the documents, the selection, the operation, Play, the import
// dialog, the dialogs, the counts, the preferences, the output and the events held) and its
// lists by page (the files, the import plan, the output lines by absolute index, the events by
// seq, the last 64 held); and over a real session the sections (the selection, the events a
// request posts when it asks a record's field or a file shown, the clipboard), a document with
// its records at every depth, a record (its path, locator, owner and fields as they apply) and
// the findings serialize the state the windows draw, with the same edit reaching the record
// through JSON as through the typed request; the Problems answer carries the counts, a page of
// its rows with the groups, what each finding is about and its fixes, whose requests read back
// and do what they say; the unsaved prompt names what waits and its files, and its "save"
// answer writes them and runs what waited.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <editor/documents/animation_map_document.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/import/import_plan.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_query.h>
#include <editor/session/project_session.h>
#include <editor/session/record_batch.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_fields.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view_json.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;
namespace fs = std::filesystem;

namespace {

using editor_test::NoProcess;

bool parse(const char *text, JsonValue &out) {
	std::string error;
	return opennova::io::json_parse(text, out, error);
}

// Parse a request; the error text (or "") comes back for the negative cases.
std::string request_error(const char *text, EditorRequest &out) {
	JsonValue json;
	std::string error;
	if (!opennova::io::json_parse(text, json, error)) return "json: " + error;
	return editor_request_from_json(json, out, error) ? std::string() : error;
}

// Parse a request whose edits are named in `names`, the record document it acts on (S13 A5).
std::string request_error_in(const std::string &text, const Document *names, EditorRequest &out) {
	JsonValue json;
	std::string error;
	if (!opennova::io::json_parse(text, json, error)) return "json: " + error;
	RequestNames in;
	in.document = names;
	return editor_request_from_json(json, out, error, &in) ? std::string() : error;
}

const JsonValue *find_row(const JsonValue &document, const char *name) {
	const JsonValue *rows = document.get("rows");
	if (!rows) return nullptr;
	for (const JsonValue &row : rows->array)
		if (row.get_string("name", "") == name) return &row;
	return nullptr;
}

const JsonValue *find_field(const JsonValue &record, const char *id) {
	const JsonValue *fields = record.get("fields");
	if (!fields) return nullptr;
	for (const JsonValue &field : fields->array)
		if (field.get_string("id", "") == id) return &field;
	return nullptr;
}

// A session over a project with its required files and the startup menu open (S13 A5): the
// record document a request's edits are named in, and two records they name, the screen's MAIN
// window and TITLE inside it.
struct OpenMenu {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	const Document *menu = nullptr;
	NodeAddress main, title;

	explicit OpenMenu(const char *name) : dir(name), session(platform, preferences) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Names"));
		editor_test::create_missing_files(session);
		editor_test::handle_to_end(session, request::open_document("main.mnu"));
		menu = session.document_for("main.mnu");
		if (!menu) return;
		find_definition(AssetGraph(), *menu, "MAIN", main);
		find_definition(AssetGraph(), *menu, "TITLE", title);
	}
};

} // namespace

static int test_tokens() {
	// Every kind to the enum's end has a token that reads back to it, and no two share one: its
	// row's in the request table (S13 A4), which the wire reads.
	for (int i = 0; i < static_cast<int>(EditorRequestKind::kCount); ++i) {
		const auto kind = static_cast<EditorRequestKind>(i);
		const std::string token = editor_request_kind_token(kind);
		TEST_EXPECT(!token.empty() && token == request_kind_row(kind).token && request_kind_row(kind).kind == kind);
		EditorRequestKind back = EditorRequestKind::Rescan;
		TEST_EXPECT(editor_request_kind_from_token(token, back) && back == kind);
		TEST_EXPECT(request_kind_from_token(token, back) && back == kind);
	}
	TEST_EXPECT(editor_request_kind_tokens().size() == kEditorRequestKindCount);
	TEST_EXPECT(std::string(editor_request_kind_token(EditorRequestKind::CancelOperation)) == "cancel_operation");
	TEST_EXPECT(editor_request_kind_tokens().front() == "new_project");
	for (int i = 0; i <= static_cast<int>(EditOperation::Apply); ++i) {
		const auto operation = static_cast<EditOperation>(i);
		EditOperation back = EditOperation::Set;
		TEST_EXPECT(*edit_operation_token(operation) && edit_operation_from_token(edit_operation_token(operation), back) &&
		            back == operation);
	}
	for (int i = 0; i <= static_cast<int>(PickPurpose::TextureImage); ++i) {
		const auto purpose = static_cast<PickPurpose>(i);
		PickPurpose back = PickPurpose::None;
		TEST_EXPECT(*pick_purpose_token(purpose) && pick_purpose_from_token(pick_purpose_token(purpose), back) &&
		            back == purpose);
	}
	for (int i = 0; i <= static_cast<int>(SelectMode::Toggle); ++i) {
		const auto mode = static_cast<SelectMode>(i);
		SelectMode back = SelectMode::Replace;
		TEST_EXPECT(*select_mode_token(mode) && select_mode_from_token(select_mode_token(mode), back) && back == mode);
	}
	for (int i = 0; i <= static_cast<int>(UnsavedChoice::Cancel); ++i) {
		const auto choice = static_cast<UnsavedChoice>(i);
		UnsavedChoice back = UnsavedChoice::Cancel;
		TEST_EXPECT(*unsaved_choice_token(choice) && unsaved_choice_from_token(unsaved_choice_token(choice), back) &&
		            back == choice);
	}
	for (int i = 0; i <= static_cast<int>(DiagnosticSeverity::Error); ++i) {
		const auto severity = static_cast<DiagnosticSeverity>(i);
		DiagnosticSeverity back = DiagnosticSeverity::Info;
		TEST_EXPECT(diagnostic_severity_from_token(diagnostic_severity_label(severity), back) && back == severity);
	}
	for (int i = 0; i <= static_cast<int>(ProblemScope::OpenFiles); ++i) {
		const auto scope = static_cast<ProblemScope>(i);
		ProblemScope back = ProblemScope::Project;
		TEST_EXPECT(*problem_scope_token(scope) && problem_scope_from_token(problem_scope_token(scope), back) && back == scope);
	}
	for (int i = 0; i <= static_cast<int>(ProblemGrouping::Kind); ++i) {
		const auto grouping = static_cast<ProblemGrouping>(i);
		ProblemGrouping back = ProblemGrouping::None;
		TEST_EXPECT(*problem_grouping_token(grouping) && problem_grouping_from_token(problem_grouping_token(grouping), back) &&
		            back == grouping);
	}
	DiagnosticSeverity severity = DiagnosticSeverity::Info;
	TEST_EXPECT(!diagnostic_severity_from_token("Error", severity) && !diagnostic_severity_from_token("errors", severity));
	EditorRequestKind kind = EditorRequestKind::Rescan;
	TEST_EXPECT(!editor_request_kind_from_token("NewProject", kind));
	TEST_EXPECT(!editor_request_kind_from_token("", kind));
	// A new file of any kind a blank factory makes (S11d): create_file, the old name gone.
	TEST_EXPECT(std::string(editor_request_kind_token(EditorRequestKind::CreateFile)) == "create_file");
	TEST_EXPECT(!editor_request_kind_from_token("create_document", kind));
	// Output's Clear (S11e).
	TEST_EXPECT(editor_request_kind_from_token("clear_output", kind) && kind == EditorRequestKind::ClearOutput);
	// The game install's import (S13 A4: preview_install_import, the retail token gone).
	TEST_EXPECT(editor_request_kind_from_token("preview_install_import", kind) &&
	            kind == EditorRequestKind::PreviewInstallImport);
	TEST_EXPECT(!editor_request_kind_from_token("preview_retail_import", kind));
	return 0;
}

// The asset kinds' tokens on the wire (S13 D5): a scanned file's kind is its row's token, the
// sound banks as the game names them (a .lwf the sound bank, a .sbf the music bank) and the
// kinds S13 D5 added; the reference kind a menu SOUND names its bank by is sound_bank too, and
// wave_bank names nothing (pre-1.0, no alias). The files are the files query's (S13 A5).
static int test_asset_kind_tokens() {
	editor_test::TempProjectDir dir("opennova_session_json_kinds");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Kinds"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(!root.empty());
	const std::pair<const char *, const char *> files[] = {
	        {"sounds/menu.lwf", "sound_bank"},
	        {"music/menumus.sbf", "music_bank"},
	        {"sounds/boom.wav", "wave"},
	        {"faces/head.grm", "face_animation"},
	        {"CC.BIN", "country_code"},
	        {"score.ini", "score"},
	        {"missions/ASP.npz", "map_project"},
	        {"game.ini", "config"},
	};
	for (const auto &file : files)
		TEST_EXPECT(editor_test::write_text(root + "/" + file.first, "x"));
	editor_test::handle_to_end(session, request::rescan());
	JsonValue args = JsonValue::make_object();
	args.set("limit", JsonValue::make_number(200.0));
	std::string error;
	const JsonValue listed = session.query("files", args, error);
	TEST_EXPECT(error.empty() && listed.get("files") != nullptr);
	if (!listed.get("files")) return 1;
	for (const auto &file : files) {
		const std::string name = std::filesystem::path(file.first).filename().string();
		std::string kind;
		for (const JsonValue &row : listed.get("files")->array)
			if (row.get_string("name", "") == name) kind = row.get_string("kind", "");
		if (kind != file.second)
			std::fprintf(stderr, "%s is %s on the wire\n", name.c_str(), kind.c_str());
		TEST_EXPECT(kind == file.second);
	}
	ReferenceKind reference = ReferenceKind::None;
	TEST_EXPECT(std::string(reference_row(ReferenceKind::SoundBank).token) == "sound_bank");
	TEST_EXPECT(reference_kind_from_token("sound_bank", reference));
	TEST_EXPECT(reference == ReferenceKind::SoundBank);
	TEST_EXPECT(!reference_kind_from_token("wave_bank", reference));
	TEST_EXPECT(asset_kind_from_token("wave_bank") == AssetKind::Unknown);
	return 0;
}

static int test_request_round_trip() {
	OpenMenu open("opennova_session_json_round_trip");
	TEST_EXPECT(open.menu != nullptr && open.title.child != 0 && open.main.child != 0);
	if (!open.menu) return 1;
	const Document &menu = *open.menu;
	const std::string path = menu.path();
	const NodeKind window = menu.kind_from_name("window");
	TEST_EXPECT(path == "menus/main.mnu" && window >= 0);
	RequestNames names;
	names.document = &menu;

	// An edit on one record: a batch of one (S13 A4: the single edit is edits of one), written in
	// the batch form (S13 A5): its op, the record by its identity, the field, the value, the fold.
	Edit hello;
	hello.address = open.title;
	hello.field = "string.value";
	hello.value = std::string("Hello");
	hello.coalesce = true;
	const EditorRequest request = request::edit_record(path, hello);
	JsonValue parsed;
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(request, &menu)).c_str(), parsed));
	const JsonValue *written =
			parsed.get("edits") && parsed.get("edits")->array.size() == 1 ? &parsed.get("edits")->array[0] : nullptr;
	TEST_EXPECT(written && written->get_string("op", "") == "set" &&
	            written->get_number("id", 0.0) == double(open.title.child) &&
	            written->get_string("field", "") == "string.value" && written->get_bool("coalesce", false) &&
	            written->get_string("value", "") == "Hello" && !written->get("row") && !written->get("child") &&
	            !written->get("kind") && !written->get("operation"));
	EditorRequest back;
	std::string error;
	TEST_EXPECT(editor_request_from_json(parsed, back, error, &names) && back == request);
	TEST_EXPECT(back.kind == EditorRequestKind::EditRecord && back.path == path && back.edits.size() == 1);
	if (back.edits.size() != 1) return 1;
	TEST_EXPECT(back.edits[0].operation == EditOperation::Set && back.edits[0].address == open.title &&
	            back.edits[0].field == "string.value" && std::get<std::string>(back.edits[0].value) == "Hello" &&
	            back.edits[0].coalesce && back.edits[0].position == SIZE_MAX && back.edits[0].parent == 0 &&
	            back.edits[0].gesture == 0);
	TEST_EXPECT(names.labels == std::vector<std::string>({""}));

	// An owner, a gesture, a batch and a selection mode.
	Edit move, left, clear;
	move.operation = EditOperation::Move;
	move.address = open.title;
	move.parent = open.main.child;
	move.position = 2;
	left.address = open.title;
	left.field = "position.left";
	left.value = int64_t(40);
	left.gesture = 5;
	clear.operation = EditOperation::Clear;
	clear.address = open.title;
	clear.field = "position.right";
	clear.gesture = 5;
	const EditorRequest batch = request::edit_record(path, std::vector<Edit>{move, left, clear}, true);
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(batch, &menu)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error, &names) && back == batch && back.open_first);
	TEST_EXPECT(back.edits.size() == 3 && back.edits[0].parent == open.main.child && back.edits[0].position == 2 &&
	            back.edits[1].field == "position.left" && std::get<int64_t>(back.edits[1].value) == 40 &&
	            back.edits[1].gesture == 5 && back.edits[2].operation == EditOperation::Clear &&
	            back.edits[2].gesture == 5);
	// A record added and filled in by the label its add gave (the batch form's labels, S9m): the
	// add names its owner, its kind by its token; the set names the record the add makes. The
	// writer labels the add ("edit<i>") where a later edit names what it makes; the reader gives
	// back each making edit's label.
	Edit add, named, into_row, duplicate, file_value;
	add.operation = EditOperation::Add;
	add.address = {open.main.row, window, 0};
	add.parent = open.main.child;
	named.address = {open.main.row, window, batch_made(0)};
	named.field = "name";
	named.value = std::string("HELLO");
	// Added straight into the screen's row: the row's identity is its owner on the wire.
	into_row.operation = EditOperation::Add;
	into_row.address = {open.main.row, window, 0};
	duplicate.operation = EditOperation::Duplicate;
	duplicate.address = open.title;
	duplicate.position = 1;
	file_value.operation = EditOperation::SetFileValue;
	file_value.field = "encoding";
	file_value.value = int64_t(1);
	const EditorRequest labelled = request::edit_record(path, std::vector<Edit>{add, named, into_row, duplicate, file_value});
	const JsonValue labelled_json = editor_request_to_json(labelled, &menu);
	const JsonValue *edits = labelled_json.get("edits");
	TEST_EXPECT(edits && edits->array.size() == 5);
	if (!edits || edits->array.size() != 5) return 1;
	TEST_EXPECT(edits->array[0].get_string("op", "") == "add" && edits->array[0].get_string("kind", "") == "window" &&
	            edits->array[0].get_number("parent", 0.0) == double(open.main.child) &&
	            edits->array[0].get_string("as", "") == "edit0" && !edits->array[0].get("id"));
	TEST_EXPECT(edits->array[1].get_string("op", "") == "set" && edits->array[1].get_string("id", "") == "edit0" &&
	            edits->array[1].get_string("value", "") == "HELLO");
	TEST_EXPECT(edits->array[2].get_number("parent", 0.0) == double(open.main.row) && !edits->array[2].get("as"));
	TEST_EXPECT(edits->array[3].get_string("op", "") == "duplicate" && edits->array[3].get_int("position", -1) == 1 &&
	            edits->array[3].get_number("id", 0.0) == double(open.title.child));
	TEST_EXPECT(edits->array[4].get_string("op", "") == "set_file_value" && !edits->array[4].get("id") &&
	            edits->array[4].get_string("field", "") == "encoding");
	TEST_EXPECT(editor_request_from_json(labelled_json, back, error, &names) && back == labelled);
	TEST_EXPECT(names.labels == std::vector<std::string>({"edit0", "", "", "", ""}));
	// A label given by a client: read as the edit it names, and the label kept for the outcome.
	const std::string by_label = "{\"kind\":\"edit_record\",\"path\":\"" + path +
	                             "\",\"edits\":[{\"op\":\"add\",\"kind\":\"window\",\"parent\":" +
	                             std::to_string(open.main.child) + ",\"as\":\"w\"},{\"op\":\"set\",\"id\":\"w\","
	                                                               "\"field\":\"name\",\"value\":\"HELLO\"}]}";
	TEST_EXPECT(parse(by_label.c_str(), parsed) && editor_request_from_json(parsed, back, error, &names));
	TEST_EXPECT(back.edits.size() == 2 && back.edits[0] == add && back.edits[1] == named &&
	            names.labels == std::vector<std::string>({"w", ""}));
	// A row a batch makes (S13 D7): a screen added, named through its label as a row, a window put
	// in it. The writer labels the add, whose row the later edits name; read back, the batch applies
	// as one step making the screen and its window.
	const NodeKind screen = menu.kind_from_name("screen");
	Edit new_screen, name_screen, window_in;
	new_screen.operation = EditOperation::Add;
	new_screen.address = {0, screen, 0};
	name_screen.address = {batch_made(0), screen, 0};
	name_screen.field = "name";
	name_screen.value = std::string("EXTRA");
	window_in.operation = EditOperation::Add;
	window_in.address = {batch_made(0), window, 0};
	const JsonValue rows_json =
	        editor_request_to_json(request::edit_record(path, std::vector<Edit>{new_screen, name_screen, window_in}), &menu);
	const JsonValue *row_edits = rows_json.get("edits");
	TEST_EXPECT(row_edits && row_edits->array.size() == 3 && row_edits->array[0].get_string("as", "") == "edit0" &&
	            row_edits->array[1].get_string("id", "") == "edit0" &&
	            row_edits->array[2].get_string("parent", "") == "edit0");
	TEST_EXPECT(editor_request_from_json(rows_json, back, error, &names) && back.edits.size() == 3);
	const size_t screens = menu.rows().size();
	editor_test::handle_to_end(open.session, back);
	TEST_EXPECT(open.session.outcome().done() && menu.rows().size() == screens + 1 &&
	            menu.rows().back()->name() == "EXTRA" && menu.last_added_records().size() == 2);
	editor_test::handle_to_end(open.session, request::undo(path));
	TEST_EXPECT(menu.rows().size() == screens);
	// A row's copy named by its label (S13 D7's second review): the copy is a row of its own, so a
	// Set naming the label, written and read back as it was, applies to the copy; a duplicate naming
	// no place reads back naming none, and the core puts the copy right after its record.
	Edit copy_screen, name_copy;
	copy_screen.operation = EditOperation::Duplicate;
	copy_screen.address = {menu.rows()[0]->id, screen, 0};
	name_copy.address = {0, screen, batch_made(0)};
	name_copy.field = "name";
	name_copy.value = std::string("COPIED");
	const EditorRequest copy_request = request::edit_record(path, std::vector<Edit>{copy_screen, name_copy});
	const JsonValue copy_json = editor_request_to_json(copy_request, &menu);
	const JsonValue *copy_edits = copy_json.get("edits");
	TEST_EXPECT(copy_edits && copy_edits->array.size() == 2 && !copy_edits->array[0].get("position") &&
	            copy_edits->array[0].get_string("as", "") == "edit0" && copy_edits->array[1].get_string("id", "") == "edit0");
	TEST_EXPECT(editor_request_from_json(copy_json, back, error, &names) && back == copy_request &&
	            names.labels == std::vector<std::string>({"edit0", ""}));
	editor_test::handle_to_end(open.session, back);
	TEST_EXPECT(open.session.outcome().done() && menu.rows().size() == screens + 1 && menu.rows()[1]->name() == "COPIED");
	editor_test::handle_to_end(open.session, request::undo(path));
	TEST_EXPECT(menu.rows().size() == screens);
	// A Paste has no batch form (the paste request carries the clipboard): written by its op, which
	// the reader refuses.
	Edit paste;
	paste.operation = EditOperation::Paste;
	paste.address = {open.main.row, window, 0};
	paste.value = std::string("payload");
	const JsonValue pasted = editor_request_to_json(request::edit_record(path, paste), &menu);
	TEST_EXPECT(pasted.get("edits") && pasted.get("edits")->array.size() == 1 &&
	            pasted.get("edits")->array[0].get_string("op", "") == "paste");
	TEST_EXPECT(!editor_request_from_json(pasted, back, error, &names) && error.find("paste") != std::string::npos);

	const EditorRequest select = request::select_record(path, open.title, SelectMode::Toggle);
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(select)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.mode == SelectMode::Toggle && back.address == select.address);
	// A marquee's records with its primary (S13 D7), of any rows; one that is no address is refused
	// by its place, and records that are no list by their name.
	const EditorRequest marquee =
	        request::select_record("menus/main.mnu", {7, 1, 9}, SelectMode::Add, {{8, 0, 0}, {9, 1, 12}});
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(marquee)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back == marquee && back.records.size() == 2 &&
	            back.records[1] == NodeAddress({9, 1, 12}));
	TEST_EXPECT(request_error("{\"kind\":\"select_record\",\"address\":{\"row\":1},\"records\":[{\"row\":2},{\"rows\":3}]}", back)
	                    .find("records[1]") != std::string::npos);
	// A member's value refused by its place too (S13 D7's second review).
	TEST_EXPECT(request_error("{\"kind\":\"select_record\",\"address\":{\"row\":1},\"records\":[{\"row\":2},{\"row\":\"x\"}]}", back) ==
	            "\"records[1].row\" must be a record identity.");
	TEST_EXPECT(request_error("{\"kind\":\"select_record\",\"address\":{\"row\":1,\"child\":-1}}", back) ==
	            "\"address.child\" must be a record identity.");
	TEST_EXPECT(request_error("{\"kind\":\"select_record\",\"address\":{\"row\":1},\"records\":{\"row\":2}}", back)
	                    .find("records") != std::string::npos);
	// A Paste's place: its row, its owner and its index; none named, after the selection.
	TEST_EXPECT(request_error("{\"kind\":\"paste\",\"paste_at\":{\"parent\":4,\"position\":1}}", back).empty());
	TEST_EXPECT(back.kind == EditorRequestKind::Paste && back.paste_at.parent == 4 && back.paste_at.position == 1 &&
	            back.paste_at.row == 0 && back.paste_at.named());
	TEST_EXPECT(request_error("{\"kind\":\"paste\"}", back).empty() && !back.paste_at.named());
	// A Go to (S12 D3): the document opened at a record by its locator, its field shown; the
	// request by symbol it replaced is no token.
	const EditorRequest go_to = request::open_document("gametext.bin", "1/string:0", "key");
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(go_to)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.kind == EditorRequestKind::OpenDocument &&
	            back.path == "gametext.bin" && back.locator == "1/string:0" && back.field == "key" && back == go_to);
	EditorRequestKind gone = EditorRequestKind::Rescan;
	TEST_EXPECT(!editor_request_kind_from_token("go_to_record", gone));

	ImportChoice native; // a loose file copied as the game's own (S11f)
	native.path = "C:/data/logo.png";
	native.native = true;
	ImportChoice install; // a file of the game install (S13 A4: install, was retail)
	install.path = "C:/games/JO";
	install.entry = "main.mnu";
	install.install = true;
	const EditorRequest import = request::import_files(
	        {{"C:/data/localres.pff", "MAIN.MNU"}, {"C:/data/loose.txt", ""}, native, install}, true);
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(import)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back == import);
	TEST_EXPECT(back.kind == EditorRequestKind::ImportFiles && back.replace && back.imports.size() == 4 &&
	            back.imports[0].entry == "MAIN.MNU" && back.imports[1].entry.empty());
	TEST_EXPECT(!back.imports[0].native && !back.imports[1].native && back.imports[2].native && !back.imports[2].install &&
	            back.imports[3].install);
	const JsonValue *sources = parsed.get("imports");
	TEST_EXPECT(sources && sources->array.size() == 4 && sources->array[3].get_bool("install", false) &&
	            !sources->array[3].get("retail"));
	TEST_EXPECT(request_error("{\"kind\":\"import_files\",\"imports\":[{\"path\":\"x\",\"native\":1}]}", back).find("native") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"import_files\",\"imports\":[{\"path\":\"x\",\"retail\":true}]}", back).find("retail") !=
	            std::string::npos);
	// S16: an install's file under the project's own name (the expansion's: jox01.bin as jxm.bin), its
	// `as`; one only an install's file has.
	ImportChoice renamed = install;
	renamed.entry = "jox01.bin";
	renamed.as = "jxm.bin";
	const EditorRequest as_import = request::import_files({renamed}, false);
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(as_import)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back == as_import && back.imports[0].name() == "jxm.bin" &&
	            parsed.get("imports")->array[0].get_string("as", "") == "jxm.bin");
	TEST_EXPECT(request_error("{\"kind\":\"import_files\",\"imports\":[{\"path\":\"x\",\"entry\":\"a.bin\",\"as\":\"b.bin\"}]}",
	                          back).find("install's file") != std::string::npos);

	const EditorRequest pick = request::pick_file(PickPurpose::RuntimeExecutable);
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(pick)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.purpose == PickPurpose::RuntimeExecutable);
	// The game install's pick (S13 A4: game_install, was retail_directory).
	TEST_EXPECT(std::string(pick_purpose_token(PickPurpose::GameInstall)) == "game_install");
	TEST_EXPECT(request_error("{\"kind\":\"pick_directory\",\"purpose\":\"retail_directory\"}", back).find("retail_directory") !=
	            std::string::npos);

	// The request of every kind of fix (problem_fixes.h) reads back as it was written, and a
	// fix's JSON carries it. A fix's edit (the missing anim_reset row: an add in a table not open,
	// its document opened first) names its kind by the token of the type its path opens.
	const EditorRequest create = request::create_missing({"main_menu", "gametext"});
	Edit reset_row;
	reset_row.operation = EditOperation::Add;
	reset_row.address.kind = node_kind(AnimationMapKind::Row);
	reset_row.field = "key";
	reset_row.value = std::string("anim_reset");
	const EditorRequest fixes[] = {create,
	                               request::preview_install_import({"MAIN.MNU"}, true), // with the files it needs (S11g)
	                               request::assign_requirement("main_menu", "menus/a.mnu"),
	                               request::create_file("Arial99.fnt", "font"),
	                               request::reimport("art/logo.png", true),
	                               request::save("defs/items.def"),
	                               request::show_in_files("strings/other.bin", true),
	                               request::reload_document("defs/items.def"),
	                               request::open_record("defs/items.def", {4, 2, 0}, "type"),
	                               request::edit_record("anims/soldier.adm", reset_row, true)};
	for (const EditorRequest &fix : fixes) {
		TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(fix)).c_str(), parsed));
		EditorRequest read;
		TEST_EXPECT(editor_request_from_json(parsed, read, error) && read == fix);
	}
	const JsonValue reset_json = editor_request_to_json(fixes[9]);
	TEST_EXPECT(reset_json.get("edits") && reset_json.get("edits")->array.size() == 1 &&
	            reset_json.get("edits")->array[0].get_string("kind", "") == "row" &&
	            reset_json.get("edits")->array[0].get_string("value", "") == "anim_reset" &&
	            reset_json.get_bool("open_first", false));
	const JsonValue fix_json = problem_fix_to_json(ProblemFix{"Create main.mnu", "Creates the startup screen.", create, true});
	TEST_EXPECT(fix_json.get_string("label", "") == "Create main.mnu" && fix_json.get_bool("bulk", false) &&
	            fix_json.get_string("detail", "") == "Creates the startup screen." && fix_json.get("request") != nullptr);
	if (const JsonValue *fix_request = fix_json.get("request"))
		TEST_EXPECT(editor_request_from_json(*fix_request, back, error) && back.kind == EditorRequestKind::CreateMissing &&
		            back.roles == create.roles && fix_request->get("roles") && !fix_request->get("names"));
	TEST_EXPECT(request_error("{\"kind\":\"create_missing\",\"roles\":\"main_menu\"}", back).find("roles") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"create_missing\",\"roles\":[3]}", back).find("roles") != std::string::npos);

	const EditorRequest resolve = request::resolve_unsaved(UnsavedChoice::Discard);
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(resolve)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.choice == UnsavedChoice::Discard);
	// The prompt's Save writes exactly the files it lists: "save" (no longer "save_all").
	TEST_EXPECT(std::string(unsaved_choice_token(UnsavedChoice::Save)) == "save");
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"choice\":\"save\"}", back).empty() &&
	            back.choice == UnsavedChoice::Save);
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"choice\":\"save_all\"}", back).find("save_all") !=
	            std::string::npos);
	// A Cancel is written too: a kind that must carry a field writes it at its default.
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(request::resolve_unsaved(UnsavedChoice::Cancel))).c_str(),
	                  parsed) &&
	            parsed.get_string("choice", "") == "cancel");

	// Numbers: a whole number is an integer value, a fraction a real, a bool 0 / 1.
	const std::string title_id = std::to_string(open.title.child);
	const std::string set_on_title = "{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"set\",\"id\":" + title_id +
	                                 ",\"field\":\"hp\",\"value\":";
	TEST_EXPECT(request_error_in(set_on_title + "40}]}", &menu, back).empty());
	TEST_EXPECT(back.edits.size() == 1 && std::get<int64_t>(back.edits[0].value) == 40 && back.edits[0].address == open.title);
	TEST_EXPECT(request_error_in(set_on_title + "2.5}]}", &menu, back).empty());
	TEST_EXPECT(back.edits.size() == 1 && std::get<double>(back.edits[0].value) == 2.5);
	TEST_EXPECT(request_error_in(set_on_title + "true}]}", &menu, back).empty());
	TEST_EXPECT(back.edits.size() == 1 && std::get<int64_t>(back.edits[0].value) == 1);
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"move\",\"id\":" + title_id +
	                                     ",\"position\":0}]}",
	                    &menu, back)
	                    .empty());
	TEST_EXPECT(back.edits.size() == 1 && back.edits[0].operation == EditOperation::Move && back.edits[0].position == 0 &&
	            back.edits[0].address == open.title);
	TEST_EXPECT(request_error("{\"kind\":\"build\"}", back).empty() && back.kind == EditorRequestKind::Build);

	// Refusals name what is wrong: the fields S13 A4 retired name nothing, a field the kind does
	// not take is refused naming what it takes, and one it must carry is refused left out.
	TEST_EXPECT(request_error("[\"build\"]", back).find("object") != std::string::npos);
	TEST_EXPECT(request_error("{\"path\":\"x\"}", back).find("kind") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":5}", back).find("kind") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"nope\"}", back).find("nope") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"build\",\"flagg\":true}", back).find("flagg") != std::string::npos);
	for (const char *retired : {"text", "flag", "edit", "unsaved_choice"}) {
		const std::string json = std::string("{\"kind\":\"save\",\"") + retired + "\":true}";
		TEST_EXPECT(request_error(json.c_str(), back).find(std::string("Unknown request member \"") + retired) !=
		            std::string::npos);
	}
	// A retired member's refusal names what the kind takes: where its field went.
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"unsaved_choice\":\"save\"}", back) ==
			"Unknown request member \"unsaved_choice\" (resolve_unsaved takes choice).");
	TEST_EXPECT(request_error("{\"kind\":\"new_project\",\"text\":\"T\"}", back) ==
			"Unknown request member \"text\" (new_project takes dir, title, game, expansion, builds_on, base_project, game_install, import_pass).");
	TEST_EXPECT(request_error("{\"kind\":\"build\",\"flagg\":true}", back) ==
			"Unknown request member \"flagg\" (build takes out_dir, rehash, report).");
	TEST_EXPECT(request_error("{\"kind\":\"build\",\"path\":\"x\"}", back) == "build takes no \"path\" (it takes out_dir, rehash, report).");
	TEST_EXPECT(request_error("{\"kind\":\"open_project\",\"path\":\"C:/x\"}", back) ==
	            "open_project takes no \"path\" (it takes dir, game_install, import_pass).");
	TEST_EXPECT(request_error("{\"kind\":\"open_project\"}", back) ==
	            "open_project needs \"dir\" (it takes dir, game_install, import_pass).");
	// S13 A7's fields: a new project's game, a build's out_dir, an open without its import pass or on
	// a game install of the session's own; import_pass is true when left out and written only when
	// false, game_install written only when set.
	EditorRequest read;
	TEST_EXPECT(request_error("{\"kind\":\"open_project\",\"dir\":\"C:/x\"}", read).empty() && read.import_pass);
	TEST_EXPECT(request_error("{\"kind\":\"open_project\",\"dir\":\"C:/x\",\"import_pass\":false}", read).empty() &&
	            !read.import_pass && editor_request_to_json(read).get_bool("import_pass", true) == false);
	TEST_EXPECT(!editor_request_to_json(request::open_project("C:/x")).get("import_pass"));
	TEST_EXPECT(request_error("{\"kind\":\"open_project\",\"dir\":\"C:/x\",\"import_pass\":0}", read)
	                    .find("import_pass") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"open_project\",\"dir\":\"C:/x\",\"game_install\":\"C:/games/JO\"}", read)
	                    .empty() &&
	            read.game_install == "C:/games/JO" &&
	            editor_request_to_json(read).get_string("game_install", "") == "C:/games/JO");
	TEST_EXPECT(!editor_request_to_json(request::open_project("C:/x")).get("game_install"));
	TEST_EXPECT(request_error("{\"kind\":\"new_project\",\"dir\":\"C:/x\",\"import_pass\":false}", read).empty() &&
	            !read.import_pass && read.title.empty());
	// A new project names its game install too (the UX round's project lane).
	TEST_EXPECT(request_error("{\"kind\":\"new_project\",\"dir\":\"C:/x\",\"game_install\":\"C:/g\"}", read).empty() &&
	            read.game_install == "C:/g");
	TEST_EXPECT(editor_request_to_json(request::new_project("C:/x", "T", "dfx")).get_string("game", "") == "dfx" &&
	            editor_request_to_json(request::build("C:/out")).get_string("out_dir", "") == "C:/out");
	// An expansion project (S16): its name and the expansion it builds on ride the request; a
	// standalone project writes neither.
	{
		const EditorRequest expansion = request::new_expansion_project("C:/x", "T", "jxm", "jox01");
		JsonValue written;
		TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(expansion)).c_str(), written));
		TEST_EXPECT(written.get_string("expansion", "") == "jxm" && written.get_string("builds_on", "") == "jox01");
		EditorRequest round;
		std::string round_error;
		TEST_EXPECT(editor_request_from_json(written, round, round_error) && round == expansion &&
		            round.expansion == "jxm" && round.builds_on == "jox01");
		TEST_EXPECT(!editor_request_to_json(request::new_project("C:/x", "T")).get("expansion") &&
		            !editor_request_to_json(request::new_project("C:/x", "T")).get("builds_on"));
		TEST_EXPECT(request_error("{\"kind\":\"new_project\",\"dir\":\"C:/x\",\"expansion\":\"jxm\"}", read).empty() &&
		            read.expansion == "jxm" && read.builds_on.empty());
		TEST_EXPECT(request_error("{\"kind\":\"new_project\",\"dir\":\"C:/x\",\"expansion\":3}", read)
		                    .find("expansion") != std::string::npos);
		TEST_EXPECT(request_error("{\"kind\":\"open_project\",\"dir\":\"C:/x\",\"builds_on\":\"jox01\"}", read)
		                    .find("builds_on") != std::string::npos);
	}
	TEST_EXPECT(request_error("{\"kind\":\"rename_asset\",\"path\":\"a.mnu\"}", back).find("needs \"new_name\"") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"save\",\"path\":3}", back).find("path") != std::string::npos);
	// An edit's refusal names it by its place in `edits` (the batch form, S13 A5).
	const std::string remove_title = "{\"op\":\"remove\",\"id\":" + title_id + "}";
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"remove\",\"id\":-1}]}", &menu, back) ==
	            "edits[0]: \"id\" must be a record identity or a label.");
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[" + remove_title + ",3]}", &menu, back) ==
	            "\"edits[1]\" must be an object.");
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[" + remove_title +
	                                     ",{\"op\":\"remove\",\"id\":-1}]}",
	                    &menu, back) == "edits[1]: \"id\" must be a record identity or a label.");
	TEST_EXPECT(request_error("{\"kind\":\"revert_to_saved\",\"edits\":[{\"rows\":1}]}", back) ==
			"Unknown edits[0] member \"rows\".");
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edits\":[{\"rows\":1}]}", back) ==
			"Unknown edits[0] member \"rows\".");
	// The retired edit members (the address form before S13 A5) name nothing.
	for (const char *retired : {"operation", "row", "child"}) {
		const std::string json = std::string("{\"kind\":\"edit_record\",\"edits\":[{\"") + retired + "\":1}]}";
		TEST_EXPECT(request_error(json.c_str(), back) ==
		            std::string("Unknown edits[0] member \"") + retired + "\".");
	}
	// A record no document holds, a label no earlier add gave, an identity with no document to
	// find it in, an op or a value the batch form does not know.
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"remove\",\"id\":999999}]}", &menu, back) ==
	            "edits[0]: no record 999999 in " + path + ".");
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"set\",\"id\":\"nobody\",\"field\":"
	                             "\"name\",\"value\":\"X\"}]}",
	                    &menu, back)
	                    .find("\"nobody\", which no earlier add or duplicate") != std::string::npos);
	TEST_EXPECT(request_error(("{\"kind\":\"edit_record\",\"edits\":[" + remove_title + "]}").c_str(), back)
	                    .find("none is open") != std::string::npos);
	TEST_EXPECT(!request_error(("{\"kind\":\"edit_record\",\"path\":\"menus/closed.mnu\",\"edits\":[" + remove_title + "]}")
	                                   .c_str(),
	                    back)
	                     .empty());
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"teleport\"}]}", &menu, back).find("teleport") !=
	            std::string::npos);
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"fold\"}]}", &menu, back).find("fold") !=
	            std::string::npos);
	TEST_EXPECT(request_error_in(set_on_title + "[1]}]}", &menu, back).find("\"value\"") != std::string::npos);
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"add\",\"kind\":\"gizmo\"}]}", &menu, back)
	                    .find("gizmo") != std::string::npos);
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"remove\",\"id\":" + title_id +
	                                     ",\"gesture\":-2}]}",
	                    &menu, back)
	                    .find("gesture") != std::string::npos);
	TEST_EXPECT(request_error_in("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"move\",\"id\":" + title_id +
	                                     ",\"parent\":\"x\",\"position\":0}]}",
	                    &menu, back)
	                    .find("\"parent\"") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edits\":{\"op\":\"set\"}}", back).find("edits") != std::string::npos);
	// A new name sent as a whole number (an item id) is its digits; a fraction names nothing.
	const char *numbered = "{\"kind\":\"rename_symbol\",\"path\":\"items.def\",\"locator\":\"L\","
						   "\"field\":\"id\",\"new_name\":100302}";
	TEST_EXPECT(request_error(numbered, back).empty() && back.new_name == "100302");
	TEST_EXPECT(request_error("{\"kind\":\"rename_asset\",\"path\":\"a.mnu\",\"new_name\":2.5}",
						back) == "\"new_name\" must be a string or a whole number.");
	TEST_EXPECT(request_error("{\"kind\":\"pick_file\",\"purpose\":\"anything\"}", back).find("anything") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"import_files\",\"imports\":[{\"entry\":\"X\"}]}", back).find("path") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"choice\":\"later\"}", back).find("later") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"select_record\",\"address\":{\"row\":1},\"mode\":\"extend\"}", back).find("extend") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"select_record\",\"address\":{\"rows\":1}}", back).find("rows") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"paste\",\"paste_at\":{\"parent\":\"x\"}}", back).find("parent") != std::string::npos);
	return 0;
}

// A sample request of `kind`: every field its row takes set away from its default, each field
// by its own sample (S13 A4), its edits naming the records of `open`'s menu (S13 A5: the batch
// form names a record by its identity there). A field added to the struct without a sample here,
// or a kind whose row takes it, fails test_request_table_samples.
static EditorRequest table_sample(EditorRequestKind kind, const OpenMenu &open) {
	using F = RequestFieldId;
	EditorRequest out = request::of(kind);
	const RequestParams &params = request_kind_row(kind).params;
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<F>(i);
		if (!params.has(id)) continue;
		switch (id) {
		case F::Dir: out.dir = "C:/mods/Sample"; break;
		case F::Title: out.title = "Sample"; break;
		case F::Game: out.game = "dfx"; break;
		case F::Expansion: out.expansion = "jxm"; break;
		case F::BuildsOn: out.builds_on = "jox01"; break;
		case F::BaseProject: out.base_project = "../../assets"; break;
		case F::GameInstall: out.game_install = "C:/games/JO2"; break;
		case F::Path: out.path = "menus/main.mnu"; break;
		case F::Locator: out.locator = "0/window:1"; break;
		case F::Field: out.field = "name"; break;
		case F::NewName: out.new_name = "RENAMED"; break;
		case F::Role: out.role = "main_menu"; break;
		case F::FileKind: out.file_kind = "menu"; break;
		case F::OutDir: out.out_dir = "C:/builds/sample"; break;
		case F::ExportDir: out.export_dir = "C:/shipped/sample"; break;
		case F::Mission: out.mission = "04TR.bms"; break;
		case F::Operation: out.operation = "resize"; break;
		// In its keys' order, as the wire keeps an object's members.
		// Given in the New file prompt's order, through the factory, which sorts them as the wire reads
		// them: the request equals its round trip (review F10).
		case F::Values:
			out.values = request::create_file("", "", {{"title", "My map"}, {"terrain", "island"}, {"environment", "day"}}).values;
			break;
		case F::Roles: out.roles = {"main_menu", "gametext"}; break;
		case F::Names: out.names = {"MAIN.MNU", "menu_style.mns"}; break;
		case F::Paths: out.paths = {"C:/art/main.mnu"}; break;
		case F::Imports: {
			ImportChoice install;
			install.path = "C:/games/JO";
			install.entry = "items.def";
			install.install = true;
			ImportChoice native;
			native.path = "C:/art/logo.png";
			native.native = true;
			out.imports = {{"C:/data/localres.pff", "MAIN.MNU"}, install, native};
			break;
		}
		case F::Edits: {
			// revert_to_saved's edits name a record's field alone.
			if (kind == EditorRequestKind::RevertToSaved) {
				Edit left, text;
				left.address = open.title;
				left.field = "position.left";
				text.address = open.main;
				text.field = "string.value";
				out.edits = {left, text};
				break;
			}
			const NodeKind window = open.menu ? open.menu->kind_from_name("window") : 0;
			Edit set, move, real, add, duplicate;
			set.address = open.title;
			set.field = "position.left";
			set.value = int64_t(40);
			set.gesture = 5;
			set.coalesce = true;
			move.operation = EditOperation::Move;
			move.address = open.title;
			move.parent = open.main.child;
			move.position = 2;
			real.address = open.title;
			real.field = "reach";
			real.value = 2.5;
			add.operation = EditOperation::Add;
			add.address = {open.main.row, window, 0};
			add.parent = open.main.child;
			add.field = "name";
			add.value = std::string("HELLO");
			duplicate.operation = EditOperation::Duplicate;
			duplicate.address = open.title;
			duplicate.position = 1;
			out.edits = {set, move, real, add, duplicate};
			break;
		}
		case F::Address: out.address = {7, 1, 9}; break;
		case F::Records: out.records = {{7, 1, 9}, {8, 0, 0}}; break;
		case F::PasteAt: out.paste_at = PasteAt{3, 4, 1}; break;
		case F::Mode: out.mode = SelectMode::Toggle; break;
		case F::Choice: out.choice = UnsavedChoice::Discard; break;
		case F::Settings:
			out.settings.serial = 3;
			out.settings.title = "Harbor";
			out.settings.game_install = "C:/games/JO";
			out.settings.play_mode = PlayMode::Strict;
			break;
		case F::Viewport: {
			// The text the wire reader keeps of the object (json_write's), so it reads back equal.
			opennova::io::JsonValue change;
			std::string error;
			opennova::io::json_parse(R"({"kind": "menu", "options": {"show_hidden": true}})", change, error);
			out.viewport = opennova::io::json_write(change);
			break;
		}
		case F::Drag:
			// Every member away from its default, each a value a float holds exactly.
			out.drag.id = 9;
			out.drag.handle = "bottom_right";
			out.drag.by = false;
			out.drag.x = 412.5f;
			out.drag.y = 300.0f;
			out.drag.snap = 0.25f;
			out.drag.gesture = 7;
			out.drag.end = false;
			out.drag.kind = ViewportKind::Model;
			break;
		case F::Command:
			out.command.name = "align_left";
			out.command.ids = {9, 11, 12};
			out.command.kind = ViewportKind::Menu;
			break;
		case F::Drop:
			out.drop.reference = "item";
			out.drop.name = "100300";
			out.drop.x = 320.5f;
			out.drop.y = 200.0f;
			out.drop.kind = ViewportKind::Model;
			break;
		case F::Workspace: {
			// The text the wire reader keeps of the object (json_write's), as a viewport's change is.
			opennova::io::JsonValue change;
			std::string error;
			opennova::io::json_parse(R"({"build_result": {"open": true}, "focus": "problems"})", change, error);
			out.workspace = opennova::io::json_write(change);
			break;
		}
		case F::Purpose: out.purpose = PickPurpose::GameInstall; break;
		case F::WithDependencies: out.with_dependencies = true; break;
		case F::Replace: out.replace = true; break;
		case F::Force: out.force = true; break;
		case F::AskName: out.ask_name = true; break;
		case F::OpenFirst: out.open_first = true; break;
		case F::ImportPass: out.import_pass = false; break; // its default is true
		case F::Rehash: out.rehash = true; break;
		case F::All: out.all = true; break;
		case F::Planned: out.planned = true; break;
		case F::Behind: out.behind = true; break;
		case F::Fresh: out.fresh = true; break;
		case F::Plan: out.plan = 7; break;
		case F::Report: out.report = false; break; // its default is true
		case F::Steps: out.steps = 3; break; // its default is 1
		case F::Folder: out.folder = "defs"; break;
		case F::Alone: out.alone = true; break;
		case F::Define: out.define = ReferenceSubject{ReferenceKind::TextId, "WEP_NEW", "GAMETEXT.BIN/WepDes"}; break;
		case F::Start:
			out.start.set = true;
			out.start.at[0] = 120.5;
			out.start.at[1] = -40.25;
			out.start.at[2] = 8.0;
			out.start.yaw = 90.0;
			break;
		case F::PlayMode: out.play_mode = PlayMode::Install; break;
		case F::SaveBeforePlay: out.save_before_play = false; break;
		case F::kCount: break;
		}
	}
	return out;
}

// The request table on the wire (S13 A4). Every field has a row of its own with a token and a
// doc, and some kind takes it. For every kind: the sample the table makes writes "kind" and
// exactly the fields its row takes, each by its token, and reads back equal (its edits named in
// the open menu, S13 A5); the sample with a field its row does not take added (as another kind's
// sample writes it) is refused naming the field; and with a field its row must carry left out,
// refused naming it.
static int test_request_table_samples() {
	OpenMenu open("opennova_session_json_samples");
	TEST_EXPECT(open.menu != nullptr && open.title.child != 0 && open.main.child != 0);
	if (!open.menu) return 1;
	RequestNames names;
	names.document = open.menu;
	RequestFieldSet taken = 0;
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		const RequestField &row = request_field(id);
		RequestFieldId back = RequestFieldId::kCount;
		TEST_EXPECT(row.id == id && *row.token && *row.doc && request_field_from_token(row.token, back) && back == id);
	}
	for (size_t k = 0; k < kEditorRequestKindCount; ++k)
		taken |= request_kind_row(static_cast<EditorRequestKind>(k)).params.takes;
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		if (!(taken & field_bit(id))) std::fprintf(stderr, "no request kind takes %s\n", request_field(id).token);
		TEST_EXPECT((taken & field_bit(id)) != 0);
	}
	RequestFieldId none = RequestFieldId::Dir;
	TEST_EXPECT(!request_field_from_token("kind", none) && !request_field_from_token("text", none) &&
	            !request_field_from_token("flag", none) && !request_field_from_token("edit", none));

	// Each field's value as a sample of a kind that takes it writes it.
	std::vector<JsonValue> sample_values(kRequestFieldCount);
	for (size_t k = 0; k < kEditorRequestKindCount; ++k) {
		const JsonValue json = editor_request_to_json(table_sample(static_cast<EditorRequestKind>(k), open), open.menu);
		for (const opennova::io::JsonMember &member : json.object) {
			RequestFieldId id = RequestFieldId::Dir;
			if (request_field_from_token(member.key, id)) sample_values[static_cast<size_t>(id)] = member.value;
		}
	}
	size_t refused_outside = 0, refused_missing = 0;
	for (size_t k = 0; k < kEditorRequestKindCount; ++k) {
		const auto kind = static_cast<EditorRequestKind>(k);
		const RequestKindRow &row = request_kind_row(kind);
		const EditorRequest sample = table_sample(kind, open);
		const JsonValue json = editor_request_to_json(sample, open.menu);
		// "kind" and exactly the fields the row takes.
		TEST_EXPECT(json.get_string("kind", "") == row.token);
		size_t members = 0;
		for (const opennova::io::JsonMember &member : json.object) {
			if (member.key == "kind") continue;
			RequestFieldId id = RequestFieldId::Dir;
			const bool known = request_field_from_token(member.key, id);
			TEST_EXPECT(known && row.params.has(id));
			++members;
		}
		size_t takes = 0;
		for (size_t i = 0; i < kRequestFieldCount; ++i) {
			const auto id = static_cast<RequestFieldId>(i);
			if (!row.params.has(id)) continue;
			++takes;
			if (!json.get(request_field(id).token))
				std::fprintf(stderr, "%s's sample writes no %s\n", row.token, request_field(id).token);
			TEST_EXPECT(json.get(request_field(id).token) != nullptr);
		}
		TEST_EXPECT(members == takes);
		// Read back, through the text, equal.
		JsonValue parsed;
		EditorRequest back;
		std::string error;
		const bool read = parse(opennova::io::json_write(json).c_str(), parsed) &&
		                  editor_request_from_json(parsed, back, error, &names);
		if (!read || !(back == sample))
			std::fprintf(stderr, "%s's sample does not read back: %s\n", row.token, error.c_str());
		TEST_EXPECT(read && back == sample);
		// A field the row does not take, refused naming it.
		for (size_t i = 0; i < kRequestFieldCount; ++i) {
			const auto id = static_cast<RequestFieldId>(i);
			if (row.params.has(id)) continue;
			JsonValue outside = json;
			outside.set(request_field(id).token, sample_values[i]);
			EditorRequest kept = back;
			const bool read = editor_request_from_json(outside, kept, error, &names);
			TEST_EXPECT(!read && kept == back &&
			            error.find(std::string("takes no \"") + request_field(id).token + "\"") != std::string::npos);
			++refused_outside;
		}
		// A field the row must carry, left out: refused naming it.
		for (size_t i = 0; i < kRequestFieldCount; ++i) {
			const auto id = static_cast<RequestFieldId>(i);
			if (!row.params.needs(id)) continue;
			JsonValue missing = JsonValue::make_object();
			for (const opennova::io::JsonMember &member : json.object)
				if (member.key != request_field(id).token) missing.set(member.key, member.value);
			TEST_EXPECT(!editor_request_from_json(missing, back, error, &names) &&
			            error.find(std::string("needs \"") + request_field(id).token + "\"") != std::string::npos);
			++refused_missing;
		}
	}
	TEST_EXPECT(refused_outside > kEditorRequestKindCount && refused_missing > 0);
	std::printf("request table: %zu kinds, %zu fields, %zu refused outside their kind's set, %zu refused left out\n",
	            kEditorRequestKindCount, kRequestFieldCount, refused_outside, refused_missing);
	return 0;
}

// apply_project_settings (S11d): the one request the project settings take, its settings
// each optional (one left out is not set), read strictly (a play_mode is runtime, install or strict; the
// editor-wide play_in_install and play_in_install_strict name nothing since Play's settings are each
// project's); the five requests it replaced are no tokens. play takes its own play_mode and
// save_before_play, for itself alone, read as strictly. Over a session, the dialogs section says what the last one could not write and its
// settings_applied event carries the serial back, and the run section names the runtime the
// settings name apart from the one Play resolves.
static int test_settings_json() {
	EditorRequestKind kind = EditorRequestKind::Rescan;
	TEST_EXPECT(editor_request_kind_from_token("apply_project_settings", kind) && kind == EditorRequestKind::ApplyProjectSettings);
	for (const char *gone : {"set_title", "set_feature", "set_runtime_executable", "set_game_install", "set_play_retail"})
		TEST_EXPECT(!editor_request_kind_from_token(gone, kind));
	ProjectSettingsChange every;
	every.serial = 12;
	every.title = "Harbor";
	every.mission = true;
	every.multiplayer = false;
	every.game_install = "C:/games/Joint Operations";
	every.runtime_executable = "";
	every.play_mode = PlayMode::Strict;
	every.save_before_play = false;
	const EditorRequest all = request::apply_project_settings(every);
	JsonValue parsed;
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(all)).c_str(), parsed));
	EditorRequest back;
	std::string error;
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.kind == EditorRequestKind::ApplyProjectSettings);
	const ProjectSettingsChange &read = back.settings;
	TEST_EXPECT(read.serial == 12 && read.title == std::optional<std::string>("Harbor") && read.mission == std::optional<bool>(true) &&
	            read.multiplayer == std::optional<bool>(false) &&
	            read.game_install == std::optional<std::string>("C:/games/Joint Operations") &&
	            read.runtime_executable == std::optional<std::string>("") && read.play_mode == std::optional<PlayMode>(PlayMode::Strict) &&
	            read.save_before_play == std::optional<bool>(false));
	const JsonValue *written = parsed.get("settings");
	TEST_EXPECT(written && written->get("game_install") && written->get_string("play_mode", "") == "strict" &&
	            !written->get("play_in_install") && !written->get("play_in_install_strict") &&
	            !written->get("retail_directory") && !written->get("play_retail") && !written->get("expansion") &&
	            !written->get("builds_on") && !read.expansion && !read.builds_on);
	// The project's expansion (S16): an empty name makes the project standalone, so it is set, not unset.
	ProjectSettingsChange expansion;
	expansion.expansion = "jxm";
	expansion.builds_on = "";
	EditorRequest expansion_back;
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(request::apply_project_settings(expansion))).c_str(),
	                  parsed) &&
	            editor_request_from_json(parsed, expansion_back, error) &&
	            expansion_back.settings.expansion == std::optional<std::string>("jxm") &&
	            expansion_back.settings.builds_on == std::optional<std::string>("") && !expansion_back.settings.title);
	// One setting named: the others are not set.
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"play_mode\":\"install\"}}", back).empty());
	TEST_EXPECT(back.settings.play_mode == std::optional<PlayMode>(PlayMode::Install) && back.settings.serial == 0 &&
	            !back.settings.title && !back.settings.mission && !back.settings.multiplayer && !back.settings.game_install &&
	            !back.settings.runtime_executable && !back.settings.save_before_play);
	// A play mode no mode has, or one that is no string, is refused naming the modes.
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"play_mode\":\"retail\"}}", back)
	                    .find("must be a play mode (runtime, install, strict), not \"retail\"") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"play_mode\":true}}", back)
	                    .find("settings.play_mode") != std::string::npos);
	// The settings must be named (an empty object sets nothing); the keys before S13 A4 name nothing.
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\"}", back).find("needs \"settings\"") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{}}", back).empty() && !back.settings.title &&
	            !back.settings.play_mode);
	for (const char *retired : {"retail_directory", "play_retail", "play_in_install", "play_in_install_strict"}) {
		const std::string json = std::string("{\"kind\":\"apply_project_settings\",\"settings\":{\"") + retired + "\":true}}";
		TEST_EXPECT(request_error(json.c_str(), back).find(retired) != std::string::npos);
	}
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"titel\":\"X\"}}", back).find("titel") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"mission\":\"yes\"}}", back).find("mission") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"serial\":-1}}", back).find("serial") !=
	            std::string::npos);
	// play's own: a mode and saving first for that Play alone, left out the project's (no member written).
	TEST_EXPECT(request_error("{\"kind\":\"play\",\"play_mode\":\"strict\",\"save_before_play\":false}", back).empty() &&
	            back.play_mode == std::optional<PlayMode>(PlayMode::Strict) && back.save_before_play == std::optional<bool>(false));
	TEST_EXPECT(request_error("{\"kind\":\"play\"}", back).empty() && !back.play_mode && !back.save_before_play);
	const JsonValue plain = editor_request_to_json(request::play());
	TEST_EXPECT(!plain.get("play_mode") && !plain.get("save_before_play"));
	TEST_EXPECT(request_error("{\"kind\":\"play\",\"play_mode\":\"jointops\"}", back)
	                    .find("\"play_mode\" must be a play mode (runtime, install, strict), not \"jointops\"") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"play\",\"save_before_play\":\"yes\"}", back).find("save_before_play") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"build\",\"play_mode\":\"runtime\"}", back).find("play_mode") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":[]}", back).find("settings") != std::string::npos);

	// Over a session: the last one's result, its event with its serial, and the runtime setting.
	editor_test::TempProjectDir dir("opennova_session_json_settings_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	// The newest event of the events page, when it is a settings_applied (null otherwise).
	const auto applied = [&view]() {
		const JsonValue page = events_page_to_json(view.events, 0, ViewEvents::kKept);
		const JsonValue *items = page.get("items");
		return items && !items->array.empty() && items->array.back().get_string("kind", "") == "settings_applied"
				? items->array.back()
				: JsonValue::make_null();
	};
	TEST_EXPECT(session.handle(request::new_project(dir.file("project"), "Settings")));
	session.run_operations();
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"serial\":4,\"title\":\"Harbor\","
	                          "\"runtime_executable\":\"C:/tools/opennova.exe\"}}",
	                          back)
	                    .empty());
	editor_test::handle_to_end(session, back);
	JsonValue dialogs = view_section_to_json(view, ViewSection::Dialogs);
	const JsonValue *result = dialogs.get("settings_result");
	TEST_EXPECT(result && result->get("serial") == nullptr && result->get("failures") &&
			result->get("failures")->array.empty());
	JsonValue event = applied();
	TEST_EXPECT(event.is_object() && event.get_int("tag", 0) == 4 && event.get("flag") == nullptr);
	TEST_EXPECT(view_section_to_json(view, ViewSection::Project).get_string("title", "") == "Harbor");
	TEST_EXPECT(view_section_to_json(view, ViewSection::Run).get_string("runtime_setting", "") == "C:/tools/opennova.exe" &&
	            view_section_to_json(view, ViewSection::Preferences).get_string("runtime_setting", "") ==
	                    "C:/tools/opennova.exe");
	// A name the project cannot take: the failure is the result's, its event the next serial's,
	// flagged.
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"serial\":5,\"title\":\"\"}}", back).empty());
	editor_test::handle_to_end(session, back);
	dialogs = view_section_to_json(view, ViewSection::Dialogs);
	result = dialogs.get("settings_result");
	TEST_EXPECT(result && result->get("failures") && result->get("failures")->array.size() == 1 &&
	            result->get("failures")->array[0].get_string("code", "") == "project.title_empty");
	event = applied();
	TEST_EXPECT(event.is_object() && event.get_int("tag", 0) == 5 && event.get_bool("flag", false));
	return 0;
}

// A grouped page names the groups of its own problems, each whole (its first row among the
// shown, its counts), and says how many there are in all: a project with a group per file
// (250 here, past what the editor MCP carries in one list) answers any page in a few. The page
// says where it is in the rows shown (S13 A5: count, offset, next_offset).
static int test_problem_groups_page() {
	SessionView view;
	view.project.open = true;
	for (int i = 0; i < 250; ++i) {
		const std::string file = "defs/f" + std::to_string(1000 + i) + ".def";
		view.findings.diagnostics.push_back(editor_test::finding_of(DiagnosticSeverity::Warning, "catalog.name_duplicate", "A finding.", file));
		if (i == 20) // a second finding in one file: that group holds two rows
			view.findings.diagnostics.push_back(editor_test::finding_of(DiagnosticSeverity::Warning, "catalog.name_duplicate", "Another.", file));
	}
	ProblemQuery by_file;
	by_file.grouping = ProblemGrouping::File;
	const ProblemAnswer answer = answer_problems(by_file, view);
	TEST_EXPECT(answer.groups.size() == 250 && answer.rows.size() == 251);
	ProblemFixCache fixes;
	JsonValue page = problems_to_json(view, answer, JsonPage{10, 5}, fixes);
	TEST_EXPECT(page.get_int("group_count", 0) == 250 && page.get_int("shown", 0) == 251);
	TEST_EXPECT(page.get_int("count", 0) == 251 && page.get_int("offset", -1) == 10 && page.get_int("next_offset", 0) == 15);
	TEST_EXPECT(page.get("problems")->array.size() == 5 && page.get("groups")->array.size() == 5);
	for (size_t i = 0; i < 5; ++i) {
		const JsonValue &group = page.get("groups")->array[i];
		TEST_EXPECT(group.get_int("first", -1) == int(10 + i) && group.get_int("count", 0) == 1);
		TEST_EXPECT(page.get("problems")->array[i].get_string("group", "") == group.get_string("key", "x"));
	}
	// A page that starts inside a group and ends in the next: both, whole.
	page = problems_to_json(view, answer, JsonPage{21, 2}, fixes);
	TEST_EXPECT(page.get("groups")->array.size() == 2);
	if (page.get("groups")->array.size() == 2) {
		const JsonValue &shared = page.get("groups")->array[0];
		TEST_EXPECT(shared.get_string("key", "") == "defs/f1020.def" && shared.get_int("first", -1) == 20 &&
		            shared.get_int("count", 0) == 2 && shared.get_int("warnings", 0) == 2);
		TEST_EXPECT(page.get("groups")->array[1].get_int("first", -1) == 22);
	}
	// A page past the rows: no problems, no groups, the count still all of them, no next page.
	page = problems_to_json(view, answer, JsonPage{400, 5}, fixes);
	TEST_EXPECT(page.get("problems")->array.empty() && page.get("groups")->array.empty() &&
	            page.get_int("group_count", 0) == 250 && page.get_int("count", 0) == 251 &&
	            page.get("next_offset") && page.get("next_offset")->is_null());
	// Ungrouped: no group metadata at all.
	page = problems_to_json(view, answer_problems(ProblemQuery(), view), JsonPage{0, 5}, fixes);
	TEST_EXPECT(page.get("groups") == nullptr && page.get("group_count") == nullptr);
	return 0;
}

static int test_over_a_session() {
	editor_test::TempProjectDir dir("opennova_session_json_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	const auto section = [&view](ViewSection which) { return view_section_to_json(view, which); };

	// No project: the view says so and carries no rows.
	TEST_EXPECT(!section(ViewSection::Project).get_bool("open", true));
	TEST_EXPECT(section(ViewSection::Requirements).get_int("total", -1) == 0);
	TEST_EXPECT(section(ViewSection::Run).get_string("state", "") == "stopped");
	const JsonValue no_documents = section(ViewSection::Documents);
	TEST_EXPECT(no_documents.get("open") && no_documents.get("open")->is_array() &&
	            no_documents.get("open")->array.empty() && no_documents.get_int("count", -1) == 0);
	TEST_EXPECT(section(ViewSection::Status).get_string("status", "") == "No project open.");

	// New project through JSON: the checklist is unmet, every row listed.
	const std::string root = dir.file("John Smith");
	EditorRequest request;
	TEST_EXPECT(request_error(("{\"kind\":\"new_project\",\"dir\":\"" + root + "\",\"title\":\"John Smith\"}").c_str(), request).empty());
	TEST_EXPECT(session.handle(request));
	// Opening it is an operation (S13 A3): the view holds nothing of the project until it ends, and
	// the operation section says what runs.
	const JsonValue opening = section(ViewSection::Operation);
	TEST_EXPECT(!section(ViewSection::Project).get_bool("open", true) &&
	            opening.get("operation")->get_bool("running", false) &&
	            opening.get("operation")->get_string("kind", "") == "open" &&
	            opening.get("operation")->get_string("unit", "") == "files" &&
	            opening.get("operation")->get_bool("cancellable", false) &&
	            !opening.get("validation")->get_bool("running", true));
	session.run_operations();
	const JsonValue project = section(ViewSection::Project);
	TEST_EXPECT(project.get_bool("open", false) && project.get_string("title", "") == "John Smith");
	TEST_EXPECT(project.get_string("target_game", "") == "jo" && project.get("features")->get_bool("menu", false));
	const JsonValue requirements = section(ViewSection::Requirements);
	TEST_EXPECT(requirements.get_int("total", 0) > 0 &&
	            requirements.get_int("missing", -1) == requirements.get_int("total", 0));
	TEST_EXPECT(requirements.get("rows")->array.size() == view.project.requirements->rows.size());
	const JsonValue &first_row = requirements.get("rows")->array.front();
	TEST_EXPECT(first_row.get_string("state", "") == "missing" && !first_row.get_string("role", "").empty() &&
	            !first_row.get_string("phase", "").empty());
	TEST_EXPECT(section(ViewSection::ProblemCounts).get_int("errors", 0) == requirements.get_int("total", 0));
	// No game runs: no endpoint, no exit code.
	const JsonValue run = section(ViewSection::Run);
	TEST_EXPECT(run.get_int("mcp_port", -1) == 0 && run.get("exit_code")->is_null());
	TEST_EXPECT(section(ViewSection::Preferences).get("recent_projects")->array.size() == 1);
	TEST_EXPECT(section(ViewSection::GraphCounts).get_int("missing", -1) == 0);
	TEST_EXPECT(diagnostics_to_json(view.findings.diagnostics).array.size() ==
			view.findings.diagnostics.size());
	// The first row, the manifest's first a project holds (game.cfg before it is this machine's own,
	// no row): an optional file the game does without, a note.
	const JsonValue first_finding = diagnostics_to_json(view.findings.diagnostics).array.front();
	TEST_EXPECT(first_finding.get_string("severity", "") == "info" &&
	            first_finding.get_string("code", "") == "requirement.optional_missing" &&
	            first_finding.get_string("role", "") == "fgn2_bin" && first_finding.get_string("target", "") == "fgn2.bin");

	// Problems through the wire: every finding counted, the errors shown, each required file
	// the project lacks naming its role and file (no file of the project's) with its fixes.
	ProblemQuery errors_only;
	errors_only.warnings = errors_only.infos = false;
	ProblemFixCache fix_cache;
	const JsonValue problems = problems_to_json(view, answer_problems(errors_only, view), JsonPage{}, fix_cache);
	const int required = requirements.get_int("total", 0);
	TEST_EXPECT(problems.get_int("total", 0) == int(view.findings.diagnostics.size()) && problems.get_int("shown", 0) == required);
	TEST_EXPECT(problems.get_int("count", 0) == required && problems.get_int("offset", -1) == 0 &&
	            problems.get("next_offset") && problems.get("next_offset")->is_null());
	const JsonValue *counts = problems.get("counts");
	TEST_EXPECT(counts && counts->get_int("errors", 0) == required && counts->get_int("infos", 0) > 0 &&
	            counts->get_int("errors", 0) + counts->get_int("warnings", 0) + counts->get_int("infos", 0) ==
	                    int(view.findings.diagnostics.size()));
	TEST_EXPECT(problems.get("groups") == nullptr && problems.get("problems")->array.size() == size_t(required));
	const JsonValue &missing = problems.get("problems")->array.front();
	TEST_EXPECT(missing.get_string("code", "") == "requirement.missing" && missing.get("asset") == nullptr &&
	            missing.get_string("role", "") == "gameerr" && missing.get_string("target", "") == "gameerr.bin");
	// In plain words, the manifest's own record the cited detail; the boot goes on without gameerr.bin, so
	// it blocks no build, while gametext.bin's absence does (the boot exits), said with why.
	TEST_EXPECT(missing.get_string("message", "").find("an error dialog") != std::string::npos &&
	            missing.get_string("witness", "").find("[orig:") != std::string::npos && missing.get("blocks_build") == nullptr);
	bool gametext_blocks = false;
	for (const JsonValue &row : problems.get("problems")->array)
		if (row.get_string("role", "") == "gametext")
			gametext_blocks = row.get_bool("blocks_build", false) &&
			                  row.get_string("blocks_because", "").find("Unable to load game strings") != std::string::npos;
	TEST_EXPECT(gametext_blocks && counts->get_int("blocking", 0) >= 4);
	const JsonValue *fixes = missing.get("fixes");
	TEST_EXPECT(fixes && !fixes->array.empty());
	if (!fixes || fixes->array.empty()) return 1;
	const JsonValue &create_fix = fixes->array.front();
	TEST_EXPECT(create_fix.get_string("label", "") == "Create gameerr.bin" && create_fix.get_bool("bulk", false));
	// The fix's request, as the wire carries it, does what its label says.
	std::string parse_error;
	TEST_EXPECT(create_fix.get("request") && editor_request_from_json(*create_fix.get("request"), request, parse_error));
	TEST_EXPECT(request.kind == EditorRequestKind::CreateMissing && request.roles == std::vector<std::string>{"gameerr"});
	TEST_EXPECT(session.handle(request) && session.outcome().done() && view.project.scan->find("gameerr.bin") != nullptr);
	session.run_operations(); // the validation the file made left due (S13 A3: no request runs it)
	// A page, and the rows grouped by kind: one group, its title in plain words.
	const JsonValue page = problems_to_json(view, answer_problems(errors_only, view), JsonPage{1, 2}, fix_cache);
	const JsonValue all = problems_to_json(view, answer_problems(errors_only, view), JsonPage{}, fix_cache);
	TEST_EXPECT(page.get("problems")->array.size() == 2 && all.get("problems")->array.size() > 2 &&
	            page.get("problems")->array[0].get_string("message", "") == all.get("problems")->array[1].get_string("message", ""));
	TEST_EXPECT(page.get_int("offset", -1) == 1 && page.get_int("next_offset", 0) == 3 &&
	            page.get_int("count", 0) == all.get_int("count", -1));
	ProblemQuery by_kind = errors_only;
	by_kind.grouping = ProblemGrouping::Kind;
	const JsonValue grouped = problems_to_json(view, answer_problems(by_kind, view), JsonPage{}, fix_cache);
	TEST_EXPECT(grouped.get("groups") && grouped.get("groups")->array.size() == 1 && grouped.get_int("group_count", 0) == 1);
	if (grouped.get("groups") && grouped.get("groups")->array.size() == 1) {
		const JsonValue &group = grouped.get("groups")->array.front();
		TEST_EXPECT(group.get_string("key", "") == "requirement" && group.get_string("title", "") == "Required files" &&
		            group.get_int("count", 0) == required - 1 && group.get_int("errors", 0) == required - 1 &&
		            group.get_int("first", -1) == 0);
		for (const JsonValue &row : grouped.get("problems")->array) TEST_EXPECT(row.get_string("group", "") == "requirement");
	}

	// Create all missing through the wire, the roles of the checklist's unmet rows named (none
	// named makes nothing), then open the startup menu: the document and its records serialize.
	TEST_EXPECT(request_error("{\"kind\":\"create_missing\"}", request).empty() && session.handle(request));
	TEST_EXPECT(session.outcome().done() &&
			view.project.requirements->required_missing == required - 1);
	std::string roles;
	const JsonValue unmet = section(ViewSection::Requirements);
	for (const JsonValue &row : unmet.get("rows")->array)
		if (row.get_bool("required", false) && row.get_string("state", "") != "present")
			roles += (roles.empty() ? "\"" : ",\"") + row.get_string("role", "") + "\"";
	TEST_EXPECT(request_error(("{\"kind\":\"create_missing\",\"roles\":[" + roles + "]}").c_str(), request).empty() &&
	            session.handle(request));
	TEST_EXPECT(view.project.requirements->required_missing == 0);
	// Every file the scan lists, each with its kind and whether the editor opens it: the files
	// query's pages (S13 A5), the project section their count.
	TEST_EXPECT(section(ViewSection::Project).get_int("file_count", -1) == int(view.project.scan->entries.size()) &&
	            section(ViewSection::Project).get("files") == nullptr);
	JsonValue file_args = JsonValue::make_object();
	file_args.set("limit", JsonValue::make_number(200.0));
	std::string query_error;
	const JsonValue listed = session.query("files", file_args, query_error);
	TEST_EXPECT(query_error.empty() && listed.get("files") && view.project.scan->entries.size() <= 200 &&
	            listed.get("files")->array.size() == view.project.scan->entries.size() &&
	            listed.get_int("count", -1) == int(view.project.scan->entries.size()));
	bool menu_editable = false, some_not_editable = false;
	if (listed.get("files"))
		for (const JsonValue &file : listed.get("files")->array) {
			if (file.get_string("name", "") == "main.mnu" && file.get_string("kind", "") == "menu")
				menu_editable = file.get_bool("editable", false);
			some_not_editable = some_not_editable || !file.get_bool("editable", true);
		}
	TEST_EXPECT(menu_editable && some_not_editable);
	TEST_EXPECT(request_error("{\"kind\":\"open_document\",\"path\":\"main.mnu\"}", request).empty() && session.handle(request));
	session.run_operations(); // the validation the files made and the open left due (S13 A3)
	const Document *document = session.document_for();
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	JsonValue documents = section(ViewSection::Documents);
	TEST_EXPECT(documents.get("open")->array.size() == 1 && documents.get_int("count", 0) == 1);
	TEST_EXPECT(documents.get("open")->array.front().get_string("kind", "") == "menu");
	TEST_EXPECT(!documents.get("open")->array.front().get_bool("dirty", true));
	TEST_EXPECT(documents.get("open")->array.front().get("rows") == nullptr);
	TEST_EXPECT(documents.get_string("active", "") == document->path());

	const JsonPage every;
	JsonValue doc = document_to_json(*document, &every);
	TEST_EXPECT(doc.get_string("path", "") == document->path() && doc.get_int("row_count", 0) == 1 &&
	            doc.get_int("count", 0) == 1 && doc.get_int("offset", -1) == 0);
	TEST_EXPECT(document_to_json(*document).get("rows") == nullptr && document_to_json(*document).get("count") == nullptr);
	TEST_EXPECT(doc.get("top_kinds")->array.size() == 1 && doc.get("top_kinds")->array.front().get_string("label", "") == "Add screen");
	const JsonValue *startup = find_row(doc, "STARTUP");
	TEST_EXPECT(startup != nullptr && startup->get_string("kind_label", "").size() > 0);
	// The screen holds its root windows; a root holds its lists and its children (last).
	const JsonValue *top = startup->get("collections");
	TEST_EXPECT(top && top->array.size() == 1 && top->get_string("x", "x") == "x");
	const JsonValue &root_collection = top->array.front();
	TEST_EXPECT(root_collection.get_string("label", "") == "Windows" && !root_collection.get_bool("fixed", false) &&
	            root_collection.get_string("kind_name", "") == "window" && root_collection.get("records")->array.size() == 1);
	const JsonValue &main_json = root_collection.get("records")->array.front();
	NodeAddress title_address, main_address;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "TITLE", title_address) &&
			find_definition(AssetGraph(), *document, "MAIN", main_address));
	TEST_EXPECT(main_json.get_string("name", "") == "MAIN" && uint64_t(main_json.get_number("id", 0)) == main_address.child);
	const JsonValue *lists = main_json.get("collections");
	TEST_EXPECT(lists && lists->array.size() == document->collections_of(main_address).size());
	const JsonValue &windows = lists->array.back();
	TEST_EXPECT(windows.get_string("label", "") == "Windows" && !windows.get_bool("fixed", false));
	bool title_listed = false, appearance_listed = false;
	for (const JsonValue &record : windows.get("records")->array)
		if (record.get_string("name", "") == "TITLE" && uint64_t(record.get_number("id", 0)) == title_address.child) title_listed = true;
	for (const JsonValue &collection : lists->array)
		if (collection.get_string("kind_name", "") == "appearance" && collection.get("records")->array.size() == 1 &&
		    collection.get("records")->array.front().get_string("name", "") == "Appearance 1")
			appearance_listed = true;
	// A list the root's type does not read says so.
	bool items_ignored = false, one_list_box = false;
	for (const JsonValue &collection : lists->array) {
		if (collection.get_string("kind_name", "") == "items.item") items_ignored = collection.get_string("applies", "") == "ignored";
		// A part is one at most (S9h2: the inspector's Add says so).
		if (collection.get_string("kind_name", "") == "list_box") one_list_box = collection.get_int("max", 0) == 1;
	}
	TEST_EXPECT(title_listed && appearance_listed && items_ignored && one_list_box);

	// The record: its fields through the schema, the value and a reference's status.
	JsonValue record = record_to_json(*document, title_address, view);
	TEST_EXPECT(record.get_string("name", "") == "TITLE" && uint64_t(record.get_number("child", 0)) == title_address.child);
	TEST_EXPECT(record.get_string("path", "") == "STARTUP/MAIN/TITLE" && record.get_string("locator", "") == "0/window:0/window:0");
	TEST_EXPECT(record.get("owner") && uint64_t(record.get("owner")->get_number("child", 0)) == main_address.child &&
	            record.get_int("index", -1) == 0);
	TEST_EXPECT(record.get("collections") &&
	            record.get("collections")->array.size() == document->collections_of(title_address).size());
	const JsonValue *text = find_field(record, "string.value");
	TEST_EXPECT(text && text->get_string("type", "") == "text" && text->get_string("value", "") == "John Smith");
	TEST_EXPECT(text->get("optional") == nullptr && text->get("reference") == nullptr);
	const JsonValue *left = find_field(record, "position.left");
	TEST_EXPECT(left && left->get_bool("optional", false) && left->get_bool("present", false));
	// What the editor shows (S9h2): the readable name, the group's heading, a choice's name.
	TEST_EXPECT(left->get_string("label", "") == "Left" && left->get_string("section", "") == "Position");
	const JsonValue *justify = find_field(record, "string.justify");
	bool centre = false;
	for (const JsonValue &choice : justify->get("choices")->array)
		centre = centre || (choice.get_string("name", "") == "CENTER" && choice.get_string("label", "") == "Centre");
	TEST_EXPECT(justify && centre);
	TEST_EXPECT(record_to_json(*document, NodeAddress{title_address.row, 0, title_address.child}, view).is_null());
	const JsonValue main_record = record_to_json(*document, main_address, view);
	// The root is a generic window: it reads no STRING.
	TEST_EXPECT(find_field(main_record, "string.value") &&
	            find_field(main_record, "string.value")->get_string("applies", "") == "ignored");
	const JsonValue *font = find_field(main_record, "font.name");
	TEST_EXPECT(font && font->get_string("reference", "") == "font" && font->get_string("reference_status", "") == "present");
	TEST_EXPECT(!font->get_string("reference_file", "").empty());
	const JsonValue graph_counts = section(ViewSection::GraphCounts);
	TEST_EXPECT(graph_counts.get_int("edges", 0) > 0 && graph_counts.get_int("symbols", 0) > 0);
	// What the graph holds, totals moving with its generation (the Graph concern); what its last
	// update did (S13 D3's GraphStats) moves with every update and is read in C++, not here.
	const AssetGraph &graph = *view.findings.graph;
	TEST_EXPECT(graph_counts.get_int("files", -1) == int64_t(graph.index().slot_count()) &&
			graph_counts.get_int("files", 0) > 0 &&
			graph_counts.get_int("edges", 0) == int64_t(graph.edge_count()) &&
			graph_counts.get_int("symbols", 0) == int64_t(graph.symbol_count()) &&
			graph_counts.get_int("missing", -1) == int64_t(graph.missing_count()));
	for (const char *stat : { "files_extracted", "files_reused", "files_failed", "files_patched",
				 "edges_resolved", "findings_made" })
		TEST_EXPECT(graph_counts.get(stat) == nullptr);
	const GraphStats &stats = graph.stats();
	TEST_EXPECT(stats.files_extracted + stats.files_reused + stats.files_failed > 0);
	TEST_EXPECT(!graph_edges_to_json(*view.findings.graph, view.findings.graph->references_of("main.mnu")).array.empty());
	TEST_EXPECT(graph_edges_to_json(*view.findings.graph, view.findings.graph->references_of("main.mnu")).array.front().get_string("status", "") == "present");
	TEST_EXPECT(record_to_json(*document, NodeAddress{}, view).is_null());
	TEST_EXPECT(record_to_json(*document, NodeAddress{99999, 1, 0}, view).is_null());
	// The root's font as its picker and its Go to see it (S12 Z2): the project's fonts and the
	// stylesheet's variables, each as the font set to it resolves; the variable where the game
	// reads it, then the .fnt its value names. Each a page of its list (S13 A5).
	const JsonValue choices = reference_choices_to_json(*document, main_address, "font.name", view);
	const JsonValue *offered = choices.get("choices");
	TEST_EXPECT(choices.get_string("reference", "") == "font" && offered &&
	            choices.get_int("count", -1) == int(offered->array.size()));
	if (!offered) return 1;
	bool font_offered = false, variable_offered = false;
	for (const JsonValue &choice : offered->array) {
		font_offered = font_offered || choice.get_string("kind", "") == "font";
		variable_offered = variable_offered || (choice.get_string("kind", "") == "style_var" &&
		                                        choice.get_string("name", "").rfind('%', 0) == 0 &&
		                                        choice.get_string("status", "") == "present");
	}
	TEST_EXPECT(font_offered && variable_offered);
	// One choice at a time, pages that concatenate to the whole list.
	std::vector<std::string> paged;
	for (size_t at = 0; at < offered->array.size(); ++at) {
		const JsonValue one = reference_choices_to_json(*document, main_address, "font.name", view, JsonPage{at, 1});
		TEST_EXPECT(one.get("choices") && one.get("choices")->array.size() == 1 &&
		            one.get_int("count", -1) == int(offered->array.size()) && one.get_int("offset", -1) == int(at));
		if (one.get("choices") && one.get("choices")->array.size() == 1)
			paged.push_back(one.get("choices")->array[0].get_string("name", ""));
	}
	TEST_EXPECT(paged.size() == offered->array.size());
	for (size_t at = 0; at < paged.size() && at < offered->array.size(); ++at)
		TEST_EXPECT(paged[at] == offered->array[at].get_string("name", ""));
	const JsonValue targets = reference_targets_to_json(*document, main_address, "font.name", view);
	const JsonValue *places = targets.get("targets");
	TEST_EXPECT(places && places->array.size() == 2 && targets.get_int("count", 0) == 2 &&
	            font && targets.get_string("value", "") == font->get_string("value", ""));
	TEST_EXPECT(places && places->array.size() == 2 && places->array[0].get_bool("editable", false) &&
	            !places->array[0].get_string("locator", "").empty() && !places->array[1].get_bool("editable", true) &&
	            places->array[1].get("locator") == nullptr);
	const JsonValue second = reference_targets_to_json(*document, main_address, "font.name", view, JsonPage{1, 5});
	TEST_EXPECT(second.get("targets") && second.get("targets")->array.size() == 1 && second.get_int("count", 0) == 2 &&
	            second.get("next_offset") && second.get("next_offset")->is_null());
	TEST_EXPECT(reference_choices_to_json(*document, main_address, "name", view).get_int("count", -1) == 0);
	TEST_EXPECT(reference_targets_to_json(*document, main_address, "no_such_field", view).is_null());
	TEST_EXPECT(reference_choices_to_json(*document, NodeAddress{99999, 1, 0}, "font.name", view).is_null());

	// A request that names a record and one of its fields (a Problems row's): a RevealRecord
	// event, in the events page from the seq a client read to. Each such ask posts one, the same
	// ask again too (a second click on the row); one naming no field posts none; the view carries
	// no reveal state of its own.
	const uint64_t before = view.events.next_seq() - 1;
	const auto reveals = [&view, before]() {
		std::vector<JsonValue> out;
		const JsonValue page = events_page_to_json(view.events, before + 1, ViewEvents::kKept);
		for (const JsonValue &item : page.get("items")->array)
			if (item.get_string("kind", "") == "reveal_record") out.push_back(item);
		return out;
	};
	const std::string reveal = "{\"kind\":\"open_document\",\"path\":\"main.mnu\",\"address\":{\"row\":" +
	                           std::to_string(title_address.row) + ",\"kind\":" + std::to_string(title_address.kind) +
	                           ",\"child\":" + std::to_string(title_address.child) + "},\"field\":\"string.value\"}";
	TEST_EXPECT(request_error(reveal.c_str(), request).empty() && session.handle(request));
	std::vector<JsonValue> asked = reveals();
	const JsonValue *asked_at = asked.size() == 1 ? asked[0].get("address") : nullptr;
	TEST_EXPECT(view.documents.selection.primary == title_address && asked.size() == 1 &&
			asked[0].get_string("path", "") == document->path() &&
			asked[0].get_string("field", "") == "string.value" && asked[0].get("flag") == nullptr &&
			asked[0].get("tag") == nullptr);
	TEST_EXPECT(asked_at && asked_at->get_int("row", 0) == int64_t(title_address.row) &&
	            asked_at->get_int("kind", 0) == int64_t(title_address.kind) &&
	            asked_at->get_int("child", 0) == int64_t(title_address.child));
	const JsonValue state = session.query("state", JsonValue::make_null(), query_error);
	TEST_EXPECT(query_error.empty() && state.get("reveal_field") == nullptr && state.get("reveal_serial") == nullptr &&
	            state.get("selection") && state.get("selection")->get("reveal_field") == nullptr &&
	            state.get("selection")->get("reveal_serial") == nullptr);
	TEST_EXPECT(request_error(reveal.c_str(), request).empty() && session.handle(request));
	// The same field asked again: a second event, so a client sees the second ask.
	asked = reveals();
	TEST_EXPECT(asked.size() == 2 && asked[1].get_int("seq", 0) == asked[0].get_int("seq", 0) + 1 &&
	            asked[1].get_string("field", "") == "string.value");
	const std::string named = "{\"kind\":\"open_document\",\"path\":\"main.mnu\",\"address\":{\"row\":" +
	                          std::to_string(title_address.row) + ",\"kind\":" + std::to_string(title_address.kind) +
	                          ",\"child\":" + std::to_string(title_address.child) + "}}";
	TEST_EXPECT(request_error(named.c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(reveals().size() == 2);
	TEST_EXPECT(request_error(reveal.c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(reveals().size() == 3);
	// Files asked to show a file and ask its new name (a Problems row, a Rename... fix): a
	// RevealFile event naming the file, its flag the rename, and the request writes back as read.
	TEST_EXPECT(request_error("{\"kind\":\"show_in_files\",\"path\":\"main.mnu\",\"ask_name\":true}", request).empty() &&
	            session.handle(request));
	const JsonValue shown = events_page_to_json(view.events, 0, ViewEvents::kKept);
	const JsonValue *items = shown.get("items");
	const JsonValue *file_event = items && !items->array.empty() ? &items->array.back() : nullptr;
	TEST_EXPECT(file_event && file_event->get_string("kind", "") == "reveal_file" &&
			file_event->get_string("path", "") == document->path() &&
			file_event->get_bool("flag", false) && file_event->get("address") == nullptr);
	TEST_EXPECT(session.query("state", JsonValue::make_null(), query_error).get("reveal_file") == nullptr);
	TEST_EXPECT(editor_request_to_json(request).get_string("kind", "") == "show_in_files" &&
	            editor_request_to_json(request).get_bool("ask_name", false));

	// The same edit through JSON as through the typed request: the batch form, the record by its
	// identity in the active document (S13 A5).
	const std::string title_id = std::to_string(title_address.child);
	const std::string edit = "{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"set\",\"id\":" + title_id +
	                         ",\"field\":\"string.value\",\"value\":\"John Smith's Game\"}]}";
	TEST_EXPECT(request_error_in(edit, document, request).empty() && session.handle(request));
	Value value;
	TEST_EXPECT(document->get(title_address, "string.value", value) && std::get<std::string>(value) == "John Smith's Game");
	TEST_EXPECT(document->dirty());
	documents = section(ViewSection::Documents);
	TEST_EXPECT(documents.get("open")->array.front().get_bool("dirty", false));
	TEST_EXPECT(documents.get("open")->array.front().get_bool("can_undo", false));
	// A Set makes no record: the outcome's added is empty.
	JsonValue outcome = action_outcome_to_json(session.outcome());
	TEST_EXPECT(outcome.get("added") && outcome.get("added")->is_array() && outcome.get("added")->array.empty());
	TEST_EXPECT(request_error("{\"kind\":\"undo\"}", request).empty() && session.handle(request));
	TEST_EXPECT(document->get(title_address, "string.value", value) && std::get<std::string>(value) == "John Smith");

	// The selection through JSON: TITLE (the document named by its logical name), then
	// EXIT added to it (named by its path: the same document); the view lists both. A
	// clear through JSON leaves TITLE's left edge out, and the record says so.
	NodeAddress exit_address;
	TEST_EXPECT(find_definition(AssetGraph(), *document, "EXIT", exit_address));
	auto select_json = [&](const NodeAddress &address, const char *mode) {
		const std::string path = std::string(mode) == "replace" ? std::string("main.mnu") : document->path();
		return "{\"kind\":\"select_record\",\"path\":\"" + path + "\",\"mode\":\"" + mode +
		       "\",\"address\":{\"row\":" + std::to_string(address.row) + ",\"kind\":" + std::to_string(address.kind) +
		       ",\"child\":" + std::to_string(address.child) + "}}";
	};
	TEST_EXPECT(request_error(select_json(title_address, "replace").c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(request_error(select_json(exit_address, "add").c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(section(ViewSection::Documents).get_string("active", "") == document->path());
	const JsonValue selection = section(ViewSection::Selection);
	TEST_EXPECT(selection.get_string("document", "") == document->path() && selection.get("reveal_field") == nullptr);
	TEST_EXPECT(selection.get("records") && selection.get("records")->array.size() == 2 &&
	            uint64_t(selection.get("primary")->get_number("child", 0)) == exit_address.child);
	TEST_EXPECT(selection.get_int("clipboard_bytes", -1) == 0);
	// A marquee through JSON (S13 D7): the records named with it, the primary the one named, the
	// screen row with them (any rows of the document); the Selection concern moves.
	const auto address_json = [](const NodeAddress &address) {
		return "{\"row\":" + std::to_string(address.row) + ",\"kind\":" + std::to_string(address.kind) +
		       ",\"child\":" + std::to_string(address.child) + "}";
	};
	const NodeAddress screen_row{title_address.row, 0, 0};
	const std::string marquee_json = "{\"kind\":\"select_record\",\"address\":" + address_json(exit_address) +
	                                 ",\"records\":[" + address_json(title_address) + "," + address_json(screen_row) + "]}";
	const uint64_t selection_stamp = view.revisions.of(ViewConcern::Selection);
	TEST_EXPECT(request_error(marquee_json.c_str(), request).empty() && session.handle(request));
	const JsonValue marqueed = section(ViewSection::Selection);
	TEST_EXPECT(marqueed.get("records") && marqueed.get("records")->array.size() == 3 &&
	            uint64_t(marqueed.get("primary")->get_number("child", 0)) == exit_address.child &&
	            view.revisions.of(ViewConcern::Selection) != selection_stamp);
	const std::string clear = "{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"clear\",\"id\":" + title_id +
	                          ",\"field\":\"position.left\"}]}";
	TEST_EXPECT(request_error_in(clear, document, request).empty() && session.handle(request) && session.last_edit_ok());
	record = record_to_json(*document, title_address, view);
	left = find_field(record, "position.left");
	TEST_EXPECT(left && left->get_bool("optional", false) && !left->get_bool("present", true));
	// Changed since the saved baseline, as the Inspector marks it: the record and the field
	// say so with what the saved file holds, a field left alone says nothing, and the
	// document's rows carry it on TITLE.
	TEST_EXPECT(record.get_string("change", "") == "changed" && left->get_bool("changed", false));
	TEST_EXPECT(left->get("saved") && left->get("saved")->is_number() && left->get("saved")->number == 0.0 &&
	            left->get_bool("saved_present", false));
	TEST_EXPECT(find_field(record, "string.value")->get("changed") == nullptr);
	doc = document_to_json(*document, &every);
	TEST_EXPECT(!doc.get_bool("file_state_changed", true));
	{
		const JsonValue &main_row = find_row(doc, "STARTUP")->get("collections")->array.front().get("records")->array.front();
		TEST_EXPECT(main_row.get_string("change", "") == "unchanged");
		bool title_changed = false;
		for (const JsonValue &window : main_row.get("collections")->array.back().get("records")->array)
			if (window.get_string("name", "") == "TITLE") title_changed = window.get_string("change", "") == "changed";
		TEST_EXPECT(title_changed);
	}
	// Revert to saved through JSON (the Inspector's): the field as the file holds it, one
	// step; again, nothing is left to revert and the request is refused. Its edits name a
	// record's field alone ({id, field}).
	const std::string revert =
			"{\"kind\":\"revert_to_saved\",\"edits\":[{\"id\":" + title_id + ",\"field\":\"position.left\"}]}";
	TEST_EXPECT(request_error_in(revert, document, request).empty() && session.handle(request) && session.last_edit_ok());
	const JsonValue revert_json = editor_request_to_json(request);
	TEST_EXPECT(revert_json.get_string("kind", "") == "revert_to_saved" && revert_json.get("edits") &&
	            revert_json.get("edits")->array.size() == 1 &&
	            revert_json.get("edits")->array[0].get_number("id", 0.0) == double(title_address.child) &&
	            revert_json.get("edits")->array[0].get_string("field", "") == "position.left" &&
	            !revert_json.get("edits")->array[0].get("op"));
	record = record_to_json(*document, title_address, view);
	left = find_field(record, "position.left");
	TEST_EXPECT(record.get_string("change", "") == "unchanged" && left->get_bool("present", false) &&
	            left->get("changed") == nullptr);
	TEST_EXPECT(request_error_in(revert, document, request).empty() && session.handle(request));
	TEST_EXPECT(!session.outcome().done() && !session.last_edit_ok());
	TEST_EXPECT(request_error("{\"kind\":\"undo\"}", request).empty() && session.handle(request) && document->dirty());
	TEST_EXPECT(request_error("{\"kind\":\"undo\"}", request).empty() && session.handle(request) && !document->dirty());

	// A request's outcome: done with no findings, refused with them, or waiting on the
	// unsaved-changes prompt.
	outcome = action_outcome_to_json(session.outcome());
	TEST_EXPECT(outcome.get_bool("done", false) && !outcome.get_bool("unsaved_prompt", true));
	TEST_EXPECT(outcome.get("findings")->is_array() && outcome.get("findings")->array.empty());
	// A rename's plan is its operation's (S13 A3): what the request came to with what its
	// operation came to folded in, as a caller that runs it to its end reads it (handle_to_end).
	TEST_EXPECT(request_error("{\"kind\":\"rename_asset\",\"path\":\"main.mnu\",\"new_name\":\"../x.mnu\"}", request).empty());
	outcome = action_outcome_to_json(editor_test::handle_to_end(session, request));
	TEST_EXPECT(!outcome.get_bool("done", true) && !outcome.get_bool("unsaved_prompt", true));
	TEST_EXPECT(outcome.get("findings")->array.size() == 1);
	if (outcome.get("findings")->array.size() == 1) {
		const JsonValue &finding = outcome.get("findings")->array.front();
		TEST_EXPECT(finding.get_string("code", "") == "rename.name" && finding.get_string("severity", "") == "error");
	}
	TEST_EXPECT(request_error_in(edit, document, request).empty() && session.handle(request) && document->dirty());
	JsonValue dialogs = section(ViewSection::Dialogs);
	TEST_EXPECT(!dialogs.get("unsaved_prompt")->get_bool("open", true) && dialogs.get("unsaved_prompt")->get("files") == nullptr);
	TEST_EXPECT(request_error("{\"kind\":\"close_document\"}", request).empty() && session.handle(request));
	outcome = action_outcome_to_json(session.outcome());
	TEST_EXPECT(!outcome.get_bool("done", true) && outcome.get_bool("unsaved_prompt", false));
	TEST_EXPECT(outcome.get("findings")->array.empty());
	// The dialogs section names what waits, the file it lists and whether Discard is offered.
	dialogs = section(ViewSection::Dialogs);
	const JsonValue *prompt = dialogs.get("unsaved_prompt");
	TEST_EXPECT(prompt && prompt->get_bool("open", false) && prompt->get_string("action", "") == "close_document" &&
	            prompt->get_string("target", "") == document->path() && prompt->get_bool("can_discard", false));
	TEST_EXPECT(prompt && prompt->get("files") && prompt->get("files")->array.size() == 1 &&
	            prompt->get("files")->array.front().string == document->path());
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"choice\":\"cancel\"}", request).empty() &&
	            session.handle(request));
	TEST_EXPECT(action_outcome_to_json(session.outcome()).get_bool("done", false) && session.document_for() == document);
	// Build packs the files on disk: no Discard; the prompt's "save" writes the file and builds.
	TEST_EXPECT(request_error("{\"kind\":\"build\"}", request).empty() && session.handle(request));
	dialogs = section(ViewSection::Dialogs);
	prompt = dialogs.get("unsaved_prompt");
	TEST_EXPECT(prompt && prompt->get_string("action", "") == "build" && !prompt->get_bool("can_discard", true) &&
	            prompt->get("target") == nullptr);
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"choice\":\"save\"}", request).empty() &&
	            session.handle(request));
	TEST_EXPECT(action_outcome_to_json(session.outcome()).get_bool("done", false) && !document->dirty() &&
	            session.view().activity.operation.running());
	// The build runs as an operation: the answer names it, and the operation section shows it
	// stepping (its kind, its progress in bytes, what it reads and writes) until it lands.
	const double operation = action_outcome_to_json(session.outcome()).get_number("operation", 0.0);
	TEST_EXPECT(operation > 0.0 && operation == double(view.activity.operation.id));
	JsonValue activity = section(ViewSection::Operation);
	TEST_EXPECT(opennova::io::json_write(activity) == opennova::io::json_write(activity_operation_to_json(view)));
	const JsonValue *running = activity.get("operation");
	TEST_EXPECT(running && running->get_bool("running", false) && running->get_number("id", 0.0) == operation &&
	            running->get_string("kind", "") == "build" && running->get_string("unit", "") == "bytes" &&
	            running->get_bool("cancellable", false) && running->get_number("total", 0.0) > 0.0 &&
	            running->get("reads") && running->get("reads")->array.size() == 1 &&
	            running->get("reads")->array.front().string == "files" && running->get("writes") &&
	            running->get("writes")->array.size() == 1 && running->get("writes")->array.front().string == "slot");
	TEST_EXPECT(activity.get("build") && activity.get("build")->get("running") == nullptr);
	session.run_operations();
	activity = section(ViewSection::Operation);
	TEST_EXPECT(!activity.get("operation")->get_bool("running", true) && activity.get("operation")->get("id") == nullptr);
	const JsonValue *last = activity.get("last_operation");
	TEST_EXPECT(last && last->get_number("id", 0.0) == operation && last->get_string("kind", "") == "build" &&
	            last->get_string("end", "") == "done" && last->get("findings") && last->get("findings")->array.empty());
	TEST_EXPECT(activity.get("build")->get_bool("has_build", false) && activity.get("build")->get_bool("ok", false));
	TEST_EXPECT(!section(ViewSection::Dialogs).get("unsaved_prompt")->get_bool("open", true));
	TEST_EXPECT(request_error("{\"kind\":\"undo\"}", request).empty() && session.handle(request) && document->dirty());

	// Output paging by absolute index: the lines held from `first` to `next`, a window yields that
	// window, a cursor past the lines none (it waits at `next` for lines to come).
	const JsonValue output = section(ViewSection::Output);
	const uint64_t first = uint64_t(output.get_number("first", -1.0));
	const uint64_t next = uint64_t(output.get_number("next", 0.0));
	TEST_EXPECT(first == view.activity.output.first_index() && next == view.activity.output.next_index() &&
	            next - first == view.activity.output.size() && view.activity.output.size() > 1 &&
	            output.get_int("count", -1) == int(view.activity.output.size()) && output.get("lines") == nullptr);
	JsonValue lines = output_page_to_json(view.activity.output, first + 1, 1);
	TEST_EXPECT(lines.get("lines")->array.size() == 1 && uint64_t(lines.get_number("next_cursor", 0.0)) == first + 2);
	TEST_EXPECT(lines.get("lines")->array.front().string == view.activity.output[1]);
	lines = output_page_to_json(view.activity.output, next + 5, 1);
	TEST_EXPECT(lines.get("lines")->array.empty() && uint64_t(lines.get_number("cursor", 0.0)) == next);

	// The written text is strict JSON that reads back: every section of the state.
	JsonValue reread;
	const JsonValue whole = session.query("state", JsonValue::make_null(), query_error);
	TEST_EXPECT(query_error.empty() && parse(opennova::io::json_write(whole).c_str(), reread) && reread.is_object());
	for (size_t i = 0; i < kViewSectionCount; ++i)
		TEST_EXPECT(reread.get(view_section_row(static_cast<ViewSection>(i)).token) != nullptr);
	return 0;
}

// A style variable's edge says what its value must name there, and a symbol the game
// does not read says so (S9i); both stay out of the JSON otherwise.
static int test_graph_style_fields() {
	AssetGraph graph;
	GraphEdge edge;
	edge.source = "menus/main.mnu";
	edge.field = "font.name";
	edge.kind = ReferenceKind::StyleVar;
	edge.value = "%DEF_FONTNAME%";
	edge.through = ReferenceKind::Font;
	TEST_EXPECT(graph_edge_to_json(graph, edge).get_string("through", "") == "font");
	edge.through = ReferenceKind::None;
	TEST_EXPECT(graph_edge_to_json(graph, edge).get("through") == nullptr);
	GraphSymbol symbol;
	symbol.kind = ReferenceKind::StyleVar;
	symbol.display = "STRAY";
	symbol.file = "other.mns";
	symbol.inert = true;
	TEST_EXPECT(graph_symbol_to_json(symbol).get_bool("inert", false));
	symbol.inert = false;
	TEST_EXPECT(graph_symbol_to_json(symbol).get("inert") == nullptr);
	symbol.inert = true;
	symbol.inert_reason = "the game reads no stylesheet but menu_style.mns and brand.mns";
	TEST_EXPECT(graph_symbol_to_json(symbol).get_string("inert_reason", "") == symbol.inert_reason);
	return 0;
}

// Find (S12 D8): a document's hits and the project's, as the editor MCP hands them out.
static int test_search_json() {
	DocumentHit hit;
	hit.address = {7, 2, 9};
	hit.locator = "0/window:1";
	hit.record = "STARTUP/MAIN";
	hit.field = "name";
	hit.label = "Name";
	hit.text = "MAIN";
	hit.at = 1;
	const JsonValue hits = document_hits_to_json({hit});
	TEST_EXPECT(hits.get_number("count", 0.0) == 1.0);
	const JsonValue *first = hits.get("hits") && hits.get("hits")->array.size() == 1 ? &hits.get("hits")->array[0] : nullptr;
	TEST_EXPECT(first && first->get_number("id", 0.0) == 9.0 && first->get_string("locator", "") == "0/window:1" &&
	            first->get_string("record", "") == "STARTUP/MAIN" && first->get_string("field", "") == "name" &&
	            first->get_string("label", "") == "Name" && first->get_string("text", "") == "MAIN" &&
	            first->get_number("at", 0.0) == 1.0 && first->get("address"));
	GraphSymbol symbol;
	symbol.kind = ReferenceKind::Weapon;
	symbol.display = "Searchgun";
	symbol.file = "weapon.def";
	GraphSearchHit file_hit;
	file_hit.name = "weapon.def";
	file_hit.file = "weapon.def";
	file_hit.usages = 3;
	GraphSearchHit symbol_hit;
	symbol_hit.symbol = &symbol;
	symbol_hit.name = symbol.display;
	symbol_hit.file = symbol.file;
	symbol_hit.usages = 1;
	const JsonValue found = graph_search_to_json({file_hit, symbol_hit});
	const JsonValue *list = found.get("hits");
	TEST_EXPECT(found.get_number("count", 0.0) == 2.0 && list && list->array.size() == 2);
	TEST_EXPECT(list->array[0].get_string("kind", "") == "file" && list->array[0].get_string("name", "") == "weapon.def" &&
	            list->array[0].get_number("usages", 0.0) == 3.0);
	TEST_EXPECT(list->array[1].get_string("kind", "") == "weapon" && list->array[1].get_string("name", "") == "Searchgun" &&
	            list->array[1].get_string("file", "") == "weapon.def" && list->array[1].get_number("usages", 0.0) == 1.0);
	return 0;
}

namespace {

// A document of one record whose fields carry what a schema can say of a field (S12 D4): the
// record's JSON hands all of it to an MCP client.
struct MetadataRow : Node {
	std::shared_ptr<Node> clone() const override { return std::make_shared<MetadataRow>(*this); }
	std::string name() const override { return "light"; }
	size_t footprint() const override { return sizeof(MetadataRow); }
};

class MetadataDocument : public Document {
public:
	const std::vector<RecordKindRow> &kinds() const override {
		static const std::vector<RecordKindRow> table = {{0, "light", "Light", "", true}};
		return table;
	}
	std::vector<Collection> collections(const Node &, const NodeAddress &) const override { return {}; }
	const std::vector<FieldSchema> &fields(NodeKind) const override {
		static const std::vector<FieldSchema> fields = [] {
			FieldSchema reach;
			reach.id = "atten_end";
			reach.type = FieldType::Real;
			reach.unit = "m";
			reach.description = "Where the light ends.";
			reach.token = "reach";
			FieldSchema red;
			red.id = "start.r";
			red.ranged = true;
			red.max = 255.0;
			red.step = 1.0;
			red.color = FieldColor::Channel;
			red.group = "Start colour";
			FieldSchema filter;
			filter.id = "charfilter[0]";
			filter.type = FieldType::Text;
			filter.width = 32;
			filter.choices = {{"medic", 1, "Medic"}};
			filter.open_choices = true;
			FieldSchema tint;
			tint.id = "tint";
			tint.type = FieldType::Text;
			tint.width = 32;
			tint.color = FieldColor::HexArgb;
			// Open, with nothing known on this record (a part index on a model whose LOD 0 has no
			// parts): its own choices, none of them (record_choices).
			FieldSchema reg;
			reg.id = "param";
			reg.open_choices = true;
			// The same field where the record names no part: a number, no list to open.
			FieldSchema phase = reg;
			phase.id = "phase";
			return std::vector<FieldSchema>{reach, red, filter, tint, reg, phase};
		}();
		return fields;
	}
	bool record_choices(const NodeAddress &, const FieldUse &use, std::vector<FieldChoice> &) const override {
		return use.schema->id == "param";
	}
	SerializeResult serialize() const override { return {}; }
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<MetadataDocument>(*this); }

protected:
	void refine_field(const NodeAddress &, FieldUse &use) const override {
		if (use.schema->id == "param") use.own_choices = true;
	}
	bool parse(const std::vector<uint8_t> &, std::vector<std::shared_ptr<Node>> &rows, std::shared_ptr<const FileState> &,
	           std::vector<SourceIssue> &, Diagnostic &) override {
		rows.push_back(std::make_shared<MetadataRow>());
		return true;
	}
	bool read(const Node &, const NodeAddress &, const std::string &field, Value &out) const override {
		out = field == "atten_end" ? Value(12.5)
		      : field == "start.r" || field == "param" || field == "phase" ? Value(int64_t(200))
		      : field == "tint"    ? Value(std::string("FF00FF00"))
		                           : Value(std::string("pilot"));
		return true;
	}
	std::shared_ptr<Node> make_node(NodeKind, NodeId, const std::vector<std::shared_ptr<const Node>> &,
	                                std::string &error) override {
		error = "No records are added here.";
		return nullptr;
	}
	bool set_field(Node &, const NodeAddress &, const std::string &, const Value &, std::string &error) override {
		error = "Read only.";
		return false;
	}
	bool edit_collection(Node &, const Edit &, const IdAllocator &, NodeId &, std::string &error) override {
		error = "No collections.";
		return false;
	}
};

} // namespace

// What a schema says of a field (S12 D4) survives the record's JSON, written and read back: the
// unit, the table's note, the key the file writes, a ranged number's range and step, a colour's
// form, the group whose row it shares, an open list's choices (open with none known too: a
// record's own list of none; S13 D2: open only where the field offers a list there);
// nothing a field lacks is written.
static int test_field_metadata() {
	MetadataDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes({}, "lights.fake", AssetKind::Unknown, "jo", error));
	const NodeAddress light{document.rows().front()->id, 0, 0};
	SessionView view;
	JsonValue record;
	TEST_EXPECT(parse(opennova::io::json_write(record_to_json(document, light, view)).c_str(), record));
	const JsonValue *reach = find_field(record, "atten_end");
	TEST_EXPECT(reach && reach->get_string("unit", "") == "m" && reach->get_string("description", "") == "Where the light ends." &&
	            reach->get_string("token", "") == "reach" && reach->get_number("value", 0.0) == 12.5);
	TEST_EXPECT(!reach->get("min") && !reach->get("color") && !reach->get("group") && !reach->get("open_choices"));
	const JsonValue *red = find_field(record, "start.r");
	TEST_EXPECT(red && red->get_number("min", -1.0) == 0.0 && red->get_number("max", 0.0) == 255.0 &&
	            red->get_number("step", 0.0) == 1.0 && red->get_string("color", "") == "channel" &&
	            red->get_string("group", "") == "Start colour" && !red->get("unit"));
	const JsonValue *filter = find_field(record, "charfilter[0]");
	TEST_EXPECT(filter && filter->get_bool("open_choices", false) && filter->get("choices")->array.size() == 1 &&
	            filter->get("choices")->array.front().get_string("label", "") == "Medic" &&
	            filter->get_string("value", "") == "pilot");
	const JsonValue *tint = find_field(record, "tint");
	TEST_EXPECT(tint && tint->get_string("color", "") == "hex_argb" && tint->get_string("value", "") == "FF00FF00");
	const JsonValue *param = find_field(record, "param");
	TEST_EXPECT(param && param->get_bool("open_choices", false) && !param->get("choices"));
	const JsonValue *phase = find_field(record, "phase");
	TEST_EXPECT(phase && !phase->get("open_choices") && !phase->get("choices"));
	return 0;
}

// The import dialog's requests and state (S11g): plan_import and set_import_dependencies
// read back as written; the import_preview page carries the preview (S13 A5): what it lists to
// choose from and the files chosen, as a request's imports take them; the plan's importable
// rows (the chosen file, a dependency with what wanted it, where it was found, where else, and a
// row the project cannot take) apart from the rows not found; the kinds not followed, the cap
// and the plan's findings; the import section its counts and the editor's setting. A row's
// source passes back to import_files as it is.
static int test_import_plan_json() {
	TEST_EXPECT(std::string(editor_request_kind_token(EditorRequestKind::PlanImport)) == "plan_import");
	TEST_EXPECT(std::string(editor_request_kind_token(EditorRequestKind::SetImportDependencies)) == "set_import_dependencies");
	JsonValue parsed;
	EditorRequest back;
	std::string error;
	const EditorRequest plan = request::plan_import({{"C:/mod/menus.pff", "a.mnu"}}, true);
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(plan)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.kind == EditorRequestKind::PlanImport &&
	            back.with_dependencies && back.imports == plan.imports);
	TEST_EXPECT(request_error("{\"kind\":\"set_import_dependencies\"}", back).find("with_dependencies") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"set_import_dependencies\",\"with_dependencies\":false}", back).empty() &&
	            back.kind == EditorRequestKind::SetImportDependencies && !back.with_dependencies);
	TEST_EXPECT(request_error("{\"kind\":\"set_import_dependencies\",\"with_dependencies\":true}", back).empty() &&
	            back.with_dependencies);

	SessionView view;
	view.project.import_dependencies = false;
	DialogsView::ImportPreview &preview = view.dialogs.import_preview;
	preview.open = true;
	preview.with_dependencies = true;
	preview.changed = true;
	preview.choices = {{"C:/mod/menus.pff", "a.mnu"}, {"C:/mod/menus.pff", "b.mnu"}};
	preview.roots = {preview.choices[0]};
	ImportPlanRow chosen;
	chosen.state = ImportPlanRow::State::Selected;
	chosen.selected = true;
	chosen.source = preview.roots[0];
	chosen.name = "a.mnu";
	chosen.kind = AssetKind::Menu;
	chosen.destination = "menus/a.mnu";
	chosen.found_in = "the archive C:/mod/menus.pff";
	ImportPlanRow font = chosen;
	font.state = ImportPlanRow::State::Found;
	font.source = {"C:/art/arial99.fnt", "", false, true};
	font.name = "arial99.fnt";
	font.kind = AssetKind::Font;
	font.destination = "fonts/arial99.fnt";
	font.found_in = "the folder C:/art";
	font.needed_by = {"a.mnu", "A/GO", "font.name", ReferenceKind::Font, "arial99", -1};
	ImportRival rival;
	rival.name = "ARIAL99.FNT";
	rival.found_in = "the game install";
	rival.source = {"C:/jo", "ARIAL99.FNT", true, false};
	rival.differs = true;
	font.rivals = {rival};
	ImportPlanRow cut = font;
	cut.name = "a_long_texture_name.tga";
	cut.kind = AssetKind::Texture;
	cut.selected = false;
	cut.problem = "The name is longer than the 16 characters the game's archives store.";
	cut.rivals.clear();
	ImportPlanRow gone;
	gone.state = ImportPlanRow::State::NotFound;
	gone.name = "gone.tga";
	gone.kind = AssetKind::Texture;
	gone.needed_by = {"a.mnu", "A/KEEP/Appearance 1", "value", ReferenceKind::MenuTexture, "gone.tga", -1};
	ImportPlan planned;
	planned.rows = {chosen, font, gone, cut};
	planned.not_followed = {{ReferenceKind::MenuScreen, AssetKind::Unknown, 2, "a.mnu"},
	                        {ReferenceKind::None, AssetKind::Terrain, 1, "level.trn"}};
	planned.undefined = {{ReferenceKind::TextId, AssetKind::Unknown, 3, "a.mnu"}};
	planned.truncated = true;
	planned.diagnostics = { editor_test::finding_of(DiagnosticSeverity::Warning, "import.unreadable",
			"The file could not be read.", "b.mnu") };
	preview.plan = std::make_shared<const ImportPlan>(std::move(planned));
	const JsonValue json = import_preview_to_json(view, JsonPage{});
	const JsonValue *import = &json;
	TEST_EXPECT(import->get_bool("open", false) && import->get_bool("with_dependencies", false) && import->get_bool("changed", false));
	TEST_EXPECT(import->get("serial") == nullptr && import->get_int("count", 0) == 3 && import->get_int("offset", -1) == 0);
	// S14: the plan by kind, the largest first (alike in bytes and files: by token), and a page of
	// one kind's rows alone, count theirs.
	{
		const JsonValue *summary = import->get("summary");
		TEST_EXPECT(summary && summary->array.size() == 3 && summary->array[0].get_string("kind", "") == "font" &&
		            summary->array[1].get_string("kind", "") == "menu" && summary->array[2].get_string("kind", "") == "texture" &&
		            summary->array[0].get_int("files", 0) == 1 && summary->array[0].get_int("bytes", -1) == 0);
		const JsonValue fonts = import_preview_to_json(view, JsonPage{}, AssetKind::Font);
		const JsonValue *rows = fonts.get("rows");
		TEST_EXPECT(fonts.get_string("kind", "") == "font" && fonts.get_int("count", 0) == 1 && rows && rows->array.size() == 1 &&
		            rows->array[0].get_string("name", "") == "arial99.fnt" && fonts.get_int("not_found_count", 0) == 1 &&
		            fonts.get("summary")->array.size() == 3);
	}
	// The import section: the dialog in short, the editor's setting, no lists.
	const JsonValue summary = view_section_to_json(view, ViewSection::Import);
	TEST_EXPECT(summary.get_bool("open", false) && summary.get_bool("changed", false) &&
	            !summary.get_bool("import_dependencies", true) && summary.get("serial") == nullptr &&
	            summary.get_int("row_count", 0) == 3 && summary.get_int("not_found_count", 0) == 1 &&
	            summary.get_int("choice_count", 0) == 2 && summary.get_int("root_count", 0) == 1 &&
	            summary.get_bool("truncated", false) && summary.get("rows") == nullptr && summary.get("choices") == nullptr);
	TEST_EXPECT(import->get_bool("truncated", false) && import->get("choices") && import->get("choices")->array.size() == 2);
	const JsonValue *roots = import->get("roots");
	TEST_EXPECT(roots && roots->array.size() == 1 && roots->array[0].get_string("entry", "") == "a.mnu" &&
	            roots->array[0].get("name") == nullptr);
	const JsonValue *rows = import->get("rows");
	TEST_EXPECT(rows && rows->array.size() == 3);
	if (!rows || rows->array.size() != 3) return 1;
	const JsonValue &first = rows->array[0];
	TEST_EXPECT(first.get_string("state", "") == "selected" && first.get_string("name", "") == "a.mnu" &&
	            first.get_string("kind", "") == "menu" && first.get_string("destination", "") == "menus/a.mnu" &&
	            first.get_bool("selected", false) && first.get("needed_by") == nullptr && first.get("rivals") == nullptr);
	const JsonValue &found = rows->array[1];
	const JsonValue *need = found.get("needed_by");
	TEST_EXPECT(found.get_string("state", "") == "found" && found.get_string("found_in", "") == "the folder C:/art" && need &&
	            need->get_string("file", "") == "a.mnu" && need->get_string("record", "") == "A/GO" &&
	            need->get_string("field", "") == "font.name" && need->get_string("reference", "") == "font" &&
	            need->get_string("name", "") == "arial99" && need->get("loader_arg") == nullptr);
	const JsonValue *source = found.get("source");
	TEST_EXPECT(source && source->get_string("path", "") == "C:/art/arial99.fnt" && source->get_bool("native", false) &&
	            source->get("entry") == nullptr && source->get("install") == nullptr);
	const JsonValue *rivals = found.get("rivals");
	TEST_EXPECT(rivals && rivals->array.size() == 1 && rivals->array[0].get_string("found_in", "") == "the game install" &&
	            rivals->array[0].get_bool("differs", false) && rivals->array[0].get("source") &&
	            rivals->array[0].get("source")->get_bool("install", false) && !rivals->array[0].get("source")->get("retail"));
	TEST_EXPECT(!rows->array[2].get_bool("selected", true) &&
	            rows->array[2].get_string("problem", "").find("16 characters") != std::string::npos);
	const JsonValue *missing = import->get("not_found");
	TEST_EXPECT(missing && missing->array.size() == 1 && missing->array[0].get_string("state", "") == "not_found" &&
	            missing->array[0].get_string("name", "") == "gone.tga" && missing->array[0].get("source") == nullptr &&
	            missing->array[0].get("needed_by") && missing->array[0].get("needed_by")->get_string("reference", "") == "menu_texture");
	TEST_EXPECT(import->get_int("not_found_count", 0) == 1 && import->get_int("choice_count", 0) == 2 &&
	            import->get_int("root_count", 0) == 1);
	const JsonValue *skipped = import->get("not_followed");
	TEST_EXPECT(skipped && skipped->array.size() == 2 && skipped->array[0].get_string("reference", "") == "menu_screen" &&
	            skipped->array[0].get_int("count", 0) == 2 && skipped->array[1].get_string("kind", "") == "terrain" &&
	            skipped->array[1].get("reference") == nullptr && skipped->array[1].get_string("first", "") == "level.trn");
	// S14: the symbols no place defines, and the row sizes with what the whole plan copies.
	const JsonValue *undefined = import->get("undefined");
	TEST_EXPECT(undefined && undefined->array.size() == 1 && undefined->array[0].get_string("reference", "") == "text_id" &&
	            undefined->array[0].get_int("count", 0) == 3 && undefined->array[0].get_string("first", "") == "a.mnu");
	TEST_EXPECT(first.get("size") && import->get("total_bytes") &&
	            import->get_number("total_bytes", -1.0) == double(preview.plan->total_bytes()));
	const JsonValue *findings = import->get("diagnostics");
	TEST_EXPECT(findings && findings->array.size() == 1 && findings->array[0].get_string("code", "") == "import.unreadable");
	// A row's source is an import a request takes as it is.
	if (source) {
		JsonValue request = JsonValue::make_object();
		request.set("kind", JsonValue::make_string("import_files"));
		JsonValue imports = JsonValue::make_array();
		imports.push(*source);
		request.set("imports", std::move(imports));
		TEST_EXPECT(editor_request_from_json(request, back, error) && back.imports.size() == 1 && back.imports[0] == font.source);
	}
	return 0;
}

// The game install on the wire (S13 A4): the run section names it game_install and how the project plays
// play_mode (its own, with save_before_play) beside the mode the last game ran in (ran_mode), the import
// section counts its files as install_files, the preferences section names both; the retail keys are gone,
// and so are the editor-wide in_install, strict and ran_strict.
static int test_game_install_keys() {
	SessionView view;
	view.project.retail_directory = "C:/games/JO";
	view.project.play_mode = PlayMode::Install;
	view.project.save_before_play = false;
	view.activity.play_run_mode = "strict";
	view.project.retail_files = {"items.def", "main.mnu"};
	view.activity.play_mission = "04TR.bms"; // S14: the mission the game was started in
	const JsonValue run = view_section_to_json(view, ViewSection::Run);
	TEST_EXPECT(run.get_string("mission", "") == "04TR.bms");
	const JsonValue import = view_section_to_json(view, ViewSection::Import);
	const JsonValue preferences = view_section_to_json(view, ViewSection::Preferences);
	TEST_EXPECT(run.get_string("play_mode", "") == "install" && !run.get_bool("save_before_play", true) &&
	            run.get_string("ran_mode", "") == "strict" && run.get_string("game_install", "") == "C:/games/JO" &&
	            !run.get("retail") && !run.get("retail_directory") && !run.get("in_install") && !run.get("strict") &&
	            !run.get("ran_strict"));
	TEST_EXPECT(view_section_to_json(SessionView(), ViewSection::Run).get_string("ran_mode", "x").empty());
	TEST_EXPECT(import.get_int("install_files", 0) == 2 && !import.get("retail_files"));
	TEST_EXPECT(preferences.get_string("game_install", "") == "C:/games/JO" &&
	            preferences.get_string("play_mode", "") == "install" && !preferences.get_bool("save_before_play", true) &&
	            !preferences.get("play_in_install") && !preferences.get("retail_directory"));
	return 0;
}

// The import plan's lists page (S13 A5: the import_preview query, 200 at most a page, as the
// editor MCP's transport caps a list at 200): a plan of 250 rows, 3 not found, with 250 files to
// choose from and 250 chosen, shows 200 of each list and their counts; the next page the rest; a
// row on a later page still carries the source an import takes. The import section holds the
// counts alone.
static int test_import_pages() {
	SessionView view;
	DialogsView::ImportPreview &preview = view.dialogs.import_preview;
	preview.open = true;
	ImportPlan plan;
	for (int i = 0; i < 250; ++i) {
		const ImportChoice source{"C:/art/f" + std::to_string(i) + ".txt", "", false, false};
		preview.choices.push_back(source);
		preview.roots.push_back(source);
		ImportPlanRow row;
		row.state = ImportPlanRow::State::Selected;
		row.selected = true;
		row.source = source;
		row.name = "f" + std::to_string(i) + ".txt";
		row.kind = AssetKind::Text;
		plan.rows.push_back(row);
	}
	for (int i = 0; i < 3; ++i) {
		ImportPlanRow gone;
		gone.state = ImportPlanRow::State::NotFound;
		gone.name = "gone" + std::to_string(i) + ".tga";
		gone.kind = AssetKind::Texture;
		plan.rows.insert(plan.rows.begin() + 100 * i, gone);
	}
	preview.plan = std::make_shared<const ImportPlan>(std::move(plan));
	const JsonValue first = import_preview_to_json(view, JsonPage{0, 200});
	TEST_EXPECT(first.get_int("offset", -1) == 0 && first.get_int("count", 0) == 250 &&
	            first.get_int("not_found_count", 0) == 3 && first.get_int("choice_count", 0) == 250 &&
	            first.get_int("root_count", 0) == 250 && first.get_int("next_offset", 0) == 200);
	TEST_EXPECT(first.get("rows") && first.get("rows")->array.size() == 200 && first.get("choices")->array.size() == 200 &&
	            first.get("roots")->array.size() == 200 && first.get("not_found")->array.size() == 3);
	// The install's files to choose from, nothing chosen yet (preview_install_import {}): no row,
	// and the choices a page at a time past the rows' end, next_offset following the longest list
	// the page covers, each list with its own count.
	SessionView listed;
	listed.dialogs.import_preview.open = true;
	for (int i = 0; i < 250; ++i)
		listed.dialogs.import_preview.choices.push_back(
				{"C:/Games/JO/f" + std::to_string(i) + ".txt", "", true, false});
	listed.dialogs.import_preview.plan = std::make_shared<const ImportPlan>();
	std::vector<std::string> chosen_from;
	size_t from = 0;
	for (int guard = 0; guard < 10; ++guard) {
		const JsonValue page = import_preview_to_json(listed, JsonPage{from, 100});
		TEST_EXPECT(page.get_int("count", -1) == 0 && page.get_int("choice_count", -1) == 250 &&
		            page.get("rows")->array.empty() && page.get_int("offset", -1) == int(from));
		for (const JsonValue &choice : page.get("choices")->array)
			chosen_from.push_back(choice.get_string("path", ""));
		const JsonValue *next = page.get("next_offset");
		if (!next || next->is_null()) break;
		TEST_EXPECT(size_t(next->number) == from + 100);
		from = size_t(next->number);
	}
	TEST_EXPECT(chosen_from.size() == 250 && chosen_from.front() == "C:/Games/JO/f0.txt" &&
	            chosen_from.back() == "C:/Games/JO/f249.txt");
	const JsonValue second = import_preview_to_json(view, JsonPage{200, 200});
	const JsonValue *rows = second.get("rows");
	TEST_EXPECT(second.get_int("offset", -1) == 200 && rows && rows->array.size() == 50 &&
	            second.get("choices")->array.size() == 50 && second.get("not_found")->array.empty() &&
	            second.get("next_offset") && second.get("next_offset")->is_null());
	if (rows && !rows->array.empty()) {
		const JsonValue *source = rows->array.back().get("source");
		TEST_EXPECT(rows->array.back().get_string("name", "") == "f249.txt" && source &&
		            source->get_string("path", "") == "C:/art/f249.txt");
	}
	TEST_EXPECT(import_preview_to_json(view, JsonPage{0, 10}).get("rows")->array.size() == 10);
	const JsonValue summary = view_section_to_json(view, ViewSection::Import);
	TEST_EXPECT(summary.get_int("row_count", 0) == 250 && summary.get_int("not_found_count", 0) == 3 &&
	            summary.get_int("choice_count", 0) == 250 && summary.get_int("root_count", 0) == 250 &&
	            summary.get("rows") == nullptr && summary.get("not_found") == nullptr);
	return 0;
}

// S13 D6: an Apply edit's change is made in C++ by its document type (Edit::payload). The batch
// form (S13 A5) writes it by its op, its record and its payload's token, and the reader takes
// neither an apply edit nor a payload: the editor MCP cannot send one.
static int test_apply_edit_json() {
	struct Brush : EditPayload {
		const char *token() const override { return "raster.brush"; }
	};
	Edit brush;
	brush.operation = EditOperation::Apply;
	brush.address = {3, 0, 0};
	brush.payload = std::make_shared<Brush>();
	EditorRequest request = request::edit_record("terrain.cpt", brush);
	const JsonValue json = editor_request_to_json(request);
	const JsonValue *edits = json.get("edits");
	const JsonValue *edit = edits && edits->array.size() == 1 ? &edits->array[0] : nullptr;
	TEST_EXPECT(edit && edit->get_string("op", "") == "apply" && edit->get_int("id", 0) == 3 &&
	            edit->get_string("payload", "") == "raster.brush");
	EditorRequest back;
	std::string error;
	const std::string apply_refused = "edits[0]: an apply edit carries a change its document type "
	                                  "makes in C++: a batch cannot send one.";
	const std::string payload_refused = "edits[1]: \"payload\" names a change a document type "
	                                    "makes in C++; the editor's JSON cannot carry one.";
	TEST_EXPECT(!editor_request_from_json(json, back, error) && error == apply_refused);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edits\":[{\"op\":\"apply\"}]}", back) ==
	            apply_refused);
	const std::string file_value = "{\"op\":\"set_file_value\",\"field\":\"f\",\"value\":1";
	const std::string with_payload = "{\"kind\":\"edit_record\",\"edits\":[" + file_value + "}," +
	                                 file_value + ",\"payload\":\"raster.brush\"}]}";
	TEST_EXPECT(request_error(with_payload.c_str(), back) == payload_refused);
	// No payload on any other edit's batch form.
	Edit set_file;
	set_file.operation = EditOperation::SetFileValue;
	set_file.field = "f";
	set_file.value = int64_t(1);
	request.edits = {set_file};
	const JsonValue set = editor_request_to_json(request);
	const JsonValue *set_edits = set.get("edits");
	TEST_EXPECT(set_edits && set_edits->array.size() == 1 && !set_edits->array[0].get("payload"));
	TEST_EXPECT(editor_request_from_json(set, back, error) && back.edits.size() == 1 &&
	            !back.edits[0].payload && back.edits[0] == set_file);
	return 0;
}

// The view events (S13 V4) by page (S13 A5: the events query, events_page_to_json), by seq as
// the output lines are, the last 64 held. Seventy posted, the first six are gone: `first` is 7,
// `next` 71, and a page from 0 starts at the first held. Paging by `next_cursor`, ten at a time,
// reads every held event once, in order, with no gap; an event posted between two pages is on
// the next one; a cursor past `next` reads none. Each item carries its seq, its kind's token and
// the fields its kind sets (none of the others); the events section says what is held, and no
// section carries the serials the events replaced.
static int test_view_events_json() {
	TEST_EXPECT(ViewEvents::kKept == 64);
	for (size_t i = 0; i < kViewEventKindCount; ++i)
		TEST_EXPECT(std::string(view_event_kind_token(static_cast<ViewEventKind>(i))) ==
				kViewEventKindRows[i].token);
	TEST_EXPECT(
			std::string(view_event_kind_token(ViewEventKind::RevealRecord)) == "reveal_record" &&
			std::string(view_event_kind_token(ViewEventKind::RevealFile)) == "reveal_file" &&
			std::string(view_event_kind_token(ViewEventKind::AskRename)) == "ask_rename" &&
			std::string(view_event_kind_token(ViewEventKind::SettingsApplied)) ==
					"settings_applied" &&
			std::string(view_event_kind_token(ViewEventKind::ImportPlanned)) == "import_planned");
	SessionView view;
	const auto page = [&view](uint64_t cursor, size_t limit) {
		return events_page_to_json(view.events, cursor, limit);
	};
	const JsonValue empty = page(0, ViewEvents::kKept);
	TEST_EXPECT(empty.get_int("first", 0) == 1 && empty.get_int("next", 0) == 1 &&
			empty.get_int("cursor", 0) == 1 && empty.get_int("next_cursor", 0) == 1 &&
			empty.get_int("count", -1) == 0 && empty.get("items") && empty.get("items")->array.empty());
	for (int i = 0; i < 70; ++i) {
		ViewEvent event;
		event.kind = static_cast<ViewEventKind>(i % int(kViewEventKindCount));
		event.path = "menus/m" + std::to_string(i) + ".mnu";
		TEST_EXPECT(view.events.post(event) == uint64_t(i + 1));
	}
	TEST_EXPECT(view.events.held().size() == 64 && view.events.first_seq() == 7 &&
			view.events.next_seq() == 71 && view.events.held().front().path == "menus/m6.mnu");
	const JsonValue all = page(0, ViewEvents::kKept);
	TEST_EXPECT(all.get_int("first", 0) == 7 && all.get_int("next", 0) == 71 &&
			all.get_int("cursor", 0) == 7 && all.get_int("next_cursor", 0) == 71 &&
			all.get_int("count", 0) == 64 && all.get("items")->array.size() == 64);
	const JsonValue held = view_section_to_json(view, ViewSection::Events);
	TEST_EXPECT(held.get_int("first", 0) == 7 && held.get_int("next", 0) == 71 && held.get_int("count", 0) == 64 &&
	            held.get("items") == nullptr);
	std::vector<int64_t> seqs;
	uint64_t cursor = 0;
	for (int pages = 0; pages < 20; ++pages) {
		const JsonValue listed = page(cursor, 10);
		TEST_EXPECT(listed.get("items")->array.size() <= 10);
		for (const JsonValue &item : listed.get("items")->array)
			seqs.push_back(item.get_int("seq", 0));
		cursor = uint64_t(listed.get_int("next_cursor", 0));
		if (listed.get("items")->array.empty())
			break;
		if (pages == 2) {
			// Posted between two pages: the next page carries it.
			ViewEvent late;
			late.kind = ViewEventKind::RevealFile;
			late.path = "late.tga";
			view.events.post(late);
		}
	}
	TEST_EXPECT(seqs.size() == 65 && seqs.front() == 7 && seqs.back() == 71);
	for (size_t i = 1; i < seqs.size(); ++i)
		TEST_EXPECT(seqs[i] == seqs[i - 1] + 1);
	const JsonValue past = page(500, 10);
	TEST_EXPECT(past.get_int("cursor", 0) == 72 && past.get("items")->array.empty());
	TEST_EXPECT(page(3, 1).get("items")->array[0].get_int("seq", 0) == 8);

	// An event's fields: those its kind sets, none of the others.
	SessionView fields;
	ViewEvent reveal;
	reveal.kind = ViewEventKind::RevealRecord;
	reveal.path = "menus/main.mnu";
	reveal.address = { 12, 3, 45 };
	reveal.field = "string.value";
	fields.events.post(reveal);
	ViewEvent file;
	file.kind = ViewEventKind::RevealFile;
	file.path = "art/a_name_too_long_for_archives.tga";
	file.flag = true;
	fields.events.post(file);
	ViewEvent applied;
	applied.kind = ViewEventKind::SettingsApplied;
	applied.tag = 9;
	fields.events.post(applied);
	const JsonValue json = events_page_to_json(fields.events, 0, ViewEvents::kKept);
	const std::vector<JsonValue> &items = json.get("items")->array;
	TEST_EXPECT(items.size() == 3);
	if (items.size() != 3)
		return 1;
	const JsonValue *address = items[0].get("address");
	TEST_EXPECT(items[0].get_int("seq", 0) == 1 &&
			items[0].get_string("kind", "") == "reveal_record" &&
			items[0].get_string("path", "") == "menus/main.mnu" &&
			items[0].get_string("field", "") == "string.value" && address &&
			address->get_int("row", 0) == 12 && address->get_int("kind", 0) == 3 &&
			address->get_int("child", 0) == 45 && !items[0].get("flag") && !items[0].get("tag"));
	TEST_EXPECT(items[1].get_string("kind", "") == "reveal_file" &&
			items[1].get_bool("flag", false) &&
			items[1].get_string("path", "") == "art/a_name_too_long_for_archives.tga" &&
			!items[1].get("address") && !items[1].get("field") && !items[1].get("tag"));
	TEST_EXPECT(items[2].get_string("kind", "") == "settings_applied" &&
			items[2].get_int("tag", 0) == 9 && !items[2].get("path") && !items[2].get("flag"));
	TEST_EXPECT(view_event_to_json(fields.events.held().back()).get_int("tag", 0) == 9);
	// The serials the events replaced are gone: no section carries one.
	for (size_t i = 0; i < kViewSectionCount; ++i) {
		const JsonValue section = view_section_to_json(fields, static_cast<ViewSection>(i));
		for (const char *gone : { "reveal_field", "reveal_serial", "reveal_file", "serial" })
			TEST_EXPECT(section.get(gone) == nullptr);
	}
	const JsonValue dialogs = view_section_to_json(fields, ViewSection::Dialogs);
	TEST_EXPECT(dialogs.get("settings_result") && dialogs.get("settings_result")->get("serial") == nullptr);
	return 0;
}

// The batch form's table (record_batch.h, S13 D9): the readers read the ops and members it lists and
// refuse the rest. In each form, every member the table lists for it is never refused as unknown
// there and every other one is (a payload over records refused in its own words); each op is read
// in its own form and refused as an op in the others.
static int test_batch_table() {
	const auto read = [](RecordBatchForm form, const std::string &edit, std::string &error) {
		JsonValue json;
		std::string unparsed;
		opennova::io::json_parse("[" + edit + "]", json, unparsed);
		RecordBatch batch;
		return record_batch_from_json(json, nullptr, form, batch, error, false);
	};
	const auto sample = [](const BatchMember &member) -> std::string {
		switch (member.json) {
		case BatchJson::String: return member.only[0] ? std::string("\"") + member.only + "\"" : std::string("\"x\"");
		case BatchJson::Boolean: return "true";
		case BatchJson::Records: return "[]";
		case BatchJson::Integer:
		case BatchJson::Id:
		case BatchJson::Value: break;
		}
		return "1";
	};
	// A form's first op, which its edits name ("" for the fields form, which names none).
	const auto first_op = [](RecordBatchForm form) {
		for (const BatchOp &op : batch_ops())
			if (op.form == form) return std::string(op.token);
		return std::string();
	};
	size_t members = 0, ops = 0;
	for (size_t f = 0; f < kRecordBatchFormCount; ++f) {
		const auto form = static_cast<RecordBatchForm>(f);
		const std::string op = first_op(form);
		for (const BatchMember &member : batch_members()) {
			if (std::string(member.name) == "op") continue;
			const std::string edit = "{" + (op.empty() ? std::string() : "\"op\": \"" + op + "\", ") + "\"" +
			                         member.name + "\": " + sample(member) + "}";
			std::string error;
			read(form, edit, error);
			const bool unknown = error.find("Unknown edits[0] member") != std::string::npos;
			const bool listed = (member.forms & batch_form_bit(form)) != 0;
			const bool payload = std::string(member.name) == "payload" && form == RecordBatchForm::Edits;
			if (listed ? unknown : !(unknown || payload))
				std::fprintf(stderr, "batch table: %s in the %s form: %s\n", member.name, batch_form_token(form),
				             error.c_str());
			TEST_EXPECT(listed ? !unknown : (unknown || payload));
			++members;
		}
	}
	for (const BatchOp &op : batch_ops())
		for (size_t f = 0; f < kRecordBatchFormCount; ++f) {
			const auto form = static_cast<RecordBatchForm>(f);
			if (form == RecordBatchForm::Fields) continue; // its edits name no op
			std::string error;
			read(form, std::string("{\"op\": \"") + op.token + "\"}", error);
			const bool refused = error.find("unknown edit op") != std::string::npos ||
			                     error.find("edits alone") != std::string::npos ||
			                     error.find("an apply edit carries") != std::string::npos;
			TEST_EXPECT((op.form == form || op.every_form) != refused);
			++ops;
		}
	// The catalog writes the table.
	TEST_EXPECT(std::string(batch_form_token(RecordBatchForm::Spans)) == "spans" &&
	            std::string(batch_json_token(BatchJson::Id)) == "id");
	std::printf("batch table: %zu ops, %zu members, %zu reads of a member, %zu of an op\n", batch_ops().count,
	            batch_members().count, members, ops);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_batch_table();
	failures += test_apply_edit_json();
	failures += test_tokens();
	failures += test_asset_kind_tokens();
	failures += test_import_pages();
	failures += test_game_install_keys();
	failures += test_request_round_trip();
	failures += test_request_table_samples();
	failures += test_settings_json();
	failures += test_problem_groups_page();
	failures += test_over_a_session();
	failures += test_graph_style_fields();
	failures += test_search_json();
	failures += test_field_metadata();
	failures += test_import_plan_json();
	failures += test_view_events_json();
	if (failures == 0) std::printf("editor_session_json: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
