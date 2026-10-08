#pragma once
// A strict, dependency-free JSON reader and writer for the editor's project file,
// import sidecars and build records (ADR 0046 d6). Portable on purpose: apps, ctests
// and headless embedders read a project without Godot.
//
// Reading is strict (RFC 8259): UTF-8 text, no comments, no trailing commas, no
// NaN/Infinity, the seven escapes plus \uXXXX (surrogate pairs decode to UTF-8), no
// raw control characters inside strings, nesting capped at kJsonMaxDepth. Writing is
// deterministic so a saved file diffs cleanly: object keys sorted, two-space indent,
// "\n" line ends, integral numbers without a fraction, other numbers in the shortest
// form that round-trips, a trailing newline.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {
namespace io {

inline constexpr int kJsonMaxDepth = 256;

struct JsonMember;

struct JsonValue {
	enum class Type { Null, Bool, Number, String, Array, Object };

	Type type = Type::Null;
	bool boolean = false;
	double number = 0.0;
	std::string string;
	std::vector<JsonValue> array;
	// Insertion order is kept for lookups; the writer emits members sorted by key.
	std::vector<JsonMember> object;

	static JsonValue make_null() { return JsonValue(); }
	static JsonValue make_bool(bool value) {
		JsonValue v;
		v.type = Type::Bool;
		v.boolean = value;
		return v;
	}
	static JsonValue make_number(double value) {
		JsonValue v;
		v.type = Type::Number;
		v.number = value;
		return v;
	}
	static JsonValue make_string(std::string_view value) {
		JsonValue v;
		v.type = Type::String;
		v.string.assign(value.data(), value.size());
		return v;
	}
	static JsonValue make_array() {
		JsonValue v;
		v.type = Type::Array;
		return v;
	}
	static JsonValue make_object() {
		JsonValue v;
		v.type = Type::Object;
		return v;
	}

	bool is_null() const { return type == Type::Null; }
	bool is_bool() const { return type == Type::Bool; }
	bool is_number() const { return type == Type::Number; }
	bool is_string() const { return type == Type::String; }
	bool is_array() const { return type == Type::Array; }
	bool is_object() const { return type == Type::Object; }

	// Object member by exact key; nullptr when absent or when this is not an object.
	const JsonValue *get(std::string_view key) const;
	JsonValue *get(std::string_view key);
	// Replace the member `key` or append it; returns the stored value. Turns a null
	// value into an object so a fresh JsonValue can be filled member by member.
	JsonValue &set(std::string_view key, JsonValue value);
	// Append to an array (a null value becomes an array).
	JsonValue &push(JsonValue value);

