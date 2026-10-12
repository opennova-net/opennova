// The terrain heights document (ADR 0046 S25, lane A2): a .cpt read as its two raw16 planes (the input sharing the
// atlas's tiles: a .cpt keeps no other input) and its side state (the header, the depth section's form, the tiles),
// written back unedited as its own bytes; the live atlas the input over the regions whose bake is due and the
// baked atlas elsewhere, which a save writes; the findings (heights past TrnGen's 128-unit draw-back, a depth
// stretch past what CDEP holds, a changed .cpt's multiplayer note); its content on the wire; a snapshot of its type;
// and (retail) every JO:CA .cpt opened and written again byte for byte with no finding.
#include <editor/documents/document_types.h>
#include <editor/documents/terrain_heights_document.h>
#include <formats/cpt/cpt_io.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

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

std::string text_of(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }

bool has_code(const std::vector<Diagnostic> &findings, const char *token) {
	for (const Diagnostic &d : findings)
		if (d.code() == token) return true;
	return false;
}

int test_cpt() {
	const std::vector<uint8_t> bytes = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/terrain/tmap/Tmap.cpt");
	TEST_EXPECT(!bytes.empty());
	TerrainHeightsDocument doc;
	Diagnostic error;
	TEST_EXPECT(doc.load_bytes(bytes, "terrain/Tmap.cpt", AssetKind::TerrainPolyData, "jo", error));
	TEST_EXPECT(doc.as_raster() == &doc && doc.plane_count() == 2 && doc.heights().form == HeightsForm::Cpt &&
	            doc.heights().baked);
	TEST_EXPECT(doc.input().width() == 1024 && doc.input().format() == RasterFormat::U16 &&
	            doc.input().shares_tiles_with(doc.atlas()));
	CptFile read;
	std::string why;
	TEST_EXPECT(load_cpt(bytes.data(), bytes.size(), read, why) && doc.heights().tiles.size() == read.tiles.size());
	TEST_EXPECT(doc.live_atlas() == read.depth_buffer && doc.live_height(300, 400) == read.depth_buffer[400 * 1024 + 300]);
	TEST_EXPECT(doc.serialize().text == text_of(bytes)); // unedited: its own bytes
	TEST_EXPECT(validate_terrain_heights_file(doc).empty());
	const DocumentType *type = document_type(DocumentTypeId::TerrainHeights);
	TEST_EXPECT(type && document_content(*type) == DocumentContent::Raster &&
	            document_type_for(AssetKind::TerrainPolyData) == type);

	// A stroke: the input raised, its region due for its bake; the live atlas reads the input there.
	const HeightsRect rect{100, 200, 30, 20};
	TEST_EXPECT(doc.apply(RasterDocument::edit(std::make_shared<RaiseInput>(rect, 512)), error));
	TEST_EXPECT(doc.dirty() && doc.unbaked_at(110, 210) && !doc.unbaked_at(99, 210));
	TEST_EXPECT(doc.live_height(110, 210) == read.depth_buffer[210 * 1024 + 110] + 512 &&
	            doc.live_height(99, 210) == read.depth_buffer[210 * 1024 + 99] && doc.atlas().u16(110, 210) == read.depth_buffer[210 * 1024 + 110]);
	ChangeSet changes;
	TEST_EXPECT(doc.changes_since(doc.load_generation(), 0, changes));
	const RasterChanges *raster = std::get_if<RasterChanges>(&changes);
	TEST_EXPECT(raster && raster->parts == kHeightsUnbakedPart && !raster->regions.empty() && raster->regions[0].plane == 0);
	// A save writes the live atlas, the tiles as they were.
	CptFile written;
	const std::string text = doc.serialize().text;
	TEST_EXPECT(load_cpt(reinterpret_cast<const uint8_t *>(text.data()), text.size(), written, why));
	TEST_EXPECT(written.depth_buffer == doc.live_atlas() && written.tiles.size() == read.tiles.size());
	TEST_EXPECT(has_code(validate_terrain_heights_file(doc), "terrain_heights.multiplayer"));
	const io::JsonValue content = terrain_heights_content_json(doc);
	TEST_EXPECT(content.get("unbaked") && content.get("unbaked")->number == 1.0 && content.get("baked")->boolean);
	const std::unique_ptr<DocumentBase> snap = doc.snapshot();
	TEST_EXPECT(dynamic_cast<const TerrainHeightsDocument *>(snap.get()) && snap->revision() == doc.revision());
	doc.undo();
	TEST_EXPECT(!doc.dirty() && !doc.unbaked_at(110, 210) && doc.serialize().text == text_of(bytes));

	// Heights past 128 units, and a depth stretch past what CDEP holds.
	TEST_EXPECT(doc.apply(RasterDocument::edit(std::make_shared<RaiseInput>(HeightsRect{0, 0, 4, 1}, 40000)), error));
	const std::vector<Diagnostic> findings = validate_terrain_heights_file(doc);
	TEST_EXPECT(has_code(findings, "terrain_heights.over_rasterizer") && has_code(findings, "terrain_heights.steep_block"));
	TEST_EXPECT(doc.facts().steep_blocks >= 1 && doc.facts().over_rasterizer == 4);
	std::printf("cpt: read as its planes, written back, the live atlas, the findings\n");
	return 0;
}

int test_retail() {
	if (!retail::selected()) return 0;
	const std::string assets = retail::assets();
	if (assets.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (the shipped .cpt files)");
		return 0;
	}
	int swept = 0;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(assets)) {
		std::string ext = entry.path().extension().string();
		for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
		if (!entry.is_regular_file() || ext != ".cpt") continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().string());
		TerrainHeightsDocument document;
		Diagnostic error;
		const std::string name = entry.path().filename().string();
		TEST_EXPECT(document.load_bytes(bytes, name, AssetKind::TerrainPolyData, "jo", error));
		TEST_EXPECT(!document.blocked() && document.issues().empty() && validate_terrain_heights_file(document).empty());
		TEST_EXPECT(document.serialize().text == text_of(bytes));
		++swept;
	}
	TEST_EXPECT(swept >= 23);
	std::printf("retail: %d .cpt opened and saved byte-identical, no finding\n", swept);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	const int failures = test_cpt() + test_retail();
	if (failures == 0) std::printf("editor_terrain_heights_document: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
