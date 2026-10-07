// The ConfigFile text reader's data-strings pool (formats/configfile DataStringsPool) and the editor's rule over it
// (documents/config_overrun.h). The reader sizes its pool of text values by their bytes (each its length and one),
// FastMem_Alloc rounds that up to 64, and the parse clears it one byte per value [orig: ConfigFile_ParseText @
// 0x7609e8]: a file of more values than the rounded pool writes past it into the game's heap. Covered: the
// allocator's rounding; the two fixtures on either side of the line (64 values and 65 over a 64-byte pool); the
// three witnessed files' numbers (the earlier base game's 72 values over 58 bytes, 8 past; the first try of
// classes 1 to 9, 105 over 60, 41 past, which crashed retail's mission start; JO:CA's 278 over 288, under); the
// reader's own count (a value past a line's first 255 bytes counted as the last one read again; a key written
// with leading spaces read from the next line of its key; a CBIN file, which the binary reader takes); the rule's
// finding on a charattr.def and a credits file, its place and words, the fix that comments out the lines the
// loader reads the same without (applied: no finding, the same classes; Undo: the finding again), none where no
// such line brings it under, none for a CBIN credits file nor a kind of another reader; the blanks of both kinds
// under the line and a credits blank over it refused; and through a session, the row over a closed charattr.def
// and its fix. The retail leg reads every file JO:CA's game reads through the text reader (charattr.def, base and
// each expansion and the extracted tree; every DATASOURCE a shipped menu names) and asserts none overruns, and
// that the editor makes no finding of JO:CA's charattr.def.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/charattr_type.h>
#include <editor/documents/config_overrun.h>
#include <editor/documents/document_types.h>
#include <editor/model/text_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/cbin/binary_config.h>
#include <formats/charattr/charattr.h>
#include <formats/configfile/config_file.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
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

int test_pool_arithmetic() {
	// FastMem_Alloc: a size under 1 is 1, rounded up to 64 [orig: FastMem_Alloc @ 0x7697c4..0x7697d7].
	TEST_EXPECT(configfile::fastmem_block_bytes(0) == 64 && configfile::fastmem_block_bytes(1) == 64 &&
	            configfile::fastmem_block_bytes(64) == 64 && configfile::fastmem_block_bytes(65) == 128 &&
	            configfile::fastmem_block_bytes(288) == 320);
	// The three witnessed files' numbers: values (the clear's length) against the text values' bytes.
	const auto overrun = [](uint32_t values, uint32_t bytes) {
		configfile::DataStringsPool pool;
		pool.values = values;
		pool.string_bytes = bytes;
		pool.pool_bytes = configfile::fastmem_block_bytes(bytes);
		return pool.overrun();
	};
	TEST_EXPECT(overrun(72, 58) == 8);   // the base game's earlier charattr.def: it ran, its heap corrupt
	TEST_EXPECT(overrun(105, 60) == 41); // the first try of classes 1 to 9: retail's mission start crashed
	TEST_EXPECT(overrun(278, 288) == 0); // JO:CA's own charattr.def
	TEST_EXPECT(overrun(64, 60) == 0 && overrun(65, 60) == 1 && overrun(64, 0) == 0 && overrun(65, 0) == 1);
	std::printf("pool: the allocator's 64; 72/58 8 past, 105/60 41 past, 278/288 under\n");
	return 0;
}

