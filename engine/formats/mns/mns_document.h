#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/mns/mns.h>

namespace opennova::mns {

// Lossless document model for .mns menu stylesheets. The format specification
// is NovaLogic's own comment header in the shipped menu_style.mns ("The style
// sheet basically relies on macro name and value pairs..."); the game reads the
// file [orig: Menu_InitShellResources @ 0x552500 -> NapiConfigMap_LoadIncludeFile
// @ 0x63b970 -> NapiConfigMap_ParseKeyValueBuffer @ 0x639870] (docs/mnu/menu-re.md "The
// stylesheet reader").
//
// Every node stores typed fields that exactly partition the line's bytes
// (no opaque raw-span replay; see ADR 0014), so parse() -> serialize() is
// byte-identical for an untouched document: comments, blank lines, alignment
// whitespace, authored name case, conditional blocks, per-line EOL style,
// BOM, and a missing final newline all survive. The lines are classified the
// way the game's reader walks them (its #if / #else / #endif machine, a value
// that starts on a later line, a continuation crossing blank, comment and
// directive lines); evaluate() is that reader itself over the document's bytes.
// Edits are minimal-delta (an edited value changes only its own line's bytes)
// and verified: an edit the game would read differently than the document
// says (a line added inside a switched-off #if, or after a line whose value
// continues) is refused.

enum class NodeKind : uint8_t {
	Blank,        // a line of blanks (and backslashes the reader steps over)
	Comment,      // full-line // comment the reader skips
	Directive,    // #if / #else / #endif / any other #-line
	Define,       // NAME value (one or more physical lines)
	InactiveText, // a line inside switched-off source the reader seeks over
};

enum class DirectiveKind : uint8_t { If, Else, Endif, Unknown };

// One physical line owned by a Define node. A value that is still to come (a
// continuation, or a name with nothing after it) crosses blank, comment,
// directive and inactive lines; those stay physically ordered here with
// contributes_value=false. Render is the exact byte partition:
//   leading_ws [name sep_ws] chunk
//     non-continued: + pre_comment_ws + comment + eol
//     continued:     + '\' + post_backslash_ws + comment + eol
struct DefineLine {
	std::string leading_ws;        // blanks, and the backslashes the reader steps over at a line start
	std::string name;              // authored case; first line only
	std::string sep_ws;            // name->value gap (alignment tabs, the NAME\ form's backslash); first line only
	bool contributes_value = true; // false for a line crossed while the value is still to come
	std::string chunk;             // authored value text, escapes intact, comment excluded;
	                               // continued lines keep their trailing ws (spec: "all
	                               // characters leading up to the backslash"). A crossed
	                               // line keeps its whole body here.
	std::string pre_comment_ws;    // ws between chunk and comment/EOL (non-continued lines)
	bool continued = false;        // line ends with a continuation backslash
	std::string post_backslash_ws; // ws between '\' and comment/EOL (continued lines)
	std::string comment;           // inline comment incl. leading "//", or ""
	std::string eol;               // "\r\n" | "\n" | "\r" | "" (EOF without final newline)
};

struct Node {
	NodeKind kind = NodeKind::Blank;
	int line = 0;                  // 1-based first physical line (as parsed; stale after an edit)
	std::string leading_ws;        // Blank/Comment/Directive/InactiveText
	std::string text;              // Comment: "//..." to EOL; Directive: authored body from
	                               // '#' to EOL; InactiveText: body after leading_ws
	std::string eol;               // Blank/Comment/Directive/InactiveText
	DirectiveKind directive = DirectiveKind::Unknown; // Directive only
	std::string directive_arg;     // "#if" argument byte ("0"/"1"/other)
	std::vector<DefineLine> define_lines; // Define only (>= 1)
};

enum class Severity : uint8_t { Warning, Error };

// An Error is a line the game stops reading at, would stop responding on, or that
// the document cannot show the way the game reads it: the editor refuses to build
// such a sheet. A Warning is a line the game reads in a way the author may not
// expect (docs/mnu/menu-re.md).
struct Diagnostic {
	int line = 0; // 1-based physical line
	Severity severity = Severity::Error;
	std::string code;    // stable id, e.g. "duplicate-name"
	std::string message; // human-readable, cites names/lines
};

struct EvaluationResult {
	StyleSheet sheet;                     // what the game reads
	std::vector<Diagnostic> diagnostics;  // the document's, plus the reader's stop when none covers it
	bool success = true;                  // the game reads the whole sheet
	int stopped_line = 0;                 // where the game stops reading (0 = it reads it all)
	bool hangs = false;                   // it would stop responding there (the port stops)
};

class Document {
public:
	// Permissive: never fails; problems land in diagnostics().
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
	// EOL used for newly added lines: always CRLF, the only line end the game reads
	// without stopping [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639c3b].
	const std::string &default_eol() const { return default_eol_; }

