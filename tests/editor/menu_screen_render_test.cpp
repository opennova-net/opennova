// The menu render check (editor/preview, ADR 0046 S9j2): every menu screen of the project
// compiled headless the way the game draws it, its compiler notes as Problems rows on the
// record and the field they name, never a build's gate, and never a finding the asset
// graph already makes. A new project's STARTUP screen renders clean: TITLE text sized in
// its font from fonts/, the document's windows in the compiler's order. An edit that cuts
// TITLE's label or makes a colour transparent shows on TITLE and on the APPEARANCE row; a
// texture the project lacks is the graph's finding, the preview's note only; the build
// still packs. A menu renders again only when it, the stylesheets or a file it read move.
// The retail legs (each a SKIP-LEG without its root): every screen of the shipped menus
// loose at OPENNOVA_JO_ASSETS' root (read with the files beside them) and of every .mnu
// OPENNOVA_JO_DIR's packed install serves (read with that mount's files) renders, in the
// document's order, and every texture and font name the compiler interns is an edge of
// the asset graph (the split the render check relies on); a table of the notes is printed.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/file_source.h>
#include <base/vfs/vfs.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/menu_preview_json.h>
#include <editor/preview/menu_render_check.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/session/project_session.h>
#include <formats/mnu/mnu_schema.h>
#include <runtime/menu/menu_screen_inputs.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::menu::MenuFrameNote;
using opennova::menu::MenuFrameNoteCode;

namespace {

using editor_test::NoProcess;

const NodeKind kWindow = node_kind(MenuKind::Window);

void edit(ProjectSession &session, const Document &document, const NodeAddress &address, const char *field,
          Value value) {
	EditorRequest request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.address = address;
	request.edit.field = field;
	request.edit.value = std::move(value);
	session.handle(request);
}

size_t list_index(const char *path) {
	const std::vector<opennova::mnu::SchemaList> &lists = opennova::mnu::schema_lists(opennova::mnu::SchemaShape::Window);
	for (size_t i = 0; i < lists.size(); ++i)
		if (std::string(lists[i].path) == path) return i;
	return SIZE_MAX;
}

std::vector<const Diagnostic *> render_findings(const SessionView &view, const std::string &code = std::string()) {
	std::vector<const Diagnostic *> out;
	for (const Diagnostic &d : view.diagnostics)
		if (d.code.rfind("menu.render.", 0) == 0 && (code.empty() || d.code == code)) out.push_back(&d);
	return out;
}

bool has_note(const std::vector<MenuFrameNote> &notes, MenuFrameNoteCode code) {
	for (const MenuFrameNote &note : notes)
		if (note.code == code) return true;
	return false;
}

// The document's windows are the compiler's, index for index.
bool same_order(const MnuDocument &document, const Node &screen, const opennova::menu::MenuFrameCompiler &compiler) {
	const int count = compiler.widget_count();
	if (document.window_at(screen, size_t(count)) != 0) return false;
	for (int index = 0; index < count; ++index) {
		const NodeId id = document.window_at(screen, size_t(index));
		Value name;
		if (!id || !document.get({screen.id, kWindow, id}, "name", name) || !std::holds_alternative<std::string>(name) ||
		    std::get<std::string>(name) != compiler.widget_name(index))
			return false;
	}
	return true;
}

} // namespace

