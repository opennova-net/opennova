// The imports (ADR 0046 d6/d10, S8): the PNG reader over every color type and depth
// with the five filters and its refusals; the quantizer (an exact small palette, a
// median cut past 256 colors, a PCX round trip); the import pass (a sidecar written
// with the importer's defaults, outputs under the cache, nothing redone for an
// unchanged source, a changed source or a missing output imported again, a bad option
// a finding); the scan listing the outputs as project files the graph resolves and the
// build packs while the source itself is never packed; the sidecar's lifetime (no file
// time in it, a record that does not parse kept as written, a rename taking it and the
// outputs along, a record without its source listing nothing: S9c); and the game
// install as an import source, the effective file copied in and a requirement
// satisfied by it; (import_plan_test.cpp) the plan of an import with the files it needs
// (S11f); and (import_apply_test.cpp) the session's import of that plan, staged and
// published (S11g), with a retail leg over a packed game install (--retail).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <base/io/hash.h>
#include <base/io/json.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/import/png_decode.h>
#include <editor/import/quantize.h>
#include <editor/import/sidecar.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/view/session_view.h>
#include <formats/pcx/pcx_io.h>
#include <formats/pff/pff.h>
#include <formats/threedi/threedi_3di3.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/menu_test_support.h"
#include "editor/png_test_support.h"

using namespace opennova::editor;
using opennova::IndexedImage8;
using opennova::RgbaImage;
using opennova::decode_pcx_indexed;
using opennova::encode_pcx_indexed;
namespace fs = std::filesystem;
namespace io = opennova::io;
using editor_test::PngSpec;
using editor_test::gradient_png;
using editor_test::make_png;

namespace {

// A PNG is an import source only with its `.import` record (importing a file
// writes it); here the author writes the importer's defaults by hand.
bool mark_for_import(const std::string &source) {
	const Importer *importer = importer_for(source);
	if (importer == nullptr) return false;
	ImportSidecar sidecar;
	sidecar.importer = importer->id;
	sidecar.version = importer->version;
	sidecar.options = importer->default_options;
	Diagnostic error;
	return save_import_sidecar(source + ".import", sidecar, error);
}

using editor_test::NoProcess;

const uint8_t *pixel(const RgbaImage &image, int x, int y) { return &image.pixels[size_t((y * image.width + x) * 4)]; }

size_t count_code(const std::vector<Diagnostic> &diagnostics, const std::string &code) {
	size_t n = 0;
	for (const Diagnostic &d : diagnostics) n += d.code == code ? 1 : 0;
	return n;
}

std::string read_text(const std::string &path) {
	std::string text, message;
	return read_file_text(path, text, message) ? text : std::string();
}

const ImportedSource *imported_source(const SessionView &view, const std::string &source) {
	for (const ImportedSource &listed : *view.project.imports)
		if (listed.source == source) return &listed;
	return nullptr;
}

} // namespace

