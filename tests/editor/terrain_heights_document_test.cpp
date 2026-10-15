// The terrain heights document (ADR 0046 S25, lane A2): a .cpt read as its two raw16 planes (the input sharing the
// atlas's tiles: a .cpt keeps no other input) and its side state (the header, the depth section's form, the tiles),
// written back unedited as its own bytes; the live atlas the input over the regions whose bake is due and the
// baked atlas elsewhere, which a save writes; the findings (heights past TrnGen's 128-unit draw-back, a depth
// stretch past what CDEP holds, a changed .cpt's multiplayer note); its content on the wire; a snapshot of its type;
// and (retail) every JO:CA .cpt opened and written again byte for byte with no finding.
#include <editor/documents/document_types.h>
#include <editor/documents/terrain_heights_document.h>
#include <formats/cpt/cpt_io.h>
#include <formats/cpt/trngen/heightmap_depth.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/png/png_encode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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
#include "editor/editor_test_support.h"

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

using editor_test::has_code;

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

// An imported terrain's heightmap (S25 A2b): a PNG or a .raw read under its import's white into the raw16 heights the
// bake takes; written back as its own bytes while its heights are as read; once changed, a 16-bit grey PNG (or the
// game's 16-bit .raw) whose decode under the same white gives the heights exactly, an 8-bit map saying it becomes
// 16-bit; the brush's cap its white.
int test_image() {
	constexpr int kSide = 1024;
	// A 16-bit grey cone, white 100 units.
	std::vector<uint16_t> samples(size_t(kSide) * kSide);
	for (int y = 0; y < kSide; ++y)
		for (int x = 0; x < kSide; ++x) {
			const double r = std::hypot(x - 512.0, y - 512.0) / 400.0;
			samples[size_t(y) * kSide + size_t(x)] = uint16_t(30000.0 * std::max(0.0, 1.0 - r));
		}
	const std::vector<uint8_t> png16 = png::encode_png_gray16(samples.data(), kSide, kSide);
	TerrainHeightsDocument doc;
	doc.set_import(100.0, "terrain/isle.cpt");
	Diagnostic error;
	TEST_EXPECT(doc.load_bytes(png16, "art/terrain/isle_heightmap.png", AssetKind::HeightmapInput, "jo", error));
	TEST_EXPECT(doc.image() && doc.heights().form == HeightsForm::Png16 && !doc.heights().baked && doc.plane_count() == 2);
	trngen::HeightmapDepth depth;
	std::string why;
	TEST_EXPECT(trngen::decode_heightmap_depth("isle_heightmap.png", png16, 100.0, depth, why) &&
	            doc.live_atlas() == depth.depth16);
	TEST_EXPECT(doc.height_cap() == 25600 && doc.cpt_path() == "terrain/isle.cpt");
	TEST_EXPECT(doc.serialize().text == text_of(png16) && doc.save_words().empty());
	TEST_EXPECT(doc.stand_in_paths().empty()); // not baked by the editor: its import's .cpt stands
	TEST_EXPECT(document_type_for(AssetKind::HeightmapInput) == document_type(DocumentTypeId::TerrainHeights));
	const HeightsRect rect{500, 500, 20, 20};
	TEST_EXPECT(doc.apply(RasterDocument::edit(std::make_shared<RaiseInput>(rect, 256)), error));
	const std::string written = doc.serialize().text;
	const std::vector<uint8_t> written_bytes(written.begin(), written.end());
	TEST_EXPECT(!written.empty() && written_bytes != png16);
	TEST_EXPECT(trngen::decode_heightmap_depth("isle_heightmap.png", written_bytes, 100.0, depth, why) &&
	            depth.depth16 == doc.live_atlas());
	doc.undo();
	TEST_EXPECT(doc.serialize().text == text_of(png16));

	// An 8-bit map at TrnGen's scale: its heights the smoothing TrnGen bakes; once changed, 16-bit grey.
	std::vector<uint8_t> rgba(size_t(kSide) * kSide * 4, 255);
	std::vector<uint8_t> levels(size_t(kSide) * kSide);
	for (size_t i = 0; i < levels.size(); ++i) {
		levels[i] = uint8_t((i % kSide) / 8);
		rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = levels[i];
	}
	const std::vector<uint8_t> png8 = png::encode_png_rgba(rgba.data(), kSide, kSide);
	TerrainHeightsDocument grey;
	grey.set_import(trngen::kDepth8Top, "terrain/isle.cpt");
	TEST_EXPECT(grey.load_bytes(png8, "art/terrain/isle_heightmap.png", AssetKind::HeightmapInput, "jo", error));
	TEST_EXPECT(grey.heights().form == HeightsForm::Png8 && grey.live_atlas() == trngen::smooth_depthmap(levels));
	TEST_EXPECT(grey.serialize().text == text_of(png8));
	TEST_EXPECT(grey.apply(RasterDocument::edit(std::make_shared<RaiseInput>(HeightsRect{8, 8, 4, 4}, 64)), error));
	const std::string grey_written = grey.serialize().text;
	const std::vector<uint8_t> grey_bytes(grey_written.begin(), grey_written.end());
	TEST_EXPECT(trngen::decode_heightmap_depth("isle_heightmap.png", grey_bytes, trngen::kDepth8Top, depth, why) &&
	            depth.depth16 == grey.live_atlas());
	TEST_EXPECT(grey.save_words().find("16-bit grey (it was 8-bit)") != std::string::npos);

	// TrnGen's 8-bit .raw: once changed, the game's 16-bit .raw.
	TerrainHeightsDocument raw;
	raw.set_import(trngen::kDepth8Top, "terrain/isle.cpt");
	TEST_EXPECT(raw.load_bytes(levels, "art/terrain/isle.raw", AssetKind::HeightmapInput, "jo", error));
	TEST_EXPECT(raw.heights().form == HeightsForm::Raw8 && raw.height_cap() == 65535 && raw.serialize().text == text_of(levels));
	TEST_EXPECT(raw.apply(RasterDocument::edit(std::make_shared<RaiseInput>(HeightsRect{8, 8, 4, 4}, 64)), error));
	const std::string raw_written = raw.serialize().text;
	TEST_EXPECT(raw_written.size() == size_t(kSide) * kSide * 2);
	const std::vector<uint16_t> live = raw.live_atlas();
	TEST_EXPECT(uint16_t(uint8_t(raw_written[(9 * kSide + 9) * 2]) | (uint8_t(raw_written[(9 * kSide + 9) * 2 + 1]) << 8)) ==
	            live[9 * kSide + 9]);
	TEST_EXPECT(raw.save_words().find("16-bit .raw (it was 8-bit)") != std::string::npos);
	std::printf("image: a heightmap read under its white, written back as read, as 16-bit once changed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	const int failures = test_cpt() + test_image() + test_retail();
	if (failures == 0) std::printf("editor_terrain_heights_document: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
