#pragma once

#include <functional>

#include "def_write.h"

namespace opennova::def {

// Internal writer context. It sees only typed records and reports an error when
// their values cannot be authored in the native grammar.
struct DefRecordWriter {
	DefWriteResult result;
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
};

} // namespace opennova::def
