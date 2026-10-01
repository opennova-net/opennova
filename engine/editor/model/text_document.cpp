#include "text_document.h"

#include <algorithm>
#include <utility>

#include <base/io/strutil.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

namespace {

bool refuse(Diagnostic &error, const std::string &path, CoreFinding code, const std::string &message) {
	error = make_finding(code, DiagnosticSeverity::Error, message, path);
	return false;
}

// A whole number that is all of `text`: digits alone.
bool whole(std::string_view text, size_t &out) {
	if (text.empty()) return false;
	for (const char c : text)
		if (c < '0' || c > '9') return false;
	const auto value = strutil::parse_ulong(std::string(text));
	if (!value) return false;
	out = size_t(*value);
	return true;
}

// The group a batch folds in: its gesture's, else a typing burst's (its edits coalesced), else none.
std::string group_of(const Edit &edit) {
	if (edit.gesture) return "g" + std::to_string(edit.gesture);
	return edit.coalesce ? "typing" : std::string();
}

} // namespace

const char *TextSpanEdit::token() const { return kTextSpanToken; }

TextDocument::TextDocument(TextDecode decode) : decode_(decode) {}

std::string_view TextDocument::line(size_t number) const {
	if (number < 1 || number > line_starts_.size()) return {};
	const size_t start = line_starts_[number - 1];
	size_t end = number < line_starts_.size() ? line_starts_[number] : text_.size();
	// Its end: the LF, and a CR before it.
	if (end > start && text_[end - 1] == '\n') {
		--end;
		if (end > start && text_[end - 1] == '\r') --end;
	}
	return std::string_view(text_).substr(start, end - start);
}

bool TextDocument::offset_of(size_t line_number, size_t column, size_t &offset) const {
	if (line_number < 1 || line_number > line_starts_.size() || column < 1) return false;
	if (column - 1 > line(line_number).size()) return false;
	offset = line_starts_[line_number - 1] + column - 1;
	return true;
}

TextSpan TextDocument::span_at(size_t offset, size_t length) const {
	offset = std::min(offset, text_.size());
	// The last line starting at or before the offset.
	const auto after = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
	const size_t index = size_t(after - line_starts_.begin()) - 1;
	TextSpan span;
	span.line = index + 1;
	span.column = offset - line_starts_[index] + 1;
	span.length = length;
	return span;
}

bool TextDocument::span_text(const TextSpan &span, std::string &out) const {
	size_t offset = 0;
	if (!offset_of(span.line, span.column, offset) || span.length > text_.size() - offset) return false;
	out = text_.substr(offset, span.length);
	return true;
}

std::string TextDocument::locator(size_t line_number, size_t column) {
	return std::to_string(line_number) + ":" + std::to_string(column);
}

bool TextDocument::read_locator(const std::string &locator, size_t &line_number, size_t &column) {
	const size_t colon = locator.find(':');
	if (colon == std::string::npos) return false;
	const std::string_view text(locator);
	return whole(text.substr(0, colon), line_number) && whole(text.substr(colon + 1), column) &&
			line_number >= 1 && column >= 1;
}

Edit TextDocument::replace(const TextSpan &span, std::string text, bool coalesce, uint64_t gesture) {
	Edit edit;
	edit.operation = EditOperation::Apply;
	edit.payload = std::make_shared<TextSpanEdit>(span, std::move(text));
	edit.coalesce = coalesce;
	edit.gesture = gesture;
	return edit;
}

void TextDocument::index_lines() {
	line_starts_.assign(1, 0);
	for (size_t i = 0; i < text_.size(); ++i)
		if (text_[i] == '\n') line_starts_.push_back(i + 1);
}

