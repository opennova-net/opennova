#pragma once

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
	void fail(const std::string &record, const std::string &field, const std::string &message);
	void record(DefRecordKind kind, const void *value, const std::string &name);
	void line(const std::string &key, const std::vector<std::string> &values);
	// The key and arguments of `property`'s one line for the record `value`, from its
	// members' `values`, as record puts them down (the key a property writes instead:
	// particletesttime, sqb_rate, sqb_error, addeweapg / addeweapc). False when the line has
	// no form (a failure reported), and for the encodings that write a line per flag or
	// entry, which record writes itself.
	bool property_args(DefRecordKind kind, const DefProperty &property, const void *value,
	                   const std::vector<DefValue> &values, const std::string &name, std::string &key,
	                   std::vector<std::string> &args);
};

} // namespace opennova::def
