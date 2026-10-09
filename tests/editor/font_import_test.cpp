// The font importer (editor/import/font_import): a font set's glyph sheet made into the .fnt the game
// reads, through formats/fnt's make_font_from_sheet (whose advance modes, pages and refusals are master's
// fnt_sheet ctest) and read back by fnt_parse. The set file and its refusals; the options and theirs.
// Through the importer's run over an ImportContext: the output named after the set, an option, the set,
// the sheet that is not there, and one that does not decode, each refused with its finding. Over a
// project: the set an import source whose record lists the sheet as its input, the .fnt a Font the
// build packs into localres, a changed sheet imported again; and an import of the set from a folder
// outside the project bringing the sheet beside it into fonts/, planned and imported, or refused when the
// sheet is not there.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/import/font_import.h>
#include <editor/import/import_context.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/import/sidecar.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/fnt/fnt.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "common/png_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using namespace opennova::fnt;
using opennova::RgbaImage;
namespace fs = std::filesystem;

namespace {

constexpr int kCellW = 8, kCellH = 10;

// A transparent sheet of 16 x 14 cells of `cell_w` x `cell_h`.
RgbaImage blank_sheet(int cell_w = kCellW, int cell_h = kCellH) {
	RgbaImage sheet;
	sheet.width = cell_w * kFontSheetColumns;
	sheet.height = cell_h * kFontSheetRows;
	sheet.pixels.assign(size_t(sheet.width) * size_t(sheet.height) * 4, 0);
	return sheet;
}

// Columns [x0, x1] and rows [y0, y1] of byte `byte`'s cell inked, red at `alpha`.
void ink(RgbaImage &sheet, int byte, int x0, int x1, int y0, int y1, uint8_t alpha = 255) {
	const int cell_w = sheet.width / kFontSheetColumns, cell_h = sheet.height / kFontSheetRows;
	const int cx = ((byte - 0x20) % kFontSheetColumns) * cell_w, cy = ((byte - 0x20) / kFontSheetColumns) * cell_h;
	for (int y = y0; y <= y1; ++y)
		for (int x = x0; x <= x1; ++x) {
			uint8_t *p = &sheet.pixels[(size_t(cy + y) * size_t(sheet.width) + size_t(cx + x)) * 4];
			p[0] = 200;
			p[1] = 10;
			p[2] = 10;
			p[3] = alpha;
		}
}

// The test sheet: 'A' five columns (1..5), 'I' one (3), '.' two at half alpha (2..3, its lowest rows),
// 'W' the whole cell, the space's and 0x7F's cells drawn on (neither is read), 0xFF two columns (0..1).
RgbaImage test_sheet() {
	RgbaImage sheet = blank_sheet();
	ink(sheet, 'A', 1, 5, 1, 8);
	ink(sheet, 'I', 3, 3, 1, 8);
	ink(sheet, '.', 2, 3, 8, 9, 128);
	ink(sheet, 'W', 0, 7, 1, 8);
	ink(sheet, ' ', 0, 7, 0, 9);
	ink(sheet, 0x7F, 0, 7, 0, 9);
	ink(sheet, 0xFF, 0, 1, 0, 9);
	return sheet;
}

// The sheet as a PNG (RGBA, 8 bits).
std::vector<uint8_t> sheet_png(const RgbaImage &sheet) {
	test_png::PngSpec spec;
	spec.width = uint32_t(sheet.width);
	spec.height = uint32_t(sheet.height);
	for (int y = 0; y < sheet.height; ++y) {
		spec.rows.push_back(0);
		const auto row = sheet.pixels.begin() + std::ptrdiff_t(size_t(y) * size_t(sheet.width) * 4);
		spec.rows.insert(spec.rows.end(), row, row + std::ptrdiff_t(sheet.width) * 4);
	}
	return test_png::make_png(spec);
}

std::vector<uint8_t> text_bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

int width_of(const fnt_font_t &font, int byte) {
	int w = 0, h = 0;
	fnt_get_glyph_size(fnt_get_glyph(&font, uint8_t(byte)), &w, &h);
	return w;
}
size_t count_code(const std::vector<Diagnostic> &diagnostics, const std::string &code) {
	size_t n = 0;
	for (const Diagnostic &d : diagnostics) n += d.code() == code ? 1 : 0;
	return n;
}

} // namespace

