#pragma once

#include <functional>

#include "def_notes.h"
#include "def_write.h"

namespace opennova::def {

// Internal writer context. It sees only typed records and reports an error when
// their values cannot be authored in the native grammar.
struct DefRecordWriter {
	DefWriteResult result;
	// What each line put down is (def_notes.h's roles), which a write over a file's modeled layout
	// generates the file's lines from: the record it is of (a slot, one per record written; -1 for the
	// file's own lines), its role and its step; a nested record's place in its parent, a Nested line of no
	// text. `plain`: the record is written in the writer's form whatever its layout (one whose own form the
	// reparse check refused).
	struct Slot {
		uint64_t note = 0;
		DefRecordKind kind = DefRecordKind::Item;
		int parent = -1;
		bool plain = false;
	};
	struct Written {
		int slot = -1;
		DefNotedRole role = DefNotedRole::Free;
		uint8_t step = 0;
		int nested = -1;
		size_t begin = 0, end = 0; // its text in result.text
	};
	std::vector<Slot> slots;
	std::vector<Written> written;
	// The record a line put down now is of, and what it is (the record writer sets Line and the step).
	DefNotedRole put_role = DefNotedRole::Free;
	uint8_t put_step = 0;
	// A record begins (its `note`, of `kind`; nested in the record open, at its `step` there): its slot.
	int begin_record(uint64_t note, DefRecordKind kind, uint8_t step = 0, bool plain = false);
	void end_record();
	// One line, its ending included, as `role` (put_role: Line).
	void put(const std::string &text, DefNotedRole role);
	const DefItemsFile *items = nullptr;
	// One property written as `replaced_key` with `replacement` for its arguments instead of
	// what its members give, whether they differ from the defaults or not: the authored set's
	// rewrite (def_authored_set).
	const DefProperty *replaced = nullptr;
	std::string replaced_key;
	std::vector<std::string> replacement;
	// The layout the file was read with (def.h's DefLineOrder and DefLayout): a record's lines in the
	// order it was read in (`keep_order`; off, the table's order), each indented by `indent` once per
	// `depth` (a block's lines one level deeper; an empty indent is the writer's tab).
	bool keep_order = true;
	std::string indent = "\t";
	int depth = 1;
	void fail(const std::string &record, const std::string &field, const std::string &message);
	// A record's lines. `nested`, where the record holds rows or blocks (an item's attachments, a
	// weapon's sights and actions, an ammo's effects table), writes them at the place its order gives
	// (DEF_LINE_ORDER_ROWS, DEF_LINE_ORDER_BLOCKS), else after its own lines, rows first.
	void record(DefRecordKind kind, const void *value, const std::string &name,
	            const std::function<void(uint8_t step)> &nested = nullptr);
	void line(const std::string &key, const std::vector<std::string> &values);
	// A line at the record's level whatever its depth inside it (a block's header and its end).
	std::string margin(int levels) const;
	// The key and arguments of `property`'s one line for the record `value`, from its
	// members' `values`, as record puts them down (the key a property writes instead:
	// particletesttime, sqb_rate, sqb_error, addeweapg / addeweapc). False when the line has
	// no form (a failure reported), and for the encodings that write a line per flag or
	// entry, which record writes itself.
	bool property_args(DefRecordKind kind, const DefProperty &property, const void *value,
	                   const std::vector<DefValue> &values, const std::string &name, std::string &key,
	                   std::vector<std::string> &args);
	// The bytes of an item's death and clip words (a bit a byte) and whether its door type the alias
	// lines put down as the words stand: the table's own lines for those words are left out where these
	// hold every byte the words have.
	struct AliasCover {
		uint8_t death = 0;
		uint8_t clip = 0;
		bool door_type = false;
	};
	// An item's line under one of the names sharing those words (def.h's DEF_LINE_ORDER_SQB_RATE..
	// DEF_LINE_ORDER_DOOR_DIR), written from the words as they stand, and the bytes it holds.
	void alias_line(const DefItemDef &item, uint8_t step, AliasCover &cover);

private:
	struct Open {
		int slot;
		DefNotedRole role;
		uint8_t step;
	};
	std::vector<Open> open_;
};

// The text a write over a file's modeled layout puts down (def_notes.h), generated in the order the file
// has its lines: each record's line from the words the writer puts down for it now in the layout's shape
// (its spellings while they spell those words, its tokens the game skips, its blanks, separators,
// comment and ending), its lines the game reads nothing of from their tokens, a line the file did not
// have in the writer's form after its record's. A record written in the writer's form (`plain`, the
// reparse check refused its own) keeps its comment lines and the lines the game skips (but a block a later
// one replaced, which would read again); what it does not keep is said in `lost`, a line per record.
std::string def_compose(const DefRecordWriter &writer, const DefTextNotes &notes, std::vector<std::string> *lost = nullptr);
// What the writer put down for each record of the notes (DefNotedRecord::baseline), and each record's lines
// modeled against it (DefNotedShape).
void def_note_baseline(const DefRecordWriter &writer, DefTextNotes &notes);
// The words an `attrib:` line holds after its key (def_write_record.cpp's cap: the game's tokenizer's 29,
// our parser's 16).
size_t def_attrib_words_per_line();

// The text of a squib's or a door's number as the writer puts it down: the shortest decimal the item
// parser's arithmetic over its atof reading takes to the word at both of the game's FPU precisions (the
// 53-bit and the 24-bit of the reloads after the D3D device is up). For the tests.
std::string def_squib_rate_text(int32_t ticks);
std::string def_squib_q16_text(int32_t word);
std::string def_door_open_rate_text(int32_t rate);

} // namespace opennova::def
