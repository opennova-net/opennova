#include <editor/preview/script_viewport.h>

#include <algorithm>
#include <utility>

#include <editor/documents/document_types.h>
#include <editor/documents/script_type.h>
#include <formats/configfile/config_file.h>
#include <editor/model/document_base.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/text_burst.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/findings_index.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view/view_events.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

int rank(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Error: return 2;
	case DiagnosticSeverity::Warning: return 1;
	case DiagnosticSeverity::Info: break;
	}
	return 0;
}

// The size its device draws at where no canvas sizes it (a headless Shell's): the model's.
constexpr ViewportState kHeadlessSize{ 800, 600 };

// The `options` of a change read into `held`: {assist: {op, line?, column?}} (a place from 1; none for op
// none). False with `error` for anything else.
bool read_assist(const JsonValue &options, ScriptAssistAsk &held, std::string &error) {
	if (!options.is_object()) {
		error = "\"options\" is an object.";
		return false;
	}
	for (const io::JsonMember &member : options.object) {
		if (member.key != "assist") {
			error = "Unknown script option \"" + member.key + "\" (it takes assist).";
			return false;
		}
		const JsonValue &assist = member.value;
		if (!assist.is_object()) {
			error = "options.assist is an object {op, line, column}.";
			return false;
		}
		ScriptAssistAsk out;
		out.serial = held.serial;
		const JsonValue *op = assist.get("op");
		if (!op || !op->is_string() || !script_assist_op_from_token(op->string, out.op)) {
			error = "options.assist.op is none, complete or hover.";
			return false;
		}
		for (const io::JsonMember &place : assist.object) {
			if (place.key == "op") continue;
			int64_t value = 0;
			if ((place.key != "line" && place.key != "column") || !io::json_whole_in(place.value, 1.0, 1e9, value)) {
				error = "options.assist takes op, line and column (each a place from 1).";
				return false;
			}
			(place.key == "line" ? out.line : out.column) = size_t(value);
		}
		if (out.op != ScriptAssistOp::None && (!out.line || !out.column)) {
			error = "options.assist needs line and column (from 1) for " + op->string + ".";
			return false;
		}
		held = out;
	}
	return true;
}

} // namespace

const char *script_assist_op_token(ScriptAssistOp op) {
	switch (op) {
	case ScriptAssistOp::None: return "none";
	case ScriptAssistOp::Complete: return "complete";
	case ScriptAssistOp::Hover: return "hover";
	}
	return "none";
}

bool script_assist_op_from_token(const std::string &token, ScriptAssistOp &out) {
	for (const ScriptAssistOp op : { ScriptAssistOp::None, ScriptAssistOp::Complete, ScriptAssistOp::Hover })
		if (token == script_assist_op_token(op)) {
			out = op;
			return true;
		}
	return false;
}

std::string script_read_only_reason(const SessionView &view, const DocumentBase &document) {
	if (document.blocked()) {
		for (const SourceIssue &issue : document.issues())
			if (issue.blocks) return issue.message;
		return "This file is held read only: its text cannot be written back as it is.";
	}
	if (!view.allows(EditorRequestKind::EditRecord))
		return "Read only while an operation holds the documents" +
				(view.activity.operation.label.empty() ? std::string() : " (" + view.activity.operation.label + ")") + ".";
	return std::string();
}

const char *script_view_status_token(ScriptViewStatus status) {
	switch (status) {
	case ScriptViewStatus::NoText: return "no_text";
	case ScriptViewStatus::Ready: return "ready";
	}
	return "no_text";
}

std::string ScriptMark::tip() const {
	std::string out;
	for (const ScriptMarkFinding &finding : findings) {
		if (!out.empty()) out += "\n";
		out += finding.message;
	}
	return out;
}

std::string ScriptMark::note() const {
	const ScriptMarkFinding *worst = nullptr;
	for (const ScriptMarkFinding &finding : findings)
		if (!worst || rank(finding.severity) > rank(worst->severity)) worst = &finding;
	if (!worst) return std::string();
	return worst->note + (findings.size() > 1 ? " (+" + std::to_string(findings.size() - 1) + " more)" : std::string());
}

std::string first_sentence(const std::string &message) {
	const size_t stop = message.find(". ");
	return stop == std::string::npos ? message : message.substr(0, stop + 1);
}

