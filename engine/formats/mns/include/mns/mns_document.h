#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mns/mns.h"

namespace mns {

// Lossless document model for .mns menu stylesheets. The format specification
// is NovaLogic's own comment header in the shipped menu_style.mns ("The style
// sheet basically relies on macro name and value pairs..."); the loader is
// referenced from [orig: UIScene_LoadAndParseContent @ 0x63c830 via sub_552500].
//
// Every node stores typed fields that exactly partition the line's bytes
// (no opaque raw-span replay; see ADR 0014), so parse() -> serialize() is
// byte-identical for an untouched document: comments, blank lines, alignment
// whitespace, authored name case, conditional blocks, per-line EOL style,
// BOM, and a missing final newline all survive. flatten() produces the
// runtime StyleSheet view (the "honor" side of the preserve-vs-honor split,
// ADR 0002/0005); edits are minimal-delta (an edited value changes only its
// own line's bytes).

enum class NodeKind : uint8_t {
	Blank,        // whitespace-only line
	Comment,      // full-line // comment
	Directive,    // #if / #else / #endif / unknown #-line
	Define,       // NAME value (one or more physical lines via continuations)
	InactiveText, // non-directive line inside an evaluated-false #if region
};

enum class DirectiveKind : uint8_t { If, Else, Endif, Unknown };

// One physical line owned by a Define node. A pending retail continuation can
// cross directive/inactive source; those lines stay physically ordered here
// with contributes_value=false. Render is the exact byte partition:
//   leading_ws [name sep_ws] chunk
//     non-continued: + pre_comment_ws + comment + eol
//     continued:     + '\' + post_backslash_ws + comment + eol
struct DefineLine {
	std::string leading_ws;
	std::string name;              // authored case; first line only
	std::string sep_ws;            // name->value gap (alignment tabs); first line only
	bool contributes_value = true; // false for directives/inactive source crossed
	                               // while a retail continuation is pending
	std::string chunk;             // authored value text, escapes intact, comment excluded;
	                               // continued lines keep their trailing ws (spec: "all
	                               // characters leading up to the backslash")
	std::string pre_comment_ws;    // ws between chunk and comment/EOL (non-continued lines)
	bool continued = false;        // line ends with a continuation backslash
	std::string post_backslash_ws; // ws between '\' and comment/EOL (continued lines)
	std::string comment;           // inline comment incl. leading "//", or ""
	std::string eol;               // "\r\n" | "\n" | "\r" | "" (EOF without final newline)
};

struct Node {
	NodeKind kind = NodeKind::Blank;
	int line = 0;                  // 1-based first physical line
	std::string leading_ws;        // Blank/Comment/Directive/InactiveText
	std::string text;              // Comment: "//..." to EOL; Directive: authored body from
	                               // '#' to EOL; InactiveText: body after leading_ws
	std::string eol;               // Blank/Comment/Directive/InactiveText
	DirectiveKind directive = DirectiveKind::Unknown; // Directive only
	std::string directive_arg;     // "#if" argument token ("0"/"1"/other)
	std::vector<DefineLine> define_lines; // Define only (>= 1)
};

enum class Severity : uint8_t { Warning, Error };

struct Diagnostic {
	int line = 0; // 1-based physical line
	Severity severity = Severity::Error;
	std::string code;    // stable id, e.g. "duplicate-name"
	std::string message; // human-readable, cites names/lines
};

struct EvaluationResult {
	StyleSheet sheet;
	std::vector<Diagnostic> diagnostics;
	bool success = true;
};

class Document {
public:
	// Permissive: never fails; problems land in diagnostics(). Lenient flatten
	// behavior matches the historical parser (last duplicate wins, non-"0"
	// #if argument is truthy, unknown %VAR% left to the substitution layer).
	static Document parse(const char *data, size_t size);
	static Document parse(const std::string &text);

	// Byte-identical to the parsed input when the document is untouched.
	std::vector<uint8_t> serialize() const;
	// serialize() as text, without the BOM.
	std::string source_text() const;
	// Replace the whole document by reparsing `text`. The BOM flag is sticky:
	// a document loaded with a BOM keeps it across source_text round-trips.
	void set_source_text(const std::string &text);

	bool has_bom() const { return has_bom_; }
	void set_has_bom(bool v) { has_bom_ = v; }
	// EOL used for newly added lines: the first EOL seen at parse, "\r\n"
	// (ship-faithful) for documents that never carried one.
	const std::string &default_eol() const { return default_eol_; }

	// Runtime view: evaluate conditionals, join continuations, strip inline
	// comments, preserve authored backslashes, uppercase keys, last duplicate
	// wins. Diagnostics and success are returned together so callers cannot
	// accidentally discard a retail syntax failure.
	// [orig: consumed by NapiXML_ExpandVariablesInText @ 0x63a000]
	EvaluationResult evaluate() const;
	// Convenience for callers that deliberately accept diagnostics.
	StyleSheet flatten() const;

	const std::vector<Node> &nodes() const { return nodes_; }
	const std::vector<Diagnostic> &diagnostics() const { return diagnostics_; }

	// ---- entry view: active defines, document order ----
	struct Entry {
		int node_index = -1;
		int line = 0;                 // 1-based first physical line
		std::string name;             // authored case
		std::string value;            // logical (comment-stripped, joined, unescaped)
		std::string raw_value;        // authored chunks joined, escapes intact
		std::string inline_comment;   // first line's comment incl. "//", or ""
		bool multiline = false;
		int group = 0;                // increments at each blank/directive run between defines
		std::vector<std::string> preceding_comments; // contiguous Comment nodes directly above
	};
	std::vector<Entry> entries() const;
	// Index into entries() of the LAST active define with this name
	// (case-insensitive) -- the flatten winner. -1 when absent.
	int find_entry(const std::string &name) const;

	// ---- edits ----
	// All return false (and set *error) with the document unmodified on
	// failure. Values are logical: '\' is re-escaped to "\\" on render, and
	// leading/trailing whitespace is trimmed (the format cannot represent it).
	bool set_value(const std::string &name, const std::string &value, std::string *error = nullptr);
	bool rename_define(const std::string &name, const std::string &new_name, std::string *error = nullptr);
	// before_node_index -1 appends; otherwise inserts before that node.
	bool add_define(const std::string &name, const std::string &value,
			int before_node_index = -1,
			const std::string &inline_comment = std::string(),
			std::string *error = nullptr);
	// Removes EVERY active define with the name (whole nodes, continuation
	// lines included; preceding comments stay). Remove/move reject a define
	// whose continuation crosses directive or inactive source, because those
	// structural bytes must remain in place.
	bool remove_define(const std::string &name, std::string *error = nullptr);
	bool move_define(const std::string &name, int before_node_index, std::string *error = nullptr);
	// Plain comment text or a "//"-prefixed comment; empty clears.
	bool set_inline_comment(const std::string &name, const std::string &comment, std::string *error = nullptr);

	// Names: non-empty, no whitespace, none of the spec's six % < > # \ /
	static bool is_valid_name(const std::string &name);
	// Values: no newlines and no "//" (the format cannot represent either).
	static bool is_valid_value(const std::string &value);

private:
	int find_define_node_(const std::string &name) const; // node index, last active match
	void refresh_(); // reparse serialize() so lines/diagnostics stay coherent after edits

	std::vector<Node> nodes_;
	std::vector<Diagnostic> diagnostics_;
	bool has_bom_ = false;
	bool eol_seen_ = false;
	std::string default_eol_ = "\r\n";
};

} // namespace mns
