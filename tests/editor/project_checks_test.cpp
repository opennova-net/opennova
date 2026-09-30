// The document types' project checks (ADR 0046 S13 V9): a type's own check across the project's
// files that keeps what it made from one validation to the next, made by its registry row
// (DocumentType::project_check) and kept by whoever validates, one per type by its DocumentTypeId
// (ProjectChecks). The menu type's is the render check, and no other registered type has one; it
// reads a closed menu only where the validation's own checks read its records (with the menu
// type's row standing in with documents that hold no records, it reads and renders none).
// A test's check, on the item catalogs' type standing in for itself (the same documents and own
// findings, DocumentTypeStandIn), runs once per validation, after every file's own findings (it
// reads which files' records their own checks read); its findings are rows after the build's gate
// (after every file's own findings, the use checks', the graph's and the open documents' own, and
// before the render check's notes, in the registry's order, and the last build's own), so an Error
// of its never blocks a build; its answer that they moved is what composes the Problems rows
// again (a validation that moves nothing else composes them when it says so, and leaves them as
// they were when it does not, even over findings it changed); the project closed, it lets go of
// what it held. A stand-in put in place after the checks were made is followed at their next
// update, and its check goes with it.
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/document_types.h>
#include <editor/documents/project_check.h>
#include <editor/documents/project_checks.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/model/document.h>
#include <editor/preview/menu_render_check.h>
#include <editor/project/project_document.h>
#include <editor/project/project_findings.h>
#include <editor/project_build/build_run.h>
#include <editor/requirements/requirements.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/blob_document.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

// What the test's check was asked and did, which the test reads and sets: the checks its hook
// made, the updates and clears they ran, what the last update was handed, the findings every
// update takes, and whether an update says they moved.
struct Probe {
	size_t made = 0, updates = 0, clears = 0;
	size_t files = 0; // the scan's files the last update was handed
	bool items_checked = false; // whether the item table's own checks read its records then
	std::vector<Diagnostic> next;
	bool moves = false;
};
Probe g_probe;

class ProbeCheck : public ProjectCheck {
public:
	bool update(const ProjectCheckInput &input) override {
		++g_probe.updates;
		g_probe.files = input.validation.scan.entries.size();
		g_probe.items_checked = input.cache.records_checked("defs/items.def");
		// Its findings follow `next` whether or not it says so: the answer alone is what the
		// composition goes by.
		findings_ = g_probe.next;
		return g_probe.moves;
	}
	const std::vector<Diagnostic> &findings() const override { return findings_; }
	void clear() override {
		++g_probe.clears;
		findings_.clear();
	}

private:
	std::vector<Diagnostic> findings_;
};

std::unique_ptr<ProjectCheck> make_probe() {
	++g_probe.made;
	return std::make_unique<ProbeCheck>();
}

// The item catalogs' type with the probe as its project check: its documents and its files' own
// findings are the catalogs'. Copied from the registry before any stand-in is in place.
const DocumentType &probe_type() {
	static const DocumentType type = [] {
		DocumentType row = *document_type(DocumentTypeId::Catalog);
		row.name = "catalog_probed";
		row.project_check = make_probe;
		return row;
	}();
	return type;
}

Diagnostic probe_finding() {
	return make_diagnostic(DiagnosticSeverity::Error, "probe.found", "The probe found this.",
			"defs/items.def");
}

std::unique_ptr<DocumentBase> make_blob() { return std::make_unique<editor_test::BlobDocument>(); }

// The menu type's row with documents that hold no records (S13 D6's blob) and its hook kept: the
// validation reads no menu's records, and the render check is still the menu type's check.
// Copied from the registry before any stand-in is in place.
const DocumentType &blob_menu_type() {
	static const DocumentType type = [] {
		DocumentType row = *document_type(DocumentTypeId::Menu);
		row.name = "menu_blob";
		row.make = make_blob;
		return row;
	}();
	return type;
}

std::string window(const std::string &name, const std::string &body) {
	return "<WINDOW TYPE=\"STATIC\" NAME=\"" + name +
			"\">\r\n<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>90</RIGHT><BOTTOM>90</BOTTOM></"
			"POSITION>\r\n" +
			body + "</WINDOW>\r\n";
}

std::string item(const std::string &name, int id) {
	return "begin \"" + name + "\"\n  id " + std::to_string(id) +
			"\n  type marker\n  hp 0\nend\n\n";
}

using Files = std::vector<std::pair<std::string, std::string>>;