namespace {

// The ConfigFile reader's words over a text it reads (a credits file's): each section's label
// line from its '[' to its ']' (a keyword), each entry's key (a command), each value as written (an operand), at
// the places the reader read them [orig: ConfigFile_ParseText @ 0x7608A0; ConfigFile_BuildSectionLabels
// @ 0x75DF20; ini_parse_section_entries @ 0x75DB80; ConfigFile_ParseValues @ 0x7606F0]. The reader is handed
// the text as Save writes it, an LF alone CR LF (the reader ends a line at CR LF alone: TextLineEnds::CrLf), each
// place it read taken back to the document's.
void config_file_highlights(const TextDocument &document, std::vector<TextHighlight> &out) {
	const std::string &held = document.text();
	std::string text;
	std::vector<size_t> added; // the written text's offsets of each CR added, in order
	text.reserve(held.size() + held.size() / 16);
	for (size_t i = 0; i < held.size(); ++i) {
		if (held[i] == '\n' && (i == 0 || held[i - 1] != '\r')) {
			added.push_back(text.size());
			text.push_back('\r');
		}
		text.push_back(held[i]);
	}
	const auto held_at = [&](size_t offset) {
		return offset - size_t(std::lower_bound(added.begin(), added.end(), offset) - added.begin());
	};
	const std::vector<configfile::ConfigSection> sections =
			configfile::parse_config_text(reinterpret_cast<const uint8_t *>(text.data()), text.size());
	const auto add = [&](TextHighlightKind kind, size_t offset, size_t length) {
		if (length == 0 || offset + length > text.size()) return;
		const size_t from = held_at(offset), to = held_at(offset + length);
		if (to <= from || to > held.size()) return;
		TextHighlight highlight;
		highlight.kind = kind;
		highlight.span = document.span_at(from, to - from);
		out.push_back(highlight);
	};
	for (const configfile::ConfigSection &section : sections) {
		const size_t close = text.find(']', section.offset);
		const size_t line_end = text.find_first_of("\r\n", section.offset);
		if (close != std::string::npos && (line_end == std::string::npos || close < line_end))
			add(TextHighlightKind::Keyword, section.offset, close - section.offset + 1);
		for (const configfile::ConfigEntry &entry : section.entries) {
			// The key: the line's text before its '=' with its spaces trimmed, as the reader keys it.
			const size_t equals = text.find('=', entry.offset);
			if (equals != std::string::npos) {
				size_t from = entry.offset, to = equals;
				while (from < to && (text[from] == ' ' || text[from] == '\t')) ++from;
				while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t')) --to;
				add(TextHighlightKind::Command, from, to - from);
			}
			for (const configfile::ConfigValue &value : entry.values)
				add(TextHighlightKind::Operand, value.offset, value.text.size());
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const TextHighlight &a, const TextHighlight &b) {
		return a.span.line != b.span.line ? a.span.line < b.span.line : a.span.column < b.span.column;
	});
}

// Each text type whose reader's port says the words it read, and the highlighter that asks it.
struct HighlighterRow {
	DocumentTypeId type;
	ScriptHighlighter highlights;
};
constexpr HighlighterRow kHighlighters[] = {
	{ DocumentTypeId::Script, script_highlights },
	{ DocumentTypeId::Credits, config_file_highlights },
};

} // namespace

ScriptHighlighter script_highlighter(DocumentTypeId type) {
	for (const HighlighterRow &row : kHighlighters)
		if (row.type == type) return row.highlights;
	return nullptr;
}

ScriptViewport::ScriptViewport(std::string path) :
		ViewportModel(ViewportKind::Script, std::move(path), kHeadlessSize) {}

std::unique_ptr<ViewportModel> ScriptViewport::make(const std::string &path) {
	return std::make_unique<ScriptViewport>(path);
}

ViewportStatus ScriptViewport::status() const {
	return reason_ == ScriptViewStatus::Ready ? ViewportStatus::Ready : ViewportStatus::Empty;
}

std::string ScriptViewport::message() const {
	switch (reason_) {
	case ScriptViewStatus::NoText: return "No text document is open here.";
	case ScriptViewStatus::Ready: break;
	}
	return std::string();
}

