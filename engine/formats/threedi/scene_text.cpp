// The shared scene-text tokenizer, readers and printers (scene_text.h).

#include <formats/threedi/scene_text.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace opennova::threedi {

SceneLine::SceneLine(const std::string &text, SceneNumbers numbers) : numbers_(numbers) {
	const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; };
	size_t i = 0;
	while (i < text.size()) {
		if (space(text[i])) {
			++i;
			continue;
		}
		std::string token;
		if (text[i] == '"') {
			const size_t close = text.find('"', i + 1);
			if (close == std::string::npos) {
				bad = "a quoted name has no closing '\"'";
				return;
			}
			token = text.substr(i + 1, close - i - 1);
			i = close + 1;
			if (i < text.size() && !space(text[i])) {
				bad = "a quoted name runs into the next field";
				return;
			}
		} else {
			while (i < text.size() && !space(text[i])) {
				if (text[i] == '"') {
					bad = "a '\"' inside a bare field (quote the whole name; a name cannot hold '\"')";
					return;
				}
				token += text[i++];
			}
		}
		tokens.push_back(token);
	}
}

bool SceneLine::number(double &out) {
	if (!more()) return false;
	const std::string &t = tokens[next];
	if (numbers_ == SceneNumbers::any && (t == "nan" || t == "-nan")) {
		out = t[0] == '-' ? -std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::quiet_NaN();
		++next;
		return true;
	}
	char *end = nullptr;
	out = std::strtod(t.c_str(), &end);
	if (t.empty() || end == nullptr || *end != '\0') return false;
	if (numbers_ == SceneNumbers::finite && !std::isfinite(out)) return false;
	++next;
	return true;
}

bool SceneLine::numbers(double *out, int n) {
	for (int i = 0; i < n; ++i)
		if (!number(out[i])) return false;
	return true;
}

bool SceneLine::integer(long long &out) {
	if (!more()) return false;
	const std::string &t = tokens[next];
	const size_t sign = !t.empty() && (t[0] == '-' || t[0] == '+') ? 1 : 0;
	const bool hex = t.size() > sign + 2 && t[sign] == '0' && (t[sign + 1] == 'x' || t[sign + 1] == 'X');
	char *end = nullptr;
	errno = 0;
	const long long value = std::strtoll(t.c_str(), &end, hex ? 16 : 10);
	if (t.size() == sign || end == nullptr || *end != '\0' || errno == ERANGE) return false;
	out = value;
	++next;
	return true;
}

bool SceneLine::integer(long long &out, long long lo, long long hi) {
	return integer(out) && out >= lo && out <= hi;
}

bool SceneLine::name(std::string &out) {
	if (!more()) return false;
	out = tokens[next++];
	return true;
}

std::string strip_comment(const std::string &line) {
	bool quoted = false;
	for (size_t i = 0; i < line.size(); ++i) {
		if (line[i] == '"') quoted = !quoted;
		if (!quoted && line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t'))
			return line.substr(0, i);
	}
	return line;
}

std::string number_text(double v, const char *format) {
	if (std::isnan(v)) return std::signbit(v) ? "-nan" : "nan";
	if (std::isinf(v)) return v < 0.0 ? "-inf" : "inf";
	char buf[40];
	std::snprintf(buf, sizeof(buf), format, v);
	return std::strcmp(buf, "-0") == 0 ? std::string("0") : std::string(buf);
}

std::string f9(double v) { return number_text(v, "%.9g"); }
std::string f17(double v) { return number_text(v, "%.17g"); }

std::string name_field(const std::string &name, std::string &kept) {
	kept.clear();
	for (char c : name)
		if (c != '"' && c != '\n') kept += c;
	const bool plain = !kept.empty() && kept[0] != '#' &&
			std::none_of(kept.begin(), kept.end(), [](unsigned char c) { return std::isspace(c) != 0; });
	return plain ? kept : "\"" + kept + "\"";
}

} // namespace opennova::threedi
