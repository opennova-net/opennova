// The session's wire form (ADR 0046 d10, the editor MCP): every request kind, edit
// operation, pick purpose, unsaved choice, selection mode, severity and Problems scope and
// grouping has a token that reads back; a request survives the JSON round trip with its
// edit (owner and gesture included), its batch of edits, paths, names and imports, and so
// does the request of every kind of fix; a malformed request or Problems query is refused
// with a reason; the project settings are one request whose settings are each optional,
// and the view says what the last one came to; and over a real session the view (the
// selection and the field a request asked to show, the clipboard), a document with its
// records at every depth, a record (its path, locator, owner and fields as they apply) and
// the findings serialize the state the windows draw, with the same edit reaching the record
// through JSON as through the typed request; the Problems answer carries the counts, the
// groups, what each finding is about and its fixes, whose requests read back and do what
// they say; the unsaved prompt names what waits and its files, and its "save" answer writes
// them and runs what waited.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_query.h>
#include <editor/session/project_session.h>
#include <editor/session/session_json.h>
#include <editor/session/session_view.h>

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

} // namespace

static int test_tokens() {
	// Every kind to the enum's end has a token that reads back to it, and no two share one.
	for (int i = 0; i < static_cast<int>(EditorRequestKind::kCount); ++i) {
		const auto kind = static_cast<EditorRequestKind>(i);
		const std::string token = editor_request_kind_token(kind);
		TEST_EXPECT(!token.empty());
		EditorRequestKind back = EditorRequestKind::Rescan;
		TEST_EXPECT(editor_request_kind_from_token(token, back) && back == kind);
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
	for (int i = 0; i <= static_cast<int>(PickPurpose::ImportFiles); ++i) {
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
	return 0;
}

// The asset kinds' tokens on the wire (S13 D5): a scanned file's kind is its row's token, the
// sound banks as the game names them (a .lwf the sound bank, a .sbf the music bank) and the
// kinds S13 D5 added; the reference kind a menu SOUND names its bank by is sound_bank too, and
// wave_bank names nothing (pre-1.0, no alias).
static int test_asset_kind_tokens() {
	editor_test::TempProjectDir dir("opennova_session_json_kinds");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Kinds"));
	const std::string root = session.view().project_root;
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
	session.handle(make_request(EditorRequestKind::Rescan));
	const JsonValue json = session_view_to_json(session.view());
	for (const auto &file : files) {
		const std::string name = std::filesystem::path(file.first).filename().string();
		std::string kind;
		for (const JsonValue &row : json.get("project")->get("files")->array)
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
	EditorRequest request = make_request(EditorRequestKind::EditRecord, "menus/main.mnu");
	request.edit.operation = EditOperation::Set;
	request.edit.address = {7, 1, 9};
	request.edit.field = "text";
	request.edit.value = std::string("Hello");
	request.edit.coalesce = true;
	JsonValue parsed;
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(request)).c_str(), parsed));
	EditorRequest back;
	std::string error;
	TEST_EXPECT(editor_request_from_json(parsed, back, error));
	TEST_EXPECT(back.kind == EditorRequestKind::EditRecord && back.path == "menus/main.mnu");
	TEST_EXPECT(back.edit.operation == EditOperation::Set && back.edit.address == request.edit.address);
	TEST_EXPECT(back.edit.field == "text" && std::get<std::string>(back.edit.value) == "Hello" && back.edit.coalesce);
	TEST_EXPECT(back.edit.position == SIZE_MAX && back.edit.parent == 0 && back.edit.gesture == 0 && back.edits.empty());

	// An owner, a gesture, a batch and a selection mode.
	EditorRequest batch = make_request(EditorRequestKind::EditRecord, "menus/main.mnu");
	batch.edit.operation = EditOperation::Move;
	batch.edit.address = {7, 1, 9};
	batch.edit.parent = 11;
	batch.edit.position = 2;
	Edit left, clear;
	left.address = {7, 1, 9};
	left.field = "left";
	left.value = int64_t(40);
	left.gesture = 5;
	clear.operation = EditOperation::Clear;
	clear.address = {7, 1, 9};
	clear.field = "right";
	clear.gesture = 5;
	batch.edits = {left, clear};
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(batch)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error));
	TEST_EXPECT(back.edit.operation == EditOperation::Move && back.edit.parent == 11 && back.edit.position == 2);
	TEST_EXPECT(back.edits.size() == 2 && back.edits[0].field == "left" && std::get<int64_t>(back.edits[0].value) == 40 &&
	            back.edits[0].gesture == 5 && back.edits[1].operation == EditOperation::Clear && back.edits[1].gesture == 5);
	EditorRequest select = make_request(EditorRequestKind::SelectRecord, "menus/main.mnu");
	select.edit.address = {7, 1, 9};
	select.select_mode = SelectMode::Toggle;
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(select)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.select_mode == SelectMode::Toggle && back.edit.address == select.edit.address);
	TEST_EXPECT(request_error("{\"kind\":\"paste\",\"edit\":{\"parent\":4,\"position\":1}}", back).empty());
	TEST_EXPECT(back.kind == EditorRequestKind::Paste && back.edit.parent == 4 && back.edit.position == 1);
	// A Go to (S12 D3): the document opened at a record by its locator, its field shown; the
	// request by symbol it replaced is no token.
	EditorRequest go_to = make_request(EditorRequestKind::OpenDocument, "gametext.bin", "1/string:0");
	go_to.edit.field = "key";
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(go_to)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.kind == EditorRequestKind::OpenDocument &&
	            back.path == "gametext.bin" && back.text == "1/string:0" && back.edit.field == "key");
	EditorRequestKind gone = EditorRequestKind::Rescan;
	TEST_EXPECT(!editor_request_kind_from_token("go_to_record", gone));

	EditorRequest import = make_request(EditorRequestKind::ImportFiles);
	import.flag = true;
	import.imports = {{"C:/data/localres.pff", "MAIN.MNU"}, {"C:/data/loose.txt", ""}};
	ImportSource native; // a loose file copied as the game's own (S11f)
	native.path = "C:/data/logo.png";
	native.native = true;
	import.imports.push_back(native);
	import.paths = {"C:/data/localres.pff"};
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(import)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error));
	TEST_EXPECT(back.kind == EditorRequestKind::ImportFiles && back.flag && back.paths == import.paths);
	TEST_EXPECT(back.imports.size() == 3 && back.imports[0].entry == "MAIN.MNU" && back.imports[1].entry.empty());
	TEST_EXPECT(!back.imports[0].native && !back.imports[1].native && back.imports[2].native && !back.imports[2].retail);
	TEST_EXPECT(request_error("{\"kind\":\"import_files\",\"imports\":[{\"path\":\"x\",\"native\":1}]}", back).find("native") !=
	            std::string::npos);

	EditorRequest pick = make_request(EditorRequestKind::PickFile);
	pick.purpose = PickPurpose::RuntimeExecutable;
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(pick)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.purpose == PickPurpose::RuntimeExecutable);

	// The request of every kind of fix (problem_fixes.h) reads back as it was written, and a
	// fix's JSON carries it.
	EditorRequest create = make_request(EditorRequestKind::CreateMissing);
	create.names = {"main_menu", "gametext"};
	EditorRequest listed = make_request(EditorRequestKind::PreviewRetailImport);
	listed.names = {"MAIN.MNU"};
	listed.flag = true; // with the files it needs (S11g)
	EditorRequest again = make_request(EditorRequestKind::Reimport, "art/logo.png");
	again.flag = true;
	const EditorRequest fixes[] = {create, listed, make_request(EditorRequestKind::AssignRequirement, "menus/a.mnu", "main_menu"),
	                               make_request(EditorRequestKind::CreateFile, "Arial99.fnt", "font"), again,
	                               make_request(EditorRequestKind::Save, "defs/items.def")};
	for (const EditorRequest &fix : fixes) {
		TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(fix)).c_str(), parsed));
		EditorRequest read;
		TEST_EXPECT(editor_request_from_json(parsed, read, error));
		TEST_EXPECT(read.kind == fix.kind && read.path == fix.path && read.text == fix.text && read.flag == fix.flag &&
		            read.names == fix.names);
	}
	const JsonValue fix_json = problem_fix_to_json(ProblemFix{"Create main.mnu", "Creates the startup screen.", create, true});
	TEST_EXPECT(fix_json.get_string("label", "") == "Create main.mnu" && fix_json.get_bool("bulk", false) &&
	            fix_json.get_string("detail", "") == "Creates the startup screen." && fix_json.get("request") != nullptr);
	if (const JsonValue *request = fix_json.get("request"))
		TEST_EXPECT(editor_request_from_json(*request, back, error) && back.kind == EditorRequestKind::CreateMissing &&
		            back.names == create.names);
	TEST_EXPECT(request_error("{\"kind\":\"create_missing\",\"names\":\"main_menu\"}", back).find("names") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"create_missing\",\"names\":[3]}", back).find("names") != std::string::npos);

	EditorRequest resolve = make_request(EditorRequestKind::ResolveUnsaved);
	resolve.unsaved_choice = UnsavedChoice::Discard;
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(resolve)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.unsaved_choice == UnsavedChoice::Discard);
	// The prompt's Save writes exactly the files it lists: "save" (no longer "save_all").
	TEST_EXPECT(std::string(unsaved_choice_token(UnsavedChoice::Save)) == "save");
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"unsaved_choice\":\"save\"}", back).empty() &&
	            back.unsaved_choice == UnsavedChoice::Save);
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"unsaved_choice\":\"save_all\"}", back).find("save_all") !=
	            std::string::npos);

	// Numbers: a whole number is an integer value, a fraction a real, a bool 0 / 1.
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"row\":3,\"field\":\"hp\",\"value\":40}}", back).empty());
	TEST_EXPECT(std::get<int64_t>(back.edit.value) == 40 && back.edit.address.row == 3);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"row\":3,\"value\":2.5}}", back).empty());
	TEST_EXPECT(std::get<double>(back.edit.value) == 2.5);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"row\":3,\"value\":true}}", back).empty());
	TEST_EXPECT(std::get<int64_t>(back.edit.value) == 1);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"operation\":\"move\",\"row\":3,\"kind\":1,\"child\":4,\"position\":0}}", back).empty());
	TEST_EXPECT(back.edit.operation == EditOperation::Move && back.edit.position == 0 && back.edit.address.child == 4);
	TEST_EXPECT(request_error("{\"kind\":\"build\"}", back).empty() && back.kind == EditorRequestKind::Build);

	// Refusals name what is wrong.
	TEST_EXPECT(request_error("[\"build\"]", back).find("object") != std::string::npos);
	TEST_EXPECT(request_error("{\"path\":\"x\"}", back).find("kind") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":5}", back).find("kind") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"nope\"}", back).find("nope") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"build\",\"flagg\":true}", back).find("flagg") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"build\",\"flag\":\"yes\"}", back).find("flag") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"row\":-1}}", back).find("row") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"operation\":\"teleport\"}}", back).find("teleport") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"value\":[1]}}", back).find("value") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"rows\":1}}", back).find("rows") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"pick_file\",\"purpose\":\"anything\"}", back).find("anything") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"import_files\",\"imports\":[{\"entry\":\"X\"}]}", back).find("path") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"unsaved_choice\":\"later\"}", back).find("later") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"select_record\",\"mode\":\"extend\"}", back).find("extend") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"gesture\":-2}}", back).find("gesture") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edit\":{\"parent\":\"x\"}}", back).find("parent") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edits\":{\"row\":1}}", back).find("edits") != std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"edit_record\",\"edits\":[{\"operation\":\"fold\"}]}", back).find("fold") != std::string::npos);
	return 0;
}