int test_fixtures_and_examples() {
	// The fixtures, on either side of the line.
	const configfile::DataStringsPool at_line = pool_of(fixture_text("pool_at_line.def"));
	TEST_EXPECT(!at_line.binary && at_line.values == 64 && at_line.string_bytes == 60 && at_line.pool_bytes == 64 &&
	            at_line.overrun() == 0);
	const configfile::DataStringsPool past = pool_of(fixture_text("pool_past_line.def"));
	TEST_EXPECT(past.values == 65 && past.string_bytes == 60 && past.pool_bytes == 64 && past.overrun() == 1);
	// The examples' files as the reader counts them: the earlier base game's (six classes, AutoScope, Medic,
	// AutoScope, AutoScope, KnifeBonus, KnifeBonus: 72 values over 58 bytes) and the first try of classes 1 to 9
	// (2 to 4 with no word: 105 over 60).
	const configfile::DataStringsPool earlier =
			pool_of(charattr_text({ "AutoScope", "Medic", "AutoScope", "AutoScope", "KnifeBonus", "KnifeBonus" }, true));
	TEST_EXPECT(earlier.values == 72 && earlier.string_bytes == 58 && earlier.overrun() == 8);
	const configfile::DataStringsPool first_try = pool_of(charattr_text(
			{ "AutoScope", "", "", "", "Medic", "AutoScope", "SpreadBonus", "KnifeBonus", "KnifeBonus" }, false));
	TEST_EXPECT(first_try.values == 105 && first_try.string_bytes == 60 && first_try.overrun() == 41);
	std::printf("fixtures: 64/60 at the line, 65/60 one past; the examples' files 8 and 41 past\n");
	return 0;
}

int test_reader_count() {
	// No file, or the CBIN form (the binary reader's): no text pool.
	TEST_EXPECT(pool_of(std::string()).values == 0 && pool_of(std::string()).overrun() == 0);
	const configfile::DataStringsPool binary = pool_of(std::string("CBIN\x01\x00\x00\x00", 8));
	TEST_EXPECT(binary.binary && binary.overrun() == 0);
	// Words and numbers: a word its length and one, a number nothing; a value outside any section is none.
	const configfile::DataStringsPool plain = pool_of(std::string("K = 9\r\n[S]\r\nK = ab, 7 1.5 -2 cd\r\n"));
	TEST_EXPECT(plain.values == 5 && plain.string_bytes == 6);
	// A value past the line's first 255 bytes [orig: String_CopyN @ 0x75eca0]: the walk cannot read it, so the
	// last value read is counted again ("abc" twice: 8 bytes, not 4 + 3).
	const std::string far = "[S]\r\nK = abc" + std::string(260, ',') + "zz\r\n";
	const configfile::DataStringsPool past_window = pool_of(far);
	TEST_EXPECT(past_window.values == 2 && past_window.string_bytes == 8);
	// A token the 255 bytes cut is read cut: "K = " and 241 commas leave 10 of its letters.
	const std::string cut = "[S]\r\nK = " + std::string(241, ',') + "abcdefghijklmnop\r\n";
	TEST_EXPECT(pool_of(cut).values == 1 && pool_of(cut).string_bytes == 11);
	// A key written with leading spaces does not match its own line: the walk reads the next line of the key, a
	// number (no byte), where the entry holds a word [orig: ConfigFile_ReadKeyValue @ 0x75fdfd].
	const configfile::DataStringsPool spaced = pool_of(std::string("[S]\r\n  K = word\r\nK = 1 2\r\n"));
	TEST_EXPECT(spaced.values == 3 && spaced.string_bytes == 0);
	// ... and where a '[' line comes first, nothing is read: the buffer as it was (empty here), one byte.
	const configfile::DataStringsPool stopped = pool_of(std::string("[S]\r\n  K = word\r\n[T]\r\nK = 1\r\n"));
	TEST_EXPECT(stopped.values == 2 && stopped.string_bytes == 1);
	std::printf("reader: the CBIN form none; the 255-byte line, a cut token, a spaced key, the walk's stop\n");
	return 0;
}

const Diagnostic *overrun_of(const std::vector<Diagnostic> &findings) {
	if (findings.size() != 1 || findings[0].code() != "document.config_overrun" ||
	    findings[0].severity != DiagnosticSeverity::Error)
		return nullptr;
	return &findings[0];
}

