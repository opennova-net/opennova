// S13 D4: per-file validation (ADR 0046). Each file's own findings are its document type's
// validate_file over its document alone, kept in the validation cache by what they were made
// from (an open document's instance, revision, unsaved state and written file; a closed file's
// size, last write, kind and game), a closed file loaded for them and let go; what other files
// make of what one defines is graph/use_checks (a stylesheet's variables through the menus'
// uses); validate_project runs the graph's update, the files, the use checks and the graph's
// findings in one pass.
//
// The rows are the trunk's: over the validator fixtures and four projects made here (the
// stylesheets' uses, the item ids, both with open documents edited), the rows validate_project
// makes are the rows the trunk's per-type validators (validate_open_documents) made before the
// split, every member of every row (the order is not: a file's own rows come first, the use
// checks' after every file's), but for the ids across item tables, which S13 D4's review dropped:
// two item tables are two files of one name (asset.name.duplicate), of which the game reads one,
// so an id is compared within its table (the trunk's rows of those projects less their
// cross-table catalog.item_identity rows: Golf and India of the item tables, Golf and Hotel with
// them open). Their count and digest were pinned from the trunk's run and pinned again, the rows
// compared member for member, when the review wrote a row's reference kind by its token (its
// number moved with MenuText) and dropped those rows. The retail leg (OPENNOVA_JO_DIR)
// exports the install's files a document type opens into a project and times a first
// validation, one with nothing changed and one after an edit of the open item table. S13 A6:
// every finding the four projects make, composed as the editor and the command line compose
// them, is made from a row of the finding codes' tables, and so is every finding of the install's
// files in the retail leg.
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/install_view.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/document_types.h>
#include <editor/documents/project_checks.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/project_validation.h>
#include <editor/graph/use_checks.h>
#include <editor/import/import_plan.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project/project_findings.h>
#include <editor/requirements/requirements.h>
#include <editor/session/finding_codes.h>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <formats/rtxt/rtxt.h>
#include <base/vfs/vfs.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/fixture_projects.h"

using namespace opennova;
using namespace opennova::editor;
using namespace fixture_projects;
namespace fs = std::filesystem;

namespace {

// S13 A6: every composed finding keeps a table's row, the row its token finds; a type's row is on
// a file its type opens; a finding whose fixes read a required file or a reference is about one.
int check_rows(const char *project, const std::vector<Diagnostic> &rows, const AssetScan &scan,
		std::set<std::string> &codes) {
	for (const Diagnostic &d : rows) {
		codes.insert(d.code());
		const char *owner = finding_owner(d.row());
		if (!owner || finding_row(d.code()) != d.row())
			std::fprintf(stderr, "%s: %s has no table's row\n", project, d.code().c_str());
		TEST_EXPECT(owner && finding_row(d.code()) == d.row());
		if (std::string(owner) != "core") {
			const AssetEntry *file = scan.at_path(d.asset);
			const DocumentType *type = file ? document_type_for(file->kind) : nullptr;
			if (!type || std::string(type->name) != owner)
				std::fprintf(stderr, "%s: %s (%s's) is on %s\n", project, d.code().c_str(), owner,
						d.asset.c_str());
			TEST_EXPECT(type && std::string(type->name) == owner);
		}
		const FindingFix fixes = d.row()->fixes;
		if (fixes == FindingFix::Requirement || fixes == FindingFix::WrongKind)
			TEST_EXPECT(requirement_subject(d) != nullptr);
		if (fixes == FindingFix::Reference || fixes == FindingFix::UnimportedTexture)
			TEST_EXPECT(reference_subject(d) != nullptr);
	}
	return 0;
}

// The rows as a sorted list: the comparison is of the rows, not their order.
std::vector<std::string> sorted_rows(const std::vector<Diagnostic> &findings) {
	std::vector<std::string> rows;
	for (const Diagnostic &d : findings)
		rows.push_back(row_of(d));
	std::sort(rows.begin(), rows.end());
	return rows;
}

uint64_t digest(const std::vector<std::string> &rows) {
	uint64_t hash = io::kFnv1a64Offset;
	for (const std::string &row : rows) {
		hash = io::fnv1a64_bytes(hash, row.data(), row.size());
		hash = io::fnv1a64_bytes(hash, "\n", 1);
	}
	return hash;
}

// A project of `files`, scanned.
struct Project {
	editor_test::TempProjectDir dir{ "opennova_editor_project_validation" };
	std::string root = dir.file("Game");
	ProjectDocument document;
	ProjectPaths paths;
	AssetScan scan;

