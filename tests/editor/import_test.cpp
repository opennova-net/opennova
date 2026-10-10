// The imports (ADR 0046 d6/d10, S8; the PNG reader's and the quantizer's own cases are
// formats/png's and formats/pcx's, tests/png and tests/pcx): the import pass (a sidecar written
// with the importer's defaults, outputs under the cache, nothing redone for an
// unchanged source, a changed source or a missing output imported again, a bad option
// a finding); the scan listing the outputs as project files the graph resolves and the
// build packs while the source itself is never packed; S13 A8's import of many inputs (a
// test's importer reading two files through its ImportContext, imported again when either
// changes and not when neither does) and the image importer's TGA output; the sidecar's lifetime (no file
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
#include <editor/graph/project_validation.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/rename_transaction.h>
#include <editor/assets/project_scan.h>
#include <editor/import/import_pass.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <formats/png/png_decode.h>
#include <editor/import/sidecar.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_run.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view_json.h>
#include <formats/pcx/pcx_io.h>
#include <formats/pff/pff.h>
#include <formats/tga/tga.h>
#include <runtime/renderer/texture_load_rules.h>
#include <formats/threedi/threedi_3di3.h>

#include "common/gp_model_bytes.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/menu_test_support.h"
#include "common/png_test_support.h"

using namespace opennova::editor;
using opennova::IndexedImage8;
using opennova::RgbaImage;
using opennova::decode_pcx_indexed;
using opennova::encode_pcx_indexed;
namespace fs = std::filesystem;
namespace io = opennova::io;
using test_png::PngSpec;
using test_png::gradient_png;
using test_png::make_png;
using opennova::png::decode_png;

namespace {

// A PNG is an import source only with its `.import` record (importing a file
// writes it); here the author writes the importer's defaults by hand.
bool mark_for_import(const std::string &source) {
	const Importer *importer = importer_for(source);
	if (importer == nullptr) return false;
	ImportSidecar sidecar;
	sidecar.importer = importer->id;
	sidecar.version = importer->version;
	// A PCX, the 8-bit indexed file these sources make (the image importer's format option; its default
	// is a 32-bit TGA).
	sidecar.options = {{"format", "pcx"}};
	Diagnostic error;
	return save_import_sidecar(source + ".import", sidecar, error);
}

using editor_test::NoProcess;

size_t count_code(const std::vector<Diagnostic> &diagnostics, const std::string &code) {
	size_t n = 0;
	for (const Diagnostic &d : diagnostics) n += d.code() == code ? 1 : 0;
	return n;
}

std::string read_text(const std::string &path) {
	std::string text, message;
	return io::read_file_text(path, text, message) ? text : std::string();
}

const ImportedSource *imported_source(const SessionView &view, const std::string &source) {
	for (const ImportedSource &listed : *view.project.imports)
		if (listed.source == source) return &listed;
	return nullptr;
}

} // namespace

