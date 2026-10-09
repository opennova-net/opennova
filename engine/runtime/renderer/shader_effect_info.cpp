#include <runtime/renderer/shader_effect_info.h>

#include <cstring>

#include <runtime/renderer/material_descriptor.h>

namespace opennova::renderer {

const std::vector<std::string> &fixed_function_shader_tags() {
	// The registry table's FF_ rows, in its order (the renderer's compile order), without their #UV twins
	// [orig: HLSLEffect_InitFixedFunctionShaders @ 0x5AFA54..0x5AFCFC, sprintf "FF%s%s%s" @ 0x5AFAD6].
	static const std::vector<std::string> tags = [] {
		std::vector<std::string> out;
		for (const MaterialDescriptorRecord &row : kMaterialDescriptorTable)
			if (row.family == MaterialDescriptorFamily::FixedFunction && std::strncmp(row.name, "FF_", 3) == 0 &&
			    std::strchr(row.name, '#') == nullptr)
				out.push_back(row.name);
		return out;
	}();
	return tags;
}

bool shader_file_is_include(const std::string &name) {
	// [orig: HLSLEffect_LoadAllFromPFFArchive @ 0x5AFF6E]
	return !name.empty() && name[0] == '_';
}

namespace {

// The text with its comments blanked to spaces (a line comment to its end, a block comment whole, its line
// ends kept), so an offset into it is one into the text; a string's contents kept.
std::string without_comments(const std::string &text) {
	std::string out = text;
	for (size_t i = 0; i < out.size();) {
		if (out[i] == '"') {
			for (++i; i < out.size() && out[i] != '"' && out[i] != '\n'; ++i)
				if (out[i] == '\\') ++i;
			++i;
		} else if (out.compare(i, 2, "//") == 0) {
			for (; i < out.size() && out[i] != '\n'; ++i) out[i] = ' ';
		} else if (out.compare(i, 2, "/*") == 0) {
			const size_t end = out.find("*/", i + 2);
			const size_t stop = end == std::string::npos ? out.size() : end + 2;
			for (; i < stop; ++i)
				if (out[i] != '\n') out[i] = ' ';
		} else {
			++i;
		}
	}
	return out;
}

bool word_char(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// The first `word` of `text` in [from, to) standing alone, npos for none.
size_t find_word(const std::string &text, const char *word, size_t from, size_t to) {
	const size_t length = std::char_traits<char>::length(word);
	for (size_t at = text.find(word, from); at != std::string::npos && at + length <= to; at = text.find(word, at + 1))
		if ((at == 0 || !word_char(text[at - 1])) && (at + length >= text.size() || !word_char(text[at + length])))
			return at;
	return std::string::npos;
}

size_t skip_space(const std::string &text, size_t at, size_t to) {
	while (at < to && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at;
	return at;
}

} // namespace

ShaderEffectInfo read_shader_effect_info(const std::string &text) {
	// The annotations the loader reads [orig: HLSLEffect_LoadFromFile @ 0x5AE899..0x5AE9BC; EffectAlt_UV @ 0x5AEA03].
	ShaderEffectInfo info;
	const std::string code = without_comments(text);
	const size_t name = find_word(code, "EffectInfo", 0, code.size());
	if (name == std::string::npos) return info;
	// Its annotations, between the '<' after the name and the '>' closing them outside a string.
	const size_t open = code.find('<', name);
	if (open == std::string::npos) return info;
	size_t close = open + 1;
	for (bool quoted = false; close < code.size() && (quoted || code[close] != '>'); ++close)
		if (code[close] == '"') quoted = !quoted;
	info.found = true;
	info.info_offset = name;
	// `<name> = <value>` after the annotation's own name.
	const auto value_at = [&](const char *annotation) -> size_t {
		const size_t at = find_word(code, annotation, open, close);
		if (at == std::string::npos) return at;
		const size_t eq = skip_space(code, at + std::char_traits<char>::length(annotation), close);
		return eq < close && code[eq] == '=' ? skip_space(code, eq + 1, close) : std::string::npos;
	};
	const size_t tag = value_at("EffectTag");
	if (tag != std::string::npos && code[tag] == '"') {
		const size_t end = code.find('"', tag + 1);
		if (end != std::string::npos && end < close) {
			info.tag = text.substr(tag + 1, end - tag - 1);
			info.tag_offset = tag + 1;
			info.tag_length = end - tag - 1;
		}
	}
	const size_t uv = value_at("EffectAlt_UV");
	if (uv != std::string::npos)
		info.alt_uv = code.compare(uv, 4, "true") == 0 || (code[uv] >= '1' && code[uv] <= '9');
	return info;
}

} // namespace opennova::renderer
