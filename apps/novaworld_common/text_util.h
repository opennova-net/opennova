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

} // namespace opennova::novaworld_server