static int test_blank_startup() {
	editor_test::TempProjectDir dir("opennova_editor_menu_render");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Render Test"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	TEST_EXPECT(view.render_check);
	const MenuRenderCheck &check = *view.render_check;
	// A new project renders clean.
	for (const Diagnostic *d : render_findings(view)) std::printf("  unexpected: %s %s\n", d->code.c_str(), d->message.c_str());
	TEST_EXPECT(render_findings(view).empty());

	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	auto *menu = dynamic_cast<MnuDocument *>(session.document_for("main.mnu"));
	TEST_EXPECT(menu);
	const Node *startup = menu->rows().front().get();
	TEST_EXPECT(startup && startup->name() == "STARTUP");
	const MenuScreenRender *render = check.render(menu->path(), startup->id);
	TEST_EXPECT(render && render->status() == MenuPreviewStatus::Ready);
	const opennova::menu::MenuFrameCompiler &compiler = render->compiler();
	TEST_EXPECT(same_order(*menu, *startup, compiler));
	const int title_index = compiler.widget_index("TITLE");
	opennova::mnu::RectEdges title_rect{};
	TEST_EXPECT(title_index >= 0 && compiler.widget_rect(title_index, render->state(), &title_rect));
	TEST_EXPECT(title_rect.bottom > title_rect.top); // text sized in the font the project ships
	const std::vector<MenuFrameNote> notes = render->notes();
	TEST_EXPECT(!has_note(notes, MenuFrameNoteCode::FontMissing) && !has_note(notes, MenuFrameNoteCode::TextNoFont));
	// MAIN's CUSTOM appearance is the shell's hook: the preview says so, Problems does not.
	TEST_EXPECT(has_note(notes, MenuFrameNoteCode::AppearanceCustom));
	// The render in the preview's schema: every window, current.
	const opennova::io::JsonValue json = menu_preview_to_json(render_snapshot(*render, *menu, *startup));
	TEST_EXPECT(json.get_string("status", "") == "ready" && json.get_bool("current", false));
	TEST_EXPECT(json.get("widgets") && int(json.get("widgets")->array.size()) == compiler.widget_count());
	TEST_EXPECT(json.get("notes") && !json.get("notes")->array.empty());
	TEST_EXPECT(json.get("notes")->array.front().get_string("severity", "") == "preview");

	// TITLE cut to 20 units: its label no longer fits.
	NodeAddress title, exit;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "TITLE", title) &&
			find_definition(AssetGraph(), *menu, "EXIT", exit));
	edit(session, *menu, title, "position.right", int64_t(20));
	std::vector<const Diagnostic *> cut = render_findings(view, "menu.render.text_truncated");
	TEST_EXPECT(cut.size() == 1);
	TEST_EXPECT(cut[0]->severity == DiagnosticSeverity::Warning && cut[0]->asset == menu->path());
	TEST_EXPECT(cut[0]->row_id == startup->id && cut[0]->child_id == title.child && cut[0]->record_kind == kWindow);
	TEST_EXPECT(cut[0]->field == "string.value" && cut[0]->record == menu->record_path(title));
	// EXIT's DEFAULT row made a six-digit colour: transparent, on that APPEARANCE row.
	const int exit_index = menu->window_index(exit);
	const NodeAddress row = menu->record_at(*menu->row(exit.row), size_t(exit_index), list_index("appearance"), 0);
	TEST_EXPECT(row.child != 0);
	edit(session, *menu, row, "type", std::string("COLOR"));
	edit(session, *menu, row, "value", std::string("FF0000"));
	std::vector<const Diagnostic *> clear = render_findings(view, "menu.render.color_transparent");
	TEST_EXPECT(clear.size() == 1 && clear[0]->child_id == row.child && clear[0]->record_kind == row.kind &&
	            clear[0]->field == "value");
	// A texture the project lacks is the graph's finding; the render check leaves it to it.
	edit(session, *menu, row, "type", std::string("IMAGE"));
	edit(session, *menu, row, "value", std::string("nope.tga"));
	TEST_EXPECT(render_findings(view, "menu.render.texture_missing").empty());
	bool graph_has_it = false;
	for (const Diagnostic &d : view.diagnostics) graph_has_it = graph_has_it || d.message.find("nope.tga") != std::string::npos;
	TEST_EXPECT(graph_has_it);
	const MenuScreenRender *again = check.render(menu->path(), startup->id);
	TEST_EXPECT(again && has_note(again->notes(), MenuFrameNoteCode::TextureMissing));
	// A note never blocks a build (the missing texture, the graph's error, would).
	edit(session, *menu, row, "type", std::string("COLOR"));
	edit(session, *menu, row, "value", std::string("FF0000"));
	session.handle(make_request(EditorRequestKind::Save, menu->path()));
	TEST_EXPECT(render_findings(view, "menu.render.text_truncated").size() == 1 &&
	            render_findings(view, "menu.render.color_transparent").size() == 1);
	session.handle(make_request(EditorRequestKind::Build));
	session.run_operations();
	for (const Diagnostic &d : view.last_build.diagnostics)
		if (d.severity == DiagnosticSeverity::Error) std::printf("  build: %s %s\n", d.code.c_str(), d.message.c_str());
	TEST_EXPECT(view.has_build && view.last_build.ok);
	return 0;
}