	bool make(const Files &files) {
		Diagnostic error;
		if (!create_project(root, "Validation", "jo", document, error))
			return false;
		for (const auto &file : files)
			if (!editor_test::write_text(root + "/" + file.first, file.second))
				return false;
		paths = ProjectPaths::for_root(root);
		rescan();
		return true;
	}
	void rescan() { scan = scan_project_assets(paths, document); }
	// A project file as its document type reads it, standing in for the file from then on.
	std::shared_ptr<Document> open(const std::string &path) const {
		const AssetEntry *asset = scan.at_path(path);
		if (!asset)
			return nullptr;
		std::shared_ptr<Document> made = records_of(document_type_for(asset->kind)->make());
		Diagnostic error;
		return made->load(root + "/" + path, path, asset->kind, document.target_game, error)
				? made
				: nullptr;
	}
};

// The shell's stylesheet and the first item table open, each with an edit of its second row.
bool open_edited(const Project &project, std::vector<std::shared_ptr<const DocumentBase>> &open) {
	for (const char *path : { "menus/menu_style.mns", "defs/items.def" }) {
		std::shared_ptr<Document> document = project.open(path);
		if (!document || document->rows().size() < 2)
			return false;
		const bool style = document->kind() == AssetKind::MenuStyle;
		Edit edit;
		edit.address = { document->rows()[1]->id, document->rows()[1]->kind, 0 };
		edit.field = style ? "value" : "id";
		edit.value = style ? Value(std::string("FF00FF")) : Value(int64_t(103));
		Diagnostic error;
		if (!document->apply(edit, error))
			return false;
		open.push_back(document);
	}
	return true;
}

struct PinnedRows {
	const char *name;
	Files (*files)();
	bool edited;
	size_t rows;
	uint64_t digest;
};

} // namespace

// The rows the trunk's validators made, per project: validate_project makes the same (the item
// projects less their cross-table rows, which the review dropped). S13 D10 numbers a model's
// records in the table shape's one order (RecordIds: a LOD's part animations right after the LOD,
// where the model numbered every list first), which moved the fixtures' model rows' child ids and
// nothing else (the same rows with the model's former order give the former digest, 57832e00).
// S17 names a model's material by its place ("armory/Material 1/armry.tga", where its shader tag
// stood), which moved the fixtures' model texture rows' paths and nothing else (c276a18f before). S17's
// review adds one note: the fixture walk.bad's footstep on its end pose, which the game never reads
// (animation.end_pose_trigger; the rows before it gave 54904de7). S18 adds the stylesheet projects' one
// texture finding: their art/logo.tga holds no TGA header, which the game cannot load
// (texture.unloadable; the former rows gave 8a3d7521 and 626d6c86); and drops the fixtures' item's
// missing shadow texture (atvshdw.tga), a name the game stores and never loads (the former 239 rows gave
// 80169f08). The demo round's bug 3 keeps what the game ignores in an item table on a save, so its rows
// no longer say a save drops it ("; saving drops it." gone from the fixture items' eight; 72af48e4 before).
static int test_rows_as_before() {
	const PinnedRows pinned[] = {
		// Master's D-3DI-8 fixture (threedi/synth/person_part9_trigger_scale.3di) adds one row: its
		// texture person.tga, which the project does not have (the 238 rows before it gave 71c5737d).
		// The sound lane adds thirteen warnings: a sound set is a bank's symbol and a sound profile
		// SndProf.def's, so the fixture items' twelve sound_profile and sound_profile_female names and
		// the barrel's sounddeath EXPLO_BARREL are missing in a project with neither (the 239 rows
		// before them gave 6289b82a); and two more, a menu SOUND's trigger being a set of the bank its FILE
		// names, MENU.LWF, which the project does not have (the 252 rows before them gave bbd5cc4e).
		// S19's reserved item ids add four on the fixture items.def: its first row a vehicle
		// (catalog.first_row), its Helicopter and Static Vehicle on the Co-op and deathmatch fallback
		// starts' ids 106001 and 106002 (catalog.reserved_kind), and what the engine keeps 105305 for
		// (catalog.reserved_id; the 254 rows before them gave 8522d767). A model material's shader names
		// the tag a shader registers (ReferenceKind::Shader): the fixture models' 118 material shaders, which
		// no shader of the project registers (it has no .fx), add a warning each (reference.missing; the 258
		// rows before them gave d8ef2f1e). DI-09: a person's launch points name user points of its graphic, so
		// the fixture rifleman's launchups_closeattack mflash01, which shed.3di lacks, adds a warning; a particle
		// effect no file defines is a warning, as the game plays stockeffect's copy for it; a particle slot's
		// point is looked up in its model's first-16 section (the 376 rows before them gave 28f4c1e1).
		{ "fixtures", fixture_files, false, 377, 0xee46e4c294dbbc93ull },
		{ "styles", style_files, false, 15, 0xe39ad4219139a453ull },
		{ "items", item_files, false, 4, 0xc8ca7a0734b9eac6ull },
		{ "open", style_and_item_files, true, 19, 0x80eb2caf43356608ull },
	};
	for (const PinnedRows &pin : pinned) {
		Project project;
		TEST_EXPECT(project.make(pin.files()));
		std::vector<std::shared_ptr<const DocumentBase>> open;
		TEST_EXPECT(!pin.edited || open_edited(project, open));
		AssetGraph graph;
		ValidationCache cache;
		const std::vector<std::string> rows = sorted_rows(validate_project(
				{ project.paths, project.document, project.scan, open }, graph, cache));
		const bool same = rows.size() == pin.rows && digest(rows) == pin.digest;
		std::printf("%s: %zu rows, digest %016llx%s\n", pin.name, rows.size(),
				static_cast<unsigned long long>(digest(rows)), same ? "" : " (not the trunk's)");
		if (!same)
			for (const std::string &row : rows)
				std::printf("  %s\n", row.c_str());
		TEST_EXPECT(same);
	}
	return 0;
}

