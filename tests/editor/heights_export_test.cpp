// A .cpt's heights written out (session/heights_export.h, the export_heights request) over a session: a closed file's
// atlas as the game's 16-bit .raw, read back through the terrain import's reader, nothing opened; an open document's
// unsaved heights in it, its file and its state as they were; the active document when no path is named; a relative
// file from the project's folder; an import's .cpt, made from a raw so dumped, which a terrain import takes; and each
// refusal writing nothing. A minted Tmap.cpt alone.
#include <editor/documents/terrain_heights_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/cpt/cpt_io.h>
#include <formats/cpt/trngen/heightmap_depth.h>
#include <formats/png/png_encode.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr size_t kRawBytes = size_t(kHeightsSide) * kHeightsSide * 2;

// Raises the input over a rectangle by `raw` and marks it due for its bake (what a brush and its stroke do).
struct RaiseInput : RasterEditPayload {
	HeightsRect rect;
	int raw;
	RaiseInput(HeightsRect r, int by) : rect(r), raw(by) {}
	const char *token() const override { return "test.raise"; }
	bool apply(RasterCanvas &canvas, Diagnostic &error) const override {
		(void)error;
		RasterWriter &writer = canvas.write(kHeightsInput);
		for (int y = rect.y; y < rect.y + rect.h; ++y)
			for (int x = rect.x; x < rect.x + rect.w; ++x)
				writer.set_u16(x, y, uint16_t(std::clamp(int(writer.plane().u16(x, y)) + raw, 0, 65535)));
		auto state = std::make_shared<HeightsState>(*static_cast<const HeightsState *>(canvas.side()));
		state->unbaked.push_back(rect);
		canvas.set_side(std::move(state));
		return true;
	}
};

// A project with Tmap, its .trn and a file named as a .cpt that reads as none.
struct Rig {
	test_temp::TempDir dir;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root, cpt, trn;
	std::vector<uint8_t> file;
	Rig() : dir("opennova_editor_heights_export") {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Heights out"));
		root = session.view().project.root;
		file = test_io::read_file(test_paths_fixture("terrain/tmap/Tmap.cpt"));
		test_io::write_file(root + "/terrain/Tmap.cpt", file);
		test_io::write_file(root + "/terrain/Tmap.trn", test_io::read_file(test_paths_fixture("terrain/tmap/Tmap.trn")));
		test_io::write_text(root + "/terrain/broken.cpt", "CPT1 and no more");
		editor_test::handle_to_end(session, request::rescan());
		cpt = session.view().project.scan->find("Tmap.cpt")->relative_path;
		trn = session.view().project.scan->find("Tmap.trn")->relative_path;
	}
	// The heights a .raw of this export holds, as the terrain import reads them.
	bool read_back(const std::string &path, std::vector<uint16_t> &out) {
		const std::vector<uint8_t> bytes = test_io::read_file(path);
		trngen::HeightmapDepth depth;
		std::string why;
		if (bytes.size() != kRawBytes || !trngen::decode_heightmap_depth(path, bytes, trngen::kDepth8Top, depth, why)) return false;
		out = depth.depth16;
		return true;
	}
	const TerrainHeightsDocument *open_cpt() {
		for (const auto &document : session.view().documents.open)
			if (document->path() == cpt) return dynamic_cast<const TerrainHeightsDocument *>(document.get());
		return nullptr;
	}
};

int test_closed_file() {
	Rig rig;
	CptFile read;
	std::string why;
	TEST_EXPECT(load_cpt(rig.file.data(), rig.file.size(), read, why));
	const std::string to = rig.dir.file("Tmap.raw");
	const ActionOutcome outcome = editor_test::handle_to_end(rig.session, request::export_heights(rig.cpt, to));
	TEST_EXPECT(outcome.done());
	std::vector<uint16_t> heights;
	TEST_EXPECT(rig.read_back(to, heights) && heights == read.depth_buffer);
	TEST_EXPECT(!std::filesystem::exists(to + ".tmp"));
	TEST_EXPECT(rig.session.view().activity.status.find("Wrote the heights of Tmap.cpt") != std::string::npos);
	TEST_EXPECT(rig.session.view().documents.open.empty()); // nothing opened
	// A relative file from the project's folder, out of it.
	TEST_EXPECT(editor_test::handle_to_end(rig.session, request::export_heights(rig.cpt, "../relative.raw")).done());
	TEST_EXPECT(rig.read_back(rig.dir.file("relative.raw"), heights) && heights == read.depth_buffer);
	std::printf("closed: the atlas as the game's 16-bit .raw, nothing opened\n");
	return 0;
}

int test_unsaved() {
	Rig rig;
	const std::string to = rig.dir.file("sculpted.raw");
	TEST_EXPECT(editor_test::handle_to_end(rig.session, request::open_document(rig.cpt)).done());
	const TerrainHeightsDocument *heights = rig.open_cpt();
	TEST_EXPECT(heights != nullptr);
	if (!heights) return 1;
	const HeightsRect stretch{256, 256, 64, 64};
	rig.session.handle(request::edit_record(rig.cpt, RasterDocument::edit(std::make_shared<RaiseInput>(stretch, 512))));
	const uint64_t revision = heights->revision();
	TEST_EXPECT(heights->dirty());
	const std::vector<uint16_t> live = heights->written_heights(HeightsRect{0, 0, kHeightsSide, kHeightsSide});
	// The active document, named by no path.
	TEST_EXPECT(editor_test::handle_to_end(rig.session, request::export_heights(std::string(), to)).done());
	std::vector<uint16_t> written;
	TEST_EXPECT(rig.read_back(to, written) && written == live);
	CptFile read;
	std::string why;
	TEST_EXPECT(load_cpt(rig.file.data(), rig.file.size(), read, why) && written != read.depth_buffer);
	TEST_EXPECT(test_io::read_file(rig.root + "/" + rig.cpt) == rig.file); // its file as it was
	TEST_EXPECT(heights->dirty() && heights->revision() == revision);      // its document too
	std::printf("unsaved: the document's heights in it, its file and its state as they were\n");
	return 0;
}