// A menu renders again only when it, the stylesheets or a file it read move.
static int test_render_again_only_when_moved() {
	editor_test::TempProjectDir dir("opennova_editor_menu_render_again");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Again"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const MenuRenderCheck &check = *view.render_check;
	// The files just made: every menu renders.
	const size_t menus = check.rendered();
	TEST_EXPECT(menus >= 1);
	// Opened, the menu's document stands in for its file (a new document state): it renders.
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	TEST_EXPECT(check.rendered() == 1);
	// A rescan with nothing changed on disk keeps the open document: nothing renders.
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu);
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(check.rendered() == 0 && session.document_for("main.mnu") == menu);
	// A validation where nothing a menu reads moved (the project's features refresh the
	// scan): nothing renders.
	editor_test::set_missions(session, true);
	TEST_EXPECT(check.rendered() == 0);
	// The menu edited: it alone renders again.
	NodeAddress title;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "TITLE", title));
	edit(session, *menu, title, "position.top", int64_t(130));
	TEST_EXPECT(check.rendered() == 1);
	// A stylesheet edit reaches every menu (its %VAR% list).
	session.handle(make_request(EditorRequestKind::OpenDocument, "menu_style.mns"));
	Document *style = session.document_for("menu_style.mns");
	NodeAddress fg;
	TEST_EXPECT(style && find_definition(AssetGraph(), *style, "DEF_TEXT_FG", fg));
	edit(session, *style, fg, "value", std::string("FFFF0000"));
	TEST_EXPECT(check.rendered() == menus);
	// Closed with the project (its edits discarded): nothing kept.
	TEST_EXPECT(check.render("menus/main.mnu", title.row));
	session.handle(make_request(EditorRequestKind::CloseProject));
	EditorRequest discard = make_request(EditorRequestKind::ResolveUnsaved);
	discard.unsaved_choice = UnsavedChoice::Discard;
	session.handle(discard);
	TEST_EXPECT(!view.project_open && !check.render("menus/main.mnu", title.row));
	return 0;
}

namespace {

// The loose files of a directory by flat, case-insensitive name.
class LooseFiles : public opennova::FileSource {
public:
	explicit LooseFiles(const std::string &root) {
		std::error_code ec;
		for (const auto &entry : std::filesystem::directory_iterator(root, ec))
			if (entry.is_regular_file(ec))
				files_[retail::lower_ascii(entry.path().filename().string())] = entry.path().generic_string();
	}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto found = files_.find(key(name));
		return found != files_.end() && test_io::read_file(found->second, out);
	}
	uint64_t stamp(const std::string &name) const override { return files_.count(key(name)) ? 1 : 0; }
	std::vector<std::string> menus() const {
		std::vector<std::string> out;
		for (const auto &entry : files_)
			if (entry.first.size() > 4 && entry.first.substr(entry.first.size() - 4) == ".mnu") out.push_back(entry.second);
		return out;
	}

private:
	static std::string key(const std::string &name) {
		return retail::lower_ascii(std::filesystem::path(name).filename().string());
	}
	std::map<std::string, std::string> files_;
};

