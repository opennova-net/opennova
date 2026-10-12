#include "terrain_heights_document.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

#include <base/io/strutil.h>
#include <editor/project/project_files.h>
#include <formats/cpt/cpt_io.h>

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
		cpt.depth_buffer.resize(kTexels);
		const RasterPlane &input = state.planes[kHeightsInput], &atlas = state.planes[kHeightsAtlas];
		for (int y = 0; y < kHeightsSide; ++y)
			for (int x = 0; x < kHeightsSide; ++x) {
				bool due = false;
				for (const HeightsRect &rect : heights->unbaked) due = due || rect.contains(x, y);
				cpt.depth_buffer[size_t(y) * kHeightsSide + size_t(x)] = due ? input.u16(x, y) : atlas.u16(x, y);
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

uint32_t HeightsState::parts_changed(const RasterSideState &before) const {
	const auto *other = dynamic_cast<const HeightsState *>(&before);
	if (!other) return kHeightsTilesPart | kHeightsUnbakedPart | kHeightsFormPart;
	uint32_t parts = 0;
	if (tiles != other->tiles) parts |= kHeightsTilesPart;
	if (unbaked != other->unbaked) parts |= kHeightsUnbakedPart;
	if (form != other->form || top != other->top || baked != other->baked || depth_format != other->depth_format ||
	    std::memcmp(&header, &other->header, sizeof(header)) != 0)
		parts |= kHeightsFormPart;
	return parts;
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
	return facts;
}

std::unique_ptr<DocumentBase> TerrainHeightsDocument::snapshot() const {
	return std::unique_ptr<DocumentBase>(new TerrainHeightsDocument(*this));
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
	if (document.dirty() || document.wrote_file())
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