	// Typed member reads with a fallback for a missing or wrongly typed member.
	bool get_bool(std::string_view key, bool fallback) const;
	double get_number(std::string_view key, double fallback) const;
	int get_int(std::string_view key, int fallback) const;
	std::string get_string(std::string_view key, std::string_view fallback) const;
};

struct JsonMember {
	std::string key;
	JsonValue value;
};

inline const JsonValue *JsonValue::get(std::string_view key) const {
	if (type != Type::Object) return nullptr;
	for (const JsonMember &m : object) {
		if (m.key == key) return &m.value;
	}
	return nullptr;
}

inline JsonValue *JsonValue::get(std::string_view key) {
	if (type != Type::Object) return nullptr;
	for (JsonMember &m : object) {
		if (m.key == key) return &m.value;
	}
	return nullptr;
}

inline JsonValue &JsonValue::set(std::string_view key, JsonValue value) {
	if (type == Type::Null) type = Type::Object;
	for (JsonMember &m : object) {
		if (m.key == key) {
			m.value = std::move(value);
			return m.value;
		}
	}
	JsonMember member;
	member.key.assign(key.data(), key.size());
	member.value = std::move(value);
	object.push_back(std::move(member));
	return object.back().value;
}

inline JsonValue &JsonValue::push(JsonValue value) {
	if (type == Type::Null) type = Type::Array;
	array.push_back(std::move(value));
	return array.back();
}

inline bool JsonValue::get_bool(std::string_view key, bool fallback) const {
	const JsonValue *v = get(key);
	return (v && v->is_bool()) ? v->boolean : fallback;
}

inline double JsonValue::get_number(std::string_view key, double fallback) const {
	const JsonValue *v = get(key);
	return (v && v->is_number()) ? v->number : fallback;
}

inline int JsonValue::get_int(std::string_view key, int fallback) const {
	const JsonValue *v = get(key);
	if (!v || !v->is_number()) return fallback;
	if (v->number != v->number || v->number < -2147483648.0 || v->number > 2147483647.0)
		return fallback;
	const int as_int = static_cast<int>(v->number);
	return (static_cast<double>(as_int) == v->number) ? as_int : fallback;
}

inline std::string JsonValue::get_string(std::string_view key, std::string_view fallback) const {
	const JsonValue *v = get(key);
	return (v && v->is_string()) ? v->string : std::string(fallback);
}

// A number or a string as a value: the shorthands a writer builds its members with.
inline JsonValue json_number(double value) { return JsonValue::make_number(value); }
inline JsonValue json_string(std::string_view value) { return JsonValue::make_string(value); }

// A number read as a whole number in [lo, hi], its fraction dropped; false for anything else
// (not a number, or a whole number outside the range).
inline bool json_whole_in(const JsonValue &json, double lo, double hi, int64_t &out) {
	if (!json.is_number()) return false;
	const double whole = std::trunc(json.number);
	if (!(whole >= lo && whole <= hi)) return false;
	out = static_cast<int64_t>(whole);
	return true;
}

// A number a float holds (finite, and no larger than a float's largest), read as one; false for
// anything else, so no double past what a float holds is ever converted to one.
inline bool json_float(const JsonValue &json, float &out) {
	if (!json.is_number() || !(std::fabs(json.number) <= double(std::numeric_limits<float>::max()))) return false;
	out = static_cast<float>(json.number);
	return true;
}

// ---------------------------------------------------------------------------
// Reader

namespace json_detail {

class Parser {
public:
	Parser(std::string_view text, std::string &error) : text_(text), error_(error) {}

	bool parse_document(JsonValue &out) {
		skip_ws();
		if (!parse_value(out, 0)) return false;
		skip_ws();
		if (pos_ != text_.size()) return fail("trailing characters after the document");
		return true;
	}

private:
	std::string_view text_;
	std::string &error_;
	size_t pos_ = 0;
	size_t line_ = 1;
	size_t line_start_ = 0;

	bool fail(const char *message) {
		char buf[256];
		std::snprintf(buf, sizeof(buf), "line %zu, column %zu: %s", line_, pos_ - line_start_ + 1,
		              message);
		error_ = buf;
		return false;
	}

	bool at_end() const { return pos_ >= text_.size(); }
	char peek() const { return at_end() ? '\0' : text_[pos_]; }

	void advance() {
		if (at_end()) return;
		if (text_[pos_] == '\n') {
			++line_;
			line_start_ = pos_ + 1;
		}
		++pos_;
	}

	void skip_ws() {
		while (!at_end()) {
			const char c = text_[pos_];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r') advance();
			else break;
		}
	}

	bool consume_literal(const char *word) {
		const size_t n = std::strlen(word);
		if (text_.size() - pos_ < n || text_.compare(pos_, n, word) != 0) return false;
		for (size_t i = 0; i < n; ++i) advance();
		return true;
	}

	bool parse_value(JsonValue &out, int depth) {
		if (depth > kJsonMaxDepth) return fail("nesting deeper than kJsonMaxDepth");
		if (at_end()) return fail("unexpected end of input");
		const char c = peek();
		switch (c) {
		case '{': return parse_object(out, depth);
		case '[': return parse_array(out, depth);
		case '"': {
			out = JsonValue::make_string("");
			return parse_string(out.string);
		}
		case 't':
			if (!consume_literal("true")) return fail("invalid literal");
			out = JsonValue::make_bool(true);
			return true;
		case 'f':
			if (!consume_literal("false")) return fail("invalid literal");
			out = JsonValue::make_bool(false);
			return true;
		case 'n':
			if (!consume_literal("null")) return fail("invalid literal");
			out = JsonValue::make_null();
			return true;
		default:
			if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
			return fail("unexpected character");
		}
	}