static int test_import_pass() {
	editor_test::TempProjectDir dir("opennova_editor_import_pass");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Imports"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", gradient_png(8, 8)));
	editor_test::handle_to_end(session, request::rescan());
	const SessionView &view = session.view();
	// With no record the PNG is a texture the game loads as it is: nothing imports,
	// a menu naming it resolves by retail's extension dispatch, and it packs.
	TEST_EXPECT(view.project.imports->empty());
	TEST_EXPECT(view.project.scan->find("logo.png") && view.project.scan->find("logo.png")->kind == AssetKind::Texture);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo.png") == ReferenceStatus::Present);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo") == ReferenceStatus::Missing);
	// Its record makes it an import source.
	TEST_EXPECT(mark_for_import(root + "/art/logo.png"));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.project.imports->size() == 1 && (*view.project.imports)[0].source == "art/logo.png" && (*view.project.imports)[0].reimported);
	TEST_EXPECT((*view.project.imports)[0].importer == "image" && (*view.project.imports)[0].ok && (*view.project.imports)[0].outputs.size() == 1);
	const std::string output = (*view.project.imports)[0].outputs[0];
	TEST_EXPECT(output.find(".opennova/imported/") == 0 && output.find("logo.pcx") != std::string::npos);
	TEST_EXPECT(fs::is_regular_file(root + "/" + output));
	ImportSidecar sidecar;
	Diagnostic error;
	TEST_EXPECT(load_import_sidecar(root + "/art/logo.png.import", sidecar, error));
	TEST_EXPECT(sidecar.importer == "image" && sidecar.version == importer_for("logo.png")->version && sidecar.options.at("format") == "pcx");
	TEST_EXPECT(sidecar.outputs == std::vector<std::string>{"logo.pcx"} && sidecar.source_hash != 0);
	// The output decodes to the source's size.
	std::vector<uint8_t> pcx;
	std::string message;
	TEST_EXPECT(io::read_file_bytes(root + "/" + output, pcx, message));
	IndexedImage8 decoded;
	TEST_EXPECT(decode_pcx_indexed(pcx.data(), pcx.size(), decoded, message) && decoded.width == 8 && decoded.height == 8);
	// The scan lists the output as a project file from its source; the source is never packed.
	const AssetEntry *produced = view.project.scan->find("logo.pcx");
	TEST_EXPECT(produced && produced->imported_from == "art/logo.png" && produced->kind == AssetKind::Texture);
	const AssetEntry *source = view.project.scan->find("logo.png");
	TEST_EXPECT(source && source->kind == AssetKind::ImportSource);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "logo.pcx") ==
			ReferenceStatus::Present);
	// A menu names the output by its own file; the source is not packed, so a menu
	// naming the PNG finds nothing the game can load.
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo.pcx") == ReferenceStatus::Present);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "logo.png") == ReferenceStatus::Missing);
	AssetGraph graph;
	ValidationCache cache;
	const BuildPlan plan = plan_build(paths, *view.project.scan, *view.project.requirements,
			validate_project({ paths, *view.project.document, *view.project.scan, view.documents.open }, graph, cache));
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
	sidecar.options["format"] = "bmp";
	TEST_EXPECT(save_import_sidecar(root + "/art/logo.png.import", sidecar, error));
	run = run_imports(paths, *view.project.document, true);
	TEST_EXPECT(run.reimported == 0 && !run.sources[0].ok && !run.diagnostics.empty() && run.diagnostics[0].code() == "import.option");
	sidecar.options["format"] = "pcx";
	TEST_EXPECT(save_import_sidecar(root + "/art/logo.png.import", sidecar, error));
	// An imported output is never renamed: its source is.
	editor_test::handle_to_end(session, request::rescan());
	const RenamePlan rename =
			plan_rename(paths, *view.project.scan, *view.findings.graph, "logo.pcx", "logo2.pcx");
	TEST_EXPECT(!rename.ok() && rename.refusals.front().code() == "rename.imported");
	// The session's Reimport request and the JSON view.
	EditorRequest reimport = request::reimport();
	reimport.force = true;
	editor_test::handle_to_end(session, reimport);
	TEST_EXPECT(view.project.imports->size() == 1 && (*view.project.imports)[0].reimported &&
			session.outcome().done());
	// A forced Reimport that fails: its finding is what its operation came to (failed) and
	// exactly one Problems row (a Reimport is the refresh with its source forced: one
	// import pass, whose findings ride the scan).
	ImportSidecar current;
	TEST_EXPECT(load_import_sidecar(root + "/art/logo.png.import", current, error));
	current.options["format"] = "bmp";
	TEST_EXPECT(save_import_sidecar(root + "/art/logo.png.import", current, error));
	const ActionOutcome failed = editor_test::handle_to_end(session, reimport);
	TEST_EXPECT(!failed.done() && count_code(failed.findings, "import.option") == 1);
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.option") == 1 && count_code(view.project.scan->diagnostics, "import.option") == 1);
	// Without force the changed record alone imports again (the import cache remembers
	// the record the outputs were made from), so the finding stays until the option is
	// fixed, and then nothing is imported: the outputs are the fixed record's.
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.option") == 1);
	current.options["format"] = "pcx";
	TEST_EXPECT(save_import_sidecar(root + "/art/logo.png.import", current, error));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.option") == 0 && view.project.imports->size() == 1 && !(*view.project.imports)[0].reimported);
	// A PNG that is not one is a finding on its source, and no output.
	TEST_EXPECT(editor_test::write_text(root + "/art/broken.png", "not a png"));
	TEST_EXPECT(mark_for_import(root + "/art/broken.png"));
	editor_test::handle_to_end(session, request::rescan());
	bool broken_reported = false;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == "import.decode" && d.asset == "art/broken.png") broken_reported = true;
	TEST_EXPECT(broken_reported && view.project.imports->size() == 2);
	// A Reimport of one source: another source's failure stays a Problems row but is not
	// this request's outcome.
	EditorRequest one = request::reimport("logo.png");
	one.force = true;
	editor_test::handle_to_end(session, one);
	TEST_EXPECT(session.outcome().done() &&
			count_code(view.findings.diagnostics, "import.decode") == 1);
	TEST_EXPECT(imported_source(view, "art/logo.png") && imported_source(view, "art/logo.png")->reimported);
	// A menu's .tga the project lacks loads its .dds (from the first dot), as retail's
	// loader does (a texture of no model finds it as its game loader does: the sky maps'
	// archive loader takes the .dds sibling first, a loader that reads the name alone does
	// not, nor does a use whose loader is not witnessed, ADR 0046 S18); with the .tga there the
	// .tga is the file.
	TEST_EXPECT(editor_test::write_bytes(root + "/art/sky.dds", std::vector<uint8_t>{'D', 'D', 'S', ' '}));
	editor_test::handle_to_end(session, request::rescan());
	std::string sky;
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "sky.tga", std::string(),
						&sky) == ReferenceStatus::Present &&
			sky == "art/sky.dds");
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "sky.tga", std::string(), &sky,
						texture_loader_arg(opennova::renderer::TextureLoader::ArchiveSelfAlpha)) ==
					ReferenceStatus::Present &&
			sky == "art/sky.dds");
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "sky.tga", std::string(), nullptr,
						texture_loader_arg(opennova::renderer::TextureLoader::Particle)) ==
			ReferenceStatus::Missing);
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::Texture, "sky.tga", std::string(), &sky) ==
			ReferenceStatus::Missing);
	TEST_EXPECT(editor_test::write_bytes(root + "/art/sky.tga", std::vector<uint8_t>(18, 0)));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.findings.graph->resolve(ReferenceKind::MenuTexture, "sky.tga", std::string(),
						&sky) == ReferenceStatus::Present &&
			sky == "art/sky.tga");
	return 0;
}

namespace {

// A test's importer of many inputs (S13 A8): a `.duo` source names the files it joins, a line each
// (its folder's paths), and its one output, `<stem>.txt`, holds their bytes one after another. Its
// runs are counted.
int duo_runs = 0;

bool run_duo(ImportContext &context, ImportProduct &out) {
	++duo_runs;
	const std::string listing(context.source().begin(), context.source().end());
	ImportOutput output;
	output.name = fs::path(context.source_name()).stem().generic_string() + ".txt";
	size_t start = 0;
	while (start < listing.size()) {
		size_t end = listing.find('\n', start);
		if (end == std::string::npos) end = listing.size();
		const std::string line = listing.substr(start, end - start);
		start = end + 1;
		if (line.empty()) continue;
		std::vector<uint8_t> bytes;
		if (!context.read(line, bytes)) return false;
		output.bytes.insert(output.bytes.end(), bytes.begin(), bytes.end());
	}
	out.outputs.push_back(std::move(output));
	return true;
}

const std::vector<Importer> &duo_table() {
	static const std::vector<Importer> table = [] {
		Importer duo;
		duo.id = "duo";
		duo.version = 1;
		duo.extensions = {".duo"};
		duo.run = run_duo;
		return std::vector<Importer>{duo};
	}();
	return table;
}

} // namespace