// What a validation reads and makes (ValidationStats, GraphStats): every file's own findings
// once; nothing again while nothing changed; an open document's from the document, reading no
// file; one edit of it that document alone, and the graph extracting it alone; a closed file
// changed on disk read again alone; and no closed file's document alive once its findings are
// made.
static int test_what_a_validation_reads() {
	Project project;
	TEST_EXPECT(project.make(style_and_item_files()));
	AssetGraph graph;
	ValidationCache cache;
	std::vector<std::shared_ptr<const DocumentBase>> open;
	const auto validate = [&] {
		return validate_project(
				{ project.paths, project.document, project.scan, open }, graph, cache);
	};
	const ValidationStats &stats = cache.stats();
	// Three stylesheets, a menu, two item tables, a weapon table, an ammo table, a texture (S18: a
	// texture's own findings are its file's) and a font (S23: a document).
	const size_t files = validation_files(project.scan).size();
	TEST_EXPECT(files == 10);
	validate();
	TEST_EXPECT(stats.passes == 1 && stats.files_validated == files &&
			stats.files_loaded == files && stats.files_reused == 0 && stats.files_failed == 0);
	// A guard more than a measure: a cache that kept a closed file's document would hold it alive.
	TEST_EXPECT(cache.documents_alive() == 0);
	validate();
	TEST_EXPECT(stats.passes == 2 && stats.files_validated == 0 && stats.files_loaded == 0 &&
			stats.files_reused == files);
	TEST_EXPECT(graph.stats().files_extracted == 0);
	std::shared_ptr<Document> items = project.open("defs/items.def");
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty())
		return 1;
	open.push_back(items);
	validate();
	TEST_EXPECT(stats.files_validated == 1 && stats.files_loaded == 0 &&
			stats.files_reused == files - 1);
	Edit edit;
	edit.address = { items->rows()[0]->id, items->rows()[0]->kind, 0 };
	edit.field = "hp";
	edit.value = int64_t(7);
	Diagnostic error;
	TEST_EXPECT(items->apply(edit, error));
	validate();
	TEST_EXPECT(stats.files_validated == 1 && stats.files_loaded == 0 &&
			stats.files_reused == files - 1);
	TEST_EXPECT(graph.stats().files_extracted == 1);
	const fs::path extra = fs::path(project.root) / "extra/items.def";
	fs::last_write_time(extra, fs::last_write_time(extra) + std::chrono::hours(1));
	project.rescan();
	validate();
	TEST_EXPECT(stats.files_validated == 1 && stats.files_loaded == 1 &&
			stats.files_reused == files - 1);
	TEST_EXPECT(cache.documents_alive() == 0);
	return 0;
}

