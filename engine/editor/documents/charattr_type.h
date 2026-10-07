#pragma once

#include <memory>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/text_document.h>
#include <formats/charattr/charattr.h>

namespace opennova::editor {

// The character attributes type (ADR 0046 DI-09's charattr follow-up): charattr.def held as its text,
// which every peer's game reads once at boot [orig: Game_Run @ 0x4A7FE3 -> CharAttr_LoadFromDef
// @ 0x412140] through the ConfigFile reader, whose lines end at CR LF alone (TextLineEnds::CrLf); the
// port of that load is formats/charattr's read_table, which says where each value it reads is written.
// Its references are the three *_CAMMO item type ids of each class the game reads: an items.def item by
// its id less 100000 (GraphEdge::name_offset), the one a player of the class spawns as by the mission's
// camouflage, ItemList_FindIndexByTypeId's first items.def row for a type no item has [orig:
// Entity_SpawnFromAnimSlotProperty @ 0x43c390 -> CharAttr_GetCammoTypeId @ 0x4127b0 ->
// ItemList_FindIndexByTypeId @ 0x49e100]. A section the game never reads names nothing, so its values
// are no references. Its findings: a section the game never reads, an ATTRIBUTES word no attribute is,
// a value the reader reads as 0 for not being a number, and a class read with no camouflage item.
std::unique_ptr<DocumentBase> make_charattr_document();
std::vector<Diagnostic> validate_charattr_file(const DocumentBase &document);
void charattr_references(const TextDocument &document, std::vector<TextReference> &out);
// The entry lines the loader reads the same table without (DocumentType::config_idle_lines, the ConfigFile pool
// rule's fix): every line of values it never reads (a key it never asks for, a later section of a label, a
// section of no class), and each line it reads whose class's row is the same with the line commented out (a
// key at 0, which the cleared table holds for a key the section lacks [orig: CharAttr_LoadFromDef @
// 0x412168], or one a later line of the key repeats), kept only where all of them out together still read the
// same table, byte for byte (charattr::same_rows). A class after the first the file lacks is left alone: it is
// meant to be read, which charattr.unread_section says.
void charattr_idle_lines(const TextDocument &document, std::vector<size_t> &line_starts);

// All listed (none refuses a build: the game reads what it can of the file and goes on).
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

// A charattr document's text through the game's loader: the table and where it read each value.
bool read_charattr_text(const TextDocument &document, charattr::Table &table, charattr::Reading &reading);

} // namespace opennova::editor
