// The editor's rule over the ConfigFile text reader's data-strings pool (documents/config_overrun.h; the pool
// itself, formats/configfile DataStringsPool, is tests/configfile/data_strings_pool_test's). The reader sizes its
// pool of text values by their bytes (each its length and one), FastMem_Alloc rounds that up to 64, and the parse
// clears it one byte per value [orig: ConfigFile_ParseText @ 0x7609e8]: a file of more values than the rounded
// pool writes past it into the game's heap. Covered: the rule's finding on a charattr.def (its rows' document, S23 B:
// over the file a build packs, the file as it sits while nothing is changed, else the text its save writes) and a
// credits file, its place and words, a save's text under the line said (the editor's form leaves out the keys at 0
// and the lines the game reads nothing of), none for a CBIN credits file nor a kind of another reader; the blanks of
// both kinds under the line and a credits blank over it refused; and through a session, the row over a closed
// charattr.def, the build it refuses (the game's own failure, cited), and the file saved, after which the project
// builds. The rule's guard against a finding on what JO:CA ships (every file its game reads through the text
// reader: charattr.def, and the credits its menus name, CBIN files): a charattr.def in JO:CA's shape at its
// counts, none past the pool and no finding; each construct of it alone at the line, no finding, and one value
// more a warning; a CBIN credits file, no pool.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <editor/documents/charattr_document.h>
#include <editor/documents/config_overrun.h>
#include <editor/documents/document_types.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/text_document.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_run.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/cbin/binary_config.h>
#include <formats/charattr/charattr.h>
#include <formats/configfile/config_file.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/charattr_shape.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

configfile::DataStringsPool pool_of(const std::string &text) {
	return configfile::data_strings_pool(reinterpret_cast<const uint8_t *>(text.data()), text.size());
}

configfile::DataStringsPool pool_of(const std::vector<uint8_t> &bytes) {
	return configfile::data_strings_pool(bytes.data(), bytes.size());
}

bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

std::string fixture_text(const char *name) {
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/configfile/" + name;
	std::vector<uint8_t> bytes;
	if (!test_io::read_file(path, bytes)) std::fprintf(stderr, "cannot read %s\n", path.c_str());
	return std::string(bytes.begin(), bytes.end());
}

std::unique_ptr<DocumentBase> loaded(const std::string &text, const char *name, AssetKind kind) {
	std::unique_ptr<DocumentBase> document = document_type_for(kind)->make();
	Diagnostic error;
	if (!document->load_bytes(std::vector<uint8_t>(text.begin(), text.end()), name, kind, "jo", error)) {
		std::fprintf(stderr, "%s: %s\n", name, error.message.c_str());
		return nullptr;
	}
	return document;
}

charattr::Table table_of(const std::string &text) {
	charattr::Table table;
	charattr::read_table(reinterpret_cast<const uint8_t *>(text.data()), text.size(), table);
	return table;
}

// A charattr.def of `classes` classes in the form the base game's earlier files took: each class the eleven
// number keys (SCOPE_MUTE at 0.0 and RUN_MODIFIER at 0 where `zeros`, which the loader reads the same missing,
// else a value away from 0), then its ATTRIBUTES word where `words` names one ("" for none). CR LF line ends.
std::string charattr_text(const std::vector<std::string> &words, bool zeros) {
	std::string text = "; charattr.def in the base game's earlier form\r\n";
	for (size_t i = 0; i < words.size(); ++i) {
		text += "[CHARACTER" + std::to_string(i + 1) + "]\r\n";
		text += "STEALTH\t\t= 25\r\nHPBONUS\t\t= 2\r\nRECOIL_MUTE\t= 0.75\r\nXHAIR_MUTE\t= 2.5\r\nXHAIRDX_MUTE\t= 2.5\r\n";
		text += zeros ? "SCOPE_MUTE\t= 0.0\r\n" : "SCOPE_MUTE\t= 0.25\r\n";
		text += "RELOAD_MUTE\t= 0.5\r\nJUNGLE_CAMMO\t= 5305\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5305\r\n";
		text += zeros ? "RUN_MODIFIER\t= 0\r\n" : "RUN_MODIFIER\t= 1\r\n";
		if (!words[i].empty()) text += "ATTRIBUTES\t= " + words[i] + "\r\n";
		text += "\r\n";
	}
	return text;
}