int test_import_round() {
	Rig rig;
	const std::string out = rig.dir.file("Tmap.raw");
	TEST_EXPECT(editor_test::handle_to_end(rig.session, request::export_heights(rig.cpt, out)).done());
	// What was dumped makes a terrain, and that terrain's .cpt, an import's output, is dumped in turn.
	const std::vector<uint8_t> rgba(size_t(kHeightsSide) * kHeightsSide * 4, 0x80);
	const std::string colour = rig.dir.file("colour.png");
	TEST_EXPECT(test_io::write_file(colour, png::encode_png_rgba(rgba.data(), kHeightsSide, kHeightsSide)));
	const ActionOutcome made =
	        editor_test::handle_to_end(rig.session, request::new_terrain("isle", {{"heightmap", out}, {"colormap", colour}}));
	for (const Diagnostic &d : made.findings) std::fprintf(stderr, "  %s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(made.done());
	const AssetEntry *isle = rig.session.view().project.scan->find("isle.cpt");
	TEST_EXPECT(isle && !isle->imported_from.empty());
	if (!isle) return 1;
	const std::string again = rig.dir.file("isle.raw");
	TEST_EXPECT(editor_test::handle_to_end(rig.session, request::export_heights(isle->relative_path, again)).done());
	std::vector<uint16_t> heights;
	TEST_EXPECT(rig.read_back(again, heights));
	std::printf("import: a dumped .raw makes a terrain, whose .cpt dumps too\n");
	return 0;
}

int test_refusals() {
	Rig rig;
	const auto refused = [&](const EditorRequest &request, const std::string &to, const char *words) {
		const ActionOutcome outcome = editor_test::handle_to_end(rig.session, request);
		bool said = false;
		for (const Diagnostic &d : outcome.findings)
			said = said || (d.message.find(words) != std::string::npos && d.code() == "terrain_heights.export");
		if (!said) std::fprintf(stderr, "no '%s' in the refusal: %s\n", words, rig.session.view().activity.status.c_str());
		return !outcome.done() && said && (to.empty() || !std::filesystem::exists(to));
	};
	const std::string out = rig.dir.file("out.raw");
	EditorRequest bare = request::export_heights(rig.cpt, out);
	bare.values.clear();
	TEST_EXPECT(refused(bare, std::string(), "Name the .raw"));
	EditorRequest extra = request::export_heights(rig.cpt, out);
	extra.values.emplace_back("bits", "8");
	TEST_EXPECT(refused(extra, out, "no value \"bits\""));
	TEST_EXPECT(refused(request::export_heights(rig.cpt, rig.dir.file("out.bin")), rig.dir.file("out.bin"), "Name the .raw"));
	TEST_EXPECT(refused(request::export_heights(rig.cpt, rig.root + "/terrain/Tmap.raw"), rig.root + "/terrain/Tmap.raw", "inside the project"));
	TEST_EXPECT(refused(request::export_heights(rig.cpt, "heights.raw"), rig.root + "/heights.raw", "inside the project"));
	TEST_EXPECT(refused(request::export_heights(std::string(), out), out, "Name the .cpt"));
	TEST_EXPECT(refused(request::export_heights(rig.trn, out), out, "no .cpt of the project"));
	TEST_EXPECT(refused(request::export_heights("terrain/none.cpt", out), out, "no .cpt of the project"));
	TEST_EXPECT(refused(request::export_heights("terrain/broken.cpt", out), out, "broken.cpt"));
	TEST_EXPECT(refused(request::export_heights(rig.cpt, rig.dir.file("no/such/folder/out.raw")), rig.dir.file("no/such/folder/out.raw"),
	                    "were not written"));
	// A name two folders hold: the file the game does not read gets no heights of the one it does.
	TEST_EXPECT(test_io::write_file(rig.root + "/other/Tmap.cpt", rig.file));
	editor_test::handle_to_end(rig.session, request::rescan());
	const std::string kept = rig.session.view().project.scan->find("Tmap.cpt")->relative_path;
	const std::string second = kept == rig.cpt ? "other/Tmap.cpt" : rig.cpt;
	TEST_EXPECT(refused(request::export_heights(second, out), out, "not the file its name reads"));
	std::printf("refusals: each in words, nothing written\n");
	return 0;
}

int test_no_project() {
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	TEST_EXPECT(!editor_test::handle_to_end(session, request::export_heights("terrain/Tmap.cpt", "C:/out.raw")).done());
	std::printf("no project: refused\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_closed_file();
	failures += test_unsaved();
	failures += test_import_round();
	failures += test_refusals();
	failures += test_no_project();
	if (failures == 0) std::printf("heights_export: all passed\n");
	return failures == 0 ? 0 : 1;
}