// apply_project_settings (S11d): the one request the project settings take, its settings
// each optional (one left out is not set), read strictly; the five requests it replaced are
// no tokens. Over a session, the view says what the last one came to under its serial, and
// names the runtime the settings name apart from the one Play resolves.
static int test_settings_json() {
	EditorRequestKind kind = EditorRequestKind::Rescan;
	TEST_EXPECT(editor_request_kind_from_token("apply_project_settings", kind) && kind == EditorRequestKind::ApplyProjectSettings);
	for (const char *gone : {"set_title", "set_feature", "set_runtime_executable", "set_retail_directory", "set_play_retail"})
		TEST_EXPECT(!editor_request_kind_from_token(gone, kind));
	EditorRequest all = make_request(EditorRequestKind::ApplyProjectSettings);
	all.settings.serial = 12;
	all.settings.title = "Harbor";
	all.settings.mission = true;
	all.settings.multiplayer = false;
	all.settings.retail_directory = "C:/games/Joint Operations";
	all.settings.runtime_executable = "";
	all.settings.play_retail = true;
	JsonValue parsed;
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(all)).c_str(), parsed));
	EditorRequest back;
	std::string error;
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.kind == EditorRequestKind::ApplyProjectSettings);
	const ProjectSettingsChange &read = back.settings;
	TEST_EXPECT(read.serial == 12 && read.title == std::optional<std::string>("Harbor") && read.mission == std::optional<bool>(true) &&
	            read.multiplayer == std::optional<bool>(false) &&
	            read.retail_directory == std::optional<std::string>("C:/games/Joint Operations") &&
	            read.runtime_executable == std::optional<std::string>("") && read.play_retail == std::optional<bool>(true));
	// One setting named: the others are not set.
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"play_retail\":false}}", back).empty());
	TEST_EXPECT(back.settings.play_retail == std::optional<bool>(false) && back.settings.serial == 0 && !back.settings.title &&
	            !back.settings.mission && !back.settings.multiplayer && !back.settings.retail_directory &&
	            !back.settings.runtime_executable);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\"}", back).empty() && !back.settings.title &&
	            !back.settings.play_retail);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"titel\":\"X\"}}", back).find("titel") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"mission\":\"yes\"}}", back).find("mission") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"serial\":-1}}", back).find("serial") !=
	            std::string::npos);
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":[]}", back).find("settings") != std::string::npos);

	// Over a session: the result of the last one under its serial, and the runtime setting.
	editor_test::TempProjectDir dir("opennova_session_json_settings_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	TEST_EXPECT(session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Settings")));
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"serial\":4,\"title\":\"Harbor\","
	                          "\"runtime_executable\":\"C:/tools/opennova.exe\"}}",
	                          back)
	                    .empty());
	session.handle(back);
	JsonValue view = session_view_to_json(session.view());
	const JsonValue *result = view.get("settings_result");
	TEST_EXPECT(result && result->get_int("serial", 0) == 4 && result->get("failures") && result->get("failures")->array.empty());
	TEST_EXPECT(view.get("project") && view.get("project")->get_string("title", "") == "Harbor");
	TEST_EXPECT(view.get("play") && view.get("play")->get_string("runtime_setting", "") == "C:/tools/opennova.exe");
	// A name the project cannot take: the failure is the result's, under the next serial.
	TEST_EXPECT(request_error("{\"kind\":\"apply_project_settings\",\"settings\":{\"serial\":5,\"title\":\"\"}}", back).empty());
	session.handle(back);
	view = session_view_to_json(session.view());
	result = view.get("settings_result");
	TEST_EXPECT(result && result->get_int("serial", 0) == 5 && result->get("failures") &&
	            result->get("failures")->array.size() == 1 &&
	            result->get("failures")->array[0].get_string("code", "") == "project.title_empty");
	return 0;
}

