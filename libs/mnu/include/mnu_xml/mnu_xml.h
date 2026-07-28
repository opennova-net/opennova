// Forgiving XML parser matching NovaLogic's MNU parser behavior.
// This is NOT a standards-compliant XML parser. It matches the game's parser:
// - Unquoted attribute values: type=button
// - Bare boolean attributes: HIDDEN (treated as HIDDEN="true")
// - Case-insensitive tag/attribute matching
// - Lenient whitespace handling
// - HTML-style comments: <!-- ... -->
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mnu_xml {

// XML attribute (name/value pair).
struct Attribute {
  std::string name;
  std::string value;  // Empty for bare boolean attributes
};

// XML node (element or text).
struct Node {
  std::string tag;                        // Tag name (empty for text nodes)
  std::string text;                       // Text content
  std::vector<Attribute> attributes;      // Attributes on this element
  std::vector<std::unique_ptr<Node>> children;  // Child nodes
  Node *parent = nullptr;                 // Parent node (not owned)

  // Check if this is a text node (no tag).
  bool is_text() const { return tag.empty() && !text.empty(); }

  // Check if this is an element node.
  bool is_element() const { return !tag.empty(); }

  // Find attribute by name (case-insensitive). Returns nullptr if not found.
  const Attribute *find_attr(const std::string &name) const;

  // Get attribute value by name (case-insensitive). Returns empty if not found.
  std::string attr(const std::string &name) const;

  // Check if attribute exists (case-insensitive).
  bool has_attr(const std::string &name) const;

  // Get attribute as bool (exists and not "false"/"0").
  bool attr_bool(const std::string &name) const;

  // Get attribute as int. Returns default_val if not found or invalid.
  int attr_int(const std::string &name, int default_val = 0) const;

  // Find first child element by tag name (case-insensitive).
  const Node *find_child(const std::string &tag_name) const;
  Node *find_child(const std::string &tag_name);

  // Find all child elements by tag name (case-insensitive).
  std::vector<const Node *> find_children(const std::string &tag_name) const;

  // Get combined text content of this node and all descendants.
  std::string get_text() const;

  // Get direct text content (only immediate text children).
  std::string get_direct_text() const;
};

// Parsed XML document.
struct Document {
  std::vector<std::unique_ptr<Node>> roots;  // Root elements (usually just one)

  // Find first root element by tag name (case-insensitive).
  const Node *find_root(const std::string &tag_name) const;
  Node *find_root(const std::string &tag_name);

  // Get first root element (regardless of tag).
  const Node *first_root() const;
  Node *first_root();
};

// Parse options.
struct ParseOptions {
  bool normalize_whitespace = true;  // Collapse whitespace in text content
  bool trim_text = true;             // Trim leading/trailing whitespace from text
};

// Parse XML content from a string.
// Returns true on success, false on error with description in `error`.
bool parse(const std::string &content, Document &out, std::string &error,
           const ParseOptions &options = {});

// Parse XML content from a byte buffer.
bool parse(const uint8_t *data, size_t size, Document &out, std::string &error,
           const ParseOptions &options = {});

// Parse XML file from disk.
bool parse_file(const std::string &path, Document &out, std::string &error,
                const ParseOptions &options = {});

}  // namespace mnu_xml