void ScriptViewport::make_text_(const TextDocument &document) {
	shown_ = ShownText(document);
	++texts_made_;
	references_.clear();
	words_.clear();
	const DocumentType *type = document_type_for(document.kind());
	if (type && type->references) type->references(document, references_);
	if (const ScriptHighlighter highlights = type ? script_highlighter(type->id) : nullptr) highlights(document, words_);
	// The words in the control's places: its lines are the document's, its columns the characters it
	// holds from the line's start.
	highlights_.clear();
	for (const TextHighlight &word : words_) {
		size_t offset = 0, line_start = 0;
		if (!document.offset_of(word.span.line, word.span.column, offset) ||
				!document.offset_of(word.span.line, 1, line_start))
			continue;
		const size_t at = shown_.shown_at(offset);
		const size_t end = shown_.shown_at(offset + word.span.length);
		if (end <= at) continue;
		ScriptHighlight highlight;
		highlight.line = word.span.line - 1;
		highlight.column = at - shown_.shown_at(line_start);
		highlight.length = end - at;
		highlight.kind = word.kind;
		highlights_.push_back(highlight);
	}
	++highlights_serial_;
}

void ScriptViewport::make_marks_(const SessionView &view, const TextDocument &document) {
	marks_.clear();
	// The findings about a place in the document's content, by line: a finding about the file as a
	// whole (its row's place) is no mark, nor one past the text's lines.
	for (const Diagnostic &d : view.findings.diagnostics) {
		if (d.asset != path() || !d.line || d.line > document.line_count()) continue;
		if (d.row() && d.row()->place == FindingPlace::File) continue;
		auto mark = std::find_if(marks_.begin(), marks_.end(), [&](const ScriptMark &m) { return m.line == d.line; });
		if (mark == marks_.end()) {
			ScriptMark made;
			made.line = d.line;
			made.severity = d.severity;
			marks_.push_back(std::move(made));
			mark = marks_.end() - 1;
		} else if (rank(d.severity) > rank(mark->severity)) {
			mark->severity = d.severity;
		}
		mark->findings.push_back({d.code(), d.severity, d.column, d.message, first_sentence(d.message)});
	}
	std::stable_sort(marks_.begin(), marks_.end(), [](const ScriptMark &a, const ScriptMark &b) { return a.line < b.line; });
	++marks_serial_;
}

void ScriptViewport::place_reveal_(const TextDocument &document) {
	ScriptReveal reveal;
	reveal.seq = pending_seq_;
	pending_seq_ = 0;
	// The place, held within the text: a line past the last is the last's, a column past a line's end
	// its end.
	const size_t line = std::min(std::max<size_t>(pending_line_, 1), document.line_count());
	const size_t column = std::min(std::max<size_t>(pending_column_, 1), document.line(line).size() + 1);
	size_t offset = 0, line_start = 0;
	document.offset_of(line, column, offset);
	document.offset_of(line, 1, line_start);
	// Selected where a reference starts there (a Go to's span), else a word of the language (a compile
	// report's token); the caret alone elsewhere.
	size_t length = 0;
	for (const TextReference &reference : references_)
		if (reference.span.line == line && reference.span.column == column) length = reference.span.length;
	if (!length)
		for (const TextHighlight &word : words_)
			if (word.span.line == line && word.span.column == column) length = word.span.length;
	reveal.span.line = line;
	reveal.span.column = column;
	reveal.span.length = length;
	const size_t start = shown_.shown_at(line_start);
	const size_t at = shown_.shown_at(offset);
	reveal.line = reveal.end_line = line - 1;
	reveal.column = at - start;
	reveal.end_column = shown_.shown_at(offset + length) - start;
	reveal_ = reveal;
}