	bool parse_object(JsonValue &out, int depth) {
		out = JsonValue::make_object();
		advance(); // '{'
		skip_ws();
		if (peek() == '}') {
			advance();
			return true;
		}
		for (;;) {
			skip_ws();
			if (peek() != '"') return fail("expected a string key");
			JsonMember member;
			if (!parse_string(member.key)) return false;
			skip_ws();
			if (peek() != ':') return fail("expected ':' after the key");
			advance();
			skip_ws();
			if (!parse_value(member.value, depth + 1)) return false;
			for (const JsonMember &existing : out.object) {
				if (existing.key == member.key) return fail("duplicate object key");
			}
			out.object.push_back(std::move(member));
			skip_ws();
			if (peek() == ',') {
				advance();
				continue;
			}
			if (peek() == '}') {
				advance();
				return true;
			}
			return fail("expected ',' or '}' in an object");
		}
	}

	bool parse_array(JsonValue &out, int depth) {
		out = JsonValue::make_array();
		advance(); // '['
		skip_ws();
		if (peek() == ']') {
			advance();
			return true;
		}
		for (;;) {
			skip_ws();
			JsonValue element;
			if (!parse_value(element, depth + 1)) return false;
			out.array.push_back(std::move(element));
			skip_ws();
			if (peek() == ',') {
				advance();
				continue;
			}
			if (peek() == ']') {
				advance();
				return true;
			}
			return fail("expected ',' or ']' in an array");
		}
	}

	static void append_utf8(std::string &out, uint32_t cp) {
		if (cp < 0x80) {
			out.push_back(static_cast<char>(cp));
		} else if (cp < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}

	bool parse_hex4(uint32_t &value) {
		value = 0;
		for (int i = 0; i < 4; ++i) {
			if (at_end()) return fail("truncated \\u escape");
			const char c = peek();
			uint32_t nibble;
			if (c >= '0' && c <= '9') nibble = static_cast<uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f') nibble = static_cast<uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') nibble = static_cast<uint32_t>(c - 'A' + 10);
			else return fail("invalid hex digit in \\u escape");
			value = (value << 4) | nibble;
			advance();
		}
		return true;
	}

	// Validates one UTF-8 sequence starting at pos_ and copies it; false on a bad byte.
	bool copy_utf8_sequence(std::string &out) {
		const unsigned char lead = static_cast<unsigned char>(peek());
		int extra;
		if (lead < 0x80) extra = 0;
		else if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) extra = 1;
		else if ((lead & 0xF0) == 0xE0) extra = 2;
		else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4) extra = 3;
		else return fail("invalid UTF-8 lead byte");
		if (text_.size() - pos_ < static_cast<size_t>(extra) + 1) return fail("truncated UTF-8");
		for (int i = 1; i <= extra; ++i) {
			const unsigned char cont = static_cast<unsigned char>(text_[pos_ + i]);
			if ((cont & 0xC0) != 0x80) return fail("invalid UTF-8 continuation byte");
		}
		out.append(text_.data() + pos_, static_cast<size_t>(extra) + 1);
		for (int i = 0; i <= extra; ++i) advance();
		return true;
	}

	bool parse_string(std::string &out) {
		advance(); // opening quote
		out.clear();
		for (;;) {
			if (at_end()) return fail("unterminated string");
			const char c = peek();
			if (c == '"') {
				advance();
				return true;
			}
			if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
			if (c != '\\') {
				if (!copy_utf8_sequence(out)) return false;
				continue;
			}
			advance(); // backslash
			if (at_end()) return fail("unterminated escape");
			const char e = peek();
			advance();
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				uint32_t cp;
				if (!parse_hex4(cp)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					if (!consume_literal("\\u")) return fail("high surrogate without a low surrogate");
					uint32_t low;
					if (!parse_hex4(low)) return false;
					if (low < 0xDC00 || low > 0xDFFF) return fail("invalid low surrogate");
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					return fail("lone low surrogate");
				}
				append_utf8(out, cp);
				break;
			}
			default: return fail("invalid escape");
			}
		}
	}

	bool parse_number(JsonValue &out) {
		const size_t start = pos_;
		if (peek() == '-') advance();
		if (peek() == '0') {
			advance();
		} else if (peek() >= '1' && peek() <= '9') {
			while (peek() >= '0' && peek() <= '9') advance();
		} else {
			return fail("invalid number");
		}
		if (peek() == '.') {
			advance();
			if (!(peek() >= '0' && peek() <= '9')) return fail("invalid number fraction");
			while (peek() >= '0' && peek() <= '9') advance();
		}
		if (peek() == 'e' || peek() == 'E') {
			advance();
			if (peek() == '+' || peek() == '-') advance();
			if (!(peek() >= '0' && peek() <= '9')) return fail("invalid number exponent");
			while (peek() >= '0' && peek() <= '9') advance();
		}
		const std::string token(text_.substr(start, pos_ - start));
		char *end = nullptr;
		const double value = std::strtod(token.c_str(), &end);
		if (end == nullptr || *end != '\0') return fail("invalid number");
		out = JsonValue::make_number(value);
		return true;
	}
};

