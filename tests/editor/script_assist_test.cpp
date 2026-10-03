// Help in a script's text (editor/session/script_assist.h, ADR 0046 S15, Events and scripts), over a
// project holding the minted synth_logic mission as missions/logic.bms and a script of its name: the
// completions where a statement goes (the keywords, the triggers first after IF and the actions after
// THEN, each its signature and a line), where a command's parameter takes an entity (the mission's, by
// SSN and in words, with the mission open and closed), an area (its zone ids), a script group (the
// seven), and the SSN_ prefix form; what a word is (a command, a keyword, an entity by its SSN, an area
// by its zone id, a variable); where it is defined (the entity's and the area's records); the mission's
// script found and one not yet made; a compiler report in plain words, a finding's message leading
// with them and its gutter mark's note its first sentence; and the `script_assist` query.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/script_type.h>
#include <editor/model/text_document.h>
#include <editor/preview/script_viewport.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/script_assist.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

bool same(const std::string &got, const std::string &want) {
	if (got != want) std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
	return got == want;
}

const ScriptCompletion *item(const ScriptCompletions &completions, const std::string &insert) {
	for (const ScriptCompletion &each : completions.items)
		if (each.insert == insert) return &each;
	return nullptr;
}

// The script, its lines numbered from 1:
//   1  ; the walk
//   2  IF ssn
//   3  IF SSNdead
//   4  THEN Gkill
//   5  IF SSNarea 7, 20
//   6  IF SSNdead SSN_
//   7  END
constexpr const char *kScript = "; the walk\r\nIF ssn\r\nIF SSNdead \r\nTHEN Gkill \r\nIF SSNarea 7, 20\r\nIF SSNdead SSN_\r\nEND\r\n";