// A closed file's findings name their records by the identities a load gives them, which a load
// of the same file gives again: read again (its last write moved) and opened, the file's findings
// are the same rows, their row and child identities included, and each names its record.
static int test_findings_keep_their_records() {
	Project project;
	const std::string menu = "<SCREEN>\r\n<NAME>S</NAME>\r\n" +
			window("A",
					window("B", "") + window("B", "") + "<ACTION TYPE=\"NOPE\">X</ACTION>\r\n") +
			"</SCREEN>\r\n";
	TEST_EXPECT(project.make({ { "menus/nested.mnu", menu } }));
	AssetGraph graph;
	ValidationCache cache;
	std::vector<std::shared_ptr<const DocumentBase>> open;
	const auto own = [&] {
		std::vector<Diagnostic> out;
		for (const Diagnostic &d : validate_project(
					 { project.paths, project.document, project.scan, open }, graph, cache))
			if (d.asset == "menus/nested.mnu" && d.code().rfind("menu.", 0) == 0)
				out.push_back(d);
		return out;
	};
	const std::vector<Diagnostic> first = own();
	TEST_EXPECT(first.size() == 2 && first[0].code() == "menu.duplicate_window" &&
			first[1].code() == "menu.action_inert");
	if (first.size() != 2)
		return 1;
	TEST_EXPECT(first[0].child_id != 0 && first[1].child_id != 0 &&
			first[0].child_id != first[1].child_id);
	const fs::path file = fs::path(project.root) / "menus/nested.mnu";
	fs::last_write_time(file, fs::last_write_time(file) + std::chrono::hours(1));
	project.rescan();
	TEST_EXPECT(own() == first && cache.stats().files_loaded == 1);
	std::shared_ptr<Document> document = project.open("menus/nested.mnu");
	TEST_EXPECT(document != nullptr);
	if (!document)
		return 1;
	open.push_back(document);
	TEST_EXPECT(own() == first && cache.stats().files_validated == 1 &&
			cache.stats().files_loaded == 0);
	for (const Diagnostic &d : first)
		TEST_EXPECT(document->record_path({ d.row_id, d.record_kind, d.child_id }) == d.record);
	return 0;
}

// The cross-file checks are a table (graph/use_checks): one row per asset kind that has one, the
// stylesheet's, on a kind a document type opens (the item table's is gone: two item tables are
// two files of one name).
// S13 A6: every finding the four projects make, composed as the editor and the command line
// compose them (the scan's, the requirements', each file's own, the use checks', the graph's and
// the render check's notes), carries the token of a row of the finding codes' tables.
static int test_every_finding_has_a_row() {
	const struct {
		const char *name;
		Files (*files)();
		bool edited;
	} projects[] = {
		{ "fixtures", fixture_files, false },
		{ "styles", style_files, false },
		{ "items", item_files, false },
		{ "open", style_and_item_files, true },
	};
	size_t findings = 0;
	std::set<std::string> codes;
	for (const auto &made : projects) {
		Project project;
		TEST_EXPECT(project.make(made.files()));
		std::vector<std::shared_ptr<const DocumentBase>> open;
		TEST_EXPECT(!made.edited || open_edited(project, open));
		const RequirementReport requirements = evaluate_requirements(project.document, project.scan);
		AssetGraph graph;
		ValidationCache cache;
		ProjectChecks checks;
		ProjectAssetSource files;
		files.set_scan(project.paths.root, project.scan, project.document.target_game);
		const std::vector<std::string> boot_missing;
		const std::vector<Diagnostic> none;
		const ProjectFindings composed = compose_project_findings(
				{ project.paths, project.document, project.scan, requirements, open, boot_missing,
						none, none, none },
				graph, cache, checks, files);
		TEST_EXPECT(!composed.rows.empty());
		findings += composed.rows.size();
		TEST_EXPECT(check_rows(made.name, composed.rows, project.scan, codes) == 0);
	}
	std::printf("every finding has a row: %zu findings of %zu codes\n", findings, codes.size());
	return 0;
}

