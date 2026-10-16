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
//   .cpt opens with its atlas as its input (docs/terrain/terrain-re.md, the region bake, D-TERRAIN-23);
// - the baked atlas (kHeightsAtlas): the .cpt's CDEP atlas as the last bake drew it.
//
// The side state (HeightsState) holds the rest of the .cpt (its header, its depth section's form, its 341 tiles, each
// shared) and the input regions whose bake is due (unbaked): the live atlas, what the game would draw now, is the
// input there and the baked atlas elsewhere. No NovaLogic tool edited heights: JO's MED reads the .cpt [orig:
// JOTACmed.exe sub_44C2B0 @ 0x44C2B0] and Nile and gobj.dll write none; the brushes are UI over TrnGen's input,
// baked by the TrnGen port (the session's terrain sculpt).
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
	// in-memory bake of its atlas: D-TERRAIN-23), and the input they were made from (a .cpt's atlas as read). Shared:
	// a step keeps the nodes its landing remade.
	std::shared_ptr<const trngen::TerrainBakeState> bake;
	RasterPlane baked_input;
	// An image's input as read and its file's bytes: an image whose input is as it was read writes those bytes again
	// (an 8-bit heightmap stays 8-bit until a stroke changes it).
	RasterPlane source_input;
	std::shared_ptr<const std::vector<uint8_t>> source;

	size_t footprint() const override;
	// Over the state a step began from: the tiles and the bake nodes it remade (the ones the two do not share), and
	// an image's first bake whole.
	size_t footprint_over(const RasterSideState *before) const override;
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
	// The live atlas over `rect` (inside the atlas), row-major, the top row first.
	std::vector<uint16_t> live_heights(const HeightsRect &rect) const;
	// The live atlas over `rect` as the .cpt written now reads back (a CDEP block past its range clamped, as
	// CptFile::write_bytes writes it; a DPTH section's as it stands): what a fresh read draws and samples, a device's
	// patch and the ground the views pick on.
	std::vector<uint16_t> written_heights(const HeightsRect &rect) const;
	// A whole atlas (1024 x 1024 depth words) as a write of the document's depth section reads it back.
	void clamp_as_written(std::vector<uint16_t> &atlas) const;
	// The .cpt the document is (or, an image's, its import makes): its header, tiles and depth section's form, the
	// live atlas its depth. False before an image's first bake.
	bool compose_cpt(CptFile &out) const;
	// What the file page and the findings read, kept for the document's state (its identity, load and revision).
	HeightsFacts facts() const;
	// An imported terrain's heightmap image (S25, the heights lane's A2b): its white, world units (the import record's
	// `top`, the 127.5 TrnGen's 8-bit scale where it holds none), which its read decodes the image by, and the .cpt its
	// import bakes from it (project-relative), which the document stands in for once the editor has baked it. Handed
	// by the session before the read (DocumentSet: the import whose set names the image as its heightmap). Not an
	// edit: no step, no revision.
	void set_import(double top, const std::string &cpt) {
		top_ = top;
		import_cpt_ = cpt;
	}
	double import_top() const { return top_; }
	const std::string &import_cpt() const { return import_cpt_; }
	// Whether the document is a heightmap image (its form not a .cpt).
	bool image() const { return heights().form != HeightsForm::Cpt; }
	// The .cpt the game reads for these heights: the document's own file, or an image's import's.
	std::string cpt_path() const { return image() ? import_cpt_ : path(); }
	// The highest raw16 height a brush reaches: a PNG heightmap's white (top x 256, its 16-bit samples spanning 0 to
	// it), else the word's 65535.
	int32_t height_cap() const;
	// The quadrant lock flags the bake takes (the .trn's lock_* pairs, top-left, top-right, bottom-left,
	// bottom-right: a locked axis keeps a leaf's height taps inside its 512 quadrant), handed by the session from
	// the first .trn naming the file; none by default. Not an edit: no step, no revision.
	const std::array<trngen::CornerLockFlags, 4> &bake_locks() const { return locks_; }
	void set_bake_locks(const std::array<trngen::CornerLockFlags, 4> &locks) { locks_ = locks; }
	// Why the bake refuses the heights as they stand (the session's terrain sculpt: TrnGen's bake refusing rough
	// ground, D-TERRAIN-22), "" none: a finding (terrain_heights.bake_refused) until the heights change. Not an edit.
	const std::string &bake_refusal() const { return bake_refusal_; }
	void set_bake_refusal(std::string why) {
		if (why == bake_refusal_) return;
		bake_refusal_ = std::move(why);
		++refusal_serial_;
	}
	// What its validation was made from beyond its revision (the validation cache's key): the bake's refusal.
	uint64_t companion_state() const override { return refusal_serial_; }

	std::unique_ptr<DocumentBase> snapshot() const override;
	// An image's document stands in for its import's .cpt once the editor baked it (its live atlas, tiles and header):
	// the views, the ground and the follow pass read the sculpted ground before the import runs again.
	std::vector<std::string> stand_in_paths() const override;
	bool serialize_stand_in(const std::string &path, std::vector<uint8_t> &out) const override;