// The first offset of `part` in `text` at or after `from`.
size_t at(const std::string &text, const std::string &part, size_t from = 0) { return text.find(part, from); }

// The one document.config_overrun of `findings` at `severity`: an error past the block's 36-byte slack (it gates),
// a warning within it (configfile::fastmem_overrun_reaches_header).
const Diagnostic *overrun_of(const std::vector<Diagnostic> &findings, DiagnosticSeverity severity) {
	if (findings.size() != 1 || findings[0].code() != "document.config_overrun" || findings[0].severity != severity)
		return nullptr;
	return &findings[0];
}

// A charattr document's pool findings (its validator's document.config_overrun) and the text its save writes.
std::vector<Diagnostic> charattr_overrun(const DocumentBase &document) {
	std::vector<Diagnostic> out;
	for (Diagnostic &d : validate_charattr_file(document))
		if (d.code() == "document.config_overrun") out.push_back(std::move(d));
	return out;
}

std::string saved_text(const DocumentBase &document) { return document.serialize().text; }

size_t line_at(const std::string &text, size_t offset) {
	return 1 + size_t(std::count(text.begin(), text.begin() + std::ptrdiff_t(offset), '\n'));
}

int test_rule_charattr() {
	const std::string past = fixture_text("pool_past_line.def");
	std::unique_ptr<DocumentBase> document = loaded(past, "charattr.def", AssetKind::CharAttrDefs);
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	TEST_EXPECT(config_overrun_findings(*document).empty()); // a record document's is its validator's
	const std::vector<Diagnostic> findings = charattr_overrun(*document);
	const Diagnostic *d = overrun_of(findings, DiagnosticSeverity::Warning); // 1 byte past: within the slack
	TEST_EXPECT(d != nullptr);
	if (!d) return 1;
	// Over the file as it sits, which a build packs while nothing is changed: at value 65, the first past the 64-byte
	// pool, CHARACTER6's ATTRIBUTES word, the file's last value.
	TEST_EXPECT(d->line == line_at(past, past.rfind("KnifeBonus")));
	TEST_EXPECT(contains(d->message, "65 values") && contains(d->message, "60 bytes") &&
	            contains(d->message, "64 bytes") && contains(d->message, "1 byte of zeros") &&
	            contains(d->message, "ConfigFile_ParseText @ 0x7609e8") && contains(d->message, "Value 65") &&
	            d->planned.empty());
	// A save writes the editor's form: CHARACTER5's RUN_MODIFIER at 0, which the loader reads the same missing, left
	// out, so 64 values, at the line; the finding says so.
	const std::string saved = saved_text(*document);
	TEST_EXPECT(saved != past && !contains(saved, "RUN_MODIFIER = 0") && pool_of(saved).values == 64 &&
	            charattr::same_rows(table_of(past), table_of(saved)));
	TEST_EXPECT(contains(d->message, "Saving it writes the table in the editor's form") &&
	            contains(d->message, "holds 64 values: under the line"));
	// An edit: the build packs what a save writes (a build saves first), under the line; undone, the file as it sits.
	const auto &rows = document->as_records()->rows();
	TEST_EXPECT(!rows.empty());
	if (rows.empty()) return 1;
	const NodeAddress first{rows[0]->id, rows[0]->kind, 0};
	Diagnostic error;
	TEST_EXPECT(document->apply(editor_test::set_edit(first, "stealth", 26.0), error) && charattr_overrun(*document).empty());
	document->undo();
	TEST_EXPECT(overrun_of(charattr_overrun(*document), DiagnosticSeverity::Warning) != nullptr);
	// At the line: none.
	std::unique_ptr<DocumentBase> at_line = loaded(fixture_text("pool_at_line.def"), "charattr.def", AssetKind::CharAttrDefs);
	TEST_EXPECT(at_line && charattr_overrun(*at_line).empty());
	// The earlier base game's form: its six SCOPE_MUTE at 0.0 and six RUN_MODIFIER at 0, which a save leaves out.
	const std::string earlier =
			charattr_text({ "AutoScope", "Medic", "AutoScope", "AutoScope", "KnifeBonus", "KnifeBonus" }, true);
	std::unique_ptr<DocumentBase> old_base = loaded(earlier, "charattr.def", AssetKind::CharAttrDefs);
	const std::vector<Diagnostic> old_findings = old_base ? charattr_overrun(*old_base) : std::vector<Diagnostic>();
	// 8 bytes past, the witnessed run: a warning (the mission started, docs/mnu/menu-re.md).
	const Diagnostic *old_d = overrun_of(old_findings, DiagnosticSeverity::Warning);
	TEST_EXPECT(old_d && contains(old_d->message, "72 values") && contains(old_d->message, "8 bytes of zeros") &&
	            contains(old_d->message, "holds 60 values: under the line"));
	TEST_EXPECT(old_base && pool_of(saved_text(*old_base)).values == 60 &&
	            charattr::same_rows(table_of(earlier), table_of(saved_text(*old_base))));
	// The first try of classes 1 to 9, every value away from 0: a save writes every one, so it says nothing of one.
	const std::string first_try = charattr_text(
			{ "AutoScope", "", "", "", "Medic", "AutoScope", "SpreadBonus", "KnifeBonus", "KnifeBonus" }, false);
	std::unique_ptr<DocumentBase> tried = loaded(first_try, "charattr.def", AssetKind::CharAttrDefs);
	const std::vector<Diagnostic> tried_findings = tried ? charattr_overrun(*tried) : std::vector<Diagnostic>();
	// 41 bytes past, the witnessed crash: past the slack, an error that gates.
	const Diagnostic *tried_d = overrun_of(tried_findings, DiagnosticSeverity::Error);
	TEST_EXPECT(tried_d && contains(tried_d->message, "105 values") && contains(tried_d->message, "41 bytes of zeros") &&
	            contains(tried_d->message, "past any block the allocator gives it") && blocks_build(*tried_d) &&
	            tried_d->message.find("Saving it") == std::string::npos && tried_d->planned.empty());
	TEST_EXPECT(old_d && contains(old_d->message, "into the block's own spare bytes") && !blocks_build(*old_d));
	TEST_EXPECT(tried && pool_of(saved_text(*tried)).values == 105);
	// Lines the loader never reads are values too: a legacy block's "//KEY = n" entries (no ConfigFile comment), a
	// section of no class, a class after the first the file lacks (CHARACTER9 after a missing 7, which
	// charattr.unread_section says); a save writes none of them.
	const std::string legacy = earlier + "//STEALTH = 25\r\n//HPBONUS = 2\r\n//ATTRIBUTES = Medic\r\n[NOTES]\r\nCOUNT = 3\r\n"
	                                     "[CHARACTER9]\r\nSTEALTH = 1\r\n";
	std::unique_ptr<DocumentBase> with_legacy = loaded(legacy, "charattr.def", AssetKind::CharAttrDefs);
	const std::vector<Diagnostic> legacy_findings = with_legacy ? charattr_overrun(*with_legacy) : std::vector<Diagnostic>();
	const Diagnostic *legacy_d = overrun_of(legacy_findings, DiagnosticSeverity::Warning);
	TEST_EXPECT(legacy_d && contains(legacy_d->message, "77 values") && contains(legacy_d->message, "holds 60 values"));
	int unread = 0;
	if (with_legacy)
		for (const Diagnostic &f : validate_charattr_file(*with_legacy))
			if (f.code() == "charattr.unread_section" &&
			    (f.line == line_at(legacy, at(legacy, "[NOTES]")) || f.line == line_at(legacy, at(legacy, "[CHARACTER9]"))))
				++unread;
	TEST_EXPECT(unread == 2);
	TEST_EXPECT(with_legacy && !contains(saved_text(*with_legacy), "//") && !contains(saved_text(*with_legacy), "CHARACTER9"));
	std::printf("charattr: the row at value 65 over the file as it sits, a save's text under the line said; an edit "
	            "packs the save's text; the examples\n");
	return 0;
}

