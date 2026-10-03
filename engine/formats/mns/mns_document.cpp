#include <formats/mns/mns_document.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace opennova::mns {

namespace {

// A blank inside a line to the reader: the CRT's isspace without the line ends
// [orig: isspace @ 0x76b964].
bool is_hws(char c) {
	return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

// The spec's six forbidden name characters (the quotes in the in-file spec
// delimit the set): % < > # \ /
bool is_invalid_name_char(char c) {
	return c == '%' || c == '<' || c == '>' || c == '#' || c == '\\' || c == '/';
}

bool is_comment_at(const char *at, const char *line_end) {
	return line_end - at >= 2 && at[0] == '/' && at[1] == '/';
}

std::string trim_hws(const std::string &s) {
	size_t b = 0;
	while (b < s.size() && is_hws(s[b])) ++b;
	size_t e = s.size();
	while (e > b && is_hws(s[e - 1])) --e;
	return s.substr(b, e - b);
}

// Logical -> authored: every backslash becomes the "\\" escape.
std::string escape_value(const std::string &logical) {
	std::string out;
	out.reserve(logical.size());
	for (char c : logical) {
		if (c == '\\') out += '\\';
		out += c;
	}
	return out;
}

// A chunk as the reader joins it: a "\\" pair kept doubled [orig: @ 0x639c17], a lone
// '\' dropped with the blanks after it (the segment ends there and the next text is
// appended) [orig: @ 0x639d27..0x639d6b]. Returns true when it joined anything.
bool append_joined(std::string &out, const std::string &chunk) {
	bool joined = false;
	for (size_t i = 0; i < chunk.size(); ++i) {
		if (chunk[i] == '\\') {
			if (i + 1 < chunk.size() && chunk[i + 1] == '\\') {
				out += "\\\\";
				++i;
				continue;
			}
			joined = true;
			while (i + 1 < chunk.size() && is_hws(chunk[i + 1])) ++i;
			continue;
		}
		out += chunk[i];
	}
	return joined;
}

bool joins_segments(const Node &define) {
	for (const DefineLine &line : define.define_lines) {
		if (!line.contributes_value) continue;
		if (line.continued) return true;
		std::string ignored;
		if (append_joined(ignored, line.chunk)) return true;
	}
	return false;
}

// The game's value with each "\\" pair collapsed: what an author means by it.
std::string logical_value(const Node &node) {
	const std::string game = game_value(node);
	std::string out;
	for (size_t i = 0; i < game.size(); ++i) {
		out += game[i];
		if (game[i] == '\\' && i + 1 < game.size() && game[i + 1] == '\\') ++i;
	}
	return out;
}

std::string raw_value(const Node &node) {
	std::string joined;
	for (const DefineLine &dl : node.define_lines) {
		if (!dl.contributes_value) continue;
		joined += dl.chunk;
	}
	return joined;
}

size_t line_count(const Node &node) {
	return node.kind == NodeKind::Define ? node.define_lines.size() : 1;
}

bool contains_newline(const std::string &s) {
	return s.find('\n') != std::string::npos || s.find('\r') != std::string::npos;
}

void set_error(std::string *error, const std::string &message) {
	if (error != nullptr) *error = message;
}

std::string define_name(const Node &node) {
	return node.define_lines.front().name;
}

} // namespace

// ---- primitives -------------------------------------------------------------------

std::string normalize_comment(const std::string &text) {
	if (text.empty()) return std::string();
	if (text.size() >= 2 && text[0] == '/' && text[1] == '/') return text;
	return "// " + text;
}

void render_node(const Node &node, bool last, const std::string &eol, std::string &out) {
	auto end_line = [&](const std::string &line_eol, bool final_line) {
		if (line_eol.empty()) {
			if (!final_line) out += eol;
		} else {
			out += line_eol;
		}
	};
	switch (node.kind) {
	case NodeKind::Blank:
		out += node.leading_ws;
		end_line(node.eol, last);
		break;
	case NodeKind::Comment:
	case NodeKind::Directive:
	case NodeKind::InactiveText:
		out += node.leading_ws;
		out += node.text;
		end_line(node.eol, last);
		break;
	case NodeKind::Define:
		for (size_t i = 0; i < node.define_lines.size(); ++i) {
			const DefineLine &dl = node.define_lines[i];
			out += dl.leading_ws;
			out += dl.name;
			out += dl.sep_ws;
			out += dl.chunk;
			if (dl.continued) {
				out += '\\';
				out += dl.post_backslash_ws;
			} else {
				out += dl.pre_comment_ws;
			}
			out += dl.comment;
			end_line(dl.eol, last && i + 1 == node.define_lines.size());
		}
		break;
	}
}

std::string render_nodes(const std::vector<const Node *> &nodes, const std::string &eol) {
	std::string out;
	for (size_t i = 0; i < nodes.size(); ++i) render_node(*nodes[i], i + 1 == nodes.size(), eol, out);
	return out;
}

bool end_lines_with(Node &node, const std::string &eol) {
	bool changed = false;
	auto end = [&](std::string &line_eol) {
		if (line_eol.empty() || line_eol == eol) return;
		line_eol = eol;
		changed = true;
	};
	end(node.eol);
	for (DefineLine &line : node.define_lines) end(line.eol);
	return changed;
}

std::string game_value(const Node &define) {
	std::string out;
	const DefineLine *last = nullptr;
	for (const DefineLine &line : define.define_lines) {
		if (!line.contributes_value) continue;
		append_joined(out, line.chunk);
		last = &line;
	}
	// A value the buffer's end stops keeps its trailing blanks [orig: @ 0x639c8b].
	if (last != nullptr && last->eol.empty() && !last->continued && last->comment.empty()) out += last->pre_comment_ws;
	return out;
}

bool crosses_structure(const Node &define) {
	return std::any_of(define.define_lines.begin(), define.define_lines.end(),
	                   [](const DefineLine &line) { return !line.contributes_value; });
}

Node make_define(const std::string &name, const std::string &chunk, const std::string &comment,
		const std::string &eol) {
	Node node;
	node.kind = NodeKind::Define;
	DefineLine dl;
	dl.name = name;
	dl.sep_ws = "\t";
	dl.chunk = chunk;
	dl.comment = normalize_comment(comment);
	dl.pre_comment_ws = dl.comment.empty() ? std::string() : std::string("\t");
	dl.eol = eol;
	node.define_lines.push_back(std::move(dl));
	return node;
}

Node make_comment(const std::string &text, const std::string &eol) {
	Node node;
	node.kind = NodeKind::Comment;
	node.text = text.empty() ? std::string("//") : normalize_comment(text);
	node.eol = eol;
	return node;
}

Node make_blank(const std::string &eol) {
	Node node;
	node.kind = NodeKind::Blank;
	node.eol = eol;
	return node;
}

bool is_valid_game_value(const std::string &chunk) {
	if (chunk.empty() || contains_newline(chunk) || chunk.find('\0') != std::string::npos) return false;
	if (chunk.find("//") != std::string::npos) return false; // would read as a comment
	if (chunk.front() == '#' || is_hws(chunk.front()) || is_hws(chunk.back())) return false;
	for (size_t i = 0; i < chunk.size(); ++i) {
		if (chunk[i] != '\\') continue;
		if (i + 1 < chunk.size() && chunk[i + 1] == '\\') {
			++i;
			continue;
		}
		return false; // a lone '\': the game joins or continues the value there
	}
	return true;
}

bool set_define_chunk(Node &define, const std::string &chunk, std::string *error) {
	if (define.kind != NodeKind::Define) {
		set_error(error, "this line is not a variable");
		return false;
	}
	const std::string v = trim_hws(chunk);
	if (v.empty()) {
		set_error(error, "a variable needs a value: with nothing after its name the game reads the next line as its value");
		return false;
	}
	if (!is_valid_game_value(v)) {
		set_error(error, v.front() == '#' ? "a value cannot start with '#': the game stops responding on it"
		                 : "a value cannot contain a line break, '//' or a lone '\\' (write '\\\\' for a backslash)");
		return false;
	}
	std::vector<DefineLine> &lines = define.define_lines;
	if (crosses_structure(define)) {
		// The crossed lines stay where they are: the value goes on the last value line and
		// the earlier value lines keep only their continuation.
		size_t last = 0;
		for (size_t i = 0; i < lines.size(); ++i)
			if (lines[i].contributes_value) last = i;
		for (size_t i = 0; i < lines.size(); ++i) {
			if (!lines[i].contributes_value) continue;
			lines[i].chunk = i == last ? v : std::string();
		}
		return true;
	}
	if (lines.size() == 1) {
		DefineLine &line = lines.front();
		line.chunk = v;
		if (line.sep_ws.empty()) line.sep_ws = "\t";
		if (line.continued) {
			// A continuation at the end of the file ends with the new value.
			line.continued = false;
			line.pre_comment_ws = line.comment.empty() ? std::string()
			                      : (line.post_backslash_ws.empty() ? std::string("\t") : line.post_backslash_ws);
			line.post_backslash_ws.clear();
		}
		return true;
	}
	// Collapse to one line. Layout comes from the first line (the NAME\ form's backslash
	// dropped); the EOL from the last (preserves a document-final missing newline).
	// Inline comments from all spanned lines coalesce so no text is lost.
	const DefineLine &first = lines.front();
	const DefineLine &last = lines.back();
	std::string combined_comment;
	for (const DefineLine &dl : lines) {
		if (dl.comment.empty()) continue;
		if (combined_comment.empty()) {
			combined_comment = dl.comment;
		} else {
			const std::string &body = dl.comment;
			size_t b = 0;
			while (b < body.size() && body[b] == '/') ++b;
			while (b < body.size() && is_hws(body[b])) ++b;
			combined_comment += " " + body.substr(b);
		}
	}
	DefineLine collapsed;
	collapsed.leading_ws = first.leading_ws;
	collapsed.name = first.name;
	for (char c : first.sep_ws)
		if (c != '\\') collapsed.sep_ws += c;
	if (collapsed.sep_ws.empty()) collapsed.sep_ws = "\t";
	collapsed.chunk = v;
	collapsed.comment = combined_comment;
	collapsed.pre_comment_ws = combined_comment.empty()
			? std::string()
			: (first.pre_comment_ws.empty() ? std::string("\t") : first.pre_comment_ws);
	collapsed.eol = last.eol;
	lines.clear();
	lines.push_back(std::move(collapsed));
	return true;
}

bool set_define_name(Node &define, const std::string &name, std::string *error) {
	if (define.kind != NodeKind::Define) {
		set_error(error, "this line is not a variable");
		return false;
	}
	const std::string nn = trim_hws(name);
	if (!Document::is_valid_name(nn)) {
		set_error(error, "'" + nn + "' is not a valid name (no spaces or % < > # \\ /)");
		return false;
	}
	DefineLine &first = define.define_lines.front();
	first.name = nn;
	if (first.sep_ws.empty()) first.sep_ws = "\t";
	return true;
}

bool set_define_comment(Node &define, const std::string &comment, std::string *error) {
	if (define.kind != NodeKind::Define) {
		set_error(error, "this line is not a variable");
		return false;
	}
	if (contains_newline(comment)) {
		set_error(error, "comment cannot contain newlines");
		return false;
	}
	DefineLine &dl = define.define_lines.front();
	dl.comment = normalize_comment(comment);
	if (dl.continued) {
		if (!dl.comment.empty() && dl.post_backslash_ws.empty()) dl.post_backslash_ws = " ";
	} else {
		dl.pre_comment_ws = dl.comment.empty()
				? std::string()
				: (dl.pre_comment_ws.empty() ? std::string("\t") : dl.pre_comment_ws);
	}
	return true;
}

Reread reread(const std::vector<const Node *> &nodes, const std::string &eol, size_t *first) {
	const Document again = Document::parse(render_nodes(nodes, eol));
	const std::vector<Node> &got = again.nodes();
	size_t i = 0;
	while (i < nodes.size() && i < got.size() && got[i].kind == nodes[i]->kind &&
	       line_count(got[i]) == line_count(*nodes[i]))
		++i;
	if (i == nodes.size() && i == got.size()) return Reread::Same;
	if (first != nullptr) *first = i;
	if (i < got.size() && i < nodes.size()) {
		if (got[i].kind == NodeKind::InactiveText && nodes[i]->kind != NodeKind::InactiveText) return Reread::Inactive;
		if (got[i].kind == NodeKind::Define && nodes[i]->kind == NodeKind::Define &&
		    line_count(got[i]) > line_count(*nodes[i]))
			return Reread::Continued;
	}
	return Reread::Changed;
}

// ---- the document -----------------------------------------------------------------

bool Document::is_valid_name(const std::string &name) {
	if (name.empty()) return false;
	for (char c : name) {
		if (is_hws(c) || c == '\n' || c == '\r' || c == '\0' || is_invalid_name_char(c)) return false;
	}
	return true;
}

bool Document::is_valid_value(const std::string &value) {
	if (value.empty() || contains_newline(value)) return false;
	if (value.find("//") != std::string::npos) return false; // would parse as a comment
	if (value.front() == '#') return false;                  // the game stops responding on it
	return true;
}

namespace {

// In DiagnosticCode's order (diagnostic_code_token indexes it).
const char *const kDiagnosticCodeTokens[] = {
	"line-ending",
	"directive-form",
	"if-without-argument",
	"noncanonical-if-arg",
	"unbalanced-else",
	"duplicate-else",
	"unbalanced-endif",
	"unknown-directive",
	"directive-tail",
	"lone-backslash",
	"value-is-directive",
	"value-starts-with-hash",
	"value-on-next-line",
	"duplicate-name",
	"continued-duplicate",
	"nul-byte",
	"invalid-name-char",
	"missing-value-delimiter",
	"no-value",
	"continuation-at-eof",
	"unterminated-if",
	"hangs",
	"stops",
};
static_assert(sizeof(kDiagnosticCodeTokens) / sizeof(kDiagnosticCodeTokens[0]) ==
                      kDiagnosticCodeCount,
              "every DiagnosticCode has exactly one token");

} // namespace

const char *diagnostic_code_token(DiagnosticCode code) {
	const size_t at = static_cast<size_t>(code);
	return at < kDiagnosticCodeCount ? kDiagnosticCodeTokens[at] : "";
}

Document Document::parse(const std::string &text) {
	return parse(text.data(), text.size());
}

// The lines, classified the way parse_key_value_buffer walks them (docs/mnu/menu-re.md
// "The stylesheet reader"); the byte-level reader itself runs in evaluate().
Document Document::parse(const char *data, size_t size) {
	Document doc;
	const char *p = data;
	const char *const end = data + size;

	if (size >= 3 && static_cast<uint8_t>(p[0]) == 0xEF &&
		static_cast<uint8_t>(p[1]) == 0xBB && static_cast<uint8_t>(p[2]) == 0xBF) {
		doc.has_bom_ = true;
		p += 3;
	}

	// The reader's conditional machine [orig: NapiConfigMap_ParseKeyValueBuffer @ 0x639944..0x639a38]:
	// a depth, whether the lines are switched off, and the depth that started it. An #if
	// inside switched-off source only counts; an #else toggles at the owning depth; an
	// #endif leaving it toggles back.
	int depth = 0;
	bool suppressed = false;
	int suppressed_depth = -1;
	// The authored nesting, for the diagnostics only.
	struct Open {
		int line = 0;
		bool had_else = false;
	};
	std::vector<Open> open;
	// Inside switched-off source the reader seeks the next '#' byte from where it stopped
	// [orig: @ 0x639aac]: after an ordinary switched-off line it is not at a token
	// boundary, so a "//" there does not hide a '#' after it.
	bool seeking = false;
	// A define whose value is still to come: after its name, or after a lone '\'.
	enum class Pending { None, Value, Continuation } pending = Pending::None;
	int line_no = 1;
	std::unordered_map<std::string, int> seen; // uppercase active name -> first line
	bool eol_reported = false, nul_reported = false;

	auto diag = [&](int line, Severity sev, DiagnosticCode code, const std::string &message) {
		doc.diagnostics_.push_back(Diagnostic{line, sev, code, message});
	};

	auto read_eol = [&](std::string &out_eol, int line) {
		out_eol.clear();
		if (p < end && *p == '\r') {
			if (p + 1 < end && p[1] == '\n') {
				out_eol = "\r\n";
				p += 2;
			} else {
				out_eol = "\r";
				p += 1;
			}
		} else if (p < end && *p == '\n') {
			out_eol = "\n";
			p += 1;
		}
		if ((out_eol == "\n" || out_eol == "\r") && !eol_reported) {
			eol_reported = true;
			diag(line, Severity::Error, DiagnosticCode::LineEnding,
			     std::string("this line ends in a lone ") + (out_eol == "\n" ? "LF" : "CR") +
			             ": the game stops responding on a stylesheet whose lines do not end in CR LF");
		}
	};

	auto line_end_from = [&](const char *start) {
		const char *line_end = start;
		while (line_end < end && *line_end != '\n' && *line_end != '\r') ++line_end;
		return line_end;
	};

	auto find_hash = [&](const char *start, const char *line_end) -> const char * {
		const char *hash = start;
		while (hash < line_end && *hash != '#') ++hash;
		return hash < line_end ? hash : nullptr;
	};

	auto starts_with = [&](const char *at, const char *word) {
		const size_t n = std::strlen(word);
		return size_t(end - at) >= n && std::memcmp(at, word, n) == 0;
	};
	// The directive a '#' starts, matched as the reader matches it (a prefix), or null.
	auto directive_named = [&](const char *hash) -> const char * {
		if (starts_with(hash + 1, "if")) return "if";
		if (starts_with(hash + 1, "else")) return "else";
		if (starts_with(hash + 1, "endif")) return "endif";
		return nullptr;
	};

	// One directive at `hash`, matched as a prefix the way the reader matches it; returns
	// where the reader goes on reading the line.
	auto apply_directive = [&](const char *hash, const char *line_end, int line, DirectiveKind &kind,
	                           std::string &arg) -> const char * {
		const char *token_end = hash + 1;
		while (token_end < line_end && !is_hws(*token_end)) ++token_end;
		const std::string token(hash + 1, token_end);
		auto misspelled = [&](const char *as) {
			diag(line, Severity::Error, DiagnosticCode::DirectiveForm,
			     "the game reads '#" + token + "' as '#" + as +
			             "' followed by the rest of the word: write #if 0, #if 1, #else or #endif");
		};
		if (starts_with(hash + 1, "if")) {
			kind = DirectiveKind::If;
			++depth;
			open.push_back(Open{line, false});
			if (token != "if") misspelled("if");
			const char *after = std::min(hash + 4, line_end); // '#', "if" and the byte after them
			if (!suppressed) {
				while (after < line_end && is_hws(*after)) ++after;
				if (after == line_end) {
					diag(line, Severity::Error, DiagnosticCode::IfWithoutArgument,
					     "'#if' has no 0 or 1 on its line: the game takes the first character of the next line as its condition");
				} else {
					arg.assign(1, *after);
					if (*after == '0') {
						suppressed = true;
						suppressed_depth = depth;
					} else if (*after != '1') {
						diag(line, Severity::Warning, DiagnosticCode::NoncanonicalIfArg,
						     "'#if' reads only its first character, '" + arg +
						             "', and anything but 0 keeps the lines on: write 0 or 1");
					}
					++after;
				}
			}
			return after;
		}
		if (starts_with(hash + 1, "else")) {
			kind = DirectiveKind::Else;
			if (token != "else") misspelled("else");
			if (open.empty()) {
				diag(line, Severity::Warning, DiagnosticCode::UnbalancedElse,
				     "'#else' has no '#if' before it: the game still switches the lines after it on or off, "
				     "until the next '#else' or '#endif'");
			} else if (open.back().had_else) {
				diag(line, Severity::Warning, DiagnosticCode::DuplicateElse,
				     "a second '#else' in one '#if' block switches the lines after it back");
			} else {
				open.back().had_else = true;
			}
			if (!suppressed || depth == suppressed_depth) {
				suppressed = !suppressed;
				suppressed_depth = suppressed ? depth : -1;
			}
			return std::min(hash + 5, line_end);
		}
		if (starts_with(hash + 1, "endif")) {
			kind = DirectiveKind::Endif;
			if (token != "endif") misspelled("endif");
			if (open.empty()) {
				diag(line, Severity::Warning, DiagnosticCode::UnbalancedEndif,
				     "'#endif' has no '#if' before it; a second one switches the lines after it off");
			} else {
				open.pop_back();
			}
			const int prev_depth = depth--;
			if (prev_depth == suppressed_depth) {
				suppressed = !suppressed;
				suppressed_depth = suppressed ? depth : -1;
			}
			return std::min(hash + 6, line_end);
		}
		kind = DirectiveKind::Unknown;
		diag(line, Severity::Error, DiagnosticCode::UnknownDirective,
		     "the game stops responding on '#" + token +
		             "': only #if, #else and #endif are directives (a comment starts with //)");
		return line_end;
	};

	// The rest of a directive's line: the reader goes on reading it at a token boundary.
	auto directive_tail = [&](const char *after, const char *line_end, int line) {
		const char *t = after;
		while (t < line_end && is_hws(*t)) ++t;
		if (t == line_end || is_comment_at(t, line_end)) {
			seeking = false;
			return;
		}
		if (!suppressed) {
			diag(line, Severity::Error, DiagnosticCode::DirectiveTail,
			     "the game reads '" + std::string(t, line_end) + "' after the directive as a variable");
			seeking = false;
		} else if (find_hash(t, line_end) != nullptr) {
			diag(line, Severity::Error, DiagnosticCode::DirectiveTail,
			     "the game reads the '#' after this directive as another directive");
			seeking = false;
		} else {
			seeking = true; // switched-off text: the seek goes on from here
		}
	};

	// One value segment from `start` to the line's end; fills the chunk, comment and
	// whitespace fields; true when the line ends in a continuation backslash.
	auto scan_value = [&](const char *start, const char *line_end, DefineLine &out, int line) -> bool {
		const char *q = start;
		const char *comment_start = nullptr;
		const char *backslash = nullptr;
		while (q < line_end) {
			if (is_comment_at(q, line_end)) {
				comment_start = q;
				break;
			}
			if (*q == '\\') {
				if (q + 1 < line_end && q[1] == '\\') { // "\\" stays in the chunk, doubled
					q += 2;
					continue;
				}
				const char *j = q + 1;
				while (j < line_end && is_hws(*j)) ++j;
				if (j == line_end || is_comment_at(j, line_end)) {
					backslash = q;
					break;
				}
				// A lone '\' inside the line: the reader ends the segment there and appends the
				// text after the blanks [orig: @ 0x639d27..0x639d6b].
				diag(line, Severity::Warning, DiagnosticCode::LoneBackslash,
				     "the game drops a lone '\\' and the blanks after it, joining the text around it "
				     "(write '\\\\' for a backslash)");
				// The reader meets the '#' at a token boundary: a directive runs there, and the
				// next text it reads joins the value; any other '#' it never leaves
				// [orig: @ 0x639944..0x639a38, @ 0x639a11].
				if (*j == '#') {
					if (const char *directive = directive_named(j))
						diag(line, Severity::Error, DiagnosticCode::ValueIsDirective,
						     std::string("the game runs the '#") + directive +
						             "' after the lone '\\' as a directive and joins the next text it reads onto "
						             "the value, so the lines below are not read as shown");
					else
						diag(line, Severity::Error, DiagnosticCode::ValueStartsWithHash,
						     "the game stops responding on the '#' that follows the lone '\\'");
				}
				q = j;
				continue;
			}
			++q;
		}
		if (backslash != nullptr) {
			out.chunk.assign(start, backslash); // pre-backslash whitespace kept (spec)
			out.continued = true;
			const char *ws = backslash + 1;
			const char *ws_end = ws;
			while (ws_end < line_end && is_hws(*ws_end)) ++ws_end;
			out.post_backslash_ws.assign(ws, ws_end);
			out.comment.assign(ws_end, line_end);
			return true;
		}
		const char *seg_end = comment_start != nullptr ? comment_start : line_end;
		const char *chunk_end = seg_end;
		while (chunk_end > start && is_hws(chunk_end[-1])) --chunk_end;
		out.chunk.assign(start, chunk_end);
		out.pre_comment_ws.assign(chunk_end, seg_end);
		if (comment_start != nullptr) out.comment.assign(comment_start, line_end);
		return false;
	};

	// A define whose value is complete: the name-level diagnostics.
	auto finish_define = [&]() {
		const Node &node = doc.nodes_.back();
		const DefineLine &first = node.define_lines.front();
		const std::string &name = first.name;
		if (first.chunk.empty() && !first.continued && first.sep_ws.find('\\') == std::string::npos) {
			for (size_t i = 1; i < node.define_lines.size(); ++i) {
				if (!node.define_lines[i].contributes_value) continue;
				diag(node.line, Severity::Warning, DiagnosticCode::ValueOnNextLine,
				     "nothing follows '" + name + "' on its line, so the game reads line " +
				             std::to_string(node.line + int(i)) + " as its value");
				break;
			}
		}
		const std::string upper = strutil::to_upper(name);
		auto it = seen.find(upper);
		if (it != seen.end()) {
			diag(node.line, Severity::Warning, DiagnosticCode::DuplicateName,
			     "duplicate macro name '" + name + "' (first defined at line " + std::to_string(it->second) +
			             "): the game reads this value under the first spelling");
			if (joins_segments(node)) {
				diag(node.line, Severity::Error, DiagnosticCode::ContinuedDuplicate,
				     "'" + name + "' is defined again and its value continues: the game adds the continued "
				     "text to the last new variable before it instead");
			}
		} else {
			seen.emplace(upper, node.line);
		}
	};

	while (p < end) {
		const int line = line_no;
		const char *const line_end = line_end_from(p);
		if (!nul_reported && std::memchr(p, 0, size_t(line_end - p)) != nullptr) {
			nul_reported = true;
			diag(line, Severity::Error, DiagnosticCode::NulByte, "the game stops reading the stylesheet at the NUL byte on this line");
		}
		// The leading blanks; at a token boundary also the backslashes the reader steps over.
		const char *rest = p;
		while (rest < line_end && (is_hws(*rest) || (!seeking && *rest == '\\'))) ++rest;
		const bool blank = rest == line_end;
		const bool comment = !blank && !seeking && is_comment_at(rest, line_end);
		const char *hash = nullptr;
		if (!blank && !comment) {
			if (!seeking && *rest == '#') hash = rest;
			else if (suppressed) hash = find_hash(rest, line_end);
		}

		// A line a pending value crosses belongs to its define, kept byte for byte.
		auto crossed = [&]() {
			DefineLine dl;
			dl.leading_ws.assign(p, rest);
			dl.contributes_value = false;
			dl.chunk.assign(rest, line_end);
			p = line_end;
			read_eol(dl.eol, line);
			doc.nodes_.back().define_lines.push_back(std::move(dl));
		};
		auto own_node = [&](NodeKind kind) -> Node & {
			Node node;
			node.kind = kind;
			node.line = line;
			node.leading_ws.assign(p, rest);
			node.text.assign(rest, line_end);
			p = line_end;
			read_eol(node.eol, line);
			doc.nodes_.push_back(std::move(node));
			return doc.nodes_.back();
		};

		if (blank || comment) {
			if (pending != Pending::None) crossed();
			else own_node(blank ? NodeKind::Blank : NodeKind::Comment);
		} else if (hash != nullptr) {
			DirectiveKind kind = DirectiveKind::Unknown;
			std::string arg;
			const char *after = apply_directive(hash, line_end, line, kind, arg);
			if (kind == DirectiveKind::Unknown) seeking = false;
			else directive_tail(after, line_end, line);
			if (pending != Pending::None) {
				crossed();
			} else {
				Node &node = own_node(NodeKind::Directive);
				node.directive = kind;
				node.directive_arg = arg;
			}
		} else if (suppressed) {
			seeking = true;
			if (pending != Pending::None) crossed();
			else own_node(NodeKind::InactiveText);
		} else if (pending != Pending::None) {
			// The value's next segment.
			DefineLine dl;
			dl.leading_ws.assign(p, rest);
			const bool continued = scan_value(rest, line_end, dl, line);
			p = line_end;
			read_eol(dl.eol, line);
			doc.nodes_.back().define_lines.push_back(std::move(dl));
			pending = continued ? Pending::Continuation : Pending::None;
			if (pending == Pending::None) finish_define();
		} else {
			// A define: NAME, the gap the reader skips (blanks, a backslash), the value.
			Node node;
			node.kind = NodeKind::Define;
			node.line = line;
			DefineLine first;
			first.leading_ws.assign(p, rest);
			const char *q = rest;
			while (q < line_end && !is_hws(*q) && *q != '\\') ++q;
			first.name.assign(rest, q);
			for (char c : first.name) {
				if (!is_invalid_name_char(c)) continue;
				diag(line, Severity::Error, DiagnosticCode::InvalidNameChar,
				     "macro name '" + first.name + "' contains '" + std::string(1, c) +
				             "': the game stops reading the stylesheet there, and every variable after it stays undefined");
				break;
			}
			const char *s = q;
			while (s < line_end && (is_hws(*s) || *s == '\\')) ++s;
			first.sep_ws.assign(q, s);
			if (s == line_end || is_comment_at(s, line_end)) {
				first.comment.assign(s, line_end);
				if (first.sep_ws.empty()) {
					diag(line, Severity::Error, DiagnosticCode::MissingValueDelimiter,
					     "macro '" + first.name + "' has no value: the game stops reading the stylesheet here, "
					     "and every variable after it stays undefined");
				} else {
					pending = Pending::Value; // the value starts on a later line
				}
			} else {
				// The '#' arm comes before the value: a directive runs, and the value is the
				// next text the reader reads; any other '#' it never leaves [orig: @
				// 0x639944..0x639a38, @ 0x639a11].
				if (*s == '#') {
					if (const char *directive = directive_named(s))
						diag(line, Severity::Error, DiagnosticCode::ValueIsDirective,
						     "'" + first.name + "' has no value on its line: the game runs '#" + directive +
						             "' as a directive and takes the next text it reads as the value, so the lines "
						             "below are not read as shown");
					else
						diag(line, Severity::Error, DiagnosticCode::ValueStartsWithHash,
						     "the game stops responding on a value that starts with '#' (write colors as AARRGGBB)");
				}
				pending = scan_value(s, line_end, first, line) ? Pending::Continuation : Pending::None;
			}
			p = line_end;
			read_eol(first.eol, line);
			node.define_lines.push_back(std::move(first));
			doc.nodes_.push_back(std::move(node));
			if (pending == Pending::None && !doc.nodes_.back().define_lines.front().sep_ws.empty()) finish_define();
		}
		++line_no;
	}

	if (pending == Pending::Value) {
		const Node &node = doc.nodes_.back();
		diag(node.line, Severity::Warning, DiagnosticCode::NoValue,
		     "the file ends before '" + define_name(node) + "' has a value: the game ignores it");
	} else if (pending == Pending::Continuation) {
		diag(line_no - 1, Severity::Warning, DiagnosticCode::ContinuationAtEof, "line continuation at end of file");
		finish_define();
	}
	for (const Open &frame : open) {
		diag(frame.line, Severity::Warning, DiagnosticCode::UnterminatedIf,
		     "'#if' is never closed: the rest of the file stays inside it");
	}
	return doc;
}

std::vector<uint8_t> Document::serialize() const {
	std::string s = source_text();
	std::vector<uint8_t> out;
	out.reserve(s.size() + 3);
	if (has_bom_) {
		out.push_back(0xEF);
		out.push_back(0xBB);
		out.push_back(0xBF);
	}
	out.insert(out.end(), s.begin(), s.end());
	return out;
}

std::string Document::source_text() const {
	std::vector<const Node *> nodes;
	nodes.reserve(nodes_.size());
	for (const Node &node : nodes_) nodes.push_back(&node);
	return render_nodes(nodes, default_eol_);
}

void Document::set_source_text(const std::string &text) {
	const bool had_bom = has_bom_;
	*this = parse(text);
	has_bom_ = has_bom_ || had_bom;
}

EvaluationResult Document::evaluate() const {
	EvaluationResult result;
	result.diagnostics = diagnostics_;
	const std::vector<uint8_t> bytes = serialize();
	const char *text = reinterpret_cast<const char *>(bytes.data());
	KeyValueList list;
	const ReadResult read = parse_key_value_buffer(text, bytes.size(), list);
	result.sheet = list.sheet();
	if (read.status == ReadStatus::Read) return result;
	result.success = false;
	result.hangs = read.status == ReadStatus::Hangs;
	result.stopped_line = line_at_offset(text, bytes.size(), read.offset);
	// The first error at or above the line says why; a stop no diagnostic explains is
	// reported on its own.
	for (const Diagnostic &d : diagnostics_)
		if (d.severity == Severity::Error && d.line <= result.stopped_line) return result;
	result.diagnostics.push_back(Diagnostic{
	        result.stopped_line, Severity::Error,
	        result.hangs ? DiagnosticCode::Hangs : DiagnosticCode::Stops,
	        result.hangs ? "the game stops responding reading this line"
	                     : "the game stops reading the stylesheet on this line, and every variable after it stays undefined"});
	return result;
}

StyleSheet Document::flatten() const {
	return evaluate().sheet;
}

std::vector<Document::Entry> Document::entries() const {
	std::vector<Entry> out;
	int group = 0;
	bool seen_define = false;
	bool pending_break = false;
	for (size_t i = 0; i < nodes_.size(); ++i) {
		const Node &node = nodes_[i];
		if (node.kind == NodeKind::Define) {
			if (seen_define && pending_break) ++group;
			pending_break = false;
			seen_define = true;
			Entry entry;
			entry.node_index = static_cast<int>(i);
			entry.line = node.line;
			entry.name = node.define_lines.front().name;
			entry.value = logical_value(node);
			entry.raw_value = raw_value(node);
			entry.inline_comment = node.define_lines.front().comment;
			entry.multiline = node.define_lines.size() > 1;
			entry.group = group;
			for (size_t j = i; j > 0 && nodes_[j - 1].kind == NodeKind::Comment; --j) {
				entry.preceding_comments.insert(entry.preceding_comments.begin(),
						nodes_[j - 1].text);
			}
			out.push_back(std::move(entry));
		} else if (node.kind != NodeKind::Comment) {
			// Blank lines, directives, and inactive text separate groups;
			// comments belong to the group they annotate.
			pending_break = true;
		}
	}
	return out;
}

int Document::find_entry(const std::string &name) const {
	const std::string upper = strutil::to_upper(name);
	const std::vector<Entry> all = entries();
	int found = -1;
	for (size_t i = 0; i < all.size(); ++i) {
		if (strutil::to_upper(all[i].name) == upper) found = static_cast<int>(i);
	}
	return found;
}

std::vector<int> Document::define_nodes_(const std::string &name) const {
	const std::string upper = strutil::to_upper(name);
	std::vector<int> found;
	for (size_t i = 0; i < nodes_.size(); ++i) {
		const Node &node = nodes_[i];
		if (node.kind == NodeKind::Define && strutil::to_upper(define_name(node)) == upper)
			found.push_back(static_cast<int>(i));
	}
	return found;
}

int Document::find_define_node_(const std::string &name) const {
	const std::vector<int> found = define_nodes_(name);
	return found.empty() ? -1 : found.back();
}

bool Document::commit_(const std::vector<Node> &proposed, std::string *error) {
	std::vector<const Node *> nodes;
	nodes.reserve(proposed.size());
	for (const Node &node : proposed) nodes.push_back(&node);
	switch (reread(nodes, default_eol_)) {
	case Reread::Same: break;
	case Reread::Inactive:
		set_error(error, "position is inside a switched-off #if block");
		return false;
	case Reread::Continued:
		set_error(error, "the line above continues onto this position: the game would read it as part of that value");
		return false;
	case Reread::Changed:
		set_error(error, "the game would read the lines around this edit differently");
		return false;
	}
	const bool bom = has_bom_;
	*this = parse(render_nodes(nodes, default_eol_));
	has_bom_ = bom;
	return true;
}

bool Document::set_value(const std::string &name, const std::string &value, std::string *error) {
	const std::string v = trim_hws(value);
	if (!is_valid_value(v)) {
		set_error(error, "value cannot be empty, contain newlines or '//', or start with '#'");
		return false;
	}
	const int ni = find_define_node_(name);
	if (ni < 0) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	std::vector<Node> proposed = nodes_;
	if (!set_define_chunk(proposed[size_t(ni)], escape_value(v), error)) return false;
	return commit_(proposed, error);
}

namespace {

// "defined N times (lines a, b): ..." for a name-addressed edit that needs one definition.
std::string defined_more_than_once(const std::string &name, const std::vector<int> &nodes,
		const std::vector<Node> &all) {
	std::string lines;
	for (int index : nodes) lines += (lines.empty() ? "" : ", ") + std::to_string(all[size_t(index)].line);
	return "'" + name + "' is defined " + std::to_string(nodes.size()) + " times (lines " + lines +
	       "): remove the extra definitions first";
}

} // namespace

bool Document::rename_define(const std::string &name, const std::string &new_name, std::string *error) {
	const std::string nn = trim_hws(new_name);
	if (!is_valid_name(nn)) {
		set_error(error, "'" + nn + "' is not a valid name (no spaces or % < > # \\ /)");
		return false;
	}
	const std::vector<int> matches = define_nodes_(name);
	if (matches.empty()) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	if (matches.size() > 1) {
		set_error(error, defined_more_than_once(name, matches, nodes_));
		return false;
	}
	const int ni = matches.front();
	const int existing = find_define_node_(nn);
	if (existing >= 0 && existing != ni) {
		set_error(error, "a variable named '" + nn + "' already exists");
		return false;
	}
	std::vector<Node> proposed = nodes_;
	if (!set_define_name(proposed[size_t(ni)], nn, error)) return false;
	return commit_(proposed, error);
}

bool Document::add_define(const std::string &name, const std::string &value,
		int before_node_index, const std::string &inline_comment, std::string *error) {
	const std::string nn = trim_hws(name);
	const std::string v = trim_hws(value);
	if (!is_valid_name(nn)) {
		set_error(error, "'" + nn + "' is not a valid name (no spaces or % < > # \\ /)");
		return false;
	}
	if (!is_valid_value(v)) {
		set_error(error, "value cannot be empty, contain newlines or '//', or start with '#'");
		return false;
	}
	if (contains_newline(inline_comment)) {
		set_error(error, "comment cannot contain newlines");
		return false;
	}
	if (find_define_node_(nn) >= 0) {
		set_error(error, "a variable named '" + nn + "' already exists");
		return false;
	}
	if (before_node_index < -1 || before_node_index > static_cast<int>(nodes_.size())) {
		set_error(error, "insert position out of range");
		return false;
	}
	const size_t pos = (before_node_index < 0 ||
			before_node_index >= static_cast<int>(nodes_.size()))
			? nodes_.size()
			: static_cast<size_t>(before_node_index);
	std::vector<Node> proposed = nodes_;
	proposed.insert(proposed.begin() + static_cast<std::ptrdiff_t>(pos),
			make_define(nn, escape_value(v), inline_comment, default_eol_));
	return commit_(proposed, error);
}

bool Document::remove_define(const std::string &name, std::string *error) {
	const std::vector<int> matches = define_nodes_(name);
	if (matches.empty()) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	for (int index : matches) {
		if (crosses_structure(nodes_[size_t(index)])) {
			set_error(error, "cannot remove macro '" + name +
					"': its value crosses other lines (conditional, inactive, blank or comment)");
			return false;
		}
	}
	std::vector<Node> proposed = nodes_;
	for (auto it = matches.rbegin(); it != matches.rend(); ++it)
		proposed.erase(proposed.begin() + static_cast<std::ptrdiff_t>(*it));
	return commit_(proposed, error);
}

bool Document::move_define(const std::string &name, int before_node_index, std::string *error) {
	const std::vector<int> matches = define_nodes_(name);
	if (matches.empty()) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	if (matches.size() > 1) {
		set_error(error, defined_more_than_once(name, matches, nodes_));
		return false;
	}
	const int ni = matches.front();
	if (before_node_index < 0 || before_node_index > static_cast<int>(nodes_.size())) {
		set_error(error, "move position out of range");
		return false;
	}
	if (before_node_index == ni || before_node_index == ni + 1) {
		return true; // already there
	}
	if (crosses_structure(nodes_[size_t(ni)])) {
		set_error(error, "cannot move macro '" + name +
				"': its value crosses other lines (conditional, inactive, blank or comment)");
		return false;
	}
	std::vector<Node> proposed = nodes_;
	Node node = std::move(proposed[size_t(ni)]);
	proposed.erase(proposed.begin() + ni);
	int target = before_node_index;
	if (target > ni) --target;
	proposed.insert(proposed.begin() + target, std::move(node));
	return commit_(proposed, error);
}

bool Document::set_inline_comment(const std::string &name, const std::string &comment, std::string *error) {
	const int ni = find_define_node_(name);
	if (ni < 0) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	std::vector<Node> proposed = nodes_;
	if (!set_define_comment(proposed[size_t(ni)], comment, error)) return false;
	return commit_(proposed, error);
}

}  // namespace opennova::mns