protected:
	std::shared_ptr<const RasterCodec> pick_codec(const std::string &path, const std::vector<uint8_t> &decoded) const override;

private:
	TerrainHeightsDocument(const TerrainHeightsDocument &other) = default;
	std::array<trngen::CornerLockFlags, 4> locks_{};
	double top_ = 127.5;
	std::string import_cpt_;
	std::string bake_refusal_;
	uint64_t refusal_serial_ = 0;
	struct FactsCache {
		uint64_t identity = 0, load = 0, revision = 0;
		bool made = false;
		HeightsFacts facts;
	};
	mutable FactsCache facts_cache_;
};

// A bake's landing (the session's terrain sculpt, ADR 0046 S25): the remade leaves' footprints and the atlas over
// them, the remade tiles, the bake state after and the input it baked, and the regions it settles. It joins the
// step of the stroke it bakes (amends; refused, document.stale, once that step is undone or edited after).
struct HeightsBakeLanding : RasterEditPayload {
	// An image's first landing: the whole bake its region bake started from (the cold bake of the input as read:
	// its atlas, tiles, header and depth section's form), laid first.
	std::shared_ptr<const CptFile> base;
	// Where the stroke's step cannot take it (undone with a redo kept, or saved): a step of its own carrying `link`
	// (the stroke's), named for the bake (the session's settle, a Save that cannot wait for a redo).
	bool alone = false;
	uint64_t link = 0;
	std::vector<HeightsRect> footprints;
	std::vector<std::vector<uint16_t>> atlas; // each footprint's texels, its rows top first
	std::vector<std::pair<int, CptTile>> tiles;
	std::shared_ptr<const trngen::TerrainBakeState> bake;
	RasterPlane input;
	std::vector<HeightsRect> settled;
	const char *token() const override { return "heights.bake"; }
	bool amends() const override { return true; }
	bool lands_alone() const override { return alone; }
	uint64_t land_link() const override { return link; }
	bool apply(RasterCanvas &canvas, Diagnostic &error) const override;
};

// The heights' findings (ADR 0046 S25). None gates (the game loads both [orig: Terrain_LoadLodStorage @
// 0x603635..0x6037A8]); each says what the game does.
enum class TerrainHeightsFinding {
	Unread,         // terrain_heights.unread: a file the editor's .cpt reader refuses (read only; never a gate)
	OverRasterizer, // terrain_heights.over_rasterizer: input past 128 units, which TrnGen's draw-back leaves undrawn
	SteepBlock,     // terrain_heights.steep_block: a CDEP block past its range, which the writer clamps
	Multiplayer,    // terrain_heights.multiplayer: a changed .cpt; the multiplayer check covers the whole file
	BakeRefused,    // terrain_heights.bake_refused: the bake refuses the heights as they stand (the .cpt is not saved)
	kCount,
};
const FindingCodeRow &finding_code(TerrainHeightsFinding code);
FindingTable terrain_heights_finding_codes();
std::vector<Diagnostic> validate_terrain_heights_file(const DocumentBase &document);
const std::vector<FieldSchema> &terrain_heights_fields(NodeKind kind);
io::JsonValue terrain_heights_content_json(const DocumentBase &document);

} // namespace opennova::editor
