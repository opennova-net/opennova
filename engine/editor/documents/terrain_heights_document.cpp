#include "terrain_heights_document.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

#include <base/io/strutil.h>
#include <editor/project/project_files.h>
#include <formats/cpt/cpt_io.h>
#include <formats/cpt/trngen/heightmap_depth.h>
#include <formats/cpt/trngen/terrain_bake.h>
#include <formats/png/png_encode.h>

namespace opennova::editor {

using io::JsonValue;
using io::json_number;
using io::json_string;

namespace {

constexpr size_t kTexels = size_t(kHeightsSide) * kHeightsSide;
// TrnGen's draw-back writes a height only up to 128 world units [orig: TrnGen.exe sub_402A20 @ 0x402A20, its range
// check; formats/cpt/trngen rasterize_terrain_depth].
constexpr uint16_t kRasterizerTop = 128 * 256;

const HeightsState &empty_state() {
	static const HeightsState none;
	return none;
}

// A .cpt read whole: its CDEP (or DPTH) depth as both planes (the input is the atlas: its tool's input is lost), the
// rest of the file the side state; written back from them, the live atlas its depth (an unedited file writes its own
// bytes: cpt_jo_assets_sweep's re-encode leg over every JO:CA .cpt).
class CptHeightsCodec final : public RasterCodec {
public:
	bool read(const std::vector<uint8_t> &decoded, RasterState &out, std::vector<SourceIssue> &issues,
	          Diagnostic &error) const override {
		(void)issues;
		(void)error;
		CptFile cpt;
		std::string why;
		if (!load_cpt(decoded.data(), decoded.size(), cpt, why)) why = "it does not read as a .cpt: " + why;
		else if (cpt.depth_buffer.size() != kTexels)
			why = "it holds " + std::to_string(cpt.depth_buffer.size()) + " heights, not the 1024 x 1024 the game reads";
		if (!why.empty()) {
			// Read only, a listed finding: the editor's reader is not the game's walk (D-TERRAIN-14), so what it
			// refuses is no gate; the build packs the file as it is.
			SourceIssue unread;
			unread.blocks = true;
			unread.message = "The editor reads no heights in this file: " + why + ".";
			issues.push_back(std::move(unread));
			auto none = std::make_shared<HeightsState>();
			out.planes.clear();
			out.side = std::move(none);
			return true;
		}
		std::vector<uint8_t> bytes(kTexels * 2);
		std::memcpy(bytes.data(), cpt.depth_buffer.data(), bytes.size());
		const RasterPlane atlas = RasterPlane::from_bytes(RasterFormat::U16, kHeightsSide, kHeightsSide, bytes.data());
		out.planes = {atlas, atlas}; // the input shares the atlas's tiles until a brush writes
		auto state = std::make_shared<HeightsState>();
		state->form = HeightsForm::Cpt;
		state->baked = true;
		state->header = cpt.header;
		state->depth_format = cpt.depth_format;
		state->tiles.reserve(cpt.tiles.size());
		for (CptTile &tile : cpt.tiles) state->tiles.push_back(std::make_shared<const CptTile>(std::move(tile)));
		state->baked_input = atlas;
		out.side = std::move(state);
		return true;
	}

