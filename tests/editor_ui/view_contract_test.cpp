// S13 V3 (ADR 0046 S13, "Adding a document type"): every document type's view (ui/document_views)
// holds to one contract over a file of its type, drawn alone on the null ImGui backend. Each
// DocumentTypeId past None has its row, and a file here (a type with none fails); the view the row
// makes (make_view) is an outline in the mode its row names (its model's), or a view of its own
// where the row names none, and a view that draws its records has no main viewport. With the
// document open and active in a view of the session and its first record selected, the view drawn
// in a window of a Document tab's size, 5 frames at each of two widths (an outline's file-wide
// values opened through its model after the third: an item table's vehicle spawn registry): it
// raises no request, nothing it draws runs past what shows of the window unless it scrolls sideways
// (the bounds sweep's measure: every window's content within its width, every table cell within
// its column), and it draws its document (the title of its first row of its own kind shows, the
// first kind the type declares: a stylesheet's variable, not its comment). A RevealRecord it
// is sent is held until it draws and taken as it does; a MainViewport row's type is one a Main-role
// viewport kind shows (S13 V5). Two open documents of a type get a view each, whose filters are their
// own. A string table's cell being edited keeps the keyboard, and ends its edit, scrolled out of
// sight. S13 D9, V10: every text type's row plays the MainViewport role with a view of its own (the
// script view), whose Main view is the script device (ViewportKind::Script); where no device draws
// (no devices here) it draws the lines (its first line that is not blank shows), and a RevealText
// it is sent marks its line; a text of many thousand lines draws only those in sight, the line a
// reveal names among them. With the workspace's devices, the script view places its device in the
// rest of the tab below its toolbar (the rect it reserves, the picture where its cursor stands),
// raising nothing, and draws the lines instead while a popup lies over the tab. The MainViewport
// role's outline view draws its outline beside the viewport, whose canvas fills the rest of the tab
// through the workspace's device; in the Document window while an operation holds the documents,
// its outline is held back and its canvas is not (a drag orbits its camera). ADR 0046 S14: an outline
// that filters its rows by kind and leaves out the rows its type says hold nothing, its chips, its
// switch, and a Shift or Ctrl click on a line.
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <editor/documents/strings_document.h>
#include <editor/model/text_document.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/session/session_operation.h>
#include <editor/preview/script_viewport.h>
#include <editor/ui/document_views.h>
#include <editor/ui/document_window.h>
#include <editor/ui/main_viewport_view.h>
#include <editor/ui/outline_view.h>
#include <editor/ui/script_view.h>
#include <editor/ui/text_view.h>
#include <formats/rtxt/rtxt.h>
#include <formats/tga/tga.h>
#include "../editor/pool_document.h"
#include "editor_ui_test_support.h"

#include <editor/ui/catalog_inspector_view.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

// A workspace of the test's own: a view it seeds, the requests it is asked, and the devices a test
// hands it (none unless it does).
class TestWorkspace : public Workspace {
public:
	SessionView seeded;
	std::vector<EditorRequest> requests;
	ViewportDeviceSource *source = nullptr;
	const SessionView &view() const override { return seeded; }
	void request(EditorRequest request) override { requests.push_back(std::move(request)); }
	ViewportDeviceSource *devices() const override { return source; }
};

struct Fixture {
	AssetKind kind;
	std::string name;
	std::vector<uint8_t> bytes;
};

std::vector<uint8_t> text_bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// A 4 x 4 TGA of graded alpha, as the writer makes it.
std::vector<uint8_t> minted_tga() {
	std::vector<uint8_t> rgba;
	for (int i = 0; i < 64; ++i) rgba.push_back(uint8_t(i * 4));
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(rgba.data(), 4, 4, out, error);
	return out;
}

// A file of each type's kinds: the repo's fixtures, and the three catalogs written here (an item
// table with a vehicle spawn registry, a weapon table with an action and a carry limit, an ammo
// table with an effect).
std::vector<Fixture> fixtures(const std::string &repo) {
	const auto file = [&](const char *relative) { return test_io::read_file(repo + "/fixtures/" + relative); };
	return {
	        {AssetKind::ItemDefs, "items.def",
	         text_bytes("begin \"Drivable Dune Buggy With A Name Long Enough To Be Cut In A Narrow Tab\"\nid 101291\n"
	                    "type vehicle\ngraphic Dbuggy1\npcvehicle_spawnlist 8 12\nend\n"
	                    "begin \"Marker Alpha\"\nid 100001\ntype marker\nend\n")},
	        {AssetKind::WeaponDefs, "weapon.def",
	         text_bytes("weapon \"WPN_CONTRACT\"\ncategory 11\nclipsize 30\naction \"FIRE\"\ndelayend 2\nend\nend\n"
	                    "ammoclass_max_carry CLASS_AK47 300\n")},
	        {AssetKind::AmmoDefs, "ammo.def", text_bytes("ammo AT_CONTRACT\nmax_age 1.5\nvelocity 900\nend\n")},
	        {AssetKind::Strings, "synth_game.bin", file("rtxt/synth_game.bin")},
	        {AssetKind::Menu, "all_widgets.mnu", file("mnu/all_widgets.mnu")},
	        {AssetKind::MenuStyle, "test_style.mns", file("mns/test_style.mns")},
	        {AssetKind::Model, "armory.3di", file("threedi/synth/armory.3di")},
	        {AssetKind::Animation, "walk.bad", file("anim/walk.bad")},
	        {AssetKind::AnimationMap, "soldier.adm", file("anim/soldier.adm")},
	        // A mission (S14): the minted one, its 3D viewport the tab's main view beside its outline.
	        {AssetKind::Mission, "synth_logic.bms", file("bms/synth_logic.bms")},
	        // The text types (S13 D9): a script, a music script, a credits file (whose lines its text
	        // form cannot carry: held read only, drawn all the same), a plain shader and a
	        // configuration.
	        {AssetKind::Script, "text_document.wac", file("wac/text_document.wac")},
	        {AssetKind::MusicScript, "gamemus.bin", file("mus/synth_gamemus.bin")},
	        {AssetKind::Credits, "nlist.kda", file("cbin/synth_nlist.kda")},
	        {AssetKind::Shader, "glass.fx", text_bytes("// glass\r\nfloat4 main() : COLOR { return 0; }\r\n")},
	        {AssetKind::Config, "game.cfg", text_bytes("\r\n[Game]\r\nname = Views\r\n")},
	        // A HUD layout (DI-20): its text in the script view, its HUD the Preview window's.
	        {AssetKind::HudPosDefs, "hudpos.def", text_bytes("// soldier panel\r\nHUDHEALTH 25,741,177,751\r\n")},
	        // The character attributes (DI-09's charattr follow-up): its text in the script view.
	        {AssetKind::CharAttrDefs, "charattr.def", text_bytes("// classes\r\n[CHARACTER1]\r\nJUNGLE_CAMMO = 5310\r\n")},
	        // Round S23 lane A: a font beside its picture, a music bank's streams, a wave's picture and edits.
	        {AssetKind::Font, "synth.fnt", file("fnt/synth_1page.fnt")},
	        {AssetKind::MusicBank, "synth.sbf", file("sbf/synth_gamemus.sbf")},
	        {AssetKind::Wave, "tone.wav", file("lwf/tone.wav")},
	        // A face animation (round S23 lane A): its face, vertices, triangles and gestures a tree.
	        {AssetKind::FaceAnimation, "person.grm", file("grm/person.grm")},
	        // A texture (S18): a TGA our writer mints, its picture the tab's main view beside its facts.
	        {AssetKind::Texture, "brick.tga", minted_tga()},
	        // The sound lane: the minted bank, its sets and waves a tree with a Play heading the Inspector, and a
	        // SndProf.def, its profile's slots under it.
	        {AssetKind::SoundBank, "menu.lwf", file("lwf/menu.lwf")},
	        {AssetKind::SoundProfileDefs, "SndProf.def",
	         text_bytes("begin \"default\"\r\n\tSSLFootGND FSP_DIRT_L 0 0 0\r\nend\r\n")},
	        // A particle file (DI-14): its text in the script view, its effect the Preview window's.
	        {AssetKind::Particles, "minimal_effect.ptl", file("particle/synth_minimal_effect.ptl")},
	        // The environment (DI-19a): its row and its ten keyframes as a tree, the missions that run on it
	        // heading the Inspector.
	        {AssetKind::Environment, "synth_full.env", file("env/synth_full.env")},
	        // The terrain (DI-30): its row, its grid rows and its foliage definitions as a tree, the import that
	        // makes it and the missions that run on it heading the Inspector.
	        {AssetKind::Terrain, "Tmap.trn", file("terrain/tmap/Tmap.trn")},
	        // The dialog bank (DI-32): its dialogs and their lines as a tree, a dialog's Play heading the Inspector.
	        {AssetKind::DialogBank, "synth_bank.dbf", file("dbf/synth_bank.dbf")},
	};
}