static int test_font_set() {
	FontSet set;
	std::string why;
	// Comments, CRLF, a tab, a key in any case; the last sheet line is the one.
	TEST_EXPECT(parse_font_set(text_bytes("; HUD digits\r\n\r\nSHEET\tfirst.png\r\nsheet  art/Hud 14.png ; the glyphs\r\n"), set, why));
	TEST_EXPECT(set.sheet == "art/Hud 14.png");
	std::vector<std::string> inputs;
	font_set_inputs(text_bytes("sheet glyphs.png\n"), inputs);
	TEST_EXPECT(inputs == std::vector<std::string>{"glyphs.png"});
	// No sheet line, a key the set does not take, a sheet naming nothing: refused, nothing named.
	TEST_EXPECT(!parse_font_set(text_bytes("; nothing\n"), set, why) && why.find("names its glyph sheet") != std::string::npos);
	TEST_EXPECT(!parse_font_set(text_bytes("sheet a.png\nheight 12\n"), set, why) && why.find("line 2") != std::string::npos &&
	            why.find("'height'") != std::string::npos);
	TEST_EXPECT(!parse_font_set(text_bytes("sheet\n"), set, why) && why.find("names no file") != std::string::npos);
	font_set_inputs(text_bytes("colour red\n"), inputs);
	TEST_EXPECT(inputs.empty());
	return 0;
}

static int test_options() {
	FontSheetSettings settings;
	std::string why, field;
	TEST_EXPECT(font_import_settings({}, settings, why, field));
	TEST_EXPECT(settings.advance == FontSheetAdvance::Ink && settings.tracking == 1 && settings.space == 0 && settings.spacing == 0 &&
	            settings.design_width == 800);
	TEST_EXPECT(!settings.sheet_color);
	TEST_EXPECT(font_import_settings({{"advance", "LEFT"}, {"tracking", "0"}, {"space", "5"}, {"spacing", "-3"}, {"design_width", "1024"},
	                                  {"color", "sheet"}},
	                                 settings, why, field));
	TEST_EXPECT(settings.advance == FontSheetAdvance::Left && settings.tracking == 0 && settings.space == 5 && settings.spacing == -3 &&
	            settings.design_width == 1024 && settings.sheet_color);
	// "" is the fallback.
	TEST_EXPECT(font_import_settings({{"tracking", ""}}, settings, why, field) && settings.tracking == 1);
	// A key no row has, and a value outside its row: refused, the key in `field`.
	TEST_EXPECT(!font_import_settings({{"kerning", "1"}}, settings, why, field) && field == "kerning");
	for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>>{
	             {"advance", "wide"}, {"tracking", "33"}, {"tracking", "-1"}, {"tracking", "2px"}, {"space", "0"},
	             {"space", "255"}, {"spacing", "17"}, {"spacing", "-17"}, {"design_width", "0"}, {"design_width", "4097"},
	             {"color", "red"}}) {
		TEST_EXPECT(!font_import_settings({{key, value}}, settings, why, field) && field == key);
		TEST_EXPECT(why.find("takes") != std::string::npos);
	}
	// The rows the import_options query lists: tracking and space only under ink and left.
	const std::vector<ImportOptionRow> &rows = font_import_option_rows();
	TEST_EXPECT(rows.size() == 6);
	TEST_EXPECT(import_option_row(rows, "color") && import_option_row(rows, "color")->fallback == "white" &&
	            import_option_row(rows, "color")->values.size() == 2 && import_option_row(rows, "color")->applies_to.empty());
	const ImportOptionRow *tracking = import_option_row(rows, "tracking");
	TEST_EXPECT(tracking && tracking->fallback == "1" && tracking->applies_to == "advance" &&
	            tracking->applies_values == std::vector<std::string>({"ink", "left"}));
	const ImportOptionRow *space = import_option_row(rows, "space");
	TEST_EXPECT(space && space->fallback.empty() && space->applies_to == "advance");
	TEST_EXPECT(import_option_row(rows, "advance")->fallback == "ink" && import_option_row(rows, "spacing")->fallback == "0" &&
	            import_option_row(rows, "design_width")->fallback == "800");
	return 0;
}

