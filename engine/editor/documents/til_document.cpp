// The tile placement document (til_document.h, ADR 0046 S25): a .til's records as rows over formats/til, in the
// file's order (the game's draw order), every byte of a record a field or carried (docs/tiles/til-re.md).
#include "til_document.h"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <set>

#include <editor/assets/asset_kinds.h>
#include <editor/model/staged_rows.h>
#include <formats/til/til_io.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kTile = node_kind(TilKind::Tile);

const TilOverlayEntry &entry_of(const RecordHandle &r) { return r.as<TilOverlayEntry>(); }

FieldSchema schema_of(const char *id, FieldType type, const char *label, const char *description) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.label = label;
	schema.description = description;
	return schema;
}

// A position in mission units as the record holds it, 16.16: the nearest 1/65536, within a 32-bit word.
bool fixed_of(const Value &value, int32_t &out, std::string &error) {
	double number = 0.0;
	if (const auto *real = std::get_if<double>(&value)) number = *real;
	else if (const auto *whole = std::get_if<int64_t>(&value)) number = double(*whole);
	else return error = "A tile's corner is a number of mission units.", false;
	const double scaled = std::nearbyint(number * 65536.0);
	if (!std::isfinite(scaled) || scaled < double(std::numeric_limits<int32_t>::min()) ||
	    scaled > double(std::numeric_limits<int32_t>::max()))
		return error = "A tile's corner is a 16.16 word of mission units: this one is past what the word holds.", false;
	out = int32_t(scaled);
	return true;
}

constexpr ChoiceRow kOrientation[] = {
	{"flip_u", TIL_FLAG_FLIP_X, "Flip U (mirrored across)"},
	{"flip_v", TIL_FLAG_FLIP_Y, "Flip V (mirrored down)"},
	{"rotate", TIL_FLAG_ROTATE_90, "Rotate (a quarter turn counter-clockwise)"},
};

RecordTable make_table() {
	using RF = LabelledField;
	TableKind tile(RecordKindRow{kTile, "tile", "Tile", "Add tile", true});
	FieldSchema index = schema_of("tile", FieldType::Byte, "Tile",
			"The tile set's cell the tile draws: the index's column (the index modulo the set's columns, its width over "
			"64) and row (the index over them) [orig: PolyTrn_RenderTile @ 0x60DEBF..0x60DF05]. An index past the set's "
			"cells draws the set's last texel row across its column (D-TIL-7). The set's .TSD table gives the surface "
			"under the tile by this index [orig: Terrain_GetSurfaceTypeAtPosition @ 0x60660C].");
	index.ranged = true;
	index.min = 0;
	index.max = 255;
	tile.field(RF{index,
	              {[](const RecordHandle &r, Value &out) { return out = int64_t(entry_of(r).tile_index), true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               const auto *whole = std::get_if<int64_t>(&v);
		               if (!whole || *whole < 0 || *whole > 255) return e = "A tile is a cell of the set, 0 to 255.", false;
		               r.as<TilOverlayEntry>().tile_index = uint8_t(*whole);
		               return true;
	               }}});
	FieldSchema x = schema_of("x", FieldType::Real, "X",
			"The square's west edge in mission units, exact to 1/65536 (the record's +0, 16.16): the tile spans 16 units "
			"east of it [orig: PolyTrn_RenderTile @ 0x60DE20..0x60DE60].");
	x.group = "position";
	tile.field(RF{x,
	              {[](const RecordHandle &r, Value &out) { return out = double(entry_of(r).x_fixed) / 65536.0, true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               int32_t fixed = 0;
		               if (!fixed_of(v, fixed, e)) return false;
		               r.as<TilOverlayEntry>().x_fixed = fixed;
		               return true;
	               }}});
	FieldSchema y = schema_of("y", FieldType::Real, "Y",
			"The square's south edge in mission units, exact to 1/65536 (the record's +4, stored negated): the tile spans "
			"16 units north of it [orig: PolyTrn_RenderTile @ 0x60DE20..0x60DE60; Terrain_GetSurfaceTypeAtPosition @ "
			"0x6065CC..0x606601].");
	y.group = "position";
	tile.field(RF{y,
	              {[](const RecordHandle &r, Value &out) { return out = -double(entry_of(r).z_fixed) / 65536.0, true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               int32_t fixed = 0;
		               if (!fixed_of(v, fixed, e)) return false;
		               if (fixed == std::numeric_limits<int32_t>::min())
			               return e = "A tile's south edge is stored negated: this one is past what the word holds negated.", false;
		               r.as<TilOverlayEntry>().z_fixed = -fixed;
		               return true;
	               }}});
	FieldSchema orientation = schema_of("orientation", FieldType::Byte, "Orientation",
			"The record's flags byte: the art turned a quarter counter-clockwise first, then mirrored across and down "
			"[orig: PolyTrn_DrawTileOverlayQuad @ 0x604782 (across), 0x6047A9 (down), 0x6047D4 (the turn); D-TIL-4]. The "
			"game reads these three bits alone; the others are kept as the file holds them (0x08 is the original editors' "
			"selection mark).");
	orientation.flags = true;
	orientation.choices = choices_of(kOrientation);
	tile.field(RF{orientation,
	              {[](const RecordHandle &r, Value &out) { return out = int64_t(entry_of(r).flags), true; },
	               [](const RecordHandle &r, const Value &v, std::string &e) {
		               const auto *whole = std::get_if<int64_t>(&v);
		               if (!whole || *whole < 0 || *whole > 255) return e = "An orientation is the flags byte, 0 to 255.", false;
		               r.as<TilOverlayEntry>().flags = uint8_t(*whole);
		               return true;
	               }}});
	return RecordTable({std::move(tile)});
}