// A Problems query from its wire form: nothing asked is the default query and every row;
// each member reads, an empty severity list shows nothing, and a malformed one is refused
// with the query left as it was.
static int test_problem_query_json() {
	ProblemQuery query;
	size_t offset = 7, limit = 7;
	std::string error;
	JsonValue json;
	TEST_EXPECT(parse("{}", json) && problem_query_from_json(json, query, offset, limit, error));
	TEST_EXPECT(query == ProblemQuery() && offset == 0 && limit == SIZE_MAX);
	TEST_EXPECT(parse("{\"severities\":[\"error\",\"info\"],\"text\":\"Menu\",\"scope\":\"open_files\",\"fixable\":true,"
	                  "\"group\":\"kind\",\"offset\":2,\"limit\":5}",
	                  json));
	TEST_EXPECT(problem_query_from_json(json, query, offset, limit, error));
	TEST_EXPECT(query.errors && !query.warnings && query.infos && query.text == "Menu" &&
	            query.scope == ProblemScope::OpenFiles && query.fixable && query.grouping == ProblemGrouping::Kind &&
	            offset == 2 && limit == 5);
	TEST_EXPECT(parse("{\"severities\":[]}", json) && problem_query_from_json(json, query, offset, limit, error));
	TEST_EXPECT(!query.errors && !query.warnings && !query.infos);
	const ProblemQuery kept = query;
	for (const char *bad : {"[]", "{\"severity\":\"error\"}", "{\"severities\":\"error\"}", "{\"severities\":[\"fatal\"]}",
	                        "{\"scope\":\"everything\"}", "{\"group\":\"folder\"}", "{\"fixable\":1}", "{\"offset\":-1}",
	                        "{\"limit\":2.5}", "{\"text\":3}"}) {
		error.clear();
		TEST_EXPECT(parse(bad, json) && !problem_query_from_json(json, query, offset, limit, error) && !error.empty());
		TEST_EXPECT(query == kept && offset == 0 && limit == SIZE_MAX);
	}
	return 0;
}

// A grouped page names the groups of its own problems, each whole (its first row among the
// shown, its counts), and says how many there are in all: a project with a group per file
// (250 here, past what the editor MCP carries in one list) answers any page in a few.
static int test_problem_groups_page() {
	SessionView view;
	view.project_open = true;
	for (int i = 0; i < 250; ++i) {
		const std::string file = "defs/f" + std::to_string(1000 + i) + ".def";
		view.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Warning, "catalog.test", "A finding.", file));
		if (i == 20) // a second finding in one file: that group holds two rows
			view.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Warning, "catalog.test", "Another.", file));
	}
	ProblemQuery by_file;
	by_file.grouping = ProblemGrouping::File;
	const ProblemAnswer answer = answer_problems(by_file, view);
	TEST_EXPECT(answer.groups.size() == 250 && answer.rows.size() == 251);
	ProblemFixCache fixes;
	JsonValue page = problems_to_json(view, answer, 10, 5, fixes);
	TEST_EXPECT(page.get_int("group_count", 0) == 250 && page.get_int("shown", 0) == 251);
	TEST_EXPECT(page.get("problems")->array.size() == 5 && page.get("groups")->array.size() == 5);
	for (size_t i = 0; i < 5; ++i) {
		const JsonValue &group = page.get("groups")->array[i];
		TEST_EXPECT(group.get_int("first", -1) == int(10 + i) && group.get_int("count", 0) == 1);
		TEST_EXPECT(page.get("problems")->array[i].get_string("group", "") == group.get_string("key", "x"));
	}
	// A page that starts inside a group and ends in the next: both, whole.
	page = problems_to_json(view, answer, 21, 2, fixes);
	TEST_EXPECT(page.get("groups")->array.size() == 2);
	if (page.get("groups")->array.size() == 2) {
		const JsonValue &shared = page.get("groups")->array[0];
		TEST_EXPECT(shared.get_string("key", "") == "defs/f1020.def" && shared.get_int("first", -1) == 20 &&
		            shared.get_int("count", 0) == 2 && shared.get_int("warnings", 0) == 2);
		TEST_EXPECT(page.get("groups")->array[1].get_int("first", -1) == 22);
	}
	// A page past the rows: no problems, no groups, the count still all of them.
	page = problems_to_json(view, answer, 400, 5, fixes);
	TEST_EXPECT(page.get("problems")->array.empty() && page.get("groups")->array.empty() &&
	            page.get_int("group_count", 0) == 250);
	// Ungrouped: no group metadata at all.
	page = problems_to_json(view, answer_problems(ProblemQuery(), view), 0, 5, fixes);
	TEST_EXPECT(page.get("groups") == nullptr && page.get("group_count") == nullptr);
	return 0;
}