// A finding of each source the composition orders: an id repeated in the item table (its own),
// a variable no menu uses (a use check's), a %NOPE% no stylesheet defines (the graph's) and a
// colour the game draws transparent (the render check's note).
Files composed_files() {
	return {
		{ "menus/menu_style.mns", "USED FF00FF00\r\nUNUSED_V 1\r\n" },
		{ "menus/main.mnu",
				"<SCREEN>\r\n<NAME>MAIN</NAME>\r\n" +
						window("A",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"COLOR\">%USED%</APPEARANCE>\r\n") +
						window("B",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"COLOR\">FF0000</APPEARANCE>\r\n") +
						window("C", "<STRING>%NOPE%</STRING>\r\n") + "</SCREEN>\r\n" },
		{ "defs/items.def", item("Alpha", 5) + item("Bravo", 5) },
	};
}

// A project of `files`, scanned, and the project's files as the game looks them up.
struct Project {
	editor_test::TempProjectDir dir;
	std::string root;
	ProjectDocument document;
	ProjectPaths paths;
	AssetScan scan;
	ProjectAssetSource files;

	explicit Project(const char *name) : dir(name), root(dir.file("Game")) {}
	bool make(const Files &made) {
		Diagnostic error;
		if (!create_project(root, "Checks", "jo", document, error))
			return false;
		for (const auto &file : made)
			if (!editor_test::write_text(root + "/" + file.first, file.second))
				return false;
		paths = ProjectPaths::for_root(root);
		scan = scan_project_assets(paths, document);
		files.set_scan(root, scan, document.target_game);
		return true;
	}
};

// The index of the first (or the last) row whose code starts with `prefix`, or SIZE_MAX.
size_t first_of(const std::vector<Diagnostic> &rows, const std::string &prefix) {
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i].code.rfind(prefix, 0) == 0)
			return i;
	return SIZE_MAX;
}
size_t last_of(const std::vector<Diagnostic> &rows, const std::string &prefix) {
	for (size_t i = rows.size(); i-- > 0;)
		if (rows[i].code.rfind(prefix, 0) == 0)
			return i;
	return SIZE_MAX;
}

void print_rows(const std::vector<Diagnostic> &rows) {
	for (size_t i = 0; i < rows.size(); ++i)
		std::printf("  %zu %s %s\n", i, rows[i].code.c_str(), rows[i].asset.c_str());
}

} // namespace

// The registry: the menu type's project check is the render check and no other registered type
// has one (document_types.cpp's static_asserts: a type may have none, and no two name one
// check); the checks made from it are the render check alone. A stand-in put in place after they
// were made is followed at their next update, which makes its check and runs it once; gone, its
// check goes with it and so do its findings, the update saying the rows moved.
static int test_registry() {
	for (size_t id = 1; id <= kDocumentTypeCount; ++id) {
		const DocumentType *type = document_type(static_cast<DocumentTypeId>(id));
		TEST_EXPECT(type != nullptr);
		if (!type)
			return 1;
		TEST_EXPECT((type->project_check != nullptr) ==
				(static_cast<DocumentTypeId>(id) == DocumentTypeId::Menu));
	}
	(void)probe_type();
	g_probe = Probe();
	ProjectChecks checks;
	TEST_EXPECT(menu_render_check(&checks) != nullptr && menu_render_check(nullptr) == nullptr);
	TEST_EXPECT(checks.of(DocumentTypeId::None) == nullptr);
	for (size_t id = 1; id <= kDocumentTypeCount; ++id)
		if (static_cast<DocumentTypeId>(id) != DocumentTypeId::Menu)
			TEST_EXPECT(checks.of(static_cast<DocumentTypeId>(id)) == nullptr);
	Project project("opennova_editor_project_checks_registry");
	TEST_EXPECT(project.make({ { "defs/items.def", item("Alpha", 5) } }));
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const ValidationInput validation{ project.paths, project.document, project.scan, open };
	refresh_project(validation, graph, cache);
	const ProjectCheckInput input{ validation, cache, project.files };
	// A project with no menu: the render check has nothing to say.
	TEST_EXPECT(!checks.update(input) && checks.findings_size() == 0);
	g_probe.next = { probe_finding() };
	g_probe.moves = true;
	{
		const DocumentTypeStandIn stand_in(probe_type());
		TEST_EXPECT(checks.of(DocumentTypeId::Catalog) == nullptr && g_probe.made == 0);
		TEST_EXPECT(checks.update(input));
		TEST_EXPECT(g_probe.made == 1 && g_probe.updates == 1 &&
				checks.of(DocumentTypeId::Catalog) != nullptr && checks.findings_size() == 1);
		TEST_EXPECT(g_probe.items_checked && g_probe.files == project.scan.entries.size());
	}
	TEST_EXPECT(checks.update(input));
	TEST_EXPECT(checks.of(DocumentTypeId::Catalog) == nullptr && checks.findings_size() == 0 &&
			g_probe.updates == 1);
	TEST_EXPECT(!checks.update(input));
	return 0;
}