static int test_png_decode() {
	// RGBA 8-bit, two rows, the second filtered with Sub (each byte adds the one bpp back).
	PngSpec rgba;
	rgba.width = 2;
	rgba.height = 2;
	rgba.rows = {0, 10, 20, 30, 255, 40, 50, 60, 128,
	             1, 1, 2, 3, 4, 1, 1, 1, 1};
	RgbaImage image;
	std::string error;
	TEST_EXPECT(decode_png(make_png(rgba), image, error));
	TEST_EXPECT(image.width == 2 && image.height == 2);
	TEST_EXPECT(pixel(image, 0, 0)[0] == 10 && pixel(image, 0, 0)[3] == 255 && pixel(image, 1, 0)[3] == 128);
	TEST_EXPECT(pixel(image, 0, 1)[0] == 1 && pixel(image, 0, 1)[1] == 2 && pixel(image, 1, 1)[0] == 2 && pixel(image, 1, 1)[3] == 5);
	// RGB 8-bit with Up (row 2 adds row 1) and Average and Paeth rows.
	PngSpec rgb;
	rgb.width = 1;
	rgb.height = 4;
	rgb.color_type = 2;
	rgb.rows = {0, 100, 110, 120, 2, 1, 1, 1, 3, 2, 2, 2, 4, 3, 3, 3};
	TEST_EXPECT(decode_png(make_png(rgb), image, error));
	TEST_EXPECT(pixel(image, 0, 1)[0] == 101 && pixel(image, 0, 2)[0] == 52 && pixel(image, 0, 3)[0] == 55 && pixel(image, 0, 3)[3] == 255);
	// A 2-bit palette image with transparency.
	PngSpec paletted;
	paletted.width = 4;
	paletted.height = 1;
	paletted.depth = 2;
	paletted.color_type = 3;
	paletted.palette = {255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9};
	paletted.palette_alpha = {255, 128};
	paletted.rows = {0, 0x1B}; // indices 0,1,2,3
	TEST_EXPECT(decode_png(make_png(paletted), image, error));
	TEST_EXPECT(image.width == 4 && pixel(image, 0, 0)[0] == 255 && pixel(image, 1, 0)[1] == 255 && pixel(image, 1, 0)[3] == 128 &&
	            pixel(image, 2, 0)[2] == 255 && pixel(image, 3, 0)[0] == 9 && pixel(image, 3, 0)[3] == 255);
	// 1-bit grayscale, 16-bit grayscale + alpha.
	PngSpec gray;
	gray.width = 8;
	gray.height = 1;
	gray.depth = 1;
	gray.color_type = 0;
	gray.rows = {0, 0xA5};
	TEST_EXPECT(decode_png(make_png(gray), image, error));
	TEST_EXPECT(pixel(image, 0, 0)[0] == 255 && pixel(image, 1, 0)[0] == 0 && pixel(image, 7, 0)[0] == 255);
	PngSpec deep;
	deep.width = 1;
	deep.height = 1;
	deep.depth = 16;
	deep.color_type = 4;
	deep.rows = {0, 0x12, 0x34, 0xAB, 0xCD};
	TEST_EXPECT(decode_png(make_png(deep), image, error));
	TEST_EXPECT(pixel(image, 0, 0)[0] == 0x12 && pixel(image, 0, 0)[3] == 0xAB);
	// Refusals name the reason.
	PngSpec interlaced = rgba;
	interlaced.interlace = 1;
	TEST_EXPECT(!decode_png(make_png(interlaced), image, error) && error.find("interlaced") != std::string::npos);
	PngSpec bad_crc = rgba;
	bad_crc.corrupt_crc = true;
	TEST_EXPECT(!decode_png(make_png(bad_crc), image, error) && error.find("CRC") != std::string::npos);
	PngSpec bad_depth = paletted;
	bad_depth.depth = 16;
	TEST_EXPECT(!decode_png(make_png(bad_depth), image, error) && error.find("Unsupported") != std::string::npos);
	std::vector<uint8_t> truncated = make_png(rgba);
	truncated.resize(truncated.size() - 20);
	TEST_EXPECT(!decode_png(truncated, image, error));
	TEST_EXPECT(!decode_png({1, 2, 3}, image, error) && error.find("Not a PNG") != std::string::npos);
	return 0;
}

static int test_quantize() {
	// Three colors: the palette is exact and sorted, and the PCX round trip keeps every pixel.
	RgbaImage small;
	small.width = 3;
	small.height = 1;
	small.pixels = {200, 0, 0, 255, 0, 200, 0, 255, 0, 0, 200, 255};
	IndexedImage8 indexed = quantize_to_256(small);
	TEST_EXPECT(indexed.width == 3 && indexed.indices.size() == 3);
	TEST_EXPECT(indexed.palette[indexed.indices[0]][0] == 200 && indexed.palette[indexed.indices[1]][1] == 200 &&
	            indexed.palette[indexed.indices[2]][2] == 200);
	std::vector<uint8_t> pcx;
	std::string error;
	TEST_EXPECT(encode_pcx_indexed(indexed, pcx, error));
	IndexedImage8 back;
	TEST_EXPECT(decode_pcx_indexed(pcx.data(), pcx.size(), back, error) && back.width == 3 && back.indices == indexed.indices);
	// Past 256 colors: at most 256 entries, every index valid, nearby colors map close.
	RgbaImage many;
	many.width = 64;
	many.height = 64;
	for (int y = 0; y < 64; ++y)
		for (int x = 0; x < 64; ++x) {
			many.pixels.push_back(uint8_t(x * 4));
			many.pixels.push_back(uint8_t(y * 4));
			many.pixels.push_back(uint8_t((x + y) * 2));
			many.pixels.push_back(255);
		}
	IndexedImage8 reduced = quantize_to_256(many);
	TEST_EXPECT(reduced.indices.size() == 64 * 64);
	int used = 0;
	bool seen[256] = {};
	for (const uint8_t index : reduced.indices) if (!seen[index]) { seen[index] = true; ++used; }
	TEST_EXPECT(used > 64 && used <= 256);
	long worst = 0;
	for (size_t i = 0; i < reduced.indices.size(); ++i)
		for (int c = 0; c < 3; ++c)
			worst = std::max(worst, std::labs(long(many.pixels[i * 4 + size_t(c)]) - long(reduced.palette[reduced.indices[i]][c])));
	TEST_EXPECT(worst < 40);
	// Deterministic.
	const IndexedImage8 again = quantize_to_256(many);
	TEST_EXPECT(again.indices == reduced.indices);
	return 0;
}

