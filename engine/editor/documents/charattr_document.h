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
// reads the file through the loader and writes the table from the rows alone (charattr::compose_table, S26): each
// class's section with the keys the loader reads that hold other than 0; the file's comments and the lines the game
// reads nothing of are not kept.

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
	// Whether the file is in the ConfigFile's binary (CBIN) form: the document holds what the game reads of it
	// (its text form's classes) read only, a save writing the text form alone (charattr.binary_form).
	bool binary_form() const { return binary_form_; }
	// The file as it sits: the bytes it was loaded from, or the text the last save wrote (read again then), which
	// the game reads and a build packs while nothing is changed. A save writes the table in the editor's form
	// instead, from what the game reads of it.
	const std::string &stored_text() const { return stored_text_; }

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

private:
	bool binary_form_ = false;
	std::string stored_text_;
};

// The character attributes' validator, over the file as it sits (stored_text: what the game reads of it): a section
// the game never reads, an ATTRIBUTES word no attribute is, a value the reader reads as 0 for not being a number, a
// class read with no camouflage item; and the ConfigFile pool rule (core document.config_overrun) over the file a
// build packs, the file as it sits while nothing is changed, else the text a save writes (a build saves first). A
// file in the CBIN form is that finding alone (charattr.binary_form).
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
	// A file in the ConfigFile's binary (CBIN) form, which the game reads through its binary reader: the editor
	// holds its classes read only (it writes the text form alone).
	BinaryForm,
	kCount
};
const FindingCodeRow &finding_code(CharAttrFinding code);
FindingTable charattr_finding_codes();


} // namespace opennova::editor
