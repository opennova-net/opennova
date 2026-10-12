#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/til/til.h>

namespace opennova::editor {

// A tile placement (`.til`, ADR 0046 S25; docs/tiles/til-re.md): the tiles of a terrain's tile set painted over
// its surface, each a 12-byte record of the file read and written through formats/til (load_til, save_til: the
// header MED's serializer writes, computed from the rows [orig: JOTACmed MapTileStore_Serialize @ 0x433EC0]).
// One row per tile in the file's order, which is the game's draw order: the last tile drawn over the others
// [orig: PolyTrn_RenderTile @ 0x60DDD4..0x60DF1B], the first holding the point deciding the surface under it
// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x6065CC..0x60660C]. A tile's fields are its cell of the tile set,
// its square's south-west corner in mission units (x the record's +0, y its +4 negated, exact to 1/65536; the
// square spans 16 units east and north [orig: PolyTrn_RenderTile @ 0x60DE20..0x60DE60]) and its orientation
// (the flags byte: mirror across, mirror down, a quarter turn; its other bits carried). MED's layer (+10) is
// carried and not shown: the game never reads it, and every edit keeps the rows ascending by it as MED writes
// them (a tile put in takes the layer of the row before it), so MED's next save keeps the draw order.
//
// Read only where the game would read none of it as a placement: under 16 bytes or no til0 mark (the game reads
// the terrain's own tiles instead [orig: Terrain_LoadTileInfoFile @ 0x60A78B, @ 0x60A7BA]), or a count past the
// file's bytes (the game's readers walk past its buffer [orig: @ 0x60A7EF]).

enum class TilKind : NodeKind { Tile = 0 };
constexpr NodeKind node_kind(TilKind kind) { return static_cast<NodeKind>(kind); }

struct TilRow : TableRow {
	TilOverlayEntry entry;

	TilRow() { kind = node_kind(TilKind::Tile); }
	std::shared_ptr<Node> clone() const override { return std::make_shared<TilRow>(*this); }
	std::string name() const override { return "Tile " + std::to_string(entry.tile_index); }
	RecordHandle record() const override { return RecordHandle{kind, const_cast<TilOverlayEntry *>(&entry)}; }
	size_t footprint() const override { return sizeof(TilRow) + ids_footprint(); }
};

const RecordTable &til_table();

class TilDocument : public TableDocument {
public:
	const RecordTable &table() const override { return til_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return til_table().fields(kind); }
	// A tile by its cell and corner ("Tile 12 at (1024, -512)").
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<TilDocument>(*this); }

	// The file as the rows hold it, in their order.
	TilFile file() const;
	// Why the game reads no tile of the file as a placement (under 16 bytes or no til0 mark), "" where it does.
	const std::string &unread() const { return unread_; }

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new tile: cell 0 at the corner (0, 0), unturned (its layer settled where it goes).
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// Every tile a batch put in place (added, duplicated, pasted or moved) takes the layer of the row before it
	// (the first row, the one after it), so the rows stay ascending by MED's layer.
	void settle_placed_rows(StagedRows &rows, const std::vector<NodeId> &placed) override;

private:
	std::string unread_;
};

bool is_til_kind(AssetKind kind);

// The tile placement's validator: its source findings (a file the game reads no tile of, a count past its bytes,
// header words or bytes past the records a save writes as MED does).
std::vector<Diagnostic> validate_til_file(const DocumentBase &document);

enum class TilFinding { Unread, InvalidInput, IgnoredInput, kCount };
const FindingCodeRow &finding_code(TilFinding code);
FindingTable til_finding_codes();

} // namespace opennova::editor