ViewportAction ScriptViewport::follow_(const ViewportInput &input, PreviewClock &) {
	const TextDocument *document = input.document ? text_of(*input.document) : nullptr;
	if (!document) {
		reason_ = ScriptViewStatus::NoText;
		detail_.clear();
		shown_ = ShownText();
		marks_.clear();
		highlights_.clear();
		references_.clear();
		words_.clear();
		editable_ = false;
		read_only_.clear();
		marks_key_ = RevisionKey();
		shown_none();
		return ViewportAction::Clear;
	}
	// The document first shown here, or another one at the path: the control takes its text anew.
	const bool anew = reason_ != ScriptViewStatus::Ready || input.change == ChangeClass::Loaded;
	reason_ = ScriptViewStatus::Ready;
	detail_.clear();
	ViewportAction action = ViewportAction::Keep;
	const auto moved = [&action] {
		if (action == ViewportAction::Keep) action = ViewportAction::Update;
	};
	if (anew) {
		make_text_(*document);
		action = ViewportAction::Rebuild;
	} else if (input.change != ChangeClass::None) {
		// The document changed since the last follow, however the follow says it (an edit, an undo or a
		// redo, the same document read again: Unknown now, and S13 V8's Changed and Unknown): its text is
		// made again and the control takes it where it holds another. The control shows the whole text, so
		// it needs no more than that the text moved: every class but a first follow reads alike.
		make_text_(*document);
		moved();
	}
	// The marks, made again when the findings moved (or the text was read anew).
	const RevisionKey key = FindingsIndex::cache_key(input.view);
	if (anew || key != marks_key_) {
		make_marks_(input.view, *document);
		marks_key_ = key;
		moved();
	}
	// Whether it takes an edit now: the document not held read only and no operation holding the
	// documents (the busy gate).
	std::string read_only = script_read_only_reason(input.view, *document);
	if (anew || read_only.empty() != editable_ || read_only != read_only_) {
		editable_ = read_only.empty();
		read_only_ = std::move(read_only);
		moved();
	}
	if (pending_seq_) {
		place_reveal_(*document);
		moved();
	}
	// The help asked at a place: closed by an edit of the document (as typing on closes the list), else
	// placed within the text once per ask.
	if (assist_.op != ScriptAssistOp::None && !anew && input.change != ChangeClass::None) {
		assist_ = ScriptAssistAsk{ ScriptAssistOp::None, 0, 0, 0, 0, assist_.serial + 1 };
	}
	if (assist_.serial != assist_placed_) {
		assist_placed_ = assist_.serial;
		if (assist_.op != ScriptAssistOp::None) {
			assist_.line = std::min(std::max<size_t>(assist_.line, 1), document->line_count());
			assist_.column = std::min(std::max<size_t>(assist_.column, 1), document->line(assist_.line).size() + 1);
			size_t offset = 0, line_start = 0;
			document->offset_of(assist_.line, assist_.column, offset);
			document->offset_of(assist_.line, 1, line_start);
			assist_.shown_line = assist_.line - 1;
			assist_.shown_column = shown_.shown_at(offset) - shown_.shown_at(line_start);
		}
		moved();
	}
	shown(*document);
	return action;
}

void ScriptViewport::receive(const ViewEvent &event) {
	size_t line = 0, column = 0;
	if (event.kind != ViewEventKind::RevealText || event.path != path() ||
			!TextDocument::read_locator(event.locator, line, column))
		return;
	pending_seq_ = event.seq;
	pending_line_ = line;
	pending_column_ = column;
}

bool ScriptViewport::edit(const ViewportContext &context, std::u32string_view control, size_t caret, double now,
		TextBurst &burst, CanvasRequests &out, std::string &error) const {
	const TextDocument *document = context.input.document ? text_of(*context.input.document) : nullptr;
	if (!document) {
		error = "No text document is open at " + path() + ".";
		return false;
	}
	if (!context.editable()) {
		error = script_read_only_reason(context.input.view, *document);
		if (error.empty()) error = "The document takes no edit now.";
		return false;
	}
	ShownTextEdit planned;
	if (!ShownText(*document).edit(control, caret, planned, error)) return false;
	if (planned.empty()) return true;
	// One burst a run of typing: an edit away from where the last left the text begins another, and so
	// does one at several places (an indent of lines, a comment of lines), which is a step of its own:
	// it goes out under a token of its own and ends its burst with it.
	const bool several = planned.spans.size() > 1;
	if (burst.open() && (several || !burst.continues(path(), planned.shown.from, planned.shown.removed))) burst.end(out);
	const uint64_t token = burst.token();
	std::vector<Edit> edits;
	edits.reserve(planned.spans.size());
	for (ShownTextSpan &each : planned.spans)
		edits.push_back(TextDocument::replace(each.span, std::move(each.text), false, token));
	out.request(request::edit_record(path(), std::move(edits)));
	burst.sent(path(), planned.shown.from + planned.shown.inserted, now);
	if (several) burst.end(out);
	return true;
}

std::unique_ptr<CanvasHalf> ScriptViewport::make_canvas() const {
	return nullptr;
}

ViewportHit ScriptViewport::hit(const ViewportContext &, float, float) const {
	// Nothing lies under a point of a control's rect that the session knows (the viewport query refuses
	// a hit on a kind with no canvas before it asks).
	return ViewportHit();
}