int test_rule_other_kinds() {
	// A credits file in the text form over the line: the rule's finding, no fix (its type names no idle line).
	std::string numbers;
	for (int i = 0; i < 70; ++i) numbers += (i ? " " : "") + std::to_string(i + 1);
	const std::string credits = "[ENV]\r\nSCROLL_RATE = 1.0\r\n[TEXT]\r\nTEXT = " + numbers + "\r\n";
	std::unique_ptr<DocumentBase> text_credits = loaded(credits, "credits.kda", AssetKind::Credits);
	const std::vector<Diagnostic> credit_findings =
			text_credits ? config_overrun_findings(*text_credits) : std::vector<Diagnostic>();
	const Diagnostic *d = overrun_of(credit_findings, DiagnosticSeverity::Warning);
	TEST_EXPECT(d && contains(d->message, "credits.kda holds 71 values and no text value") &&
	            contains(d->message, "7 bytes of zeros") && d->planned.empty());
	// The same in the CBIN form: the binary reader's, whose pools are cleared at their sizes.
	cbin::BinaryConfig config;
	config.strings = { "text", "TEXT" };
	config.xor_key = 0x1234;
	cbin::BinaryConfig::Label label;
	label.name = 1;
	for (uint32_t i = 0; i < 40; ++i) {
		cbin::BinaryConfig::Entry entry;
		entry.name = 2;
		entry.values = { { i, cbin::BinaryConfig::kInteger }, { i + 1, cbin::BinaryConfig::kInteger } };
		label.entries.push_back(entry);
	}
	config.labels.push_back(label);
	std::vector<uint8_t> stored;
	std::string why;
	TEST_EXPECT(cbin::encode_binary_config(config, stored, why));
	std::unique_ptr<DocumentBase> binary_credits = document_type_for(AssetKind::Credits)->make();
	Diagnostic error;
	TEST_EXPECT(binary_credits->load_bytes(stored, "nlist.kda", AssetKind::Credits, "jo", error));
	TEST_EXPECT(text_of(*binary_credits)->encoding() != nullptr && pool_of(text_of(*binary_credits)->text()).overrun() > 0 &&
	            config_overrun_findings(*binary_credits).empty());
	// A kind of another reader: none, whatever its text.
	std::unique_ptr<DocumentBase> other = loaded(credits, "notes.txt", AssetKind::Text);
	TEST_EXPECT(other && config_overrun_findings(*other).empty());
	std::printf("credits: the text form's row, the CBIN form none; another reader's kind none\n");
	return 0;
}