// S13 A8: one import of many inputs (ImportContext). A source and the files its importer reads
// through its context are recorded, the files' paths in its import record (`inputs`, each from its
// folder) and the hashes of what was read in the machine-local import cache (so an input's edit
// changes no committed record), so the pass imports again when either input (or the source)
// changes, and when neither does reads neither: the cache vouches for an input while its size and
// last write hold, so one written a while ago and rewritten under the same size and time is not read
// (the import cache's rule, a source's before). One written within the settle window
// (io::kFileStampSettle) is never vouched for: rewritten under the stamp it had, it is read and
// imported again. An input no longer named stops counting; one gone, or one outside the project,
// fails the import with import.input.
static int test_import_inputs() {
	editor_test::TempProjectDir dir("opennova_editor_import_inputs");
	const std::string root = dir.file("project");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Inputs", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	TEST_EXPECT(editor_test::write_text(root + "/art/pair.duo", "left.txt\nsub/right.txt\n"));
	TEST_EXPECT(editor_test::write_text(root + "/art/left.txt", "L") && editor_test::write_text(root + "/art/sub/right.txt", "R"));
	ImportSidecar record;
	record.importer = "duo";
	record.version = 1;
	TEST_EXPECT(save_import_sidecar(root + "/art/pair.duo.import", record, error));
	TEST_EXPECT(editor_test::backdate_tree(root, std::chrono::hours(1))); // written a while ago
	const auto pass = [&]() {
		ImportPass walk(paths, doc, false, std::string(), duo_table());
		while (!walk.step(kWholeWalkStep)) {
		}
		return walk.take();
	};
	const auto output = [&](const ImportRunResult &run) {
		return run.sources.size() == 1 && run.sources[0].outputs.size() == 1 ? read_text(root + "/" + run.sources[0].outputs[0])
		                                                                      : std::string();
	};
	const auto hash_of = [](const std::string &text) {
		return io::fnv1a64_bytes(io::kFnv1a64Offset, text.data(), text.size());
	};
	duo_runs = 0;
	ImportRunResult run = pass();
	TEST_EXPECT(run.reimported == 1 && duo_runs == 1 && run.sources.size() == 1 && run.sources[0].ok);
	TEST_EXPECT(run.sources[0].inputs == std::vector<std::string>({"art/left.txt", "art/sub/right.txt"}));
	TEST_EXPECT(output(run) == "LR");
	TEST_EXPECT(load_import_sidecar(root + "/art/pair.duo.import", record, error));
	TEST_EXPECT(record.inputs == std::vector<std::string>({"left.txt", "sub/right.txt"}));
	const std::string record_text = read_text(root + "/art/pair.duo.import");
	const std::string cache_text = read_text(paths.import_cache_file);
	TEST_EXPECT(record_text.find("\"inputs\"") != std::string::npos);
	for (const char *content : {"L", "R"}) {
		TEST_EXPECT(record_text.find(io::hex64(hash_of(content))) == std::string::npos);
		TEST_EXPECT(cache_text.find(io::hex64(hash_of(content))) != std::string::npos);
	}

	// Neither input changed: nothing imported. One rewritten under the size and the last write it
	// had: the cache vouches for it, so it is not read and nothing is imported either.
	run = pass();
	TEST_EXPECT(run.reimported == 0 && duo_runs == 1 && output(run) == "LR");
	const fs::file_time_type written = fs::last_write_time(root + "/art/left.txt");
	TEST_EXPECT(editor_test::write_text(root + "/art/left.txt", "X"));
	fs::last_write_time(root + "/art/left.txt", written);
	run = pass();
	TEST_EXPECT(run.reimported == 0 && duo_runs == 1);
	// The first input changes: imported again; then the second.
	TEST_EXPECT(editor_test::write_text(root + "/art/left.txt", "LL"));
	run = pass();
	TEST_EXPECT(run.reimported == 1 && duo_runs == 2 && output(run) == "LLR");
	TEST_EXPECT(editor_test::write_text(root + "/art/sub/right.txt", "RRR"));
	run = pass();
	TEST_EXPECT(run.reimported == 1 && duo_runs == 3 && output(run) == "LLRRR");
	run = pass();
	TEST_EXPECT(run.reimported == 0 && duo_runs == 3);
	// A file whose last write is within the settle window of a pass (here a minute ahead, as a file
	// system clock that stood still would leave it) is never vouched for: rewritten under the size
	// and the last write it had, it is still read and imported again.
	const fs::file_time_type ahead = fs::file_time_type::clock::now() + std::chrono::minutes(1);
	fs::last_write_time(root + "/art/sub/right.txt", ahead);
	run = pass();
	TEST_EXPECT(run.reimported == 0 && duo_runs == 3);
	TEST_EXPECT(editor_test::write_text(root + "/art/sub/right.txt", "QQQ"));
	fs::last_write_time(root + "/art/sub/right.txt", ahead);
	run = pass();
	TEST_EXPECT(run.reimported == 1 && duo_runs == 4 && output(run) == "LLQQQ");
	// The source names one input now: imported again, its record listing that one; the other then
	// changes without an import.
	TEST_EXPECT(editor_test::write_text(root + "/art/pair.duo", "left.txt\n"));
	run = pass();
	TEST_EXPECT(run.reimported == 1 && duo_runs == 5 && output(run) == "LL");
	TEST_EXPECT(run.sources[0].inputs == std::vector<std::string>({"art/left.txt"}));
	TEST_EXPECT(editor_test::write_text(root + "/art/sub/right.txt", "R"));
	run = pass();
	TEST_EXPECT(run.reimported == 0 && duo_runs == 5);
	// An input gone fails the import (the record and the outputs kept as they were).
	fs::remove(root + "/art/left.txt");
	run = pass();
	TEST_EXPECT(run.reimported == 0 && duo_runs == 6 && !run.sources[0].ok && count_code(run.diagnostics, "import.input") == 1);
	TEST_EXPECT(!run.diagnostics.empty() && run.diagnostics[0].asset == "art/pair.duo");
	// One outside the project is no input.
	TEST_EXPECT(editor_test::write_text(dir.file("outside.txt"), "O"));
	TEST_EXPECT(editor_test::write_text(root + "/art/pair.duo", "../../outside.txt\n"));
	run = pass();
	TEST_EXPECT(!run.sources[0].ok && count_code(run.diagnostics, "import.input") == 1);
	return 0;
}

