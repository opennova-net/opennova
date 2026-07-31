#include "mns/mns_document.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace mns {

namespace {

bool is_hws(char c) {
	return c == ' ' || c == '\t';
}

std::string to_upper(const std::string &s) {
	std::string result = s;
	std::transform(result.begin(), result.end(), result.begin(),
				   [](unsigned char c) { return std::toupper(c); });
	return result;
}

// The spec's six forbidden name characters (the quotes in the in-file spec
// delimit the set): % < > # \ /
bool is_invalid_name_char(char c) {
	return c == '%' || c == '<' || c == '>' || c == '#' || c == '\\' || c == '/';
}

std::string trim_hws(const std::string &s) {
	size_t b = 0;
	while (b < s.size() && is_hws(s[b])) ++b;
	size_t e = s.size();
	while (e > b && is_hws(s[e - 1])) --e;
	return s.substr(b, e - b);
}

void rtrim_hws(std::string &s) {
	while (!s.empty() && is_hws(s.back())) s.pop_back();
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

// Authored chunk -> logical: "\\" collapses to '\'; a bare '\' stays (it is
// surfaced as a lone-backslash diagnostic at parse, but the legacy parser kept
// it literal and flatten must match).
void append_unescaped(std::string &out, const std::string &chunk) {
	for (size_t i = 0; i < chunk.size(); ++i) {
		if (chunk[i] == '\\' && i + 1 < chunk.size() && chunk[i + 1] == '\\') {
			out += '\\';
			++i;
			continue;
		}
		out += chunk[i];
	}
}

// Joined logical value of a define: concatenated unescaped chunks (continued
// lines keep their pre-backslash whitespace), right-trimmed as a whole --
// exactly the legacy read_value() result.
std::string logical_value(const Node &node) {
	std::string joined;
	for (const DefineLine &dl : node.define_lines) {
		if (!dl.contributes_value) continue;
		append_unescaped(joined, dl.chunk);
	}
	rtrim_hws(joined);
	return joined;
}

// Retail runtime value. parse_key_value_buffer scans over a doubled
// backslash so it cannot be mistaken for a continuation, but later copies the
// original contiguous source range; it does not collapse the pair.
// [orig: parse_key_value_buffer @ 0x639bba, scan @ 0x639c0e]
std::string retail_value(const Node &node) {
	std::string joined;
	for (const DefineLine &dl : node.define_lines) {
		if (!dl.contributes_value) continue;
		joined += dl.chunk;
	}
	rtrim_hws(joined);
	return joined;
}

std::string raw_value(const Node &node) {
	std::string joined;
	for (const DefineLine &dl : node.define_lines) {
		if (!dl.contributes_value) continue;
		joined += dl.chunk;
	}
	return joined;
}

// "" stays empty; "//..." kept verbatim; plain text gains a "// " prefix.
std::string normalize_comment(const std::string &text) {
	if (text.empty()) return std::string();
	if (text.size() >= 2 && text[0] == '/' && text[1] == '/') return text;
	return "// " + text;
}

bool contains_newline(const std::string &s) {
	return s.find('\n') != std::string::npos || s.find('\r') != std::string::npos;
}

void set_error(std::string *error, const std::string &message) {
	if (error != nullptr) *error = message;
}

} // namespace

bool Document::is_valid_name(const std::string &name) {
	if (name.empty()) return false;
	for (char c : name) {
		if (is_hws(c) || c == '\n' || c == '\r' || is_invalid_name_char(c)) return false;
	}
	return true;
}

bool Document::is_valid_value(const std::string &value) {
	if (contains_newline(value)) return false;
	if (value.find("//") != std::string::npos) return false; // would parse as a comment
	return true;
}

Document Document::parse(const std::string &text) {
	return parse(text.data(), text.size());
}

Document Document::parse(const char *data, size_t size) {
	Document doc;
	const char *p = data;
	const char *end = data + size;

	if (size >= 3 && static_cast<uint8_t>(p[0]) == 0xEF &&
		static_cast<uint8_t>(p[1]) == 0xBB && static_cast<uint8_t>(p[2]) == 0xBF) {
		doc.has_bom_ = true;
		p += 3;
	}

	// The retail loader uses the first argument character as its condition:
	// leading '0' is false, every other spelling is true. A stack keeps nested
	// inactive regions and #else branches independent.
	struct ConditionalFrame {
		int line = 0;
		bool parent_active = true;
		bool condition = true;
		bool in_else = false;
	};
	std::vector<ConditionalFrame> conditionals;
	bool active = true;

	int line_no = 1;
	std::unordered_map<std::string, int> seen; // uppercase active name -> first line

	auto diag = [&](int line, Severity sev, const char *code, const std::string &message) {
		doc.diagnostics_.push_back(Diagnostic{line, sev, code, message});
	};

	auto read_eol = [&](std::string &out_eol) {
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
		if (!out_eol.empty() && !doc.eol_seen_) {
			doc.eol_seen_ = true;
			doc.default_eol_ = out_eol;
		}
	};

	auto body_to_eol = [&]() {
		const char *start = p;
		while (p < end && *p != '\n' && *p != '\r') ++p;
		return std::string(start, p);
	};

	auto line_end_from = [&](const char *start) {
		const char *line_end = start;
		while (line_end < end && *line_end != '\n' && *line_end != '\r') ++line_end;
		return line_end;
	};

	auto find_hash = [&](const char *start, const char *line_end) {
		const char *hash = start;
		while (hash < line_end && *hash != '#') ++hash;
		return hash < line_end ? hash : nullptr;
	};

	// Apply one directive whose '#' may be embedded in otherwise inactive source.
	// Retail's false-branch scanner seeks the next '#' byte rather than requiring
	// a line start [orig: parse_key_value_buffer @ 0x639870].
	auto apply_directive = [&](const char *hash, int directive_line,
			DirectiveKind &kind, std::string &arg) {
		const char *line_end = line_end_from(hash);
		const char *tok_start = hash + 1;
		const char *t = tok_start;
		while (t < line_end && !is_hws(*t)) ++t;
		const std::string tok(tok_start, t);

		if (tok == "if") {
			kind = DirectiveKind::If;
			const char *a = t;
			while (a < line_end && is_hws(*a)) ++a;
			const char *arg_start = a;
			while (a < line_end && !is_hws(*a)) ++a;
			arg.assign(arg_start, a);
			if (arg != "0" && arg != "1") {
				diag(directive_line, Severity::Warning, "noncanonical-if-arg",
						"'#if' uses its first character; canonical arguments are 0 or 1");
			}
			const bool condition = arg.empty() || arg.front() != '0';
			conditionals.push_back(
					ConditionalFrame{directive_line, active, condition, false});
			active = active && condition;
		} else if (tok == "else") {
			kind = DirectiveKind::Else;
			if (conditionals.empty()) {
				diag(directive_line, Severity::Error, "unbalanced-else",
						"'#else' without a matching '#if'");
			} else {
				ConditionalFrame &frame = conditionals.back();
				if (frame.in_else) {
					diag(directive_line, Severity::Error, "duplicate-else",
							"'#if' block has more than one '#else'");
				} else {
					frame.in_else = true;
					active = frame.parent_active && !frame.condition;
				}
			}
		} else if (tok == "endif") {
			kind = DirectiveKind::Endif;
			if (conditionals.empty()) {
				diag(directive_line, Severity::Error, "unbalanced-endif",
						"'#endif' without a matching '#if'");
			} else {
				active = conditionals.back().parent_active;
				conditionals.pop_back();
			}
		} else {
			kind = DirectiveKind::Unknown;
			diag(directive_line, Severity::Error, "unknown-directive",
					"unknown stylesheet directive '#" + tok + "'");
		}
	};

	// Scan one value segment from p to its EOL. Fills chunk/comment/whitespace
	// fields; returns true when the segment ends in a continuation backslash.
	auto scan_segment = [&](DefineLine &out) -> bool {
		const char *start = p;
		const char *comment_start = nullptr;
		const char *backslash = nullptr;
		while (p < end && *p != '\n' && *p != '\r') {
			if (p + 1 < end && p[0] == '/' && p[1] == '/') {
				comment_start = p;
				break;
			}
			if (*p == '\\') {
				if (p + 1 < end && p[1] == '\\') { // "\\" escape stays in the chunk
					p += 2;
					continue;
				}
				// Continuation when only whitespace separates the backslash
				// from EOL/EOF or an inline comment (the spec's "or the last
				// character before a continuation backslash" parenthetical).
				const char *j = p + 1;
				while (j < end && is_hws(*j)) ++j;
				const bool continuation = (j >= end || *j == '\n' || *j == '\r' ||
						(j + 1 < end && j[0] == '/' && j[1] == '/'));
				if (continuation) {
					backslash = p;
					break;
				}
				diag(line_no, Severity::Error, "lone-backslash",
						"value contains a bare '\\' (escape it as '\\\\')");
				++p;
				continue;
			}
			++p;
		}
		if (backslash != nullptr) {
			out.chunk.assign(start, backslash); // pre-backslash whitespace kept (spec)
			out.continued = true;
			p = backslash + 1;
			const char *ws_start = p;
			while (p < end && is_hws(*p)) ++p;
			out.post_backslash_ws.assign(ws_start, p);
			if (p + 1 < end && p[0] == '/' && p[1] == '/') {
				out.comment = body_to_eol();
			}
			return true;
		}
		const char *seg_end = (comment_start != nullptr) ? comment_start : p;
		const char *chunk_end = seg_end;
		while (chunk_end > start && is_hws(chunk_end[-1])) --chunk_end;
		out.chunk.assign(start, chunk_end);
		out.pre_comment_ws.assign(chunk_end, seg_end);
		if (comment_start != nullptr) {
			p = comment_start;
			out.comment = body_to_eol();
		}
		return false;
	};

	while (p < end) {
		Node node;
		node.line = line_no;

		const char *ws_start = p;
		while (p < end && is_hws(*p)) ++p;
		std::string leading_ws(ws_start, p);

		// Whitespace-only line (or trailing whitespace at EOF).
		if (p >= end || *p == '\n' || *p == '\r') {
			node.kind = NodeKind::Blank;
			node.leading_ws = leading_ws;
			read_eol(node.eol);
			doc.nodes_.push_back(std::move(node));
			++line_no;
			continue;
		}

		// Full-line comment (active regions only; inactive ones are plain
		// skipped text to the runtime and stay InactiveText here).
		if (active && p + 1 < end && p[0] == '/' && p[1] == '/') {
			node.kind = NodeKind::Comment;
			node.leading_ws = leading_ws;
			node.text = body_to_eol();
			read_eol(node.eol);
			doc.nodes_.push_back(std::move(node));
			++line_no;
			continue;
		}

		// In active source a directive starts at the first non-whitespace byte.
		// In inactive source retail scans forward to the next '#' even when it is
		// not line-leading.
		const char *directive_hash = *p == '#'
				? p
				: (!active ? find_hash(p, line_end_from(p)) : nullptr);
		if (directive_hash != nullptr) {
			node.kind = NodeKind::Directive;
			node.leading_ws = leading_ws;
			node.text = body_to_eol(); // whole body; inactive scans may have a prefix before '#'
			apply_directive(directive_hash, node.line, node.directive,
					node.directive_arg);
			read_eol(node.eol);
			doc.nodes_.push_back(std::move(node));
			++line_no;
			continue;
		}

		// Inside an evaluated-false region: preserve the line verbatim. The
		// runtime skips these line-by-line, so no define structure is imposed
		// (see ADR 0014 on why this is not "raw passthrough").
		if (!active) {
			node.kind = NodeKind::InactiveText;
			node.leading_ws = leading_ws;
			node.text = body_to_eol();
			read_eol(node.eol);
			doc.nodes_.push_back(std::move(node));
			++line_no;
			continue;
		}

		// Define: NAME [value], possibly spanning lines via continuations.
		node.kind = NodeKind::Define;
		DefineLine first;
		first.leading_ws = leading_ws;
		const char *name_start = p;
		while (p < end && !is_hws(*p) && *p != '\n' && *p != '\r') ++p;
		first.name.assign(name_start, p);
		for (char c : first.name) {
			if (is_invalid_name_char(c)) {
				diag(node.line, Severity::Error, "invalid-name-char",
						"macro name '" + first.name + "' contains an invalid character ('" +
								std::string(1, c) + "')");
				break;
			}
		}
		const char *sep_start = p;
		while (p < end && is_hws(*p)) ++p;
		first.sep_ws.assign(sep_start, p);
		if (first.sep_ws.empty()) {
			diag(node.line, Severity::Error, "missing-value-delimiter",
					"macro '" + first.name +
							"' has no whitespace delimiter before its value");
		}

		bool continued = scan_segment(first);
		read_eol(first.eol);
		node.define_lines.push_back(std::move(first));
		++line_no;

		// A pending continuation skips directive and inactive physical lines,
		// then resumes with the next ordinary active line. Those crossed lines
		// remain byte-owned by this Define node so serialization is still in
		// physical order, but they do not contribute to the evaluated value.
		while (continued && p < end) {
			DefineLine cont;
			const char *cont_ws = p;
			while (p < end && is_hws(*p)) ++p;
			cont.leading_ws.assign(cont_ws, p);
			const char *physical_end = line_end_from(p);
			const char *hash = active
					? (*p == '#' ? p : nullptr)
					: find_hash(p, physical_end);
			if (!active || hash != nullptr) {
				cont.contributes_value = false;
				const char *body_start = p;
				cont.chunk.assign(body_start, physical_end);
				p = physical_end;
				if (hash != nullptr) {
					DirectiveKind crossed_kind = DirectiveKind::Unknown;
					std::string crossed_arg;
					apply_directive(hash, line_no, crossed_kind, crossed_arg);
				}
				// A directive/inactive line does not terminate the pending
				// continuation, regardless of whether its own source ends in '\'.
				continued = true;
			} else {
				continued = scan_segment(cont);
			}
			read_eol(cont.eol);
			node.define_lines.push_back(std::move(cont));
			++line_no;
		}
		if (continued) {
			diag(line_no - 1, Severity::Warning, "continuation-at-eof",
					"line continuation at end of file");
		}

		const std::string &name = node.define_lines.front().name;
		const std::string upper = to_upper(name);
		if (logical_value(node).empty()) {
			diag(node.line, Severity::Warning, "empty-value",
					"macro '" + name + "' has an empty value");
		}
		auto it = seen.find(upper);
		if (it != seen.end()) {
			diag(node.line, Severity::Warning, "duplicate-name",
					"duplicate macro name '" + name + "' (first defined at line " +
							std::to_string(it->second) + ")");
		} else {
			seen.emplace(upper, node.line);
		}
		doc.nodes_.push_back(std::move(node));
	}

	for (const ConditionalFrame &frame : conditionals) {
		diag(frame.line, Severity::Error, "unterminated-if",
				"'#if' block not closed before end of file");
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
	std::string s;
	for (const Node &node : nodes_) {
		switch (node.kind) {
			case NodeKind::Blank:
				s += node.leading_ws;
				s += node.eol;
				break;
			case NodeKind::Comment:
			case NodeKind::Directive:
			case NodeKind::InactiveText:
				s += node.leading_ws;
				s += node.text;
				s += node.eol;
				break;
			case NodeKind::Define:
				for (const DefineLine &dl : node.define_lines) {
					s += dl.leading_ws;
					s += dl.name;
					s += dl.sep_ws;
					s += dl.chunk;
					if (dl.continued) {
						s += '\\';
						s += dl.post_backslash_ws;
					} else {
						s += dl.pre_comment_ws;
					}
					s += dl.comment;
					s += dl.eol;
				}
				break;
		}
	}
	return s;
}

void Document::set_source_text(const std::string &text) {
	const bool had_bom = has_bom_;
	*this = parse(text);
	has_bom_ = has_bom_ || had_bom;
}

EvaluationResult Document::evaluate() const {
	EvaluationResult result;
	result.diagnostics = diagnostics_;
	for (const Node &node : nodes_) {
		if (node.kind != NodeKind::Define) continue;
		result.sheet.variables[to_upper(node.define_lines.front().name)] =
				retail_value(node);
	}
	for (const Diagnostic &diagnostic : result.diagnostics) {
		if (diagnostic.severity == Severity::Error) {
			result.success = false;
			break;
		}
	}
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
	const std::string upper = to_upper(name);
	const std::vector<Entry> all = entries();
	int found = -1;
	for (size_t i = 0; i < all.size(); ++i) {
		if (to_upper(all[i].name) == upper) found = static_cast<int>(i);
	}
	return found;
}

int Document::find_define_node_(const std::string &name) const {
	const std::string upper = to_upper(name);
	int found = -1;
	for (size_t i = 0; i < nodes_.size(); ++i) {
		const Node &node = nodes_[i];
		if (node.kind == NodeKind::Define &&
				to_upper(node.define_lines.front().name) == upper) {
			found = static_cast<int>(i);
		}
	}
	return found;
}

void Document::refresh_() {
	const std::string eol = default_eol_;
	const bool seen = eol_seen_;
	const std::vector<uint8_t> bytes = serialize();
	*this = parse(reinterpret_cast<const char *>(bytes.data()), bytes.size());
	if (!eol_seen_ && seen) {
		default_eol_ = eol;
		eol_seen_ = true;
	}
}

bool Document::set_value(const std::string &name, const std::string &value, std::string *error) {
	const std::string v = trim_hws(value);
	if (!is_valid_value(v)) {
		set_error(error, "value cannot contain newlines or '//'");
		return false;
	}
	const int ni = find_define_node_(name);
	if (ni < 0) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	Node &node = nodes_[ni];
	if (node.define_lines.size() == 1) {
		node.define_lines[0].chunk = escape_value(v);
	} else if (std::any_of(node.define_lines.begin(), node.define_lines.end(),
					   [](const DefineLine &line) {
						   return !line.contributes_value;
					   })) {
		// Preserve crossed directives/inactive source in place. Put the edited
		// logical value on the first real value line and empty the remaining real
		// segments; their continuation structure/comments/EOLs remain intact.
		bool wrote = false;
		for (DefineLine &line : node.define_lines) {
			if (!line.contributes_value) continue;
			line.chunk = wrote ? std::string() : escape_value(v);
			wrote = true;
		}
	} else {
		// Collapse to one line. Layout comes from the first line; the EOL from
		// the last (preserves a document-final missing newline). Inline
		// comments from all spanned lines coalesce so no text is lost.
		const DefineLine &first = node.define_lines.front();
		const DefineLine &last = node.define_lines.back();
		std::string combined_comment;
		for (const DefineLine &dl : node.define_lines) {
			if (dl.comment.empty()) continue;
			if (combined_comment.empty()) {
				combined_comment = dl.comment;
			} else {
				std::string body = dl.comment;
				size_t b = 0;
				while (b < body.size() && body[b] == '/') ++b;
				while (b < body.size() && is_hws(body[b])) ++b;
				combined_comment += " " + body.substr(b);
			}
		}
		DefineLine collapsed;
		collapsed.leading_ws = first.leading_ws;
		collapsed.name = first.name;
		collapsed.sep_ws = first.sep_ws;
		collapsed.chunk = escape_value(v);
		collapsed.comment = combined_comment;
		collapsed.pre_comment_ws = combined_comment.empty()
				? std::string()
				: (first.pre_comment_ws.empty() ? std::string("\t") : first.pre_comment_ws);
		collapsed.eol = last.eol;
		node.define_lines.clear();
		node.define_lines.push_back(std::move(collapsed));
	}
	refresh_();
	return true;
}

bool Document::rename_define(const std::string &name, const std::string &new_name, std::string *error) {
	const std::string nn = trim_hws(new_name);
	if (!is_valid_name(nn)) {
		set_error(error, "'" + nn + "' is not a valid name (no spaces or % < > # \\ /)");
		return false;
	}
	const int ni = find_define_node_(name);
	if (ni < 0) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	const int existing = find_define_node_(nn);
	if (existing >= 0 && existing != ni) {
		set_error(error, "a variable named '" + nn + "' already exists");
		return false;
	}
	nodes_[ni].define_lines.front().name = nn;
	refresh_();
	return true;
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
		set_error(error, "value cannot contain newlines or '//'");
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

	Node node;
	node.kind = NodeKind::Define;
	DefineLine dl;
	dl.name = nn;
	dl.sep_ws = "\t";
	dl.chunk = escape_value(v);
	dl.comment = normalize_comment(inline_comment);
	dl.pre_comment_ws = dl.comment.empty() ? std::string() : std::string("\t");
	dl.eol = default_eol_;
	node.define_lines.push_back(std::move(dl));

	const size_t pos = (before_node_index < 0 ||
			before_node_index >= static_cast<int>(nodes_.size()))
			? nodes_.size()
			: static_cast<size_t>(before_node_index);
	if (pos == nodes_.size() && !nodes_.empty()) {
		// Appending below a final line that has no EOL first terminates it.
		Node &last = nodes_.back();
		std::string &last_eol = (last.kind == NodeKind::Define)
				? last.define_lines.back().eol
				: last.eol;
		if (last_eol.empty()) last_eol = default_eol_;
	}
	nodes_.insert(nodes_.begin() + static_cast<long>(pos), std::move(node));
	refresh_();
	return true;
}

bool Document::remove_define(const std::string &name, std::string *error) {
	const std::string upper = to_upper(name);
	for (const Node &node : nodes_) {
		if (node.kind != NodeKind::Define ||
				to_upper(node.define_lines.front().name) != upper) {
			continue;
		}
		if (std::any_of(node.define_lines.begin(), node.define_lines.end(),
					[](const DefineLine &line) {
						return !line.contributes_value;
					})) {
			set_error(error, "cannot remove macro '" + name +
					"': its continuation crosses conditional or inactive source");
			return false;
		}
	}
	bool removed = false;
	for (size_t i = nodes_.size(); i > 0; --i) {
		const Node &node = nodes_[i - 1];
		if (node.kind == NodeKind::Define &&
				to_upper(node.define_lines.front().name) == upper) {
			nodes_.erase(nodes_.begin() + static_cast<long>(i - 1));
			removed = true;
		}
	}
	if (!removed) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	refresh_();
	return true;
}

bool Document::move_define(const std::string &name, int before_node_index, std::string *error) {
	const int ni = find_define_node_(name);
	if (ni < 0) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	if (before_node_index < 0 || before_node_index > static_cast<int>(nodes_.size())) {
		set_error(error, "move position out of range");
		return false;
	}
	if (before_node_index == ni || before_node_index == ni + 1) {
		return true; // already there
	}
	if (std::any_of(nodes_[ni].define_lines.begin(),
				nodes_[ni].define_lines.end(), [](const DefineLine &line) {
					return !line.contributes_value;
				})) {
		set_error(error, "cannot move macro '" + name +
				"': its continuation crosses conditional or inactive source");
		return false;
	}
	Node node = std::move(nodes_[ni]);
	nodes_.erase(nodes_.begin() + ni);
	int target = before_node_index;
	if (target > ni) --target;
	nodes_.insert(nodes_.begin() + target, std::move(node));
	// Every line except the document's last must end with an EOL.
	for (size_t i = 0; i + 1 < nodes_.size(); ++i) {
		Node &n = nodes_[i];
		std::string &eol = (n.kind == NodeKind::Define) ? n.define_lines.back().eol : n.eol;
		if (eol.empty()) eol = default_eol_;
	}
	refresh_();
	return true;
}

bool Document::set_inline_comment(const std::string &name, const std::string &comment, std::string *error) {
	if (contains_newline(comment)) {
		set_error(error, "comment cannot contain newlines");
		return false;
	}
	const int ni = find_define_node_(name);
	if (ni < 0) {
		set_error(error, "no variable named '" + name + "'");
		return false;
	}
	DefineLine &dl = nodes_[ni].define_lines.front();
	dl.comment = normalize_comment(comment);
	if (dl.continued) {
		if (!dl.comment.empty() && dl.post_backslash_ws.empty()) dl.post_backslash_ws = " ";
	} else {
		dl.pre_comment_ws = dl.comment.empty()
				? std::string()
				: (dl.pre_comment_ws.empty() ? std::string("\t") : dl.pre_comment_ws);
	}
	refresh_();
	return true;
}

} // namespace mns
