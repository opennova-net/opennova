// The .mnu XML reader, a structural translation of retail's
// [orig: NapiXML_ParseElementTree @ 0x769d70; XML_ParseCharEntity @ 0x769cc0].
#include <formats/mnu/mnu_xml.h>

#include <climits>
#include <cstdint>

namespace opennova::mnu_xml {

namespace {

constexpr char32_t kNul = 0;

// The named-entity table [orig: off_85A628, lengths filled once per load by
// NapiXML_ParserCtor @ 0x769c70]: each full name (the '&' and the ';' included) is
// matched as a prefix of the text at the '&', the first seven ignoring case, the
// Latin-1 set and the guillemets case-sensitively. nbsp is a plain space and there
// is no &apos;. Table order, witnessed.
struct NamedEntity {
	const char *name;
	char32_t value;
	bool case_sensitive;
};
const NamedEntity kNamedEntities[] = {
	{"&quot;", 0x22, false}, {"&amp;", 0x26, false}, {"&lt;", 0x3c, false}, {"&gt;", 0x3e, false},
	{"&copy;", 0xa9, false}, {"&reg;", 0xae, false}, {"&nbsp;", 0x20, false},
	{"&Agrave;", 0xc0, true}, {"&Aacute;", 0xc1, true}, {"&Acirc;", 0xc2, true}, {"&Atilde;", 0xc3, true},
	{"&Auml;", 0xc4, true}, {"&Aring;", 0xc5, true}, {"&AElig;", 0xc6, true}, {"&Ccedil;", 0xc7, true},
	{"&Egrave;", 0xc8, true}, {"&Eacute;", 0xc9, true}, {"&Ecirc;", 0xca, true}, {"&Euml;", 0xcb, true},
	{"&ETH;", 0xd0, true}, {"&Igrave;", 0xcc, true}, {"&Iacute;", 0xcd, true}, {"&Icirc;", 0xce, true},
	{"&Iuml;", 0xcf, true}, {"&Ntilde;", 0xd1, true}, {"&Ograve;", 0xd2, true}, {"&Oacute;", 0xd3, true},
	{"&Ocirc;", 0xd4, true}, {"&Otilde;", 0xd5, true}, {"&Ouml;", 0xd6, true}, {"&Oslash;", 0xd8, true},
	{"&Ugrave;", 0xd9, true}, {"&Uacute;", 0xda, true}, {"&Ucirc;", 0xdb, true}, {"&Uuml;", 0xdc, true},
	{"&Yacute;", 0xdd, true}, {"&THORN;", 0xde, true}, {"&szlig;", 0xdf, true}, {"&agrave;", 0xe0, true},
	{"&aacute;", 0xe1, true}, {"&acirc;", 0xe2, true}, {"&atilde;", 0xe3, true}, {"&auml;", 0xe4, true},
	{"&aring;", 0xe5, true}, {"&aelig;", 0xe6, true}, {"&ccedil;", 0xe7, true}, {"&egrave;", 0xe8, true},
	{"&eacute;", 0xe9, true}, {"&ecirc;", 0xea, true}, {"&euml;", 0xeb, true}, {"&eth;", 0xf0, true},
	{"&igrave;", 0xec, true}, {"&iacute;", 0xed, true}, {"&icirc;", 0xee, true}, {"&iuml;", 0xef, true},
	{"&ntilde;", 0xf1, true}, {"&ograve;", 0xf2, true}, {"&oacute;", 0xf3, true}, {"&ocirc;", 0xf4, true},
	{"&otilde;", 0xf5, true}, {"&ouml;", 0xf6, true}, {"&oslash;", 0xf8, true}, {"&ugrave;", 0xf9, true},
	{"&uacute;", 0xfa, true}, {"&ucirc;", 0xfb, true}, {"&uuml;", 0xfc, true}, {"&yacute;", 0xfd, true},
	{"&yuml;", 0xff, true}, {"&thorn;", 0xfe, true}, {"&raquo;", 0xbb, true}, {"&laquo;", 0xab, true},
};

char32_t fold(char32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// CRT_wchartodigit @ 0x77fd43: ASCII and the Unicode decimal digit blocks it knows.
int digit_value(char32_t c) {
	static const char32_t kZeros[] = {0x30, 0x660, 0x6F0, 0x966, 0x9E6, 0xA66, 0xAE6, 0xB66, 0xC66,
	                                  0xCE6, 0xD66, 0xE50, 0xED0, 0xF20, 0x1040, 0x17E0, 0x1810, 0xFF10};
	for (char32_t zero : kZeros)
		if (c >= zero && c < zero + 10) return int(c - zero);
	return -1;
}

// CRT_wcstoxl @ 0x76e93b over a NUL-terminated view: returns the unsigned
// accumulator with the sign and overflow flags, as the CRT does before its final
// clamp; `is_unsigned` picks wcstoul's clamp.
unsigned long wcstoxl(const char32_t *p, const char32_t *end, int base, bool is_unsigned) {
	auto at = [&](const char32_t *q) { return q < end ? *q : kNul; };
	while (at(p) && is_space(at(p))) ++p;
	bool negative = false;
	if (at(p) == '-') { negative = true; ++p; }
	else if (at(p) == '+') ++p;
	if (base == 16 && digit_value(at(p)) == 0 && (at(p + 1) == 'x' || at(p + 1) == 'X')) p += 2;
	const uint32_t limit = 0xFFFFFFFFu / uint32_t(base), limit_digit = 0xFFFFFFFFu % uint32_t(base);
	uint32_t value = 0;
	bool any = false, overflow = false;
	for (;; ++p) {
		const char32_t c = at(p);
		int digit = digit_value(c);
		if (digit < 0) {
			if (c >= 'A' && c <= 'Z') digit = int(c - 'A') + 10;
			else if (c >= 'a' && c <= 'z') digit = int(c - 'a') + 10;
			else break;
		}
		if (digit >= base) break;
		any = true;
		if (value < limit || (value == limit && uint32_t(digit) <= limit_digit)) value = uint32_t(digit) + uint32_t(base) * value;
		else overflow = true;
	}
	if (!any) return 0;
	if (overflow || (!is_unsigned && ((negative && value > 0x80000000u) || (!negative && value > 0x7FFFFFFFu))))
		value = is_unsigned ? 0xFFFFFFFFu : (negative ? 0x80000000u : 0x7FFFFFFFu);
	if (negative) value = uint32_t(0u - value);
	return value;
}

// The reader's state over one text [orig: NapiXML_ParseElementTree @ 0x769d70].
class Reader {
public:
	Reader(const Text &text, Document &out) : s_(text), out_(out) {
		const size_t nul = s_.find(kNul);
		n_ = nul == Text::npos ? s_.size() : nul;
	}
	bool run(std::string &error);

private:
	char32_t at(size_t i) const { return i < n_ ? s_[i] : kNul; }
	bool fail(std::string &error, const std::string &why) {
		error = why;
		out_.roots.clear();
		return false;
	}
	void note(const std::string &message) { out_.notes.push_back({line_at(pos_), message}); }
	size_t line_at(size_t index) {
		for (; line_pos_ < index && line_pos_ < n_; ++line_pos_)
			if (s_[line_pos_] == '\n') ++line_;
		return line_;
	}
	bool starts_with(size_t at_index, const char *ascii) const {
		for (size_t i = 0; ascii[i]; ++i)
			if (at(at_index + i) != char32_t(static_cast<unsigned char>(ascii[i]))) return false;
		return true;
	}
	char32_t entity();
	// LABEL_61: the attribute list up to '>'. False when the text ends first.
	bool attributes();

	const Text &s_;
	Document &out_;
	size_t n_ = 0;
	size_t pos_ = 0;
	size_t line_pos_ = 0, line_ = 1;
	Node *current_ = nullptr; // where text and children go
	Node *created_ = nullptr; // where attributes go: the element created last
	int screens_ = 0;         // top-level SCREENs so far
};

// [orig: XML_ParseCharEntity @ 0x769cc0] at the '&': the scan end is the first ';',
// ' ' or end after it (never '<' or a line break: a numeric entity swallows markup up
// to there). "&#" is decimal _wtol, its low byte sign-extended to the wide character
// (0x80..0xFF become U+FF80..U+FFFF) and needs no ';'; a named entity must match a
// table name whole; anything else is a bare '&' and the name reads on as text. The
// cursor moves one past the scan end, or one past the '&' for a bare '&'.
char32_t Reader::entity() {
	const size_t start = pos_;
	size_t end = start;
	for (char32_t ch = at(end); ch != ';'; ch = at(end)) {
		if (!ch || ch == ' ') break;
		++end;
	}
	if (at(start + 1) == '#') {
		const unsigned long number = start + 2 <= n_ ? wcstoxl(s_.data() + start + 2, s_.data() + n_, 10, false) : 0;
		pos_ = end + 1;
		const uint8_t low = static_cast<uint8_t>(number & 0xFF);
		return low >= 0x80 ? char32_t(0xFF00u | low) : char32_t(low);
	}
	for (const NamedEntity &e : kNamedEntities) {
		size_t i = 0;
		for (; e.name[i]; ++i) {
			const char32_t want = char32_t(static_cast<unsigned char>(e.name[i]));
			const char32_t got = at(start + i);
			if (e.case_sensitive ? got != want : fold(got) != fold(want)) break;
		}
		if (!e.name[i]) {
			pos_ = end + 1;
			return e.value;
		}
	}
	pos_ = start + 1;
	return '&';
}

bool Reader::attributes() {
	while (at(pos_) != '>') {
		while (at(pos_) && is_space(at(pos_))) ++pos_;
		const size_t name_start = pos_;
		while (at(pos_) && !is_space(at(pos_)) && at(pos_) != '=' && at(pos_) != '>') ++pos_;
		Attribute attr;
		attr.name.assign(s_, name_start, pos_ - name_start);
		if (at(pos_) == '=') {
			++pos_;
			const char32_t open = at(pos_);
			if (open == '"' || open == '\'') {
				size_t q = pos_ + 1;
				const size_t value_start = q;
				while (at(q) && at(q) != '"' && at(q) != '\'') ++q;
				if (!at(q)) {
					// Retail steps past the end of the text here and reads on.
					note("The file ends inside a quoted value: retail reads past the end of the text "
					     "(the rest of the file is lost).");
					return false;
				}
				pos_ = q + 1;
				attr.value.assign(s_, value_start, pos_ - value_start); // the closing quote kept
			} else {
				const size_t value_start = pos_;
				while (at(pos_) && !is_space(at(pos_)) && at(pos_) != '>' && at(pos_) != '"') ++pos_;
				attr.value.assign(s_, value_start, pos_ - value_start);
			}
			attr.has_value = true;
		}
		// Retail prepends to the element created last (before any element, into the
		// text buffer it was handed: nothing a menu can hold).
		if (created_) created_->attributes.push_back(std::move(attr));
		while (at(pos_) && !is_space(at(pos_)) && at(pos_) != '>') ++pos_;
		if (!at(pos_)) {
			// Retail keeps adding empty attributes at the end of the text forever.
			note("The file ends inside a tag: retail hangs here.");
			return false;
		}
	}
	++pos_;
	return true;
}

bool Reader::run(std::string &error) {
	if (!at(0)) return true;
	for (;;) {
		const char32_t ch = at(pos_);
		if (ch == '<') {
			const size_t open = pos_;
			++pos_;
			while (at(pos_) && is_space(at(pos_))) ++pos_;
			const size_t tag_start = pos_;
			if (!at(pos_)) return fail(error, "The file ends inside a tag.");
			while (!is_space(at(pos_)) && at(pos_) != '>') {
				++pos_;
				if (!at(pos_)) return fail(error, "The file ends inside a tag name.");
			}
			if (at(tag_start) == '/') {
				// Any close tag pops one level; its name is never compared.
				if (!current_)
					return fail(error, "A close tag at the top level: retail crashes loading this file.");
				current_ = current_->parent;
			} else if (starts_with(tag_start, "!--")) {
				// The terminator search starts where the tag name stopped, so <!--x-->
				// with no whitespace runs on to the next "-->".
				while (at(pos_)) {
					while (at(pos_) && at(pos_) != '-') ++pos_;
					if (starts_with(pos_, "-->")) {
						pos_ += 2;
						break;
					}
					if (at(pos_)) ++pos_;
				}
				if (!at(pos_)) {
					note("The file ends inside a comment: retail's comment scan reads past the end of the text.");
					return true;
				}
			} else if (at(tag_start) == 'R' && starts_with(tag_start, "RAW_TEXT")) {
				// Case-sensitive, a prefix: the body up to </RAW_TEXT goes verbatim into
				// the current element's text, its first character before the check.
				if (!current_) return fail(error, "RAW_TEXT outside any element.");
				++pos_;
				if (!at(pos_)) return fail(error, "The file ends inside RAW_TEXT.");
				for (;;) {
					current_->text.push_back(at(pos_));
					++pos_;
					if (at(pos_) == '<' && at(pos_ + 1) == '/' && starts_with(pos_ + 2, "RAW_TEXT")) break;
					if (!at(pos_)) return fail(error, "The file ends inside RAW_TEXT.");
				}
				while (!is_space(at(pos_)) && at(pos_) != '>') {
					++pos_;
					if (!at(pos_)) return fail(error, "The file ends inside RAW_TEXT.");
				}
			} else {
				auto node = std::make_unique<Node>();
				node->tag.assign(s_, tag_start, pos_ - tag_start);
				node->parent = current_;
				node->line = line_at(open);
				Node *raw = node.get();
				if (!current_ && iequals(raw->tag, "SCREEN")) raw->screen_ordinal = screens_++;
				(current_ ? current_->children : out_.roots).push_back(std::move(node));
				current_ = raw;
				created_ = raw;
			}
			if (!attributes()) return true;
		} else if (ch == '&') {
			const size_t at_amp = pos_;
			const char32_t decoded = entity();
			if (decoded) {
				if (!current_) {
					pos_ = at_amp;
					return fail(error, "An entity outside any element: retail crashes loading this file.");
				}
				current_->text.push_back(decoded);
			}
			if (pos_ > n_) {
				note("The file ends inside an entity: retail reads past the end of the text.");
				return true;
			}
		} else if (!current_) {
			if (!is_space(ch)) return fail(error, "Text outside any element: retail loads nothing from this file.");
			while (at(pos_) && is_space(at(pos_))) ++pos_;
		} else {
			current_->text.push_back(ch);
			++pos_;
		}
		if (!at(pos_)) return true;
	}
}

} // namespace

bool Attribute::token(Text &out) const {
	out.clear();
	if (!has_value) return false;
	size_t i = 0;
	while (i < value.size() && value[i] == '"') ++i;
	size_t j = i;
	while (j < value.size() && value[j] != '"') ++j;
	out.assign(value, i, j - i);
	return j > i;
}

const Attribute *Node::attr(const char *name) const {
	for (const Attribute &a : attributes)
		if (iequals(a.name, name)) return &a;
	return nullptr;
}

const Attribute *Node::last_attr(const char *name) const {
	for (auto it = attributes.rbegin(); it != attributes.rend(); ++it)
		if (iequals(it->name, name)) return &*it;
	return nullptr;
}

bool parse(const Text &text, Document &out, std::string &error) {
	out.roots.clear();
	out.notes.clear();
	Reader reader(text, out);
	return reader.run(error);
}

bool is_space(char32_t c) {
	if (c < 0x100) return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0xA0;
	return c == 0x1680 || c == 0x180E || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
	       c == 0x202F || c == 0x205F || c == 0x3000;
}

bool iequals(const Text &a, const char *ascii) {
	size_t i = 0;
	for (; ascii[i]; ++i)
		if (i >= a.size() || fold(a[i]) != fold(char32_t(static_cast<unsigned char>(ascii[i])))) return false;
	return i == a.size();
}

long wcstol(const Text &s, int base) {
	return static_cast<long>(static_cast<int32_t>(static_cast<uint32_t>(wcstoxl(s.data(), s.data() + s.size(), base, false))));
}

unsigned long wcstoul(const Text &s, int base) { return wcstoxl(s.data(), s.data() + s.size(), base, true); }

Text widen(const char *ascii) {
	Text out;
	for (size_t i = 0; ascii[i]; ++i) out.push_back(char32_t(static_cast<unsigned char>(ascii[i])));
	return out;
}

std::string ascii(const Text &text) {
	std::string out;
	for (char32_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
	return out;
}

std::string path_key(const Node &node, const Attribute *attribute) {
	auto upper = [](std::string s) {
		for (char &c : s)
			if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
		return s;
	};
	std::vector<const Node *> chain;
	for (const Node *n = &node; n; n = n->parent) chain.push_back(n);
	std::string key;
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		const Node &n = **it;
		key += "/" + upper(ascii(n.tag));
		if (n.screen_ordinal >= 0) {
			key += "[" + std::to_string(n.screen_ordinal) + "]";
		} else if (iequals(n.tag, "WINDOW")) {
			Text name;
			if (const Attribute *a = n.attr("NAME")) a->token(name);
			key += "[" + upper(ascii(name)) + "]";
		}
	}
	if (attribute) key += "@" + upper(ascii(attribute->name));
	return key;
}

} // namespace opennova::mnu_xml