static int test_import_pass() {
	editor_test::TempProjectDir dir("opennova_editor_import_pass");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Imports"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", gradient_png(8, 8)));
	session.handle(make_request(EditorRequestKind::Rescan));
	const SessionView &view = session.view();
	// With no record the PNG is a texture the game loads as it is: nothing imports,
	// a menu naming it resolves by retail's extension dispatch, and it packs.
	TEST_EXPECT(view.project.imports->empty());
	TEST_EXPECT(view.project.scan->find("logo.png") && view.project.scan->find("logo.png")->kind == AssetKind::Texture);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo.png") == ReferenceStatus::Present);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo") == ReferenceStatus::Missing);
	// Its record makes it an import source.
	TEST_EXPECT(mark_for_import(root + "/art/logo.png"));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(view.project.imports->size() == 1 && (*view.project.imports)[0].source == "art/logo.png" && (*view.project.imports)[0].reimported);
	TEST_EXPECT((*view.project.imports)[0].importer == "image" && (*view.project.imports)[0].ok && (*view.project.imports)[0].outputs.size() == 1);
	const std::string output = (*view.project.imports)[0].outputs[0];
	TEST_EXPECT(output.find(".opennova/imported/") == 0 && output.find("logo.pcx") != std::string::npos);
	TEST_EXPECT(fs::is_regular_file(root + "/" + output));
	ImportSidecar sidecar;
	Diagnostic error;
	TEST_EXPECT(load_import_sidecar(root + "/art/logo.png.import", sidecar, error));
	TEST_EXPECT(sidecar.importer == "image" && sidecar.version == 1 && sidecar.options.at("format") == "pcx");
	TEST_EXPECT(sidecar.outputs == std::vector<std::string>{"logo.pcx"} && sidecar.source_hash != 0);
	// The output decodes to the source's size.
	std::vector<uint8_t> pcx;
	std::string message;
	TEST_EXPECT(read_file_bytes(root + "/" + output, pcx, message));
	IndexedImage8 decoded;
	TEST_EXPECT(decode_pcx_indexed(pcx.data(), pcx.size(), decoded, message) && decoded.width == 8 && decoded.height == 8);
	// The scan lists the output as a project file from its source; the source is never packed.
	const AssetEntry *produced = view.project.scan->find("logo.pcx");
	TEST_EXPECT(produced && produced->imported_from == "art/logo.png" && produced->kind == AssetKind::Texture);
	const AssetEntry *source = view.project.scan->find("logo.png");
	TEST_EXPECT(source && source->kind == AssetKind::ImageSource);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "logo") ==
			ReferenceStatus::Present);
	// A menu names the output by its own file; the source is not packed, so a menu
	// naming the PNG finds nothing the game can load.
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo.pcx") == ReferenceStatus::Present);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo.png") == ReferenceStatus::Missing);
	const BuildPlan plan = plan_build(paths, *view.project.scan, *view.project.requirements,
	                                  validate_open_documents(paths, *view.project.document, *view.project.scan, view.documents.open));
	bool packed = false, source_packed = false;
	for (const BuildArchive &archive : plan.archives)
		for (const BuildEntry &entry : archive.entries) {
			if (entry.logical_name == "logo.pcx") packed = true;
			if (entry.logical_name == "logo.png") source_packed = true;
		}
	for (const BuildEntry &entry : plan.loose) if (entry.logical_name == "logo.png") source_packed = true;
	TEST_EXPECT(packed && !source_packed);
	// Nothing changed: nothing is imported again. A changed source is; so is a missing output.
	ImportRunResult run = run_imports(paths, *view.project.document);
	TEST_EXPECT(run.reimported == 0 && run.sources.size() == 1 && run.sources[0].outputs.size() == 1);
	// A changed source: its size or last-write time moves (the import cache trusts them
	// while they hold, the way Godot's does, at the file system's own clock resolution).
	TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", gradient_png(9, 8, 77)));
	run = run_imports(paths, *view.project.document);
	TEST_EXPECT(run.reimported == 1);
	fs::remove(root + "/" + output);
	run = run_imports(paths, *view.project.document);
	TEST_EXPECT(run.reimported == 1 && fs::is_regular_file(root + "/" + output));
	run = run_imports(paths, *view.project.document, true);
	TEST_EXPECT(run.reimported == 1);
	// One source by name; an unknown option is a finding and the source stays not ok.
	run = run_imports(paths, *view.project.document, true, "logo.png");
	TEST_EXPECT(run.reimported == 1);
	sidecar.options["format"] = "tga";
	TEST_EXPECT(save_import_sidecar(root + "/art/logo.png.import", sidecar, error));
	run = run_imports(paths, *view.project.document, true);
	TEST_EXPECT(run.reimported == 0 && !run.sources[0].ok && !run.diagnostics.empty() && run.diagnostics[0].code == "import.option");
	sidecar.options["format"] = "pcx";
	TEST_EXPECT(save_import_sidecar(root + "/art/logo.png.import", sidecar, error));
	// An imported output is never renamed: its source is.
	session.handle(make_request(EditorRequestKind::Rescan));
	const RenamePlan rename =
			plan_rename(paths, *view.project.scan, *view.findings.graph, "logo.pcx", "logo2.pcx");
	TEST_EXPECT(!rename.ok() && rename.refusals.front().code == "rename.imported");
	// The session's Reimport request and the JSON view.
	EditorRequest reimport = make_request(EditorRequestKind::Reimport);
	reimport.flag = true;
	session.handle(reimport);
	TEST_EXPECT(view.project.imports->size() == 1 && (*view.project.imports)[0].reimported &&
			session.outcome().done());
	// A forced Reimport that fails: its finding is the request's outcome (refused) and
	// exactly one Problems row (a Reimport is the refresh with its source forced: one
	// import pass, whose findings ride the scan).
	ImportSidecar current;
	TEST_EXPECT(load_import_sidecar(root + "/art/logo.png.import", current, error));
	current.options["format"] = "tga";
	TEST_EXPECT(save_import_sidecar(root + "/art/logo.png.import", current, error));
	session.handle(reimport);
	TEST_EXPECT(!session.outcome().done() && count_code(session.outcome().findings, "import.option") == 1);
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.option") == 1 && count_code(view.project.scan->diagnostics, "import.option") == 1);
	// Without force the changed record alone imports again (the import cache remembers
	// the record the outputs were made from), so the finding stays until the option is
	// fixed, and then nothing is imported: the outputs are the fixed record's.
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.option") == 1);
	current.options["format"] = "pcx";
	TEST_EXPECT(save_import_sidecar(root + "/art/logo.png.import", current, error));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.option") == 0 && view.project.imports->size() == 1 && !(*view.project.imports)[0].reimported);
	// A PNG that is not one is a finding on its source, and no output.
	TEST_EXPECT(editor_test::write_text(root + "/art/broken.png", "not a png"));
	TEST_EXPECT(mark_for_import(root + "/art/broken.png"));
	session.handle(make_request(EditorRequestKind::Rescan));
	bool broken_reported = false;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code == "import.decode" && d.asset == "art/broken.png") broken_reported = true;
	TEST_EXPECT(broken_reported && view.project.imports->size() == 2);
	// A Reimport of one source: another source's failure stays a Problems row but is not
	// this request's outcome.
	EditorRequest one = make_request(EditorRequestKind::Reimport, "logo.png");
	one.flag = true;
	session.handle(one);
	TEST_EXPECT(session.outcome().done() &&
			count_code(view.findings.diagnostics, "import.decode") == 1);
	TEST_EXPECT(imported_source(view, "art/logo.png") && imported_source(view, "art/logo.png")->reimported);
	// A menu's .tga the project lacks loads its .dds (from the first dot), as retail's
	// loader does (a texture of no model finds it too, the runtime's lookup trying the
	// name's stem with .dds, S11h); with the .tga there the .tga is the file.
	TEST_EXPECT(editor_test::write_bytes(root + "/art/sky.dds", std::vector<uint8_t>{'D', 'D', 'S', ' '}));
	session.handle(make_request(EditorRequestKind::Rescan));
	std::string sky;
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "sky.tga", std::string(),
						&sky) == ReferenceStatus::Present &&
			sky == "art/sky.dds");
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "sky.tga", std::string(), &sky) == ReferenceStatus::Present &&
	            sky == "art/sky.dds");
	TEST_EXPECT(editor_test::write_bytes(root + "/art/sky.tga", std::vector<uint8_t>(18, 0)));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "sky.tga", std::string(),
						&sky) == ReferenceStatus::Present &&
			sky == "art/sky.tga");
	return 0;
}

