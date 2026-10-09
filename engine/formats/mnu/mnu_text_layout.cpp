// The capture of a file's text layout (mnu_text_layout.h): the reader's spans cut into the
// layout's tokens, each element's what-it-is named by the reader (TextLayoutCapture's record,
// keep and tie), and every token modeled against the writer's words for the records as read.
#include "mnu_text_layout.h"

#include <formats/mnu/mnu_xml.h>

#include <algorithm>
#include <atomic>
#include <utility>

#include <base/io/cp1252.h>

namespace opennova::mnu {

namespace {

using mnu_xml::Attribute;
using mnu_xml::Node;
using mnu_xml::Span;

std::atomic<uint32_t> g_stamp{0};

std::string upper(std::string s) {
	for (char &c : s)
		if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
	return s;
}

} // namespace

const NotedElement *TextLayout::element(uint64_t source) const {
	const uint32_t i = index(source);
	return i == UINT32_MAX ? nullptr : &elements[i];
}

uint32_t TextLayout::index(uint64_t source) const {
	if (!source || uint32_t(source >> 32) != stamp) return UINT32_MAX;
	const uint32_t n = uint32_t(source);
	if (n == 0 || n > records.size()) return UINT32_MAX;
	return records[n - 1];
}

TextLayoutCapture::TextLayoutCapture(SourceEncoding encoding) : layout_(std::make_shared<TextLayout>()) {
	layout_->encoding = encoding;
	layout_->stamp = ++g_stamp;
	if (!layout_->stamp) layout_->stamp = ++g_stamp; // never 0: a source of 0 names none
}

uint64_t TextLayoutCapture::record(const Node &node) {
	record_nodes_.push_back(&node);
	const uint64_t source = text_layout_source(layout_->stamp, uint32_t(record_nodes_.size()));
	keys_[&node] = "#" + std::to_string(source);
	tied_.erase(&node);
	return source;
}

void TextLayoutCapture::keep(const Node &node, const std::string &key) {
	keys_[&node] = key;
	tied_.erase(&node);
}

void TextLayoutCapture::tie(const Node &node) {
	keys_.erase(&node);
	tied_.insert(&node);
}

// The layout built from one reading: the tokens, then each element modeled against the writer's
// words for it.
class TextLayoutBuilder {
public:
	TextLayoutBuilder(TextLayoutCapture &capture, const std::u32string &text, const mnu_xml::Document &xml)
	    : c_(capture), l_(*capture.layout_), text_(text), xml_(xml) {}
	void build(const std::vector<WrittenElement> &screens);

private:
	std::string narrow(size_t begin, size_t end) const;
	void tokens();
	void open_tag(NotedElement &e, const Node &node, size_t begin, size_t end);
	// Each element piece's place after the runs are split at the line ends that part an element's
	// own text from the next one's.
	void split_runs(std::vector<NotedPiece> &content);
	void own(std::vector<NotedPiece> &content);
	void pair_content(const std::vector<WrittenElement> &children, const std::vector<uint32_t> &members);
	void pair_element(const WrittenElement &w, const std::vector<uint32_t> &members);
	void pair_attributes(const WrittenElement &w, const std::vector<uint32_t> &members);
	void free_element(uint32_t index, NotedRole role);
	void style();