// The render check reads a closed menu only where the validation's own checks read its records
// (ValidationCache::records_checked), which is the contract a type's check keeps whatever its
// type's documents are. Over the same project: with the menu type's row as registered, main.mnu is
// read and rendered (its transparent colour a note); with the row standing in with documents that
// hold no records (the hook kept), the file's own finding is document.no_records, and the render
// check has no document for it and no note. Each leg has its own cache and checks (a cache keeps
// a file's answer until its stamps move).
static int test_records_checked_gate() {
	(void)blob_menu_type();
	Project project("opennova_editor_project_checks_gate");
	TEST_EXPECT(project.make(composed_files()));
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const ValidationInput validation{ project.paths, project.document, project.scan, open };
	const auto notes = [](const ProjectChecks &checks) {
		std::vector<Diagnostic> rows;
		checks.append_findings(rows);
		return first_of(rows, "menu.render.") != SIZE_MAX;
	};
	{
		AssetGraph graph;
		ValidationCache cache;
		ProjectChecks checks;
		refresh_project(validation, graph, cache);
		TEST_EXPECT(cache.records_checked("menus/main.mnu"));
		TEST_EXPECT(checks.update({ validation, cache, project.files }));
		const MenuRenderCheck *check = menu_render_check(&checks);
		TEST_EXPECT(check != nullptr && check->document("menus/main.mnu") != nullptr && notes(checks));
	}
	{
		const DocumentTypeStandIn stand_in(blob_menu_type());
		AssetGraph graph;
		ValidationCache cache;
		ProjectChecks checks;
		refresh_project(validation, graph, cache);
		const std::vector<Diagnostic> *own = cache.kept_findings("menus/main.mnu");
		TEST_EXPECT(!cache.records_checked("menus/main.mnu") && own && own->size() == 1 &&
				own->front().code == "document.no_records");
		checks.update({ validation, cache, project.files });
		const MenuRenderCheck *check = menu_render_check(&checks);
		TEST_EXPECT(check != nullptr && check->document("menus/main.mnu") == nullptr && !notes(checks));
	}
	return 0;
}

// The composition (project/project_findings), as the command line composes it: the rows in
// order, the scan's, the requirements', the boot report's and the last Play's; the gate (every
// file's own findings, the use checks', the graph's, the open documents' own); the project
// checks' in the registry's order (the probe's, the item catalogs' type being before the menu
// type, then the render check's notes); the last build's own. The probe runs once per
// composition, after every file's own findings (the item table's records read), over the scan's
// files; the refresh says a row moved when the probe says so, and not while nothing did.
static int test_composition_order() {
	(void)probe_type();
	g_probe = Probe();
	const DocumentTypeStandIn stand_in(probe_type());
	Project project("opennova_editor_project_checks_order");
	TEST_EXPECT(project.make(composed_files()));
	AssetGraph graph;
	ValidationCache cache;
	ProjectChecks checks;
	TEST_EXPECT(g_probe.made == 1 && checks.of(DocumentTypeId::Catalog) != nullptr &&
			menu_render_check(&checks) != nullptr);
	g_probe.next = { probe_finding() };
	g_probe.moves = true;
	const RequirementReport requirements;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const std::vector<std::string> boot_missing{ "missing.bin" };
	const std::vector<Diagnostic> play{ make_diagnostic(
			DiagnosticSeverity::Warning, "play.exit", "The game ended with an error.") };
	const std::vector<Diagnostic> open_own{ make_diagnostic(DiagnosticSeverity::Warning,
			"document.outside", "The file changed outside the editor.", "defs/items.def") };
	const std::vector<Diagnostic> build{ make_diagnostic(
			DiagnosticSeverity::Error, "build.step", "A step of the build failed.") };
	const ProjectFindingsInput input{ project.paths, project.document, project.scan, requirements,
		open, boot_missing, play, open_own, build };
	const ProjectFindings findings =
			compose_project_findings(input, graph, cache, checks, project.files);
	const std::vector<Diagnostic> &rows = findings.rows;
	TEST_EXPECT(g_probe.updates == 1 && g_probe.items_checked &&
			g_probe.files == project.scan.entries.size());
	const char *const sources[] = { "catalog.item_identity", "style.unused", "reference.missing",
		"menu.render.", "probe.found" };
	bool all = true;
	for (const char *source : sources)
		all = all && first_of(rows, source) != SIZE_MAX;
	const bool ordered = first_of(rows, "play.boot_missing") < first_of(rows, "play.exit") &&
			last_of(rows, "play.exit") < findings.gate_begin &&
			findings.gate_begin <= first_of(rows, "catalog.item_identity") &&
			last_of(rows, "catalog.item_identity") < first_of(rows, "style.unused") &&
			last_of(rows, "style.unused") < first_of(rows, "reference.missing") &&
			last_of(rows, "reference.missing") < first_of(rows, "document.outside") &&
			last_of(rows, "document.outside") < findings.gate_end &&
			findings.gate_end <= first_of(rows, "probe.found") &&
			last_of(rows, "probe.found") < first_of(rows, "menu.render.") &&
			last_of(rows, "menu.render.") < first_of(rows, "build.step") &&
			last_of(rows, "build.step") == rows.size() - 1;
	if (!all || !ordered)
		print_rows(rows);
	TEST_EXPECT(all && ordered);
	// Nothing moved since: the refresh says so, unless the probe says its findings moved.
	g_probe.moves = false;
	TEST_EXPECT(!refresh_project_findings(input, graph, cache, checks, project.files));
	g_probe.moves = true;
	TEST_EXPECT(refresh_project_findings(input, graph, cache, checks, project.files));
	TEST_EXPECT(g_probe.updates == 3);
	return 0;
}