// The sidecar's lifetime (S9c): it holds nothing a checkout changes (a touched source
// leaves its bytes alone, new content changes its hash) while the machine-local import
// cache holds the size and the time; a record that does not parse is never replaced;
// renaming a source takes its record and its output along with every reference to the
// output; a record whose source is gone lists nothing.
static int test_import_lifetime() {
	editor_test::TempProjectDir dir("opennova_editor_import_lifetime");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Lifetime"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const std::string source = root + "/art/logo.png";
	const std::string sidecar_path = source + ".import";
	TEST_EXPECT(editor_test::write_bytes(source, gradient_png(8, 8)));
	TEST_EXPECT(mark_for_import(source));
	session.handle(make_request(EditorRequestKind::Rescan));
	const std::string first = read_text(sidecar_path);
	TEST_EXPECT(!first.empty() && first.find("modified") == std::string::npos && first.find("size") == std::string::npos);
	TEST_EXPECT(fs::is_regular_file(paths.import_cache_file));
	ImportSidecar before;
	Diagnostic error;
	TEST_EXPECT(load_import_sidecar(sidecar_path, before, error) && before.source_hash != 0);
	// A touched source (a checkout, a copy): nothing imports and the record keeps its bytes.
	fs::last_write_time(source, fs::last_write_time(source) + std::chrono::hours(1));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(view.project.imports->size() == 1 && !(*view.project.imports)[0].reimported && read_text(sidecar_path) == first);
	// The cache lost with the outputs (a fresh clone): imported again, the record unchanged.
	std::error_code ec;
	fs::remove_all(paths.imported_dir, ec);
	fs::remove(paths.import_cache_file, ec);
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(view.project.imports->size() == 1 && (*view.project.imports)[0].reimported && read_text(sidecar_path) == first);
	TEST_EXPECT(view.project.scan->find("logo.pcx") != nullptr);
	// A cache of another schema (one an older build wrote, its digests made another way)
	// vouches for nothing: the source is hashed again and the record keeps the true hash.
	{
		io::JsonValue cache;
		std::string message;
		TEST_EXPECT(io::json_parse(read_text(paths.import_cache_file), cache, message) && cache.is_object());
		const int schema = cache.get_int("schema_version", -1);
		cache.set("schema_version", io::JsonValue::make_number(schema - 1));
		if (io::JsonValue *sources = cache.get("sources"))
			for (io::JsonValue &item : sources->array) item.set("hash", io::JsonValue::make_string(io::hex64(1)));
		TEST_EXPECT(editor_test::write_text(paths.import_cache_file, io::json_write(cache)));
		session.handle(make_request(EditorRequestKind::Rescan));
		TEST_EXPECT(view.project.imports->size() == 1 && read_text(sidecar_path) == first);
	}
	// New content of the same size: the record's hash changes.
	TEST_EXPECT(editor_test::write_bytes(source, gradient_png(8, 8, 99)));
	session.handle(make_request(EditorRequestKind::Rescan));
	ImportSidecar after;
	TEST_EXPECT(load_import_sidecar(sidecar_path, after, error) && after.source_hash != before.source_hash);
	TEST_EXPECT(view.project.imports->size() == 1 && (*view.project.imports)[0].reimported);
	// A record that does not parse (a hand edit with a typo) is the author's: reported,
	// kept byte for byte and its source not imported (its output not listed) for as long
	// as it does not read; fixed back to what it was, nothing imports.
	const std::string good = read_text(sidecar_path);
	std::string typo = good;
	typo.insert(typo.find('{') + 1, "\"my_setting\": \"keep-me\",");
	typo.insert(typo.rfind('}'), ","); // a trailing comma
	TEST_EXPECT(editor_test::write_text(sidecar_path, typo));
	for (int pass = 0; pass < 2; ++pass) {
		session.handle(make_request(EditorRequestKind::Rescan));
		TEST_EXPECT(read_text(sidecar_path) == typo && count_code(view.findings.diagnostics, "import.sidecar") == 1);
		TEST_EXPECT(view.project.imports->size() == 1 && !(*view.project.imports)[0].ok &&
				!(*view.project.imports)[0].reimported);
		TEST_EXPECT(view.project.scan->find("logo.pcx") == nullptr);
	}
	EditorRequest forced = make_request(EditorRequestKind::Reimport, "logo.png");
	forced.flag = true;
	session.handle(forced);
	TEST_EXPECT(!session.outcome().done() && count_code(session.outcome().findings, "import.sidecar") == 1);
	TEST_EXPECT(read_text(sidecar_path) == typo);
	TEST_EXPECT(editor_test::write_text(sidecar_path, good));
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.sidecar") == 0 &&
			view.project.imports->size() == 1 && (*view.project.imports)[0].ok &&
			!(*view.project.imports)[0].reimported &&
			view.project.scan->find("logo.pcx") != nullptr);
	// A menu names the output; renaming the source renames the output with it.
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	Document *menu = session.document_for("main.mnu");
	NodeAddress exit;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit));
	EditorRequest set = make_request(EditorRequestKind::EditRecord, menu->path());
	set.edits = menu_test::image_edits(*menu, exit, "logo.pcx");
	session.handle(set);
	session.handle(make_request(EditorRequestKind::SaveAll));
	TEST_EXPECT(count_code(view.findings.diagnostics, "reference.missing") == 0);
	const std::string old_dir = (*view.project.imports)[0].output_dir;
	{
		const RenamePlan plan = plan_rename(paths, *view.project.scan, *view.findings.graph, "art/logo.png", "logo2.png");
		TEST_EXPECT(plan.ok() && plan.sidecar == "art/logo.png.import" && plan.new_sidecar == "art/logo2.png.import");
		TEST_EXPECT(plan.outputs.size() == 1 && plan.outputs[0].old_name == "logo.pcx" && plan.outputs[0].new_name == "logo2.pcx");
		TEST_EXPECT(plan.sites.size() == 1 && plan.sites[0].after == "logo2.pcx" && plan.sites[0].target == plan.outputs[0].path);
		// The output's new name must fit the archives and be free.
		TEST_EXPECT(editor_test::write_text(root + "/taken.pcx", "x"));
		session.handle(make_request(EditorRequestKind::Rescan));
		const RenamePlan taken = plan_rename(paths, *view.project.scan, *view.findings.graph, "art/logo.png", "taken.png");
		TEST_EXPECT(!taken.ok() && taken.refusals.front().code == "rename.exists");
		fs::remove(root + "/taken.pcx", ec);
		session.handle(make_request(EditorRequestKind::Rescan));
	}
	session.handle(make_request(EditorRequestKind::RenameAsset, "art/logo.png", "logo2.png"));
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(!fs::exists(source) && fs::exists(root + "/art/logo2.png"));
	TEST_EXPECT(!fs::exists(sidecar_path) && read_text(root + "/art/logo2.png.import").find("logo2.pcx") != std::string::npos);
	TEST_EXPECT(!fs::exists(root + "/" + old_dir));
	const AssetEntry *renamed = view.project.scan->find("logo2.pcx");
	TEST_EXPECT(renamed && renamed->imported_from == "art/logo2.png" && view.project.scan->find("logo.pcx") == nullptr);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "logo2.pcx") == ReferenceStatus::Present);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "logo.pcx") == ReferenceStatus::Missing);
	menu = session.document_for("main.mnu"); // reloaded after the rewrite
	Value image;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit) &&
	            menu->get(menu_test::child_of(*menu, exit, "appearance"), "value", image) &&
	            std::get<std::string>(image) == "logo2.pcx");
	TEST_EXPECT(count_code(view.findings.diagnostics, "reference.missing") == 0);
	// The source gone: its record lists nothing (a warning says so), so the output no
	// longer resolves and the menu's reference is missing.
	fs::remove(root + "/art/logo2.png", ec);
	session.handle(make_request(EditorRequestKind::Rescan));
	TEST_EXPECT(view.project.imports->empty() && view.project.scan->find("logo2.pcx") == nullptr);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "logo2.pcx") == ReferenceStatus::Missing);
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.orphan_record") == 1 && count_code(view.findings.diagnostics, "reference.missing") == 1);
	return 0;
}

