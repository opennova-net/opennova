#pragma once

#include <cstdint>
#include <string>

#include <base/io/json.h>

namespace opennova::editor {

// The reads a viewport's options take, a member at a time (ADR 0046 S13 V5: a SetViewport's `options`, `camera`,
// `clock`): each sets `out` from a value of its kind, else is false with `error` the caller's own refusal, so every
// option keeps its words on the wire.

// A boolean.
inline bool read_json_bool(const io::JsonValue &value, bool &out, std::string &error, const std::string &refusal) {
	if (!value.is_bool()) {
		error = refusal;
		return false;
	}
	out = value.boolean;
	return true;
}

// A whole number from `least` to `most` (io::json_whole_in).
template <typename Whole>
bool read_json_whole(const io::JsonValue &value, int64_t least, int64_t most, Whole &out, std::string &error,
		const std::string &refusal) {
	int64_t whole = 0;
	if (!io::json_whole_in(value, least, most, whole)) {
		error = refusal;
		return false;
	}
	out = Whole(whole);
	return true;
}

// A token its enum's `from_token` reads.
template <typename Enum>
bool read_json_token(const io::JsonValue &value, bool (*from_token)(const std::string &, Enum &), Enum &out,
		std::string &error, const std::string &refusal) {
	if (!value.is_string() || !from_token(value.string, out)) {
		error = refusal;
		return false;
	}
	return true;
}

} // namespace opennova::editor