	SerializeResult write(const RasterState &state) const override {
		SerializeResult out;
		const auto *heights = dynamic_cast<const HeightsState *>(state.side.get());
		if (!heights || state.planes.size() < 2) {
			out.issues.push_back({});
			out.issues.back().message = "The heights hold no .cpt to write.";
			return out;
		}
		CptFile cpt;
		cpt.header = heights->header;
		cpt.depth_format = heights->depth_format;
		// The live atlas: the baked atlas, the input over the regions whose bake is due.
		cpt.depth_buffer.resize(kTexels);
		const RasterPlane &input = state.planes[kHeightsInput], &atlas = state.planes[kHeightsAtlas];
		atlas.copy_rect(0, 0, kHeightsSide, kHeightsSide, reinterpret_cast<uint8_t *>(cpt.depth_buffer.data()));
		std::vector<uint16_t> region;
		for (const HeightsRect &rect : heights->unbaked) {
			region.resize(size_t(rect.w) * size_t(rect.h));
			input.copy_rect(rect.x, rect.y, rect.w, rect.h, reinterpret_cast<uint8_t *>(region.data()));
			for (int row = 0; row < rect.h; ++row)
				std::copy_n(region.begin() + std::ptrdiff_t(row) * rect.w, rect.w,
				            cpt.depth_buffer.begin() + std::ptrdiff_t(rect.y + row) * kHeightsSide + rect.x);
		}
		cpt.tiles.reserve(heights->tiles.size());
		for (const auto &tile : heights->tiles) cpt.tiles.push_back(*tile);
		std::vector<uint8_t> bytes;
		std::string why;
		if (!save_cpt(cpt, bytes, why)) {
			out.issues.push_back({});
			out.issues.back().message = "The .cpt cannot be written: " + why + ".";
			return out;
		}
		out.text.assign(bytes.begin(), bytes.end());
		return out;
	}
};

// An imported terrain's heightmap (S25 A2b): a PNG (grey at any depth, a colour image read as its grey) or a .raw
// (TrnGen's 1 MiB of 8-bit heights, the game's 2 MiB of 16-bit), decoded as the terrain import decodes it under the
// import's white (trngen::decode_heightmap_depth): the input plane holds the raw16 heights the bake takes (an 8-bit
// map smoothed as TrnGen smooths it [orig: TrnGen.exe build_terrain_thread @ 0x4013A0], so a bake of the plane is
// the bake of the map), its atlas plane the same until the editor bakes it (the import's own .cpt stands till
// then). Written back as its bytes while the input is as read; once a stroke changed it, as a 16-bit grey PNG
// whose samples the import decodes back to the input exactly (trngen::encode_heightmap_depth16), or a .raw as the
// game's 16-bit heights: an 8-bit map becomes 16-bit, and the terrain the import makes from it is the one the
// editor shows.
class HeightmapImageCodec final : public RasterCodec {
public:
	HeightmapImageCodec(double top, bool raw) : top_(top), raw_(raw) {}

	bool read(const std::vector<uint8_t> &decoded, RasterState &out, std::vector<SourceIssue> &issues,
	          Diagnostic &error) const override {
		(void)error;
		trngen::HeightmapDepth depth;
		std::string why;
		if (!trngen::decode_heightmap_depth(raw_ ? "the heightmap.raw" : "the heightmap.png", decoded, top_, depth, why)) {
			SourceIssue unread;
			unread.blocks = true;
			unread.message = "The editor reads no heights in this image: " + why + ".";
			issues.push_back(std::move(unread));
			out.planes.clear();
			out.side = std::make_shared<HeightsState>();
			return true;
		}
		const std::vector<uint16_t> raw16 = depth.depth16.empty() ? trngen::smooth_depthmap(depth.depth8) : depth.depth16;
		const RasterPlane input = RasterPlane::from_bytes(RasterFormat::U16, kHeightsSide, kHeightsSide,
		                                                  reinterpret_cast<const uint8_t *>(raw16.data()));
		out.planes = {input, input}; // the atlas plane holds nothing of its own until the editor bakes
		auto state = std::make_shared<HeightsState>();
		if (raw_) state->form = decoded.size() == kTexels ? HeightsForm::Raw8 : HeightsForm::Raw16;
		else state->form = decoded.size() > 24 && decoded[24] == 16 ? HeightsForm::Png16 : HeightsForm::Png8; // IHDR's bit depth
		state->top = top_;
		state->baked = false;
		state->baked_input = input;
		state->source_input = input;
		state->source = std::make_shared<const std::vector<uint8_t>>(decoded);
		out.side = std::move(state);
		return true;
	}

	SerializeResult write(const RasterState &state) const override {
		SerializeResult out;
		const auto *heights = dynamic_cast<const HeightsState *>(state.side.get());
		if (!heights || state.planes.empty()) {
			out.issues.push_back({});
			out.issues.back().message = "The heightmap holds no heights to write.";
			return out;
		}
		const RasterPlane &input = state.planes[kHeightsInput];
		if (heights->source && input.same_texels(heights->source_input)) {
			out.text.assign(heights->source->begin(), heights->source->end());
			return out;
		}
		std::vector<uint16_t> raw16(kTexels);
		input.copy_rect(0, 0, kHeightsSide, kHeightsSide, reinterpret_cast<uint8_t *>(raw16.data()));
		if (raw_) {
			out.text.resize(kTexels * 2);
			for (size_t i = 0; i < kTexels; ++i) {
				out.text[i * 2] = char(raw16[i] & 0xFF);
				out.text[i * 2 + 1] = char(raw16[i] >> 8);
			}
			return out;
		}
		std::vector<uint16_t> samples;
		std::string why;
		if (!trngen::encode_heightmap_depth16(raw16, heights->top, samples, why)) {
			out.issues.push_back({});
			out.issues.back().message = "The heightmap cannot be written: " + why + ".";
			return out;
		}
		const std::vector<uint8_t> png = png::encode_png_gray16(samples.data(), kHeightsSide, kHeightsSide);
		out.text.assign(png.begin(), png.end());
		return out;
	}