static int test_retail_source() {
	editor_test::TempProjectDir dir("opennova_editor_import_retail");
	// A fake install: the boot table's resource.pff with a text file and a string table.
	const std::string retail = dir.file("retail");
	std::error_code ec;
	fs::create_directories(retail, ec);
	std::vector<uint8_t> table;
	Diagnostic error;
	BlankRequest blank;
	blank.logical_name = "gametext.bin";
	blank.project_title = "Retail";
	TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	const uint8_t note[] = {'r', 'e', 't', 'a', 'i', 'l'};
	const std::vector<uint8_t> splash = gradient_png(4, 4, 3);
	const std::vector<uint8_t> icon = gradient_png(4, 4, 9);
	const opennova::pff::PffWriteEntry entries[] = {
		{"note.txt", note, sizeof(note), 0, 0, 0},
		{"gametext.bin", table.data(), uint32_t(table.size()), 0, 0, 0},
		{"splash.png", splash.data(), uint32_t(splash.size()), 0, 0, 0},
		{"icon.png", icon.data(), uint32_t(icon.size()), 0, 0, 0},
	};
	TEST_EXPECT(opennova::pff::pff_write_archive((retail + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 4) ==
	            opennova::pff::PFF_WRITE_OK);
	// A loose note.txt beside the archive: a stock launch reads the archive's alone (the
	// loose file wins only under /d) [orig: FileSystem_OpenFile @ 0x75b1c0, the gate
	// @ 0x75b1e5], and so does the import.
	TEST_EXPECT(editor_test::write_text(retail + "/note.txt", "loose"));
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Retail"));
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.retail_files.empty());
	session.handle(make_request(EditorRequestKind::PreviewRetailImport));
	TEST_EXPECT(!view.dialogs.import_preview.open &&
			view.findings.diagnostics.back().code == "import.retail");
	editor_test::set_retail_directory(session, retail);
	TEST_EXPECT(
			view.project.retail_files.size() == 4); // the archive itself is not an importable file
	std::vector<Diagnostic> diagnostics;
	const std::vector<ImportSource> sources = list_retail_import_sources(retail, *view.project.document, diagnostics);
	TEST_EXPECT(diagnostics.empty() && sources.size() == 4 && sources[0].retail && sources[0].path == retail);
	// The whole list to choose from, none chosen: nothing planned yet.
	session.handle(make_request(EditorRequestKind::PreviewRetailImport));
	TEST_EXPECT(view.dialogs.import_preview.open && view.dialogs.import_preview.choices.size() == 4 && view.dialogs.import_preview.choices[0].retail);
	TEST_EXPECT(view.dialogs.import_preview.roots.empty() &&
			view.dialogs.import_preview.plan->rows.empty());
	// The requirement gametext.bin, missing in the project, chosen from the list (the plan made
	// again, the list kept) and imported from the game data.
	const RequirementRow *row = nullptr;
	for (const RequirementRow &candidate : view.project.requirements->rows) if (candidate.name == "gametext.bin") row = &candidate;
	TEST_EXPECT(row && row->state == RequirementState::Missing);
	EditorRequest choose = make_request(EditorRequestKind::PlanImport);
	ImportSource source;
	source.path = retail;
	source.entry = "gametext.bin";
	source.retail = true;
	choose.imports.push_back(source);
	source.entry = "note.txt";
	choose.imports.push_back(source);
	session.handle(choose);
	TEST_EXPECT(view.dialogs.import_preview.open && view.dialogs.import_preview.choices.size() == 4 && view.dialogs.import_preview.roots.size() == 2);
	TEST_EXPECT(view.dialogs.import_preview.plan->rows.size() == 2 && view.dialogs.import_preview.plan->rows[0].found_in == "the game install");
	EditorRequest import = make_request(EditorRequestKind::ImportFiles);
	import.imports = choose.imports;
	session.handle(import);
	TEST_EXPECT(session.outcome().done() && !view.dialogs.import_preview.open);
	for (const RequirementRow &candidate : view.project.requirements->rows) if (candidate.name == "gametext.bin") row = &candidate;
	TEST_EXPECT(row && row->state == RequirementState::Present);
	std::string text, message;
	TEST_EXPECT(view.project.scan->find("note.txt") && read_file_text(view.project.root + "/" + view.project.scan->find("note.txt")->relative_path, text, message) && text == "retail");
	// A name the install does not have.
	import.imports.clear();
	source.entry = "absent.txt";
	import.imports.push_back(source);
	session.handle(import);
	TEST_EXPECT(view.findings.diagnostics.back().code == "import.read");
	// A PNG of the game install, and one of an archive, is the game's own file: copied as it is,
	// with no import record, so it is a texture (a loose PNG from the disk becomes an import
	// source with its record: test_import_pass, the command line's import).
	import.imports.clear();
	source.entry = "splash.png";
	import.imports.push_back(source);
	ImportSource member;
	member.path = retail + "/resource.pff";
	member.entry = "icon.png";
	import.imports.push_back(member);
	session.handle(import);
	TEST_EXPECT(session.outcome().done());
	for (const char *name : {"splash.png", "icon.png"}) {
		const AssetEntry *png = view.project.scan->find(name);
		TEST_EXPECT(png && png->kind == AssetKind::Texture);
		TEST_EXPECT(png && !fs::exists(fs::path(view.project.root) / (png->relative_path + kImportSidecarSuffix)));
	}
	TEST_EXPECT(view.project.imports->empty());
	// A directory that is no install.
	editor_test::set_retail_directory(session, dir.file("empty"));
	TEST_EXPECT(view.project.retail_files.empty());
	return 0;
}