// The importer's run over an ImportContext, as the import pass calls it.
static int test_import_run() {
	editor_test::TempProjectDir dir("opennova_editor_font_import_run");
	const std::string root = dir.root();
	const std::string folder = root + "/fonts";
	TEST_EXPECT(editor_test::write_bytes(folder + "/Hud14.png", sheet_png(test_sheet())));
	const auto run = [&](const std::string &name, const std::string &set, const ImportOptions &options, ImportProduct &product,
	                     std::vector<Diagnostic> &context_findings) {
		const std::vector<uint8_t> bytes = text_bytes(set);
		ImportContext context(name, bytes, options, folder, root);
		const bool ok = run_font_import(context, product);
		context_findings = context.findings();
		return ok ? 0 : 1;
	};
	ImportProduct product;
	std::vector<Diagnostic> findings;
	TEST_EXPECT(run("Hud14.fntset", "sheet Hud14.png\n", {{"spacing", "-2"}}, product, findings) == 0);
	TEST_EXPECT(findings.empty() && product.diagnostics.empty() && product.outputs.size() == 1);
	TEST_EXPECT(product.outputs[0].name == "Hud14.fnt");
	fnt_font_t font{};
	TEST_EXPECT(fnt_parse(product.outputs[0].bytes.data(), product.outputs[0].bytes.size(), &font) == FNT_OK);
	const bool read_back = width_of(font, 'A') == 6 && font.glyph_spacing == -2;
	fnt_free(&font);
	TEST_EXPECT(read_back);
	// Refused, each with its finding: an option no row takes (import.option, its key the field), a set
	// with no sheet or with a key it does not take (import.font), a sheet that is not there
	// (import.input, the context's), one that does not decode (import.decode), a stem too long for an
	// archive's name (import.font).
	product = ImportProduct();
	TEST_EXPECT(run("Hud14.fntset", "sheet Hud14.png\n", {{"tracking", "99"}}, product, findings) == 1);
	TEST_EXPECT(count_code(product.diagnostics, "import.option") == 1 && product.diagnostics[0].field == "tracking" &&
	            product.outputs.empty());
	product = ImportProduct();
	TEST_EXPECT(run("Hud14.fntset", "; no sheet\n", {}, product, findings) == 1);
	TEST_EXPECT(count_code(product.diagnostics, "import.font") == 1 && product.outputs.empty());
	product = ImportProduct();
	TEST_EXPECT(run("Hud14.fntset", "sheet Hud14.png\nsize 14\n", {}, product, findings) == 1);
	TEST_EXPECT(count_code(product.diagnostics, "import.font") == 1 &&
	            product.diagnostics[0].message.find("'size'") != std::string::npos);
	product = ImportProduct();
	TEST_EXPECT(run("Hud14.fntset", "sheet Missing.png\n", {}, product, findings) == 1);
	TEST_EXPECT(count_code(findings, "import.input") == 1 && product.outputs.empty());
	TEST_EXPECT(editor_test::write_text(folder + "/Broken.png", "not a picture"));
	product = ImportProduct();
	TEST_EXPECT(run("Hud14.fntset", "sheet Broken.png\n", {}, product, findings) == 1);
	TEST_EXPECT(count_code(product.diagnostics, "import.decode") == 1);
	product = ImportProduct();
	TEST_EXPECT(run("AVeryLongFontName.fntset", "sheet Hud14.png\n", {}, product, findings) == 1);
	TEST_EXPECT(count_code(product.diagnostics, "import.font") == 1 &&
	            product.diagnostics[0].message.find("AVeryLongFontName.fnt") != std::string::npos);
	// A sheet the size option makes too narrow for its space is the option's.
	product = ImportProduct();
	TEST_EXPECT(run("Hud14.fntset", "sheet Hud14.png\n", {{"space", "20"}}, product, findings) == 1);
	TEST_EXPECT(count_code(product.diagnostics, "import.option") == 1 && product.diagnostics[0].field == "space");
	return 0;
}

