#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/trn/trn.h>

namespace opennova::editor {

// A terrain (the deep-integration plan's DI-30; CONTEXT.md "Terrain document"): a `.trn`, the settings a
// mission's ground is loaded by, read and written through the engine's own reader and writer (load_trn,
// save_trn: the writer from scratch, ADR 0003) over the engine's own record, TrnConfig
// (docs/terrain/terrain-re.md). One row, the terrain, whose fields are the keys the game's terrain reader
// reads [orig: Terrain_ParseConfigCallback @ 0x60F330] in the units the file writes them (a density as
// the repeats over 512 texels, a water height in half metres, the grid in sectors of 512 units) and the
// environment's water keywords a terrain carries, which the environment's reader takes in the terrain's
// pass (trn.h); its sector grid's rows, at most 16, each a row's sixteen sector ids; and its foliage
// definitions, at most four [orig: Terrain_ParseConfigCallback @ 0x60F330, `dword_31BC900 < 4`]. Each map
// and texture names a file through the loader of its role (the asset graph's edges, each a Go to), the
// definitions' models too. What the reader reads otherwise than written is a source finding of its line
// (terrain_source_issues); what the game refuses or ignores in the record is a finding with its fix
// (validate_terrain_file).
//
// A terrain an import makes (S20: a terrain set's images, ImportSource `<stem>.tset`) is the import's: it
// opens, but no edit is taken (DocumentSet's rule for every import output: document.imported); its
// set's images and options are what change it, and Reimport makes it again (session/terrain_uses).

enum class TerrainKind : NodeKind { Terrain = 0, SectorRow = 1, Foliage = 2 };
constexpr NodeKind node_kind(TerrainKind kind) { return static_cast<NodeKind>(kind); }

// The terrain row: the engine's record. Its grid's `polytrn_sectors` lines (TrnConfig::sector_grid's first
// sector_rows rows) are a list of sector rows, each its sixteen cells (the sector a grid cell places: 0
// none, 1 to 4 the heightmap's quadrants), of which the file writes polytrn_sectorcount.
struct TerrainRow : TableRow {
	TrnConfig config;

	TerrainRow();
	std::shared_ptr<Node> clone() const override { return std::make_shared<TerrainRow>(*this); }
	std::string name() const override { return "Terrain"; }
	RecordHandle record() const override;
	size_t footprint() const override;
};

// The terrain's table (terrain_document.cpp): the terrain's, a sector row's and a foliage definition's
// kinds, their fields in the game's words and units, cited, and the two lists.
const RecordTable &terrain_table();

// The terrain table's field of a keyword (the terrain's or a foliage definition's field whose id, else whose file
// token, is the keyword: a lock's and the origin's first number) and, where given, the loader its value names a file
// by (its texture role's, -1 none): what a later file's line of the keyword sets of the terrain (an environment's
// terrain keys, D-TERRAIN-18). Null for a keyword the table holds no field of (foliage, end, a grid row,
// polytrn_scale).
const FieldSchema *terrain_key_field(const std::string &key, int32_t *loader_arg = nullptr);

// The lines of a terrain's text the game's readers read otherwise than the record holds (formats/trn
// trn_source_issues, each a SourceIssue on its line in the editor's words): a key neither reader reads, one
// written again (the last line wins), a grid row before the width or short of it (its cells the slots an earlier
// line left), a row past the width's columns, a fifth foliage block (from it on the file is read by no arm), a
// block's line its arms do not read, a match past its fourth code or past a byte, a murk past 0.99, a last line
// no CR LF ends; and, blocking, an environment keyword the record does not hold (the environment's reader takes
// it in the terrain's pass), polytrn_scale (the multiplayer check's alone) and polytrn_depthmap.
void terrain_source_issues(const std::string &text, std::vector<SourceIssue> &issues);

class TerrainDocument : public TableDocument {
public:
	const RecordTable &table() const override { return terrain_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return terrain_table().fields(kind); }
	// A sector row by its place ("Row 3"), a definition by its model ("Foliage 1: mveg5b.3di").
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<TerrainDocument>(*this); }
	std::string save_words() const override;

	const TerrainRow *terrain_row() const;
	// The terrain as the engine holds it (null before a load).
	const TrnConfig *config() const;
	// The admission gate's words where the game refuses the terrain as it stands ("" it takes it): an empty
	// colour map, detail map or height data name, a grid of rows or a width past 16 or not a power of two
	// [orig: Terrain_LoadEnvironmentConfig @ 0x610940, its tail].
	std::string refused() const;

protected:
	// Each map and texture by its role's loader (texture_roles.h), as the game's PolyTrn_InitTextures opens it.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// A terrain keeps its one row.
	bool accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const override;
};

bool is_terrain_kind(AssetKind kind);

// The admission gate over a record (formats/trn trn_refusal, the rows the writer writes: max(1, sector_rows))
// [orig: Terrain_LoadEnvironmentConfig @ 0x610940, its tail]: "" where the game takes it, else its words.
std::string terrain_refusal(const TrnConfig &config);

// The terrain type's validator (DocumentType::validate_file), an open document standing in for its file:
// its source findings (terrain.invalid_input, terrain.ignored_input); the gate's refusal (terrain.refused,
// an Error the build gates on: the mission's load aborts [orig: Game_StartMission @ 0x524780..0x52479F]),
// with its fix where one follows the game's rule (a grid's rows or its width filled to the next power of
// two as the game's extension would read them); a width of 0 (terrain.no_width: the extension copies the
// config's bytes into the grid); a foliage definition that grows nothing (terrain.foliage_inert: no model,
// or no code but 0), whose fix removes it.
std::vector<Diagnostic> validate_terrain_file(const DocumentBase &document);

// The terrain type's own finding codes (DocumentType::findings), in the order of its table.
enum class TerrainFinding { InvalidInput, IgnoredInput, Refused, NoWidth, FoliageInert, kCount };
const FindingCodeRow &finding_code(TerrainFinding code);
FindingTable terrain_finding_codes();

} // namespace opennova::editor