bool ScriptViewport::handle_point(const ViewportContext &, NodeId, const std::string &, float &, float &,
		std::string &error) const {
	error = "A script viewport has no canvas, so no handle: its device edits its text itself (send its spans "
			"with edit_record).";
	return false;
}

bool ScriptViewport::drag(const ViewportContext &, const ViewportDrag &, CanvasRequests &, std::string &error) const {
	error = "A script viewport has no canvas, so nothing to drag: its device edits its text itself (send its "
			"spans with edit_record).";
	return false;
}

bool ScriptViewport::command(const ViewportContext &, const std::string &name, const std::vector<NodeId> &,
		CanvasRequests &, std::string &error) const {
	error = "A script viewport has no canvas and no command \"" + name + "\": its device edits its text itself "
			"(send its spans with edit_record).";
	return false;
}

io::JsonValue ScriptViewport::options_json() const {
	JsonValue out = JsonValue::make_object();
	JsonValue assist = JsonValue::make_object();
	assist.set("op", json_string(script_assist_op_token(assist_.op)));
	if (assist_.op != ScriptAssistOp::None) {
		assist.set("line", json_number(double(assist_.line)));
		assist.set("column", json_number(double(assist_.column)));
	}
	assist.set("serial", json_number(double(assist_.serial)));
	out.set("assist", std::move(assist));
	return out;
}

io::JsonValue ScriptViewport::body_json(const ViewportInput &input) const {
	const TextDocument *document = input.document ? text_of(*input.document) : nullptr;
	JsonValue out = JsonValue::make_object();
	out.set("line_count", json_number(double(document ? document->line_count() : 0)));
	out.set("editable", JsonValue::make_bool(editable_));
	out.set("read_only", json_string(read_only_));
	out.set("highlights", json_number(double(highlights_.size())));
	out.set("marks", json_number(double(marks_.size())));
	if (reveal_.seq) {
		JsonValue reveal = JsonValue::make_object();
		reveal.set("seq", json_number(double(reveal_.seq)));
		reveal.set("line", json_number(double(reveal_.span.line)));
		reveal.set("column", json_number(double(reveal_.span.column)));
		reveal.set("length", json_number(double(reveal_.span.length)));
		out.set("reveal", std::move(reveal));
	} else {
		out.set("reveal", JsonValue::make_null());
	}
	return out;
}

io::JsonValue ScriptViewport::items_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_array();
	for (const ScriptMark &mark : marks_) {
		JsonValue item = JsonValue::make_object();
		item.set("line", json_number(double(mark.line)));
		item.set("severity", json_string(diagnostic_severity_label(mark.severity)));
		JsonValue findings = JsonValue::make_array();
		for (const ScriptMarkFinding &finding : mark.findings) {
			JsonValue entry = JsonValue::make_object();
			entry.set("code", json_string(finding.code));
			entry.set("severity", json_string(diagnostic_severity_label(finding.severity)));
			entry.set("column", json_number(double(finding.column)));
			entry.set("message", json_string(finding.message));
			entry.set("note", json_string(finding.note));
			findings.push(std::move(entry));
		}
		item.set("findings", std::move(findings));
		item.set("note", json_string(mark.note()));
		out.push(std::move(item));
	}
	return out;
}

bool ScriptViewport::takes_(const std::string &member) const {
	return member == "options";
}

bool ScriptViewport::check_(const io::JsonValue &json, std::string &error) const {
	const JsonValue *options = json.get("options");
	ScriptAssistAsk held = assist_;
	return !options || read_assist(*options, held, error);
}

// Each ask a new serial, the same place asked again too (the device shows it again).
void ScriptViewport::apply_(const io::JsonValue &json, PreviewClock &) {
	const JsonValue *options = json.get("options");
	ScriptAssistAsk held = assist_;
	std::string error;
	if (!options || !options->get("assist") || !read_assist(*options, held, error)) return;
	held.serial = assist_.serial + 1;
	assist_ = held;
}

bool ScriptViewport::report_(const ViewportDeviceReport &report) {
	// The device took this ask (placed by the last follow) and shows it no longer: closed, as the session says.
	if (assist_.op == ScriptAssistOp::None || report.assist_serial != assist_.serial || assist_placed_ != assist_.serial ||
	    report.assist_shown)
		return false;
	assist_ = ScriptAssistAsk{ ScriptAssistOp::None, 0, 0, 0, 0, assist_.serial + 1 };
	return true;
}

} // namespace opennova::editor