static int test_use_check_table() {
	size_t rows = 0;
	for (size_t k = 0; k < kAssetKindCount; ++k) {
		const UseCheckRow *row = use_check(static_cast<AssetKind>(k));
		if (!row)
			continue;
		++rows;
		TEST_EXPECT(row->kind == static_cast<AssetKind>(k) && row->check &&
				document_type_for(row->kind));
	}
	// The textures' check (S18), the gametext keys a single-player mission reads (the mission authoring
	// round), the stylesheet's unused-variable check and the mission's pool check (S14).
	TEST_EXPECT(rows == 4 && use_check(AssetKind::Texture) && use_check(AssetKind::Strings) && use_check(AssetKind::MenuStyle) &&
	            use_check(AssetKind::Mission) && !use_check(AssetKind::ItemDefs));
	return 0;
}

// The gametext keys a single-player mission's flow reads (graph/use_checks): with no mission, none is
// asked; with one, each the table's first section of its name lacks, or holds empty, a warning on
// gametext.bin: of the ten, one present, one empty, one only in a second section of its name (no lookup
// reads it [orig: TextResource_FindEntryBySectionAndKey @ 0x75D250]).
static int test_gametext_flow_keys() {
	rtxt::File table;
	const auto section = [&table](const char *name, std::vector<std::pair<const char *, const char *>> rows) {
		const uint32_t index = static_cast<uint32_t>(table.sections.size());
		table.sections.push_back({ name, static_cast<uint32_t>(rows.size()) });
		for (const auto &row : rows)
			table.entries.push_back({ row.first, row.second, {}, index });
	};
	section("Overlays", { { "STROVER_MISSION_FAILED", "Mission failed" } });
	section("Epilog", { { "STREPILOG_KEYINFO", "" } });
	section("overlays", { { "STROVER_MISSIONOBJECTIVES", "Objectives" } });
	std::vector<uint8_t> bytes;
	std::string error;
	TEST_EXPECT(rtxt::write(table, bytes, error));
	bool made = true;
	const auto flow_rows = [&](bool mission) {
		Project project;
		made = made && project.make({}) && editor_test::write_bytes(project.root + "/strings/gametext.bin", bytes);
		if (mission)
			made = made && editor_test::write_bytes(project.root + "/missions/synth_logic.bms",
					test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms"));
		project.rescan();
		AssetGraph graph;
		ValidationCache cache;
		const std::vector<std::shared_ptr<const DocumentBase>> open;
		std::vector<Diagnostic> rows;
		for (const Diagnostic &d : validate_project({ project.paths, project.document, project.scan, open }, graph, cache))
			if (d.code() == "strings.flow_key_missing") rows.push_back(d);
		return rows;
	};
	TEST_EXPECT(flow_rows(false).empty());
	const std::vector<Diagnostic> rows = flow_rows(true);
	TEST_EXPECT(made && rows.size() == 9);
	size_t empty = 0, shadowed = 0;
	for (const Diagnostic &d : rows) {
		TEST_EXPECT(d.severity == DiagnosticSeverity::Warning && d.asset == "strings/gametext.bin");
		TEST_EXPECT(d.message.find("STROVER_MISSION_FAILED") == std::string::npos);
		empty += d.message.find("Epilog/STREPILOG_KEYINFO, the end screens' key help: it holds no text") != std::string::npos;
		shadowed += d.message.find("Overlays/STROVER_MISSIONOBJECTIVES") != std::string::npos &&
				d.message.find("the table has no such string") != std::string::npos;
	}
	TEST_EXPECT(empty == 1 && shadowed == 1);
	return 0;
}

// A stylesheet line whose name is written as a %NAME% itself is no definition its file reads of
// that name (MnsDocument::winning_row looks a name up by its variable_name), so the stylesheet's
// use checks make nothing of it: FOO, which a menu uses, is used, and "%FOO%" is not unused. The
// line is the file's own finding (style.invalid_name_char: the game stops reading there), which
// asset_graph_test pins.
static int test_style_name_as_reference() {
	Project project;
	TEST_EXPECT(project.make({
			{ "menus/menu_style.mns", "FOO FF00FF00\r\n%FOO% 1\r\n" },
			{ "menus/main.mnu",
					"<SCREEN>\r\n<NAME>MAIN</NAME>\r\n" +
							window("A",
									"<APPEARANCE STATE=\"DEFAULT\" "
									"TYPE=\"COLOR\">%FOO%</APPEARANCE>\r\n") +
							"</SCREEN>\r\n" },
	}));
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	size_t style_rows = 0;
	for (const Diagnostic &d :
			validate_project({ project.paths, project.document, project.scan, open }, graph, cache))
		if (d.code() == "style.unused" || d.code() == "style.overridden_by_brand" ||
				d.code() == "style.not_a_color" || d.code() == "style.mixed_use") {
			std::printf("  %s %s: %s\n", d.code().c_str(), d.record.c_str(), d.message.c_str());
			++style_rows;
		}
	TEST_EXPECT(style_rows == 0);
	return 0;
}

// What a use of a variable is, by the field that names it (S13 D4's reviews): the game expands a
// menu's whole text before its parse, so any text that is one %NAME% is a use of the variable (a
// label's STRING, an ACTION's URL, a window's NAME), and none of those is style.unused; a colour's
// variable a string id's STRING names too is no style.mixed_use (a string id is none of a colour,
// a font and an image). A text's %NAME% no stylesheet defines is the graph's finding on its field,
// which the field's reference status reads as the variable's.
static int test_style_uses_by_what_names_them() {
	Project project;
	const auto button = [](const std::string &name, const std::string &body) {
		return "<WINDOW TYPE=\"BUTTON\" NAME=\"" + name +
				"\">\r\n<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>90</RIGHT><BOTTOM>20</BOTTOM>"
				"</POSITION>\r\n" +
				body + "</WINDOW>\r\n";
	};
	TEST_EXPECT(project.make({
			{ "menus/menu_style.mns",
					"LABEL_TEXT Hello\r\nFG FF00FF00\r\nHOME_URL http://x/\r\nPANEL PANEL_A\r\n" },
			{ "menus/main.mnu",
					"<SCREEN>\r\n<NAME>MAIN</NAME>\r\n" +
							window("A", "<STRING>%LABEL_TEXT%</STRING>\r\n") +
							window("B",
									"<APPEARANCE STATE=\"DEFAULT\" "
									"TYPE=\"COLOR\">%FG%</APPEARANCE>\r\n") +
							window("C", "<STRING TYPE=\"ID\">%FG%</STRING>\r\n") +
							window("D", "<STRING>%NOPE%</STRING>\r\n") +
							button("WEB", "<ACTION TYPE=\"URL\">%HOME_URL%</ACTION>\r\n") +
							window("%PANEL%", "") +
							button("OPEN",
									"<ACTION TYPE=\"WINDOW\" STATE=\"SHOW\">%PANEL%</ACTION>\r\n") +
							"</SCREEN>\r\n" },
	}));
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const std::vector<Diagnostic> rows =
			validate_project({ project.paths, project.document, project.scan, open }, graph, cache);
	size_t style_rows = 0, nope = 0;
	for (const Diagnostic &d : rows) {
		if (d.code() == "style.unused" || d.code() == "style.mixed_use" ||
				d.code() == "style.not_a_color") {
			std::printf("  %s %s: %s\n", d.code().c_str(), d.record.c_str(), d.message.c_str());
			++style_rows;
		}
		if (d.code() == "reference.missing" && d.record == "MAIN/D") {
			++nope;
			TEST_EXPECT(d.field == "string.value" && editor_test::reference_of(d).kind == ReferenceKind::StyleVar &&
					d.severity == DiagnosticSeverity::Warning &&
					d.message.find("%NOPE%") != std::string::npos);
		}
	}
	TEST_EXPECT(style_rows == 0 && nope == 1);
	// Each text is the variable's edge through the text.
	const auto through_text = [&graph](const char *name, const char *field) {
		for (const GraphEdge *edge : graph.referrers_of(ReferenceKind::StyleVar, name))
			if (edge->through == ReferenceKind::MenuText && edge->field == field)
				return true;
		return false;
	};
	TEST_EXPECT(through_text("LABEL_TEXT", "string.value") && through_text("HOME_URL", "target") &&
			through_text("PANEL", "name"));
	// D's STRING references the variable it names: missing, as the Problems row says.
	std::shared_ptr<Document> menu = project.open("menus/main.mnu");
	NodeAddress d;
	TEST_EXPECT(menu && find_definition(graph, *menu, "D", d));
	if (!menu)
		return 1;
	bool checked = false;
	for (const FieldSchema &schema : menu->fields(d.kind)) {
		if (schema.id != "string.value")
			continue;
		const FieldUse use = menu->field_on(d, schema);
		Value value;
		TEST_EXPECT(menu->get(d, schema.id, value));
		TEST_EXPECT(use.variable_through == ReferenceKind::MenuText &&
				value_reference(use, value) == ReferenceKind::StyleVar &&
				reference_status(graph, use, value) == ReferenceStatus::Missing);
		checked = true;
	}
	TEST_EXPECT(checked);
	return 0;
}

// Item ids within a table (S13 D4's review): two item tables are two files of one name, of which
// the game reads one (the scan's asset.name.duplicate), so an id is compared within its table
// alone, as the load compares them: a repeat names the table's first item of the id, an id of 0
// too (the load logs "Duplicate ID number" for each); an id an earlier table has is no finding (it
// was, before the review); a table a source error blocks reports that alone.
static int test_item_ids_within_a_table() {
	Project project;
	TEST_EXPECT(project.make({
			{ "a/items.def", item("Alpha", 5) + item("Zero", 0) },
			{ "b/items.def",
					item("Bravo", 5) + item("Bravo2", 5) + item("Nil", 0) + item("Nil2", 0) },
			{ "c/items.def", "begin \"Broken\"\n  id 7\n  type marker\n  hp twelve\nend\n" },
			{ "d/items.def", item("Delta", 7) },
	}));
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	std::vector<std::string> ids;
	bool blocked = false;
	for (const Diagnostic &d : validate_project(
				 { project.paths, project.document, project.scan, open }, graph, cache)) {
		if (d.code() == "catalog.item_identity")
			ids.push_back(d.asset + " " + d.record + " " + d.field + ": " + d.message);
		blocked = blocked || (d.code() == "catalog.invalid_input" && d.asset == "c/items.def");
	}
	const std::string tail = ": the game keeps both, and a lookup by the id finds the earlier one.";
	TEST_EXPECT(ids ==
			std::vector<std::string>({
					"b/items.def Bravo2 id: An earlier item, \"Bravo\", has id 5" + tail,
					"b/items.def Nil2 id: An earlier item, \"Nil\", has id 0" + tail,
			}));
	TEST_EXPECT(blocked && !cache.records_checked("c/items.def") &&
			cache.records_checked("d/items.def"));
	TEST_EXPECT(std::any_of(project.scan.diagnostics.begin(), project.scan.diagnostics.end(),
			[](const Diagnostic &d) { return d.code() == "asset.name.duplicate"; }));
	return 0;
}

// The retail leg (OPENNOVA_JO_DIR): the install's files of every kind a document type opens and
// validates (its catalogs, string tables, menus, stylesheet, models, clips and animation tables; its
// textures are the texture checks' retail leg's) exported into a
// project and validated: every file's own findings made once, and no closed file's document
// alive after (the models' geometry included); nothing made again while nothing changed; an edit
// of the open item table validates that table alone and the graph extracts it alone.
static int test_retail_validation() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the JO install's files validated a file at a time)");
		return 0;
	}
	Project project;
	TEST_EXPECT(project.make({}));
	ImportOrigin origin;
	std::string error;
	TEST_EXPECT(origin.open(ImportOrigin::Kind::GameInstall, install, project.document, error));
	InstallView view;
	TEST_EXPECT(view.open(install_spec(install, project.document), error));
	const opennova::Vfs &mount = view.vfs();
	size_t exported = 0, models = 0;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		if (opennova::strutil::ends_with_icase(name, ".pff"))
			continue;
		// The install's textures are the texture checks' retail leg's (editor_texture_roles): left out here.
		const AssetKind kind = origin.file_kind(name);
		const DocumentType *type = document_type_for(kind);
		if (!type || type->id == DocumentTypeId::Texture)
			continue;
		std::vector<uint8_t> bytes;
		if (!origin.read(name, bytes))
			continue;
		TEST_EXPECT(editor_test::write_bytes(
				project.root + "/" + asset_kind_token(kind) + "/" + name, bytes));
		++exported;
		models += kind == AssetKind::Model ? 1 : 0;
	}
	project.rescan();
	const size_t files = validation_files(project.scan).size();
	TEST_EXPECT(exported > 0 && files == exported && models > 0);
	AssetGraph graph;
	ValidationCache cache;
	std::vector<std::shared_ptr<const DocumentBase>> open;
	const ValidationStats &stats = cache.stats();
	const auto timed = [&](size_t &rows) {
		const auto start = std::chrono::steady_clock::now();
		rows = validate_project(
				{ project.paths, project.document, project.scan, open }, graph, cache)
					   .size();
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
				.count();
	};
	size_t rows = 0;
	const double first = timed(rows);
	std::printf("retail: %zu files a document type opens exported (%zu models), %zu rows; the "
				"first validation "
				"made %zu files' findings, read %zu (%zu did not load) in %.0f ms, %zu documents "
				"alive after\n",
			exported, models, rows, stats.files_validated, stats.files_loaded, stats.files_failed,
			first, cache.documents_alive());
	TEST_EXPECT(stats.files_validated == files && stats.files_loaded == files &&
			cache.documents_alive() == 0);
	const double again = timed(rows);
	std::printf("retail: nothing changed: %zu files' findings kept, %zu read, in %.1f ms\n",
			stats.files_reused, stats.files_loaded, again);
	TEST_EXPECT(stats.files_validated == 0 && stats.files_loaded == 0 &&
			stats.files_reused == files && graph.stats().files_extracted == 0);
	// The rows composed again while nothing moved (the use checks among them; the editor composes
	// them after every edit that validates a file), timed, not held to a bound.
	{
		size_t missions = 0;
		for (const AssetEntry &asset : project.scan.entries) missions += asset.kind == AssetKind::Mission ? 1 : 0;
		const auto start = std::chrono::steady_clock::now();
		const size_t composed = project_rows({ project.paths, project.document, project.scan, open }, graph, cache).size();
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		std::vector<Diagnostic> uses;
		const auto checks_start = std::chrono::steady_clock::now();
		run_use_checks(graph, cache, { project.paths, project.document, project.scan, open }, uses);
		std::printf("retail: the rows composed again over %zu missions, nothing moved: %zu in %.2f ms, the use checks' "
				"%zu in %.2f ms\n",
				missions, composed, ms, uses.size(),
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - checks_start).count());
		TEST_EXPECT(composed == rows);
	}
	// S13 A6: every finding the install's files make, composed as the editor composes them (the
	// project checks' among them, the render check's notes over the install's menus), keeps a
	// table's row.
	{
		const RequirementReport requirements = evaluate_requirements(project.document, project.scan);
		ProjectChecks checks;
		ProjectAssetSource source;
		source.set_scan(project.paths.root, project.scan, project.document.target_game);
		const std::vector<std::string> boot_missing;
		const std::vector<Diagnostic> none;
		const ProjectFindings composed = compose_project_findings(
				{ project.paths, project.document, project.scan, requirements, open, boot_missing,
						none, none, none },
				graph, cache, checks, source);
		std::set<std::string> codes;
		TEST_EXPECT(check_rows("retail", composed.rows, project.scan, codes) == 0);
		std::printf("retail: every finding has a row: %zu findings of %zu codes\n",
				composed.rows.size(), codes.size());
	}
	const AssetEntry *items_entry = project.scan.find("items.def");
	TEST_EXPECT(items_entry != nullptr);
	if (!items_entry)
		return 1;
	std::shared_ptr<Document> items = project.open(items_entry->relative_path);
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty())
		return 1;
	open.push_back(items);
	timed(rows);
	Edit edit;
	edit.address = { items->rows()[0]->id, items->rows()[0]->kind, 0 };
	edit.field = "hp";
	edit.value = int64_t(12345);
	Diagnostic edit_error;
	TEST_EXPECT(items->apply(edit, edit_error));
	const double edited = timed(rows);
	std::printf("retail: an edit of the open item table: %zu file's findings made, %zu kept, %zu "
				"read, the graph "
				"extracting %zu, in %.1f ms\n",
			stats.files_validated, stats.files_reused, stats.files_loaded,
			graph.stats().files_extracted, edited);
	TEST_EXPECT(stats.files_validated == 1 && stats.files_loaded == 0 &&
			stats.files_reused == files - 1 && graph.stats().files_extracted == 1);
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_rows_as_before();
	failures += test_every_finding_has_a_row();
	failures += test_what_a_validation_reads();
	failures += test_findings_keep_their_records();
	failures += test_use_check_table();
	failures += test_gametext_flow_keys();
	failures += test_style_name_as_reference();
	failures += test_style_uses_by_what_names_them();
	failures += test_item_ids_within_a_table();
	failures += test_retail_validation();
	return failures == 0 ? 0 : 1;
}