// S13 A8: the image importer's TGA output (`format tga`): the 32-bit TGA formats/tga writes of the
// decoded PNG, named after the source, its alpha kept (no import.alpha_dropped), the PCX the PCX
// output made removed, the scan listing it as a texture the build packs; back to PCX the alpha is
// dropped, and said.
static int test_image_tga_output() {
	editor_test::TempProjectDir dir("opennova_editor_import_tga");
	const std::string root = dir.file("project");
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Targa", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	PngSpec spec;
	spec.width = 3;
	spec.height = 2;
	for (uint32_t y = 0; y < 2; ++y) {
		spec.rows.push_back(0);
		for (uint32_t x = 0; x < 3; ++x)
			for (const uint32_t channel : {x * 80, y * 120, 33u, 64u + x * 60}) spec.rows.push_back(uint8_t(channel));
	}
	const std::vector<uint8_t> png = make_png(spec);
	TEST_EXPECT(editor_test::write_bytes(root + "/art/glow.png", png) && mark_for_import(root + "/art/glow.png"));
	ImportRunResult run = run_imports(paths, doc);
	TEST_EXPECT(run.reimported == 1 && count_code(run.diagnostics, "import.alpha_dropped") == 1);
	TEST_EXPECT(run.sources.size() == 1 && run.sources[0].outputs.size() == 1);
	const std::string pcx = run.sources.empty() || run.sources[0].outputs.empty() ? std::string() : run.sources[0].outputs[0];
	TEST_EXPECT(fs::path(pcx).filename() == "glow.pcx" && fs::is_regular_file(root + "/" + pcx));

	ImportSidecar record;
	TEST_EXPECT(load_import_sidecar(root + "/art/glow.png.import", record, error));
	record.options["format"] = "tga";
	TEST_EXPECT(save_import_sidecar(root + "/art/glow.png.import", record, error));
	run = run_imports(paths, doc);
	TEST_EXPECT(run.reimported == 1 && count_code(run.diagnostics, "import.alpha_dropped") == 0 && run.sources[0].ok);
	TEST_EXPECT(run.sources[0].outputs.size() == 1);
	const std::string tga = run.sources[0].outputs.empty() ? std::string() : run.sources[0].outputs[0];
	TEST_EXPECT(fs::path(tga).filename() == "glow.tga" && !fs::exists(root + "/" + pcx));
	RgbaImage decoded;
	std::string message;
	TEST_EXPECT(decode_png(png, decoded, message));
	std::vector<uint8_t> expected, written;
	TEST_EXPECT(opennova::tga::tga_write_rgba32(decoded.pixels.data(), 3, 2, expected, message));
	TEST_EXPECT(io::read_file_bytes(root + "/" + tga, written, message) && written == expected);
	const AssetScan scan = scan_project_assets(paths, doc);
	TEST_EXPECT(scan.find("glow.tga") && scan.find("glow.tga")->kind == AssetKind::Texture &&
	            scan.find("glow.tga")->imported_from == "art/glow.png");
	TEST_EXPECT(scan.find("glow.png") && scan.find("glow.png")->kind == AssetKind::ImportSource);
	TEST_EXPECT(load_import_sidecar(root + "/art/glow.png.import", record, error) &&
	            record.outputs == std::vector<std::string>{"glow.tga"});
	record.options["format"] = "pcx";
	TEST_EXPECT(save_import_sidecar(root + "/art/glow.png.import", record, error));
	run = run_imports(paths, doc);
	TEST_EXPECT(run.reimported == 1 && count_code(run.diagnostics, "import.alpha_dropped") == 1 &&
	            !fs::exists(root + "/" + tga) && fs::is_regular_file(root + "/" + pcx));

	// The source and its record moved outside the editor: the next pass makes its outputs under its new place
	// and removes the folder no source names any more (ADR 0046 S23 D); a pass over some sources alone does not.
	const std::string old_dir = import_output_dir(paths, "art/glow.png");
	TEST_EXPECT(editor_test::write_text(root + "/.opennova/imported/0123456789abcdef/stray.pcx", "stray"));
	std::error_code ec;
	fs::create_directories(root + "/moved", ec);
	fs::rename(root + "/art/glow.png", root + "/moved/glow.png", ec);
	fs::rename(root + "/art/glow.png.import", root + "/moved/glow.png.import", ec);
	ImportPass limited(paths, doc);
	limited.limit_to({"moved/glow.png"});
	while (!limited.step(1u << 20)) {
	}
	TEST_EXPECT(fs::is_directory(root + "/" + old_dir) && fs::exists(root + "/.opennova/imported/0123456789abcdef"));
	run = run_imports(paths, doc);
	TEST_EXPECT(run.sources.size() == 1 && run.sources[0].source == "moved/glow.png" && run.sources[0].ok);
	TEST_EXPECT(!fs::exists(root + "/" + old_dir) && !fs::exists(root + "/.opennova/imported/0123456789abcdef"));
	TEST_EXPECT(fs::is_directory(root + "/" + import_output_dir(paths, "moved/glow.png")));
	return 0;
}

// S13 A8: an output is named after its source's stem, or after it with an underscore and a suffix
// of its own where an importer makes several (a particle layout's frames, a terrain's colour
// tiles), and follows a rename of the source either way; any other keeps its name.
static int test_renamed_import_outputs() {
	TEST_EXPECT(renamed_import_output("logo.pcx", "logo.png", "brand.png") == "brand.pcx");
	TEST_EXPECT(renamed_import_output("LOGO.pcx", "logo.png", "brand.png") == "brand.pcx");
	TEST_EXPECT(renamed_import_output("smoke_01.tga", "smoke.png", "fire.png") == "fire_01.tga");
	TEST_EXPECT(renamed_import_output("Smoke_02.TGA", "smoke.png", "fire.png") == "fire_02.TGA");
	TEST_EXPECT(renamed_import_output("smokey.tga", "smoke.png", "fire.png") == "smokey.tga");
	TEST_EXPECT(renamed_import_output("smoke_.tga", "smoke.png", "fire.png") == "smoke_.tga");
	TEST_EXPECT(renamed_import_output("other.tga", "smoke.png", "fire.png") == "other.tga");
	return 0;
}