std::string resolved(const std::string &value, const std::map<std::string, std::string> &vars) {
	if (value.size() < 2 || value.front() != '%' || value.back() != '%') return value;
	for (const auto &var : vars)
		if (opennova::strutil::iequals(var.first, value.substr(1, value.size() - 2))) return var.second;
	return value;
}

// A name the compiler interned that some edge of the file names, through the stylesheet.
bool has_edge(const Extracted &extracted, ReferenceKind kind, const std::string &name,
              const std::map<std::string, std::string> &vars) {
	for (const GraphEdge &edge : extracted.edges) {
		const bool names = edge.kind == kind || (edge.kind == ReferenceKind::StyleVar && edge.through == kind);
		if (names && opennova::strutil::iequals(resolved(edge.value, vars), name)) return true;
	}
	return false;
}

} // namespace

namespace {

// The mounted files of one layer of the packed install (base, or an expansion), decoded.
class MountedFiles : public opennova::FileSource {
public:
	explicit MountedFiles(const opennova::Vfs &vfs) : vfs_(vfs) {}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override { return vfs_.read_file(name, out); }
	uint64_t stamp(const std::string &name) const override { return vfs_.has_file(name) ? 1 : 0; }

private:
	const opennova::Vfs &vfs_;
};

struct SweepTotals {
	int menus = 0, screens = 0, failures = 0;
	std::map<std::string, int> by_code;
};

// One menu's every screen through MenuScreenRender: it renders, its windows are the
// document's in order, every texture and font name the compiler interns is an edge the
// asset graph reads from the document.
void sweep_menu(const std::string &label, const std::string &path, const opennova::FileSource &files,
                const std::map<std::string, std::string> &vars, SweepTotals &totals) {
	++totals.menus;
	MnuDocument document;
	Diagnostic error;
	if (!document.load(path, std::filesystem::path(path).filename().generic_string(), AssetKind::Menu, "jo", error) ||
	    document.blocked()) {
		std::printf("  FAIL %s: does not load (%s)\n", label.c_str(), error.message.c_str());
		++totals.failures;
		return;
	}
	Extracted extracted;
	extract_from_document(document, extracted);
	for (const auto &row : document.rows()) {
		++totals.screens;
		MenuScreenRender render;
		if (render.configure(document, row->id, files, vars) != MenuPreviewStatus::Ready) {
			std::printf("  FAIL %s %s: %s\n", label.c_str(), row->name().c_str(), render.detail().c_str());
			++totals.failures;
			continue;
		}
		const opennova::menu::MenuDrawList &drawn = render.compile(1.0f, 1.0f);
		const opennova::menu::MenuFrameCompiler &compiler = render.compiler();
		if (!same_order(document, *row, compiler)) {
			std::printf("  FAIL %s %s: the compiler's windows are not the document's\n", label.c_str(),
			            row->name().c_str());
			++totals.failures;
		}
		for (const std::string &name : compiler.texture_names())
			if (!has_edge(extracted, ReferenceKind::MenuTexture, name, vars)) {
				std::printf("  FAIL %s %s: the texture %s has no graph edge\n", label.c_str(), row->name().c_str(),
				            name.c_str());
				++totals.failures;
			}
		for (const std::string &name : compiler.font_names())
			if (!name.empty() && !has_edge(extracted, ReferenceKind::Font, name, vars)) {
				std::printf("  FAIL %s %s: the font %s has no graph edge\n", label.c_str(), row->name().c_str(),
				            name.c_str());
				++totals.failures;
			}
		const std::vector<MenuFrameNote> notes = render.notes();
		for (const MenuFrameNote &note : notes) ++totals.by_code[opennova::menu::menu_frame_note_token(note.code)];
		std::printf("  %-28s %-18s widgets %4d drawn %4lld notes %3zu\n", label.c_str(), row->name().c_str(),
		            compiler.widget_count(), static_cast<long long>(drawn.widgets_drawn), notes.size());
	}
}

void print_totals(const char *leg, const SweepTotals &totals) {
	std::printf("notes by code over %d screen(s):\n", totals.screens);
	for (const auto &entry : totals.by_code) std::printf("  %-26s %d\n", entry.first.c_str(), entry.second);
	std::printf("%s: %d menu(s), %d screen(s), %d failure(s)\n", leg, totals.menus, totals.screens, totals.failures);
}

bool is_menu_name(const std::string &name) {
	return name.size() > 4 && retail::lower_ascii(name.substr(name.size() - 4)) == ".mnu";
}

} // namespace