// Over a project: the set an import source, the .fnt the build packs, the sheet an input.
static int test_project() {
	editor_test::TempProjectDir dir("opennova_editor_font_import_project");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Fonts"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(editor_test::write_bytes(root + "/fonts/Hud14.png", sheet_png(test_sheet())));
	TEST_EXPECT(editor_test::write_text(root + "/fonts/Hud14.fntset", "sheet Hud14.png\n"));
	// Without its record the set is no import source.
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.project.imports->empty());
	ImportSidecar record;
	record.importer = "font";
	record.version = kFontImporterVersion;
	Diagnostic error;
	TEST_EXPECT(save_import_sidecar(root + "/fonts/Hud14.fntset.import", record, error));
	TEST_EXPECT(editor_test::backdate_tree(root + "/fonts", std::chrono::hours(1)));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.project.imports->size() == 1);
	const ImportedSource &source = (*view.project.imports)[0];
	TEST_EXPECT(source.source == "fonts/Hud14.fntset" && source.importer == "font" && source.ok && source.reimported);
	TEST_EXPECT(source.inputs == std::vector<std::string>{"fonts/Hud14.png"} && source.outputs.size() == 1);
	TEST_EXPECT(load_import_sidecar(root + "/fonts/Hud14.fntset.import", record, error));
	TEST_EXPECT(record.inputs == std::vector<std::string>{"Hud14.png"} && record.outputs == std::vector<std::string>{"Hud14.fnt"});
	const AssetEntry *fnt = view.project.scan->find("Hud14.fnt");
	TEST_EXPECT(fnt && fnt->kind == AssetKind::Font && fnt->imported_from == "fonts/Hud14.fntset");
	const AssetEntry *set = view.project.scan->find("Hud14.fntset");
	TEST_EXPECT(set && set->kind == AssetKind::ImportSource);
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.font") == 0 && count_code(view.findings.diagnostics, "import.input") == 0);
	// The build packs the font into localres, never the set.
	AssetGraph graph;
	ValidationCache cache;
	const BuildPlan plan = plan_build(paths, *view.project.scan, *view.project.requirements,
	                                  validate_project({paths, *view.project.document, *view.project.scan, view.documents.open}, graph, cache));
	bool in_localres = false, set_packed = false;
	for (const BuildArchive &archive : plan.archives)
		for (const BuildEntry &entry : archive.entries) {
			if (entry.logical_name == "Hud14.fnt") in_localres = archive.slot == ArchiveSlot::Localres;
			if (entry.logical_name == "Hud14.fntset") set_packed = true;
		}
	for (const BuildEntry &entry : plan.loose) set_packed = set_packed || entry.logical_name == "Hud14.fntset";
	TEST_EXPECT(in_localres && !set_packed);
	// Nothing changed: nothing imported. The sheet changed: the font imported again, wider.
	ImportRunResult run = run_imports(paths, *view.project.document);
	TEST_EXPECT(run.reimported == 0 && run.sources.size() == 1 && run.sources[0].ok);
	RgbaImage wider = test_sheet();
	ink(wider, 'A', 0, 6, 1, 8);
	TEST_EXPECT(editor_test::write_bytes(root + "/fonts/Hud14.png", sheet_png(wider)));
	run = run_imports(paths, *view.project.document);
	TEST_EXPECT(run.reimported == 1 && run.sources[0].ok);
	std::vector<uint8_t> bytes;
	std::string message;
	TEST_EXPECT(opennova::io::read_file_bytes(root + "/" + run.sources[0].outputs[0], bytes, message));
	fnt_font_t font{};
	TEST_EXPECT(fnt_parse(bytes.data(), bytes.size(), &font) == FNT_OK);
	const int a = width_of(font, 'A');
	fnt_free(&font);
	TEST_EXPECT(a == 8);
	// The sheet gone: the import fails on its input, the outputs kept.
	fs::remove(root + "/fonts/Hud14.png");
	run = run_imports(paths, *view.project.document);
	TEST_EXPECT(run.reimported == 0 && !run.sources[0].ok && count_code(run.diagnostics, "import.input") == 1);
	TEST_EXPECT(fs::is_regular_file(system_path(root + "/" + run.sources[0].outputs[0])));
	return 0;
}