	// The game's own read of the document's bytes [orig: NapiConfigMap_ParseKeyValueBuffer @
	// 0x639870] into an empty list, with the document's diagnostics. The sheet holds
	// what it read up to where it stops.
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
		std::string value;            // logical (the game's value with "\\" collapsed)
		std::string raw_value;        // authored chunks joined, escapes intact
		std::string inline_comment;   // first line's comment incl. "//", or ""
		bool multiline = false;
		int group = 0;                // increments at each blank/directive run between defines
		std::vector<std::string> preceding_comments; // contiguous Comment nodes directly above
	};
	std::vector<Entry> entries() const;
	// Index into entries() of the LAST active define with this name
	// (case-insensitive): the one whose value the game reads. -1 when absent.
	int find_entry(const std::string &name) const;

	// ---- edits ----
	// All return false (and set *error) with the document unmodified on failure,
	// including an edit the game would read differently (a define added or moved into
	// a switched-off #if block, or after a line whose value continues). Values are
	// logical: '\' is re-escaped to "\\" on render, and leading/trailing whitespace is
	// trimmed (the format cannot represent it).
	//
	// A name defined more than once: set_value and set_inline_comment edit the last
	// definition (the one the game reads), remove_define removes every definition, and
	// rename_define and move_define refuse (renaming or moving only the last would change
	// which value the game reads): remove the extra definitions first.
	bool set_value(const std::string &name, const std::string &value, std::string *error = nullptr);
	bool rename_define(const std::string &name, const std::string &new_name, std::string *error = nullptr);
	// before_node_index -1 appends; otherwise inserts before that node.
	bool add_define(const std::string &name, const std::string &value,
			int before_node_index = -1,
			const std::string &inline_comment = std::string(),
			std::string *error = nullptr);
	// Removes EVERY active define with the name (whole nodes, continuation
	// lines included; preceding comments stay). Remove/move reject a define
	// whose value crosses other lines (blank, comment, directive or inactive),
	// because those bytes must remain in place.
	bool remove_define(const std::string &name, std::string *error = nullptr);
	bool move_define(const std::string &name, int before_node_index, std::string *error = nullptr);
	// Plain comment text or a "//"-prefixed comment; empty clears.
	bool set_inline_comment(const std::string &name, const std::string &comment, std::string *error = nullptr);

	// Names: non-empty, no whitespace, none of the spec's six % < > # \ /
	static bool is_valid_name(const std::string &name);
	// Logical values: non-empty, no newline, no "//", not starting with '#' (the game
	// stops responding on a value that does).
	static bool is_valid_value(const std::string &value);

private:
	int find_define_node_(const std::string &name) const; // node index, last active match
	std::vector<int> define_nodes_(const std::string &name) const; // every active match
	// Adopt `proposed` when the game reads it as the document says (reread), else refuse.
	bool commit_(const std::vector<Node> &proposed, std::string *error);

	std::vector<Node> nodes_;
	std::vector<Diagnostic> diagnostics_;
	bool has_bom_ = false;
	std::string default_eol_ = "\r\n";
};

// ---- the primitives the Document and the editor's stylesheet document share ----

// One node's exact bytes; a node that is not the file's last and has no EOL gets `eol`.
void render_node(const Node &node, bool last, const std::string &eol, std::string &out);
std::string render_nodes(const std::vector<const Node *> &nodes, const std::string &eol);
// Every line end the node has becomes `eol` (a missing final one stays missing): the
// editor's stylesheets end every line CRLF. True when any line end changed.
bool end_lines_with(Node &node, const std::string &eol);
// The value the game reads for a define as the document classifies its lines: the
// contributing segments joined, a lone '\' dropped with the blanks after it, a "\\"
// pair kept doubled, a value at the end of the file untrimmed
// [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639bbc..0x639d6b].
std::string game_value(const Node &define);
// True when a define's value crosses other lines (blank, comment, directive,
// inactive): those lines are owned by it and must stay where they are.
bool crosses_structure(const Node &define);
Node make_define(const std::string &name, const std::string &chunk, const std::string &comment,
		const std::string &eol);
Node make_comment(const std::string &text, const std::string &eol);
Node make_blank(const std::string &eol);
// Replace a define's value with an AUTHORED chunk (the game's value, escapes as the
// file writes them): trimmed, then checked by is_valid_game_value. A one-line value is
// replaced in place; a multi-line one collapses onto its first line (its inline
// comments coalesced); one whose value crosses other lines keeps them and carries the
// new value on its last value line.
bool set_define_chunk(Node &define, const std::string &chunk, std::string *error);
bool set_define_name(Node &define, const std::string &name, std::string *error);
// Plain comment text or a "//"-prefixed comment; empty clears.
bool set_define_comment(Node &define, const std::string &comment, std::string *error);
// An authored value the game reads as written: non-empty, no newline, no "//", every
// '\' inside a "\\" pair, not starting with '#' or a blank.
bool is_valid_game_value(const std::string &chunk);
// "" stays empty; "//..." kept verbatim; plain text gains a "// " prefix.
std::string normalize_comment(const std::string &text);

enum class Reread : uint8_t {
	Same,      // the game reads the rendered nodes as they are
	Inactive,  // a line would sit in switched-off source
	Continued, // a line would become part of the value of the define above it
	Changed,   // the lines would be read another way
};
// Render the nodes, reparse, and compare each node's kind and physical line count;
// `first` receives the first node that differs.
Reread reread(const std::vector<const Node *> &nodes, const std::string &eol, size_t *first = nullptr);

}  // namespace opennova::mns
