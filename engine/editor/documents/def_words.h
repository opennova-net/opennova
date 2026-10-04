#pragma once

#include <cstddef>
#include <string>

#include <formats/def/def_schema.h>

namespace opennova::editor {

// The def catalogs' members in a modder's words (ADR 0046, the UX round's plain-words lane): each
// member of each record kind (def_fields) its label, the section the Inspector groups it under and
// what the game does with it, from the RE records (docs/world/itemdef-re.md, powerup-re.md,
// vehicle-client-movers-re.md, world-wac-ai-re.md, hud-re.md, ptl-format-re.md and the runtime's
// cited consumers), each row citing its witness; a meaning no witness gives starts "Unknown:" and
// says only what is known. Tooling words over the game's meanings: the format's own table
// (formats/def/def_schema_properties.cpp) keeps what the parser makes of a line (its units, its
// note), which follows the meaning in the field's tooltip.
struct DefWords {
	const char *id;      // the member, as def_fields names it ("soundloops[2]", "particlefx.effect")
	const char *label;   // "Sound loop 3"
	const char *section; // "Sound"; "" for a kind of one section (its fields need no heading)
	const char *meaning; // what the game does with it, or "Unknown: ..."
	const char *cite;    // its witness: "[orig: Name @ 0xADDR]", or a record and its section
};

// The words of a kind's members, one row per member in def_fields' order (a test holds the two in
// step); null with `count` 0 for a kind with none.
const DefWords *def_words(def::DefRecordKind kind, size_t &count);
// One member's words (null for a member of no row).
const DefWords *def_words_of(def::DefRecordKind kind, const std::string &id);
// A kind's sections in the order the Inspector shows them (its members are put in that order,
// each section's in def_fields' order), null-ended; a section's place in it (SIZE_MAX for none).
const char *const *def_sections(def::DefRecordKind kind);
size_t def_section_rank(def::DefRecordKind kind, const char *section);

} // namespace opennova::editor
