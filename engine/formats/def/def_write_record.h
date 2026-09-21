#pragma once

#include "def_write.h"

namespace opennova::def {

// Internal writer context. It sees only typed records and reports an error when
// their values cannot be authored in the native grammar.
struct DefRecordWriter {
	DefWriteResult result;
	const DefItemsFile *items = nullptr;
	void fail(const std::string &record, const std::string &field, const std::string &message);
	void record(DefRecordKind kind, const void *value, const std::string &name);
	void line(const std::string &key, const std::vector<std::string> &values);
};

} // namespace opennova::def