const TilRow &tile_row(const Node &node) { return static_cast<const TilRow &>(node); }

std::string units(int32_t fixed) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.6g", double(fixed) / 65536.0);
	return text;
}

constexpr FindingCodeEntry<TilFinding> kFindingEntries[] = {
	// The game refuses the file and places the terrain's own tiles [orig: Terrain_LoadTileInfoFile @ 0x60A78B,
	// @ 0x60A7BA; PolyTrn_LoadTerrainConfig @ 0x60E6E5]: no failure, listed; the editor holds no tile of it (the
	// document is read only).
	{ TilFinding::Unread, listed_code("til.unread") },
	// A count past the file's bytes: the game takes the count as it stands and its readers walk past the loaded
	// file [orig: @ 0x60A7EF]; the editor holds the whole records there are, read only.
	{ TilFinding::InvalidInput, unwritable_code("til.invalid_input") },
	{ TilFinding::IgnoredInput, listed_code("til.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput) },
	// S25 B5: what JO's mission editor makes of the file, never the game's: its save groups the tiles by layer
	// [orig: JOTACmed MapTileStore_Serialize @ 0x433EC0], and it holds 6144 tiles at most [orig: JOTACmed
	// MapTileStore_Append @ 0x433B59; MapTileStore_RemoveAt @ 0x433BE0].
	{ TilFinding::LayerOrder, listed_code("til.layer_order") },
	{ TilFinding::MedCap, listed_code("til.med_cap") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(TilFinding::kCount), "every TilFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries), "the tile placement's rows follow TilFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::TilePlacements);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const RecordTable &til_table() {
	static const RecordTable table = make_table();
	return table;
}

bool is_til_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::TilePlacement; }

TilFile TilDocument::file() const {
	TilFile out;
	out.entries.reserve(rows().size());
	for (const auto &node : rows())
		if (node && node->kind == kTile) out.entries.push_back(tile_row(*node).entry);
	return out;
}

std::string TilDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	const TilOverlayEntry &e = tile_row(*node).entry;
	return "Tile " + std::to_string(e.tile_index) + " at (" + units(e.x_fixed) + ", " + units(-e.z_fixed) + ")";
}

bool TilDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                        std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	if (!is_til_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a tile placement.", path());
		return false;
	}
	unread_.clear();
	if (!til_load_accepts(bytes.data(), bytes.size())) {
		// The game's load refuses it as it refuses a missing file: the terrain's own tile placement stands.
		unread_ = bytes.size() < 16 ? "it is under 16 bytes" : "it does not open with the til0 mark";
		SourceIssue issue{true, 0, std::string(), std::string(),
		                  "The game reads no tile from this file: " + unread_ + ", so it places the terrain's own tiles "
		                  "instead [orig: Terrain_LoadTileInfoFile @ 0x60A78B, @ 0x60A7BA; PolyTrn_LoadTerrainConfig @ "
		                  "0x60E6E5]. The editor holds no tile of it: write a tile placement in its place."};
		issues.push_back(std::move(issue));
		return true;
	}
	TilFile read;
	TilHeaderFacts facts;
	std::string why;
	if (!load_til(bytes.data(), bytes.size(), read, why, &facts)) {
		// A count past the file's bytes: the whole records there are, read only.
		const size_t present = (bytes.size() - 16) / 12;
		std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + std::ptrdiff_t(16 + present * 12));
		cut[4] = uint8_t(present), cut[5] = uint8_t(present >> 8), cut[6] = uint8_t(present >> 16), cut[7] = uint8_t(present >> 24);
		const uint32_t count = uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8) | (uint32_t(bytes[6]) << 16) | (uint32_t(bytes[7]) << 24);
		if (!load_til(cut.data(), cut.size(), read, why)) {
			error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error,
			                     "The tile placement could not be read: " + why + ".", path());
			return false;
		}
		SourceIssue issue{true, 0, std::string(), std::string(),
		                  "The header counts " + std::to_string(count) + " tiles and the file holds " +
		                          std::to_string(present) + ": the game takes the count as it stands and reads past the "
		                          "file's end [orig: Terrain_LoadTileInfoFile @ 0x60A7EF]. The editor holds the " +
		                          std::to_string(present) + " whole tiles, read only: fix the count outside the editor."};
		issues.push_back(std::move(issue));
	} else {
		// What a save writes as MED does and the game never places by [orig: JOTACmed MapTileStore_Serialize @
		// 0x433F44..0x433F50; Terrain_LoadTileInfoFile @ 0x60A7E6..0x60A7EF].
		const uint32_t meds = uint32_t(16 + 12 * read.entries.size());
		if (facts.size_word != meds || facts.word12 != 0)
			issues.push_back({false, 0, std::string(), std::string(),
			                  "The header's words at +8 and +12 hold " + std::to_string(facts.size_word) + " and " +
			                          std::to_string(facts.word12) + ", where the original editor writes " + std::to_string(meds) +
			                          " and 0; the game places nothing by them: a save writes the original editor's."});
		if (facts.trailing)
			issues.push_back({false, 0, std::string(), std::string(),
			                  std::to_string(facts.trailing) +
			                          " bytes follow the last tile, which the game never reads: a save leaves them out."});
	}
	rows.reserve(read.entries.size());
	for (const TilOverlayEntry &entry : read.entries) {
		auto row = std::make_shared<TilRow>();
		row->entry = entry;
		shape(*row);
		rows.push_back(std::move(row));
	}
	return true;
}

