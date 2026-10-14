#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
// the name defined above it [orig: @ 0x57A830..0x57A8D3], and no part is defined inside a nationality, so where a
// nationality stands among the parts is what its combinations bind (D-PLAYERINFO-35). The rows are the file's parts
// and nationalities in its order, the order the writer puts them down in (formats/avatars, each nationality's
// parts_below), so a save binds every combination as the rows do.

enum class AvatarsKind : NodeKind { Part = 0, Nationality = 1, Division = 2, Combo = 3 };
constexpr NodeKind node_kind(AvatarsKind kind) { return static_cast<NodeKind>(kind); }

// The records, value types over formats/avatars' C structs (whose arrays a copy cannot own).
struct AvatarPartRecord {
	int kind = avatars::AVATAR_PART_HEAD;
	std::string name, display_name, graphic, graphic_j, graphic_s;
	std::array<int, 3> camo{};
	int voice = 0;
	int sex = avatars::AVATAR_SEX_MALE;
};
// Each id is the number the reader takes (formats/avatars: written as its decimal); the words after a key, which
// the reader reads nothing of, are none of the model's.
struct AvatarComboRecord {
	std::string head, body, arms;
	int id = 0;
};
struct AvatarDivisionRecord {
	std::string name_key;
	int id = 0;
	std::vector<AvatarComboRecord> combos;
};
struct AvatarNationalityRecord {
	std::string name_key;
	int id = 0;
	int alignment = avatars::AVATAR_ALIGN_GOOD;
	bool has_alignment = false;
	std::vector<AvatarDivisionRecord> divisions;
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
	std::string name() const override { return std::to_string(nationality.id); }
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
	// A combination added where no head or no body stands above its nationality is refused (the reader would drop
	// it).
	bool accept_list_edit(const Node &row, const ListChange &change, std::string &error) const override;
	// A step that leaves a combination naming a head or a body no part of the kind above its nationality is, or arms
	// no arms part there is, is refused: the game would drop it, or read it without its arms.
	bool accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const override;
	// A new division: the first id its nationality lacks; a new combination: its division's next number, the first
	// head, body and arms above its nationality.
	void after_add(Node &row, const ListChange &change, const RecordHandle &made) override;
	// A part's name is a symbol of its kind in this file (AVATARS.DEF/HEAD); a combination's head, body and arms
	// name parts of their kinds there, each the last part of the name above its nationality [orig:
	// CAvatarDefs_ParseConfigLine @ 0x57A830..0x57A8D3]. Where parts of a kind share a name, the last defines it in
	// the plain section and each earlier one in a section of its own (AVATARS.DEF/HEAD#1, by its rank in the rows'
	// order), which the combinations it binds name.
	void refine_field(const NodeAddress &address, FieldUse &use) const override;
	// A part no combination binds is inert: no nationality stands between it and the next part of its name, or below
	// it.
	void refine_symbol(const NodeAddress &address, SymbolFacts &facts) const override;

public:
	// The text a save writes read again as the game reads it: the reader stopping on it (avatars.reader_stops, a
	// 513th part or a 129th combination the edits made). The writer keeps the rows' order (formats/avatars
	// avatars_write), so every combination binds as the rows do; one naming a part the file defines nowhere is the
	// graph's missing reference.
	void saved_text_findings(std::vector<Diagnostic> &out) const;
	// The line the game's reader stops at in the file as read (its 513th part or 129th combination; 0 where it
	// reads the file whole): the document then holds what the reader read before it, read only.
	size_t reader_stops_line() const { return reader_stops_line_; }

private:
	// The rows' order as the combinations bind by it, kept for the document's state: each row's place among the
	// rows; where parts of a kind share a name, each earlier one's rank among its name's (1 the first); the parts no
	// combination binds.
	struct Order {
		bool made = false;
		uint64_t load_generation = 0, revision = 0;
		std::unordered_map<NodeId, size_t> place;
		std::unordered_map<NodeId, size_t> rank;
		std::unordered_set<NodeId> unbound;
	};
	const Order &order() const;
	// The first part of a kind above the nationality row `nationality` ("" for none).
	std::string first_part(int kind, NodeId nationality) const;
	size_t reader_stops_line_ = 0;
	mutable Order order_;
};

// The scope an Avatars.def's parts of a kind live in: the file's name and the kind ("AVATARS.DEF/HEAD").
std::string avatar_part_scope(const std::string &path, int kind);

// The avatars table's validator: the reader's own notes (a refused nationality or division, a combination whose
// part is not defined before it: dropped), each in the game's words; and the reader stopping, in the file as read
// or in the text a save writes (AvatarsDocument::saved_text_findings).
std::vector<Diagnostic> validate_avatars_file(const DocumentBase &document);

// The reader's notes (invalid, ignored); the reader stopping at a 513th part or a 129th combination
// (avatars.reader_stops: the game fails, so a build is refused).
enum class AvatarsFinding { InvalidInput, IgnoredInput, ReaderStops, kCount };
const FindingCodeRow &finding_code(AvatarsFinding code);
FindingTable avatars_finding_codes();

} // namespace opennova::editor