int test_blanks() {
	BlankRequest charattr_request;
	charattr_request.logical_name = "charattr.def";
	charattr_request.role = "charattr_def";
	charattr_request.project_title = "Pool";
	std::vector<uint8_t> bytes;
	Diagnostic error;
	TEST_EXPECT(make_blank(charattr_request, AssetKind::CharAttrDefs, bytes, error) && pool_of(bytes).overrun() == 0);
	BlankRequest credits_request;
	credits_request.logical_name = "credits.kda";
	credits_request.project_title = "A Title Of Many Words";
	TEST_EXPECT(make_blank(credits_request, AssetKind::Credits, bytes, error) && pool_of(bytes).values == 5 &&
	            pool_of(bytes).overrun() == 0);
	// A title the reader splits into numbers past the pool (a tab is a blank to it): refused, never written.
	std::string tabs;
	for (int i = 0; i < 70; ++i) tabs += (i ? "\t" : "") + std::to_string(i);
	credits_request.project_title = tabs;
	TEST_EXPECT(!make_blank(credits_request, AssetKind::Credits, bytes, error) && bytes.empty() &&
	            error.code() == "document.config_overrun" && contains(error.message, "credits.kda is not made") &&
	            contains(error.message, "0x7609e8"));
	std::printf("blanks: charattr.def and credits under the line; a credits blank over it refused\n");
	return 0;
}