// An import of a set from a folder outside the project: planned and imported with its sheet beside it
// in fonts/ (the font importer's folder), the sheet copied as it is with no record of its own.
static int test_import_from_disk() {
	editor_test::TempProjectDir dir("opennova_editor_font_import_disk");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Fonts"));
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const std::string art = dir.file("art");
	TEST_EXPECT(editor_test::write_bytes(art + "/glyphs/Hud14.png", sheet_png(test_sheet())));
	TEST_EXPECT(editor_test::write_text(art + "/Hud14.fntset", "sheet glyphs/Hud14.png\n"));
	TEST_EXPECT(editor_test::write_text(art + "/Lost.fntset", "sheet Lost.png\n"));
	TEST_EXPECT(editor_test::write_text(art + "/Away.fntset", "sheet ../../Hud14.png\n"));
	ImportChoice choice;
	choice.path = art + "/Hud14.fntset";
	const ImportPlan plan = plan_import({choice}, false, paths, *view.project.document, *view.project.scan, *view.findings.graph, "");
	TEST_EXPECT(plan.diagnostics.empty() && plan.rows.size() == 2);
	TEST_EXPECT(plan.rows[0].name == "Hud14.fntset" && plan.rows[0].kind == AssetKind::ImportSource &&
	            plan.rows[0].destination == "fonts/Hud14.fntset" && plan.rows[0].made_from.empty());
	TEST_EXPECT(plan.rows[1].name == "Hud14.png" && plan.rows[1].kind == AssetKind::Texture &&
	            plan.rows[1].destination == "fonts/glyphs/Hud14.png" && plan.rows[1].made_from == "Hud14.fntset" &&
	            plan.rows[1].selected && plan.rows[1].problem.empty() && plan.rows[1].source == choice);
	const ImportResult result = import_assets({choice}, paths, *view.project.document, false);
	TEST_EXPECT(result.diagnostics.empty());
	TEST_EXPECT(result.imported == std::vector<std::string>({"fonts/Hud14.fntset", "fonts/glyphs/Hud14.png"}));
	TEST_EXPECT(fs::is_regular_file(system_path(root + "/fonts/Hud14.fntset.import")));
	TEST_EXPECT(!fs::exists(system_path(root + "/fonts/glyphs/Hud14.png.import")));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.project.imports->size() == 1 && (*view.project.imports)[0].ok &&
	            (*view.project.imports)[0].inputs == std::vector<std::string>{"fonts/glyphs/Hud14.png"});
	TEST_EXPECT(view.project.scan->find("Hud14.fnt") && view.project.scan->find("Hud14.fnt")->kind == AssetKind::Font);
	// Imported again with the sheet changed beside it: the project's sheet differs, so it waits for a
	// replace; with it, both come.
	RgbaImage wider = test_sheet();
	ink(wider, 'A', 0, 6, 1, 8);
	TEST_EXPECT(editor_test::write_bytes(art + "/glyphs/Hud14.png", sheet_png(wider)));
	ImportResult again = import_assets({choice}, paths, *view.project.document, false);
	TEST_EXPECT(count_code(again.diagnostics, "import.exists") == 1 && again.imported.empty());
	again = import_assets({choice}, paths, *view.project.document, true);
	TEST_EXPECT(again.diagnostics.empty() && again.imported.size() == 2);
	// A sheet not beside the set, and one whose place would leave the project: planned with the
	// finding and no rows, and refused with it, nothing written.
	for (const char *name : {"Lost.fntset", "Away.fntset"}) {
		ImportChoice refused;
		refused.path = art + "/" + name;
		const ImportPlan lost = plan_import({refused}, false, paths, *view.project.document, *view.project.scan, *view.findings.graph, "");
		TEST_EXPECT(lost.rows.empty() && count_code(lost.diagnostics, "import.input") == 1);
		const ImportResult none = import_assets({refused}, paths, *view.project.document, false);
		TEST_EXPECT(count_code(none.diagnostics, "import.input") == 1 && none.imported.empty());
		TEST_EXPECT(!fs::exists(system_path(root + "/fonts/" + name)));
	}
	return 0;
}

int main() {
	int failures = 0;
	failures += test_font_set();
	failures += test_options();
	failures += test_import_run();
	failures += test_project();
	failures += test_import_from_disk();
	if (failures == 0) std::printf("editor_font_import: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
