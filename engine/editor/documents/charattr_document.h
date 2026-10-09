#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/table_document.h>
#include <formats/charattr/charattr.h>

namespace opennova::editor {

// The character attributes (`charattr.def`, ADR 0046 DI-09's charattr follow-up, S23 B): the classes' rows every
// peer's game reads once at boot [orig: Game_Run @ 0x4A7FE3 -> CharAttr_LoadFromDef @ 0x412140] through the
// ConfigFile reader, CHARACTER1, CHARACTER2 and on to the first class the file has no section of, sixteen at most
// (formats/charattr). A row a class, its fields the keys the loader reads in the units it reads them: the eight
// scales, the three camouflage items (each an items.def item by its id less 100000, the one a player of the class
// spawns as by the mission's camouflage [orig: Entity_SpawnFromAnimSlotProperty @ 0x43C390 -> CharAttr_GetCammoTypeId
// @ 0x4127B0 -> ItemList_FindIndexByTypeId @ 0x49E100]), the run modifier and the ATTRIBUTES words. The document
// reads the file through the loader with its layout modeled and writes it back over that layout
// (charattr::write_table): the file as it was but for a changed value's line.

enum class CharAttrKind : NodeKind { Class = 0 };
constexpr NodeKind node_kind(CharAttrKind kind) { return static_cast<NodeKind>(kind); }

struct CharAttrRow : TableRow {
	charattr::ClassRow row;

	CharAttrRow() { kind = node_kind(CharAttrKind::Class); }
	std::shared_ptr<Node> clone() const override { return std::make_shared<CharAttrRow>(*this); }
	std::string name() const override { return "CHARACTER" + std::to_string(row.class_id); }
	RecordHandle record() const override { return RecordHandle{kind, const_cast<charattr::ClassRow *>(&row)}; }
	size_t footprint() const override { return sizeof(CharAttrRow) + ids_footprint(); }
};

const RecordTable &charattr_table();

// The pool rule's fix (documents/config_overrun.h): an Edit SetFileValue of this field, its value the file's text
// with the lines the loader reads the same without commented out, which the document takes as its layout.
inline constexpr const char *kCommentIdleLines = "comment_idle_lines";

class CharAttrDocument : public TableDocument {
public:
	const RecordTable &table() const override { return charattr_table(); }
	static const std::vector<FieldSchema> &schema(NodeKind kind) { return charattr_table().fields(kind); }
	std::string record_title(const NodeAddress &address) const override;
	SerializeResult serialize() const override;
	std::string save_words() const override;
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<CharAttrDocument>(*this); }
	// The table as the rows hold it, each class at its id's place.
	charattr::Table file() const;
	// The text a save writes, the ConfigFile pool left unchecked (charattr::compose_table): what the game would read.
	// False with the reason for a table no file loads as.
	bool composed(std::string &text, std::string &error) const;

protected:
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
	           Diagnostic &error) override;
	// A new class: the one after the file's last, sixteen at most (the loader reads none past it).
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
	                                std::string &error) override;
	// A copy is the class after the file's last, in the writer's form.
	void prepare_duplicate(Node &copy, const Node &original,
	                       const std::vector<std::shared_ptr<const Node>> &rows) const override;
	// The classes stay CHARACTER1 to CHARACTERn, each once: the loader stops at the first it lacks.
	bool accept_step(const EditStep &step, const StagedRows &rows, StepRefusal &refusal) const override;
	// kCommentIdleLines: the layout of the text the edit holds, read again (the classes it reads are the rows').
	bool set_file_value(std::shared_ptr<const FileState> &state, const Edit &edit, Diagnostic &error) override;
};

bool is_charattr_kind(AssetKind kind);

// The character attributes' validator: a section the game never reads, an ATTRIBUTES word no attribute is, a value
// the reader reads as 0 for not being a number, a class read with no camouflage item; and the ConfigFile pool rule
// (core document.config_overrun) over the text a save writes, its fix commenting out the lines the loader reads the
// same table without (kCommentIdleLines).
std::vector<Diagnostic> validate_charattr_file(const DocumentBase &document);

// All listed (none refuses a build: the game reads what it can of the file and goes on). A file past the
// ConfigFile reader's pool is the core document.config_overrun's (documents/config_overrun.h), which refuses one.
enum class CharAttrFinding {
	// A [CHARACTERn] the game never reads: after the first class the file lacks, a second of a label, or
	// a section of no class's label.
	Unread,
	// An ATTRIBUTES word no attribute is (AutoScope, SpreadBonus, KnifeBonus, Medic, WaterGirl): the class
	// gets nothing for it. NULL and NONE, written for none, are not one.
	AttributeWord,
	// A value the reader classifies as text where the key takes a number: it reads 0.
	NotANumber,
	// A class the game reads with no camouflage item for a camouflage (no key, or 0): a player of the class
	// spawns as items.def's first row (or the item whose id is 100000) in such a mission.
	NoCammo,
	kCount
};
const FindingCodeRow &finding_code(CharAttrFinding code);
FindingTable charattr_finding_codes();

// The lines of a charattr.def text the loader reads the same table without (the pool rule's fix): every line of
// values it never reads (a key it never asks for, a later section of a label, a section of no class), and each line
// it reads whose class's row is the same with the line commented out (a key at 0, which the cleared table holds for
// a key the section lacks [orig: CharAttr_LoadFromDef @ 0x412168], or one a later line of the key repeats), kept
// only where all of them out together still read the same table, byte for byte (charattr::same_rows). A class after
// the first the file lacks is left alone: it is meant to be read, which charattr.unread_section says. Each line by
// its first byte's offset.
void charattr_idle_lines(const std::string &text, std::vector<size_t> &line_starts);

} // namespace opennova::editor
