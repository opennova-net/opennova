#include "source_state.h"

namespace opennova::editor {

size_t first_lone_lf(std::string_view text, size_t *count) {
	size_t first = std::string_view::npos, found = 0;
	for (size_t i = 0; i < text.size(); ++i)
		if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r') && !found++) first = i;
	if (count) *count = found;
	return first;
}

std::string with_crlf_line_ends(std::string_view text) {
	std::string out;
	out.reserve(text.size() + text.size() / 16 + 1);
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) out += '\r';
		out += text[i];
	}
	return out;
}

} // namespace opennova::editor
