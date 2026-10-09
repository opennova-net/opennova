#include <editor/session/script_assist.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_set>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_table.h>
#include <editor/documents/script_type.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/text_document.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/mission_params.h>
#include <formats/wac/command.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/wac/wac_lexis.h>

namespace opennova::editor {

namespace {

using wac::ParamType;

// --- the text ------------------------------------------------------------------------------------

// The characters that end a token [orig: Script_Compile's tokenizer @0x4F3412..0x4F345A: a blank, ';',
// ',' or an operator]; a '"' opens a string.
bool ends_token(char c) {
	return static_cast<unsigned char>(c) <= ' ' || std::strchr(";,()+-*/%^=!<>&|~\"[]", c) != nullptr;
}

struct Token {
	size_t from = 0, to = 0; // offsets in the line, [from, to)
	std::string text;
};

// The line's tokens before `end` (a comment ends the line: wac::wac_comment_starts); `comment` set where
// `end` lies in one.
std::vector<Token> tokens_of(std::string_view line, size_t end, bool &comment) {
	std::vector<Token> out;
	comment = false;
	size_t i = 0;
	while (i < line.size() && i < end) {
		const char c = line[i];
		if (wac::wac_comment_starts(c, i + 1 < line.size() ? line[i + 1] : '\0')) {
			comment = true;
			return out;
		}
		if (c == '"') {
			// A string runs to its closing quote.
			size_t close = line.find('"', i + 1);
			if (close == std::string_view::npos) close = line.size();
			i = close + 1;
			continue;
		}
		if (ends_token(c)) {
			++i;
			continue;
		}
		Token token;
		token.from = i;
		while (i < line.size() && !ends_token(line[i])) ++i;
		token.to = i;
		token.text = std::string(line.substr(token.from, token.to - token.from));
		out.push_back(token);
	}
	return out;
}

bool contains_nocase(const std::string &text, const std::string &part) {
	return part.empty() || strutil::to_upper(text).find(strutil::to_upper(part)) != std::string::npos;
}

// --- the language --------------------------------------------------------------------------------

// What each of the language's keywords does, in wac::kWacKeywords' order [orig: the WAC help text,
// WacCmd_Help @0x4F6DE0].
constexpr const char *kKeywordWords[] = {
	"IF triggers THEN actions END: the actions run while the triggers hold (named: IF [name] ...).", // IF
	"Starts an IF's actions.", // THEN
	"Starts the actions that run while an IF's triggers do not hold.", // ELSE
	"ELSEIF triggers THEN actions: another test after an IF's.", // ELSEIF
	"Ends an IF, a DOSEQ, a DORND or a loop.", // END
	"IF triggers ENTER actions END: the actions run when the triggers first hold.", // ENTER
	"IF triggers LEAVE actions END: the actions run when the triggers stop holding.", // LEAVE
	"DOSEQ actions NEXT actions ... END: each section in turn.", // DOSEQ
	"DORND actions NEXT actions ... END: a section at random.", // DORND
	"Starts a DOSEQ's or a DORND's next section.", // NEXT
	"GLOOP group actions END: the actions for each of a group's members.", // GLOOP
	"PLOOP actions END: the actions for each player.", // PLOOP
	"VAR name: declares a number variable (shown on the script debug screen).", // VAR
	"CHEAT name: declares a server cheat variable.", // CHEAT
	"RUN file: compiles another script's text here (at the top level only).", // RUN
	"Negates the trigger after it.", // NOT
	"Both triggers must hold.", // AND
	"Either trigger must hold.", // OR
};
static_assert(std::size(kKeywordWords) == std::size(wac::kWacKeywords), "every WAC keyword has its words");

struct Keyword {
	const char *word, *words;
};

std::optional<Keyword> keyword_of(const std::string &word) {
	for (size_t i = 0; i < std::size(wac::kWacKeywords); ++i)
		if (strutil::iequals(word, wac::kWacKeywords[i])) return Keyword{wac::kWacKeywords[i], kKeywordWords[i]};
	return std::nullopt;
}

// A parameter's kind in words.
const char *param_words(ParamType type) {
	switch (type) {
	case ParamType::Value: return "a value";
	case ParamType::Number: return "a number";
	case ParamType::Red: return "red (0 to 255)";
	case ParamType::Green: return "green (0 to 255)";
	case ParamType::Blue: return "blue (0 to 255)";
	case ParamType::Distance: return "a distance";
	case ParamType::Heading: return "a heading";
	case ParamType::Seconds: return "seconds";
	case ParamType::Hour: return "an hour of the day";
	case ParamType::Meters: return "metres";
	case ParamType::Ssn: return "an entity, by its SSN";
	case ParamType::Group: return "a script group";
	case ParamType::Team: return "a team";
	case ParamType::Area: return "an area, by its zone id";
	case ParamType::Target: return "a target";
	case ParamType::WpList: return "a waypoint path, by its number";
	case ParamType::Text: return "a text";
	case ParamType::Filename: return "a wave file";
	case ParamType::SoundSet: return "a sound set";
	case ParamType::TextToken: return "a text key";
	case ParamType::Face: return "a facial expression";
	case ParamType::Fx: return "a particle effect";
	case ParamType::Ammo: return "an ammo";
	case ParamType::Anim: return "an animation";
	case ParamType::Cheat: return "a cheat";
	case ParamType::IfName: return "an IF's name";
	case ParamType::Variable: return "a variable";
	case ParamType::Null: break;
	}
	return "nothing";
}

// A command as the help file writes it, "SSNarea(SSN, AREA)" [orig: WacScript_DumpActionDefsToFile
// @0x4F0400], and what kind of command it is.
std::string signature(const wac::CommandDef &command) {
	std::string out = std::string(command.name) + "(";
	for (int i = 0; i < 4; ++i) {
		if (command.params[i] == ParamType::Null) continue;
		out += std::string(i ? ", " : "") + strutil::to_upper(wac::param_type_name(command.params[i]));
	}
	return out + ")";
}

std::string command_words(const wac::CommandDef &command) {
	std::string kind = wac::cmd_is_condition(command) ? "a trigger (after IF)"
	                   : wac::cmd_is_action(command)  ? "an action (after THEN)"
	                                                  : "a debug command";
	std::string out = signature(command) + ": " + kind;
	if (wac::cmd_is_replicated(command)) out += ", sent to the players' games";
	std::string params;
	for (int i = 0; i < 4; ++i)
		if (command.params[i] != ParamType::Null) params += std::string(params.empty() ? "" : "; ") + param_words(command.params[i]);
	if (!params.empty()) out += ". It takes " + params;
	return out + ".";
}

// The seven default script groups [orig: Server_BuildEntitySlotLists @0x4F97A0; runtime/world
// EntityRegistry::script_groups].
struct GroupRow {
	const char *name, *words;
};
constexpr GroupRow kGroups[] = {
	{"emptygroup", "no one"},
	{"humans", "the soldiers people play"},
	{"blueplayers", "the players of team 1"},
	{"redplayers", "the players of team 2"},
	{"ai", "the soldiers no one plays"},
	{"blueai", "the soldiers no one plays, of team 1"},
	{"redai", "the soldiers no one plays, of team 2"},
};

// --- the mission of the script's name --------------------------------------------------------------

// The open mission whose name the script's is (its stem, as the game finds the script: <stem>.wac).
const MissionDocument *mission_open(const SessionView &view, const TextDocument &script) {
	const std::string stem = mission::mission_base_name(basename_of(script.path()));
	for (const auto &open : view.documents.open) {
		const auto *mission = open ? dynamic_cast<const MissionDocument *>(records_of(*open)) : nullptr;
		if (mission && strutil::iequals(mission::mission_base_name(basename_of(mission->path())), stem)) return mission;
	}
	return nullptr;
}

std::string mission_scope_of(const TextDocument &script) {
	return strutil::to_upper(mission::mission_base_name(basename_of(script.path()))) + ".BMS";
}

// An entity in words, the display names' (documents/mission_labels.h, S15 Names): the open mission's
// with the graph's names ("Ranger #12 (Sgt. Miller)"), else the graph's symbol and its item's name.
std::string entity_words(const SessionView &view, const TextDocument &script, int64_t ssn) {
	const AssetGraph *graph = view.findings.graph.get();
	if (const MissionDocument *mission = mission_open(view, script)) {
		std::optional<GraphNameSource> names;
		if (graph) names.emplace(*graph);
		return mission_ssn_display(*mission, ssn, names ? &*names : nullptr).text;
	}
	if (ssn == mission::kPlayerSsn) return "The player"; // the player's SSN, which names no record
	const GraphSymbol *symbol = graph ? graph->resolve_symbol(ReferenceKind::MissionEntity, std::to_string(ssn), mission_scope_of(script)) : nullptr;
	if (!symbol) return "No entity has SSN " + std::to_string(ssn);
	const GraphSymbol *item = symbol->value.empty() ? nullptr : graph->resolve_symbol(ReferenceKind::Item, symbol->value);
	return (item && !item->record.empty() ? item->record : "SSN") + " #" + std::to_string(ssn);
}

std::string zone_words(const SessionView &view, const TextDocument &script, int64_t id) {
	if (const MissionDocument *mission = mission_open(view, script)) return mission_zone_display(*mission, id).text;
	const AssetGraph *graph = view.findings.graph.get();
	const GraphSymbol *symbol = graph ? graph->resolve_symbol(ReferenceKind::MissionZone, std::to_string(id), mission_scope_of(script)) : nullptr;
	return symbol ? "Zone " + std::to_string(id) : "No area has zone " + std::to_string(id);
}

// --- where a word stands --------------------------------------------------------------------------

// What a place on a line is in: the command whose parameter it is (and which), or a statement where
// conditions go (after IF, AND, OR, NOT, ELSEIF) or actions do.
struct Context {
	const wac::CommandDef *command = nullptr;
	int param = -1;
	ParamType type = ParamType::Null;
	bool conditions = false;
	bool comment = false;
	bool declaration = false; // after VAR, CHEAT: a new name
	bool run = false;         // after RUN: a file
};

Context context_at(std::string_view line, size_t at) {
	Context out;
	bool comment = false;
	const std::vector<Token> tokens = tokens_of(line, at, comment);
	out.comment = comment;
	std::string keyword;
	for (const Token &token : tokens) {
		if (token.to >= at && token.from < at) break; // the word at the place itself
		if (const std::optional<Keyword> kw = keyword_of(token.text)) {
			out.command = nullptr;
			keyword = kw->word;
			out.declaration = keyword == "VAR" || keyword == "CHEAT";
			out.run = keyword == "RUN";
			continue;
		}
		out.declaration = out.run = false;
		if (const wac::CommandDef *command = wac::wac_find_command(token.text)) {
			out.command = command;
			out.param = 0;
			continue;
		}
		if (out.command) ++out.param;
		else if (keyword == "GLOOP") keyword.clear(); // its group came
	}
	if (out.command) {
		// The parameter its count reaches, the Null slots passed over.
		int seen = 0;
		for (int i = 0; i < 4; ++i) {
			if (out.command->params[i] == ParamType::Null) continue;
			if (seen++ == out.param) {
				out.type = out.command->params[i];
				return out;
			}
		}
		out.command = nullptr; // its parameters all came: a statement again
	}
	if (keyword == "GLOOP") out.type = ParamType::Group;
	out.conditions = keyword == "IF" || keyword == "AND" || keyword == "OR" || keyword == "NOT" || keyword == "ELSEIF";
	return out;
}

// Whether a text edge's span holds the character at a place (line and column from 1).
bool covers(const GraphEdge &edge, size_t line, size_t column) {
	return edge.span.line == line && edge.span.column <= column && column < edge.span.column + edge.span.length;
}

// The word at a place (column from 1): its start, its length and its text; false where the place is in
// no word.
bool word_at(std::string_view line, size_t column, Token &out, bool touching_end) {
	const size_t at = column - 1;
	if (at > line.size()) return false;
	size_t from = at, to = at;
	while (from > 0 && !ends_token(line[from - 1])) --from;
	while (to < line.size() && !ends_token(line[to])) ++to;
	if (from == to && !touching_end) return false;
	out.from = from;
	out.to = to;
	out.text = std::string(line.substr(from, to - from));
	return true;
}

// --- the names a parameter takes -----------------------------------------------------------------

void add(ScriptCompletions &out, std::string label, std::string insert, const char *kind, std::string detail) {
	if (out.items.size() >= kMostCompletions) return;
	out.items.push_back({std::move(label), std::move(insert), kind, std::move(detail)});
}

// Whether a candidate goes in for what is typed: its insert starting with it (without case), or, for
// a name shown beside a number, its words holding it.
bool wanted(const std::string &typed, const std::string &insert, const std::string &words = std::string()) {
	return typed.empty() || strutil::starts_with_icase(insert, typed) || (!words.empty() && contains_nocase(words, typed));
}

// The open mission's entities by SSN with their titles, kept while the document (its identity, load
// and revision) and the graph's names (their generation) stand: completion asks on every keystroke, and
// titling every entity of a large mission is not a keystroke's work (S15).
const std::vector<std::pair<int64_t, std::string>> &entity_titles(const SessionView &view, const MissionDocument &mission) {
	struct Titles {
		bool held = false, has_graph = false;
		uint64_t identity = 0, load = 0, revision = 0, generation = 0;
		std::vector<std::pair<int64_t, std::string>> titles;
	};
	static Titles cache;
	const AssetGraph *graph = view.findings.graph.get();
	const uint64_t generation = graph ? graph->generation() : 0;
	if (cache.held && cache.identity == mission.identity() && cache.load == mission.load_generation() &&
	    cache.revision == mission.revision() && cache.has_graph == (graph != nullptr) && cache.generation == generation)
		return cache.titles;
	cache = Titles();
	cache.held = true;
	cache.identity = mission.identity();
	cache.load = mission.load_generation();
	cache.revision = mission.revision();
	cache.has_graph = graph != nullptr;
	cache.generation = generation;
	std::optional<GraphNameSource> names;
	if (graph) names.emplace(*graph);
	for (const auto &row : mission.rows())
		if (row && is_entity_kind(row->kind)) {
			const int64_t ssn = static_cast<const EntityRow &>(*row).native.id;
			if (mission.entity_holder(ssn) == row->id)
				cache.titles.emplace_back(ssn, mission_entity_title(mission, *row, names ? &*names : nullptr));
		}
	return cache.titles;
}

void entities(const SessionView &view, const TextDocument &script, const std::string &prefix, const std::string &typed,
              ScriptCompletions &out) {
	std::vector<std::pair<int64_t, std::string>> found;
	if (const MissionDocument *mission = mission_open(view, script)) {
		// The player first: the game resolves SSN 10000 itself (S15).
		found.emplace_back(mission::kPlayerSsn, "The player");
		const auto &titles = entity_titles(view, *mission);
		found.insert(found.end(), titles.begin(), titles.end());
	} else if (const AssetGraph *graph = view.findings.graph.get()) {
		const std::string scope = mission_scope_of(script);
		for (const GraphSymbol *symbol : graph->symbols_of_kind(ReferenceKind::MissionEntity)) {
			if (symbol->inert || !strutil::iequals(symbol->scope, scope)) continue;
			const std::optional<int> ssn = strutil::parse_int(symbol->name);
			if (ssn) found.emplace_back(*ssn, entity_words(view, script, *ssn));
		}
	}
	// Those whose SSN starts with what is typed first, then those whose words hold it.
	for (int pass = 0; pass < 2; ++pass)
		for (const auto &[ssn, words] : found) {
			const std::string insert = prefix + std::to_string(ssn);
			const bool by_number = typed.empty() || strutil::starts_with_icase(insert, typed);
			if (pass == 0 ? by_number : !by_number && wanted(typed, insert, words))
				add(out, std::to_string(ssn) + "  " + words, insert, "entity", words);
		}
}

void areas(const SessionView &view, const TextDocument &script, const std::string &typed, ScriptCompletions &out) {
	if (const MissionDocument *mission = mission_open(view, script)) {
		for (const Node *row : mission->rows_of(MissionKind::Area)) {
			const int64_t id = static_cast<const AreaRow &>(*row).native.id;
			const std::string words = mission_zone_display(*mission, id).text, insert = std::to_string(id);
			if (wanted(typed, insert, words)) add(out, insert + "  " + words, insert, "area", words);
		}
		return;
	}
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return;
	const std::string scope = mission_scope_of(script);
	for (const GraphSymbol *symbol : graph->symbols_of_kind(ReferenceKind::MissionZone))
		if (!symbol->inert && strutil::iequals(symbol->scope, scope) && wanted(typed, symbol->name))
			add(out, symbol->name + "  Zone " + symbol->name, symbol->name, "area", "Zone " + symbol->name);
}

void paths(const SessionView &view, const TextDocument &script, const std::string &typed, ScriptCompletions &out) {
	const MissionDocument *mission = mission_open(view, script);
	if (!mission) return;
	for (const Node *row : mission->rows_of(MissionKind::WaypointPath)) {
		const MissionPath &path = static_cast<const PathRow &>(*row).native;
		if (path.number <= 0 || path.number >= mission::kFirstPathCommand || path.record.waypoint_numbers.empty()) continue;
		const std::string insert = std::to_string(path.number);
		const std::string words = mission_path_title(path);
		if (wanted(typed, insert)) add(out, insert + "  " + words, insert, "path", words);
	}
}

void symbols(const SessionView &view, ReferenceKind kind, const char *what, const std::string &prefix, const std::string &typed,
             const std::vector<std::string> &scopes, ScriptCompletions &out) {
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return;
	// A name once (a set, not a list: a project's tables hold thousands of keys), and no more walking
	// once the list holds the most it shows (S15: completion asks on every keystroke).
	std::unordered_set<std::string> seen;
	for (const GraphSymbol *symbol : graph->symbols_of_kind(kind)) {
		if (out.items.size() >= kMostCompletions) break;
		if (symbol->inert) continue;
		if (!scopes.empty() && std::none_of(scopes.begin(), scopes.end(), [&](const std::string &scope) {
			    return strutil::starts_with_icase(symbol->scope, scope);
		    }))
			continue;
		const std::string &name = symbol->display.empty() ? symbol->name : symbol->display;
		const std::string insert = prefix + name;
		if (!wanted(typed, insert) || !seen.insert(strutil::to_upper(name)).second) continue;
		add(out, insert, insert, what, std::string(what) + " defined in " + symbol->file + (symbol->scope.empty() ? "" : " (" + symbol->scope + ")"));
	}
}

// What a parameter of `type` takes, as typed so far: a prefix form (wac::kWacOperandPrefixes: SSN_, FX_,
// TT_, AMMO_) keeps its prefix.
void names_for(const SessionView &view, const TextDocument &script, ParamType type, const std::string &typed,
               ScriptCompletions &out) {
	const std::string stem = strutil::to_upper(mission::mission_base_name(basename_of(script.path())));
	const auto prefixed = [&](const char *prefix) { return strutil::starts_with_icase(typed, prefix); };
	// The mission's table is its own <stem>.bin, else medmssn.bin, never both [orig:
	// TextResource_LoadMissionTextBin @0x51ed90], then gametext.bin: the keys of the one it reads.
	const mission::Sidecar &table = *mission::sidecar_for_role("text");
	const std::string own = strutil::to_upper(mission::sidecar_name(stem, table));
	const bool own_table = view.project.scan && view.project.scan->find(own) != nullptr;
	const std::vector<std::string> text_scopes = {(own_table ? own : strutil::to_upper(table.fallback)) + "/", strutil::to_upper(hud::kGameTextTable) + "/"};
	const char *const ssn = wac::wac_operand_prefix(ParamType::Ssn), *const fx = wac::wac_operand_prefix(ParamType::Fx);
	const char *const tt = wac::wac_operand_prefix(ParamType::TextToken), *const ammo = wac::wac_operand_prefix(ParamType::Ammo);
	if (prefixed(ssn)) return entities(view, script, ssn, typed, out);
	if (prefixed(fx)) return symbols(view, ReferenceKind::Particle, "effect", fx, typed, {}, out);
	if (prefixed(tt)) return symbols(view, ReferenceKind::TextId, "text key", tt, typed, text_scopes, out);
	if (prefixed(ammo)) return symbols(view, ReferenceKind::Ammo, "ammo", ammo, typed, {}, out);
	switch (type) {
	case ParamType::Ssn: return entities(view, script, "", typed, out);
	case ParamType::Area: return areas(view, script, typed, out);
	case ParamType::WpList: return paths(view, script, typed, out);
	case ParamType::Group:
		for (const GroupRow &group : kGroups)
			if (wanted(typed, group.name)) add(out, std::string(group.name) + "  " + group.words, group.name, "group", group.words);
		return;
	case ParamType::Fx: return symbols(view, ReferenceKind::Particle, "effect", "", typed, {}, out);
	case ParamType::Ammo: return symbols(view, ReferenceKind::Ammo, "ammo", "", typed, {}, out);
	case ParamType::TextToken: return symbols(view, ReferenceKind::TextId, "text key", "", typed, text_scopes, out);
	default: break;
	}
}

// The commands and keywords where a statement goes: the triggers first after IF, AND, OR, NOT and
// ELSEIF, the actions first elsewhere.
void statements(bool conditions, const std::string &typed, ScriptCompletions &out) {
	for (size_t i = 0; i < std::size(wac::kWacKeywords); ++i)
		if (wanted(typed, wac::kWacKeywords[i]))
			add(out, wac::kWacKeywords[i], wac::kWacKeywords[i], "keyword", kKeywordWords[i]);
	for (int pass = 0; pass < 2; ++pass)
		for (int i = 0; i < wac::wac_command_count(); ++i) {
			const wac::CommandDef &command = wac::wac_commands()[i];
			if (wac::cmd_is_condition(command) != (pass == 0 ? conditions : !conditions)) continue;
			if (wanted(typed, command.name)) add(out, signature(command), command.name, "command", command_words(command));
		}
}

std::string expected_words(const Context &context) {
	if (context.command)
		return std::string(context.command->name) + "'s parameter " + std::to_string(context.param + 1) + ": " +
		       param_words(context.type);
	if (context.type == ParamType::Group) return "GLOOP's group";
	if (context.declaration) return "a new name";
	if (context.run) return "a script file";
	return context.conditions ? "a trigger (a command after IF, AND, OR or NOT) or a keyword"
	                          : "an action (a command after THEN) or a keyword";
}

} // namespace

ScriptCompletions script_completions(const SessionView &view, const TextDocument &script, size_t line, size_t column,
                                     const std::string *held) {
	ScriptCompletions out;
	const std::string_view text = held ? std::string_view(*held) : script.line(line);
	Token word;
	if (!word_at(text, column, word, true)) return out;
	// What is typed: the word's part before the place.
	out.column = word.from + 1;
	out.typed = std::string(text.substr(word.from, std::min(column - 1, word.to) - word.from));
	const Context context = context_at(text, word.from + 1);
	if (context.comment) return out;
	out.expected = expected_words(context);
	if (context.declaration || context.run) return out;
	if (context.type != ParamType::Null) names_for(view, script, context.type, out.typed, out);
	else if (!context.command) {
		// A prefix form wherever it is typed; else the statements.
		names_for(view, script, ParamType::Null, out.typed, out);
		if (out.items.empty()) statements(context.conditions, out.typed, out);
	}
	return out;
}

bool script_hover(const SessionView &view, const TextDocument &script, size_t line, size_t column, ScriptHover &out) {
	const std::string_view text = script.line(line);
	Token word;
	if (!word_at(text, column, word, false)) return false;
	bool comment = false;
	tokens_of(text, word.from, comment);
	if (comment) return false;
	out.column = word.from + 1;
	out.length = word.to - word.from;
	out.word = word.text;
	if (const wac::CommandDef *command = wac::wac_find_command(word.text)) {
		out.text = command_words(*command);
		return true;
	}
	if (const std::optional<Keyword> keyword = keyword_of(word.text)) {
		out.text = keyword->words;
		return true;
	}
	const Context context = context_at(text, word.from + 1);
	const std::string up = strutil::to_upper(word.text);
	const auto number = [&](size_t skip) { return strutil::parse_int(word.text.substr(skip)); };
	const std::string_view ssn_prefix = wac::wac_operand_prefix(ParamType::Ssn);
	const bool ssn_form = strutil::starts_with_icase(up, ssn_prefix);
	if (ssn_form || (context.type == ParamType::Ssn && number(0))) {
		const std::optional<int> ssn = number(ssn_form ? ssn_prefix.size() : 0);
		if (!ssn) return false;
		const std::string words = entity_words(view, script, *ssn);
		out.text = (words.rfind("No ", 0) == 0 ? words : "An entity: " + words) + ".";
		return true;
	}
	if (context.type == ParamType::Area && number(0)) {
		const std::string words = zone_words(view, script, *number(0));
		out.text = (words.rfind("No ", 0) == 0 ? words : "An area: " + words) + ".";
		return true;
	}
	if (context.type == ParamType::WpList && number(0)) {
		out.text = "Waypoint path " + word.text + ".";
		return true;
	}
	for (const GroupRow &group : kGroups)
		if (strutil::iequals(word.text, group.name) || strutil::iequals(up, wac::wac_operand_prefix(ParamType::Group) + strutil::to_upper(group.name))) {
			out.text = std::string("Script group ") + group.name + ": " + group.words + ".";
			return true;
		}
	if ((up[0] == 'V' || up[0] == 'G' || up[0] == 'M') && up.size() > 1 && std::isdigit(static_cast<unsigned char>(up[1]))) {
		out.text = up[0] == 'V' ? "Mission variable " + up.substr(1) + "." : up[0] == 'G' ? "Global variable " + up.substr(1) + "."
		                                                                                    : "Music variable " + up.substr(1) + ".";
		return true;
	}
	// A name the graph knows at this place (a text key, an effect, an ammo, a RUN's file, a wave).
	if (const AssetGraph *graph = view.findings.graph.get())
		for (const GraphEdge *edge : graph->references_of(script.path())) {
			if (!covers(*edge, line, column)) continue;
			std::string file;
			const ReferenceStatus status = graph->resolve(*edge, &file);
			const std::string kind = reference_row(edge->kind).label;
			out.text = kind + " '" + edge->value + "': " +
			           (status == ReferenceStatus::Present ? "defined in " + file + "." : std::string("the project has none."));
			return true;
		}
	if (context.command) {
		out.text = std::string(context.command->name) + "'s parameter " + std::to_string(context.param + 1) + ": " +
		           param_words(context.type) + ".";
		return true;
	}
	return false;
}

bool script_definition(const SessionView &view, const TextDocument &script, size_t line, size_t column, EditorRequest &out) {
	const std::string_view text = script.line(line);
	Token word;
	if (!word_at(text, column, word, false)) return false;
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return false;
	// A name the graph knows at this place: the file or the record it reaches.
	for (const GraphEdge *edge : graph->references_of(script.path())) {
		if (!covers(*edge, line, column)) continue;
		if (const GraphSymbol *symbol = graph->symbol_reached(*edge)) {
			out = request::open_document(symbol->file, symbol->locator, symbol->field);
			return true;
		}
		std::string file;
		if (graph->resolve(*edge, &file) == ReferenceStatus::Present && !file.empty()) {
			out = request::open_document(file);
			return true;
		}
		return false;
	}
	// An entity or an area of the mission by the number a parameter takes.
	const Context context = context_at(text, word.from + 1);
	const std::string up = strutil::to_upper(word.text);
	ReferenceKind kind = ReferenceKind::None;
	std::string name = word.text;
	const std::string_view ssn_prefix = wac::wac_operand_prefix(ParamType::Ssn);
	if (strutil::starts_with_icase(up, ssn_prefix)) {
		kind = ReferenceKind::MissionEntity;
		name = word.text.substr(ssn_prefix.size());
	} else if (context.type == ParamType::Ssn) {
		kind = ReferenceKind::MissionEntity;
	} else if (context.type == ParamType::Area) {
		kind = ReferenceKind::MissionZone;
	}
	if (kind == ReferenceKind::None || !strutil::parse_int(name)) return false;
	const GraphSymbol *symbol = graph->resolve_symbol(kind, std::to_string(*strutil::parse_int(name)), mission_scope_of(script));
	if (!symbol) return false;
	out = request::open_document(symbol->file, symbol->locator, symbol->field);
	return true;
}

MissionScript mission_script(const SessionView &view, const std::string &mission_path) {
	MissionScript out;
	const mission::Sidecar *script = mission::sidecar_for_role("script");
	if (!script) return out;
	out.name = mission::sidecar_name(basename_of(mission_path), *script);
	const std::string folder = mission_path.substr(0, mission_path.find_last_of('/') + 1);
	out.create_at = folder + out.name;
	// The project's file of the name, as the game's lookup finds it (its logical name, any case).
	if (view.project.scan)
		if (const AssetEntry *entry = view.project.scan->find(out.name)) out.path = entry->relative_path;
	return out;
}

} // namespace opennova::editor
