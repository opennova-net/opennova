#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

namespace opennova::editor {

// The editing core's vocabulary (ADR 0046 d9): toolkit- and format-neutral. A document
// type maps its native records onto these; the session, the windows and the shell
// never name a format type.

using Value = std::variant<int64_t, double, std::string>;
using NodeId = uint64_t; // the session-local identity of a row or a nested record
using NodeKind = int;    // a document type's own record-kind vocabulary

// Two values the file would write alike: a real compared bit for bit (a NaN is itself).
inline bool same_value(const Value &a, const Value &b) {
	const double *x = std::get_if<double>(&a);
	const double *y = std::get_if<double>(&b);
	return x && y ? std::memcmp(x, y, sizeof(double)) == 0 : a == b;
}

// A row, or a nested record inside a row (`child` set, `kind` the nested kind). `child`
// names a record at any depth: the document type owns its tree (a menu window inside a
// window inside a screen), identities are unique within a document, and
// Document::placement gives a nested record's owner and its index there.
struct NodeAddress {
	NodeId row = 0;
	NodeKind kind = 0;
	NodeId child = 0;
};
inline bool operator==(const NodeAddress &a, const NodeAddress &b) {
	return a.row == b.row && a.kind == b.kind && a.child == b.child;
}
inline bool operator!=(const NodeAddress &a, const NodeAddress &b) { return !(a == b); }

// A field's Value by its type: a whole number (int64_t) for the four whole types, a double for Real, a
// string for Text; but a whole number in units its record does not store whole (a catalog's scaled
// member whose stored word no whole number of the file's line makes: an item's default climb speed,
// 1/293 km/h) reads that real, and a Set of it changes nothing.
enum class FieldType { Integer, Unsigned, Byte, Count, Real, Text };

// What a field names outside its own record: a file, a name some file defines, or another record
// of its own file by its index (a Record reference, S13 D8). A document type resolves each kind
// against the project (reference_status, graph/reference_queries). What each kind is to the graph
// (its token, words, where it resolves, how names compare, what a missing one means) is its row in
// graph/reference_kinds: a new kind is one value here and one row there, at the end of each.
enum class ReferenceKind {
	None, Model, AnimationMap, Ammo, Weapon, Item, Texture, Sound, Particle, AiProfile,
	Font,      // a .fnt by name, possibly through a %VAR% of the stylesheet
	Menu,      // a .mnu file
	TextTable, // a string table (.bin)
	TextId,    // a key of the project's string tables, in the table and section the scope names ("GAMETEXT.BIN/WepDes")
	StyleVar,  // a %NAME% the menu stylesheet defines (a literal value is not a reference)
	Terrain,     // a .trn by base name (a mission's terrain)
	Environment, // a .env by base name (a mission's environment)
	MenuTexture, // a menu's texture: the file retail's menu loader picks by the name's extension
	SoundBank,   // a .lwf sound bank by its file name (a menu SOUND's file)
	Credits,     // a .kda credits file by its file name (a marquee's DATASOURCE)
	MenuScreen,  // a menu screen by NAME, in the menu file the scope names (an ACTION SCREEN's target)
	MenuWindow,  // a menu window by NAME, on the screen the scope names (an ACTION WINDOW's target)
	Animation,   // a .bad clip by file name, its extension optional (an animation map row's variant)
	MenuText,    // a menu's text as the game reads it: never a reference, what a style variable stands for
	ModelRegister, // a model's CTRL register by its index in the model's table (a generator's, a track's, a light's)
	ModelFrame,    // a model's rotation frame by its MTRX row (a part animation's)
	UserPoint,   // a model's user point by name, on the model file the scope names (an item's particle slot)
	TerrainData, // a terrain's height data, a .cpt by the name as written (a .trn's polytrn_polydata)
	Wave,        // a .wav by the file name of the path a sound bank's single holds
	Powerup,        // a powerup.def row by name (an item's powerupdef)
	MissionMarker,  // a mission's marker by its index in the file's markers (a waypoint path's stop)
	MissionEvent,   // a mission's event by its index in the file's event table (an Event trigger's, a ResetEvent action's)
	MissionGroup,   // a mission's group by its index in the file's 64 (an entity's, a parameter's), 0 none
	MissionPath,    // a mission's waypoint path by its number among the file's 128, 0 none and 123..127 commands
	MissionEntity,  // a mission's entity by its SSN, in the mission file the scope names (a parameter's)
	MissionZone,    // a mission's area trigger by its zone id, in the mission file the scope names (a parameter's)
	Script,         // a .wac by the name as written (a mission's own script, a RUN's)
	LoadingImage,   // a mission's loading image, a .pcx by its name (else loadscrn.pcx)
	TilePlacement,  // a mission's tile placement, a .til by its name
	DialogBank,     // a mission's dialog bank, a .dbf by its name
	MissionStrings, // a mission's own string table, a .bin by its name (else medmssn.bin)
	BankWave,       // a sound bank's wave by its name, in the bank the scope names (a member's)
	SoundProfile,   // a SndProf.def profile by its name (an item's sound_profile)
	Shader,         // a model material's shader, by the tag an effect (.fx) registers under
	AnimationKey,   // an animation map's row by its slot's key, in the map file the scope names (a weapon action's anim)
	ItemAlias,      // an item by its alias, items.def's sid (a hudpos.def VEHICLE_HUD block's)
	AvatarPart,     // an avatar part by its name, of the kind and file the scope names (an Avatars.def combo's head)
	Dialog,         // a dialog bank's dialog by its name, in the bank the scope names (a mission's Play dialog: dlg%03i)
	FaceVertex,     // a face animation's vertex by its index in the file's vertices (a triangle's corner)
	FaceAnimation,  // a person's face animation, the .grm its model's name makes (an item's graphic)
};

// Whether the game reads a field on a particular record (Document::field_on). Ignored is
// used only where the original's readers are witnessed in full; anything else not
// witnessed is Unverified.
enum class Applicability { Reads, Ignored, Unverified };

// A choice holds its own strings, so a record can make its own (Document::record_choices: a
// model's parts, its registers).
struct FieldChoice {
	std::string name; // the value as the file writes it (a token, or the number's name)
	int64_t value = 0;
	std::string label; // what the editor shows ("" = the name)
	// What the game does with it, cited, where a table says: its tooltip ("" = nothing said).
	std::string description;
};

// How a field holds a colour, when it does: a text the menu parse reads as a hex AARRGGBB
// word (mnu::color_value; a %VAR% the stylesheet resolves is a text like any other), an
// integer packed 0xRRGGBB, or one channel (0..255) of a red / green / blue group of three
// fields (`group`), drawn with one swatch.
enum class FieldColor { None, HexArgb, PackedRgb, Channel };

// One editable field of a record kind: what the generic inspector renders. A kind's fields live
// in a table of its type (Document::fields), which a record's use of a field points into
// (FieldUse, Document::field_on): the members a record refines are the defaults here.
struct FieldSchema {
	std::string id;
	FieldType type = FieldType::Integer;
	size_t width = 0; // text capacity in bytes, including the terminator
	// The game holds the text in its code page (Windows-1252: one byte a character), so `width`
	// counts characters, not the bytes of the UTF-8 the editor holds (a string table's texts and a
	// menu's, which their documents transcode: a code-page menu's bytes, a Unicode menu's text the
	// reader narrows); else it counts the value's own bytes (a def's, a stylesheet's).
	bool code_page = false;
	ReferenceKind reference = ReferenceKind::None;
	// A number whose reference names its definition by itself plus this (an ammo's tracer id, the items.def
	// id less 100000, which the game compares with the item's own id less 100000 [orig: ItemDef_ParseProperty,
	// the id arm's sub 186A0h @ 0x49EC54; ItemList_FindIndexByTypeId @ 0x49E100]): the reference reaches the
	// name its value plus this makes, and a pick writes the name less it. 0 for none.
	int64_t name_offset = 0;
	std::vector<FieldChoice> choices;
	bool flags = false;        // the choices are bits of one integer
	bool open_choices = false; // the choices are the values known; the file takes any other typed
	bool read_only = false;    // derived from other fields
	// The format may leave the field out (ADR 0002): Clear unsets it and keeps the latent
	// value, which `get` still reads; Document::present says whether it is written.
	bool optional = false;
	Applicability applies = Applicability::Reads; // refined per record by Document::field_on
	// The namespace the field's name lives in, the one it references or the one it defines
	// (a string table's section, a menu's screen); refined per record by Document::field_on
	// ("" = any).
	std::string scope;
	// The symbol the field's value names its record as, which other records reference it
	// by (a menu screen's or window's NAME); refined per record by Document::field_on.
	ReferenceKind defines = ReferenceKind::None;
	// What the editor shows: the field's readable name ("" = the id); the heading of the
	// group its id's first step names ("Position" for position.left, carried too by a
	// block's own switch, which is named as its group; "" = the step itself), which a field
	// of one step naming no group is shown under with the others sharing it (a def's
	// "Physics"); and whether the text may run over several lines.
	std::string label;
	std::string section;
	bool multiline = false;
	// What the format table says of the field, where it says it (nothing is made up here: an
	// unwitnessed unit or range stays unset): the unit its number is in, shown after the
	// value; the table's note, the field's tooltip; how the file spells its key where that
	// differs from the id ("" = the id); the range a number keeps to (`ranged`), and the
	// step a drag moves it by (0 = the control's own).
	std::string unit;
	std::string description;
	std::string token;
	bool ranged = false;
	double min = 0.0, max = 0.0, step = 0.0;
	FieldColor color = FieldColor::None;
	// The fields drawn on one row, named by it (a position's x / y / z, a colour's red / green
	// / blue, a matrix row): neighbours in the kind's fields sharing it ("" = a row alone).
	std::string group;
};

// A finding about the source text. A blocking one names input the typed model cannot
// carry: editing and saving wait for the source to be corrected. A non-blocking one
// names input the game ignores: reported, dropped on save.
struct SourceIssue {
	bool blocks = true;
	size_t line = 0;
	std::string record;
	std::string field;
	std::string message;
	std::string locator; // the record's Document::locator, when the type knows it ("" = the file)
	// A blocking issue the game's own reader stops at, or corrupts its state over, beside the input the
	// model cannot carry (source_issue_findings' `stops` row; the audit's ABORT-FILE).
	bool game_stops = false;
};

struct SerializeResult {
	std::string text;
	std::vector<SourceIssue> issues;
	// What a save of the text says beyond writing it, a sentence each (a record written in the table's
	// order: its own would read back otherwise); never a refusal.
	std::vector<std::string> notes;
	bool ok() const { return issues.empty(); }
};

enum class ReferenceStatus {
	NotAReference, // the field is not a reference, or it is empty / NONE / NULL
	Present,
	Missing,
	Unverified, // no symbol table for this kind yet
};

} // namespace opennova::editor
