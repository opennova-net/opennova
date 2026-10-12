#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/raster_document.h>
#include <editor/model/value.h>
#include <formats/cpt/cpt.h>
#include <formats/cpt/trngen/terrain_bake_state.h>

namespace opennova::editor {

// A terrain's heights (ADR 0046 S25, the heights lane): the file its brush sculpts, a .cpt in place (the maintainer's
// D1) or an imported terrain's heightmap image, held as two raw16 planes of 1024 x 1024 texels, a height a texel
// (1/256 of a world unit a step, one texel a world unit across):
//
// - the input (kHeightsInput): TrnGen's depth input, what a brush changes. A .cpt keeps none (its CDEP atlas is the
//   leaves drawn back over its tool's lost input [orig: TrnGen.exe sub_402A20 @ 0x402A20, its draw-back]), so a
//   .cpt opens with its atlas as its input (docs/terrain/terrain-re.md, the region bake, D-TERRAIN-22);
// - the baked atlas (kHeightsAtlas): the .cpt's CDEP atlas as the last bake drew it.
//
// The side state (HeightsState) holds the rest of the .cpt (its header, its depth section's form, its 341 tiles, each
// shared) and the input regions whose bake is due (unbaked): the live atlas, what the game would draw now, is the
// input there and the baked atlas elsewhere. No NovaLogic tool edited heights: JO's MED reads the .cpt [orig:
// JOTACmed sub_44C2B0 @ 0x44C2B0] and Nile and gobj write none; the brushes are UI over TrnGen's input, baked by the
// TrnGen port (the session's terrain sculpt).
inline constexpr size_t kHeightsInput = 0;
inline constexpr size_t kHeightsAtlas = 1;
inline constexpr int kHeightsSide = 1024;

// The file's form: a .cpt, or an imported terrain's heightmap image as the import reads it (formats/cpt/trngen
// heightmap_depth: an 8-bit or 16-bit grey PNG, TrnGen's own 1 MiB .raw, the game's 2 MiB raw16).
enum class HeightsForm : uint8_t { Cpt, Png8, Png16, Raw8, Raw16 };
const char *heights_form_words(HeightsForm form);

// A rectangle of texels: x in [x, x + w), y in [y, y + h).
struct HeightsRect {
	int x = 0, y = 0, w = 0, h = 0;
	bool operator==(const HeightsRect &other) const {
		return x == other.x && y == other.y && w == other.w && h == other.h;
	}
	bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// The parts of the side state changes_since names (RasterChanges::parts).
inline constexpr uint32_t kHeightsTilesPart = 1;   // a tile of the .cpt replaced
inline constexpr uint32_t kHeightsUnbakedPart = 2; // the regions whose bake is due
inline constexpr uint32_t kHeightsFormPart = 4;    // the form, the header, the depth section's form
inline constexpr uint32_t kHeightsBakePart = 8;    // the bake state, the input it was made from

struct HeightsState : RasterSideState {
	HeightsForm form = HeightsForm::Cpt;
	// An image's white, world units (the import's `top`); 0 for a .cpt.
	double top = 0.0;
	// Whether the atlas plane and the tiles hold a bake of the input (a .cpt's as read; an image's after its first
	// bake in the editor: until then the import's own .cpt stands).
	bool baked = false;
	CptFile::Header header;
	DepthFormat depth_format = DepthFormat::CDEP;
	std::vector<std::shared_ptr<const CptTile>> tiles;
	std::vector<HeightsRect> unbaked;
	// What a region bake starts from (formats/cpt/trngen terrain_region_bake.h): the bake state the tiles and the
	// baked atlas were made with, null before the editor's first bake of the file (a .cpt's then comes from a cold
	// in-memory bake of its atlas: D-TERRAIN-22), and the input they were made from (a .cpt's atlas as read). Shared:
	// a step keeps the nodes its landing remade.
	std::shared_ptr<const trngen::TerrainBakeState> bake;
	RasterPlane baked_input;

