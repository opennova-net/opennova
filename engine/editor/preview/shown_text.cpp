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

// `text` cut at its LFs: each line without its line end, at least one (an empty text is one empty
// line).
std::vector<std::u32string_view> lines_of(std::u32string_view text) {
	std::vector<std::u32string_view> lines;
	size_t at = 0;
	for (;;) {
		const size_t lf = text.find(U'\n', at);
		if (lf == std::u32string_view::npos) {
			lines.push_back(text.substr(at));
			return lines;
		}
		lines.push_back(text.substr(at, lf - at));
		at = lf + 1;
	}
}

// One change of the shown text: `removed` characters from `from` gave way to `inserted`.
struct Hunk {
	size_t from = 0;
	size_t removed = 0;
	std::u32string_view inserted;
};

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
	const ShownDiff whole = ShownText::diff(text_, control, caret);
	if (whole.empty()) return true;
	// The hunks: the whole change as one, or, where it spans lines and keeps their count (an indent or
	// a comment of several lines: a change at several places), one for each line it changes, the line
	// ends between them unchanged and so untouched, as are the hidden bytes of the lines it leaves.
	const std::u32string_view old_mid = std::u32string_view(text_).substr(whole.from, whole.removed);
	const std::u32string_view new_mid = control.substr(whole.from, whole.inserted);
	const std::vector<std::u32string_view> old_lines = lines_of(old_mid), new_lines = lines_of(new_mid);
	std::vector<Hunk> hunks;
	if (old_lines.size() > 1 && old_lines.size() == new_lines.size()) {
		size_t old_at = whole.from, new_at = whole.from;
		for (size_t i = 0; i < old_lines.size(); ++i) {
			if (old_lines[i] != new_lines[i]) {
				// Where a keystroke leaves the caret picks among runs of one size, in the line that holds it.
				const bool caret_here = caret >= new_at && caret <= new_at + new_lines[i].size();
				const ShownDiff within = ShownText::diff(old_lines[i], new_lines[i], caret_here ? caret - new_at : SIZE_MAX);
				hunks.push_back({ old_at + within.from, within.removed, new_lines[i].substr(within.from, within.inserted) });
			}
			old_at += old_lines[i].size() + 1;
			new_at += new_lines[i].size() + 1;
		}
	} else {
		hunks.push_back({ whole.from, whole.removed, new_mid });
	}
	// Each span: the text each character in the code page, an LF as the document's line end; a
	// character it has no byte for refuses the edit whole. The bytes it covers run from the first
	// changed character's to the last's (the bytes not shown between them with them; those at either
	// edge stay where they are); an insertion goes in after the bytes not shown before the character it
	// precedes. Its place is the line it starts on (the line ends shown before it, counted along the
	// text once) and the column within it.
	std::vector<ShownTextSpan> spans;
	std::u32string unstorable;
	size_t line = 1, line_start = 0, scanned = 0;
	for (const Hunk &hunk : hunks) {
		std::string text;
		for (const char32_t cp : hunk.inserted) {
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
		const size_t begin = document_offset(hunk.from);
		const size_t end = hunk.removed ? document_end(hunk.from + hunk.removed - 1) : begin;
		for (; scanned < hunk.from; ++scanned)
			if (text_[scanned] == U'\n') {
				++line;
				line_start = document_end(scanned);
			}
		ShownTextSpan span;
		span.span.line = line;
		span.span.column = begin - line_start + 1;
		span.span.length = end - begin;
		span.text = std::move(text);
		spans.push_back(std::move(span));
	}
	if (!unstorable.empty()) {
		error = "The game's text encoding (Windows-1252) has no byte for";
		for (size_t i = 0; i < unstorable.size(); ++i) error += (i ? ", " : " ") + code_point_name(unstorable[i]);
		error += ": the edit is not taken.";
		return false;
	}
	// The ones further on first: each is then where the plan put it as the spans before it are done.
	std::reverse(spans.begin(), spans.end());
	out.spans = std::move(spans);
	out.shown = whole;
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