// The view of an open document as the session shows it: the project, the file in its scan, the
// document open and active, its first row selected.
void seed(SessionView &view, const std::shared_ptr<const DocumentBase> &document) {
	view.project.open = true;
	view.project.root = "C:/mods/Views";
	editor_test::own(view.project.document).title = "Views";
	editor_test::own(view.project.scan).entries = {file_entry(document->path(), document->path(), document->kind())};
	editor_test::own(view.project.scan).index();
	view.documents.open = {document};
	view.documents.active = document->path();
	if (const Document *records = records_of(*document); records && !records->rows().empty()) {
		const NodeAddress first{records->rows()[0]->id, records->rows()[0]->kind, 0};
		view.documents.selection.select_only(document->path(), first);
	}
	for (size_t concern = 0; concern < kViewConcernCount; ++concern)
		view.revisions.touch(static_cast<ViewConcern>(concern));
}

// A frame of the view drawn alone in a window `width` pixels wide and 560 high, a Document tab's
// room, its modals after it as the workspace draws them; what it wrote as text when `log`.
std::string view_frame(TestWorkspace &workspace, DocumentView &view, const DocumentBase &document, float width,
                       bool log = false) {
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(width, 560.0f));
	ImGui::Begin("Tab", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
	if (log) ImGui::LogToBuffer();
	view.draw(workspace, document);
	std::string text;
	if (log) {
		text = GImGui->LogBuffer.c_str();
		ImGui::LogFinish();
	}
	ImGui::End();
	view.draw_modals(workspace);
	ImGui::Render();
	return text;
}

void draw_frames(TestWorkspace &workspace, DocumentView &view, const DocumentBase &document, float width,
                 int count) {
	for (int i = 0; i < count; ++i) view_frame(workspace, view, document, width);
}

// The title of the document's first row of its first kind (the type's own records: a catalog's
// items, a stylesheet's variables rather than its comments): what its view shows first; a text's
// first line that is not blank.
std::string first_title(const DocumentBase &document) {
	// A file held whole (S18 a texture, S23 a wave): its file's name, which heads its facts.
	if (document.holds_bytes()) return document.path().substr(document.path().find_last_of('/') + 1);
	if (const TextDocument *text = text_of(document)) {
		for (size_t line = 1; line <= text->line_count(); ++line)
			if (text->line(line).find_first_not_of(" \t") != std::string_view::npos) return std::string(text->line(line));
		return std::string();
	}
	const Document *records = records_of(document);
	if (!records || records->kinds().empty()) return std::string();
	const NodeKind own = records->kinds().front().kind;
	for (const auto &row : records->rows())
		if (row && row->kind == own) return records->record_title({row->id, row->kind, 0});
	return std::string();
}

void test_every_view() {
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::vector<Fixture> files = fixtures(repo);
	size_t types = 0, views = 0, frames = 0, main_rows = 0, scripts = 0;
	for (size_t id = 1; id <= kDocumentTypeCount; ++id) {
		const DocumentTypeId type_id = static_cast<DocumentTypeId>(id);
		const DocumentType *type = document_type(type_id);
		const DocumentViewRow *row = document_view_row(type_id);
		CHECK(type && row && row->type == type_id && (row->outline || row->make),
		      "every document type has its view's row, an outline or a make");
		if (!type || !row) continue;
		// A MainViewport row's type is one a Main-role viewport kind shows, the tab's viewport, with
		// the outline beside it or in a view of its own (a text's script view).
		if (row->role == DocumentViewRole::MainViewport) {
			const ViewportKind main = main_viewport_kind(type_id);
			CHECK(main != ViewportKind::kCount && viewport_kind_row(main).role == ViewportRole::Main,
			      (std::string(type->name) + ": its MainViewport row's Main-role kind").c_str());
			++main_rows;
		}
		size_t drawn = 0;
		for (const Fixture &fixture : files) {
			if (asset_kind_row(fixture.kind).document != type_id) continue;
			++drawn;
			const std::string where = std::string(type->name) + " over " + fixture.name;
			std::shared_ptr<DocumentBase> loaded = type->make();
			Diagnostic error;
			CHECK(loaded && loaded->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error),
			      (where + ": the file loads").c_str());
			if (!loaded) continue;
			const std::shared_ptr<const DocumentBase> document = loaded;
			std::unique_ptr<DocumentView> view = make_view(*document);
			CHECK(view != nullptr, (where + ": make_view makes its view").c_str());
			if (!view) continue;
			// An outline in its row's mode, or a view of its own where the row names no outline.
			OutlineModel *outline = view->outline();
			CHECK(row->outline ? outline && outline->mode() == row->outline->mode : outline == nullptr,
			      (where + ": the view is the outline its row names, in its mode, or its own").c_str());
			NullBackend backend;
			TestWorkspace workspace;
			seed(workspace.seeded, document);
			++views;
			for (const float width : {520.0f, 320.0f}) {
				const std::string at = where + " at " + std::to_string(int(width));
				draw_frames(workspace, *view, *document, width, 3);
				if (outline) outline->set_values_open(true);
				draw_frames(workspace, *view, *document, width, 2);
				frames += 5;
				CHECK(workspace.requests.empty(), (at + ": drawing raises no request").c_str());
				for (const std::string &offender : overflowing()) CHECK(false, (at + ": " + offender).c_str());
			}
			// It draws its document: the title of its first row of its own kind shows.
			const std::string title = first_title(*document);
			const std::string shown = view_frame(workspace, *view, *document, 520.0f, true);
			CHECK(!title.empty() && shown.find(title.substr(0, 8)) != std::string::npos,
			      (where + ": the view draws its document (" + title + ")").c_str());
			// A view that draws its records has no main viewport, nor does a text's where no device draws
			// it (the workspace here has none); the frame it is asked in draws nothing. A row of an
			// outline beside a Main-role viewport (the mission's, S14) draws its viewport's view in that
			// frame: with no viewport kept for the document, its kind's message, raising nothing.
			// An image's view (S18, a texture's) draws its viewport's view beside its facts alike.
			const bool main_beside_outline =
					row->role == DocumentViewRole::MainViewport && (row->outline || document->holds_bytes());
			ImGui::NewFrame();
			CHECK(view->main_viewport(workspace, *document) == main_beside_outline,
			      (where + (main_beside_outline ? ": the main viewport drawn beside the outline" : ": no main viewport drawn")).c_str());
			ImGui::Render();
			CHECK(workspace.requests.empty(), (where + ": the main viewport raises nothing").c_str());
			// A RevealRecord held until the view draws, taken as it draws.
			ViewEvent reveal;
			reveal.kind = ViewEventKind::RevealRecord;
			reveal.path = document->path();
			reveal.address = workspace.seeded.documents.selection.primary;
			view->receive(reveal);
			CHECK(view->held_events() == 1, (where + ": a RevealRecord held").c_str());
			draw_frames(workspace, *view, *document, 520.0f, 1);
			CHECK(view->held_events() == 0 && workspace.requests.empty(),
			      (where + ": taken as the view draws, nothing raised").c_str());
			// A text's view: the script view, its lines drawn where no device draws, a RevealText marking
			// its line as it is taken.
			if (text_of(*document)) {
				auto *script = dynamic_cast<ScriptView *>(view.get());
				CHECK(script != nullptr && row->role == DocumentViewRole::MainViewport && row->make,
				      (where + ": a text type's view is the script view").c_str());
				ViewEvent go;
				go.kind = ViewEventKind::RevealText;
				go.path = document->path();
				go.locator = "2:1";
				view->receive(go);
				draw_frames(workspace, *view, *document, 520.0f, 1);
				CHECK(script && script->lines().marked_line() == 2 && view->held_events() == 0 && !script->device_drawn(),
				      (where + ": a RevealText marks its line").c_str());
				scripts += script ? 1 : 0;
			}
		}
		CHECK(drawn > 0, (std::string(type->name) + ": the contract has a file of its type").c_str());
		types += drawn > 0 ? 1 : 0;
	}
	CHECK(types == kDocumentTypeCount, "every document type's view drawn");
	CHECK(main_rows == 13 && scripts == 8,
	      "every text type's row the Main role's (a particle file's, the HUD layout's and the character attributes' among "
	      "them), its view the "
	      "script view, and the mission's, the texture's, the environment's, the terrain's and the font's rows the Main role's "
	      "too");
	std::printf("%zu document types, %zu views over their files, %zu frames drawn, %zu script views\n", types, views,
	            frames, scripts);
}

