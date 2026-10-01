#include <editor/preview/shown_text.h>

#include <algorithm>
#include <cstdint>

#include <base/io/cp1252.h>
#include <editor/model/text_document.h>

namespace opennova::editor {

namespace {

// What a line end the control puts in is written as: CR LF for a type whose game reader ends a line
// at a CR or at a CR LF alone (Save writes every line end so in any case); else as the document's
// first line end, an LF in a text with none.
std::string line_end_of(const TextDocument &document) {
	if (document.line_ends() != TextLineEnds::AsWritten) return "\r\n";
	const std::string &text = document.text();
	const size_t lf = text.find('\n');
	if (lf == std::string::npos) return "\n";
	return lf > 0 && text[lf - 1] == '\r' ? "\r\n" : "\n";
}

// "U+2260" for a character the code page has no byte for.
std::string code_point_name(char32_t cp) {
	static const char digits[] = "0123456789ABCDEF";
	std::string hex;
	for (int shift = 20; shift >= 0; shift -= 4) {
		const char digit = digits[(cp >> shift) & 0xF];
		if (hex.empty() && digit == '0' && shift > 12) continue;
		hex.push_back(digit);
	}
	return "U+" + hex;
}

} // namespace

ShownText::ShownText(const TextDocument &document) : line_end_(line_end_of(document)) {
	const std::string &text = document.text();
	text_.reserve(text.size());
	begin_.clear();
	begin_.reserve(text.size() + 1);
	for (size_t i = 0; i < text.size(); ++i) {
		const uint8_t byte = static_cast<uint8_t>(text[i]);
		if (byte == '\r') {
			// A CR before an LF is the line's end with it (one LF shown, two bytes); a CR alone is text
			// of its line the control cannot hold.
			if (i + 1 < text.size() && text[i + 1] == '\n') {
				crlf_.push_back(text_.size());
				begin_.push_back(i);
				text_.push_back(U'\n');
				++i;
			}
			continue;
		}
		if (byte == 0) continue; // a NUL: no control holds one
		begin_.push_back(i);
		text_.push_back(cp1252_decode_byte(byte));
	}
	begin_.push_back(text.size());
}

size_t ShownText::document_offset(size_t index) const {
	return begin_[std::min(index, text_.size())];
}

size_t ShownText::document_end(size_t index) const {
	const bool crlf = std::binary_search(crlf_.begin(), crlf_.end(), index);
	return begin_[index] + (crlf ? 2 : 1);
}

size_t ShownText::shown_at(size_t document_offset) const {
	return size_t(std::lower_bound(begin_.begin(), begin_.end() - 1, document_offset) - begin_.begin());
}

ShownDiff ShownText::diff(std::u32string_view from, std::u32string_view to, size_t caret) {
	ShownDiff out;
	const size_t n = from.size(), m = to.size();
	const size_t shorter = std::min(n, m);
	size_t prefix = 0;
	while (prefix < shorter && from[prefix] == to[prefix]) ++prefix;
	size_t suffix = 0;
	while (suffix < shorter - prefix && from[n - 1 - suffix] == to[m - 1 - suffix]) ++suffix;
	if (prefix == n && prefix == m) return out; // the same text
	out.from = prefix;
	out.removed = n - prefix - suffix;
	out.inserted = m - prefix - suffix;
	// Runs of this size end at other places when the common suffix reaches further back than this
	// one's: the change slides left over the characters it repeats. It ends at the caret in `to`
	// where it may.
	size_t whole = 0;
	while (whole < shorter && from[n - 1 - whole] == to[m - 1 - whole]) ++whole;
	const size_t slide = std::min(prefix, whole - suffix);
	if (slide && caret >= out.inserted) {
		const size_t wanted = caret - out.inserted;
		if (wanted < prefix) out.from = std::max(prefix - slide, wanted);
	}
	return out;
}

bool ShownText::edit(std::u32string_view control, size_t caret, ShownTextEdit &out, std::string &error) const {
	out = ShownTextEdit();
	const ShownDiff diff = ShownText::diff(text_, control, caret);
	if (diff.empty()) return true;
	// The text: each character in the code page, an LF as the document's line end; a character it
	// has no byte for refuses the edit whole.
	std::string text;
	std::u32string unstorable;
	for (size_t i = diff.from; i < diff.from + diff.inserted; ++i) {
		const char32_t cp = control[i];
		if (cp == U'\n') {
			text += line_end_;
			continue;
		}
		uint8_t byte = 0;
		if (cp == U'\r' || cp == 0 || !cp1252_encode_codepoint(cp, byte)) {
			if (unstorable.find(cp) == std::u32string::npos) unstorable.push_back(cp);
			continue;
		}
		text.push_back(static_cast<char>(byte));
	}
	if (!unstorable.empty()) {
		error = "The game's text encoding (Windows-1252) has no byte for";
		for (size_t i = 0; i < unstorable.size(); ++i) error += (i ? ", " : " ") + code_point_name(unstorable[i]);
		error += ": the edit is not taken.";
		return false;
	}
	// The bytes it covers: from the first changed character's to the last's (the bytes not shown
	// between them with them; those at either edge stay where they are). An insertion goes in after
	// the bytes not shown before the character it precedes.
	const size_t begin = document_offset(diff.from);
	const size_t end = diff.removed ? document_end(diff.from + diff.removed - 1) : begin;
	// Its place: the line it starts on (the line ends shown before it) and the column within it.
	size_t line = 1, line_start = 0;
	for (size_t i = 0; i < diff.from; ++i)
		if (text_[i] == U'\n') {
			++line;
			line_start = document_end(i);
		}
	out.span.line = line;
	out.span.column = begin - line_start + 1;
	out.span.length = end - begin;
	out.text = std::move(text);
	out.shown = diff;
	return true;
}

void ShownText::place_of(std::u32string_view text, size_t offset, size_t &line, size_t &column) {
	offset = std::min(offset, text.size());
	line = 0;
	size_t start = 0;
	for (size_t i = 0; i < offset; ++i)
		if (text[i] == U'\n') {
			++line;
			start = i + 1;
		}
	column = offset - start;
}

size_t ShownText::offset_of(std::u32string_view text, size_t line, size_t column) {
	size_t at = 0;
	for (size_t lines = 0; lines < line; ++lines) {
		const size_t lf = text.find(U'\n', at);
		if (lf == std::u32string_view::npos) return text.size();
		at = lf + 1;
	}
	const size_t lf = text.find(U'\n', at);
	const size_t end = lf == std::u32string_view::npos ? text.size() : lf;
	return std::min(at + column, end);
}

} // namespace opennova::editor