int test_rule_charattr() {
	const std::string past = fixture_text("pool_past_line.def");
	std::unique_ptr<DocumentBase> document = loaded(past, "charattr.def", AssetKind::CharAttrDefs);
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	const std::vector<Diagnostic> findings = config_overrun_findings(*document);
	const Diagnostic *d = overrun_of(findings);
	TEST_EXPECT(d != nullptr);
	if (!d) return 1;
	// At value 65, the first past the 64-byte pool: CHARACTER6's ATTRIBUTES word, the file's last value.
	const TextDocument &text = *text_of(*document);
	const TextSpan word = text.span_at(past.rfind("KnifeBonus"), 0);
	TEST_EXPECT(d->line == word.line && d->column == word.column);
	TEST_EXPECT(contains(d->message, "65 values") && contains(d->message, "60 bytes") &&
	            contains(d->message, "64 bytes") && contains(d->message, "1 byte of zeros") &&
	            contains(d->message, "ConfigFile_ParseText @ 0x7609e8") && contains(d->message, "Value 65"));
	// Its fix: CHARACTER5's RUN_MODIFIER at 0, which the loader reads the same missing, commented out.
	TEST_EXPECT(d->planned.size() == 1);
	if (d->planned.size() != 1) return 1;
	const PlannedFix &fix = d->planned[0];
	TEST_EXPECT(fix.label == "Comment out the 1 line the game loads the same without" && fix.edits.size() == 1 &&
	            contains(fix.detail, "64 values against its 64-byte buffer") && contains(fix.detail, "Undo takes it back"));
	Diagnostic error;
	TEST_EXPECT(document->apply(fix.edits, error));
	const std::string fixed = text.text();
	TEST_EXPECT(contains(fixed, "\r\n;RUN_MODIFIER = 0\r\n") && fixed.size() == past.size() + 1);
	TEST_EXPECT(config_overrun_findings(*document).empty() && pool_of(fixed).values == 64);
	TEST_EXPECT(charattr::same_rows(table_of(past), table_of(fixed)));
	document->undo();
	TEST_EXPECT(text.text() == past && overrun_of(config_overrun_findings(*document)) != nullptr);
	// At the line: none.
	std::unique_ptr<DocumentBase> at_line = loaded(fixture_text("pool_at_line.def"), "charattr.def", AssetKind::CharAttrDefs);
	TEST_EXPECT(at_line && config_overrun_findings(*at_line).empty());
	// The earlier base game's form: its six SCOPE_MUTE at 0.0 and six RUN_MODIFIER at 0 the fix's lines.
	const std::string earlier =
			charattr_text({ "AutoScope", "Medic", "AutoScope", "AutoScope", "KnifeBonus", "KnifeBonus" }, true);
	std::unique_ptr<DocumentBase> old_base = loaded(earlier, "charattr.def", AssetKind::CharAttrDefs);
	const std::vector<Diagnostic> old_findings = old_base ? config_overrun_findings(*old_base) : std::vector<Diagnostic>();
	const Diagnostic *old_d = overrun_of(old_findings);
	TEST_EXPECT(old_d && contains(old_d->message, "8 bytes of zeros") && old_d->planned.size() == 1 &&
	            old_d->planned[0].edits.size() == 12);
	if (!old_d || old_d->planned.size() != 1) return 1;
	TEST_EXPECT(old_base->apply(old_d->planned[0].edits, error));
	TEST_EXPECT(config_overrun_findings(*old_base).empty() && pool_of(text_of(*old_base)->text()).values == 60 &&
	            charattr::same_rows(table_of(earlier), table_of(text_of(*old_base)->text())));
	// The first try of classes 1 to 9, every value away from 0: no line the loader reads the same without, no fix.
	const std::string first_try = charattr_text(
			{ "AutoScope", "", "", "", "Medic", "AutoScope", "SpreadBonus", "KnifeBonus", "KnifeBonus" }, false);
	std::unique_ptr<DocumentBase> tried = loaded(first_try, "charattr.def", AssetKind::CharAttrDefs);
	const std::vector<Diagnostic> tried_findings = tried ? config_overrun_findings(*tried) : std::vector<Diagnostic>();
	const Diagnostic *tried_d = overrun_of(tried_findings);
	TEST_EXPECT(tried_d && contains(tried_d->message, "105 values") && contains(tried_d->message, "41 bytes of zeros") &&
	            tried_d->planned.empty());
	// Lines the loader never reads go with the zeros: a legacy block's "//KEY = n" entries (no ConfigFile comment)
	// and a section of no class, their words staying (each a byte of the pool); a class after the first the file
	// lacks (CHARACTER9 after a missing 7) is meant to be read, charattr.unread_section's, and is left alone.
	const std::string legacy = earlier + "//STEALTH = 25\r\n//HPBONUS = 2\r\n//ATTRIBUTES = Medic\r\n[NOTES]\r\nCOUNT = 3\r\n"
	                                     "[CHARACTER9]\r\nSTEALTH = 1\r\n";
	std::unique_ptr<DocumentBase> with_legacy = loaded(legacy, "charattr.def", AssetKind::CharAttrDefs);
	std::vector<size_t> idle;
	if (with_legacy) charattr_idle_lines(*text_of(*with_legacy), idle);
	TEST_EXPECT(idle.size() == 16 && std::count(idle.begin(), idle.end(), at(legacy, "//ATTRIBUTES")) == 1 &&
	            std::count(idle.begin(), idle.end(), at(legacy, "COUNT = 3")) == 1 &&
	            std::count(idle.begin(), idle.end(), at(legacy, "STEALTH = 1")) == 0);
	// The fix takes the fifteen of numbers alone (77 values over 64 bytes, then 62).
	const std::vector<Diagnostic> legacy_findings = with_legacy ? config_overrun_findings(*with_legacy) : std::vector<Diagnostic>();
	const Diagnostic *legacy_d = overrun_of(legacy_findings);
	TEST_EXPECT(legacy_d && legacy_d->planned.size() == 1 && legacy_d->planned[0].edits.size() == 15 &&
	            contains(legacy_d->planned[0].detail, "62 values"));
	// The base game's earlier file with no CHARACTER2 (its classes 5 and on never read): only class 1's two zeros
	// are idle, which leave it past the line, so no fix (charattr.unread_section says what is wrong).
	const std::string gap = "[CHARACTER1]\r\n" + earlier.substr(at(earlier, "STEALTH"), at(earlier, "[CHARACTER2]") - at(earlier, "STEALTH")) +
	                        "[CHARACTER5]\r\n" + earlier.substr(at(earlier, "[CHARACTER2]") + 14);
	std::unique_ptr<DocumentBase> gapped = loaded(gap, "charattr.def", AssetKind::CharAttrDefs);
	std::vector<size_t> gap_idle;
	if (gapped) charattr_idle_lines(*text_of(*gapped), gap_idle);
	const std::vector<Diagnostic> gap_findings = gapped ? config_overrun_findings(*gapped) : std::vector<Diagnostic>();
	const Diagnostic *gap_d = overrun_of(gap_findings);
	TEST_EXPECT(gap_idle.size() == 2 && gap_d && contains(gap_d->message, "72 values") && gap_d->planned.empty());
	std::printf("charattr: the row at value 65, its fix applied (the same classes) and undone; the examples\n");
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
	const Diagnostic *d = overrun_of(credit_findings);
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
	editor_test::TempProjectDir dir("opennova_editor_config_overrun");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Pool"));
	editor_test::create_missing_files(session);
	const AssetEntry *entry = session.view().project.scan->find("charattr.def");
	const std::string path = entry ? entry->relative_path : std::string("charattr.def");
	const std::string past = fixture_text("pool_past_line.def");
	TEST_EXPECT(editor_test::write_bytes(session.view().project.root + "/" + path,
	                                     std::vector<uint8_t>(past.begin(), past.end())));
	editor_test::handle_to_end(session, request::rescan());
	const auto finding = [&]() -> const Diagnostic * {
		for (const Diagnostic &d : session.view().findings.diagnostics)
			if (d.code() == "document.config_overrun" && d.asset == path) return &d;
		return nullptr;
	};
	const Diagnostic *d = finding();
	TEST_EXPECT(d && session.document_for(path) == nullptr);
	if (!d) return 1;
	const std::vector<ProblemFix> fixes = fixes_for(*d, session.view());
	TEST_EXPECT(fixes.size() == 1 && fixes[0].request.kind == EditorRequestKind::EditRecord &&
	            fixes[0].request.path == path && fixes[0].request.open_first && !fixes[0].bulk);
	if (fixes.size() != 1) return 1;
	editor_test::handle_to_end(session, fixes[0].request);
	TEST_EXPECT(session.outcome().done() && !finding());
	editor_test::handle_to_end(session, request::undo(path));
	TEST_EXPECT(finding() != nullptr);
	std::printf("session: the closed charattr.def's row, its fix applied and undone\n");
	return 0;
}