int test_session() {
	test_temp::TempDir dir("opennova_editor_config_overrun");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Pool"));
	editor_test::create_missing_files(session);
	const AssetEntry *entry = session.view().project.scan->find("charattr.def");
	const std::string path = entry ? entry->relative_path : std::string("charattr.def");
	const auto write = [&](const std::string &text) {
		return test_io::write_file(session.view().project.root + "/" + path, std::vector<uint8_t>(text.begin(), text.end()));
	};
	// The witnessed crash: nine classes, 105 values, 41 bytes past the pool and its block's 36-byte slack.
	const std::string crash = charattr_text(
			{ "AutoScope", "", "", "", "Medic", "AutoScope", "SpreadBonus", "KnifeBonus", "KnifeBonus" }, false);
	TEST_EXPECT(write(crash));
	editor_test::handle_to_end(session, request::rescan());
	const auto finding = [&]() -> const Diagnostic * {
		for (const Diagnostic &d : session.view().findings.diagnostics)
			if (d.code() == "document.config_overrun" && d.asset == path) return &d;
		return nullptr;
	};
	const Diagnostic *d = finding();
	TEST_EXPECT(d && session.document_for(path) == nullptr);
	if (!d) return 1;
	TEST_EXPECT(d->severity == DiagnosticSeverity::Error && blocks_build(*d) && blocker_is_the_games(*d) &&
	            contains(blocker_reason(*d), "0x7609e8"));
	// The build is refused for it, the game's heap corrupt from that load on: the row is its refusal, named.
	const SessionView &v = session.view();
	const auto refused_for_it = [&]() {
		if (!v.activity.has_build || v.activity.last_build->ok || !v.activity.last_build->refused) return false;
		bool row = false, named = false;
		for (const Diagnostic &b : v.activity.last_build->diagnostics) {
			if (b.code() == "document.config_overrun" && b.asset == path && blocks_build(b)) row = true;
			if (b.code() == "build.blocked" && contains(b.message, "1 problem stops it: charattr.def holds 105 values"))
				named = true;
		}
		return row && named;
	};
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(refused_for_it());
	TEST_EXPECT(contains(v.activity.status, "Build refused: charattr.def holds 105 values"));
	d = finding();
	TEST_EXPECT(d != nullptr && fixes_for(*d, session.view()).empty());
	// One byte past the pool, within the slack: a warning, and the project builds.
	const std::string past = fixture_text("pool_past_line.def");
	TEST_EXPECT(write(past));
	editor_test::handle_to_end(session, request::rescan());
	d = finding();
	TEST_EXPECT(d && d->severity == DiagnosticSeverity::Warning && !blocks_build(*d));
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(v.activity.has_build && v.activity.last_build->ok && !v.activity.last_build->refused);
	// Opened and saved, the file holds the editor's form, under the line: no row.
	editor_test::handle_to_end(session, request::open_document(path));
	editor_test::handle_to_end(session, request::save(path));
	TEST_EXPECT(session.outcome().done() && !finding());
	// The crash written again outside the editor and reloaded: the row and the refusal again.
	editor_test::handle_to_end(session, request::close_document(path));
	TEST_EXPECT(write(crash));
	editor_test::handle_to_end(session, request::rescan());
	TEST_EXPECT(finding() != nullptr);
	editor_test::handle_to_end(session, request::build());
	TEST_EXPECT(refused_for_it());
	std::printf("session: 41 bytes past refuse the build, 1 byte past builds (a warning); saved under the line; the "
	            "crash written again refuses it again\n");
	return 0;
}

