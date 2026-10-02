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
	// A document record's place, stable across a reload (Document::locator); a text's span's,
	// "line:column" (TextDocument::locator).
	std::string locator;
	NodeAddress address;  // the record's address in the document the edge was read from
	std::string field;    // the field id, or a native format's slot ("material[2].texture[0]")
	ReferenceKind kind = ReferenceKind::None;
	std::string value;    // the reference as written
	std::string target;   // what is looked up: the value after a style variable resolved, normalized
	// A string id's table and section ("GAMETEXT.BIN/WepDes", a menu's "MENUTXT.BIN/menu";
	// "/menu" names no table: the window reads none); "" = any table.
	std::string scope;
	bool rewritable = false; // the source is a document type and the field is in its schema
	// On a style variable edge: what the variable's value must name there (a font, a menu
	// texture), None where it stands for a colour.
	ReferenceKind through = ReferenceKind::None;
	// What the reference's loader picks the one file the name loads by, as its kind's row reads
	// it (FieldUse::loader_arg, reference_file_candidates: a model's texture row's type, or the
	// game's loader of another texture, texture_loader_arg); -1 for none.
	int32_t loader_arg = -1;
	// In a text document (ADR 0046 S13 D9), where the name is written: its line and column
	// (1-based) and its length, which a rename rewrites and a Go to opens the document at; a line
	// of 0 for a reference of a record (TextDocument, TextReference).
	TextSpan span;
	// A second name the lookup takes when the value finds nothing (a script's AMMO operand: its
	// name, then "ammo_" and its name [orig: WacScript_ResolveParameter @ 0x4F2E21..0x4F2E92]); ""
	// for none. A symbol kind's edge only: it resolves to the first of the two a lookup finds.
	std::string fallback;
};

// A name a file defines that other files may reference: a document's field whose field_on
// says it defines one (FieldUse::defines; what the type's lookup makes of it,
// Document::refine_symbol), or a native file's name.
struct GraphSymbol {
	ReferenceKind kind = ReferenceKind::None;
	std::string name;    // normalized
	std::string display; // as defined
	std::string value;   // a style variable's value; a record set's record by its own name (S13 D8)
	std::string file;    // the defining file, project-relative
	std::string record;  // the defining record, every name from the row down ("" = the file itself)
	std::string locator; // a document record's place, stable across a reload (Document::locator)
	NodeAddress address; // the defining record in the document it was read from
	std::string field;   // the field that defines it ("" for a native file's)
	// Where a lookup finds it: a string id's "TABLE.BIN/Section"; a menu screen's menu file
	// ("MAIN.MNU"); a menu window's menu file and screen ("MAIN.MNU/STARTUP"); a user point's
	// model file ("GUN.3DI").
	std::string scope;
	// Defined, but not what the game reads: a style variable of a stylesheet the game does
	// not load, one defined again later in its file (the game reads the last), one menu_style.mns
	// defines and brand.mns defines again, one on a line after the place the game stops reading
	// its file; a string id in a later section of a name the table already has (a lookup reads
	// the first [orig: TextResource_FindEntryBySectionAndKey @ 0x75d250]); a menu screen or
	// window no by-name lookup returns (MnuDocument::lookup_names: an earlier screen of a name,
	// a later window of a name, one under a window with no NAME or on a screen a later one
	// shadows); a model's user point past the first 16 an item's lookup scans.
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