// S13 A8: a project folder some 218 characters long, its own files short of Windows' MAX_PATH
// (260 characters), its import's outputs under .opennova/imported/ past it: the output is written,
// found by the next pass (which imports nothing) and listed by the scan, every call on it taking the
// system path (system_path).
static int test_deep_root_import() {
	editor_test::TempProjectDir dir("opennova_editor_import_deep");
	std::string root = dir.root();
	while (root.size() + 1 < 218) root += "/" + std::string(std::min<size_t>(60, 218 - root.size() - 1), 'd');
	ProjectDocument doc;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Deep", "jo", doc, error));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	PngSpec spec;
	spec.width = 1;
	spec.height = 1;
	spec.rows = {0, 200, 100, 50, 255};
	TEST_EXPECT(editor_test::write_bytes(root + "/art/glow.png", make_png(spec)) && mark_for_import(root + "/art/glow.png"));
	ImportRunResult run = run_imports(paths, doc);
	TEST_EXPECT(run.reimported == 1 && run.sources.size() == 1 && run.sources[0].ok && run.sources[0].outputs.size() == 1);
	const std::string output = run.sources.empty() || run.sources[0].outputs.empty() ? std::string()
	                                                                                 : root + "/" + run.sources[0].outputs[0];
	std::error_code ec;
	TEST_EXPECT(output.size() > 259 && fs::is_regular_file(system_path(output), ec));
	std::printf("editor_import: a project folder %zu characters long, its import's output at %zu\n", root.size(),
	            output.size());
	run = run_imports(paths, doc);
	TEST_EXPECT(run.reimported == 0 && run.sources.size() == 1 && run.sources[0].ok);
	const AssetScan scan = scan_project_assets(paths, doc);
	TEST_EXPECT(scan.find("glow.pcx") && scan.find("glow.pcx")->imported_from == "art/glow.png" &&
	            scan.find("glow.pcx")->size_bytes > 0);
	TEST_EXPECT(count_code(scan.diagnostics, "import.output_missing") == 0);
	return 0;
}