	TextLayoutCapture &c_;
	TextLayout &l_;
	const std::u32string &text_;
	const mnu_xml::Document &xml_;
	std::vector<const Node *> nodes_;           // each element's node
	std::map<const Node *, uint32_t> index_of_; // each node's element
	std::vector<size_t> carried_;               // where attributes outside their element's open tag stand
};

std::string TextLayoutBuilder::narrow(size_t begin, size_t end) const {
	std::string out;
	end = std::min(end, text_.size());
	for (size_t i = begin; i < end; ++i) {
		const char32_t c = text_[i];
		if (l_.encoding == SourceEncoding::CodePage) {
			uint8_t byte = 0;
			out.push_back(cp1252_encode_codepoint(c, byte) ? static_cast<char>(byte) : '?');
		} else {
			utf8_append(out, c);
		}
	}
	return out;
}

// An open tag cut where the reader cut it [orig: NapiXML_ParseElementTree @ 0x769d70]: the blanks
// it skips after '<', the name up to a blank or '>', then each attribute from the blanks before it
// to past what it skips after the value; what is left before '>' is the blanks of the empty
// attribute the reader makes there, or the rest of a tag the text ends inside.
void TextLayoutBuilder::open_tag(NotedElement &e, const Node &node, size_t begin, size_t end) {
	size_t at = begin + 1;
	while (at < end && mnu_xml::is_space(text_[at])) ++at;
	e.after_lt = narrow(begin + 1, at);
	const size_t tag_end = std::min(end, at + node.tag.size());
	e.tag = narrow(at, tag_end);
	size_t last = tag_end;
	for (const Attribute &a : node.attributes) {
		if (a.name_at < begin || a.end > end || a.at < tag_end) continue; // another tag's
		NotedAttribute n;
		n.gap = narrow(a.at, a.name_at);
		n.name = narrow(a.name_at, a.name_at + a.name.size());
		n.value = narrow(a.name_at + a.name.size(), a.end);
		n.read = a.read;
		e.attributes.push_back(std::move(n));
		last = a.end;
	}
	std::string rest = narrow(last, end);
	if (!rest.empty() && rest.back() == '>') {
		e.ended = true;
		rest.pop_back();
	}
	e.tail = std::move(rest);
}

void TextLayoutBuilder::tokens() {
	// Where each element's open tag stands, then the attributes the reader gave an element from
	// another tag (a close tag's, a RAW_TEXT end's): what that tag carries.
	std::map<const Node *, std::pair<size_t, size_t>> open_at;
	for (const Span &s : xml_.spans)
		if (s.kind == Span::Kind::Open) open_at[s.node] = {s.begin, s.end};
	for (const auto &entry : open_at)
		for (const Attribute &a : entry.first->attributes)
			if ((a.name_at < entry.second.first || a.end > entry.second.second) && (!a.name.empty() || a.has_value))
				carried_.push_back(a.name_at);
	std::sort(carried_.begin(), carried_.end());
	auto carries = [&](size_t begin, size_t end) {
		const auto it = std::lower_bound(carried_.begin(), carried_.end(), begin);
		return it != carried_.end() && *it < end;
	};

	std::vector<uint32_t> open;
	for (const Span &s : xml_.spans) {
		std::vector<NotedPiece> &content = open.empty() ? l_.content : l_.elements[open.back()].content;
		NotedPiece piece;
		switch (s.kind) {
		case Span::Kind::Text:
		case Span::Kind::Comment:
		case Span::Kind::RawText:
			piece.kind = s.kind == Span::Kind::Text      ? NotedPiece::Kind::Text
			             : s.kind == Span::Kind::Comment ? NotedPiece::Kind::Comment
			                                             : NotedPiece::Kind::RawText;
			piece.text = narrow(s.begin, s.end);
			piece.carries = s.kind == Span::Kind::RawText && carries(s.begin, s.end);
			content.push_back(std::move(piece));
			break;
		case Span::Kind::Open: {
			NotedElement e;
			e.parent = open.empty() ? UINT32_MAX : open.back();
			e.at = uint32_t(content.size());
			open_tag(e, *s.node, s.begin, s.end);
			const uint32_t index = uint32_t(l_.elements.size());
			piece.kind = NotedPiece::Kind::Element;
			piece.element = index;
			content.push_back(std::move(piece)); // before the push below moves the elements
			l_.elements.push_back(std::move(e));
			nodes_.push_back(s.node);
			index_of_[s.node] = index;
			open.push_back(index);
			break;
		}
		case Span::Kind::Close: {
			if (open.empty()) break;
			NotedElement &e = l_.elements[open.back()];
			e.closed = true;
			e.close = narrow(s.begin, s.end);
			e.close_carries = carries(s.begin, s.end);
			open.pop_back();
			break;
		}
		}
	}
	l_.after_end = narrow(xml_.end, text_.size());
}

// The line end that parts what goes with an element from what goes with the next: a run after an
// element is cut before its first line end (the cut keeps "\r\n" whole).
void TextLayoutBuilder::split_runs(std::vector<NotedPiece> &content) {
	std::vector<NotedPiece> out;
	out.reserve(content.size() + 4);
	bool after_element = false;
	for (NotedPiece &piece : content) {
		if (piece.kind == NotedPiece::Kind::Element) {
			after_element = true;
			out.push_back(std::move(piece));
			continue;
		}
		if (after_element && piece.kind == NotedPiece::Kind::Text) {
			const size_t nl = piece.text.find('\n');
			if (nl != std::string::npos) {
				const size_t cut = nl > 0 && piece.text[nl - 1] == '\r' ? nl - 1 : nl;
				after_element = false;
				if (cut > 0) {
					NotedPiece head = piece;
					head.text = piece.text.substr(0, cut);
					out.push_back(std::move(head));
					piece.text = piece.text.substr(cut);
				}
			}
		}
		out.push_back(std::move(piece));
	}
	content = std::move(out);
	for (size_t i = 0; i < content.size(); ++i)
		if (content[i].kind == NotedPiece::Kind::Element) l_.elements[content[i].element].at = uint32_t(i);
}

// Who each piece goes with (NotedPiece::owner): runs are already cut at their first line end.
void TextLayoutBuilder::own(std::vector<NotedPiece> &content) {
	std::vector<size_t> elements;
	for (size_t i = 0; i < content.size(); ++i)
		if (content[i].kind == NotedPiece::Kind::Element) elements.push_back(i);
	for (NotedPiece &piece : content) piece.owner = -1;
	if (elements.empty()) return;
	auto line_ends = [&](size_t from, size_t to) { // the first piece of [from, to) holding a line end
		for (size_t i = from; i < to; ++i)
			if (content[i].kind == NotedPiece::Kind::Text && content[i].text.find('\n') != std::string::npos) return i;
		return to;
	};
	for (size_t i = 0; i < elements.front(); ++i) content[i].owner = int32_t(elements.front());
	for (size_t k = 0; k < elements.size(); ++k) {
		const size_t from = elements[k] + 1;
		const size_t to = k + 1 < elements.size() ? elements[k + 1] : content.size();
		const size_t cut = line_ends(from, to);
		// The rest of the element's line goes with it when a line end follows; the run after
		// that, with the next element (or with the content itself at its end).
		const bool broken = cut < to;
		for (size_t i = from; i < to; ++i) {
			if (broken && i < cut) content[i].owner = int32_t(elements[k]);
			else content[i].owner = k + 1 < elements.size() ? int32_t(elements[k + 1]) : -1;
		}
	}
}

void TextLayoutBuilder::free_element(uint32_t index, NotedRole role) {
	NotedElement &e = l_.elements[index];
	e.role = role;
	e.key.clear();
	for (NotedAttribute &a : e.attributes) a.role = NotedRole::Inert;
	for (const NotedPiece &piece : e.content)
		if (piece.kind == NotedPiece::Kind::Element) free_element(piece.element, NotedRole::Inert);
}

void TextLayoutBuilder::pair_attributes(const WrittenElement &w, const std::vector<uint32_t> &members) {
	// A kept attribute's place counts across the elements one record was read from, as the reader
	// appends them in order.
	std::vector<std::pair<std::string, size_t>> seen;
	for (uint32_t m : members) {
		for (NotedAttribute &a : l_.elements[m].attributes) {
			if (a.name.empty() && a.value.empty()) { // the blanks before '>'
				a.role = NotedRole::Inert;
				a.key.clear();
				continue;
			}
			const std::string name = upper(a.name);
			size_t place = 0;
			auto it = std::find_if(seen.begin(), seen.end(), [&](const auto &s) { return s.first == name; });
			if (it == seen.end()) seen.emplace_back(name, 1);
			else place = it->second++;
			a.key = written_key(w.kind, a.name, place);
			a.role = NotedRole::Inert;
		}
	}
	// The written one of each of the writer's attributes: the one the reader took its value from,
	// the first of the last element that has one (a later element read into the same record wins,
	// as each repeated element's walk writes the same fields [orig: CUIElement_ParseXMLDefinition
	// @ 0x648120]).
	for (const WrittenAttribute &wa : w.attributes) {
		NotedAttribute *claimant = nullptr;
		for (size_t k = members.size(); k-- > 0 && !claimant;)
			for (NotedAttribute &a : l_.elements[members[k]].attributes)
				if (a.key == wa.key && a.read && a.role == NotedRole::Inert) {
					claimant = &a;
					break;
				}
		if (!claimant) {
			// What the reader supplies with no attribute of its own: a row's COLUMN the running index
			// carries, a table sort key the file sets on another row. Anything else the writer puts
			// down (an attribute a close tag gave the element is not one the layout holds).
			const bool row = w.kind == WrittenKind::Header || w.kind == WrittenKind::Body || w.kind == WrittenKind::Subst;
			if ((row && wa.key == "COLUMN") || (w.kind == WrittenKind::Header && wa.key.rfind("@SORT", 0) == 0))
				l_.elements[members.front()].left_out.emplace_back(wa.key, wa.value_token());
			continue;
		}
		claimant->role = NotedRole::Written;
		claimant->as_name = wa.name;
		claimant->as_value = wa.value_token();
	}
	for (uint32_t m : members)
		for (NotedAttribute &a : l_.elements[m].attributes)
			if (a.role != NotedRole::Written && !a.key.empty() && written_vocabulary(w.kind, a.key))
				a.role = NotedRole::Tied;
}

void TextLayoutBuilder::pair_element(const WrittenElement &w, const std::vector<uint32_t> &members) {
	for (size_t k = 0; k < members.size(); ++k) {
		NotedElement &e = l_.elements[members[k]];
		e.role = NotedRole::Written;
		e.key = w.key;
		e.primary = k == 0;
		e.as_tag = w.tag;
		e.as_form = w.form;
		e.as_own = w.own_digest;
		e.as_tree = w.tree_digest;
		if (w.form != WrittenForm::Container) e.as_text = escape_text(w.text);
		// A POSITION edge: the number it carried (WIDTH and HEIGHT add theirs to the edge before).
		if (w.kind == WrittenKind::Edge)
			e.as_text = std::to_string(static_cast<int>(mnu_xml::wcstol(nodes_[members[k]]->text, 10)));
		if (w.form == WrittenForm::Container) {
			split_runs(e.content);
			own(e.content);
		}
	}
	pair_attributes(w, members);
	pair_content(w.children, members);
}

void TextLayoutBuilder::pair_content(const std::vector<WrittenElement> &children, const std::vector<uint32_t> &members) {
	// Each element of the contents, by what the reader named it: its record's or singleton's key
	// (several for one record read from repeated elements, in file order).
	std::map<std::string, std::vector<uint32_t>> by_key;
	auto visit = [&](const std::vector<NotedPiece> &content, NotedRole tied_role) {
		for (const NotedPiece &piece : content) {
			if (piece.kind != NotedPiece::Kind::Element) continue;
			const Node *node = nodes_[piece.element];
			const auto key = c_.keys_.find(node);
			if (key != c_.keys_.end()) by_key[key->second].push_back(piece.element);
			else free_element(piece.element, c_.tied_.count(node) ? tied_role : NotedRole::Inert);
		}
	};
	if (members.empty()) visit(l_.content, NotedRole::Inert);
	for (uint32_t m : members) visit(l_.elements[m].content, NotedRole::Tied);
	for (const WrittenElement &child : children) {
		const auto it = by_key.find(child.key);
		if (it == by_key.end()) continue;
		pair_element(child, it->second);
		by_key.erase(it);
	}
	// Read into the model, but the writer puts nothing down for it (an empty row): kept while its
	// element is as read.
	for (const auto &left : by_key)
		for (uint32_t index : left.second) free_element(index, NotedRole::Tied);
}

// The file's line ending and indent step: the most common step between an element's indentation
// and its parent's, where both begin a line.
void TextLayoutBuilder::style() {
	auto lead = [&](const std::vector<NotedPiece> &content, uint32_t at, std::string &indent) {
		std::string text;
		for (uint32_t i = 0; i < at; ++i)
			if (content[i].owner == int32_t(at) && content[i].kind == NotedPiece::Kind::Text) text += content[i].text;
		const size_t nl = text.rfind('\n');
		if (nl == std::string::npos) return false;
		indent = text.substr(nl + 1);
		return std::all_of(indent.begin(), indent.end(), [](char c) { return c == ' ' || c == '\t'; });
	};
	const size_t nl = text_.find(U'\n');
	if (nl != std::u32string::npos) {
		l_.styled = true;
		l_.eol = nl > 0 && text_[nl - 1] == U'\r' ? "\r\n" : "\n";
	}
	std::map<std::string, size_t> steps;
	std::vector<std::string> indents(l_.elements.size());
	std::vector<bool> begins_line(l_.elements.size(), false);
	for (uint32_t i = 0; i < l_.elements.size(); ++i) {
		const NotedElement &e = l_.elements[i];
		if (e.role != NotedRole::Written) continue;
		const std::vector<NotedPiece> &content = e.parent == UINT32_MAX ? l_.content : l_.elements[e.parent].content;
		begins_line[i] = lead(content, e.at, indents[i]);
		if (!begins_line[i]) continue;
		const bool parent_begins = e.parent == UINT32_MAX || begins_line[e.parent];
		const std::string parent_indent = e.parent == UINT32_MAX ? std::string() : indents[e.parent];
		if (!parent_begins || indents[i].size() <= parent_indent.size() ||
		    indents[i].compare(0, parent_indent.size(), parent_indent) != 0)
			continue;
		++steps[indents[i].substr(parent_indent.size())];
	}
	size_t best = 0;
	for (const auto &step : steps)
		if (step.second > best) {
			best = step.second;
			l_.unit = step.first;
			l_.has_unit = true;
		}
}

void TextLayoutBuilder::build(const std::vector<WrittenElement> &screens) {
	tokens();
	l_.records.assign(c_.record_nodes_.size(), UINT32_MAX);
	for (size_t i = 0; i < c_.record_nodes_.size(); ++i) {
		const auto it = index_of_.find(c_.record_nodes_[i]);
		if (it != index_of_.end()) l_.records[i] = it->second;
	}
	split_runs(l_.content);
	own(l_.content);
	pair_content(screens, {});
	style();
}

std::shared_ptr<const TextLayout> TextLayoutCapture::finish(const std::u32string &text, const mnu_xml::Document &xml,
                                                            const std::vector<WrittenElement> &screens, std::string bom) {
	layout_->bom = std::move(bom);
	TextLayoutBuilder builder(*this, text, xml);
	builder.build(screens);
	return layout_;
}

bool text_layout_applies(const Document &doc) {
	return doc.text_layout && doc.text_layout->encoding == doc.source_encoding;
}

} // namespace opennova::mnu
