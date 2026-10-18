#pragma once

// The text helpers the service's config readers and validators share.

#include <base/io/strutil.h>

#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server {

// A comma-separated list, each entry trimmed, empty entries dropped.
inline std::vector<std::string> split_list(std::string_view list) {
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= list.size()) {
		const size_t comma = list.find(',', pos);
		const size_t end = comma == std::string_view::npos ? list.size() : comma;
		const std::string_view entry = strutil::trim_view(list.substr(pos, end - pos));
		if (!entry.empty()) out.emplace_back(entry);
		pos = end + 1;
	}
	return out;
}

// `text` begins with `head`, case and all.
inline bool starts_with(std::string_view text, std::string_view head) {
	return text.substr(0, head.size()) == head;
}

// An ASCII letter or digit.
inline bool is_alnum(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// Every character of `text` passes `ok` (an empty text passes).
inline bool all_of(std::string_view text, bool (*ok)(char)) {
	for (const char c : text)
		if (!ok(c))
			return false;
	return true;
}

// A docker object's or an EC2 deployment's name character: an ASCII letter or digit, '.',
// '_' or '-'.
inline bool is_object_name_char(char c) {
	return is_alnum(c) || c == '.' || c == '_' || c == '-';
}

} // namespace opennova::novaworld_server
