#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::def {

// Authoring findings contain locations and typed reasons, never source lines that
// could be replayed by a writer. Runtime callers may omit the report and retain
// the witnessed permissive parse behavior.
enum class DefIssueCode { UnknownProperty, InvalidValue, MalformedBlock, Unrepresentable };

struct DefIssue {
	DefIssueCode code = DefIssueCode::InvalidValue;
	size_t line = 0;
	std::string record;
	std::string field;
	std::string message;
};

using DefParseReport = std::vector<DefIssue>;

} // namespace opennova::def