// Two open catalogs in the Document window: a view each, whose filter (its model's) is its own (the
// one view a type shared before S13 V3 filtered every open catalog by the text typed into one).
void test_a_view_per_document() {
	editor_test::TempProjectDir dir("opennova_editor_view_per_document");
	const auto load_catalog = [&](const char *file, const char *relative, const char *text) {
		auto document = std::make_shared<DefCatalogDocument>();
		Diagnostic error;
		CHECK(editor_test::write_text(dir.file(file), text), "catalog fixture");
		CHECK(document->load(dir.file(file), relative, AssetKind::ItemDefs, "jo", error), error.message.c_str());
		return document;
	};
	const auto a = load_catalog("a.def", "a/items.def", "begin \"Alpha\"\nid 100001\ntype marker\nend\n");
	const auto b = load_catalog("b.def", "b/items.def", "begin \"Bravo\"\nid 100002\ntype marker\nend\n");
	SessionView v;
	v.project.open = true;
	v.project.root = dir.root();
	editor_test::own(v.project.document).title = "Views";
	editor_test::own(v.project.scan).entries = {file_entry("items.def", a->path(), AssetKind::ItemDefs),
	                                          file_entry("items.def", b->path(), AssetKind::ItemDefs)};
	editor_test::own(v.project.scan).index();
	v.documents.open = {a, b};
	v.documents.active = a->path();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	DocumentWindow *documents = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count() && !documents; ++i)
		documents = dynamic_cast<DocumentWindow *>(&ui.windows.pass().window(i));
	CHECK(documents != nullptr, "the Document window");
	if (!documents) return;
	CHECK(documents->view_of(a->path()) && !documents->view_of(b->path()), "a view for the document drawn");
	OutlineModel *a_outline = documents->view_of(a->path()) ? documents->view_of(a->path())->outline() : nullptr;
	CHECK(a_outline != nullptr, "a's view is an outline");
	if (!a_outline) return;
	// a filtered: it lists none of its records.
	a_outline->set_filter("zzz");
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("No record matches the filter.") != std::string::npos, "a's filter hides its records");
	// b made active: its own view, its own (empty) filter.
	v.documents.active = b->path();
	v.revisions.touch(ViewConcern::ActiveDocument);
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(4);
	ui.away();
	const std::string shown = logged_frame(ui);
	DocumentView *b_view = documents->view_of(b->path());
	CHECK(b_view && b_view != documents->view_of(a->path()) && b_view->outline() && b_view->outline()->filter().empty(),
	      "b gets a view of its own, its filter empty");
	CHECK(shown.find("Bravo") != std::string::npos && shown.find("No record matches the filter.") == std::string::npos,
	      "b's filter is its own: its record listed");
	// Back to a: its filter kept.
	v.documents.active = a->path();
	v.revisions.touch(ViewConcern::ActiveDocument);
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(4);
	ui.away();
	CHECK(a_outline->filter() == "zzz" && logged_frame(ui).find("No record matches the filter.") != std::string::npos,
	      "a keeps its filter");
	// b closed: its view goes with it.
	v.documents.open = {a};
	v.revisions.touch(ViewConcern::DocumentSet);
	ui.frames(2);
	CHECK(documents->view_of(a->path()) && !documents->view_of(b->path()), "a closed document's view goes");
	ui.drain();
}

// A string table's cell being edited, its row scrolled out of sight (the detail table draws only
// the rows in sight): it keeps the keyboard, and Enter ends its edit (an EndEdit for the table),
// where the row's cells not drawn had let the keyboard go with the edit group still open.
void test_edit_scrolled_out() {
	opennova::rtxt::File table;
	table.sections = {{"Menu", 60}};
	for (int i = 0; i < 60; ++i) {
		char key[16];
		std::snprintf(key, sizeof(key), "KEY_%02d", i);
		table.entries.push_back({key, "a text", {}, 0});
	}
	std::vector<uint8_t> bytes;
	std::string io_error;
	CHECK(opennova::rtxt::write(table, bytes, io_error), "the table written");
	auto loaded = std::make_shared<StringsDocument>();
	Diagnostic error;
	CHECK(loaded->load_bytes(bytes, "many.bin", AssetKind::Strings, "jo", error), "the table loads");
	const std::shared_ptr<const DocumentBase> document = loaded;
	std::unique_ptr<DocumentView> view = make_view(*document);
	CHECK(view != nullptr && !loaded->rows().empty(), "its view and its section");
	if (!view || loaded->rows().empty()) return;
	std::vector<NodeId> strings;
	for (const Document::Collection &collection : loaded->collections_of({loaded->rows()[0]->id, loaded->rows()[0]->kind, 0}))
		strings = collection.ids;
	CHECK(strings.size() == 60, "the section's strings");
	if (strings.empty()) return;
	NullBackend backend;
	TestWorkspace workspace;
	seed(workspace.seeded, document); // the section selected: its strings the detail table
	draw_frames(workspace, *view, *document, 520.0f, 3);
	// The first string's key cell given the keyboard, a character typed: its Set.
	const ImGuiID records = item_id(Ui::window_id("Tab"), {"master", "records"});
	const ImGuiID key = item_id(pushed(records, static_cast<int>(strings.front())), {"##key"});
	ImGui::ActivateItemByID(key);
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	draw_frames(workspace, *view, *document, 520.0f, 2);
	CHECK(GImGui->ActiveId == key, "the first key's cell has the keyboard");
	ImGui::GetIO().AddInputCharactersUTF8("x");
	draw_frames(workspace, *view, *document, 520.0f, 2);
	CHECK(!workspace.requests.empty() && workspace.requests.back().kind == EditorRequestKind::EditRecord,
	      "typed: the key's Set");
	// The table scrolled to its end: the first string far out of sight.
	ImGuiTable *details = ImGui::TableFindByID(records);
	CHECK(details && details->InnerWindow && details->InnerWindow->ScrollMax.y > 0.0f, "the detail table scrolls");
	if (!details || !details->InnerWindow) return;
	ImGui::SetScrollY(details->InnerWindow, details->InnerWindow->ScrollMax.y);
	draw_frames(workspace, *view, *document, 520.0f, 3);
	CHECK(details->InnerWindow->Scroll.y > 0.0f && GImGui->ActiveId == key,
	      "scrolled out of sight, the cell keeps the keyboard");
	workspace.requests.clear();
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	draw_frames(workspace, *view, *document, 520.0f, 1);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
	draw_frames(workspace, *view, *document, 520.0f, 1);
	bool ended = false;
	for (const EditorRequest &request : workspace.requests)
		ended = ended || (request.kind == EditorRequestKind::EndEdit && request.path == document->path());
	CHECK(ended && GImGui->ActiveId != key, "Enter ends its edit");
}