	size_t footprint() const override;
	uint32_t parts_changed(const RasterSideState &before) const override;
};

// What the file page and the findings say of the heights (heights_facts), over the live atlas and the input.
struct HeightsFacts {
	uint16_t lowest = 0, highest = 0;  // the live atlas's, raw16
	size_t over_rasterizer = 0;        // input heights past TrnGen's 128-unit draw-back
	int steep_blocks = 0;              // CDEP blocks of the live atlas past their 32,766-raw range
	size_t unbaked = 0;                // the regions whose bake is due
	size_t tiles = 0;
};

class TerrainHeightsDocument final : public RasterDocument {
public:
	explicit TerrainHeightsDocument(HistoryBudget budget = {});

	// The side state (an empty one before a read).
	const HeightsState &heights() const;
	const RasterPlane &input() const { return plane(kHeightsInput); }
	const RasterPlane &atlas() const { return plane(kHeightsAtlas); }
	bool unbaked_at(int x, int y) const;
	// The height the game would draw at texel (x, y) now: the input where a bake is due, else the baked atlas.
	uint16_t live_height(int x, int y) const;
	// The live atlas whole, row-major, the top row first.
	std::vector<uint16_t> live_atlas() const;
	// The .cpt the document is (or, an image's, its import makes): its header, tiles and depth section's form, the
	// live atlas its depth. False before an image's first bake.
	bool compose_cpt(CptFile &out) const;
	HeightsFacts facts() const;
	// The quadrant lock flags the bake takes (the .trn's lock_* pairs, top-left, top-right, bottom-left,
	// bottom-right: a locked axis keeps a leaf's height taps inside its 512 quadrant), handed by the session from
	// the first .trn naming the file; none by default. Not an edit: no step, no revision.
	const std::array<trngen::CornerLockFlags, 4> &bake_locks() const { return locks_; }
	void set_bake_locks(const std::array<trngen::CornerLockFlags, 4> &locks) { locks_ = locks; }

	std::unique_ptr<DocumentBase> snapshot() const override;

private:
	TerrainHeightsDocument(const TerrainHeightsDocument &other) = default;
	std::array<trngen::CornerLockFlags, 4> locks_{};
};

// A bake's landing (the session's terrain sculpt, ADR 0046 S25): the remade leaves' footprints and the atlas over
// them, the remade tiles, the bake state after and the input it baked, and the regions it settles. It joins the
// step of the stroke it bakes (amends; refused, document.stale, once that step is undone or edited after).
struct HeightsBakeLanding : RasterEditPayload {
	std::vector<HeightsRect> footprints;
	std::vector<std::vector<uint16_t>> atlas; // each footprint's texels, its rows top first
	std::vector<std::pair<int, CptTile>> tiles;
	std::shared_ptr<const trngen::TerrainBakeState> bake;
	RasterPlane input;
	std::vector<HeightsRect> settled;
	const char *token() const override { return "heights.bake"; }
	bool amends() const override { return true; }
	bool apply(RasterCanvas &canvas, Diagnostic &error) const override;
};

// The heights' findings (ADR 0046 S25). None gates (the game loads both [orig: Terrain_LoadLodStorage @
// 0x603635..0x6037A8]); each says what the game does.
enum class TerrainHeightsFinding {
	Unread,         // terrain_heights.unread: a file the editor's .cpt reader refuses (read only; never a gate)
	OverRasterizer, // terrain_heights.over_rasterizer: input past 128 units, which TrnGen's draw-back leaves undrawn
	SteepBlock,     // terrain_heights.steep_block: a CDEP block past its range, which the writer clamps
	Multiplayer,    // terrain_heights.multiplayer: a changed .cpt; the multiplayer check covers the whole file
	kCount,
};
const FindingCodeRow &finding_code(TerrainHeightsFinding code);
FindingTable terrain_heights_finding_codes();
std::vector<Diagnostic> validate_terrain_heights_file(const DocumentBase &document);
const std::vector<FieldSchema> &terrain_heights_fields(NodeKind kind);
io::JsonValue terrain_heights_content_json(const DocumentBase &document);

} // namespace opennova::editor