// The shipped menus loose at OPENNOVA_JO_ASSETS' root, read with the files beside them.
static int test_retail_assets_leg() {
	const std::string root = retail::assets();
	if (root.empty() || !retail::dir_exists(root)) {
		retail::skip_leg("OPENNOVA_JO_ASSETS/*.mnu (every shipped screen through the render check's headless render)");
		return 0;
	}
	LooseFiles files(root);
	std::vector<std::string> menus = files.menus();
	std::sort(menus.begin(), menus.end());
	if (menus.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS/*.mnu (no menu at the tree's root)");
		return 0;
	}
	opennova::menu::MenuStyleSource style;
	const std::map<std::string, std::string> &vars = style.vars(files);
	SweepTotals totals;
	std::printf("the shipped menus at %s through MenuScreenRender:\n", root.c_str());
	for (const std::string &path : menus) sweep_menu(std::filesystem::path(path).filename().string(), path, files, vars, totals);
	print_totals("assets leg", totals);
	return totals.failures == 0 ? 0 : 1;
}

// Every .mnu the packed install serves (the base mount, then each expansion's), read with
// the files that mount serves; a menu a later layer serves byte for byte again is not
// swept twice.
static int test_retail_install_leg() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (every .mnu the packed install serves, through the headless render)");
		return 0;
	}
	editor_test::TempProjectDir scratch("opennova_menu_render_install");
	SweepTotals totals;
	std::map<std::string, std::vector<uint8_t>> seen;
	std::vector<std::string> layers{std::string()};
	for (const std::string &expansion : retail::expansions()) layers.push_back(expansion);
	for (const std::string &layer : layers) {
		opennova::Vfs vfs;
		if (!vfs.mount_game(install, layer, opennova::VfsMountMode::Packed)) {
			std::printf("  FAIL mount_game(%s): %s\n", layer.c_str(), vfs.last_error().c_str());
			++totals.failures;
			continue;
		}
		MountedFiles files(vfs);
		opennova::menu::MenuStyleSource style;
		const std::map<std::string, std::string> &vars = style.vars(files);
		for (const auto &location : vfs.list_files()) {
			if (!is_menu_name(location.logical_name)) continue;
			std::vector<uint8_t> stored;
			if (!vfs.read_file_raw(location.logical_name, stored)) continue;
			std::vector<uint8_t> &earlier = seen[retail::lower_ascii(location.logical_name)];
			if (earlier == stored) continue;
			earlier = stored;
			const std::string path = scratch.file(location.logical_name.c_str());
			if (!editor_test::write_bytes(path, stored)) {
				++totals.failures;
				continue;
			}
			sweep_menu((layer.empty() ? std::string() : layer + "/") + location.logical_name, path, files, vars, totals);
		}
	}
	if (totals.menus == 0) {
		std::printf("  FAIL the packed install served no .mnu\n");
		++totals.failures;
	}
	print_totals("install leg", totals);
	return totals.failures == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	const std::pair<const char *, int (*)()> cases[] = {
	        {"blank_startup", test_blank_startup},
	        {"render_again_only_when_moved", test_render_again_only_when_moved},
	        {"retail_assets_leg", test_retail_assets_leg},
	        {"retail_install_leg", test_retail_install_leg},
	};
	for (const auto &entry : cases) {
		std::fflush(stdout);
		const int result = entry.second();
		std::printf("%s %s\n", result == 0 ? "PASS" : "FAIL", entry.first);
		if (result != 0) return 1;
	}
	return 0;
}
