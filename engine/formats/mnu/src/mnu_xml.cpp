// Forgiving XML parser implementation matching NovaLogic's MNU parser.
#include "mnu_xml/mnu_xml.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include <io/strutil.h>

namespace mnu_xml {

using opennova::strutil::iequals;


// Node methods.
const Attribute *Node::find_attr(const std::string &name) const {
  for (const auto &attr : attributes) {
    if (iequals(attr.name, name)) {
      return &attr;
    }
  }
  return nullptr;
}

std::string Node::attr(const std::string &name) const {
  const Attribute *a = find_attr(name);
  return a ? a->value : std::string();
}

bool Node::has_attr(const std::string &name) const {
  return find_attr(name) != nullptr;
}

bool Node::attr_bool(const std::string &name) const {
  const Attribute *a = find_attr(name);
  if (!a) return false;
  // Bare attribute or non-false value = true.
  if (a->value.empty()) return true;
  return !iequals(a->value, "false") && a->value != "0";
}

int Node::attr_int(const std::string &name, int default_val) const {
  const Attribute *a = find_attr(name);
  if (!a || a->value.empty()) return default_val;
  try {
    return std::stoi(a->value);
  } catch (...) {
    return default_val;
  }
}

const Node *Node::find_child(const std::string &tag_name) const {
  for (const auto &child : children) {
    if (child->is_element() && iequals(child->tag, tag_name)) {
      return child.get();
    }
  }
  return nullptr;
}

Node *Node::find_child(const std::string &tag_name) {
  for (auto &child : children) {
    if (child->is_element() && iequals(child->tag, tag_name)) {
      return child.get();
    }
  }
  return nullptr;
}

std::vector<const Node *> Node::find_children(const std::string &tag_name) const {
  std::vector<const Node *> result;
  for (const auto &child : children) {
    if (child->is_element() && iequals(child->tag, tag_name)) {
      result.push_back(child.get());
    }
  }
  return result;
}

std::string Node::get_text() const {
  std::string result;
  if (!text.empty()) {
    result = text;
  }
  for (const auto &child : children) {
    std::string child_text = child->get_text();
    if (!child_text.empty()) {
      if (!result.empty() && !std::isspace(static_cast<unsigned char>(result.back()))) {
        result += ' ';
      }
      result += child_text;
    }
  }
  return result;
}

std::string Node::get_direct_text() const {
  std::string result;
  for (const auto &child : children) {
    if (child->is_text()) {
      result += child->text;
    }
  }
  return result;
}

// Document methods.
const Node *Document::find_root(const std::string &tag_name) const {
  for (const auto &root : roots) {
    if (root->is_element() && iequals(root->tag, tag_name)) {
      return root.get();
    }
  }
  return nullptr;
}

Node *Document::find_root(const std::string &tag_name) {
  for (auto &root : roots) {
    if (root->is_element() && iequals(root->tag, tag_name)) {
      return root.get();
    }
  }
  return nullptr;
}

const Node *Document::first_root() const {
  for (const auto &root : roots) {
    if (root->is_element()) {
      return root.get();
    }
  }
  return nullptr;
}

Node *Document::first_root() {
  for (auto &root : roots) {
    if (root->is_element()) {
      return root.get();
    }
  }
  return nullptr;
}

// Parser implementation.
namespace {

// Check if character is whitespace.
inline bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Check if character is a name character (tag/attribute names).
inline bool is_name_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
         c == ':' || c == '.';
}

// Skip whitespace, return new position.
const char *skip_space(const char *p, const char *end) {
  while (p < end && is_space(*p)) ++p;
  return p;
}

// Skip to next occurrence of char, return position (or end if not found).
const char *skip_to(const char *p, const char *end, char c) {
  while (p < end && *p != c) ++p;
  return p;
}

// Check if starts with string (case-insensitive).
bool starts_with_i(const char *p, const char *end, const char *prefix) {
  while (*prefix && p < end) {
    if (std::tolower(static_cast<unsigned char>(*p)) !=
        std::tolower(static_cast<unsigned char>(*prefix))) {
      return false;
    }
    ++p;
    ++prefix;
  }
  return *prefix == '\0';
}

// The engine's named-entity table, name -> byte value.
// [orig: off_85A628] The first 7 are case-INSENSITIVE (table flag 0); the
// Latin-1 accented set + laquo/raquo are case-SENSITIVE (flag 1) so Agrave and
// agrave stay distinct. Values are the Latin-1 codepoint truncated to a byte
// (the engine returns the low byte). Notably nbsp -> 0x20 (a plain space), and
// there is NO &apos; entry.
struct NamedEntity {
  const char *name;
  unsigned char value;
  bool case_sensitive;
};
const NamedEntity k_named_entities[] = {
    {"quot", 0x22, false}, {"amp", 0x26, false}, {"lt", 0x3c, false},
    {"gt", 0x3e, false}, {"copy", 0xa9, false}, {"reg", 0xae, false},
    {"nbsp", 0x20, false},
    {"Agrave", 0xc0, true}, {"Aacute", 0xc1, true}, {"Acirc", 0xc2, true},
    {"Atilde", 0xc3, true}, {"Auml", 0xc4, true}, {"Aring", 0xc5, true},
    {"AElig", 0xc6, true}, {"Ccedil", 0xc7, true}, {"Egrave", 0xc8, true},
    {"Eacute", 0xc9, true}, {"Ecirc", 0xca, true}, {"Euml", 0xcb, true},
    {"ETH", 0xd0, true}, {"Igrave", 0xcc, true}, {"Iacute", 0xcd, true},
    {"Icirc", 0xce, true}, {"Iuml", 0xcf, true}, {"Ntilde", 0xd1, true},
    {"Ograve", 0xd2, true}, {"Oacute", 0xd3, true}, {"Ocirc", 0xd4, true},
    {"Otilde", 0xd5, true}, {"Ouml", 0xd6, true}, {"Oslash", 0xd8, true},
    {"Ugrave", 0xd9, true}, {"Uacute", 0xda, true}, {"Ucirc", 0xdb, true},
    {"Uuml", 0xdc, true}, {"Yacute", 0xdd, true}, {"THORN", 0xde, true},
    {"szlig", 0xdf, true}, {"agrave", 0xe0, true}, {"aacute", 0xe1, true},
    {"acirc", 0xe2, true}, {"atilde", 0xe3, true}, {"auml", 0xe4, true},
    {"aring", 0xe5, true}, {"aelig", 0xe6, true}, {"ccedil", 0xe7, true},
    {"egrave", 0xe8, true}, {"eacute", 0xe9, true}, {"ecirc", 0xea, true},
    {"euml", 0xeb, true}, {"eth", 0xf0, true}, {"igrave", 0xec, true},
    {"iacute", 0xed, true}, {"icirc", 0xee, true}, {"iuml", 0xef, true},
    {"ntilde", 0xf1, true}, {"ograve", 0xf2, true}, {"oacute", 0xf3, true},
    {"ocirc", 0xf4, true}, {"otilde", 0xf5, true}, {"ouml", 0xf6, true},
    {"oslash", 0xf8, true}, {"ugrave", 0xf9, true}, {"uacute", 0xfa, true},
    {"ucirc", 0xfb, true}, {"uuml", 0xfc, true}, {"yacute", 0xfd, true},
    {"yuml", 0xff, true}, {"thorn", 0xfe, true}, {"raquo", 0xbb, true},
    {"laquo", 0xab, true},
};

// Decode an XML entity reference, advancing p past it.
// [orig: XML_ParseCharEntity @ 0x769cc0; named-entity table @ 0x85a628]
// Faithful policy: numeric entities are DECIMAL-only (&#xNN; is not recognized)
// and truncate to the low byte; the named set is the Latin-1 table above (NO
// &apos;); an UNKNOWN entity (no match, or no terminating ';') decodes to a bare
// '&' and the name text is left to re-parse verbatim (the engine emits '&' and
// advances one char), so an unrecognized "&foo;" survives round-trip instead of
// being lost. Returns '\0' (dropped by callers) only for a numeric NUL.
char decode_entity(const char *&p, const char *end) {
  if (p >= end || *p != '&') return '\0';

  const char *start = p;
  ++p;  // Skip '&'

  // The original scans to ';', a space, or the terminator.
  const char *semi = p;
  while (semi < end && *semi != ';' && !is_space(*semi)) {
    ++semi;
  }

  if (semi >= end || *semi != ';') {
    // Unknown form: emit '&' and re-parse the rest as literal text (advance
    // past the '&' only). This also prevents an infinite loop on a bare '&'.
    p = start + 1;
    return '&';
  }

  const std::string entity(p, semi);

  // Numeric entity: decimal only, low byte.
  if (!entity.empty() && entity[0] == '#') {
    // A leading 'x'/'X' is NOT hex here; std::strtol(base 10) stops at it and
    // yields 0, matching the engine's _wtol(&#x..) -> 0 (a dropped NUL).
    const long code = std::strtol(entity.c_str() + 1, nullptr, 10);
    p = semi + 1;
    return static_cast<char>(code & 0xFF);
  }

  for (const NamedEntity &e : k_named_entities) {
    const bool match = e.case_sensitive ? (entity == e.name)
                                        : iequals(entity, e.name);
    if (match) {
      p = semi + 1;
      return static_cast<char>(e.value);
    }
  }

  // Unrecognized named entity: keep the text verbatim (emit '&', re-parse name).
  p = start + 1;
  return '&';
}

// Parse text content until '<' or end, handling entities.
std::string parse_text(const char *&p, const char *end, const ParseOptions &opts) {
  std::string result;
  while (p < end && *p != '<') {
    if (*p == '&') {
      char decoded = decode_entity(p, end);
      if (decoded) {
        result += decoded;
      }
    } else {
      if (opts.normalize_whitespace && is_space(*p)) {
        // Collapse whitespace.
        if (result.empty() || !is_space(result.back())) {
          result += ' ';
        }
        ++p;
      } else {
        result += *p++;
      }
    }
  }

  if (opts.trim_text) {
    // Trim leading whitespace.
    size_t start = 0;
    while (start < result.size() && is_space(result[start])) ++start;
    // Trim trailing whitespace.
    size_t end_pos = result.size();
    while (end_pos > start && is_space(result[end_pos - 1])) --end_pos;
    result = result.substr(start, end_pos - start);
  }

  return result;
}

// Parse attribute value (quoted or unquoted).
std::string parse_attr_value(const char *&p, const char *end) {
  p = skip_space(p, end);
  if (p >= end) return "";

  std::string result;

  if (*p == '"' || *p == '\'') {
    // Quoted value.
    char quote = *p++;
    // Retail menus contain a known missing closing quote immediately before
    // the tag terminator (vjustify="CENTER>). Recover at '>' so the following
    // sibling elements are not swallowed into the attribute value.
    while (p < end && *p != quote && *p != '>') {
      if (*p == '&') {
        char decoded = decode_entity(p, end);
        if (decoded) result += decoded;
      } else {
        result += *p++;
      }
    }
    if (p < end && *p == quote) ++p;  // Skip a real closing quote.
  } else {
    // Unquoted value (game allows this).
    // Value ends at whitespace, '>', or '/'.
    while (p < end && !is_space(*p) && *p != '>' && *p != '/') {
      if (*p == '&') {
        char decoded = decode_entity(p, end);
        if (decoded) result += decoded;
      } else {
        result += *p++;
      }
    }
  }

  return result;
}

// Parse a single element and its children recursively.
// p should point just after the opening '<'.
// Returns nullptr on error.
std::unique_ptr<Node> parse_element(const char *&p, const char *end,
                                    Node *parent, std::string &error,
                                    const ParseOptions &opts);

// Main parse loop for content (text and child elements).
bool parse_content(const char *&p, const char *end, Node *current,
                   std::string &error, const ParseOptions &opts) {
  while (p < end) {
    p = skip_space(p, end);
    if (p >= end) break;

    if (*p == '<') {
      ++p;  // Skip '<'
      if (p >= end) {
        error = "Unexpected end after '<'";
        return false;
      }

      // Check for closing tag.
      if (*p == '/') {
        ++p;  // Skip '/'
        p = skip_space(p, end);

        // Read closing tag name.
        const char *name_start = p;
        while (p < end && is_name_char(*p)) ++p;
        std::string close_tag(name_start, p);

        // Skip to '>'.
        p = skip_space(p, end);
        if (p < end && *p == '>') ++p;

        // Verify tag matches (case-insensitive).
        if (!iequals(close_tag, current->tag)) {
          // Game is forgiving - just warn but continue.
          // error = "Mismatched close tag: expected </" + current->tag + "> got </" + close_tag + ">";
          // return false;
        }
        return true;  // Done with this element.
      }

      // Check for comment.
      if (p + 2 < end && *p == '!' && *(p + 1) == '-' && *(p + 2) == '-') {
        p += 3;  // Skip '!--'
        // Find '-->'
        while (p + 2 < end) {
          if (*p == '-' && *(p + 1) == '-' && *(p + 2) == '>') {
            p += 3;
            break;
          }
          ++p;
        }
        continue;
      }

      // Check for processing instruction or declaration (skip).
      if (*p == '?' || *p == '!') {
        // Skip to '>'.
        while (p < end && *p != '>') ++p;
        if (p < end) ++p;
        continue;
      }

      // Parse child element.
      auto child = parse_element(p, end, current, error, opts);
      if (!child) {
        return false;
      }
      current->children.push_back(std::move(child));

    } else {
      // Parse text content.
      std::string text = parse_text(p, end, opts);
      if (!text.empty()) {
        auto text_node = std::make_unique<Node>();
        text_node->text = std::move(text);
        text_node->parent = current;
        current->children.push_back(std::move(text_node));
      }
    }
  }

  return true;
}

std::unique_ptr<Node> parse_element(const char *&p, const char *end,
                                    Node *parent, std::string &error,
                                    const ParseOptions &opts) {
  p = skip_space(p, end);
  if (p >= end) {
    error = "Unexpected end in element";
    return nullptr;
  }

  // Read tag name.
  const char *name_start = p;
  while (p < end && is_name_char(*p)) ++p;

  if (p == name_start) {
    error = "Expected tag name";
    return nullptr;
  }

  auto node = std::make_unique<Node>();
  node->tag = std::string(name_start, p);
  node->parent = parent;

  // Parse attributes until '>' or '/>'.
  while (p < end) {
    p = skip_space(p, end);
    if (p >= end) break;

    if (*p == '>') {
      ++p;  // Skip '>'.
      // Parse content.
      if (!parse_content(p, end, node.get(), error, opts)) {
        return nullptr;
      }
      break;
    }

    if (*p == '/') {
      ++p;  // Skip '/'.
      p = skip_space(p, end);
      if (p < end && *p == '>') {
        ++p;  // Self-closing tag, no content.
      }
      break;
    }

    // Parse attribute name.
    const char *attr_name_start = p;
    while (p < end && is_name_char(*p)) ++p;

    if (p == attr_name_start) {
      // No attribute name - skip this character and continue.
      ++p;
      continue;
    }

    Attribute attr;
    attr.name = std::string(attr_name_start, p);

    p = skip_space(p, end);

    if (p < end && *p == '=') {
      ++p;  // Skip '='.
      attr.value = parse_attr_value(p, end);
    } else {
      // Bare boolean attribute (game supports this).
      attr.value = "true";
    }

    node->attributes.push_back(std::move(attr));
  }

  return node;
}

// Detect and skip BOM, return encoding type.
// [orig: XML_ParseWithBOMDetection @ 0x76a690]  The original detects UTF-16 LE/BE and
// UTF-8 BOMs and routes no-BOM/UTF-8 through MultiByteToWideChar (its parser is
// wchar_t throughout); this reimpl handles only the UTF-8 BOM and works in UTF-8/ASCII
// (matching for the all-ASCII shipped corpus). See notes/mnu/mnu.md.
const char *skip_bom(const char *data, size_t size) {
  if (size >= 3 && static_cast<uint8_t>(data[0]) == 0xEF &&
      static_cast<uint8_t>(data[1]) == 0xBB &&
      static_cast<uint8_t>(data[2]) == 0xBF) {
    return data + 3;  // UTF-8 BOM.
  }
  // Note: We don't handle UTF-16 BOM here - assume UTF-8 input.
  return data;
}

}  // namespace

