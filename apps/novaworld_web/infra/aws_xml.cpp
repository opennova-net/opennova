#include "aws_xml.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::novaworld_server::infra {

namespace {

bool is_space(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_name_char(char c) {
	return !is_space(c) && c != '>' && c != '/' && c != '=' && c != '<' && c != '"' && c != '\'';
}

std::string_view local_name(std::string_view qualified) {
	const size_t colon = qualified.rfind(':');
	return colon == std::string_view::npos ? qualified : qualified.substr(colon + 1);
}

void append_utf8(std::string &out, uint32_t cp) {
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

// `raw` character data with its references decoded onto `out`; false for a
// reference that is not one.
bool decode_text(std::string_view raw, std::string &out) {
	for (size_t i = 0; i < raw.size(); ++i) {
		if (raw[i] != '&') {
			out.push_back(raw[i]);
			continue;
		}
		const size_t end = raw.find(';', i);
		if (end == std::string_view::npos || end - i > 12)
			return false;
		const std::string_view ref = raw.substr(i + 1, end - i - 1);
		if (ref == "lt")
			out.push_back('<');
		else if (ref == "gt")
			out.push_back('>');
		else if (ref == "amp")
			out.push_back('&');
		else if (ref == "quot")
			out.push_back('"');
		else if (ref == "apos")
			out.push_back('\'');
		else if (ref.size() > 1 && ref[0] == '#') {
			const bool hex = ref[1] == 'x' || ref[1] == 'X';
			const std::string_view digits = ref.substr(hex ? 2 : 1);
			if (digits.empty())
				return false;
			uint32_t cp = 0;
			for (const char c : digits) {
				uint32_t d = 0;
				if (c >= '0' && c <= '9')
					d = static_cast<uint32_t>(c - '0');
				else if (hex && c >= 'a' && c <= 'f')
					d = static_cast<uint32_t>(c - 'a' + 10);
				else if (hex && c >= 'A' && c <= 'F')
					d = static_cast<uint32_t>(c - 'A' + 10);
				else
					return false;
				cp = cp * (hex ? 16 : 10) + d;
				if (cp > 0x10FFFF)
					return false;
			}
			if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF))
				return false;
			append_utf8(out, cp);
		} else {
			return false;
		}
		i = end;
	}
	return true;
}

} // namespace

const XmlNode *XmlNode::child(std::string_view child_name) const {
	for (const XmlNode &c : children)
		if (c.name == child_name)
			return &c;
	return nullptr;
}

std::string XmlNode::child_text(std::string_view child_name) const {
	const XmlNode *c = child(child_name);
	return c != nullptr ? std::string(strutil::trim_view(c->text)) : std::string();
}

const XmlNode *XmlNode::at(std::initializer_list<std::string_view> path) const {
	const XmlNode *node = this;
	for (const std::string_view step : path) {
		node = node->child(step);
		if (node == nullptr)
			return nullptr;
	}
	return node;
}

const XmlNode *XmlNode::find(std::string_view element_name) const {
	if (name == element_name)
		return this;
	for (const XmlNode &c : children)
		if (const XmlNode *found = c.find(element_name))
			return found;
	return nullptr;
}

