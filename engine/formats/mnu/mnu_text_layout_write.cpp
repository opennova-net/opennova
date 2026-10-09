// The generation of a document in its text layout (mnu_text_layout.h, which states the rules):
// every byte comes from the records (the writer's words, mnu_write.h) and the modeled layout. A
// token keeps the file's spelling while the writer's token is the one it wrote for the record as
// read, else it is the writer's own; a record the layout does not hold is the writer's own layout
// in the file's style.
#include "mnu_text_layout.h"

#include <formats/mnu/mnu_xml.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

namespace opennova::mnu {

namespace {

using Pieces = std::vector<NotedPiece>;

std::string upper(std::string s) {
	for (char &c : s)
		if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
	return s;
}

bool starts_with(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

// The number a token reads as [orig: CRT_wcstoxl @ 0x76e93b, mnu_xml::wcstol].
int number(const std::string &token) {
	return static_cast<int>(mnu_xml::wcstol(mnu_xml::widen(token.c_str()), 10));
}

// The value token the reader takes from an attribute spelled `=value` (NotedAttribute::value): the
// raw value it stores (a quoted one up to its first quote of either kind, that quote kept; an
// unquoted one up to a blank, '>' or '"'), then the first run of it that is not '"', as every
// consumer's wcstok(value, "\"") takes it [orig: NapiXML_ParseElementTree @ 0x769d70; the
// consumers' wcstok @ 0x76e883].
std::string spelled_token(const std::string &spelling) {
	if (spelling.empty() || spelling[0] != '=') return std::string();
	std::string raw;
	const char open = spelling.size() > 1 ? spelling[1] : '\0';
	if (open == '"' || open == '\'') {
		size_t q = 2;
		while (q < spelling.size() && spelling[q] != '"' && spelling[q] != '\'') ++q;
		raw = spelling.substr(2, q < spelling.size() ? q - 1 : std::string::npos);
	} else {
		size_t q = 1;
		while (q < spelling.size() && !mnu_xml::is_space(char32_t(static_cast<unsigned char>(spelling[q]))) &&
		       spelling[q] != '>' && spelling[q] != '"')
			++q;
		raw = spelling.substr(1, q - 1);
	}
	size_t i = 0;
	while (i < raw.size() && raw[i] == '"') ++i;
	size_t j = i;
	while (j < raw.size() && raw[j] != '"') ++j;
	return raw.substr(i, j - i);
}

// The blanks before an element: whether a line end is among them, the last one, and the blanks
// after it (the element's indentation).
struct Lead {
	bool broken = false;
	std::string eol;
	std::string indent;
	bool blanks = true; // nothing but blanks
};

Lead lead_of(const std::string &text) {
	Lead lead;
	lead.blanks = std::all_of(text.begin(), text.end(),
	                          [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
	const size_t nl = text.rfind('\n');
	if (nl == std::string::npos) return lead;
	lead.broken = true;
	lead.eol = nl > 0 && text[nl - 1] == '\r' ? "\r\n" : "\n";
	size_t k = nl + 1;
	while (k < text.size() && (text[k] == ' ' || text[k] == '\t')) ++k;
	lead.indent = text.substr(nl + 1, k - nl - 1);
	return lead;
}

// An attribute as it will be written.
struct Token {
	std::string text;  // its blanks, name and value
	std::string key;   // what it is (WrittenAttribute::key, NotedAttribute::key)
	std::string name;  // its name, upper-cased
	std::string value; // the value token the reader takes from it
	bool written = false; // one of the writer's attributes (an anchor for those it adds)
};

Token verbatim(const NotedAttribute &a) {
	return {a.gap + a.name + a.value, a.key, upper(a.name), spelled_token(a.value), false};
}

Token written_token(const WrittenAttribute &a) {
	return {" " + a.name + a.value_token(), a.key, upper(a.name), a.value, true};
}

int sort_slot(const std::string &key) { return starts_with(key, "@SORT") && key.size() == 6 ? key[5] - '0' : -1; }

// What retail's COLUMN walk reads from a run of HEADER / BODY / SUBST attribute lists [orig:
// CTableWnd_ParseXMLContentDefinition @ 0x6427d0, the HEADER walk's stores @ 0x643240 ..
// 0x64329d; the running index, mnu.cpp's read_column]: each sort key the index the walk (last
// authored first) stands at, the last write winning; the running index the first COLUMN's.
struct SortRead {
	int value[3] = {-1, -1, -1};
	std::string name0; // the spelling of the primary key's last write
	bool operator==(const SortRead &o) const {
		return value[0] == o.value[0] && value[1] == o.value[1] && value[2] == o.value[2] &&
		       (value[0] < 0 || name0 == o.name0);
	}
};

void fold(SortRead &read, bool header, const std::vector<Token> &tokens, int &running) {
	if (header) {
		int walk = running;
		for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
			if (it->key == "COLUMN" && !it->value.empty()) walk = number(it->value);
			const int slot = sort_slot(it->key);
			if (slot < 0) continue;
			read.value[slot] = walk;
			if (slot == 0) read.name0 = it->name;
		}
	}
	for (const Token &t : tokens)
		if (t.key == "COLUMN") {
			if (!t.value.empty()) running = number(t.value);
			break;
		}
}

bool column_row(WrittenKind kind) {
	return kind == WrittenKind::Header || kind == WrittenKind::Body || kind == WrittenKind::Subst;
}

// A COLUMN's pass over its rows: the running index, which sort keys the file's tokens still carry,
// where the writer puts down those they do not, and each row's attributes as planned.
struct ColumnPass {
	int running = 0;
	bool keep[3] = {true, true, true};
	const WrittenElement *place_on[3] = {nullptr, nullptr, nullptr};
	bool own[3] = {true, true, true};
	std::string names[3];
	std::map<const WrittenElement *, std::vector<Token>> planned;
};

// Where an inserted attribute goes among the tokens: after the nearest of the writer's attributes
// before it in the writer's order that is there, else before the nearest after it, else at the end
// (before the blanks that close the tag).
void insert_token(std::vector<Token> &tokens, const WrittenElement &w, size_t index, Token token) {
	auto find = [&](const std::string &key) {
		for (size_t t = 0; t < tokens.size(); ++t)
			if (tokens[t].written && tokens[t].key == key) return long(t);
		return -1L;
	};
	for (size_t k = index; k-- > 0;) {
		const long at = find(w.attributes[k].key);
		if (at >= 0) {
			tokens.insert(tokens.begin() + at + 1, std::move(token));
			return;
		}
	}
	for (size_t k = index + 1; k < w.attributes.size(); ++k) {
		const long at = find(w.attributes[k].key);
		if (at >= 0) {
			tokens.insert(tokens.begin() + at, std::move(token));
			return;
		}
	}
	const bool closing_blanks = !tokens.empty() && tokens.back().key.empty() && tokens.back().name.empty() &&
	                            tokens.back().value.empty() && !tokens.back().written;
	tokens.insert(closing_blanks ? tokens.end() - 1 : tokens.end(), std::move(token));
}

// The sort keys the pass places on a row the writer puts down (before its first COLUMN for its own
// index, after its last for the index before it).
void place_sorts(const WrittenElement &h, std::vector<Token> &tokens, const ColumnPass &pass) {
	for (int s = 0; s < 3; ++s) {
		if (pass.place_on[s] != &h) continue;
		Token t{" " + pass.names[s], "@SORT" + std::to_string(s), upper(pass.names[s]), std::string(), true};
		long first = -1, last = -1;
		for (size_t i = 0; i < tokens.size(); ++i)
			if (tokens[i].key == "COLUMN") {
				if (first < 0) first = long(i);
				last = long(i);
			}
		if (first < 0) {
			const bool closing_blanks = !tokens.empty() && tokens.back().key.empty() && tokens.back().name.empty();
			tokens.insert(closing_blanks ? tokens.end() - 1 : tokens.end(), std::move(t));
		} else if (pass.own[s]) {
			tokens.insert(tokens.begin() + first, std::move(t));
		} else {
			tokens.insert(tokens.begin() + last + 1, std::move(t));
		}
	}
}

class Generator {
public:
	Generator(const TextLayout &layout, const WrittenStyle &canonical) : l_(layout), style_(canonical) {
		if (!layout.styled) {
			style_.eol.clear();
			style_.unit.clear();
		} else {
			style_.eol = layout.eol;
			if (layout.has_unit) style_.unit = layout.unit;
		}
	}
	std::string run(const std::vector<WrittenElement> &screens);

private:
	struct Occurrence {
		size_t member;
		size_t piece;
		uint32_t noted;
	};
	// How a writer's element's children stand in the contents it is written from.
	struct Placement {
		std::vector<std::vector<Occurrence>> of; // each child's occurrences, in file order
		std::vector<bool> whole;                 // every occurrence is written (else the first alone)
		std::vector<bool> moved;                 // held, but out of its list's order
		std::map<std::pair<size_t, size_t>, size_t> child_at;
	};
	// One thing a content writes, in order.
	struct Entry {
		enum class Kind : uint8_t { Text, Piece, Child, Insert } kind = Kind::Text;
		std::string text;                  // Text
		const NotedPiece *piece = nullptr; // Piece
		size_t child = 0;                  // Child, Insert
		uint32_t noted = UINT32_MAX;       // Child, a moved Insert
		std::string lead;                  // a new Insert: what stands before it
		std::string indent;                // Child, Insert: its line's indentation
	};
	struct Scope {
		const WrittenElement *w = nullptr; // null: the document
		const std::vector<WrittenElement> *children = nullptr;
		std::vector<const Pieces *> contents; // the contents it is written from, in file order
		size_t member = 0;                    // the one being written
		bool text_modeled = false;
		std::string text, as_text; // text_modeled: the writer's text and the text as read, escaped
		bool carried = false;      // a RAW_TEXT of it gave attributes away
		std::string indent;        // its line's indentation
		bool tied_ok = true;       // its Tied tokens are kept (it is written as read)
		bool top = false;
	};
	struct PositionSim {
		bool ready = false;
		std::map<std::string, uint32_t> last; // each edge's last element: the one that sets it
		int left = 0, top = 0;                // the edges as the reader stands
	};

	Placement place(const Scope &scope) const;
	std::vector<Entry> plan(const Scope &scope, const Placement &p) const;
	void emit(const Scope &scope, const std::vector<Entry> &entries, const Placement &p);
	void element(const WrittenElement &c, const std::vector<uint32_t> &members, size_t member, const std::string &indent);
	void column(const WrittenElement &c, uint32_t noted, const std::string &indent);
	void edge(const WrittenElement &position, const WrittenElement &e, uint32_t noted, const std::string &indent);
	std::vector<Token> attributes(const WrittenElement &c, const NotedElement &n, bool primary,
	                              const std::set<std::string> &elsewhere, ColumnPass *pass) const;
	std::vector<Token> new_attributes(const WrittenElement &c, ColumnPass *pass) const;
	void verbatim_element(const NotedElement &n);
	void new_element(const WrittenElement &c, const std::string &indent);
	SortRead plan_rows(const std::vector<Entry> &entries, const std::vector<WrittenElement> &rows, ColumnPass &pass) const;

	const TextLayout &l_;
	WrittenStyle style_;
	std::string out_;
	std::set<uint32_t> used_;
	ColumnPass *column_ = nullptr;
	std::map<const WrittenElement *, PositionSim> positions_;
	const WrittenElement *position_ = nullptr; // the POSITION whose edges are being written
};

Generator::Placement Generator::place(const Scope &s) const {
	const std::vector<WrittenElement> &children = *s.children;
	Placement p;
	p.of.resize(children.size());
	p.whole.assign(children.size(), true);
	p.moved.assign(children.size(), false);
	std::map<std::string, std::vector<Occurrence>> by_key;
	for (size_t m = 0; m < s.contents.size(); ++m) {
		const Pieces &pieces = *s.contents[m];
		for (size_t i = 0; i < pieces.size(); ++i) {
			if (pieces[i].kind != NotedPiece::Kind::Element) continue;
			const NotedElement &n = l_.elements[pieces[i].element];
			if (n.role == NotedRole::Written) by_key[n.key].push_back({m, i, pieces[i].element});
		}
	}
	for (size_t ci = 0; ci < children.size(); ++ci) {
		const WrittenElement &c = children[ci];
		const auto it = by_key.find(c.key);
		if (it == by_key.end()) continue; // a record the file does not hold (or its second copy)
		p.of[ci] = std::move(it->second);
		by_key.erase(it);
		for (const Occurrence &o : p.of[ci]) p.child_at[{o.member, o.piece}] = ci;
		p.whole[ci] = c.kind == WrittenKind::Position || c.kind == WrittenKind::Edge || p.of[ci].size() <= 1 ||
		              c.tree_digest == l_.elements[p.of[ci][0].noted].as_tree;
	}
	// Each list in the model's order: the longest run of its records the file holds in that order
	// stays in place, the others move.
	for (size_t m = 0; m < s.contents.size(); ++m) {
		std::map<std::string, std::vector<std::pair<size_t, size_t>>> lists;
		for (size_t ci = 0; ci < children.size(); ++ci)
			if (!children[ci].list.empty() && !p.of[ci].empty() && p.of[ci][0].member == m)
				lists[children[ci].list].emplace_back(ci, p.of[ci][0].piece);
		for (const auto &list : lists) {
			const auto &seq = list.second;
			std::vector<size_t> tails, from(seq.size(), SIZE_MAX), tail_at;
			for (size_t k = 0; k < seq.size(); ++k) {
				size_t lo = 0, hi = tails.size();
				while (lo < hi) {
					const size_t mid = (lo + hi) / 2;
					if (seq[tail_at[mid]].second < seq[k].second) lo = mid + 1;
					else hi = mid;
				}
				from[k] = lo > 0 ? tail_at[lo - 1] : SIZE_MAX;
				if (lo == tails.size()) {
					tails.push_back(seq[k].second);
					tail_at.push_back(k);
				} else {
					tails[lo] = seq[k].second;
					tail_at[lo] = k;
				}
			}
			std::vector<bool> keep(seq.size(), false);
			for (size_t k = tail_at.empty() ? SIZE_MAX : tail_at.back(); k != SIZE_MAX; k = from[k]) keep[k] = true;
			for (size_t k = 0; k < seq.size(); ++k)
				if (!keep[k]) p.moved[seq[k].first] = true;
		}
	}
	return p;
}

std::vector<Generator::Entry> Generator::plan(const Scope &s, const Placement &p) const {
	const Pieces &pieces = *s.contents[s.member];
	const std::vector<WrittenElement> &children = *s.children;
	const size_t n = pieces.size();
	// Which element pieces are written, and as which child.
	std::vector<bool> written(n, true);
	std::vector<long> child_of(n, -1);
	bool any_element = false;
	for (size_t i = 0; i < n; ++i) {
		if (pieces[i].kind != NotedPiece::Kind::Element) continue;
		any_element = true;
		const NotedElement &e = l_.elements[pieces[i].element];
		if (e.role == NotedRole::Written) {
			const auto it = p.child_at.find({s.member, i});
			if (it == p.child_at.end()) { // its record is gone
				written[i] = false;
				continue;
			}
			const size_t ci = it->second;
			const Occurrence &first = p.of[ci].front();
			const bool is_first = first.member == s.member && first.piece == i;
			written[i] = (p.whole[ci] || is_first) && !(is_first && p.moved[ci]);
			if (written[i]) child_of[i] = long(ci);
		} else {
			written[i] = e.role == NotedRole::Inert || s.tied_ok;
		}
	}
	// What goes with each element: its run of pieces.
	std::vector<std::vector<size_t>> starts(n), ends(n);
	{
		std::vector<size_t> first(n, SIZE_MAX), last(n, 0);
		for (size_t i = 0; i < n; ++i) {
			const long owner = pieces[i].kind == NotedPiece::Kind::Element ? long(i) : pieces[i].owner;
			if (owner < 0) continue;
			first[size_t(owner)] = std::min(first[size_t(owner)], i);
			last[size_t(owner)] = std::max(last[size_t(owner)], i);
		}
		for (size_t i = 0; i < n; ++i)
			if (pieces[i].kind == NotedPiece::Kind::Element) {
				starts[first[i]].push_back(i);
				ends[last[i]].push_back(i);
			}
	}
	auto lead_text = [&](size_t at) {
		std::string text;
		for (size_t i = 0; i < at; ++i)
			if (pieces[i].owner == long(at) && pieces[i].kind == NotedPiece::Kind::Text) text += pieces[i].text;
		return text;
	};
	auto indent_at = [&](size_t at) {
		const Lead lead = lead_of(lead_text(at));
		return lead.broken ? lead.indent : s.indent;
	};
	// The children the file does not hold in place (new, or moved within the list), each after the
	// nearest child before it in the writer's order that stands here, else before the nearest after
	// it, else at the end.
	std::vector<std::vector<size_t>> after(n), before(n);
	std::vector<size_t> at_end;
	if (s.member == 0) {
		std::vector<long> here(children.size(), -1);
		for (size_t i = 0; i < n; ++i)
			if (child_of[i] >= 0 && here[size_t(child_of[i])] < 0) here[size_t(child_of[i])] = long(i);
		for (size_t ci = 0; ci < children.size(); ++ci) {
			if (!(p.of[ci].empty() || p.moved[ci])) continue;
			bool placed = false;
			for (size_t k = ci; k-- > 0 && !placed;)
				if (here[k] >= 0) {
					after[size_t(here[k])].push_back(ci);
					placed = true;
				}
			for (size_t k = ci + 1; k < children.size() && !placed; ++k)
				if (here[k] >= 0) {
					before[size_t(here[k])].push_back(ci);
					placed = true;
				}
			if (!placed) at_end.push_back(ci);
		}
	}
	auto insert = [&](size_t ci, long anchor) {
		Entry e;
		e.kind = Entry::Kind::Insert;
		e.child = ci;
		if (p.moved[ci] && !p.of[ci].empty()) {
			// Moved with its own text and comments, from the content it stood in.
			e.noted = p.of[ci].front().noted;
			const NotedElement &moved = l_.elements[e.noted];
			const Pieces &home = moved.parent == UINT32_MAX ? l_.content : l_.elements[moved.parent].content;
			std::string text;
			for (size_t i = 0; i < moved.at; ++i)
				if (home[i].owner == long(moved.at) && home[i].kind == NotedPiece::Kind::Text) text += home[i].text;
			const Lead lead = lead_of(text);
			e.indent = lead.broken ? lead.indent : s.indent;
			return e;
		}
		// A new child: the anchor's line start and indentation, else a line of its own one step in.
		if (s.text_modeled) {
			e.indent = s.indent;
		} else if (anchor >= 0) {
			const std::string text = lead_text(size_t(anchor));
			const Lead lead = lead_of(text);
			if (lead.broken) {
				e.lead = lead.eol + lead.indent;
				e.indent = lead.indent;
			} else if (s.top) {
				e.lead = style_.eol;
			} else {
				e.lead = lead.blanks ? text : std::string();
				e.indent = s.indent;
			}
		} else if (s.top) {
			e.lead = style_.eol;
		} else {
			e.indent = s.indent + style_.unit;
			e.lead = style_.eol.empty() ? std::string() : style_.eol + e.indent;
		}
		return e;
	};
	// The text of a text-modeled content: as spelled while it is the writer's text as read, else
	// the writer's text where the first run of it stood.
	const bool text_as_read = !s.text_modeled || (!s.carried && escape_text(s.text) == s.as_text);
	bool text_put = false;
	std::vector<Entry> out;
	size_t end_at = n;
	if (!s.text_modeled) {
		// The content's own run after its last element: before it go the children added at its end.
		while (end_at > 0 && pieces[end_at - 1].kind != NotedPiece::Kind::Element && pieces[end_at - 1].owner < 0)
			--end_at;
	}
	auto put_at_end = [&]() {
		for (size_t ci : at_end) out.push_back(insert(ci, -1));
		if (!at_end.empty() && !any_element && !s.text_modeled && !s.top && !style_.eol.empty()) {
			// A content that held no element gets its close tag back on a line of its own.
			bool broken = false;
			for (size_t i = end_at; i < n; ++i)
				if (pieces[i].kind == NotedPiece::Kind::Text && pieces[i].text.find('\n') != std::string::npos) broken = true;
			if (!broken) {
				Entry e;
				e.text = style_.eol + s.indent;
				out.push_back(std::move(e));
			}
		}
	};
	if (s.text_modeled && !text_as_read) {
		bool any_text = false;
		for (const NotedPiece &piece : pieces)
			if (piece.owner < 0 && (piece.kind == NotedPiece::Kind::Text || piece.kind == NotedPiece::Kind::RawText))
				any_text = true;
		if (!any_text) {
			Entry e;
			e.text = escape_text(s.text);
			out.push_back(std::move(e));
			text_put = true;
		}
	}
	for (size_t i = 0; i < n; ++i) {
		if (i == end_at) put_at_end();
		for (size_t x : starts[i])
			for (size_t ci : before[x]) out.push_back(insert(ci, long(x)));
		const NotedPiece &piece = pieces[i];
		if (piece.kind == NotedPiece::Kind::Element) {
			if (written[i]) {
				Entry e;
				if (child_of[i] >= 0) {
					e.kind = Entry::Kind::Child;
					e.child = size_t(child_of[i]);
					e.noted = piece.element;
					e.indent = indent_at(i);
				} else {
					e.kind = Entry::Kind::Piece;
					e.piece = &piece;
				}
				out.push_back(std::move(e));
			}
		} else if (piece.owner >= 0) {
			if (written[size_t(piece.owner)]) {
				Entry e;
				e.kind = Entry::Kind::Piece;
				e.piece = &piece;
				out.push_back(std::move(e));
			}
		} else if (s.text_modeled && (piece.kind == NotedPiece::Kind::Text || piece.kind == NotedPiece::Kind::RawText)) {
			Entry e;
			if (text_as_read) {
				e.kind = Entry::Kind::Piece;
				e.piece = &piece;
			} else if (!text_put) {
				e.text = escape_text(s.text);
				text_put = true;
			} else {
				continue;
			}
			out.push_back(std::move(e));
		} else {
			Entry e;
			e.kind = Entry::Kind::Piece;
			e.piece = &piece;
			out.push_back(std::move(e));
		}
		for (size_t x : ends[i])
			for (size_t ci : after[x]) out.push_back(insert(ci, long(x)));
	}
	if (end_at == n) put_at_end();
	return out;
}

void Generator::verbatim_element(const NotedElement &n) {
	out_ += "<" + n.after_lt + n.tag;
	for (const NotedAttribute &a : n.attributes) out_ += a.gap + a.name + a.value;
	out_ += n.tail;
	if (n.ended) out_ += ">";
	for (const NotedPiece &piece : n.content) {
		if (piece.kind == NotedPiece::Kind::Element) verbatim_element(l_.elements[piece.element]);
		else out_ += piece.text;
	}
	if (n.closed) out_ += n.close;
}

void Generator::new_element(const WrittenElement &c, const std::string &indent) {
	if (column_ && column_row(c.kind)) {
		const auto it = column_->planned.find(&c);
		if (it != column_->planned.end()) {
			out_ += "<" + c.tag;
			for (const Token &t : it->second) out_ += t.text;
			out_ += ">";
			render_written_content(c, style_, indent, out_);
			out_ += "</" + c.tag + ">";
			return;
		}
	}
	render_written(c, style_, indent, out_);
}

void Generator::emit(const Scope &s, const std::vector<Entry> &entries, const Placement &p) {
	const std::vector<WrittenElement> &children = *s.children;
	for (const Entry &e : entries) {
		switch (e.kind) {
		case Entry::Kind::Text: out_ += e.text; break;
		case Entry::Kind::Piece:
			if (e.piece->kind == NotedPiece::Kind::Element) verbatim_element(l_.elements[e.piece->element]);
			else out_ += e.piece->text;
			break;
		case Entry::Kind::Child: {
			const WrittenElement &c = children[e.child];
			std::vector<uint32_t> members;
			size_t member = 0;
			for (const Occurrence &o : p.of[e.child]) {
				if (!p.whole[e.child] && !members.empty()) break;
				if (o.noted == e.noted) member = members.size();
				members.push_back(o.noted);
			}
			if (position_ && c.kind == WrittenKind::Edge) edge(*position_, c, e.noted, e.indent);
			else element(c, members, member, e.indent);
			break;
		}
		case Entry::Kind::Insert: {
			const WrittenElement &c = children[e.child];
			if (e.noted == UINT32_MAX) {
				out_ += e.lead;
				if (position_ && c.kind == WrittenKind::Edge) {
					edge(*position_, c, UINT32_MAX, e.indent);
				} else {
					new_element(c, e.indent);
				}
				break;
			}
			// A record moved within its list: with its own text and comments.
			const NotedElement &n = l_.elements[e.noted];
			const Pieces &home = n.parent == UINT32_MAX ? l_.content : l_.elements[n.parent].content;
			for (size_t i = 0; i < n.at; ++i)
				if (home[i].owner == long(n.at)) out_ += home[i].text;
			element(c, {e.noted}, 0, e.indent);
			for (size_t i = n.at + 1; i < home.size(); ++i)
				if (home[i].owner == long(n.at)) out_ += home[i].text;
			break;
		}
		}
	}
}

std::vector<Token> Generator::attributes(const WrittenElement &c, const NotedElement &n, bool primary,
                                         const std::set<std::string> &elsewhere, ColumnPass *pass) const {
	std::vector<Token> out;
	std::set<std::string> put;
	const bool own_as_read = c.own_digest == n.as_own;
	auto find = [&](const std::string &key) -> const WrittenAttribute * {
		for (const WrittenAttribute &a : c.attributes)
			if (a.key == key) return &a;
		return nullptr;
	};
	// A BODY's draw kind is the first of the three it authors [orig: CTableWnd_ParseXMLContentDefinition
	// @ 0x6427d0, the BODY walk]: when the file's first is not the writer's, the three are put down
	// again in the writer's order.
	bool redraw = false;
	if (c.kind == WrittenKind::Body) {
		auto draw = [](const std::string &key) { return known_token(key, kBodyDisplays); };
		std::string file_first, writer_first;
		for (const NotedAttribute &a : n.attributes)
			if (a.role == NotedRole::Written && draw(a.key) && find(a.key)) {
				file_first = a.key;
				break;
			}
		for (const WrittenAttribute &a : c.attributes)
			if (draw(a.key)) {
				writer_first = a.key;
				break;
			}
		redraw = !writer_first.empty() && file_first != writer_first;
		if (redraw)
			for (const NotedAttribute &a : n.attributes)
				if (draw(a.key)) put.insert("!" + a.key); // marks the file's to drop
	}
	for (const NotedAttribute &a : n.attributes) {
		const int slot = c.kind == WrittenKind::Header ? sort_slot(a.key) : -1;
		if (slot >= 0) {
			// A table sort key stays where the file has it while the walk still reads it as the model's.
			if (pass ? pass->keep[slot] : own_as_read) out.push_back(verbatim(a));
			continue;
		}
		if (put.count("!" + a.key)) continue;
		switch (a.role) {
		case NotedRole::Written: {
			const WrittenAttribute *wa = find(a.key);
			if (!wa) break; // removed
			Token t = written_token(*wa);
			t.text = a.gap + (wa->name == a.as_name ? a.name : wa->name) +
			         (wa->value_token() == a.as_value ? a.value : wa->value_token());
			out.push_back(std::move(t));
			put.insert(a.key);
			break;
		}
		case NotedRole::Tied:
			if (own_as_read) out.push_back(verbatim(a));
			break;
		case NotedRole::Inert: out.push_back(verbatim(a)); break;
		}
	}
	if (!primary) return out;
	for (size_t k = 0; k < c.attributes.size(); ++k) {
		const WrittenAttribute &wa = c.attributes[k];
		if (put.count(wa.key) || elsewhere.count(wa.key)) continue;
		if (c.kind == WrittenKind::Header && sort_slot(wa.key) >= 0) continue; // the pass places those
		bool left_out = false;
		for (const auto &lo : n.left_out)
			if (lo.first == wa.key && lo.second == wa.value_token()) left_out = true;
		// A row leaves its COLUMN out while the running index carries it.
		if (left_out && pass && column_row(c.kind) && wa.key == "COLUMN") left_out = pass->running == number(wa.value);
		if (left_out) continue;
		insert_token(out, c, k, written_token(wa));
	}
	if (pass) place_sorts(c, out, *pass);
	return out;
}

std::vector<Token> Generator::new_attributes(const WrittenElement &c, ColumnPass *pass) const {
	std::vector<Token> out;
	for (const WrittenAttribute &a : c.attributes)
		if (!(c.kind == WrittenKind::Header && sort_slot(a.key) >= 0)) out.push_back(written_token(a));
	if (pass) place_sorts(c, out, *pass);
	return out;
}

SortRead Generator::plan_rows(const std::vector<Entry> &entries, const std::vector<WrittenElement> &rows,
                              ColumnPass &pass) const {
	pass.running = 0;
	pass.planned.clear();
	SortRead read;
	for (const Entry &e : entries) {
		if (e.kind != Entry::Kind::Child && e.kind != Entry::Kind::Insert) continue;
		const WrittenElement &row = rows[e.child];
		if (!column_row(row.kind)) continue;
		std::vector<Token> tokens = e.noted != UINT32_MAX ? attributes(row, l_.elements[e.noted], true, {}, &pass)
		                                                  : new_attributes(row, &pass);
		fold(read, row.kind == WrittenKind::Header, tokens, pass.running);
		pass.planned[&row] = std::move(tokens);
	}
	return read;
}

// A COLUMN: its rows planned first, so the running index and the table sort keys read back as the
// model's; when no placement of the keys does, the COLUMN is the writer's own layout.
void Generator::column(const WrittenElement &c, uint32_t noted, const std::string &indent) {
	const NotedElement &n = l_.elements[noted];
	Scope s;
	s.w = &c;
	s.children = &c.children;
	s.contents = {&n.content};
	s.indent = indent;
	s.tied_ok = c.tree_digest == n.as_tree;
	const Placement p = place(s);
	const std::vector<Entry> entries = plan(s, p);
	// The model's keys: what the writer's own form reads back as.
	SortRead model;
	int model_running = 0;
	for (const WrittenElement &row : c.children) {
		std::vector<Token> tokens;
		for (const WrittenAttribute &a : row.attributes) tokens.push_back(written_token(a));
		fold(model, row.kind == WrittenKind::Header, tokens, model_running);
	}
	ColumnPass pass;
	for (const WrittenElement &row : c.children)
		for (const WrittenAttribute &a : row.attributes)
			if (sort_slot(a.key) >= 0) pass.names[sort_slot(a.key)] = a.name;
	SortRead read = plan_rows(entries, c.children, pass);
	if (!(read == model)) {
		for (int slot = 0; slot < 3; ++slot) {
			const bool same = read.value[slot] == model.value[slot] && (slot != 0 || model.value[0] < 0 || read.name0 == model.name0);
			if (same) continue;
			pass.keep[slot] = false;
			if (model.value[slot] < 0) continue;
			// The first row whose own index is the key's, else the first the running index reaches
			// it before and that puts down a COLUMN.
			int running = 0;
			const WrittenElement *own = nullptr, *after = nullptr;
			for (const Entry &e : entries) {
				if (e.kind != Entry::Kind::Child && e.kind != Entry::Kind::Insert) continue;
				const WrittenElement &row = c.children[e.child];
				if (!column_row(row.kind)) continue;
				const auto &tokens = pass.planned[&row];
				const int before = running;
				SortRead ignored;
				fold(ignored, false, tokens, running);
				if (row.kind != WrittenKind::Header) continue;
				const bool has_column = std::any_of(tokens.begin(), tokens.end(), [](const Token &t) { return t.key == "COLUMN"; });
				if (!own && running == model.value[slot]) own = &row;
				if (!after && before == model.value[slot] && has_column) after = &row;
			}
			pass.place_on[slot] = own ? own : after;
			pass.own[slot] = own != nullptr;
		}
		read = plan_rows(entries, c.children, pass);
	}
	if (!(read == model)) {
		render_written(c, style_, indent, out_);
		return;
	}
	out_ += "<" + n.after_lt + (c.tag == n.as_tag ? n.tag : c.tag);
	for (const Token &t : attributes(c, n, true, {}, nullptr)) out_ += t.text;
	out_ += n.tail;
	if (n.ended) out_ += ">";
	if (!n.ended) return;
	ColumnPass *outer = column_;
	column_ = &pass;
	emit(s, entries, p);
	column_ = outer;
	if (n.closed) out_ += c.tag == n.as_tag && !n.close_carries ? n.close : "</" + c.tag + ">";
}

// A POSITION edge: its element's form (LEFT or ULX, WIDTH for the right, ...) and spelling kept,
// its number generated where it stands so the reader's edge comes out as the model's [orig:
// CUIElement_ParseXMLDefinition @ 0x648ae6, the edges read in order, WIDTH and HEIGHT added to
// the left and top as they stand].
void Generator::edge(const WrittenElement &position, const WrittenElement &e, uint32_t noted, const std::string &indent) {
	PositionSim &sim = positions_[&position];
	auto edge_value = [&](const char *key) {
		for (const WrittenElement &child : position.children)
			if (child.key == key) return number(child.text);
		return 0;
	};
	const std::string form = noted == UINT32_MAX ? e.tag : upper(l_.elements[noted].tag);
	int value = 0;
	if (noted != UINT32_MAX && sim.last.count(e.key) && sim.last[e.key] != noted) {
		value = number(l_.elements[noted].as_text); // an edge a later element sets again: as it was
	} else if (form == "WIDTH") {
		value = int(int32_t(uint32_t(edge_value("RIGHT")) - uint32_t(sim.left)));
	} else if (form == "HEIGHT") {
		value = int(int32_t(uint32_t(edge_value("BOTTOM")) - uint32_t(sim.top)));
	} else {
		value = edge_value(e.key.c_str());
	}
	if (form == "LEFT" || form == "ULX") sim.left = value;
	if (form == "TOP" || form == "ULY") sim.top = value;
	WrittenElement written = e;
	written.text = std::to_string(value);
	if (noted == UINT32_MAX) {
		new_element(written, indent);
		return;
	}
	written.tag = l_.elements[noted].as_tag;
	element(written, {noted}, 0, indent);
}

void Generator::element(const WrittenElement &c, const std::vector<uint32_t> &members, size_t member,
                        const std::string &indent) {
	const uint32_t noted = members[member];
	if (!used_.insert(noted).second) { // a layout element named twice: the second is the writer's own
		new_element(c, indent);
		return;
	}
	const NotedElement &n = l_.elements[noted];
	if (c.kind == WrittenKind::Column && members.size() == 1) {
		column(c, noted, indent);
		return;
	}
	std::set<std::string> elsewhere;
	for (size_t m = 0; m < members.size(); ++m)
		if (m != member)
			for (const NotedAttribute &a : l_.elements[members[m]].attributes)
				if (a.role == NotedRole::Written) elsewhere.insert(a.key);
	out_ += "<" + n.after_lt + (c.tag == n.as_tag ? n.tag : c.tag);
	std::vector<Token> tokens;
	if (column_ && column_row(c.kind) && column_->planned.count(&c)) tokens = column_->planned[&c];
	else tokens = attributes(c, n, member == 0, elsewhere, nullptr);
	for (const Token &t : tokens) out_ += t.text;
	out_ += n.tail;
	if (n.ended) out_ += ">";
	if (!n.ended) return; // the file ends inside its open tag
	if (c.form != n.as_form) {
		render_written_content(c, style_, indent, out_);
		out_ += "</" + c.tag + ">";
		return;
	}
	Scope s;
	s.w = &c;
	s.children = &c.children;
	for (uint32_t m : members) s.contents.push_back(&l_.elements[m].content);
	s.member = member;
	s.text_modeled = c.form != WrittenForm::Container;
	s.text = c.text;
	s.as_text = n.as_text;
	for (const NotedPiece &piece : n.content)
		if (piece.carries) s.carried = true;
	s.indent = indent;
	s.tied_ok = c.tree_digest == n.as_tree;
	const Placement p = place(s);
	if (c.kind == WrittenKind::Position) {
		PositionSim &sim = positions_[&c];
		if (!sim.ready) {
			// Each edge's last element across the POSITIONs, the one that sets it.
			sim.ready = true;
			for (const Pieces *content : s.contents)
				for (const NotedPiece &piece : *content)
					if (piece.kind == NotedPiece::Kind::Element && l_.elements[piece.element].role == NotedRole::Written)
						sim.last[l_.elements[piece.element].key] = piece.element;
		}
	}
	const std::vector<Entry> entries = plan(s, p);
	const WrittenElement *outer = position_;
	position_ = c.kind == WrittenKind::Position ? &c : nullptr;
	ColumnPass *outer_column = column_;
	if (!column_row(c.kind)) column_ = nullptr;
	emit(s, entries, p);
	position_ = outer;
	column_ = outer_column;
	if (n.closed) out_ += c.tag == n.as_tag && !n.close_carries ? n.close : "</" + c.tag + ">";
}

std::string Generator::run(const std::vector<WrittenElement> &screens) {
	Scope s;
	s.children = &screens;
	s.contents = {&l_.content};
	s.top = true;
	const Placement p = place(s);
	emit(s, plan(s, p), p);
	out_ += l_.after_end;
	return std::move(out_);
}

} // namespace

std::string text_layout_generate(const TextLayout &layout, const std::vector<WrittenElement> &screens,
                                 const WrittenStyle &canonical) {
	Generator generator(layout, canonical);
	return generator.run(screens);
}

} // namespace opennova::mnu