// The mission authoring round: a string table's sections. The selected string's Move to section...
// lists the other sections, and a pick raises a Move whose destination is that section (which the type
// makes an add there and a remove here); the sections' Add asks the new section's name first, says
// when the table has one of it (in any case: the one a lookup reads, which Add selects), and raises an
// Add with that name.
void test_strings_sections() {
	opennova::rtxt::File table;
	table.sections = {{"Menu", 2}, {"WepDes", 1}};
	table.entries = {{"MM_Exit", "Exit", {}, 0}, {"MM_Cafe", "Cafe", {}, 0}, {"WPN_ONE", "The first weapon", {}, 1}};
	std::vector<uint8_t> bytes;
	std::string io_error;
	CHECK(opennova::rtxt::write(table, bytes, io_error), "the table written");
	auto loaded = std::make_shared<StringsDocument>();
	Diagnostic error;
	CHECK(loaded->load_bytes(bytes, "table.bin", AssetKind::Strings, "jo", error) && loaded->rows().size() == 2, "the table loads");
	if (loaded->rows().size() != 2) return;
	const std::shared_ptr<const DocumentBase> document = loaded;
	std::unique_ptr<DocumentView> view = make_view(*document);
	CHECK(view != nullptr, "its view");
	if (!view) return;
	const NodeId menu = loaded->rows()[0]->id, wepdes = loaded->rows()[1]->id;
	const NodeAddress cafe{menu, node_kind(StringsKind::String), loaded->rows()[0]->collections[0][1]};
	NullBackend backend;
	TestWorkspace workspace;
	seed(workspace.seeded, document);
	workspace.seeded.documents.selection.select_only(document->path(), cafe);
	draw_frames(workspace, *view, *document, 640.0f, 3);
	CHECK(view_frame(workspace, *view, *document, 640.0f, true).find("Move to section...") != std::string::npos,
	      "the selected string's Move to section...");
	const ImGuiID tab = Ui::window_id("Tab");
	ImGui::ActivateItemByID(item_id(tab, {"master", "details", "Move to section..."}));
	draw_frames(workspace, *view, *document, 640.0f, 2);
	const ImGuiID rows_popup = item_id(tab, {"master", "details", "move to row"});
	char popup_name[32];
	std::snprintf(popup_name, sizeof(popup_name), "##Popup_%08x", rows_popup);
	ImGui::ActivateItemByID(ImHashStr("WepDes", 0, pushed(ImHashStr(popup_name), static_cast<int>(wepdes))));
	draw_frames(workspace, *view, *document, 640.0f, 2);
	bool moved = false;
	for (const EditorRequest &request : workspace.requests)
		moved = moved || (request.kind == EditorRequestKind::EditRecord && request.edits.size() == 1 &&
		                  request.edits[0].operation == EditOperation::Move && request.edits[0].address == cafe &&
		                  request.edits[0].parent == wepdes);
	CHECK(moved, "a pick: a Move of the string whose destination is WepDes");
	// The sections' Add: its name asked, the one the table has said, an Add with the name on Enter.
	workspace.requests.clear();
	ImGui::ActivateItemByID(item_id(tab, {"master", "masters", "Add"}));
	draw_frames(workspace, *view, *document, 640.0f, 3);
	ImGui::GetIO().AddInputCharactersUTF8("wepdes");
	draw_frames(workspace, *view, *document, 640.0f, 2);
	CHECK(view_frame(workspace, *view, *document, 640.0f, true).find("Section WepDes is already in the file: Add selects it.") !=
	              std::string::npos,
	      "the name the table has said");
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
	draw_frames(workspace, *view, *document, 640.0f, 1);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
	draw_frames(workspace, *view, *document, 640.0f, 1);
	bool added = false;
	for (const EditorRequest &request : workspace.requests)
		added = added || (request.kind == EditorRequestKind::EditRecord && request.edits.size() == 1 &&
		                  request.edits[0].operation == EditOperation::Add && request.edits[0].field == "name" &&
		                  request.edits[0].value == Value(std::string("wepdes")));
	CHECK(added, "Enter: an Add with the name typed");
}