// The files JO:CA's game reads through the text reader: charattr.def [orig: CharAttr_LoadFromDef @ 0x412177] and
// each DATASOURCE a menu names [orig: CMarqueeWnd_LoadCreditsFromIni @ 0x65c5fe -> ConfigFile_LoadGlobal], its
// nlist.kda (base and jox01), a CBIN file; the reader's other entry, IniFile_LoadGlobal @ 0x760b00, has no caller.
// A charattr.def in JO:CA's shape (editor_test::jo_charattr_text) holds JO:CA's counts, 278 values over 288 bytes
// of text values, a 320-byte pool (the shipped file's, base and jox01 alike, read by the reader when the suite
// still held it): none past the pool, nor past it in the text a save writes, and no finding. A credits file in the
// CBIN form, laid out as nlist.kda is, goes to the binary reader: no text pool, no value cleared.
static int test_shipped_shapes() {
	const std::string jo = editor_test::jo_charattr_text();
	const configfile::DataStringsPool pool = pool_of(jo);
	std::printf("  JO:CA's charattr.def shape: %u values, %u bytes of text values, a %u-byte pool\n", pool.values,
	            pool.string_bytes, pool.pool_bytes);
	TEST_EXPECT(!pool.binary && pool.values == 278 && pool.string_bytes == 288 && pool.pool_bytes == 320 &&
	            pool.overrun() == 0);
	std::unique_ptr<DocumentBase> document = loaded(jo, "charattr.def", AssetKind::CharAttrDefs);
	TEST_EXPECT(document && charattr_overrun(*document).empty() && pool_of(saved_text(*document)).overrun() == 0);
	cbin::BinaryConfig config;
	config.strings = { "env", "text", "scroll_rate", "~JC", "Name_One", "Gunpl22b", "<CR>" };
	config.xor_key = 0x600DF00Du;
	cbin::BinaryConfig::Label env;
	env.name = 1;
	env.entries.push_back({ 3, { { 14, cbin::BinaryConfig::kInteger } } });
	cbin::BinaryConfig::Label text;
	text.name = 2;
	for (int i = 0; i < 60; ++i) {
		text.entries.push_back({ 2, { { 4, cbin::BinaryConfig::kString } } });
		text.entries.push_back({ 2, { { 5, cbin::BinaryConfig::kString }, { 6, cbin::BinaryConfig::kString } } });
		text.entries.push_back({ 2, { { 7, cbin::BinaryConfig::kString } } });
	}
	config.labels = { env, text };
	std::vector<uint8_t> stored;
	std::string why;
	TEST_EXPECT(cbin::encode_binary_config(config, stored, why));
	const configfile::DataStringsPool binary = pool_of(stored);
	TEST_EXPECT(binary.binary && binary.values == 0 && binary.pool_bytes == 0 && binary.overrun() == 0);
	std::unique_ptr<DocumentBase> credits = document_type_for(AssetKind::Credits)->make();
	Diagnostic error;
	TEST_EXPECT(credits->load_bytes(stored, "nlist.kda", AssetKind::Credits, "jo", error) &&
	            config_overrun_findings(*credits).empty());
	std::printf("shipped shapes: charattr.def at JO:CA's counts and a CBIN credits file, none past a pool\n");
	return 0;
}