// Every file JO:CA's game reads through the text reader: charattr.def [orig: CharAttr_LoadFromDef @ 0x412177] and
// each DATASOURCE a menu names [orig: CMarqueeWnd_LoadCreditsFromIni @ 0x65c5fe -> ConfigFile_LoadGlobal]; the
// reader's other entry, IniFile_LoadGlobal @ 0x760b00, has no caller.
int test_retail() {
	int ran = 0;
	const auto check = [&](const std::vector<uint8_t> &bytes, const std::string &what, bool charattr) -> int {
		const configfile::DataStringsPool pool = pool_of(bytes);
		std::printf("  %s: %s%u values, %u bytes of text values, a %u-byte pool\n", what.c_str(),
		            pool.binary ? "CBIN (the binary reader), " : "", pool.values, pool.string_bytes, pool.pool_bytes);
		TEST_EXPECT(pool.overrun() == 0);
		if (charattr) {
			TEST_EXPECT(!pool.binary && pool.values == 278 && pool.string_bytes == 288 && pool.pool_bytes == 320);
			const std::string text(bytes.begin(), bytes.end());
			std::unique_ptr<DocumentBase> document = loaded(text, "charattr.def", AssetKind::CharAttrDefs);
			TEST_EXPECT(document && config_overrun_findings(*document).empty());
		}
		++ran;
		return 0;
	};
	int failures = 0;
	const std::string install = retail::install();
	if (!install.empty()) {
		std::vector<std::string> mounts{ std::string() };
		for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
		for (const std::string &expansion : mounts) {
			Vfs vfs;
			TEST_EXPECT(vfs.mount_game(install, expansion, VfsMountMode::Packed));
			const std::string where = expansion.empty() ? std::string("the install") : expansion;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(vfs.read_file("charattr.def", bytes) && !bytes.empty());
			failures += check(bytes, where + "'s charattr.def", true);
			// The DATASOURCEs the shipped menus name.
			std::set<std::string> sources;
			const std::regex tag("<DATASOURCE>\\s*([^<\\s]+)\\s*</DATASOURCE>", std::regex::icase);
			for (const VfsFileLocation &file : vfs.list_files()) {
				const std::string &name = file.logical_name;
				if (name.size() < 4 || retail::lower_ascii(name.substr(name.size() - 4)) != ".mnu") continue;
				std::vector<uint8_t> menu;
				if (!vfs.read_file(name, menu)) continue;
				const std::string menu_text(menu.begin(), menu.end());
				for (std::sregex_iterator it(menu_text.begin(), menu_text.end(), tag), end; it != end; ++it)
					sources.insert(retail::lower_ascii((*it)[1].str()));
			}
			TEST_EXPECT(!sources.empty());
			for (const std::string &source : sources) {
				std::vector<uint8_t> credits;
				TEST_EXPECT(vfs.read_file(source, credits) && !credits.empty());
				failures += check(credits, where + "'s " + source, false);
			}
		}
	} else {
		retail::skip_leg("OPENNOVA_JO_DIR (the packed install's charattr.def and menus)");
	}
	const std::string path = retail::asset_file("charattr.def");
	if (!path.empty()) {
		std::vector<uint8_t> bytes;
		TEST_EXPECT(test_io::read_file(path, bytes));
		failures += check(bytes, path, true);
	} else if (retail::selected()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (the extracted charattr.def)");
	}
	if (ran) std::printf("retail: %d of JO:CA's ConfigFile reads, none past its pool\n", ran);
	return failures;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_pool_arithmetic();
	failures += test_fixtures_and_examples();
	failures += test_reader_count();
	failures += test_rule_charattr();
	failures += test_rule_other_kinds();
	failures += test_blanks();
	failures += test_session();
	failures += test_retail();
	if (failures == 0) std::printf("editor_config_overrun: all passed\n");
	return failures == 0 ? 0 : 1;
}
