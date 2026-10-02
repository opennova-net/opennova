#pragma once

#include <godot_cpp/classes/syntax_highlighter.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <vector>

#include <editor/preview/script_viewport.h>

namespace godot {

// The script device's colours (ADR 0046 S13 V10; authoring/script_device): a SyntaxHighlighter over
// the runs its viewport says the document's type's game reader knows as words of its language
// (ScriptViewport::highlights: a script's keywords, its commands and the operands it looks a name up
// for, the WAC compiler's own), each kind its colour, the rest of the text the control's. It
// computes nothing: the device hands it the runs each time its viewport makes them, so a text type
// whose reader the editor has no port of is coloured nowhere.
class ScriptHighlighter : public SyntaxHighlighter {
	GDCLASS(ScriptHighlighter, SyntaxHighlighter)

public:
	// The runs to colour, in the control's places (lines and columns from 0).
	void set_highlights(const std::vector<opennova::editor::ScriptHighlight> &highlights);
	Dictionary _get_line_syntax_highlighting(int32_t p_line) const override;

	// What the device coloured: how many runs, and the kind of the run at a place ("keyword",
	// "command", "operand"; "" for none), for the GUT tests.
	int get_highlight_count() const { return count_; }
	String get_highlight_kind(int p_line, int p_column) const;

protected:
	static void _bind_methods();

private:
	std::vector<std::vector<opennova::editor::ScriptHighlight>> lines_; // the runs, by line
	int count_ = 0;
};

} // namespace godot