	std::string save_words(const RasterState &state) const override {
		const auto *heights = dynamic_cast<const HeightsState *>(state.side.get());
		if (!heights || state.planes.empty() || (heights->source && state.planes[kHeightsInput].same_texels(heights->source_input)))
			return std::string();
		if (heights->form == HeightsForm::Png8)
			return "Writes the heightmap as 16-bit grey (it was 8-bit); the terrain the import makes is unchanged.";
		if (heights->form == HeightsForm::Raw8)
			return "Writes the heightmap as the game's 16-bit .raw (it was 8-bit); the terrain the import makes is unchanged.";
		return std::string();
	}

private:
	double top_ = trngen::kDepth8Top;
	bool raw_ = false;
};

std::shared_ptr<const RasterCodec> pick_heights_codec(const std::string &path, const std::vector<uint8_t> &decoded) {
	static const auto cpt = std::make_shared<CptHeightsCodec>();
	if (strutil::ends_with_icase(path, ".cpt") || (decoded.size() >= 4 && std::memcmp(decoded.data(), "CPT1", 4) == 0))
		return cpt;
	return nullptr;
}

constexpr FindingCodeEntry<TerrainHeightsFinding> kFindingEntries[] = {
	{ TerrainHeightsFinding::Unread, listed_code("terrain_heights.unread") },
	{ TerrainHeightsFinding::OverRasterizer, listed_code("terrain_heights.over_rasterizer") },
	{ TerrainHeightsFinding::SteepBlock, listed_code("terrain_heights.steep_block") },
	{ TerrainHeightsFinding::Multiplayer, listed_code("terrain_heights.multiplayer") },
	{ TerrainHeightsFinding::BakeRefused, listed_code("terrain_heights.bake_refused") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(TerrainHeightsFinding::kCount),
              "every TerrainHeightsFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries), "the heights' rows follow TerrainHeightsFinding's order");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::TerrainHeights);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const char *heights_form_words(HeightsForm form) {
	switch (form) {
	case HeightsForm::Cpt: return "a .cpt";
	case HeightsForm::Png8: return "an 8-bit grey PNG";
	case HeightsForm::Png16: return "a 16-bit grey PNG";
	case HeightsForm::Raw8: return "TrnGen's 8-bit .raw";
	case HeightsForm::Raw16: return "the game's 16-bit .raw";
	}
	return "";
}

size_t HeightsState::footprint() const {
	return sizeof(*this) + tiles.size() * sizeof(tiles.front()) + unbaked.size() * sizeof(HeightsRect);
}

namespace {

size_t tile_bytes(const CptTile &tile) {
	size_t bytes = sizeof(CptTile) + tile.vertex_indices.size() * sizeof(tile.vertex_indices.front());
	for (const CptTileLOD &lod : tile.lods) bytes += lod.indices.size() * sizeof(uint16_t);
	return bytes;
}

size_t node_bytes(const trngen::NodeBakeState &node) {
	return sizeof(node) + node.section.faces.size() * sizeof(uint16_t) + node.section.records.size() * sizeof(float) +
	       node.draw_back.positions.size() * sizeof(float) + node.draw_back.faces.size() * sizeof(uint16_t);
}

} // namespace

size_t HeightsState::footprint_over(const RasterSideState *before_state) const {
	const auto *before = dynamic_cast<const HeightsState *>(before_state);
	size_t bytes = footprint();
	for (size_t i = 0; i < tiles.size(); ++i)
		if (tiles[i] && (!before || i >= before->tiles.size() || before->tiles[i] != tiles[i])) bytes += tile_bytes(*tiles[i]);
	if (bake && (!before || before->bake != bake))
		for (size_t i = 0; i < bake->nodes.size(); ++i) {
			const bool shared = before && before->bake && i < before->bake->nodes.size() && before->bake->nodes[i] == bake->nodes[i];
			if (bake->nodes[i] && !shared) bytes += node_bytes(*bake->nodes[i]);
		}
	return bytes;
}

uint32_t HeightsState::parts_changed(const RasterSideState &before) const {
	const auto *other = dynamic_cast<const HeightsState *>(&before);
	if (!other) return kHeightsTilesPart | kHeightsUnbakedPart | kHeightsFormPart;
	uint32_t parts = 0;
	if (tiles != other->tiles) parts |= kHeightsTilesPart;
	if (unbaked != other->unbaked) parts |= kHeightsUnbakedPart;
	if (form != other->form || top != other->top || baked != other->baked || depth_format != other->depth_format ||
	    std::memcmp(&header, &other->header, sizeof(header)) != 0)
		parts |= kHeightsFormPart;
	if (bake != other->bake || !baked_input.shares_tiles_with(other->baked_input)) parts |= kHeightsBakePart;
	return parts;
}

bool HeightsBakeLanding::apply(RasterCanvas &canvas, Diagnostic &error) const {
	const auto *now = dynamic_cast<const HeightsState *>(canvas.side());
	if (!now || canvas.plane_count() < 2) {
		error = make_finding(CoreFinding::DocumentPayload, DiagnosticSeverity::Error, "A bake lands only in the heights.");
		return false;
	}
	auto state = std::make_shared<HeightsState>(*now);
	RasterWriter &atlas = canvas.write(kHeightsAtlas);
	if (base && !now->baked) {
		if (base->depth_buffer.size() != kTexels) {
			error = make_finding(CoreFinding::DocumentPayload, DiagnosticSeverity::Error, "A bake's base is no 1024 x 1024 atlas.");
			return false;
		}
		atlas.set_rect_u16(0, 0, kHeightsSide, kHeightsSide, base->depth_buffer.data(), size_t(kHeightsSide));
		state->header = base->header;
		state->depth_format = base->depth_format;
		state->tiles.clear();
		for (const CptTile &tile : base->tiles) state->tiles.push_back(std::make_shared<const CptTile>(tile));
	}
	for (size_t i = 0; i < footprints.size() && i < this->atlas.size(); ++i) {
		const HeightsRect &r = footprints[i];
		for (int row = 0; row < r.h; ++row)
			for (int col = 0; col < r.w; ++col)
				atlas.set_u16(r.x + col, r.y + row, this->atlas[i][size_t(row) * size_t(r.w) + size_t(col)]);
	}
	for (const auto &[index, tile] : tiles)
		if (index >= 0 && size_t(index) < state->tiles.size()) state->tiles[size_t(index)] = std::make_shared<const CptTile>(tile);
	state->bake = bake;
	state->baked_input = input;
	state->baked = true;
	std::vector<HeightsRect> due;
	for (const HeightsRect &rect : state->unbaked)
		if (std::find(settled.begin(), settled.end(), rect) == settled.end()) due.push_back(rect);
	state->unbaked = std::move(due);
	canvas.set_side(std::move(state));
	return true;
}

TerrainHeightsDocument::TerrainHeightsDocument(HistoryBudget budget) : RasterDocument(pick_heights_codec, budget) {}

const HeightsState &TerrainHeightsDocument::heights() const {
	const auto *state = dynamic_cast<const HeightsState *>(side());
	return state ? *state : empty_state();
}

bool TerrainHeightsDocument::unbaked_at(int x, int y) const {
	for (const HeightsRect &rect : heights().unbaked)
		if (rect.contains(x, y)) return true;
	return false;
}

uint16_t TerrainHeightsDocument::live_height(int x, int y) const {
	return unbaked_at(x, y) || !heights().baked ? input().u16(x, y) : atlas().u16(x, y);
}

std::vector<uint16_t> TerrainHeightsDocument::live_heights(const HeightsRect &rect) const {
	std::vector<uint16_t> out(size_t(std::max(rect.w, 0)) * size_t(std::max(rect.h, 0)));
	if (plane_count() < 2 || out.empty() || rect.x < 0 || rect.y < 0 || rect.x + rect.w > kHeightsSide ||
			rect.y + rect.h > kHeightsSide)
		return std::vector<uint16_t>();
	const RasterPlane &from = heights().baked ? atlas() : input();
	from.copy_rect(rect.x, rect.y, rect.w, rect.h, reinterpret_cast<uint8_t *>(out.data()));
	if (!heights().baked) return out;
	// The input where a bake is due.
	for (const HeightsRect &due : heights().unbaked) {
		const int x0 = std::max(rect.x, due.x), y0 = std::max(rect.y, due.y);
		const int x1 = std::min(rect.x + rect.w, due.x + due.w), y1 = std::min(rect.y + rect.h, due.y + due.h);
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) out[size_t(y - rect.y) * size_t(rect.w) + size_t(x - rect.x)] = input().u16(x, y);
	}
	return out;
}

std::vector<uint16_t> TerrainHeightsDocument::written_heights(const HeightsRect &rect) const {
	if (heights().depth_format != DepthFormat::CDEP) return live_heights(rect);
	// The rows' whole blocks the rect reaches (a block is a 256-texel stretch of a row), clamped, then the rect's own.
	const int x0 = rect.x / 256 * 256, x1 = (rect.x + rect.w + 255) / 256 * 256;
	std::vector<uint16_t> blocks = live_heights(HeightsRect{x0, rect.y, x1 - x0, rect.h});
	if (blocks.empty()) return blocks;
	cpt_clamp_steep_blocks(blocks.data(), blocks.size());
	if (x0 == rect.x && x1 == rect.x + rect.w) return blocks;
	std::vector<uint16_t> out(size_t(rect.w) * size_t(rect.h));
	for (int row = 0; row < rect.h; ++row)
		std::copy_n(blocks.begin() + std::ptrdiff_t(row) * (x1 - x0) + (rect.x - x0), rect.w,
		            out.begin() + std::ptrdiff_t(row) * rect.w);
	return out;
}

void TerrainHeightsDocument::clamp_as_written(std::vector<uint16_t> &atlas) const {
	if (heights().depth_format == DepthFormat::CDEP) cpt_clamp_steep_blocks(atlas.data(), atlas.size());
}

std::vector<uint16_t> TerrainHeightsDocument::live_atlas() const {
	std::vector<uint16_t> out(kTexels);
	if (plane_count() < 2) return out;
	const RasterPlane &from = heights().baked ? atlas() : input();
	from.copy_rect(0, 0, kHeightsSide, kHeightsSide, reinterpret_cast<uint8_t *>(out.data()));
	if (!heights().baked) return out;
	std::vector<uint16_t> region;
	for (const HeightsRect &rect : heights().unbaked) {
		region.resize(size_t(rect.w) * size_t(rect.h));
		input().copy_rect(rect.x, rect.y, rect.w, rect.h, reinterpret_cast<uint8_t *>(region.data()));
		for (int row = 0; row < rect.h; ++row)
			std::copy_n(region.begin() + std::ptrdiff_t(row) * rect.w, rect.w,
			            out.begin() + std::ptrdiff_t(rect.y + row) * kHeightsSide + rect.x);
	}
	return out;
}

bool TerrainHeightsDocument::compose_cpt(CptFile &out) const {
	const HeightsState &state = heights();
	if (!state.baked || plane_count() < 2) return false;
	out = CptFile();
	out.header = state.header;
	out.depth_format = state.depth_format;
	out.depth_buffer = live_atlas();
	out.tiles.reserve(state.tiles.size());
	for (const auto &tile : state.tiles) out.tiles.push_back(*tile);
	return true;
}

HeightsFacts TerrainHeightsDocument::facts() const {
	if (facts_cache_.made && facts_cache_.identity == identity() && facts_cache_.load == load_generation() &&
	    facts_cache_.revision == revision())
		return facts_cache_.facts;
	HeightsFacts facts;
	if (plane_count() < 2) return facts;
	const std::vector<uint16_t> live = live_atlas();
	const auto range = std::minmax_element(live.begin(), live.end());
	facts.lowest = *range.first;
	facts.highest = *range.second;
	const std::vector<uint8_t> input_bytes = input().bytes();
	for (size_t i = 0; i < kTexels; ++i) {
		uint16_t h = 0;
		std::memcpy(&h, input_bytes.data() + i * 2, 2);
		facts.over_rasterizer += h > kRasterizerTop;
	}
	facts.steep_blocks = cpt_steep_blocks(live);
	facts.unbaked = heights().unbaked.size();
	facts.tiles = heights().tiles.size();
	facts_cache_ = FactsCache{identity(), load_generation(), revision(), true, facts};
	return facts;
}

std::unique_ptr<DocumentBase> TerrainHeightsDocument::snapshot() const {
	return std::unique_ptr<DocumentBase>(new TerrainHeightsDocument(*this));
}

int32_t TerrainHeightsDocument::height_cap() const {
	const HeightsForm form = heights().form;
	if (form != HeightsForm::Png8 && form != HeightsForm::Png16) return 65535;
	return int32_t(std::clamp(std::lround(heights().top * 256.0), 0L, 65535L));
}

std::vector<std::string> TerrainHeightsDocument::stand_in_paths() const {
	if (!image() || !heights().baked || import_cpt_.empty()) return {};
	return {import_cpt_};
}

bool TerrainHeightsDocument::serialize_stand_in(const std::string &path, std::vector<uint8_t> &out) const {
	CptFile cpt;
	std::string why;
	return path == import_cpt_ && compose_cpt(cpt) && save_cpt(cpt, out, why);
}

std::shared_ptr<const RasterCodec> TerrainHeightsDocument::pick_codec(const std::string &path,
		const std::vector<uint8_t> &decoded) const {
	if (std::shared_ptr<const RasterCodec> cpt = pick_heights_codec(path, decoded)) return cpt;
	const bool raw = strutil::ends_with_icase(path, ".raw");
	if (raw || strutil::ends_with_icase(path, ".png")) return std::make_shared<HeightmapImageCodec>(top_, raw);
	return nullptr;
}

const FindingCodeRow &finding_code(TerrainHeightsFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable terrain_heights_finding_codes() { return {kFindingRows.data(), kFindingRows.size()}; }

std::vector<Diagnostic> validate_terrain_heights_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(&document);
	if (!heights) return findings;
	for (const SourceIssue &issue : heights->issues())
		findings.push_back(make_finding(TerrainHeightsFinding::Unread, DiagnosticSeverity::Warning, issue.message,
		                                document.path()));
	if (heights->plane_count() < 2) return findings;
	const HeightsFacts facts = heights->facts();
	const std::string name = basename_of(document.path());
	if (facts.over_rasterizer)
		findings.push_back(make_finding(
				TerrainHeightsFinding::OverRasterizer, DiagnosticSeverity::Warning,
				std::to_string(facts.over_rasterizer) + " heights of " + name +
						" stand past 128 units, where TrnGen's bake draws no ground back: there the game's ground is the "
						"sculpted height itself, not the simplified mesh its terrain draws, so collisions and the drawn "
						"ground part.",
				document.path()));
	if (facts.steep_blocks)
		findings.push_back(make_finding(
				TerrainHeightsFinding::SteepBlock, DiagnosticSeverity::Warning,
				std::to_string(facts.steep_blocks) + " stretches of 256 heights along a row of " + name +
						" rise more than 128 units: the .cpt's depth section holds at most 32,766 raw a stretch, so the "
						"file is written with them clamped.",
				document.path()));
	if (!heights->bake_refusal().empty())
		findings.push_back(make_finding(
				TerrainHeightsFinding::BakeRefused, DiagnosticSeverity::Error,
				"TrnGen's bake refuses the heights of " + name + " as they stand (" + heights->bake_refusal() +
						"): the sculpt shows over the old tiles, and " + name +
						" is not saved until they change (smooth or lower the ground, or undo the stroke).",
				document.path()));
	// The multiplayer check covers the .cpt, which a heightmap image's import makes (not the image).
	if (!heights->image() && (document.dirty() || document.wrote_file()))
		findings.push_back(make_finding(
				TerrainHeightsFinding::Multiplayer, DiagnosticSeverity::Info,
				name + " is changed: the multiplayer check covers the whole .cpt, so players joining a game on this "
				       "terrain need this file as the host has it.",
				document.path()));
	return findings;
}

const std::vector<FieldSchema> &terrain_heights_fields(NodeKind) {
	static const std::vector<FieldSchema> none;
	return none;
}

JsonValue terrain_heights_content_json(const DocumentBase &document) {
	const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(&document);
	if (!heights || heights->plane_count() < 2) return JsonValue::make_null();
	const HeightsFacts facts = heights->facts();
	JsonValue out = JsonValue::make_object();
	out.set("form", json_string(heights_form_words(heights->heights().form)));
	out.set("baked", JsonValue::make_bool(heights->heights().baked));
	out.set("lowest", json_number(facts.lowest / 256.0));
	out.set("highest", json_number(facts.highest / 256.0));
	out.set("over_rasterizer", json_number(double(facts.over_rasterizer)));
	out.set("steep_blocks", json_number(double(facts.steep_blocks)));
	out.set("unbaked", json_number(double(facts.unbaked)));
	out.set("tiles", json_number(double(facts.tiles)));
	return out;
}

} // namespace opennova::editor