// ADR 0046 S14: an outline that filters its rows by kind and leaves out the rows its type says hold
// nothing (OutlineSpec::by_kind, row_listed), over the pool document (crates, barrels and a note; a
// crate that weighs nothing is left out): a chip per kind, a click on one leaving its kind's rows out
// and another listing them again; the switch listing the rows left out; a Shift click selecting the
// lines from the primary's to it in one selection, a Ctrl click toggling one; in the list and in the
// tree alike.
void test_outline_kinds_and_clicks() {
	using editor_test::PoolDocument;
	for (const OutlineMode mode : {OutlineMode::List, OutlineMode::Tree}) {
		auto pool = std::make_shared<PoolDocument>();
		Diagnostic error;
		const std::string text = "C alpha 5\nB bravo 3\nC charlie 0\nN delta words\nB echo 7\n";
		CHECK(pool->load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "pool.txt", AssetKind::Unknown, "jo", error),
		      "the pool loads");
		const std::shared_ptr<const DocumentBase> document = pool;
		OutlineSpec spec;
		spec.mode = mode;
		spec.by_kind = true;
		spec.row_listed = editor_test::pool_row_listed;
		spec.unlisted = "Empty rows";
		OutlineView view(spec);
		NullBackend backend;
		TestWorkspace workspace;
		seed(workspace.seeded, document); // alpha selected
		const auto row_at = [&](size_t i) { return NodeAddress{pool->rows()[i]->id, pool->rows()[i]->kind, 0}; };
		const auto shown = [&] { return view_frame(workspace, view, *document, 520.0f, true); };
		draw_frames(workspace, view, *document, 520.0f, 3);
		std::string frame = shown();
		CHECK(in_order(frame, {"Crate", "Barrel", "Note", "Empty rows", "alpha", "bravo", "delta", "echo"}) &&
		              frame.find("charlie") == std::string::npos,
		      "a chip per kind, the switch, the rows that hold something");
		CHECK(workspace.requests.empty(), "drawing raises nothing");
		// The Barrel chip: its rows left out, then listed again.
		const ImGuiID tab = Ui::window_id("Tab");
		// A tree's lines are in a child of their own; its chips and tools stand in the tab.
		const ImGuiID chip = item_id(tab, {"kinds", "Barrel"});
		ImGui::ActivateItemByID(chip);
		draw_frames(workspace, view, *document, 520.0f, 2);
		frame = shown();
		CHECK(frame.find("bravo") == std::string::npos && frame.find("echo") == std::string::npos &&
		              in_order(frame, {"alpha", "delta"}) && view.outline()->kinds() == ~uint64_t(2),
		      "the Barrel chip pressed: the barrels left out");
		ImGui::ActivateItemByID(chip);
		draw_frames(workspace, view, *document, 520.0f, 2);
		CHECK(in_order(shown(), {"alpha", "bravo", "delta", "echo"}) && view.outline()->kinds() == ~uint64_t(0),
		      "pressed again: listed");
		// The switch: the crate that weighs nothing listed too.
		ImGui::ActivateItemByID(item_id(tab, {"Empty rows"}));
		draw_frames(workspace, view, *document, 520.0f, 2);
		CHECK(in_order(shown(), {"alpha", "bravo", "charlie", "delta", "echo"}) && view.outline()->all_rows(),
		      "the switch lists the rows left out");
		// What the outline lists is the workspace's (the MCP gaps lane): each change a set_workspace of the
		// document's view, the view showing it the while.
		CHECK(!workspace.requests.empty() &&
		              std::all_of(workspace.requests.begin(), workspace.requests.end(),
		                          [](const EditorRequest &request) {
			                          return request.kind == EditorRequestKind::SetWorkspace &&
			                                 request.workspace.find("\"document\"") != std::string::npos &&
			                                 request.workspace.find("pool.txt") != std::string::npos;
		                          }),
		      "the chips and the switch are the workspace's: a set_workspace each");
		workspace.requests.clear();

		// A line by its record: where the mouse hovers it, found down the window.
		const ImGuiWindow *window = ImGui::FindWindowByName("Tab");
		const auto line_of = [&](size_t row, ImVec2 &at) {
			for (float y = window->Pos.y + 30.0f; y < window->Pos.y + window->Size.y; y += 2.0f) {
				ImGui::GetIO().AddMousePosEvent(window->Pos.x + 60.0f, y);
				view_frame(workspace, view, *document, 520.0f);
				workspace.requests.clear();
				ImGui::GetIO().AddMouseButtonEvent(0, true);
				view_frame(workspace, view, *document, 520.0f);
				ImGui::GetIO().AddMouseButtonEvent(0, false);
				view_frame(workspace, view, *document, 520.0f);
				const bool hit = !workspace.requests.empty() &&
				                 workspace.requests.back().kind == EditorRequestKind::SelectRecord &&
				                 workspace.requests.back().address == row_at(row);
				workspace.requests.clear();
				if (hit) {
					at = ImVec2(window->Pos.x + 60.0f, y);
					return true;
				}
			}
			return false;
		};
		ImVec2 delta, bravo;
		CHECK(line_of(3, delta) && line_of(1, bravo), "a plain click on a line selects its record");
		const auto click = [&](ImVec2 at, ImGuiKey modifier) {
			workspace.requests.clear();
			ImGui::GetIO().AddMousePosEvent(at.x, at.y);
			view_frame(workspace, view, *document, 520.0f);
			ImGui::GetIO().AddKeyEvent(modifier, true);
			ImGui::GetIO().AddMouseButtonEvent(0, true);
			view_frame(workspace, view, *document, 520.0f);
			ImGui::GetIO().AddMouseButtonEvent(0, false);
			view_frame(workspace, view, *document, 520.0f);
			ImGui::GetIO().AddKeyEvent(modifier, false);
			view_frame(workspace, view, *document, 520.0f);
			return workspace.requests;
		};
		// Shift, the primary alpha: alpha, bravo and charlie named with delta, delta the record.
		std::vector<EditorRequest> requests = click(delta, ImGuiMod_Shift);
		CHECK(requests.size() == 1 && requests[0].kind == EditorRequestKind::SelectRecord &&
		              requests[0].address == row_at(3) && requests[0].mode == SelectMode::Replace &&
		              requests[0].records == (std::vector<NodeAddress>{row_at(0), row_at(1), row_at(2)}),
		      "Shift+click: the lines from the primary's to it, one selection");
		requests = click(bravo, ImGuiMod_Ctrl);
		CHECK(requests.size() == 1 && requests[0].address == row_at(1) && requests[0].mode == SelectMode::Toggle &&
		              requests[0].records.empty(),
		      "Ctrl+click toggles the record");
		// Several rows selected: drawn (each one's line marked), nothing raised.
		workspace.requests.clear();
		workspace.seeded.documents.selection.select(document->path(), row_at(3), {row_at(0), row_at(1)}, SelectMode::Replace);
		workspace.seeded.revisions.touch(ViewConcern::Selection);
		ImGui::GetIO().AddMousePosEvent(-1000.0f, -1000.0f);
		draw_frames(workspace, view, *document, 520.0f, 3);
		CHECK(workspace.seeded.documents.selection.holds(row_at(1)) && workspace.requests.empty(),
		      "several rows selected: drawing them raises nothing");
	}
}

// The minted mission (fixtures/bms/synth_logic.bms) as its type's document.
std::shared_ptr<const DocumentBase> load_mission(const std::string &repo) {
	std::shared_ptr<DocumentBase> made = document_type_for(AssetKind::Mission)->make();
	Diagnostic error;
	CHECK(made && made->load_bytes(test_io::read_file(repo + "/fixtures/bms/synth_logic.bms"), "synth_logic.bms",
	                               AssetKind::Mission, "jo", error),
	      "the mission loads");
	return made;
}

// S13 V5, S14: the MainViewport role's view (ui/main_viewport_view), the mission type's row's (its
// kMissionOutline beside the mission kind, make_view): the document's outline in a column, the
// viewport beside it, its canvas filling the rest of the tab, drawn through the workspace's device of
// (document, kind) once the Shell's pump has made it; the viewport the one kept for the document.
void test_main_viewport_view() {
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::shared_ptr<const DocumentBase> mission = load_mission(repo);
	if (!mission) return;
	const DocumentViewRow *row = document_view_row(DocumentTypeId::Mission);
	CHECK(row && row->role == DocumentViewRole::MainViewport && row->outline,
	      "the mission's row: the MainViewport role over an outline");
	if (!row || !row->outline) return;
	NullBackend backend;
	TestWorkspace workspace;
	seed(workspace.seeded, mission);
	HandViewports shell;
	shell.bind(workspace.seeded);
	shell.viewports->ensure(mission->path(), ViewportKind::Mission);
	workspace.source = &shell.devices.cache;
	std::unique_ptr<DocumentView> made = make_view(*mission);
	auto *view = dynamic_cast<MainViewportView *>(made.get());
	CHECK(view && view->kind() == ViewportKind::Mission && view->outline() && view->outline()->mode() == row->outline->mode,
	      "its view: the outline in its row's mode, beside the mission kind's viewport");
	if (!view) return;
	// The Shell's pump before each frame: the device the canvas asked for made; the canvas sizes the
	// device as it draws it, raising nothing (the device reports its size at the pump).
	for (int i = 0; i < 4; ++i) {
		shell.devices.sync(*shell.viewports, workspace.seeded);
		view_frame(workspace, *view, *mission, 720.0f);
		view->end_frame(workspace);
		CHECK(workspace.requests.empty(), "the canvas raises nothing as it draws");
		workspace.requests.clear();
	}
	shell.devices.sync(*shell.viewports, workspace.seeded);
	const ViewportModel *viewport = shell.find(mission->path(), ViewportKind::Mission);
	const DrawnDevice *device = shell.device(mission->path(), ViewportKind::Mission);
	CHECK(viewport && viewport->status() == ViewportStatus::Ready, "the viewport shows the mission");
	CHECK(device && device->draws > 0 && device->width > 0 && device->width < 720 && device->origin.x > 100.0f,
	      "the viewport drawn on its device beside the outline");
	CHECK(device && viewport && device->width == viewport->size().width && device->height == viewport->size().height &&
	              viewport->canvas_sized(),
	      "the viewport at the canvas's size, its device's as drawn");
	const std::string title = first_title(*mission);
	CHECK(!title.empty() && view_frame(workspace, *view, *mission, 720.0f, true).find(title.substr(0, 8)) != std::string::npos,
	      "the outline drawn beside it");
}

