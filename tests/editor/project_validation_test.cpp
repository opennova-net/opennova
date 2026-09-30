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
// validation, one with nothing changed and one after an edit of the open item table.
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/project_validation.h>
#include <editor/graph/use_checks.h>
#include <editor/import/import_plan.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"

using namespace opennova;
using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

std::string repo() {
	return std::string(test_paths_repo_root(__FILE__));
}

std::string text_of(const std::vector<uint8_t> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

// A finding as one line, every member of it (its reference kind by its token, which a new kind
// leaves as it is).
std::string row_of(const Diagnostic &d) {
	return std::string(diagnostic_severity_label(d.severity)) + "|" + d.code + "|" + d.asset + "|" +
			std::to_string(d.line) + "|" + d.record + "|" + d.field + "|" +
			std::to_string(d.row_id) + "|" + std::to_string(d.child_id) + "|" +
			std::to_string(d.record_kind) + "|" + d.role + "|" + d.target + "|" +
			reference_row(d.reference).token + "|" + d.scope + "|" + std::to_string(d.loader_arg) +
			"|" + d.message;
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

using Files = std::vector<std::pair<std::string, std::string>>;

// Every fixture a document type reads: the item table, the string tables, the menus, the
// stylesheet (as menu_style.mns, the one the game reads), the models, the clips and the tables.
Files fixture_files() {
	Files files;
	const std::string root = repo() + "/fixtures/";
	const auto add_dir = [&](const std::string &dir, const std::string &to, const char *extension,
								 bool as_style = false) {
		std::vector<fs::path> found;
		std::error_code ec;
		for (const auto &entry : fs::directory_iterator(root + dir, ec))
			if (entry.is_regular_file(ec) && entry.path().extension() == extension)
				found.push_back(entry.path());
		std::sort(found.begin(), found.end());
		for (const fs::path &path : found)
			files.push_back({ to +
							(as_style ? std::string("menu_style.mns")
									  : path.filename().generic_string()),
					text_of(test_io::read_file(path.generic_string())) });
	};
	add_dir("def", "defs/", ".def");
	add_dir("rtxt", "strings/", ".bin");
	add_dir("mnu", "menus/", ".mnu");
	add_dir("mns", "menus/", ".mns", true);
	add_dir("threedi/synth", "models/", ".3di");
	add_dir("anim", "anims/", ".bad");
	add_dir("anim", "anims/", ".adm");
	return files;
}

std::string window(const std::string &name, const std::string &body) {
	return "<WINDOW TYPE=\"STATIC\" NAME=\"" + name +
			"\">\r\n<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>90</RIGHT><BOTTOM>90</BOTTOM></"
			"POSITION>\r\n" +
			body + "</WINDOW>\r\n";
}

// The stylesheet's cross-file checks: the shell's two stylesheets and one the game never reads,
// and a menu naming their variables as colours, a font and an image.
Files style_files() {
	return {
		{ "menus/menu_style.mns",
				"// the shell\r\nCOL_OK FF00FF00\r\nCOL_BAD red\r\nFONT_A arial.fnt\r\nIMG_A "
				"logo.tga\r\n"
				"MIXED FF112233\r\nUNUSED_V 1\r\nDUP 1\r\nDUP 2\r\nXML <b>\r\nNESTED %COL_OK%\r\n"
				"SLASH a\\\\b\r\nBRANDED 11\r\nALSO FF000000\r\n" },
		{ "menus/brand.mns", "BRANDED FF222222\r\nNEWONE 5\r\n" },
		{ "menus/other.mns", "COL_OK 1\r\nLOST 2\r\n" },
		{ "menus/main.mnu",
				"<SCREEN>\r\n<NAME>MAIN</NAME>\r\n" +
						window("A",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"COLOR\">%COL_OK%</APPEARANCE>\r\n"
								"<FONT><NAME>%FONT_A%</NAME><DEFAULT_FG>%COL_BAD%</DEFAULT_FG></"
								"FONT>\r\n") +
						window("B",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"IMAGE\">%IMG_A%</APPEARANCE>\r\n"
								"<FONT><NAME>%MIXED%</NAME><DEFAULT_FG>%MIXED%</DEFAULT_FG></"
								"FONT>\r\n") +
						window("C",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"COLOR\">%BRANDED%</APPEARANCE>\r\n"
								"<FONT><DEFAULT_FG>%DUP%</DEFAULT_FG><DEFAULT_BG>%NEWONE%</"
								"DEFAULT_BG></FONT>\r\n") +
						window("D",
								"<APPEARANCE STATE=\"DEFAULT\" "
								"TYPE=\"COLOR\">%XML%</APPEARANCE>\r\n") +
						"</SCREEN>\r\n" },
		{ "fonts/arial.fnt", "fnt" },
		{ "art/logo.tga", "tga" },
	};
}

std::string item(const std::string &name, int id) {
	return "begin \"" + name + "\"\n  id " + std::to_string(id) +
			"\n  type marker\n  hp 0\nend\n\n";
}

// The item ids: repeated inside a table (0 among them), and a second table repeating the
// first's (no finding: the game reads one of the two); the weapon and ammo names repeated in
// their tables.
Files item_files() {
	return {
		{ "defs/items.def",
				item("Alpha", 100) + item("Bravo", 101) + item("Charlie", 100) + item("Delta", 0) +
						item("Echo", 0) + item("Foxtrot", 102) },
		{ "extra/items.def", item("Golf", 102) + item("Hotel", 103) + item("India", 101) },
		{ "defs/weapon.def",
				"weapon \"WPN_A\"\ncategory 3\nend\nweapon \"wpn_a\"\ncategory 4\nend\n" },
		{ "defs/ammo.def", "ammo AMMO_A\nvelocity 900\nend\nammo AMMO_A\nvelocity 800\nend\n" },
	};
}

// The stylesheets' and the item tables' files together (the case with open documents).
Files style_and_item_files() {
	Files files = style_files();
	for (auto &file : item_files())
		files.push_back(file);
	return files;
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
		std::shared_ptr<Document> made = document_type_for(asset->kind)->make();
		Diagnostic error;
		return made->load(root + "/" + path, path, asset->kind, document.target_game, error)
				? made
				: nullptr;
	}
};

// The shell's stylesheet and the first item table open, each with an edit of its second row.
bool open_edited(const Project &project, std::vector<std::shared_ptr<const Document>> &open) {
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
// projects less their cross-table rows, which the review dropped).
static int test_rows_as_before() {
	const PinnedRows pinned[] = {
		{ "fixtures", fixture_files, false, 238, 0x57832e00899f75e7ull },
		{ "styles", style_files, false, 14, 0x8a3d7521d4f40d63ull },
		{ "items", item_files, false, 4, 0xc8ca7a0734b9eac6ull },
		{ "open", style_and_item_files, true, 18, 0x626d6c867164ff5aull },
	};
	for (const PinnedRows &pin : pinned) {
		Project project;
		TEST_EXPECT(project.make(pin.files()));
		std::vector<std::shared_ptr<const Document>> open;
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
	std::vector<std::shared_ptr<const Document>> open;
	const auto validate = [&] {
		return validate_project(
				{ project.paths, project.document, project.scan, open }, graph, cache);
	};
	const ValidationStats &stats = cache.stats();
	// Three stylesheets, a menu, two item tables, a weapon table and an ammo table.
	const size_t files = validation_files(project.scan).size();
	TEST_EXPECT(files == 8);
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
	std::vector<std::shared_ptr<const Document>> open;
	const auto own = [&] {
		std::vector<Diagnostic> out;
		for (const Diagnostic &d : validate_project(
					 { project.paths, project.document, project.scan, open }, graph, cache))
			if (d.asset == "menus/nested.mnu" && d.code.rfind("menu.", 0) == 0)
				out.push_back(d);
		return out;
	};
	const std::vector<Diagnostic> first = own();
	TEST_EXPECT(first.size() == 2 && first[0].code == "menu.duplicate_window" &&
			first[1].code == "menu.action_inert");
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
	TEST_EXPECT(rows == 1 && use_check(AssetKind::MenuStyle) && !use_check(AssetKind::ItemDefs));
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
	const std::vector<std::shared_ptr<const Document>> open;
	size_t style_rows = 0;
	for (const Diagnostic &d :
			validate_project({ project.paths, project.document, project.scan, open }, graph, cache))
		if (d.code == "style.unused" || d.code == "style.overridden_by_brand" ||
				d.code == "style.not_a_color" || d.code == "style.mixed_use") {
			std::printf("  %s %s: %s\n", d.code.c_str(), d.record.c_str(), d.message.c_str());
			++style_rows;
		}
	TEST_EXPECT(style_rows == 0);
	return 0;
}

// What a use of a variable is, by the field that names it (S13 D4's review): a label's text that
// is one %NAME% (a STRING of no id type) is a use of the variable, whose value the game shows
// there, so the variable is no style.unused; a colour's variable a string id's STRING names too is
// no style.mixed_use (a string id is none of a colour, a font and an image).
static int test_style_uses_by_what_names_them() {
	Project project;
	TEST_EXPECT(project.make({
			{ "menus/menu_style.mns", "LABEL_TEXT Hello\r\nFG FF00FF00\r\n" },
			{ "menus/main.mnu",
					"<SCREEN>\r\n<NAME>MAIN</NAME>\r\n" +
							window("A", "<STRING>%LABEL_TEXT%</STRING>\r\n") +
							window("B",
									"<APPEARANCE STATE=\"DEFAULT\" "
									"TYPE=\"COLOR\">%FG%</APPEARANCE>\r\n") +
							window("C", "<STRING TYPE=\"ID\">%FG%</STRING>\r\n") +
							"</SCREEN>\r\n" },
	}));
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const Document>> open;
	size_t style_rows = 0;
	for (const Diagnostic &d :
			validate_project({ project.paths, project.document, project.scan, open }, graph, cache))
		if (d.code == "style.unused" || d.code == "style.mixed_use" ||
				d.code == "style.not_a_color") {
			std::printf("  %s %s: %s\n", d.code.c_str(), d.record.c_str(), d.message.c_str());
			++style_rows;
		}
	TEST_EXPECT(style_rows == 0);
	// The label's text is the variable's edge through the shown text.
	const std::vector<const GraphEdge *> uses =
			graph.referrers_of(ReferenceKind::StyleVar, "LABEL_TEXT");
	TEST_EXPECT(uses.size() == 1 && uses[0]->through == ReferenceKind::MenuText &&
			uses[0]->field == "string.value");
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
	const std::vector<std::shared_ptr<const Document>> open;
	std::vector<std::string> ids;
	bool blocked = false;
	for (const Diagnostic &d : validate_project(
				 { project.paths, project.document, project.scan, open }, graph, cache)) {
		if (d.code == "catalog.item_identity")
			ids.push_back(d.asset + " " + d.record + " " + d.field + ": " + d.message);
		blocked = blocked || (d.code == "catalog.invalid_input" && d.asset == "c/items.def");
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
			[](const Diagnostic &d) { return d.code == "asset.name.duplicate"; }));
	return 0;
}

// The retail leg (OPENNOVA_JO_DIR): the install's files of every kind a document type opens (its
// catalogs, string tables, menus, stylesheet, models, clips and animation tables) exported into a
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
	opennova::Vfs mount;
	TEST_EXPECT(mount_retail(mount, install, project.document));
	size_t exported = 0, models = 0;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		if (opennova::strutil::ends_with_icase(name, ".pff"))
			continue;
		const AssetKind kind = origin.file_kind(name);
		if (!document_type_for(kind))
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
	std::vector<std::shared_ptr<const Document>> open;
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
	failures += test_what_a_validation_reads();
	failures += test_findings_keep_their_records();
	failures += test_use_check_table();
	failures += test_style_name_as_reference();
	failures += test_style_uses_by_what_names_them();
	failures += test_item_ids_within_a_table();
	failures += test_retail_validation();
	return failures == 0 ? 0 : 1;
}