static int test_over_a_session() {
	editor_test::TempProjectDir dir("opennova_session_json_test");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();

	// No project: the view says so and carries no rows.
	JsonValue json = session_view_to_json(view);
	TEST_EXPECT(!json.get("project")->get_bool("open", true));
	TEST_EXPECT(json.get("requirements")->get_int("total", -1) == 0);
	TEST_EXPECT(json.get("play")->get_string("state", "") == "stopped");
	TEST_EXPECT(json.get("documents")->is_array() && json.get("documents")->array.empty());
	TEST_EXPECT(json.get_string("status", "") == "No project open.");

	// New project through JSON: the checklist is unmet, every row listed.
	const std::string root = dir.file("John Smith");
	EditorRequest request;
	TEST_EXPECT(request_error(("{\"kind\":\"new_project\",\"path\":\"" + root + "\",\"text\":\"John Smith\"}").c_str(), request).empty());
	TEST_EXPECT(session.handle(request));
	json = session_view_to_json(view);
	const JsonValue *project = json.get("project");
	TEST_EXPECT(project->get_bool("open", false) && project->get_string("title", "") == "John Smith");
	TEST_EXPECT(project->get_string("target_game", "") == "jo" && project->get("features")->get_bool("menu", false));
	const JsonValue *requirements = json.get("requirements");
	TEST_EXPECT(requirements->get_int("total", 0) > 0 &&
	            requirements->get_int("missing", -1) == requirements->get_int("total", 0));
	TEST_EXPECT(requirements->get("rows")->array.size() == view.requirements.rows.size());
	const JsonValue &first_row = requirements->get("rows")->array.front();
	TEST_EXPECT(first_row.get_string("state", "") == "missing" && !first_row.get_string("role", "").empty() &&
	            !first_row.get_string("phase", "").empty());
	TEST_EXPECT(json.get("problems")->get_int("errors", 0) == requirements->get_int("total", 0));
	// No game runs: no endpoint, no exit code.
	TEST_EXPECT(json.get("play")->get_int("mcp_port", -1) == 0 && json.get("play")->get("exit_code")->is_null());
	TEST_EXPECT(json.get("recent_projects")->array.size() == 1);
	TEST_EXPECT(json.get("graph")->get_int("missing", -1) == 0);
	TEST_EXPECT(diagnostics_to_json(view.diagnostics).array.size() == view.diagnostics.size());
	// The first row, the manifest's first: an optional file the game does without, a note.
	const JsonValue first_finding = diagnostics_to_json(view.diagnostics).array.front();
	TEST_EXPECT(first_finding.get_string("severity", "") == "info" &&
	            first_finding.get_string("code", "") == "requirement.optional_missing" &&
	            first_finding.get_string("role", "") == "game_cfg" && first_finding.get_string("target", "") == "game.cfg");

	// Problems through the wire: every finding counted, the errors shown, each required file
	// the project lacks naming its role and file (no file of the project's) with its fixes.
	ProblemQuery errors_only;
	errors_only.warnings = errors_only.infos = false;
	ProblemFixCache fix_cache;
	const JsonValue problems = problems_to_json(view, answer_problems(errors_only, view), 0, SIZE_MAX, fix_cache);
	const int required = requirements->get_int("total", 0);
	TEST_EXPECT(problems.get_int("total", 0) == int(view.diagnostics.size()) && problems.get_int("shown", 0) == required);
	const JsonValue *counts = problems.get("counts");
	TEST_EXPECT(counts && counts->get_int("errors", 0) == required && counts->get_int("infos", 0) > 0 &&
	            counts->get_int("errors", 0) + counts->get_int("warnings", 0) + counts->get_int("infos", 0) ==
	                    int(view.diagnostics.size()));
	TEST_EXPECT(problems.get("groups") == nullptr && problems.get("problems")->array.size() == size_t(required));
	const JsonValue &missing = problems.get("problems")->array.front();
	TEST_EXPECT(missing.get_string("code", "") == "requirement.missing" && missing.get("asset") == nullptr &&
	            missing.get_string("role", "") == "gameerr" && missing.get_string("target", "") == "gameerr.bin");
	const JsonValue *fixes = missing.get("fixes");
	TEST_EXPECT(fixes && !fixes->array.empty());
	if (!fixes || fixes->array.empty()) return 1;
	const JsonValue &create_fix = fixes->array.front();
	TEST_EXPECT(create_fix.get_string("label", "") == "Create gameerr.bin" && create_fix.get_bool("bulk", false));
	// The fix's request, as the wire carries it, does what its label says.
	std::string parse_error;
	TEST_EXPECT(create_fix.get("request") && editor_request_from_json(*create_fix.get("request"), request, parse_error));
	TEST_EXPECT(request.kind == EditorRequestKind::CreateMissing && request.names == std::vector<std::string>{"gameerr"});
	TEST_EXPECT(session.handle(request) && session.outcome().done() && view.scan.find("gameerr.bin") != nullptr);
	// A page, and the rows grouped by kind: one group, its title in plain words.
	const JsonValue page = problems_to_json(view, answer_problems(errors_only, view), 1, 2, fix_cache);
	const JsonValue all = problems_to_json(view, answer_problems(errors_only, view), 0, SIZE_MAX, fix_cache);
	TEST_EXPECT(page.get("problems")->array.size() == 2 && all.get("problems")->array.size() > 2 &&
	            page.get("problems")->array[0].get_string("message", "") == all.get("problems")->array[1].get_string("message", ""));
	ProblemQuery by_kind = errors_only;
	by_kind.grouping = ProblemGrouping::Kind;
	const JsonValue grouped = problems_to_json(view, answer_problems(by_kind, view), 0, SIZE_MAX, fix_cache);
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
	TEST_EXPECT(session.outcome().done() && view.requirements.required_missing == required - 1);
	std::string roles;
	json = session_view_to_json(view);
	for (const JsonValue &row : json.get("requirements")->get("rows")->array)
		if (row.get_bool("required", false) && row.get_string("state", "") != "present")
			roles += (roles.empty() ? "\"" : ",\"") + row.get_string("role", "") + "\"";
	TEST_EXPECT(request_error(("{\"kind\":\"create_missing\",\"names\":[" + roles + "]}").c_str(), request).empty() &&
	            session.handle(request));
	TEST_EXPECT(view.requirements.required_missing == 0);
	// Every file the scan lists, each with its kind and whether the editor opens it.
	json = session_view_to_json(view);
	bool menu_editable = false, some_not_editable = false;
	TEST_EXPECT(json.get("project")->get("files")->array.size() == view.scan.entries.size());
	for (const JsonValue &file : json.get("project")->get("files")->array) {
		if (file.get_string("name", "") == "main.mnu" && file.get_string("kind", "") == "menu")
			menu_editable = file.get_bool("editable", false);
		some_not_editable = some_not_editable || !file.get_bool("editable", true);
	}
	TEST_EXPECT(menu_editable && some_not_editable);
	TEST_EXPECT(request_error("{\"kind\":\"open_document\",\"path\":\"main.mnu\"}", request).empty() && session.handle(request));
	const Document *document = session.document_for();
	TEST_EXPECT(document != nullptr);
	json = session_view_to_json(view);
	TEST_EXPECT(json.get("documents")->array.size() == 1);
	TEST_EXPECT(json.get("documents")->array.front().get_string("kind", "") == "menu");
	TEST_EXPECT(!json.get("documents")->array.front().get_bool("dirty", true));
	TEST_EXPECT(json.get("documents")->array.front().get("rows") == nullptr);
	TEST_EXPECT(json.get_string("active_document", "") == document->path());

	JsonValue doc = document_to_json(*document, true);
	TEST_EXPECT(doc.get_string("path", "") == document->path() && doc.get_int("row_count", 0) == 1);
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
	TEST_EXPECT(json.get("graph")->get_int("edges", 0) > 0 && json.get("graph")->get_int("symbols", 0) > 0);
	// What the graph's last update did (S13 D3): its stats' counts, as the stats hold them.
	const GraphStats &stats = view.graph->stats();
	const JsonValue graph_json = *session_view_to_json(view).get("graph");
	TEST_EXPECT(graph_json.get_int("edges", 0) == int64_t(view.graph->edge_count()) &&
	            graph_json.get_int("missing", -1) == int64_t(view.graph->missing_count()) &&
	            graph_json.get_int("files_patched", -1) == int64_t(stats.files_patched) &&
	            graph_json.get_int("edges_resolved", -1) == int64_t(stats.edges_resolved) &&
	            graph_json.get_int("findings_made", -1) == int64_t(stats.findings_made));
	TEST_EXPECT(!graph_edges_to_json(*view.graph, view.graph->references_of("main.mnu")).array.empty());
	TEST_EXPECT(graph_edges_to_json(*view.graph, view.graph->references_of("main.mnu")).array.front().get_string("status", "") == "present");
	TEST_EXPECT(record_to_json(*document, NodeAddress{}, view).is_null());
	TEST_EXPECT(record_to_json(*document, NodeAddress{99999, 1, 0}, view).is_null());
	// The root's font as its picker and its Go to see it (S12 Z2): the project's fonts and the
	// stylesheet's variables, each as the font set to it resolves; the variable where the game
	// reads it, then the .fnt its value names.
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
	const JsonValue targets = reference_targets_to_json(*document, main_address, "font.name", view);
	const JsonValue *places = targets.get("targets");
	TEST_EXPECT(places && places->array.size() == 2 && targets.get_int("count", 0) == 2 &&
	            font && targets.get_string("value", "") == font->get_string("value", ""));
	TEST_EXPECT(places && places->array.size() == 2 && places->array[0].get_bool("editable", false) &&
	            !places->array[0].get_string("locator", "").empty() && !places->array[1].get_bool("editable", true) &&
	            places->array[1].get("locator") == nullptr);
	TEST_EXPECT(reference_choices_to_json(*document, main_address, "name", view).get_int("count", -1) == 0);
	TEST_EXPECT(reference_targets_to_json(*document, main_address, "no_such_field", view).is_null());
	TEST_EXPECT(reference_choices_to_json(*document, NodeAddress{99999, 1, 0}, "font.name", view).is_null());

	// A request that names a record and one of its fields (a Problems row's): the view says
	// which field to show until the selection moves. Each such ask moves the serial, the same
	// ask again too (a second click on the row); one naming no field does not.
	const uint64_t serial = view.reveal_serial;
	const std::string reveal = "{\"kind\":\"open_document\",\"path\":\"main.mnu\",\"edit\":{\"row\":" +
	                           std::to_string(title_address.row) + ",\"kind\":" + std::to_string(title_address.kind) +
	                           ",\"child\":" + std::to_string(title_address.child) + ",\"field\":\"string.value\"}}";
	TEST_EXPECT(request_error(reveal.c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(view.selection == title_address && session_view_to_json(view).get_string("reveal_field", "") == "string.value");
	TEST_EXPECT(view.reveal_serial == serial + 1);
	TEST_EXPECT(session_view_to_json(view).get_number("reveal_serial", -1.0) == double(serial + 1));
	TEST_EXPECT(request_error(reveal.c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(view.reveal_field == "string.value" && view.reveal_serial == serial + 2);
	// The same field asked again: the JSON's serial moves with it, so a client sees the second ask.
	TEST_EXPECT(session_view_to_json(view).get_number("reveal_serial", -1.0) == double(serial + 2));
	const std::string named = "{\"kind\":\"open_document\",\"path\":\"main.mnu\",\"edit\":{\"row\":" +
	                          std::to_string(title_address.row) + ",\"kind\":" + std::to_string(title_address.kind) +
	                          ",\"child\":" + std::to_string(title_address.child) + "}}";
	TEST_EXPECT(request_error(named.c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(view.reveal_field.empty() && view.reveal_serial == serial + 2);
	TEST_EXPECT(request_error(reveal.c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(view.reveal_serial == serial + 3);
	// Files asked to show a file and ask its new name (a Problems row, a Rename... fix): the view
	// carries the file, the ask's serial and the rename, and the request writes back as read.
	TEST_EXPECT(request_error("{\"kind\":\"show_in_files\",\"path\":\"main.mnu\",\"flag\":true}", request).empty() &&
	            session.handle(request));
	const JsonValue shown = session_view_to_json(view);
	const JsonValue *reveal_file = shown.get("reveal_file");
	TEST_EXPECT(reveal_file && reveal_file->get_string("path", "") == document->path() && reveal_file->get_bool("rename", false) &&
	            reveal_file->get_int("serial", 0) == int64_t(view.reveal_file_serial) && view.reveal_file_serial > 0);
	TEST_EXPECT(editor_request_to_json(request).get_string("kind", "") == "show_in_files" &&
	            editor_request_to_json(request).get_bool("flag", false));

	// The same edit through JSON as through the typed request.
	const std::string edit = "{\"kind\":\"edit_record\",\"edit\":{\"row\":" + std::to_string(title_address.row) +
	                         ",\"kind\":" + std::to_string(title_address.kind) + ",\"child\":" +
	                         std::to_string(title_address.child) + ",\"field\":\"string.value\",\"value\":\"John Smith's Game\"}}";
	TEST_EXPECT(request_error(edit.c_str(), request).empty() && session.handle(request));
	Value value;
	TEST_EXPECT(document->get(title_address, "string.value", value) && std::get<std::string>(value) == "John Smith's Game");
	TEST_EXPECT(document->dirty());
	json = session_view_to_json(view);
	TEST_EXPECT(json.get("documents")->array.front().get_bool("dirty", false));
	TEST_EXPECT(json.get("documents")->array.front().get_bool("can_undo", false));
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
		       "\",\"edit\":{\"row\":" + std::to_string(address.row) + ",\"kind\":" + std::to_string(address.kind) +
		       ",\"child\":" + std::to_string(address.child) + "}}";
	};
	TEST_EXPECT(request_error(select_json(title_address, "replace").c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(request_error(select_json(exit_address, "add").c_str(), request).empty() && session.handle(request));
	json = session_view_to_json(view);
	TEST_EXPECT(json.get_string("active_document", "") == document->path() && json.get("reveal_field") == nullptr);
	TEST_EXPECT(json.get("selected") && json.get("selected")->array.size() == 2 &&
	            uint64_t(json.get("selection")->get_number("child", 0)) == exit_address.child);
	TEST_EXPECT(json.get_int("clipboard_bytes", -1) == 0);
	const std::string clear = "{\"kind\":\"edit_record\",\"edit\":{\"operation\":\"clear\",\"row\":" +
	                          std::to_string(title_address.row) + ",\"kind\":" + std::to_string(title_address.kind) +
	                          ",\"child\":" + std::to_string(title_address.child) + ",\"field\":\"position.left\"}}";
	TEST_EXPECT(request_error(clear.c_str(), request).empty() && session.handle(request) && session.last_edit_ok());
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
	doc = document_to_json(*document, true);
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
	// step; again, nothing is left to revert and the request is refused.
	const std::string revert = "{\"kind\":\"revert_to_saved\",\"edit\":{\"row\":" + std::to_string(title_address.row) +
	                           ",\"kind\":" + std::to_string(title_address.kind) + ",\"child\":" +
	                           std::to_string(title_address.child) + ",\"field\":\"position.left\"}}";
	TEST_EXPECT(request_error(revert.c_str(), request).empty() && session.handle(request) && session.last_edit_ok());
	TEST_EXPECT(editor_request_to_json(request).get_string("kind", "") == "revert_to_saved");
	record = record_to_json(*document, title_address, view);
	left = find_field(record, "position.left");
	TEST_EXPECT(record.get_string("change", "") == "unchanged" && left->get_bool("present", false) &&
	            left->get("changed") == nullptr);
	TEST_EXPECT(request_error(revert.c_str(), request).empty() && session.handle(request));
	TEST_EXPECT(!session.outcome().done() && !session.last_edit_ok());
	TEST_EXPECT(request_error("{\"kind\":\"undo\"}", request).empty() && session.handle(request) && document->dirty());
	TEST_EXPECT(request_error("{\"kind\":\"undo\"}", request).empty() && session.handle(request) && !document->dirty());

	// A request's outcome: done with no findings, refused with them, or waiting on the
	// unsaved-changes prompt.
	JsonValue outcome = action_outcome_to_json(session.outcome());
	TEST_EXPECT(outcome.get_bool("done", false) && !outcome.get_bool("unsaved_prompt", true));
	TEST_EXPECT(outcome.get("findings")->is_array() && outcome.get("findings")->array.empty());
	TEST_EXPECT(request_error("{\"kind\":\"rename_asset\",\"path\":\"main.mnu\",\"text\":\"../x.mnu\"}", request).empty() &&
	            session.handle(request));
	outcome = action_outcome_to_json(session.outcome());
	TEST_EXPECT(!outcome.get_bool("done", true) && !outcome.get_bool("unsaved_prompt", true));
	TEST_EXPECT(outcome.get("findings")->array.size() == 1);
	if (outcome.get("findings")->array.size() == 1) {
		const JsonValue &finding = outcome.get("findings")->array.front();
		TEST_EXPECT(finding.get_string("code", "") == "rename.name" && finding.get_string("severity", "") == "error");
	}
	TEST_EXPECT(request_error(edit.c_str(), request).empty() && session.handle(request) && document->dirty());
	json = session_view_to_json(view);
	TEST_EXPECT(!json.get("unsaved_prompt")->get_bool("open", true) && json.get("unsaved_prompt")->get("files") == nullptr);
	TEST_EXPECT(request_error("{\"kind\":\"close_document\"}", request).empty() && session.handle(request));
	outcome = action_outcome_to_json(session.outcome());
	TEST_EXPECT(!outcome.get_bool("done", true) && outcome.get_bool("unsaved_prompt", false));
	TEST_EXPECT(outcome.get("findings")->array.empty());
	// The view names what waits, the file it lists and whether Discard is offered.
	json = session_view_to_json(view);
	const JsonValue *prompt = json.get("unsaved_prompt");
	TEST_EXPECT(prompt && prompt->get_bool("open", false) && prompt->get_string("action", "") == "close_document" &&
	            prompt->get_string("target", "") == document->path() && prompt->get_bool("can_discard", false));
	TEST_EXPECT(prompt && prompt->get("files") && prompt->get("files")->array.size() == 1 &&
	            prompt->get("files")->array.front().string == document->path());
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"unsaved_choice\":\"cancel\"}", request).empty() &&
	            session.handle(request));
	TEST_EXPECT(action_outcome_to_json(session.outcome()).get_bool("done", false) && session.document_for() == document);
	// Build packs the files on disk: no Discard; the prompt's "save" writes the file and builds.
	TEST_EXPECT(request_error("{\"kind\":\"build\"}", request).empty() && session.handle(request));
	json = session_view_to_json(view);
	prompt = json.get("unsaved_prompt");
	TEST_EXPECT(prompt && prompt->get_string("action", "") == "build" && !prompt->get_bool("can_discard", true) &&
	            prompt->get("target") == nullptr);
	TEST_EXPECT(request_error("{\"kind\":\"resolve_unsaved\",\"unsaved_choice\":\"save\"}", request).empty() &&
	            session.handle(request));
	TEST_EXPECT(action_outcome_to_json(session.outcome()).get_bool("done", false) && !document->dirty() &&
	            session.view().operation.running());
	// The build runs as an operation: the answer names it, and the view's operation block shows
	// it stepping (its kind, its progress in bytes, what it reads and writes) until it lands.
	const double operation = action_outcome_to_json(session.outcome()).get_number("operation", 0.0);
	TEST_EXPECT(operation > 0.0 && operation == double(view.operation.id));
	json = session_view_to_json(view);
	const JsonValue *running = json.get("operation");
	TEST_EXPECT(running && running->get_bool("running", false) && running->get_number("id", 0.0) == operation &&
	            running->get_string("kind", "") == "build" && running->get_string("unit", "") == "bytes" &&
	            running->get_bool("cancellable", false) && running->get_number("total", 0.0) > 0.0 &&
	            running->get("reads") && running->get("reads")->array.size() == 1 &&
	            running->get("reads")->array.front().string == "files" && running->get("writes") &&
	            running->get("writes")->array.size() == 1 && running->get("writes")->array.front().string == "slot");
	TEST_EXPECT(json.get("build") && json.get("build")->get("running") == nullptr);
	session.run_operations();
	json = session_view_to_json(view);
	TEST_EXPECT(!json.get("operation")->get_bool("running", true) && json.get("operation")->get("id") == nullptr);
	const JsonValue *last = json.get("last_operation");
	TEST_EXPECT(last && last->get_number("id", 0.0) == operation && last->get_string("kind", "") == "build" &&
	            last->get_string("end", "") == "done" && last->get("findings") && last->get("findings")->array.empty());
	TEST_EXPECT(json.get("build")->get_bool("has_build", false) && json.get("build")->get_bool("ok", false));
	TEST_EXPECT(!session_view_to_json(view).get("unsaved_prompt")->get_bool("open", true));
	TEST_EXPECT(request_error("{\"kind\":\"undo\"}", request).empty() && session.handle(request) && document->dirty());

	// Output paging by absolute index: the lines held from `first` to `next`, a window yields that
	// window, a cursor past the lines none (it waits at `next` for lines to come).
	json = session_view_to_json(view);
	const uint64_t first = uint64_t(json.get("output")->get_number("first", -1.0));
	const uint64_t next = uint64_t(json.get("output")->get_number("next", 0.0));
	TEST_EXPECT(first == view.output.first_index() && next == view.output.next_index() && next - first == view.output.size() &&
	            view.output.size() > 1);
	SessionJsonOptions options;
	options.output_cursor = first + 1;
	options.output_limit = 1;
	json = session_view_to_json(view, options);
	TEST_EXPECT(json.get("output")->get("lines")->array.size() == 1 &&
	            uint64_t(json.get("output")->get_number("next_cursor", 0.0)) == first + 2);
	TEST_EXPECT(json.get("output")->get("lines")->array.front().string == view.output[1]);
	options.output_cursor = next + 5;
	json = session_view_to_json(view, options);
	TEST_EXPECT(json.get("output")->get("lines")->array.empty() && uint64_t(json.get("output")->get_number("cursor", 0.0)) == next);

	// The written text is strict JSON that reads back.
	JsonValue reread;
	TEST_EXPECT(parse(opennova::io::json_write(session_view_to_json(view)).c_str(), reread) && reread.is_object());
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
			// Open, with nothing known on this record (a model with no registers): its own
			// choices, none of them (record_choices).
			FieldSchema reg;
			reg.id = "param";
			reg.open_choices = true;
			// The same field where the record names no register: a number, no list to open.
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
	std::shared_ptr<Node> make_node(NodeKind, NodeId, std::string &error) override {
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
// read back as written; the view's import block carries the preview: what it lists to
// choose from and the files chosen, as a request's imports take them; the plan's importable
// rows (the chosen file, a dependency with what wanted it, where it was found, where else,
// and a row the project cannot take) apart from the rows not found; the kinds not followed,
// the cap and the plan's findings; the editor's setting. A row's source passes back to
// import_files as it is.
static int test_import_plan_json() {
	TEST_EXPECT(std::string(editor_request_kind_token(EditorRequestKind::PlanImport)) == "plan_import");
	TEST_EXPECT(std::string(editor_request_kind_token(EditorRequestKind::SetImportDependencies)) == "set_import_dependencies");
	JsonValue parsed;
	EditorRequest back;
	std::string error;
	EditorRequest plan = make_request(EditorRequestKind::PlanImport);
	plan.flag = true;
	plan.imports = {{"C:/mod/menus.pff", "a.mnu"}};
	TEST_EXPECT(parse(opennova::io::json_write(editor_request_to_json(plan)).c_str(), parsed));
	TEST_EXPECT(editor_request_from_json(parsed, back, error) && back.kind == EditorRequestKind::PlanImport && back.flag &&
	            back.imports == plan.imports);
	TEST_EXPECT(request_error("{\"kind\":\"set_import_dependencies\"}", back).empty() &&
	            back.kind == EditorRequestKind::SetImportDependencies && !back.flag);
	TEST_EXPECT(request_error("{\"kind\":\"set_import_dependencies\",\"flag\":true}", back).empty() && back.flag);

	SessionView view;
	view.import_dependencies = false;
	SessionView::ImportPreview &preview = view.import_preview;
	preview.open = true;
	preview.with_dependencies = true;
	preview.changed = true;
	preview.serial = 7;
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
	preview.plan.rows = {chosen, font, gone, cut};
	preview.plan.not_followed = {{ReferenceKind::MenuScreen, AssetKind::Unknown, 2, "a.mnu"},
	                             {ReferenceKind::None, AssetKind::Terrain, 1, "level.trn"}};
	preview.plan.truncated = true;
	preview.plan.diagnostics = {make_diagnostic(DiagnosticSeverity::Warning, "import.unreadable", "The file could not be read.", "b.mnu")};
	const JsonValue json = session_view_to_json(view);
	const JsonValue *import = json.get("import");
	TEST_EXPECT(import != nullptr);
	if (!import) return 1;
	TEST_EXPECT(import->get_bool("open", false) && import->get_bool("with_dependencies", false) && import->get_bool("changed", false));
	TEST_EXPECT(!import->get_bool("import_dependencies", true) && import->get_int("serial", 0) == 7);
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
	            source->get("entry") == nullptr && source->get("retail") == nullptr);
	const JsonValue *rivals = found.get("rivals");
	TEST_EXPECT(rivals && rivals->array.size() == 1 && rivals->array[0].get_string("found_in", "") == "the game install" &&
	            rivals->array[0].get_bool("differs", false) && rivals->array[0].get("source") &&
	            rivals->array[0].get("source")->get_bool("retail", false));
	TEST_EXPECT(!rows->array[2].get_bool("selected", true) &&
	            rows->array[2].get_string("problem", "").find("16 characters") != std::string::npos);
	const JsonValue *missing = import->get("not_found");
	TEST_EXPECT(missing && missing->array.size() == 1 && missing->array[0].get_string("state", "") == "not_found" &&
	            missing->array[0].get_string("name", "") == "gone.tga" && missing->array[0].get("source") == nullptr &&
	            missing->array[0].get("needed_by") && missing->array[0].get("needed_by")->get_string("reference", "") == "menu_texture");
	const JsonValue *skipped = import->get("not_followed");
	TEST_EXPECT(skipped && skipped->array.size() == 2 && skipped->array[0].get_string("reference", "") == "menu_screen" &&
	            skipped->array[0].get_int("count", 0) == 2 && skipped->array[1].get_string("kind", "") == "terrain" &&
	            skipped->array[1].get("reference") == nullptr && skipped->array[1].get_string("first", "") == "level.trn");
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

// The import block's lists page (import_offset / import_limit, 200 by default, as the editor
// MCP's transport caps a list at 200): a plan of 250 rows, 3 not found, with 250 files to
// choose from and 250 chosen, shows 200 of each list and their counts; the next page the rest;
// a row on a later page still carries the source an import takes.
static int test_import_pages() {
	SessionView view;
	SessionView::ImportPreview &preview = view.import_preview;
	preview.open = true;
	for (int i = 0; i < 250; ++i) {
		const ImportSource source{"C:/art/f" + std::to_string(i) + ".txt", "", false, false};
		preview.choices.push_back(source);
		preview.roots.push_back(source);
		ImportPlanRow row;
		row.state = ImportPlanRow::State::Selected;
		row.selected = true;
		row.source = source;
		row.name = "f" + std::to_string(i) + ".txt";
		row.kind = AssetKind::Text;
		preview.plan.rows.push_back(row);
	}
	for (int i = 0; i < 3; ++i) {
		ImportPlanRow gone;
		gone.state = ImportPlanRow::State::NotFound;
		gone.name = "gone" + std::to_string(i) + ".tga";
		gone.kind = AssetKind::Texture;
		preview.plan.rows.insert(preview.plan.rows.begin() + 100 * i, gone);
	}
	const JsonValue first = session_view_to_json(view);
	const JsonValue *import = first.get("import");
	TEST_EXPECT(import != nullptr);
	if (!import) return 1;
	TEST_EXPECT(import->get_int("offset", -1) == 0 && import->get_int("row_count", 0) == 250 &&
	            import->get_int("not_found_count", 0) == 3 && import->get_int("choice_count", 0) == 250 &&
	            import->get_int("root_count", 0) == 250);
	TEST_EXPECT(import->get("rows") && import->get("rows")->array.size() == 200 && import->get("choices")->array.size() == 200 &&
	            import->get("roots")->array.size() == 200 && import->get("not_found")->array.size() == 3);
	SessionJsonOptions next;
	next.import_offset = 200;
	const JsonValue second = session_view_to_json(view, next);
	import = second.get("import");
	TEST_EXPECT(import != nullptr);
	if (!import) return 1;
	const JsonValue *rows = import->get("rows");
	TEST_EXPECT(import->get_int("offset", -1) == 200 && rows && rows->array.size() == 50 &&
	            import->get("choices")->array.size() == 50 && import->get("not_found")->array.empty());
	if (rows && !rows->array.empty()) {
		const JsonValue *source = rows->array.back().get("source");
		TEST_EXPECT(rows->array.back().get_string("name", "") == "f249.txt" && source &&
		            source->get_string("path", "") == "C:/art/f249.txt");
	}
	next.import_offset = 0;
	next.import_limit = 10;
	TEST_EXPECT(session_view_to_json(view, next).get("import")->get("rows")->array.size() == 10);
	return 0;
}

// S13 D6: an Apply edit's change is made in C++ by its document type (Edit::payload). A request's
// JSON names it by the payload's token alone, and the reader takes neither an apply edit nor a
// payload: the editor MCP cannot send one.
static int test_apply_edit_json() {
	struct Brush : EditPayload {
		const char *token() const override { return "raster.brush"; }
	};
	EditorRequest request;
	request.kind = EditorRequestKind::EditRecord;
	request.path = "terrain.cpt";
	request.edit.operation = EditOperation::Apply;
	request.edit.address = {3, 0, 0};
	request.edit.payload = std::make_shared<Brush>();
	const JsonValue json = editor_request_to_json(request);
	const JsonValue *edit = json.get("edit");
	TEST_EXPECT(edit && edit->get_string("operation", "") == "apply" &&
	            edit->get_string("payload", "") == "raster.brush");
	EditorRequest back;
	std::string error;
	TEST_EXPECT(!editor_request_from_json(json, back, error) &&
	            error.find("apply edit") != std::string::npos);
	TEST_EXPECT(!request_error("{\"kind\":\"edit_record\",\"edit\":{\"operation\":\"apply\"}}",
	                           back).empty());
	TEST_EXPECT(!request_error("{\"kind\":\"edit_record\","
	                           "\"edit\":{\"field\":\"name\",\"payload\":\"raster.brush\"}}",
	                           back).empty());
	// No payload on any other edit's JSON.
	request.edit = Edit();
	const JsonValue set = editor_request_to_json(request);
	TEST_EXPECT(set.get("edit") && !set.get("edit")->get("payload"));
	TEST_EXPECT(editor_request_from_json(set, back, error) && !back.edit.payload);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_apply_edit_json();
	failures += test_tokens();
	failures += test_asset_kind_tokens();
	failures += test_import_pages();
	failures += test_request_round_trip();
	failures += test_settings_json();
	failures += test_problem_query_json();
	failures += test_problem_groups_page();
	failures += test_over_a_session();
	failures += test_graph_style_fields();
	failures += test_search_json();
	failures += test_field_metadata();
	failures += test_import_plan_json();
	if (failures == 0) std::printf("editor_session_json: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