bool parse_xml(std::string_view text, XmlNode &root, std::string &error) {
	root = XmlNode();
	// The open elements, outermost first, and the names their start tags spelled.
	std::vector<XmlNode *> open;
	std::vector<std::string_view> open_names;
	bool root_started = false;
	bool root_done = false;
	size_t pos = 0;
	if (text.substr(0, 3) == "\xEF\xBB\xBF")
		pos = 3;

	const auto skip_past = [&](std::string_view marker, const char *what) {
		const size_t end = text.find(marker, pos);
		if (end == std::string_view::npos) {
			error = std::string("an unterminated ") + what;
			return false;
		}
		pos = end + marker.size();
		return true;
	};

	while (pos < text.size()) {
		if (text[pos] != '<') {
			const size_t end = std::min(text.find('<', pos), text.size());
			const std::string_view raw = text.substr(pos, end - pos);
			if (open.empty()) {
				if (!strutil::trim_view(raw).empty()) {
					error = "text outside the root element";
					return false;
				}
			} else if (!decode_text(raw, open.back()->text)) {
				error = "a reference that is not one";
				return false;
			}
			pos = end;
			continue;
		}
		const std::string_view rest = text.substr(pos);
		if (rest.substr(0, 2) == "<?") {
			if (!skip_past("?>", "processing instruction"))
				return false;
		} else if (rest.substr(0, 4) == "<!--") {
			if (!skip_past("-->", "comment"))
				return false;
		} else if (rest.substr(0, 9) == "<![CDATA[") {
			const size_t start = pos + 9;
			const size_t end = text.find("]]>", start);
			if (end == std::string_view::npos || open.empty()) {
				error = "a CDATA section outside an element, or unterminated";
				return false;
			}
			open.back()->text.append(text.substr(start, end - start));
			pos = end + 3;
		} else if (rest.substr(0, 2) == "<!") {
			if (root_started) {
				error = "a declaration inside the root element";
				return false;
			}
			if (!skip_past(">", "declaration"))
				return false;
		} else if (rest.substr(0, 2) == "</") {
			const size_t end = text.find('>', pos);
			if (end == std::string_view::npos) {
				error = "an unterminated end tag";
				return false;
			}
			const std::string_view name = strutil::trim_view(text.substr(pos + 2, end - pos - 2));
			if (open.empty() || name != open_names.back()) {
				error = "an end tag that matches no open element";
				return false;
			}
			open.pop_back();
			open_names.pop_back();
			if (open.empty())
				root_done = true;
			pos = end + 1;
		} else {
			size_t i = pos + 1;
			while (i < text.size() && is_name_char(text[i]))
				++i;
			const std::string_view name = text.substr(pos + 1, i - pos - 1);
			if (name.empty()) {
				error = "a start tag with no name";
				return false;
			}
			// Read past the attributes to the tag's end.
			bool self_closing = false;
			for (;;) {
				while (i < text.size() && is_space(text[i]))
					++i;
				if (i >= text.size()) {
					error = "an unterminated start tag";
					return false;
				}
				if (text[i] == '>') {
					++i;
					break;
				}
				if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '>') {
					self_closing = true;
					i += 2;
					break;
				}
				const size_t name_start = i;
				while (i < text.size() && is_name_char(text[i]))
					++i;
				while (i < text.size() && is_space(text[i]))
					++i;
				if (i == name_start || i >= text.size() || text[i] != '=') {
					error = "a malformed attribute";
					return false;
				}
				++i;
				while (i < text.size() && is_space(text[i]))
					++i;
				if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) {
					error = "an attribute value with no quotes";
					return false;
				}
				const size_t close = text.find(text[i], i + 1);
				if (close == std::string_view::npos) {
					error = "an unterminated attribute value";
					return false;
				}
				i = close + 1;
			}
			if (root_done || (open.empty() && root_started)) {
				error = "an element past the root element";
				return false;
			}
			if (open.size() >= kMaxXmlDepth) {
				error = "elements nested too deep";
				return false;
			}
			XmlNode *node = nullptr;
			if (open.empty()) {
				root_started = true;
				node = &root;
			} else {
				open.back()->children.emplace_back();
				node = &open.back()->children.back();
			}
			node->name = std::string(local_name(name));
			if (self_closing) {
				if (open.empty())
					root_done = true;
			} else {
				open.push_back(node);
				open_names.push_back(name);
			}
			pos = i;
		}
	}
	if (!open.empty()) {
		error = "an element left open";
		return false;
	}
	if (!root_started) {
		error = "no root element";
		return false;
	}
	return true;
}

} // namespace opennova::novaworld_server::infra