// A project whose own files lie past Windows' MAX_PATH (a folder some 246 characters long, its
// project file and its files under folders past 260): the project is made and opened, every missing
// required file made, the scan walks and lists them (their sizes read), the import pass finds a source
// there and imports it, a document opens and a file is made there, and a build packs them, every call
// on them taking the system path (system_path).
static int test_deep_project_files() {
	editor_test::TempProjectDir dir("opennova_editor_deep_files");
	std::string root = dir.root();
	while (root.size() + 1 < 246) root += "/" + std::string(std::min<size_t>(60, 246 - root.size() - 1), 'f');
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(root, "Deep files"));
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.open);
	if (!view.project.open) return 1;
	editor_test::create_missing_files(session);
	if (view.project.requirements->required_missing != 0)
		for (const Diagnostic &d : view.findings.diagnostics)
			if (d.severity == DiagnosticSeverity::Error) std::printf("  deep: %s %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(view.project.requirements->required_missing == 0 && view.project.requirements->required_total > 0);
	const AssetEntry *table = view.project.scan->find("gametext.bin");
	TEST_EXPECT(table && (root + "/" + table->relative_path).size() > 260 && table->size_bytes > 0 &&
	            table->kind == AssetKind::Strings);
	// The entry lives in the scan the view holds now, which the Rescan below replaces (and frees):
	// what the last line says of it is kept by value.
	const std::string table_path = table ? table->relative_path : std::string();
	// An import source under a folder past MAX_PATH: found by the pass, imported, its output listed.
	PngSpec spec;
	spec.width = 1;
	spec.height = 1;
	spec.rows = {0, 200, 100, 50, 255};
	const std::vector<uint8_t> png = make_png(spec);
	std::string write_error;
	const std::string source = root + "/art/source/glow.png";
	TEST_EXPECT(source.size() > 259 && io::ensure_directory(root + "/art/source", write_error) &&
	            io::write_file_atomic(source, png.data(), png.size(), write_error) && mark_for_import(source));
	editor_test::handle_to_end(session, request::rescan());
	const AssetEntry *output = view.project.scan->find("glow.pcx");
	TEST_EXPECT(output && output->imported_from == "art/source/glow.png" && output->size_bytes > 0);
	// A document opens, and a new file is made, there.
	session.handle(request::open_document("gametext.bin"));
	TEST_EXPECT(session.outcome().done() && session.document_base_for("gametext.bin") != nullptr);
	session.handle(request::create_file("deep_extra.mnu", "menu"));
	TEST_EXPECT(session.outcome().done() && session.document_base_for("deep_extra.mnu") != nullptr);
	session.run_operations();
	// And the build packs them.
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(view.activity.has_build && view.activity.last_build->ok);
	if (view.activity.has_build && !view.activity.last_build->ok)
		for (const Diagnostic &d : view.activity.last_build->diagnostics)
			std::printf("  build: %s %s\n", d.code().c_str(), d.message.c_str());
	std::printf("editor_import: a project folder %zu characters long, its string table at %zu\n", root.size(),
	            table_path.empty() ? size_t(0) : root.size() + 1 + table_path.size());
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
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Lifetime"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const std::string source = root + "/art/logo.png";
	const std::string sidecar_path = source + ".import";
	TEST_EXPECT(editor_test::write_bytes(source, gradient_png(8, 8)));
	TEST_EXPECT(mark_for_import(source));
	// Written a while ago, so the cache keeps it by its stamp (S13 A8: a file written within the
	// settle window is read by every pass until it settles).
	TEST_EXPECT(editor_test::backdate(source, std::chrono::hours(2)));
	editor_test::handle_to_end(session, request::rescan());
	const std::string first = read_text(sidecar_path);
	TEST_EXPECT(!first.empty() && first.find("modified") == std::string::npos && first.find("size") == std::string::npos);
	TEST_EXPECT(fs::is_regular_file(paths.import_cache_file));
	ImportSidecar before;
	Diagnostic error;
	TEST_EXPECT(load_import_sidecar(sidecar_path, before, error) && before.source_hash != 0);
	// A touched source (a checkout, a copy): nothing imports and the record keeps its bytes.
	fs::last_write_time(source, fs::last_write_time(source) + std::chrono::hours(1));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(view.project.imports->size() == 1 && !(*view.project.imports)[0].reimported && read_text(sidecar_path) == first);
	// The cache lost with the outputs (a fresh clone): imported again, the record unchanged.
	std::error_code ec;
	fs::remove_all(paths.imported_dir, ec);
	fs::remove(paths.import_cache_file, ec);
	editor_test::handle_to_end(session, request::rescan());
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
		io::JsonValue *files = cache.get("files");
		TEST_EXPECT(files && files->is_array() && !files->array.empty());
		if (files)
			for (io::JsonValue &item : files->array) item.set("hash", io::JsonValue::make_string(io::hex64(1)));
		TEST_EXPECT(editor_test::write_text(paths.import_cache_file, io::json_write(cache)));
		editor_test::handle_to_end(session, request::rescan());
		TEST_EXPECT(view.project.imports->size() == 1 && read_text(sidecar_path) == first);
	}
	// New content of the same size: the record's hash changes.
	TEST_EXPECT(editor_test::write_bytes(source, gradient_png(8, 8, 99)));
	editor_test::handle_to_end(session, request::rescan());
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
		editor_test::handle_to_end(session, request::rescan());
		TEST_EXPECT(read_text(sidecar_path) == typo &&
				count_code(view.findings.diagnostics, "import.sidecar") == 1);
		TEST_EXPECT(view.project.imports->size() == 1 && !(*view.project.imports)[0].ok &&
				!(*view.project.imports)[0].reimported);
		TEST_EXPECT(view.project.scan->find("logo.pcx") == nullptr);
	}
	EditorRequest forced = request::reimport("logo.png");
	forced.force = true;
	const ActionOutcome refused = editor_test::handle_to_end(session, forced);
	TEST_EXPECT(!refused.done() && count_code(refused.findings, "import.sidecar") == 1);
	TEST_EXPECT(read_text(sidecar_path) == typo);
	TEST_EXPECT(editor_test::write_text(sidecar_path, good));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(count_code(view.findings.diagnostics, "import.sidecar") == 0 &&
			view.project.imports->size() == 1 && (*view.project.imports)[0].ok &&
			!(*view.project.imports)[0].reimported &&
			view.project.scan->find("logo.pcx") != nullptr);
	// A menu names the output; renaming the source renames the output with it.
	editor_test::handle_to_end(session, request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	NodeAddress exit;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit));
	EditorRequest set =
			request::edit_record(menu->path(), menu_test::image_edits(*menu, exit, "logo.pcx"));
	editor_test::handle_to_end(session, set);
	editor_test::handle_to_end(session, request::save_all());
	TEST_EXPECT(count_code(view.findings.diagnostics, "reference.missing") == 0);
	const std::string old_dir = (*view.project.imports)[0].output_dir;
	{
		const RenamePlan plan = plan_rename(paths, *view.project.scan, *view.findings.graph, "art/logo.png", "logo2.png");
		TEST_EXPECT(plan.ok() && plan.sidecar == "art/logo.png.import" && plan.new_sidecar == "art/logo2.png.import");
		TEST_EXPECT(plan.outputs.size() == 1 && plan.outputs[0].old_name == "logo.pcx" && plan.outputs[0].new_name == "logo2.pcx");
		TEST_EXPECT(plan.sites.size() == 1 && plan.sites[0].after == "logo2.pcx" && plan.sites[0].target == plan.outputs[0].path);
		// The output's new name must fit the archives and be free.
		TEST_EXPECT(editor_test::write_text(root + "/taken.pcx", "x"));
		editor_test::handle_to_end(session, request::rescan());
		const RenamePlan taken = plan_rename(paths, *view.project.scan, *view.findings.graph, "art/logo.png", "taken.png");
		TEST_EXPECT(!taken.ok() && taken.refusals.front().code() == "rename.exists");
		fs::remove(root + "/taken.pcx", ec);
		editor_test::handle_to_end(session, request::rescan());
	}
	editor_test::handle_to_end(session, request::rename_asset("art/logo.png", "logo2.png"));
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
	editor_test::handle_to_end(session, request::rescan());
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
	// @ 0x75b1e5], and so does the import. A music bank beside it is a file the game ships
	// loose and reads from there (ADR 0046 S14: install_loose_kind), so the install lists it; a
	// player's save is never the game's to import.
	TEST_EXPECT(editor_test::write_text(retail + "/note.txt", "loose"));
	TEST_EXPECT(editor_test::write_text(retail + "/MENUMUS.SBF", "music") && editor_test::write_text(retail + "/player.sav", "save"));
	TEST_EXPECT(install_loose_kind(AssetKind::MusicBank) && install_loose_kind(AssetKind::Video) &&
	            !install_loose_kind(AssetKind::PlayerSave) && !install_loose_kind(AssetKind::Config) &&
	            !install_loose_kind(AssetKind::Text) && !install_loose_kind(AssetKind::Texture));
	TEST_EXPECT(list_install_loose_files(retail) == std::vector<std::string>{"MENUMUS.SBF"});
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Retail"));
	const SessionView &view = session.view();
	TEST_EXPECT(view.project.retail_files.empty());
	editor_test::handle_to_end(session, request::preview_install_import());
	TEST_EXPECT(!view.dialogs.import_preview.open &&
			view.findings.diagnostics.back().code() == "import.install");
	editor_test::set_game_install(session, retail);
	TEST_EXPECT(
			view.project.retail_files.size() == 5); // the archive itself is not an importable file; the bank is
	std::vector<Diagnostic> diagnostics;
	const std::vector<ImportChoice> sources = list_retail_import_choices(retail, *view.project.document, diagnostics);
	TEST_EXPECT(diagnostics.empty() && sources.size() == 5 && sources[0].install && sources[0].path == retail &&
	            sources.back().entry == "MENUMUS.SBF");
	// The whole list to choose from, none chosen: nothing planned yet.
	editor_test::handle_to_end(session, request::preview_install_import());
	TEST_EXPECT(view.dialogs.import_preview.open &&
			view.dialogs.import_preview.choices.size() == 5 &&
			view.dialogs.import_preview.choices[0].install && !view.dialogs.import_preview.all);
	TEST_EXPECT(view.dialogs.import_preview.roots.empty() &&
			view.dialogs.import_preview.plan->rows.empty());
	// The requirement gametext.bin, missing in the project, chosen from the list (the plan made
	// again, the list kept) and imported from the game data.
	const RequirementRow *row = nullptr;
	for (const RequirementRow &candidate : view.project.requirements->rows) if (candidate.name == "gametext.bin") row = &candidate;
	TEST_EXPECT(row && row->state == RequirementState::Missing);
	EditorRequest choose = request::of(EditorRequestKind::PlanImport);
	ImportChoice source;
	source.path = retail;
	source.entry = "gametext.bin";
	source.install = true;
	choose.imports.push_back(source);
	source.entry = "note.txt";
	choose.imports.push_back(source);
	editor_test::handle_to_end(session, choose);
	TEST_EXPECT(view.dialogs.import_preview.open && view.dialogs.import_preview.choices.size() == 5 && view.dialogs.import_preview.roots.size() == 2);
	TEST_EXPECT(view.dialogs.import_preview.plan->rows.size() == 2 && view.dialogs.import_preview.plan->rows[0].found_in == "the game install");
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = choose.imports;
	editor_test::handle_to_end(session, import);
	TEST_EXPECT(session.outcome().done() && !view.dialogs.import_preview.open);
	for (const RequirementRow &candidate : view.project.requirements->rows) if (candidate.name == "gametext.bin") row = &candidate;
	TEST_EXPECT(row && row->state == RequirementState::Present);
	std::string text, message;
	TEST_EXPECT(view.project.scan->find("note.txt") && io::read_file_text(view.project.root + "/" + view.project.scan->find("note.txt")->relative_path, text, message) && text == "retail");
	// A name the install does not have.
	import.imports.clear();
	source.entry = "absent.txt";
	import.imports.push_back(source);
	editor_test::handle_to_end(session, import);
	TEST_EXPECT(view.findings.diagnostics.back().code() == "import.read");
	// A PNG of the game install, and one of an archive, is the game's own file: copied as it is,
	// with no import record, so it is a texture (a loose PNG from the disk becomes an import
	// source with its record: test_import_pass, the command line's import).
	import.imports.clear();
	source.entry = "splash.png";
	import.imports.push_back(source);
	ImportChoice member;
	member.path = retail + "/resource.pff";
	member.entry = "icon.png";
	import.imports.push_back(member);
	editor_test::handle_to_end(session, import);
	TEST_EXPECT(session.outcome().done());
	for (const char *name : {"splash.png", "icon.png"}) {
		const AssetEntry *png = view.project.scan->find(name);
		TEST_EXPECT(png && png->kind == AssetKind::Texture);
		TEST_EXPECT(png && !fs::exists(fs::path(view.project.root) / (png->relative_path + kImportSidecarSuffix)));
	}
	TEST_EXPECT(view.project.imports->empty());
	// Everything the install has, chosen at once (ADR 0046 S14): the archives' files and the loose
	// ones the game ships beside them, none to choose from, no walk (the setting changes nothing:
	// a walk of every file finds nothing not chosen); imported as the plan has them, nothing echoed
	// back (planned). A file the project holds already is marked held and not taken by default
	// (review F2): the project's file is left as it is, the rest comes; Replace takes them too.
	{
		const DialogsView::ImportPreview &preview = view.dialogs.import_preview;
		const std::string note_path = view.project.root + "/" + view.project.scan->find("note.txt")->relative_path;
		TEST_EXPECT(editor_test::write_text(note_path, "edited in the project"));
		editor_test::handle_to_end(session, request::rescan());
		editor_test::handle_to_end(session, request::import_whole_install());
		TEST_EXPECT(preview.open && preview.all && preview.choices.empty() && preview.roots.size() == 5 &&
		            !preview.with_dependencies && preview.plan->rows.size() == 5);
		const ImportPlanRow *bank = nullptr;
		size_t held = 0;
		for (const ImportPlanRow &row : preview.plan->rows) {
			if (row.name == "MENUMUS.SBF") bank = &row;
			if (row.held) {
				++held;
				TEST_EXPECT(!row.selected && row.problem.empty() && row.destination == view.project.scan->find(row.name)->relative_path);
			}
		}
		TEST_EXPECT(held == 4);
		TEST_EXPECT(bank && bank->state == ImportPlanRow::State::Selected && bank->kind == AssetKind::MusicBank && bank->selected &&
		            !bank->held && bank->size == 5 && bank->found_in == "the game install" && bank->source.install &&
		            bank->problem.empty());
		TEST_EXPECT(view_section_to_json(view, ViewSection::Import).get_bool("all", false));
		const uint64_t planned = view.activity.last_operation.id;
		editor_test::handle_to_end(session, request::set_import_dependencies(true));
		TEST_EXPECT(preview.open && preview.all && !preview.with_dependencies && view.activity.last_operation.id == planned);
		// Without Replace existing files: the bank comes, the project's own files stay as they are, no
		// refusal.
		editor_test::handle_to_end(session, request::import_planned(preview.plan_serial));
		TEST_EXPECT(view.activity.last_operation.end == OperationEnd::Done && !preview.open &&
		            count_code(view.activity.last_operation.findings, "import.exists") == 0);
		const AssetEntry *music = view.project.scan->find("MENUMUS.SBF");
		TEST_EXPECT(music && music->kind == AssetKind::MusicBank && !view.project.scan->find("player.sav"));
		TEST_EXPECT(music && io::read_file_text(view.project.root + "/" + music->relative_path, text, message) && text == "music");
		TEST_EXPECT(io::read_file_text(note_path, text, message) && text == "edited in the project");
		// With replace and the held rows unchecked (the plan's own checks): an unchecked row is never taken, so the
		// edited file stays as it is (review X1).
		editor_test::handle_to_end(session, request::import_whole_install());
		TEST_EXPECT(preview.open && preview.plan->rows.size() == 5);
		editor_test::handle_to_end(session, request::import_planned(preview.plan_serial, true));
		TEST_EXPECT(view.activity.last_operation.end == OperationEnd::Done && !preview.open);
		TEST_EXPECT(io::read_file_text(note_path, text, message) && text == "edited in the project");
		// Replace existing files checked (the dialog's checkbox, the workspace's): the held files are written over,
		// the edited one too, with no replace asked (the checked held rows replace: review X2).
		editor_test::handle_to_end(session, request::import_whole_install());
		TEST_EXPECT(session.handle(request::set_workspace(R"({"import": {"replace_existing": true}})")) && session.outcome().done());
		editor_test::handle_to_end(session, request::import_planned(preview.plan_serial));
		TEST_EXPECT(view.activity.last_operation.end == OperationEnd::Done && !preview.open);
		TEST_EXPECT(io::read_file_text(note_path, text, message) && text == "retail");
		// The same bytes imported again without Replace: held, not an error (the source is read
		// before a file of its name is refused).
		EditorRequest again = request::of(EditorRequestKind::ImportFiles);
		again.imports = {source};
		again.imports.back().entry = "note.txt";
		editor_test::handle_to_end(session, again);
		TEST_EXPECT(session.outcome().done() && view.activity.last_operation.end == OperationEnd::Done &&
		            count_code(view.activity.last_operation.findings, "import.exists") == 0);
		// Nothing planned: a planned import is refused.
		editor_test::handle_to_end(session, request::import_planned(0));
		TEST_EXPECT(!session.outcome().done() && view.findings.diagnostics.back().code() == "import.not_planned");
		// A request asking for two things at once is refused, never served in part (review F14): every
		// file and some by name, every file and a walk; an import naming nothing and planning nothing.
		EditorRequest both = request::import_whole_install();
		both.names = {"note.txt"};
		editor_test::handle_to_end(session, both);
		TEST_EXPECT(!session.outcome().done() && count_code(session.outcome().findings, "import.request") == 1 && !preview.open);
		EditorRequest walked = request::import_whole_install();
		walked.with_dependencies = true;
		editor_test::handle_to_end(session, walked);
		TEST_EXPECT(!session.outcome().done() && count_code(session.outcome().findings, "import.request") == 1 && !preview.open);
		editor_test::handle_to_end(session, request::import_files({}));
		TEST_EXPECT(!session.outcome().done() && count_code(session.outcome().findings, "import.request") == 1);
	}
	// A directory that is no install.
	editor_test::set_game_install(session, dir.file("empty"));
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
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Scenes"));
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
	TEST_EXPECT(io::read_file_bytes(root + "/models/spinner.3di", model, message));
	opennova::threedi::Threedi3di3 check{};
	TEST_EXPECT(opennova::threedi::threedi_3di3_read_memory(model.data(), model.size(), &check) == 0);
	opennova::threedi::threedi_3di3_free(&check);

	// Again: the same outputs, left as they are.
	r = import_assets({{source + "/spinner.o3d", {}}}, paths, document, false);
	TEST_EXPECT(!has_error(r.diagnostics) && r.imported.empty());

	// A scene that does not read names its line and writes nothing.
	TEST_EXPECT(editor_test::write_text(source + "/broken.o3d", "o3d 2\nmodel broken\nlod 0\nbogus 1\n"));
	r = import_assets({{source + "/broken.o3d", {}}}, paths, document, false);
	bool line = false;
	for (const Diagnostic &d : r.diagnostics) line = line || (d.code() == "import.scene" && d.line == 4);
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
			said = said || (d.code() == "import.scene" && d.message.find(words) != std::string::npos);
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

