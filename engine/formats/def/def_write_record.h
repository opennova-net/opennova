#pragma once

#include <functional>

#include "def_schema.h"
#include "def_write.h"

namespace opennova::def {

// Where a record's rows and its blocks stand among its lines (DefRecordWriter::record's `nested`): after its
// own lines, rows first. A property's place in its kind's table is below both.
inline constexpr uint8_t kDefStepRows = 254;
inline constexpr uint8_t kDefStepBlocks = 255;

// Internal writer context. It sees only typed records and reports an error when
// their values cannot be authored in the native grammar.
struct DefRecordWriter {
	DefWriteResult result;
	// One line, its ending included.
	void put(const std::string &text);
	const DefItemsFile *items = nullptr;
	// One property written as `replaced_key` with `replacement` for its arguments instead of
	// what its members give, whether they differ from the defaults or not: the authored set's
	// rewrite (def_authored_set).
	const DefProperty *replaced = nullptr;
	std::string replaced_key;
	std::vector<std::string> replacement;
	// A record's lines are indented a tab per `depth` (a block's lines one level deeper).
	int depth = 1;
	void fail(const std::string &record, const std::string &field, const std::string &message);
	// A record's lines, in its kind's property table's order. `nested`, where the record holds rows or blocks
	// (an item's attachments, a weapon's sights and actions, an ammo's effects table), writes them after its
	// own lines, rows first (kDefStepRows, kDefStepBlocks).
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
};

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
