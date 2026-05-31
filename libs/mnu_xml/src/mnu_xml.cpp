// Forgiving XML parser implementation matching NovaLogic's MNU parser.
#include "mnu_xml/mnu_xml.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mnu_xml {

// Case-insensitive string comparison.
bool iequals(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string to_upper(const std::string &s) {
  std::string result = s;
  for (char &c : result) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return result;
}

std::string to_lower(const std::string &s) {
  std::string result = s;
  for (char &c : result) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

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

// Decode XML entity reference. Returns decoded char and advances p past the entity.
// If not a valid entity, returns '&' and doesn't advance.
char decode_entity(const char *&p, const char *end) {
  if (p >= end || *p != '&') return '\0';

  const char *start = p;
  ++p;  // Skip '&'

  // Find ';'
  const char *semi = p;
  while (semi < end && *semi != ';' && *semi != '<' && *semi != '>' && !is_space(*semi)) {
    ++semi;
  }

  if (semi >= end || *semi != ';') {
    p = start;  // Not a valid entity, restore position
    return '&';
  }

  std::string entity(p, semi);
  p = semi + 1;  // Skip past ';'

  // Common entities.
  if (entity == "amp") return '&';
  if (entity == "lt") return '<';
  if (entity == "gt") return '>';
  if (entity == "quot") return '"';
  if (entity == "apos") return '\'';
  if (entity == "nbsp") return ' ';

  // Numeric entity.
  if (!entity.empty() && entity[0] == '#') {
    int code = 0;
    if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
      // Hex.
      code = std::strtol(entity.c_str() + 2, nullptr, 16);
    } else {
      // Decimal.
      code = std::strtol(entity.c_str() + 1, nullptr, 10);
    }
    if (code > 0 && code < 128) {
      return static_cast<char>(code);
    }
    // For non-ASCII, just return '?' for now.
    return '?';
  }

  // Unknown entity - return '?'.
  return '?';
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
    while (p < end && *p != quote) {
      if (*p == '&') {
        char decoded = decode_entity(p, end);
        if (decoded) result += decoded;
      } else {
        result += *p++;
      }
    }
    if (p < end) ++p;  // Skip closing quote.
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