SerializeResult TilDocument::serialize() const {
	SerializeResult result;
	std::vector<uint8_t> bytes;
	std::string error;
	if (!save_til(file(), bytes, error)) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The tile placement could not be written: " + error + "."});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

std::string TilDocument::save_words() const {
	return "A save writes the tiles in their order (the game's draw order), each record's every byte as held, under the "
	       "header the original editor writes (the count, 12 x count + 16, 0); an empty placement is the header alone.";
}

std::shared_ptr<Node> TilDocument::make_node(NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &,
                                             std::string &error) {
	if (kind != kTile) {
		error = "A tile placement's rows are its tiles.";
		return nullptr;
	}
	auto row = std::make_shared<TilRow>();
	shape(*row);
	return row;
}

void TilDocument::settle_placed_rows(StagedRows &rows, const std::vector<NodeId> &placed) {
	// Each in its order among the rows, so a run of tiles put in together each takes the one before it; a run at the
	// very start takes the first row after it that the batch did not place (none: each keeps its own).
	const std::set<NodeId> moving(placed.begin(), placed.end());
	for (const NodeId id : placed) {
		const size_t at = rows.index_of(id);
		const auto &all = rows.rows();
		if (at >= all.size()) continue;
		const Node *neighbour = at > 0 ? all[at - 1].get() : nullptr;
		for (size_t next = at + 1; !neighbour && next < all.size(); ++next)
			if (!moving.count(all[next]->id)) neighbour = all[next].get();
		if (!neighbour || neighbour->kind != kTile || all[at]->kind != kTile) continue;
		const uint16_t layer = tile_row(*neighbour).entry.layer;
		if (tile_row(*all[at]).entry.layer == layer) continue;
		auto *row = static_cast<TilRow *>(rows.touch(id));
		if (!row) continue;
		row->entry.layer = layer;
		rows.mark_changed(id);
	}
}

const FindingCodeRow &finding_code(TilFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable til_finding_codes() { return {kFindingRows.data(), kFindingRows.size()}; }

std::vector<Diagnostic> validate_til_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *til = dynamic_cast<const TilDocument *>(&document);
	if (!til) return findings;
	for (const SourceIssue &issue : til->issues()) {
		const TilFinding code = !issue.blocks ? TilFinding::IgnoredInput
		                        : !til->unread().empty() ? TilFinding::Unread
		                                                 : TilFinding::InvalidInput;
		// A file the game reads no tile of is no failure of the game's (the terrain's own tiles stand): a warning.
		const DiagnosticSeverity severity = code == TilFinding::InvalidInput ? DiagnosticSeverity::Error
		                                                                     : DiagnosticSeverity::Warning;
		Diagnostic finding = make_finding(code, severity, issue.message, document.path(), issue.field);
		finding.line = issue.line;
		findings.push_back(std::move(finding));
	}
	if (!til->unread().empty()) return findings;
	// What JO's mission editor makes of the file (S25 B5): notes, the game drawing the file as it stands.
	const TilFile file = til->file();
	size_t out_of_order = 0;
	for (size_t i = 1; i < file.entries.size(); ++i) out_of_order += file.entries[i].layer < file.entries[i - 1].layer ? 1 : 0;
	if (out_of_order)
		findings.push_back(make_finding(TilFinding::LayerOrder, DiagnosticSeverity::Info,
		                                "Its tiles' layers are not in ascending order (" + std::to_string(out_of_order) +
		                                        (out_of_order == 1 ? " tile follows" : " tiles follow") +
		                                        " a higher layer). The game draws the file's order; JO's mission editor writes "
		                                        "its tiles grouped by layer, so a save there draws them in another order.",
		                                document.path()));
	constexpr size_t kMedTiles = 6144;
	if (file.entries.size() > kMedTiles)
		findings.push_back(make_finding(TilFinding::MedCap, DiagnosticSeverity::Info,
		                                "It holds " + std::to_string(file.entries.size()) +
		                                        " tiles. The game draws them all; JO's mission editor holds 6144 at most and "
		                                        "fails on a file past them.",
		                                document.path()));
	return findings;
}

} // namespace opennova::editor