// A Black Hawk Down GP model imported from the disk is migrated to 3DI3 as it comes in (ADR 0027
// as amended; the converter claims it by its bytes): the project gets a 3DI3 of the source's name,
// each migration note an import note; a 3DI3 of the same extension is copied as it is; a GP
// model the reader refuses writes nothing and says why; the plan lists the migrated model.
static int test_gp_import() {
	editor_test::TempProjectDir dir("opennova_editor_gp_import");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Bhd"));
	const std::string root = session.view().project.root;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const ProjectDocument &document = *session.view().project.document;
	const std::string source = dir.file("bhd");
	fs::create_directories(source);
	const std::vector<uint8_t> gp = gp_test::gpm_model();
	std::string message;
	TEST_EXPECT(editor_test::write_bytes(source + "/crate.3di", gp));

	ImportResult r = import_assets({{source + "/crate.3di", {}}}, paths, document, false);
	bool error = false, note = false;
	for (const Diagnostic &d : r.diagnostics) {
		error = error || d.severity == DiagnosticSeverity::Error;
		note = note || (d.code() == "import.migrate_note" && d.message.find("flag 0x2") != std::string::npos);
	}
	TEST_EXPECT(!error && note);
	TEST_EXPECT(r.imported.size() == 1 && r.imported[0] == "models/crate.3di");
	std::vector<uint8_t> model;
	TEST_EXPECT(io::read_file_bytes(root + "/models/crate.3di", model, message));
	TEST_EXPECT(model.size() >= 4 && std::string(model.begin(), model.begin() + 4) == "3DI3");
	opennova::threedi::Threedi3di3 check{};
	TEST_EXPECT(opennova::threedi::threedi_3di3_read_memory(model.data(), model.size(), &check) == 0);
	TEST_EXPECT(check.lod_count == 1 && check.material_count == 2);
	opennova::threedi::threedi_3di3_free(&check);

	// A 3DI3 is no GP model: the same extension, copied as it is.
	TEST_EXPECT(editor_test::write_bytes(source + "/native.3di", model));
	r = import_assets({{source + "/native.3di", {}}}, paths, document, false);
	std::vector<uint8_t> copied;
	TEST_EXPECT(r.imported.size() == 1 && io::read_file_bytes(root + "/models/native.3di", copied, message));
	TEST_EXPECT(copied == model);

	// A GP model the reader refuses: nothing written, the reason given.
	const std::vector<uint8_t> broken(gp.begin(), gp.begin() + 0x100);
	TEST_EXPECT(editor_test::write_bytes(source + "/broken.3di", broken));
	r = import_assets({{source + "/broken.3di", {}}}, paths, document, false);
	bool refused = false;
	for (const Diagnostic &d : r.diagnostics)
		refused = refused || (d.code() == "import.migrate" && d.severity == DiagnosticSeverity::Error);
	TEST_EXPECT(refused && r.imported.empty() && !fs::exists(root + "/models/broken.3di"));

	// The plan shows the migrated model made from the source.
	const SessionView &view = session.view();
	ImportChoice choice;
	choice.path = source + "/crate.3di";
	const ImportPlan plan =
	        plan_import({choice}, false, paths, *view.project.document, *view.project.scan, *view.findings.graph, "");
	bool planned = false;
	for (const ImportPlanRow &row : plan.rows)
		planned = planned || (row.name == "crate.3di" && row.made_from == "crate.3di");
	TEST_EXPECT(planned);
	return 0;
}

// The plan of an import with the files it needs (import_plan_test.cpp), and the session's
// import of it (import_apply_test.cpp; its retail leg runs with --retail).
int run_import_plan_tests();
int run_import_apply_tests();

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_import_pass();
	failures += test_import_inputs();
	failures += test_image_tga_output();
	failures += test_renamed_import_outputs();
	failures += test_deep_root_import();
	failures += test_deep_project_files();
	failures += test_import_lifetime();
	failures += test_retail_source();
	failures += test_scene_imports();
	failures += test_gp_import();
	failures += run_import_plan_tests();
	failures += run_import_apply_tests();
	if (failures == 0) std::printf("editor_import: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