void TextDocument::replace_at(size_t offset, size_t count, const std::string &with) {
	text_.replace(offset, count, with);
	// A line starts after each LF: the starts the replaced characters made go, those after them move
	// with the text, and each LF put in makes one.
	const auto first = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
	const auto last = std::upper_bound(first, line_starts_.end(), offset + count);
	const std::ptrdiff_t delta = std::ptrdiff_t(with.size()) - std::ptrdiff_t(count);
	for (auto it = last; it != line_starts_.end(); ++it) *it = size_t(std::ptrdiff_t(*it) + delta);
	std::vector<size_t> made;
	for (size_t i = 0; i < with.size(); ++i)
		if (with[i] == '\n') made.push_back(offset + i + 1);
	const auto at = line_starts_.erase(first, last);
	line_starts_.insert(at, made.begin(), made.end());
}

bool TextDocument::apply_edits(const std::vector<Edit> &edits, Diagnostic &error) {
	// Each edit done to the text as the ones before it left it; a refusal puts back what the batch
	// did and commits nothing.
	std::vector<TextReplacement> batch;
	const auto put_back = [this, &batch] {
		for (auto it = batch.rbegin(); it != batch.rend(); ++it)
			replace_at(it->offset, it->inserted.size(), it->removed);
	};
	for (const Edit &edit : edits) {
		const auto *span = edit.operation == EditOperation::Apply
				? dynamic_cast<const TextSpanEdit *>(edit.payload.get())
				: nullptr;
		if (!span) {
			put_back();
			return refuse(error, path(), CoreFinding::DocumentPayload,
					"A text document takes only spans of its text replaced.");
		}
		size_t offset = 0;
		if (!offset_of(span->span.line, span->span.column, offset) ||
				span->span.length > text_.size() - offset) {
			put_back();
			return refuse(error, path(), CoreFinding::DocumentSpan,
					"The span " + locator(span->span.line, span->span.column) + " (" +
							std::to_string(span->span.length) +
							" characters) runs outside the text of " + std::to_string(line_count()) +
							" lines.");
		}
		TextReplacement replacement{offset, text_.substr(offset, span->span.length), span->text};
		if (replacement.removed == replacement.inserted) continue; // a span given its own text
		replace_at(offset, replacement.removed.size(), replacement.inserted);
		batch.push_back(std::move(replacement));
	}
	// A batch that changes nothing takes no step (as a Set of the value held takes none).
	if (batch.empty()) return true;
	history_.commit(std::move(batch), group_of(edits.front()));
	return true;
}

void TextDocument::undo_step() {
	history_.undo([this](size_t offset, size_t count, const std::string &with) {
		replace_at(offset, count, with);
	});
}

void TextDocument::redo_step() {
	history_.redo([this](size_t offset, size_t count, const std::string &with) {
		replace_at(offset, count, with);
	});
}

bool TextDocument::changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const {
	if (load_generation != this->load_generation()) return false;
	std::vector<TextRange> ranges;
	if (!history_.changes_since(revision, ranges)) return false;
	TextChanges changes;
	for (const TextRange &range : ranges) changes.spans.push_back(span_at(range.offset, range.length));
	out = std::move(changes);
	return true;
}

SerializeResult TextDocument::serialize() const {
	SerializeResult result;
	if (!encoding_) {
		result.text = text_;
		return result;
	}
	if (!encoding_->encode(text_, result.text, result.issues) && result.issues.empty())
		result.issues.push_back({true, 0, std::string(), std::string(),
				"The text does not go in the form the file is stored in."});
	return result;
}

std::unique_ptr<DocumentBase> TextDocument::snapshot() const {
	return std::unique_ptr<DocumentBase>(new TextDocument(*this));
}

bool TextDocument::read_source(const std::vector<uint8_t> &decoded, bool adopt,
		std::vector<SourceIssue> &issues, Diagnostic &error) {
	std::string text;
	std::shared_ptr<const TextEncoding> encoding;
	if (decode_) {
		std::string message;
		if (!decode_(decoded, text, encoding, issues, message))
			return refuse(error, path(), CoreFinding::DocumentParse,
					message.empty() ? std::string("The file does not read.") : message);
	} else {
		text.assign(decoded.begin(), decoded.end());
	}
	if (!adopt) return true;
	text_ = std::move(text);
	encoding_ = std::move(encoding);
	index_lines();
	history_.reset();
	return true;
}

} // namespace opennova::editor
