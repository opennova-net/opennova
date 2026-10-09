#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/avatars/avatars.h>

namespace opennova::editor {

// The avatars table (`Avatars.def`, ADR 0046 S23 B; docs/playerinfo/avatars-re.md): the parts a player is made of
// (heads, bodies, arms: each a model and its text key) and the nationalities, each holding its divisions, each
// holding its combinations of a head, a body and arms, read once [orig: CAvatarDefs_Init @ 0x57B180 ->
// CAvatarDefs_ParseConfigLine @ 0x57A3F0]. A combination names its parts by name, the last part of the kind and
// the name defined before it [orig: @ 0x57A830..0x57A854]. The rows are the file's parts and nationalities in its
// order (each kind's own order kept: the file may interleave them); the document reads the file through
// formats/avatars with its layout modeled and writes it back over that layout.

enum class AvatarsKind : NodeKind { Part = 0, Nationality = 1, Division = 2, Combo = 3 };
constexpr NodeKind node_kind(AvatarsKind kind) { return static_cast<NodeKind>(kind); }

// The records, value types over formats/avatars' C structs (whose arrays a copy cannot own).
struct AvatarPartRecord {
	int kind = avatars::AVATAR_PART_HEAD;
	std::string name, display_name, graphic, graphic_j, graphic_s;
	std::array<int, 3> camo{};
	int voice = 0;
	int sex = avatars::AVATAR_SEX_MALE;
	std::vector<std::string> raw_lines; // lines the walk reads nothing of inside the block (the layout keeps them)
	uint64_t note = 0;
};
struct AvatarComboRecord {
	std::string raw_id, head, body, arms;
	int id = 0;
	uint64_t note = 0;
};
struct AvatarDivisionRecord {
	std::string raw_id, name_key, flags;
	int id = 0;
	std::vector<AvatarComboRecord> combos;
	std::vector<std::string> raw_lines;
	uint64_t note = 0;
};
struct AvatarNationalityRecord {
	std::string raw_id, name_key, flags;
	int id = 0;
	int alignment = avatars::AVATAR_ALIGN_GOOD;
	bool has_alignment = false;
	std::vector<AvatarDivisionRecord> divisions;
	std::vector<std::string> raw_lines;
	uint64_t note = 0;
};

struct AvatarPartRow : TableRow {
	AvatarPartRecord part;
	AvatarPartRow() { kind = node_kind(AvatarsKind::Part); }
	std::shared_ptr<Node> clone() const override { return std::make_shared<AvatarPartRow>(*this); }
	std::string name() const override { return part.name; }
	RecordHandle record() const override { return RecordHandle{kind, const_cast<AvatarPartRecord *>(&part)}; }
	size_t footprint() const override;
};
struct AvatarNationalityRow : TableRow {
	AvatarNationalityRecord nationality;
	AvatarNationalityRow() { kind = node_kind(AvatarsKind::Nationality); }
	std::shared_ptr<Node> clone() const override { return std::make_shared<AvatarNationalityRow>(*this); }
	std::string name() const override { return nationality.raw_id; }
	RecordHandle record() const override { return RecordHandle{kind, const_cast<AvatarNationalityRecord *>(&nationality)}; }
	size_t footprint() const override;
};

const RecordTable &avatars_table();

class AvatarsDocument : public TableDocument {
public:
	const RecordTable &table() const override { return avatars_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return avatars_table().fields(kind); }
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<AvatarsDocument>(*this); }
	// The table as formats/avatars holds it, from the rows (free it with avatars::avatars_free).
	avatars::AvatarsFile file() const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new part (a head named apart from the file's) or nationality (the first id 0..31 the file has none of).
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// A copy names none of the file's layout (written in the writer's form).
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	void prepare_record(const Node &row, const ListChange &change, DetachedRecord &record) const override;
	// A combination added where the file defines no head or no body is refused (the reader would drop it).
	bool accept_list_edit(const Node &row, const ListChange &change, std::string &error) const override;
	// A new division: the first id its nationality lacks; a new combination: its division's next number, the
	// file's first head, body and arms.
	void after_add(Node &row, const ListChange &change, const RecordHandle &made) override;
	// A part's name is a symbol of its kind in this file (AVATARS.DEF/HEAD); a combination's head, body and arms
	// name parts of their kinds there.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	// Of two parts of a kind and a name, a combination after both takes the later: the earlier is inert there.
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;

private:
	std::string first_part(int kind) const;
};

bool is_avatars_kind(AssetKind kind);
// The scope an Avatars.def's parts of a kind live in: the file's name and the kind ("AVATARS.DEF/HEAD").
std::string avatar_part_scope(const std::string &path, int kind);

// The avatars table's validator: the reader's own notes (a refused nationality or division, a combination whose
// part is not defined before it: dropped), each in the game's words.
std::vector<Diagnostic> validate_avatars_file(const DocumentBase &document);

enum class AvatarsFinding { InvalidInput, IgnoredInput, kCount };
const FindingCodeRow &finding_code(AvatarsFinding code);
FindingTable avatars_finding_codes();

} // namespace opennova::editor
