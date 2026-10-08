#pragma once

#include <string>
#include <vector>

#include <formats/def/def.h>
#include <formats/def/def_write.h>

namespace opennova::def {

// hudpos.def's writer (ADR 0003: made from the model, never from the file it was read from): what the
// game's reader reads, a line a key, every line ending CR LF as its reader ends a line [orig:
// File_ParseASCIIFile @ 0x53D8C7..0x53D8F5]. Each key's values are written in the order and the form
// HUD_ParseHudposToken reads them [orig: HUD_ParseHudposToken @ 0x59F370, each arm's atof / strcpy of
// tokens[2]..]: a number as a whole number (ALPHAFADE's as the shortest decimal that reads back the
// same double), an alignment as its word ("left", "right", "center": HUD_ParseTextAlignment @
// 0x59D6B0), a name as written (quoted where a separator or a comment start would cut it, as the
// tokenizer keeps it [orig: Terrain_TokenizeConfigLine @ 0x53CB60, the delimiters @
// 0x53CC33..0x53CC4C]), the values after the key's tab separated by commas. The writer's own order: the
// fonts, the rects, the colours, the map, the positioned texts, the weapon slot bar, the anchors, the
// single values, the stances, the declutter rows, the graphics, then the vehicle blocks. A key whose
// values are the model's unauthored ones (what a file without it reads) is left out; a stance, a
// declutter row, a static frame and a vehicle block each make lines of their own, in the model's
// order. What the model holds nothing of (a comment, a key no arm reads) is no part of what it writes.

// One line the writer writes: its key as the game's _stricmp spells it, and its values, each one token.
struct HudposLine {
	std::string key;
	std::vector<std::string> values;
};

// The lines the writer writes of `hud`, in its order (a vehicle block its VEHICLE_HUD line, its lines
// and its VEHICLE_END line).
std::vector<HudposLine> hudpos_lines(const DefHudPosDef &hud);

// A line's text, without its line end: the key, a tab, the values separated by commas.
std::string hudpos_line_text(const HudposLine &line);

// The values the writer writes for one key (compared without case, as the parser's _stricmp), whether
// the model authors it or not (an unauthored key's are what a file without it reads): a HUDSTANCE's by
// its id (`first`), a HUDLS_SLOT's by its slot (`first`, 1 to 10), a HUDDECLUT_ row's the last of its
// name, a StaticFrame's the last frame's (the one the game keeps). False for a key the writer does not
// write alone (a vehicle block's), and for a HUDSTANCE, a HUDLS_SLOT or a StaticFrame the model has
// none of.
bool hudpos_key_values(const DefHudPosDef &hud, const std::string &key, const std::string &first,
                       std::vector<std::string> &out);

// The model a file with no line reads: the parser's own start [orig: the globals' static values,
// HUDORDERS' -1 / -1 at dword_2723D84 / dword_2723D88; the rest BSS zero].
const DefHudPosDef &hudpos_unauthored();

// hudpos.def of `file`: hudpos_lines' text, each line ending CR LF, read back through the family's
// parser and compared (def_hudpos_same): a model that does not read back the same (a name the tokenizer
// cannot carry, a stance with no name, which the parser reads only with one) is a blocking issue.
DefWriteResult def_write_hudpos(const DefHudPosFile &file);

// Whether two models read the same: every value of every key, every stance, declutter row, static frame
// and vehicle block. False with the first key that differs in `difference` ("" for none).
bool def_hudpos_same(const DefHudPosDef &a, const DefHudPosDef &b, std::string *difference = nullptr);

// A number as the writer writes it: an integer whole, a double as the shortest decimal the parser's
// atof reads back as the same double.
std::string hudpos_number(int value);
std::string hudpos_number(double value);
// An alignment's word: "left" (0), "right" (1), "center" (2) [orig: HUD_ParseTextAlignment @ 0x59D6B0].
const char *hudpos_align_word(int align);

} // namespace opennova::def