// Each construct of JO:CA's charattr.def alone, in a file at the line (its values the pool's bytes exactly, the
// rest whole numbers): no finding, so the rule counts none of the construct that the reader does not; and one value
// more, 1 byte past, the warning, so it counts none fewer. The constructs' counts are the reader's (a line opening
// with '[' and a label a section; an entry a line of a key, an '=' and a value, which runs to a ';' or the line's
// end; its values the words between commas and blanks, a tab a blank; a word no number a text value, its length and
// one) [orig: ConfigFile_ParseText @ 0x7608a0, ConfigFile_CountValuesAndStringLengths @ 0x7605d0].
static int test_shipped_constructs() {
	struct Construct {
		const char *what;
		const char *before; // lines before the file's first section
		const char *inside; // lines in its [CHARACTER1]
		uint32_t values, string_bytes;
	};
	const Construct constructs[] = {
		{ "a comment holding an '=' before any section", "// setchar n (n = 1-9)\r\n", "", 0, 0 },
		{ "a commented-out section's label", "", "//[CHARACTER?]\r\n", 0, 0 },
		{ "a commented-out entry", "", "//STEALTH\t\t\t= 20\t\r\n", 1, 0 },
		{ "a commented-out entry of no blank before its '='", "", "//JUNGLE_CAMMO_ALT= 5307\r\n", 1, 0 },
		{ "an entry of blanks alone", "", "//ATTRIBUTES  \t= \t\r\n", 0, 0 },
		{ "a real's trailing tab and a negative number", "", "SCOPE_MUTE  \t= 0.0\t\r\nRUN_MODIFIER\t= -1\r\n", 2, 0 },
		{ "a list whose note runs on", "", "PREF_WEAP\t\t= 1,2,3\t// scoped rifles\r\n", 6, 3 + 7 + 7 },
		{ "a note of commas, blanks and a hyphened word", "", "PREF_WEAP\t\t= 8,9\t// belt fed, CQB-2\r\n", 6,
		  3 + 5 + 4 + 6 },
		{ "two attribute words", "", "ATTRIBUTES  \t= Medic WaterGirl\r\n", 2, 6 + 10 },
		{ "the NULL attribute", "", "ATTRIBUTES\t\t= NULL\r\n", 1, 5 },
		{ "an attribute word's trailing blank", "", "ATTRIBUTES  \t= AutoScope \r\n", 1, 10 },
		{ "a second section of a label", "", "STEALTH\t\t\t= 20\r\n[CHARACTER1]\r\nSTEALTH\t\t\t= 20\r\n", 2, 0 },
	};
	for (const Construct &construct : constructs) {
		const uint32_t step = configfile::kFastMemStep;
		const uint32_t line = construct.string_bytes == 0 ? step : (construct.string_bytes + step - 1) / step * step;
		std::string text = std::string(construct.before) + "[CHARACTER1]\r\n" + construct.inside;
		int pad = 0;
		for (uint32_t left = line - construct.values; left > 0; ++pad) {
			const uint32_t n = std::min<uint32_t>(left, 10);
			text += "PAD" + std::to_string(pad) + "\t\t=";
			for (uint32_t i = 0; i < n; ++i) text += " 1";
			text += "\r\n";
			left -= n;
		}
		const configfile::DataStringsPool pool = pool_of(text);
		if (pool.values != line || pool.string_bytes != construct.string_bytes || pool.pool_bytes != line)
			std::fprintf(stderr, "%s: %u values over %u bytes, a %u-byte pool\n", construct.what, pool.values,
			             pool.string_bytes, pool.pool_bytes);
		TEST_EXPECT(pool.values == line && pool.string_bytes == construct.string_bytes && pool.pool_bytes == line);
		std::unique_ptr<DocumentBase> at_line = loaded(text, "charattr.def", AssetKind::CharAttrDefs);
		TEST_EXPECT(at_line && charattr_overrun(*at_line).empty());
		std::unique_ptr<DocumentBase> one_past = loaded(text + "PAST\t\t= 1\r\n", "charattr.def", AssetKind::CharAttrDefs);
		TEST_EXPECT(one_past && overrun_of(charattr_overrun(*one_past), DiagnosticSeverity::Warning) != nullptr);
	}
	std::printf("constructs: each of JO:CA's charattr.def's at the line, no finding; one value more, the warning\n");
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_rule_charattr();
	failures += test_rule_other_kinds();
	failures += test_blanks();
	failures += test_session();
	failures += test_shipped_shapes();
	failures += test_shipped_constructs();
	if (failures == 0) std::printf("editor_config_overrun: all passed\n");
	return failures == 0 ? 0 : 1;
}