int test_assist() {
	editor_test::TempProjectDir dir("opennova_script_assist");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Scripts"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/missions/logic.bms",
	                                     test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms")));
	TEST_EXPECT(editor_test::write_text(root + "/missions/logic.wac", kScript));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.wac"));
	const SessionView &view = session.view();
	const TextDocument *script = text_of(*session.document_base_for("missions/logic.wac"));
	TEST_EXPECT(script && script->line_count() >= 7);
	if (!script) return 1;

	// A statement after IF: the triggers whose names start with what is typed.
	ScriptCompletions completions = script_completions(view, *script, 2, 7);
	TEST_EXPECT(completions.column == 4 && same(completions.typed, "ssn") && completions.expected.find("trigger") != std::string::npos);
	const ScriptCompletion *dead = item(completions, "SSNdead");
	TEST_EXPECT(dead && dead->kind == "command" && same(dead->label, "SSNdead(SSN)") &&
	            same(dead->detail, "SSNdead(SSN): a trigger (after IF). It takes an entity, by its SSN."));
	// After IF the triggers come before the actions.
	size_t first_action = SIZE_MAX, last_trigger = 0;
	for (size_t i = 0; i < completions.items.size(); ++i) {
		if (completions.items[i].kind != "command") continue;
		if (completions.items[i].detail.find("a trigger") != std::string::npos) last_trigger = i;
		else first_action = std::min(first_action, i);
	}
	TEST_EXPECT(item(completions, "SSNtoWP") && last_trigger < first_action);
	// The line as a control holds it, a keystroke ahead of the document.
	const std::string held = "IF SSNde";
	completions = script_completions(view, *script, 2, 9, &held);
	TEST_EXPECT(same(completions.typed, "SSNde") && item(completions, "SSNdead") && !item(completions, "SSNtoWP"));
	// SSNdead's parameter: the mission's entities, by SSN and in words, the mission closed (the graph).
	completions = script_completions(view, *script, 3, 12);
	TEST_EXPECT(same(completions.expected, "SSNdead's parameter 1: an entity, by its SSN"));
	size_t entities = 0;
	for (const ScriptCompletion &each : completions.items) entities += each.kind == "entity";
	TEST_EXPECT(entities == 12); // the minted mission's entities, each SSN once
	// THEN Gkill: the script groups.
	completions = script_completions(view, *script, 4, 12);
	TEST_EXPECT(item(completions, "humans") && item(completions, "redai") && completions.items.size() == 7);
	// SSNarea's second parameter: the areas by zone id.
	completions = script_completions(view, *script, 5, 15);
	TEST_EXPECT(same(completions.expected, "SSNarea's parameter 2: an area, by its zone id") && item(completions, "20") &&
	            item(completions, "30"));
	// The SSN_ prefix form keeps its prefix.
	completions = script_completions(view, *script, 6, 16);
	TEST_EXPECT(!completions.items.empty() && completions.items[0].insert.rfind("SSN_", 0) == 0);
	// A comment completes nothing.
	TEST_EXPECT(script_completions(view, *script, 1, 6).items.empty());

	// What a word is.
	ScriptHover hover;
	TEST_EXPECT(script_hover(view, *script, 3, 5, hover) && same(hover.word, "SSNdead") && hover.column == 4 &&
	            hover.text.find("a trigger (after IF)") != std::string::npos);
	TEST_EXPECT(script_hover(view, *script, 7, 1, hover) && same(hover.text, "Ends an IF, a DOSEQ, a DORND or a loop."));
	// With the mission open: an entity's words are its document's.
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto *mission = dynamic_cast<const MissionDocument *>(session.document_for("missions/logic.bms"));
	TEST_EXPECT(mission);
	if (!mission) return 1;
	const int32_t walker = static_cast<const EntityRow &>(*mission->rows_of(MissionKind::Organic)[0]).native.id;
	completions = script_completions(view, *script, 3, 12);
	const ScriptCompletion *walking = item(completions, std::to_string(walker));
	TEST_EXPECT(walking && same(walking->detail, "Organic #" + std::to_string(walker)));
	// Typed letters find an entity by its words.
	const std::string typed = "IF SSNdead orga\r\n";
	{
		Diagnostic error;
		TextDocument &editable = *text_of(*session.document_base_for("missions/logic.wac"));
		TEST_EXPECT(editable.apply({TextDocument::replace({8, 1, 0}, typed)}, error));
	}
	completions = script_completions(view, *script, 8, 16);
	TEST_EXPECT(item(completions, std::to_string(walker)) != nullptr);
	// A typed number: the SSNs it starts first, then the entities whose words hold it.
	const std::string digit = std::to_string(walker).substr(0, 1);
	const std::string numbered = "IF SSNdead " + digit;
	completions = script_completions(view, *script, 8, numbered.size() + 1, &numbered);
	TEST_EXPECT(!completions.items.empty() && completions.items[0].insert.rfind(digit, 0) == 0);
	bool words_after = true, seen_words = false;
	for (const ScriptCompletion &each : completions.items) {
		const bool by_number = each.insert.rfind(digit, 0) == 0;
		if (!by_number) seen_words = true;
		else if (seen_words) words_after = false;
	}
	TEST_EXPECT(words_after);
	// The number written: its words, and where it is defined.
	{
		Diagnostic error;
		TextDocument &editable = *text_of(*session.document_base_for("missions/logic.wac"));
		TEST_EXPECT(editable.apply({TextDocument::replace({8, 12, 4}, std::to_string(walker))}, error));
	}
	TEST_EXPECT(script_hover(view, *script, 8, 13, hover) && same(hover.text, "An entity: Organic #" + std::to_string(walker) + "."));
	EditorRequest go;
	TEST_EXPECT(script_definition(view, *script, 8, 13, go) && go.kind == EditorRequestKind::OpenDocument &&
	            same(go.path, "missions/logic.bms") && !go.locator.empty());
	TEST_EXPECT(script_hover(view, *script, 5, 12, hover) && hover.text.rfind("An entity: ", 0) == 0); // SSNarea's first: an SSN
	TEST_EXPECT(script_hover(view, *script, 5, 15, hover) && same(hover.text, "An area: Zone 20."));
	TEST_EXPECT(!script_definition(view, *script, 1, 3, go)); // a comment names nothing

	// The mission's script: found; one of another mission not yet made goes beside it.
	MissionScript found = mission_script(view, "missions/logic.bms");
	TEST_EXPECT(same(found.name, "logic.wac") && same(found.path, "missions/logic.wac"));
	found = mission_script(view, "missions/other.bms");
	TEST_EXPECT(same(found.name, "other.wac") && found.path.empty() && same(found.create_at, "missions/other.wac"));

	// The query.
	const auto ask = [&](const std::string &args) {
		JsonValue parsed;
		std::string error;
		opennova::io::json_parse(args, parsed, error);
		JsonValue answer = session.query("script_assist", parsed, error);
		if (!error.empty()) std::printf("  script_assist refused: %s\n", error.c_str());
		return answer;
	};
	JsonValue answer = ask("{\"path\": \"missions/logic.wac\", \"op\": \"complete\", \"line\": 4, \"column\": 12}");
	TEST_EXPECT(answer.get_number("count", 0) == 7 && answer.get("items"));
	answer = ask("{\"path\": \"missions/logic.wac\", \"op\": \"hover\", \"line\": 3, \"column\": 5}");
	TEST_EXPECT(same(answer.get_string("word", ""), "SSNdead"));
	answer = ask("{\"path\": \"missions/logic.wac\", \"op\": \"definition\", \"line\": 8, \"column\": 13}");
	TEST_EXPECT(answer.get("request") && answer.get("request")->get_string("kind", "") == "open_document");
	answer = ask("{\"path\": \"missions/logic.bms\", \"op\": \"mission_script\"}");
	TEST_EXPECT(answer.get_bool("held", false) && same(answer.get_string("path", ""), "missions/logic.wac"));
	return 0;
}

// A compiler report in plain words; the finding's message leads with them, the mark's note is its
// first sentence.
int test_report_words() {
	TEST_EXPECT(same(script_report_words("Missing END"), "A block opened on this line (an IF, a DOSEQ, a DORND or a loop) has no END."));
	TEST_EXPECT(same(script_report_words("Unknown 'FOOBAR'"), "'FOOBAR' is no command, keyword, variable or name the compiler knows."));
	TEST_EXPECT(same(script_report_words("Something new"), "Something new"));
	TextDocument script(nullptr, TextLineEnds::Cr);
	Diagnostic error;
	const std::string text = "IF SSNdead 5 THEN\r\n";
	TEST_EXPECT(script.load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "missions/x.wac", AssetKind::Script, "jo", error));
	const std::vector<Diagnostic> findings = validate_script_file(script);
	TEST_EXPECT(findings.size() == 1 && findings[0].message.rfind("A block opened on this line", 0) == 0 &&
	            findings[0].message.find("The WAC compiler: \"Missing END\"") != std::string::npos);
	TEST_EXPECT(!findings.empty() && same(first_sentence(findings[0].message),
	                                      "A block opened on this line (an IF, a DOSEQ, a DORND or a loop) has no END."));
	return 0;
}

} // namespace

int main() {
	if (test_report_words() != 0) return 1;
	return test_assist();
}