// Through a session: the probe runs once per validation whatever asked for it (a rescan, an
// open, an edit, a save); its answer is what composes the rows again, not its findings; its rows
// sit after the use checks' and an Error of its never blocks a build; the project closed, it lets
// go of what it held.
static int test_session() {
	(void)probe_type();
	g_probe = Probe();
	const DocumentTypeStandIn stand_in(probe_type());
	editor_test::TempProjectDir dir("opennova_editor_project_checks_session");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	TEST_EXPECT(g_probe.made == 1);
	session.handle(request::new_project(dir.file("project"), "Checks"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	TEST_EXPECT(view.findings.project_checks &&
			view.findings.project_checks->of(DocumentTypeId::Catalog) != nullptr);
	const ValidationStats &stats = session.validation_stats();
	const AssetEntry *items_entry = view.project.scan->find("items.def");
	TEST_EXPECT(items_entry != nullptr);
	if (!items_entry)
		return 1;
	const std::string items_path = items_entry->relative_path;
	// Once per validation, whatever asked for it.
	size_t passes = stats.passes;
	size_t updates = g_probe.updates;
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document(items_path));
	Document *items = session.document_for(items_path);
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty())
		return 1;
	EditorRequest edit = request::edit_record(items_path, Edit());
	edit.edits[0].address = { items->rows()[0]->id, items->rows()[0]->kind, 0 };
	edit.edits[0].field = "hp";
	edit.edits[0].value = int64_t(7);
	session.handle(edit);
	TEST_EXPECT(items->dirty());
	session.handle(request::save(items_path));
	TEST_EXPECT(!items->dirty());
	TEST_EXPECT(stats.passes - passes >= 3 && g_probe.updates - updates == stats.passes - passes);
	const auto probe_rows = [&view] {
		size_t found = 0;
		for (const Diagnostic &d : view.findings.diagnostics)
			found += d.code == "probe.found" ? 1 : 0;
		return found;
	};
	// Its findings changed but it says they did not, nothing else moving: no composition, the rows
	// as they were.
	g_probe.next = { probe_finding() };
	g_probe.moves = false;
	size_t compositions = session.problems_compositions();
	uint64_t findings = view.revisions.of(ViewConcern::Findings);
	passes = stats.passes;
	updates = g_probe.updates;
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(stats.passes == passes + 1 && g_probe.updates == updates + 1);
	TEST_EXPECT(session.problems_compositions() == compositions && probe_rows() == 0 &&
			view.revisions.of(ViewConcern::Findings) == findings);
	// It says they moved: the rows composed again, its finding among them, after the use checks'.
	g_probe.moves = true;
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(session.problems_compositions() == compositions + 1 && probe_rows() == 1 &&
			view.revisions.of(ViewConcern::Findings) != findings);
	TEST_EXPECT(first_of(view.findings.diagnostics, "style.unused") != SIZE_MAX &&
			last_of(view.findings.diagnostics, "style.unused") <
					first_of(view.findings.diagnostics, "probe.found"));
	// Said once: a validation where nothing moves again composes nothing, and its row stays.
	g_probe.moves = false;
	compositions = session.problems_compositions();
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(session.problems_compositions() == compositions && probe_rows() == 1);
	// An Error of a project check never blocks a build.
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(probe_rows() == 1);
	if (view.activity.has_build && !view.activity.last_build->ok)
		print_rows(view.activity.last_build->diagnostics);
	TEST_EXPECT(view.activity.has_build && view.activity.last_build->ok);
	// The project closed: the probe lets go of what it held.
	const size_t clears = g_probe.clears;
	session.handle(request::close_project());
	TEST_EXPECT(!view.project.open && g_probe.clears == clears + 1);
	const ProjectCheck *probe = view.findings.project_checks->of(DocumentTypeId::Catalog);
	TEST_EXPECT(probe != nullptr && probe->findings().empty());
	return 0;
}

int main() {
	int failures = 0;
	failures += test_registry();
	failures += test_records_checked_gate();
	failures += test_composition_order();
	failures += test_session();
	if (failures == 0)
		std::printf("editor_project_checks: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