// The MainViewport role's view in the Document window, as its tab draws it (the mission's, S14),
// while an operation holds the documents (a refresh, S13 A3): its outline column is held back (a
// click on a line selects nothing), its canvas is not (Alt and a drag on the picture orbit the
// camera, a SetViewport that runs beside any operation); with no operation, the same click on the
// line selects its record.
void test_main_viewport_held() {
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::shared_ptr<const DocumentBase> mission = load_mission(repo);
	if (!mission) return;
	SessionView v;
	seed(v, mission);
	HandViewports shell;
	shell.bind(v);
	shell.viewports->ensure(mission->path(), ViewportKind::Mission);
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&shell.devices.cache);
	ui.pump = [&] { shell.pump(ui.windows, v); };
	ui.frames(2);
	DocumentWindow *documents = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count() && !documents; ++i)
		documents = dynamic_cast<DocumentWindow *>(&ui.windows.pass().window(i));
	CHECK(documents != nullptr, "the Document window");
	if (!documents) return;
	documents->set_view(*mission, make_view(*mission));
	ui.focus("Document");
	ui.frames(4);
	ui.away();
	ui.drain();
	const auto *viewport = static_cast<const MissionViewport *>(shell.find(mission->path(), ViewportKind::Mission));
	const DrawnDevice *device = shell.device(mission->path(), ViewportKind::Mission);
	CHECK(viewport && viewport->status() == ViewportStatus::Ready && device && device->draws > 0,
	      "the viewport drawn in the tab through its device");
	if (!viewport || !device) return;

	// A line of the outline (the tree's own child, in the view's outline column: two outline children
	// deep) under the mouse.
	const ImGuiWindow *window = ImGui::FindWindowByName("Document");
	CHECK(window != nullptr, "the Document window's ImGui window");
	if (!window) return;
	const auto selects = [&](const std::vector<EditorRequest> &requests) {
		return std::count_if(requests.begin(), requests.end(),
		                     [](const EditorRequest &request) { return request.kind == EditorRequestKind::SelectRecord; });
	};
	const float x = window->Pos.x + 40.0f;
	ImVec2 line(0.0f, 0.0f);
	for (float y = window->Pos.y + 30.0f; y < window->Pos.y + window->Size.y && line.y == 0.0f; y += 3.0f) {
		ui.mouse(x, y);
		const ImGuiWindow *hovered = GImGui->HoveredWindow;
		const std::string name = hovered ? hovered->Name : "";
		const size_t first = name.find("/outline_");
		if (GImGui->HoveredId == 0 || first == std::string::npos || name.find("/outline_", first + 1) == std::string::npos)
			continue;
		ui.button(true);
		ui.button(false);
		if (selects(ui.drain()) > 0) line = ImVec2(x, y);
	}
	CHECK(line.y != 0.0f, "with no operation, a click on an outline line selects its record");

	// A refresh holds the documents: the outline held back, the canvas not.
	OperationStatus refresh;
	refresh.id = 9;
	refresh.kind = OperationKind::Refresh;
	refresh.label = "Refreshing";
	refresh.reads = operation_kind_row(OperationKind::Refresh).reads;
	refresh.writes = operation_kind_row(OperationKind::Refresh).writes;
	v.activity.operation = refresh;
	v.revisions.touch(ViewConcern::Operation);
	CHECK(!v.allows(EditorRequestKind::EditRecord) && v.allows(EditorRequestKind::SetViewport),
	      "a refresh holds the documents, never a viewport");
	ui.frames(2);
	ui.drain();
	if (line.y != 0.0f) {
		ui.click(line);
		CHECK(selects(ui.drain()) == 0, "held: a click on the outline line selects nothing");
	}
	ui.away();
	const float yaw = viewport->camera().yaw;
	// A press on nothing: a point of the picture clear of every shown mark.
	ImVec2 from(device->origin.x + 16.0f, device->origin.y + 16.0f);
	{
		const std::vector<MissionMark> marks = viewport->marks(device->width, device->height, nullptr);
		for (float y = 16.0f; y < float(device->height) - 16.0f; y += 24.0f) {
			bool clear = false;
			for (float x = 16.0f; x < float(device->width) - 16.0f && !clear; x += 24.0f) {
				clear = true;
				for (const MissionMark &mark : marks)
					clear = clear && !(mark.shown && std::fabs(mark.x - x) < 16.0f && std::fabs(mark.y - y) < 16.0f);
				if (clear) from = ImVec2(device->origin.x + x, device->origin.y + y);
			}
			if (clear) break;
		}
	}
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
	ui.mouse(from.x, from.y);
	ui.button(true);
	ui.mouse(from.x + 40.0f, from.y + 8.0f);
	ui.mouse(from.x + 90.0f, from.y + 16.0f);
	ui.button(false);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
	ui.frames(2);
	CHECK(viewport->camera().yaw != yaw, "held: Alt and a drag on the picture orbit the camera");
	CHECK(selects(ui.drain()) == 0, "the drag selects nothing");
	ui.away();
}

