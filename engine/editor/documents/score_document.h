#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/score/score.h>

namespace opennova::editor {

// The scoring table (`score.ini`, ADR 0046 S23 B; docs/world/world-wac-ai-re.md §20.8b): read loose by its bare
// name when the game types' default settings are made, over those defaults [orig: GameType_CreateDefaultSettings @
// 0x52DD00 -> ScoreConfig_LoadFile @ 0x52D8A0]; written there with the defaults when it is missing or its version is
// not 40 [orig: ScoreConfig_SaveFile @ 0x52CDD0]. Its rows: the file's version and fanfare first, then its GAMETYPE
// blocks in the file's order (none at a version other than 40: the game reads none of the file), each holding its FIELD lines (the end-of-round scoreboard's columns, in order) and its
// VAR values (the points each event scores). The document reads the file through formats/score with its layout
// modeled and writes it through ScoreConfig_SaveFile's form over that layout.

enum class ScoreKind : NodeKind { Header = 0, Block = 1, Field = 2, Var = 3 };
constexpr NodeKind node_kind(ScoreKind kind) { return static_cast<NodeKind>(kind); }

// The file's own values: what its VERSION line says, and its fanfare line (has_exp_fanfare: the file's EXP_FANFARE
// line before its blocks, its pair whether or not the reader keeps it; score::File's), with what the file's other
// EXP_FANFARE lines leave stored, which a save keeps as they stand.
struct ScoreHeader {
	int32_t version = score::kVersion;
	int32_t exp_fanfare[2] = {0, 0};
	bool has_exp_fanfare = false;
	score::StoredFanfare fanfare_before, fanfare_after;
};

struct ScoreHeaderRow : TableRow {
	ScoreHeader header;
	ScoreHeaderRow() { kind = node_kind(ScoreKind::Header); }
	std::shared_ptr<Node> clone() const override { return std::make_shared<ScoreHeaderRow>(*this); }
	std::string name() const override { return "Score table"; }
	RecordHandle record() const override { return RecordHandle{kind, const_cast<ScoreHeader *>(&header)}; }
	size_t footprint() const override { return sizeof(ScoreHeaderRow) + ids_footprint(); }
};

struct ScoreBlockRow : TableRow {
	score::GameTypeBlock block;
	ScoreBlockRow() { kind = node_kind(ScoreKind::Block); }
	std::shared_ptr<Node> clone() const override { return std::make_shared<ScoreBlockRow>(*this); }
	std::string name() const override { return block.name; }
	RecordHandle record() const override { return RecordHandle{kind, const_cast<score::GameTypeBlock *>(&block)}; }
	size_t footprint() const override;
};

const RecordTable &score_table();

class ScoreDocument : public TableDocument {
public:
	const RecordTable &table() const override { return score_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return score_table().fields(kind); }
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<ScoreDocument>(*this); }
	// The file as the rows hold it.
	score::File file() const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new block: the first game type the file has no block of, no line yet (the game keeps that row's defaults).
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// A copy names none of the file's layout (written in the writer's form).
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	void prepare_record(const Node &row, const ListChange &change, DetachedRecord &record) const override;
};

bool is_score_kind(AssetKind kind);

// The scoring table's validator: a version other than 40 (the game writes its defaults over the file instead), a
// fanfare the game does not keep, a block of a name no game type has (its lines read for nothing), a block of a
// name an earlier one has (both read into one row: the later's lines win), a block of more FIELD lines than a row
// holds.
std::vector<Diagnostic> validate_score_file(const DocumentBase &document);

enum class ScoreFinding { Version, FanfareUnkept, UnknownGameType, GameTypeRepeated, FieldsPast34, kCount };
const FindingCodeRow &finding_code(ScoreFinding code);
FindingTable score_finding_codes();

} // namespace opennova::editor
