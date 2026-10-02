#include "authoring/script_highlighter.h"

#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/variant/color.hpp>

namespace godot {

namespace {

using opennova::editor::TextHighlightKind;

// The editor's own colours for a script's words (its UI's, not the game's look).
Color kind_color(TextHighlightKind kind) {
	switch (kind) {
	case TextHighlightKind::Keyword: return Color(1.0f, 0.44f, 0.52f);
	case TextHighlightKind::Command: return Color(0.34f, 0.70f, 1.0f);
	case TextHighlightKind::Operand: return Color(0.64f, 1.0f, 0.69f);
	}
	return Color(1.0f, 1.0f, 1.0f);
}

const char *kind_token(TextHighlightKind kind) {
	switch (kind) {
	case TextHighlightKind::Keyword: return "keyword";
	case TextHighlightKind::Command: return "command";
	case TextHighlightKind::Operand: return "operand";
	}
	return "";
}

} // namespace

void ScriptHighlighter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_highlight_count"), &ScriptHighlighter::get_highlight_count);
	ClassDB::bind_method(D_METHOD("get_highlight_kind", "line", "column"), &ScriptHighlighter::get_highlight_kind);
}

void ScriptHighlighter::set_highlights(const std::vector<opennova::editor::ScriptHighlight> &highlights) {
	lines_.clear();
	count_ = int(highlights.size());
	for (const opennova::editor::ScriptHighlight &highlight : highlights) {
		if (highlight.line >= lines_.size()) lines_.resize(highlight.line + 1);
		lines_[highlight.line].push_back(highlight);
	}
	clear_highlighting_cache();
}

Dictionary ScriptHighlighter::_get_line_syntax_highlighting(int32_t p_line) const {
	Dictionary out;
	if (p_line < 0 || size_t(p_line) >= lines_.size() || lines_[size_t(p_line)].empty()) return out;
	TextEdit *edit = get_text_edit();
	const Color plain = edit ? edit->get_theme_color("font_color") : Color(1.0f, 1.0f, 1.0f);
	// Each run its kind's colour from its first column, the control's own from its end on.
	for (const opennova::editor::ScriptHighlight &highlight : lines_[size_t(p_line)]) {
		Dictionary run;
		run["color"] = kind_color(highlight.kind);
		out[int64_t(highlight.column)] = run;
		const int64_t end = int64_t(highlight.column + highlight.length);
		if (!out.has(end)) {
			Dictionary after;
			after["color"] = plain;
			out[end] = after;
		}
	}
	return out;
}

String ScriptHighlighter::get_highlight_kind(int p_line, int p_column) const {
	if (p_line < 0 || size_t(p_line) >= lines_.size() || p_column < 0) return String();
	for (const opennova::editor::ScriptHighlight &highlight : lines_[size_t(p_line)])
		if (size_t(p_column) >= highlight.column && size_t(p_column) < highlight.column + highlight.length)
			return String(kind_token(highlight.kind));
	return String();
}

} // namespace godot