// S13 V10: a text's script view with the workspace's devices (the Shell's pump before each frame
// making the device its Main view asks for, as the headless cache pins the active document's): the
// device placed in the rest of the tab below the toolbar, as wide as the tab's content and as tall as
// what the toolbar leaves, where the view's cursor stands, raising nothing; a popup open over the tab:
// the device not placed, the lines drawn in its place.
void test_script_view_device() {
	const std::string repo = test_paths_repo_root(__FILE__);
	std::shared_ptr<DocumentBase> made = document_type_for(AssetKind::Script)->make();
	Diagnostic error;
	CHECK(made->load_bytes(test_io::read_file(repo + "/fixtures/wac/text_document.wac"), "text_document.wac",
	                       AssetKind::Script, "jo", error),
	      "the script loads");
	const std::shared_ptr<const DocumentBase> document = made;
	NullBackend backend;
	TestWorkspace workspace;
	seed(workspace.seeded, document);
	HandViewports shell;
	shell.bind(workspace.seeded);
	shell.viewports->track(workspace.seeded); // its Main view's viewport, made as its document is open
	workspace.source = &shell.devices.cache;
	std::unique_ptr<DocumentView> view = make_view(*document);
	auto *script = dynamic_cast<ScriptView *>(view.get());
	CHECK(script != nullptr, "a script's view is the script view");
	if (!script) return;
	for (int i = 0; i < 3; ++i) {
		shell.devices.sync(*shell.viewports, workspace.seeded);
		view_frame(workspace, *view, *document, 640.0f);
		view->end_frame(workspace);
	}
	const auto *viewport = static_cast<const ScriptViewport *>(shell.find(document->path(), ViewportKind::Script));
	const DrawnDevice *device = shell.device(document->path(), ViewportKind::Script);
	CHECK(viewport && viewport->status() == ViewportStatus::Ready && device && device->draws > 0 && script->device_drawn(),
	      "the script device drawn in the tab");
	CHECK(workspace.requests.empty(), "placing the device raises nothing");
	if (!device) return;
	const ImGuiWindow *tab = ImGui::FindWindowByName("Tab");
	CHECK(tab && device->origin.y > tab->Pos.y + ImGui::GetFrameHeight() && device->width >= 600 &&
	              float(device->origin.y + float(device->height)) <= tab->Pos.y + tab->Size.y,
	      "the device in the rest of the tab below the toolbar");
	CHECK(device->last.canvas_sized && device->last.clip_right > device->last.clip_left, "its rect, sized by the view, clipped");
	// One frame of the tab, at `tab_y`: Dear ImGui's implicit window first brought over it in the display
	// order where asked; another window drawn before it whose item holds the input where asked (made the
	// active item where asked, kept alive each frame: as a slider held by the keys or a field typed in that
	// lets nothing overlap it); a window drawn after it over its rect where asked.
	struct Frame {
		bool implicit_front = false;
		bool field = false;
		bool focus_field = false;
		bool over = false;
		float tab_y = 0.0f;
	};

	const auto frame = [&](const Frame &f) {
		shell.devices.sync(*shell.viewports, workspace.seeded);
		ImGui::NewFrame();
		if (f.implicit_front) ImGui::BringWindowToDisplayFront(ImGui::FindWindowByName("Debug##Default"));
		if (f.field) {
			ImGui::SetNextWindowPos(ImVec2(700.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(240.0f, 80.0f));
			ImGui::Begin("Field", nullptr, ImGuiWindowFlags_NoSavedSettings);
			const ImGuiID held = ImGui::GetID("##held");
			if (f.focus_field) ImGui::SetActiveID(held, ImGui::GetCurrentWindow());
			ImGui::KeepAliveID(held);
			ImGui::End();
		}
		ImGui::SetNextWindowPos(ImVec2(0.0f, f.tab_y));
		ImGui::SetNextWindowSize(ImVec2(640.0f, 560.0f));
		ImGui::Begin("Tab", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
		view->draw(workspace, *document);
		ImGui::End();
		if (f.over) {
			ImGui::SetNextWindowPos(ImVec2(100.0f, 200.0f));
			ImGui::SetNextWindowSize(ImVec2(240.0f, 120.0f));
			ImGui::Begin("Over", nullptr, ImGuiWindowFlags_NoSavedSettings);
			ImGui::TextUnformatted("a window over the tab");
			ImGui::End();
		}
		ImGui::Render();
		view->end_frame(workspace);
	};
	Frame implicit_front;
	implicit_front.implicit_front = true;
	Frame over;
	over.over = true;
	const Frame plain;
	// Dear ImGui's implicit window over the tab (begun every frame, drawn only where something is written
	// to it, which nothing is): no cover, the device placed.
	const ImGuiWindow *implicit = ImGui::FindWindowByName("Debug##Default");
	CHECK(implicit && implicit->IsFallbackWindow && tab && implicit->Rect().Overlaps(tab->Rect()),
	      "Dear ImGui's implicit window, where the tab is");
	int placed = device->draws;
	frame(implicit_front);
	CHECK(GImGui->Windows.back() == implicit && device->draws == placed + 1 && script->device_drawn(),
	      "the implicit window last in the display order: the device placed all the same");
	// A window over the tab, begun after it: the device not placed from the frame after it first drew;
	// gone, placed again once a frame has not drawn it.
	for (int i = 0; i < 3; ++i) frame(over);
	CHECK(!script->device_drawn() && script->lines().lines_drawn() > 0, "a window over the tab: the lines drawn");
	frame(plain);
	frame(plain);
	placed = device->draws;
	frame(plain);
	CHECK(script->device_drawn() && device->draws == placed + 1, "the window gone: the device placed again");
	// A window begun after the tab is known only once begun: the frame's end looks again and hides the
	// device the same frame (a picture with no room), the lines coming the frame after.
	placed = device->draws;
	frame(over);
	CHECK(!script->device_drawn() && device->draws == placed + 2 && device->last.width == 0 && device->last.height == 0,
	      "a window begun after the tab over its rect: the device hidden the frame it first drew");
	frame(plain);
	frame(plain);
	CHECK(script->device_drawn(), "that window gone: the device placed again");
	// Another window's item holding the input (the active item, overlapping nothing): the pointer over
	// the device's rect is let through all the same (the next frame's WantCaptureMouse none), so the
	// press that ends the other item's hold reaches the control rather than Dear ImGui.
	Frame field;
	field.field = true;
	field.focus_field = true;
	frame(field);
	field.focus_field = false;
	for (int i = 0; i < 3; ++i) frame(field);
	CHECK(GImGui->ActiveId != 0 && !GImGui->ActiveIdAllowOverlap, "another window's item holds the input");
	ImGui::GetIO().AddMousePosEvent(float(device->origin.x) + 40.0f, float(device->origin.y) + 40.0f);
	frame(field);
	frame(field);
	CHECK(GImGui->ActiveId != 0 && !ImGui::GetIO().WantCaptureMouse,
	      "the pointer over the device's rect while another item holds the input: let through to the control");
	ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	frame(plain);
	frame(plain);
	// The toolbar's tips above their tools, never over the device's rect (a Control draws over every
	// tip): the tab lower down, the pointer over its first tool (Reload).
	Frame lower;
	lower.tab_y = 120.0f;
	frame(lower);
	const ImGuiWindow *lowered = ImGui::FindWindowByName("Tab");
	const ImVec2 reload = lowered ? lowered->DC.CursorStartPos : ImVec2(0.0f, 0.0f);
	ImGui::GetIO().AddMousePosEvent(reload.x + 6.0f, reload.y + 6.0f);
	for (int i = 0; i < 3; ++i) frame(lower);
	const ImGuiWindow *tip = ImGui::FindWindowByName("##Tooltip_00");
	CHECK(tip && tip->Active && tip->Rect().Max.y <= reload.y + 0.5f && tip->Rect().Max.y <= float(device->origin.y) &&
	              script->device_drawn(),
	      "the Reload tool's tip above it, clear of the device placed under it");
	ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	frame(plain);
	frame(plain);	// A popup over the tab: the device not placed, the lines in its place.
	const int draws = device->draws;
	shell.devices.sync(*shell.viewports, workspace.seeded);
	ImGui::NewFrame();
	ImGui::OpenPopup("covering");
	if (ImGui::BeginPopup("covering")) {
		ImGui::TextUnformatted("a menu");
		ImGui::EndPopup();
	}
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(640.0f, 560.0f));
	ImGui::Begin("Tab", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
	view->draw(workspace, *document);
	ImGui::End();
	ImGui::Render();
	CHECK(device->draws == draws && !script->device_drawn() && script->lines().lines_drawn() > 0,
	      "a popup open: the lines drawn, the device not placed");
	std::printf("script view: its device drawn %d times, a popup's frame the lines\n", draws);
}

// A text of many lines is a list clipped to what shows (a log of a frame draws every line: Dear
// ImGui's clipper draws all while it logs); a reveal scrolls the line it names into sight; its
// markers are made once per change of the findings or the text, never for a frame.
void test_long_text() {
	std::string text;
	for (int i = 1; i <= 5000; ++i) text += "line " + std::to_string(i) + "\r\n";
	auto loaded = std::make_shared<TextDocument>();
	Diagnostic error;
	CHECK(loaded->load_bytes(text_bytes(text), "long.txt", AssetKind::Text, "jo", error), "the text loads");
	const std::shared_ptr<const DocumentBase> document = loaded;
	std::unique_ptr<DocumentView> view = make_view(*document);
	auto *script = dynamic_cast<ScriptView *>(view.get());
	CHECK(script != nullptr, "a text's view");
	if (!script) return;
	TextView *text_view = &script->lines();
	NullBackend backend;
	TestWorkspace workspace;
	seed(workspace.seeded, document);
	draw_frames(workspace, *view, *document, 520.0f, 3);
	// The lines' child window, scrolled to the top.
	ImGuiWindow *tab = ImGui::FindWindowByName("Tab");
	CHECK(tab != nullptr, "the tab's window");
	if (!tab) return;
	char name[64];
	std::snprintf(name, sizeof(name), "Tab/text_%08X", tab->GetID("text"));
	const ImGuiWindow *lines = ImGui::FindWindowByName(name);
	CHECK(lines != nullptr && lines->Scroll.y == 0.0f, "the lines' window, at the top");
	if (!lines) return;
	const float step = ImGui::GetFrameHeightWithSpacing();
	ViewEvent go;
	go.kind = ViewEventKind::RevealText;
	go.path = document->path();
	go.locator = "4000:3";
	view->receive(go);
	draw_frames(workspace, *view, *document, 520.0f, 3);
	const float top = 3999.0f * step;
	CHECK(text_view->marked_line() == 4000 && lines->Scroll.y <= top && lines->Scroll.y + lines->Size.y >= top + step,
	      "the revealed line scrolled into sight");
	CHECK(text_view->markers_made() == 1 && workspace.requests.empty(), "its markers made once, nothing raised");
	// Clipped: the frame drew the lines in sight (a 560-pixel tab), never the 5,000.
	CHECK(text_view->lines_drawn() > 0 && text_view->lines_drawn() < 100, "the lines in sight drawn alone");
}

// A weapon's Show on the HUD (DI-20, ui/catalog_inspector_view, heading the Inspector over a catalog's
// record): over a weapon of weapon.def it goes to the project's hudpos.def (an OpenDocument, as Go to
// raises it) and holds the weapon there (a SetViewport of the HUD viewport's weapon); over a project
// with no hudpos.def it cannot; over another catalog's record it draws nothing.
void test_weapon_shows_on_the_hud() {
	const auto loaded = std::make_shared<DefCatalogDocument>();
	Diagnostic error;
	const std::string weapons = "weapon \"WPN_HUD\"\r\nclipsize 30\r\nend\r\n";
	CHECK(loaded->load_bytes(std::vector<uint8_t>(weapons.begin(), weapons.end()), "defs/weapon.def",
	                         AssetKind::WeaponDefs, "jo", error) &&
	              !loaded->rows().empty(),
	      "the weapon table loads");
	if (loaded->rows().empty()) return;
	const std::shared_ptr<const DocumentBase> document = loaded;
	NullBackend backend;
	TestWorkspace workspace;
	seed(workspace.seeded, document);
	const NodeAddress weapon{loaded->rows()[0]->id, loaded->rows()[0]->kind, 0};
	const auto draw = [&](const NodeAddress &record) {
		InspectorTaken taken;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
		ImGui::Begin("Inspector part", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
		const bool drew = draw_catalog_inspector(workspace, *loaded, record, taken);
		ImGui::End();
		ImGui::Render();
		return drew;
	};
	// No hudpos.def in the project: the tool is there, and does nothing.
	CHECK(draw(weapon), "a weapon's part draws");
	const ImGuiID show = item_id(Ui::window_id("Inspector part"), {"Show on the HUD"});
	ImGui::ActivateItemByID(show);
	draw(weapon);
	draw(weapon);
	CHECK(workspace.requests.empty(), "with no hudpos.def it cannot show the weapon");
	// The project's HUD layout: the weapon held in its HUD's preview.
	editor_test::own(workspace.seeded.project.scan).entries.push_back(
	        file_entry("hudpos.def", "defs/hudpos.def", AssetKind::HudPosDefs));
	editor_test::own(workspace.seeded.project.scan).index();
	draw(weapon);
	ImGui::ActivateItemByID(show);
	draw(weapon);
	draw(weapon);
	CHECK(workspace.requests.size() == 2 && workspace.requests[0].kind == EditorRequestKind::OpenDocument &&
	              workspace.requests[0].path == "defs/hudpos.def" &&
	              workspace.requests[1].kind == EditorRequestKind::SetViewport &&
	              workspace.requests[1].path == "defs/hudpos.def" &&
	              workspace.requests[1].viewport.find("\"hud\"") != std::string::npos &&
	              workspace.requests[1].viewport.find("\"WPN_HUD\"") != std::string::npos,
	      "Show on the HUD goes to hudpos.def and holds the weapon in its HUD");
	// An item table's record (DI-18): Place in mission, held back with no mission open.
	const auto items = std::make_shared<DefCatalogDocument>();
	const std::string item = "begin \"Crate\"\r\nid 100001\r\ntype building\r\nend\r\n";
	CHECK(items->load_bytes(std::vector<uint8_t>(item.begin(), item.end()), "defs/items.def", AssetKind::ItemDefs, "jo",
	                        error) &&
	              !items->rows().empty(),
	      "the item table loads");
	if (items->rows().empty()) return;
	const NodeAddress crate{items->rows()[0]->id, items->rows()[0]->kind, 0};
	const auto draw_item = [&] {
		InspectorTaken taken;
		ImGui::NewFrame();
		ImGui::Begin("Inspector part", nullptr, ImGuiWindowFlags_NoSavedSettings);
		const bool drew = draw_catalog_inspector(workspace, *items, crate, taken);
		ImGui::End();
		ImGui::Render();
		return drew;
	};
	workspace.requests.clear();
	CHECK(draw_item() && places_in_mission(*items, crate), "an item's part: Place in mission");
	ImGui::ActivateItemByID(item_id(Ui::window_id("Inspector part"), {"Place in mission"}));
	draw_item();
	draw_item();
	CHECK(workspace.requests.empty(), "with no mission open it places nothing");
}

} // namespace

} // namespace editor_ui_test

int main() {
	editor_ui_test::test_every_view();
	editor_ui_test::test_long_text();
	editor_ui_test::test_script_view_device();
	editor_ui_test::test_a_view_per_document();
	editor_ui_test::test_edit_scrolled_out();
	editor_ui_test::test_strings_sections();
	editor_ui_test::test_outline_kinds_and_clicks();
	editor_ui_test::test_main_viewport_view();
	editor_ui_test::test_main_viewport_held();
	editor_ui_test::test_weapon_shows_on_the_hud();
	if (editor_ui_test::g_failures) {
		std::printf("%d check(s) failed\n", editor_ui_test::g_failures);
		return 1;
	}
	std::printf("editor_view_contract passed\n");
	return 0;
}