inline void write_escaped(const std::string &s, std::string &out) {
	out.push_back('"');
	for (const char c : s) {
		const unsigned char u = static_cast<unsigned char>(c);
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (u < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", u);
				out += buf;
			} else {
				out.push_back(c);
			}
		}
	}
	out.push_back('"');
}

inline void write_number(double value, std::string &out) {
	char buf[40];
	if (value != value || value > 1.7976931348623157e308 || value < -1.7976931348623157e308) {
		out += "null"; // NaN and infinities have no JSON spelling
		return;
	}
	const double whole = value < 0 ? -value : value;
	if (whole < 9007199254740992.0 && whole == static_cast<double>(static_cast<int64_t>(whole))) {
		std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
		out += buf;
		return;
	}
	// The shortest of %.15g / %.17g that reads back to the same double.
	std::snprintf(buf, sizeof(buf), "%.15g", value);
	if (std::strtod(buf, nullptr) != value) std::snprintf(buf, sizeof(buf), "%.17g", value);
	for (char *p = buf; *p; ++p) {
		if (*p == ',') *p = '.'; // never depend on the process locale
	}
	out += buf;
}

inline void write_value(const JsonValue &v, int indent, std::string &out) {
	switch (v.type) {
	case JsonValue::Type::Null: out += "null"; break;
	case JsonValue::Type::Bool: out += v.boolean ? "true" : "false"; break;
	case JsonValue::Type::Number: write_number(v.number, out); break;
	case JsonValue::Type::String: write_escaped(v.string, out); break;
	case JsonValue::Type::Array: {
		if (v.array.empty()) {
			out += "[]";
			break;
		}
		out += "[\n";
		for (size_t i = 0; i < v.array.size(); ++i) {
			out.append(static_cast<size_t>(indent + 1) * 2, ' ');
			write_value(v.array[i], indent + 1, out);
			out += (i + 1 < v.array.size()) ? ",\n" : "\n";
		}
		out.append(static_cast<size_t>(indent) * 2, ' ');
		out += "]";
		break;
	}
	case JsonValue::Type::Object: {
		if (v.object.empty()) {
			out += "{}";
			break;
		}
		std::vector<const JsonMember *> members;
		members.reserve(v.object.size());
		for (const JsonMember &m : v.object) members.push_back(&m);
		std::sort(members.begin(), members.end(),
		          [](const JsonMember *a, const JsonMember *b) { return a->key < b->key; });
		out += "{\n";
		for (size_t i = 0; i < members.size(); ++i) {
			out.append(static_cast<size_t>(indent + 1) * 2, ' ');
			write_escaped(members[i]->key, out);
			out += ": ";
			write_value(members[i]->value, indent + 1, out);
			out += (i + 1 < members.size()) ? ",\n" : "\n";
		}
		out.append(static_cast<size_t>(indent) * 2, ' ');
		out += "}";
		break;
	}
	}
}

} // namespace json_detail

// Parse `text` into `out`. False with `error` = "line L, column C: reason" on any
// deviation from strict JSON; `out` is unspecified after a failure.
inline bool json_parse(std::string_view text, JsonValue &out, std::string &error) {
	// A UTF-8 byte order mark is tolerated (editors add it); nothing else precedes the value.
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
	    static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
		text.remove_prefix(3);
	}
	error.clear();
	json_detail::Parser parser(text, error);
	return parser.parse_document(out);
}

// The deterministic text form of `value`, ending in one "\n".
inline std::string json_write(const JsonValue &value) {
	std::string out;
	json_detail::write_value(value, 0, out);
	out += "\n";
	return out;
}

} // namespace io
} // namespace opennova