bool parse(const std::string &content, Document &out, std::string &error,
           const ParseOptions &options) {
  return parse(reinterpret_cast<const uint8_t *>(content.data()),
               content.size(), out, error, options);
}

bool parse(const uint8_t *data, size_t size, Document &out, std::string &error,
           const ParseOptions &options) {
  out.roots.clear();

  if (!data || size == 0) {
    return true;  // Empty document is valid.
  }

  const char *p = skip_bom(reinterpret_cast<const char *>(data), size);
  const char *end = reinterpret_cast<const char *>(data) + size;

  // Parse root elements.
  while (p < end) {
    p = skip_space(p, end);
    if (p >= end) break;

    if (*p == '<') {
      ++p;  // Skip '<'.
      if (p >= end) break;

      // Skip comments and declarations at root level.
      if (*p == '!' || *p == '?') {
        if (p + 2 < end && *p == '!' && *(p + 1) == '-' && *(p + 2) == '-') {
          p += 3;
          while (p + 2 < end) {
            if (*p == '-' && *(p + 1) == '-' && *(p + 2) == '>') {
              p += 3;
              break;
            }
            ++p;
          }
        } else {
          while (p < end && *p != '>') ++p;
          if (p < end) ++p;
        }
        continue;
      }

      // Parse root element.
      auto root = parse_element(p, end, nullptr, error, options);
      if (!root) {
        return false;
      }
      out.roots.push_back(std::move(root));
    } else {
      // Skip unexpected content at root level.
      ++p;
    }
  }

  return true;
}

bool parse_file(const std::string &path, Document &out, std::string &error,
                const ParseOptions &options) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Failed to open file: " + path;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  return parse(content, out, error, options);
}

}  // namespace mnu_xml
