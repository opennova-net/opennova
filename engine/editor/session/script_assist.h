#pragma once

// Help in a script's text (ADR 0046 S15, Events and scripts): what a word being typed may be, what a
// word is, and where it is defined, from the WAC compiler's own tables (its 165 commands with the
// kinds of their parameters [orig: the WAC command table @0x82D290], its keywords, its seven default
// script groups) and the project (the mission of the script's name: its entities by SSN, its areas by
// zone id, its waypoint paths; the text keys, effects and ammo the asset graph knows). Data only: the
// script device draws it (its completion list, its tooltip, its Ctrl+click), the `script_assist` query
// answers it.

#include <cstddef>
#include <string>
#include <vector>

#include <editor/model/value.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

struct SessionView;
class TextDocument;

// One thing a word being typed may be: what the list shows, what takes the word's place, its kind
// ("command", "keyword", "entity", "area", "path", "group", "text", "effect", "ammo") and a line on it.
struct ScriptCompletion {
	std::string label;
	std::string insert;
	std::string kind;
	std::string detail;
};
// What may complete the word at a place: the word as typed so far and where it starts (its column,
// from 1, on the place's line), what is expected there in words ("SSNarea's 2nd parameter: an area"),
// and the items, those whose insert starts with the typed word, the commands and keywords where a
// statement goes, else what the command's parameter there takes; at most `kMostCompletions`.
struct ScriptCompletions {
	size_t column = 1;
	std::string typed;
	std::string expected;
	std::vector<ScriptCompletion> items;
};
inline constexpr size_t kMostCompletions = 300;
// `line` and `column` from 1, the caret's (the word ends there); `held`, where given, the line's text as
// a control holds it now, a keystroke ahead of the document (whose change comes at its next deferred
// call), a character a byte.
ScriptCompletions script_completions(const SessionView &view, const TextDocument &script, size_t line, size_t column,
                                     const std::string *held = nullptr);

// What the word at a place is, in words: a command (its parameters by kind, a condition or an
// action), a keyword (what it opens or ends), an entity by SSN, an area by zone id or a waypoint path
// where a command's parameter takes one, a group, a text key, an effect, an ammo, a variable. False
// where no word stands there or none is known.
struct ScriptHover {
	size_t column = 1, length = 0; // the word's place on its line
	std::string word;
	std::string text;
};
bool script_hover(const SessionView &view, const TextDocument &script, size_t line, size_t column, ScriptHover &out);

// Where the word at a place is defined, as the request that goes there: a RUN's file opened, an
// entity or an area of the mission selected, a text key, an effect or an ammo at its record. False
// where it names nothing the project defines.
bool script_definition(const SessionView &view, const TextDocument &script, size_t line, size_t column,
                       EditorRequest &out);

// The script a mission's name finds (the game's sidecar of the mission, runtime/mission/mission_sidecars:
// <stem>.wac, which WacScript_InitAndLoad compiles with it), for the mission's Script button: its file
// name, the project's file of that name where the project holds it ("" where not), and where a new one
// goes (beside the mission).
struct MissionScript {
	std::string name;
	std::string path;
	std::string create_at;
};
MissionScript mission_script(const SessionView &view, const std::string &mission_path);

} // namespace opennova::editor