// The scene texts the Blender add-on writes convert once on import (S10f): an .o3d makes
// the model alone (the textures it names are its references, which an import with the
// files it needs brings: S11g, import_apply_test.cpp), a clip set makes its table and
// clips, the source is not kept, a scene that does not read names its line and writes
// nothing, importing the same scene again leaves the same outputs as they are, and a clip
// set `opennova-3di anim build` refuses is refused here too.
static int test_scene_imports() {
	editor_test::TempProjectDir dir("opennova_editor_scene_imports");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Scenes"));
	const std::string root = session.view().project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const ProjectDocument &document = *session.view().project.document;
	const auto has_error = [](const std::vector<Diagnostic> &findings) {
		for (const Diagnostic &d : findings)
			if (d.severity == DiagnosticSeverity::Error) return true;
		return false;
	};
	const auto imported = [](const ImportResult &r, const std::string &path) {
		return std::find(r.imported.begin(), r.imported.end(), path) != r.imported.end();
	};

	// spinner.o3d names spinner.tga and glow.tga; the first is beside it (upper case), and the
	// converter copies neither.
	const std::string source = dir.file("export");
	fs::create_directories(source);
	const fs::path fixture = fs::path(__FILE__).parent_path().parent_path().parent_path() / "fixtures" / "threedi" / "o3d" / "spinner.o3d";
	std::error_code ec;
	fs::copy_file(fixture, source + "/spinner.o3d", ec);
	TEST_EXPECT(!ec && editor_test::write_text(source + "/SPINNER.TGA", "tga"));
	ImportResult r = import_assets({{source + "/spinner.o3d", {}}}, paths, document, false);
	TEST_EXPECT(!has_error(r.diagnostics));
	TEST_EXPECT(imported(r, "models/spinner.3di") && r.imported.size() == 1 && !fs::exists(root + "/SPINNER.TGA"));
	TEST_EXPECT(!fs::exists(root + "/spinner.o3d") && !fs::exists(root + "/models/spinner.o3d"));
	std::vector<uint8_t> model;
	std::string message;
	TEST_EXPECT(read_file_bytes(root + "/models/spinner.3di", model, message));
	opennova::threedi::Threedi3di3 check{};
	TEST_EXPECT(opennova::threedi::threedi_3di3_read_memory(model.data(), model.size(), &check) == 0);
	opennova::threedi::threedi_3di3_free(&check);

	// Again: the same outputs, left as they are.
	r = import_assets({{source + "/spinner.o3d", {}}}, paths, document, false);
	TEST_EXPECT(!has_error(r.diagnostics) && r.imported.empty());

	// A scene that does not read names its line and writes nothing.
	TEST_EXPECT(editor_test::write_text(source + "/broken.o3d", "o3d 1\nmodel broken\nlod 0\nbogus 1\n"));
	r = import_assets({{source + "/broken.o3d", {}}}, paths, document, false);
	bool line = false;
	for (const Diagnostic &d : r.diagnostics) line = line || (d.code == "import.scene" && d.line == 4);
	TEST_EXPECT(line && r.imported.empty() && !fs::exists(root + "/models/broken.3di"));

	// A clip set: the table its adm record names and its clip.
	const std::string set = "o3a 1\nadm CHECK.adm\nrow anim_reset \"walk\"\nclip walk\nfps 30\nflags 0x1\nframes 1\n"
	                        "bone -1 0 0 0 0.5 \"BN01 Pelvis\"\n k 0 0 0 1\n k 0 0 0 1\n"
	                        "event 0 0 0 0x0 0.9 1.7\nevent 0 0 0 0x0 0.9 1.7\n";
	TEST_EXPECT(editor_test::write_text(source + "/walk.o3a", set));
	r = import_assets({{source + "/walk.o3a", {}}}, paths, document, false);
	TEST_EXPECT(!has_error(r.diagnostics));
	TEST_EXPECT(imported(r, "anims/CHECK.adm") && imported(r, "anims/walk.bad"));

	// What `opennova-3di anim build` refuses, the converter refuses (one reader, one mint): a
	// row whose key names no anim slot, and a table name the game could not pack.
	const auto refused = [&](const std::string &name, const std::string &text, const std::string &words) {
		if (!editor_test::write_text(source + "/" + name, text)) return false;
		const ImportResult result = import_assets({{source + "/" + name, {}}}, paths, document, false);
		bool said = false;
		for (const Diagnostic &d : result.diagnostics)
			said = said || (d.code == "import.scene" && d.message.find(words) != std::string::npos);
		return said && result.imported.empty();
	};
	std::string no_slot = set;
	no_slot.insert(no_slot.find("row anim_reset"), "row anim_notaslot \"walk\"\n");
	TEST_EXPECT(refused("noslot.o3a", no_slot, "names no anim slot"));
	std::string long_table = set;
	long_table.replace(long_table.find("CHECK.adm"), 9, "CHECK_TOO_LONG.adm");
	TEST_EXPECT(refused("long.o3a", long_table, "the game packs a file name of at most 15"));
	return 0;
}

// The plan of an import with the files it needs (import_plan_test.cpp), and the session's
// import of it (import_apply_test.cpp; its retail leg runs with --retail).
int run_import_plan_tests();
int run_import_apply_tests();

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_png_decode();
	failures += test_quantize();
	failures += test_import_pass();
	failures += test_import_lifetime();
	failures += test_retail_source();
	failures += test_scene_imports();
	failures += run_import_plan_tests();
	failures += run_import_apply_tests();
	if (failures == 0) std::printf("editor_import: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
