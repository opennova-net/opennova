#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/change_set.h>
#include <editor/model/value.h>

namespace opennova::editor {

// A reference one file makes (ADR 0046 d10, S7), extracted from the engine's own parsed records:
// from a source file (and the record and field inside it, or the span of its text) to a target
// name in a namespace, the ReferenceKind.
struct GraphEdge {
	std::string source;   // the referencing file, project-relative
	std::string record;   // the record inside it, every name from the row down ("" = the file itself)
	std::string record_key; // the record as itself (Document::record_identity), "" for none
	// The record in its type's words with no project names (record_display's own words: a menu's action
	// as what it does), where they are not its name: what a place in a file names the record by where the
	// file is not open (a closed file's Problems row, the import plan, a find's uses, a file's page; the
	// plain-words lane). "" where its name says it.
	std::string record_title;
	// A document record's place, stable across a reload (Document::locator); a text's span's,
	// "line:column" (TextDocument::locator).
	std::string locator;
	NodeAddress address;  // the record's address in the document the edge was read from
	std::string field;    // the field id, or a native format's slot ("material[2].texture[0]")
	ReferenceKind kind = ReferenceKind::None;
	std::string value;    // the reference as written
	std::string target;   // what is looked up: the value after a style variable resolved, normalized
	// The kind `target` is a name of: the fallback's (fallback_as) where the lookup reached that name, else None
	// (the edge's own); set with target (AssetGraph::resolve_edge), which the index keys the target by.
	ReferenceKind target_kind = ReferenceKind::None;
	// A string id's table and section ("GAMETEXT.BIN/WepDes", a menu's "MENUTXT.BIN/menu";
	// "/menu" names no table: the window reads none); "" = any table.
	std::string scope;
	bool rewritable = false; // the source is a document type and the field is in its schema
	// On a style variable edge: what the variable's value must name there (a font, a menu
	// texture), None where it stands for a colour.
	ReferenceKind through = ReferenceKind::None;
	// What the reference's loader picks the one file the name loads by, as its kind's row reads
	// it (FieldUse::loader_arg, reference_file_candidates: a model's texture row's type, another
	// texture's role, documents/texture_roles.h texture_role_arg, or the game's loader of it alone,
	// texture_loader_arg); -1 for none.
	int32_t loader_arg = -1;
	// What the record says of the use beyond that (FieldUse::use_context: a model texture row's slot,
	// flags and its material's alpha test, texture_roles.h); 0 for none.
	uint32_t use_context = 0;
	// In a text document (ADR 0046 S13 D9), where the name is written: its line and column
	// (1-based) and its length, which a rename rewrites and a Go to opens the document at; a line
	// of 0 for a reference of a record (TextDocument, TextReference).
	TextSpan span;
	// A second name the lookup takes when the value finds nothing (a script's AMMO operand: its
	// name, then "ammo_" and its name [orig: WacScript_ResolveParameter @ 0x4F2E21..0x4F2E92]); ""
	// for none. A symbol kind's edge only: it resolves to the first of the two a lookup finds.
	std::string fallback;
	// The kind the fallback is a name of, where it is another kind's (an ammo's tracer item: its type id, then the
	// item named as the ammo, ItemName [orig: AmmoDef_ParseProperty @ 0x40A5DA..0x40A5FE, ItemList_FindIndexByTypeId
	// then ItemList_FindIndexByPrimaryName over the ammo's name]); None for the edge's own kind.
	ReferenceKind fallback_kind = ReferenceKind::None;
	ReferenceKind fallback_as() const { return fallback_kind == ReferenceKind::None ? kind : fallback_kind; }
	// The kind the index keys the edge's target by.
	ReferenceKind target_as() const { return target_kind == ReferenceKind::None ? kind : target_kind; }
	// The scopes the lookup tries after `scope`, in order, where the name finds nothing there (a
	// mission's text key: the mission's own table, then GAMETEXT.BIN [orig:
	// MissionText_GetStringByKeyOrGameText @ 0x51ECD0]); none for a lookup of one scope. A symbol
	// kind's edge only: it resolves to the first scope's definition a lookup finds (a fallback is a
	// second name, this a second place).
	std::vector<std::string> scopes_after;
	// The table the lookup reads in the scope's table's place when the project has no file of that
	// table's name, the scope's section kept: the game loads the one or the other, never both (a
	// mission's text: <stem>.bin, else medmssn.bin [orig: TextResource_LoadMissionTextBin @0x51ed90]);
	// "" for none. A symbol kind's edge only.
	std::string scope_alternate;
	// The file whose presence makes the scope the lookup's (a script of a mission's name reads that
	// mission's text: <stem>.bms): without it the name resolves in any table, and no rename rewrites
	// the use (its table is whichever mission's script compiles it in, AssetGraph::rewrites); "" for
	// none. A symbol kind's edge only.
	std::string scope_owner;
	// The game runs without the file (a mission's script, which 40 of 115 shipped missions have,
	// ADR 0046 S14): the graph makes no finding of it missing; the resolver, the pickers, References
	// and the import read the edge as any other.
	bool optional = false;
	// The file whose presence the reader needs before it reads this one at all (a mission's dialog
	// sounds, read only when its .dbf exists [orig: DialogManager_LoadFromFile @ 0x44e650, opened
	// from DialogSystem_Init @ 0x5275e0 only when the .dbf exists @ 0x527648]); while the project
	// lacks it the edge is no reference (NotAReference: no finding, no user, no rename companion,
	// nothing the import follows). "" for none. A file kind's edge only.
	std::string needs;
	// What the field's number names its definition by less (FieldUse::name_offset: an ammo's tracer
	// id, an item's type id, the items.def id less 100000): `value` is the name it reaches, the field
	// holds it less this, which a rename writes back so. 0 for none.
	int64_t name_offset = 0;
	// A use whose field holds the number its name forms, "%s%03i" of the prefix and the number (a
	// mission's Play dialog names dlg%03i by its number [orig: Dialog_PlayByIndex @ 0x527ae0]): the
	// prefix, so a rename writes the number the new name forms, refusing a name no number forms; "" for
	// a use that writes the name itself. A symbol kind's edge only.
	std::string key_prefix;
};

// A name a file defines that other files may reference: a document's field whose field_on
// says it defines one (FieldUse::defines; what the type's lookup makes of it,
// Document::refine_symbol), or a native file's name.
struct GraphSymbol {
	ReferenceKind kind = ReferenceKind::None;
	std::string name;    // normalized
	std::string display; // as defined
	std::string value;   // a style variable's value; a record set's record by its own name (S13 D8); an
	                     // ItemName's item id (the STR_ITM key its gametext name is under)
	std::string file;    // the defining file, project-relative
	std::string record;  // the defining record, every name from the row down ("" = the file itself)
	std::string record_key; // the record as itself (Document::record_identity), "" for none
	std::string title;   // the defining record in its type's own words where they are not its name (GraphEdge::record_title)
	std::string locator; // a document record's place, stable across a reload (Document::locator)
	NodeAddress address; // the defining record in the document it was read from
	std::string field;   // the field that defines it ("" for a native file's)
	// Where a lookup finds it: a string id's "TABLE.BIN/Section"; a menu screen's menu file
	// ("MAIN.MNU"); a menu window's menu file and screen ("MAIN.MNU/STARTUP"); a user point's
	// model file ("GUN.3DI"), one of its first 16 in that file's first-16 section
	// ("GUN.3DI/FIRST16", kFirstUserPointsSection); an animation map row's map file ("M4_1ST.ADM").
	std::string scope;
	// Defined, but not what the game reads: a style variable of a stylesheet the game does
	// not load, one defined again later in its file (the game reads the last), one menu_style.mns
	// defines and brand.mns defines again, one on a line after the place the game stops reading
	// its file; a string id in a later section of a name the table already has (a lookup reads
	// the first [orig: TextResource_FindEntryBySectionAndKey @ 0x75d250]); a menu screen or
	// window no by-name lookup returns (MnuDocument::lookup_names: an earlier screen of a name,
	// a later window of a name, one under a window with no NAME or on a screen a later one
	// shadows); an animation map's row whose key names no slot.
	bool inert = false;
	// Why no lookup finds it, in a few words (the picker's, the find's), when it is inert.
	std::string inert_reason;
	// The line of its file it is defined on, where its type knows it (a stylesheet variable's
	// first line); 0 for none.
	size_t line = 0;
};

// What one file references and defines.
struct Extracted {
	std::vector<GraphEdge> edges;
	std::vector<GraphSymbol> symbols;
};

} // namespace opennova::editor
