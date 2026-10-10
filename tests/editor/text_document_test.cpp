// S13 D9 (ADR 0046 S13, "text documents and span references"): the text document and the text types.
// The model (TextDocument): a text held as the game reads it, as lines (a CR before an LF is the
// line's end, a lone CR its text), its places "line:column"; a batch of span replacements one step,
// each against the text as the ones before left it, refused whole for a span outside the text or a
// change of another kind; undo and redo byte for byte; a gesture's batches and a typing burst folding
// into one step, never over the saved checkpoint; changes_since answering the spans that changed
// (after an edit, its undo and its redo, a fold read mid-way, a discarded branch, another load);
// the history under its budget; a snapshot read only. The script type: the WAC compiler's reports at
// their place but the lookups of names other files hold, and its operands' names as references with
// their spans. Through a session over a project: the script's FX:, AMMO: and TT: names resolve
// against a minted particle file, an ammo table (one through the lookup's "ammo_" fallback) and a
// string table, a definition's "Referenced by" reaching the script's span; an open script standing in
// for its file in the graph; Rename everywhere rewriting the span, the compile still resolving it;
// a Go to opening the script at a span (a RevealText event); a compile report a Problems row at its
// line and column; the wire's span edits, refusals and pages of lines. The other types: a credits
// file through the CBIN codec byte for byte and an edit written back in the form, one the text form
// cannot carry held read only; a music script through its MUS text byte for byte, one with a message
// file of two scripts held read only, a text that does not compile refused at its line and column; a shader in
// the shader loader's SCR form byte for byte, a plain one a finding Save fixes. After the review: a
// line end a type's reader reads otherwise (a script's LF alone and CR alone, a credits text's LF
// alone) a finding a Rewrite fixes, Save writing every line CR LF as a step of the text; a credits
// text's line the reader does not read whole refused, never saved short; a declared name no
// reference; a text key's use one Rename everywhere leaves alone (refused); a rename that would take
// over a use reaching another ammo through its fallback refused, and one of a definition reached
// through it keeping the prefix once; a stale text site partial; the rename preview's text sites; a
// closed music script's spans read by its scan's kind; a shader imported as stored; one compile of
// a text for its findings and its references; a snapshot copying the text alone. The retail leg
// (OPENNOVA_JO_DIR): the install's scripts, music scripts, credits and shaders read and validated,
// its counts pinned.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/assets/install_view.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/credits_type.h>
#include <editor/documents/document_types.h>
#include <editor/documents/line_ends.h>
#include <editor/documents/music_script_type.h>
#include <editor/documents/script_type.h>
#include <editor/documents/text_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_names.h>
#include <editor/graph/reference_queries.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_plan.h>
#include <editor/model/text_document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_query.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view_json.h>
#include <formats/cbin/binary_config.h>
#include <formats/cbin/cbin.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mus/mus.h>
#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>
#include <formats/scr/scr.h>
#include <base/io/sha256.h>
#include <net/novacrypto/pubcrypto.h>
#include <runtime/wac/compiler.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "wac/wac_listing.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

std::string repo_file(const std::string &relative) {
	const std::vector<uint8_t> bytes = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative);
	return std::string(bytes.begin(), bytes.end());
}

TextSpan span(size_t line, size_t column, size_t length) {
	TextSpan out;
	out.line = line;
	out.column = column;
	out.length = length;
	return out;
}

// A text document over `text` (a script's: the file is its text).
std::unique_ptr<TextDocument> text_document(const std::string &text, const std::string &name = "t.wac",
		AssetKind kind = AssetKind::Script) {
	auto document = std::make_unique<TextDocument>();
	Diagnostic error;
	document->load_bytes(bytes_of(text), name, kind, "jo", error);
	return document;
}

bool apply(DocumentBase &document, const std::vector<Edit> &edits) {
	Diagnostic error;
	return document.apply(edits, error);
}

const TextChanges *spans_of(const ChangeSet &changes) { return std::get_if<TextChanges>(&changes); }

// A payload no text type makes.
struct OtherChange : EditPayload {
	const char *token() const override { return "other.change"; }
};

// The minted music script twice in one file (two chunks: the MUS text carries the first alone).
std::vector<uint8_t> script_in_two_chunks() {
	const std::string bin = repo_file("mus/synth_gamemus.bin");
	opennova::mus::MusFile file{};
	std::vector<uint8_t> out;
	if (opennova::mus::mus_open_memory(&file, reinterpret_cast<const uint8_t *>(bin.data()), bin.size()) != 0)
		return out;
	const opennova::mus::MusScript *scripts[] = {&file.scripts[0], &file.scripts[0]};
	uint8_t *buffer = nullptr;
	size_t size = 0;
	if (opennova::mus::mus_encode_file(scripts, 2, &buffer, &size) == 0) out.assign(buffer, buffer + size);
	opennova::mus::mus_free(buffer);
	opennova::mus::mus_close(&file);
	return out;
}

// A shader in the shader loader's SCR form: version 1, the text and a NUL under the shaders' key.
std::vector<uint8_t> scr_shader(const std::string &text) {
	std::string payload = text + std::string(1, '\0');
	opennova::scr::scr_encrypt(reinterpret_cast<uint8_t *>(payload.data()), payload.size(),
	                           opennova::scr::SCR_KEY_SHADERS);
	return bytes_of(std::string("SCR\x01", 4) + payload);
}

// A CBIN credits file laid out as the shipped one is (its [ENV] values, then its [TEXT] lines: a
// justify, a colour, a line with its font, a line end, an image; a space written '_', which the
// marquee draws as one), minted through the form's writer.
std::vector<uint8_t> minted_credits() {
	using Config = opennova::cbin::BinaryConfig;
	Config config;
	config.strings = {"env", "text", "scroll_rate", "vertical_space", "center_x", "~JR", "~CFF0000",
	                  "Joint_Operations:", "Serpen24", "<CR>", "~F0|0|cr1.png"};
	config.xor_key = 0x5EEDF00Du;
	const auto value = [](uint32_t raw, uint32_t flags) { return Config::Value{raw, flags}; };
	float half = 0.5f;
	uint32_t half_bits = 0;
	std::memcpy(&half_bits, &half, sizeof half_bits);
	Config::Label env{1, {{3, {value(half_bits, Config::kFloat)}}, {4, {value(14, Config::kInteger)}},
	                      {5, {value(400, Config::kInteger)}}}};
	Config::Label text{2, {{2, {value(6, Config::kString)}}, {2, {value(7, Config::kString)}},
	                       {2, {value(8, Config::kString), value(9, Config::kString)}},
	                       {2, {value(10, Config::kString)}}, {2, {value(11, Config::kString)}}}};
	config.labels = {env, text};
	std::vector<uint8_t> out;
	std::string error;
	opennova::cbin::encode_binary_config(config, out, error);
	return out;
}

// The listings the original compiler wrote of the shipped scripts (wac_corpus's vectors, by each
// source's SHA-256), which the runtime's own corpus test holds its compiler to.
struct CorpusVector {
	const char *source_sha256;
	const char *listing_sha256;
};
const CorpusVector kCorpus[] = {
#include "wac/fixtures/wac_retail_corpus_vectors.inc"
};

std::string sha256(const std::string &bytes) {
	return opennova::io::sha256_hex(bytes.data(), bytes.size());
}

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; });
}

} // namespace

// The lines and their places: an LF ends a line, a CR before it part of its end, a lone CR text.
static int test_lines_and_places() {
	const auto document = text_document("one\r\ntwo\nthree\rstill\r\n");
	TEST_EXPECT(!document->blocked() && document->line_count() == 4);
	TEST_EXPECT(document->line(1) == "one" && document->line(2) == "two" && document->line(3) == "three\rstill" &&
	            document->line(4).empty());
	size_t offset = 0;
	TEST_EXPECT(document->offset_of(2, 1, offset) && offset == 5);
	TEST_EXPECT(document->offset_of(2, 4, offset) && offset == 8); // one past the line's last character
	TEST_EXPECT(!document->offset_of(2, 5, offset) && !document->offset_of(5, 1, offset) &&
	            !document->offset_of(0, 1, offset));
	const TextSpan at = document->span_at(10, 3);
	TEST_EXPECT(at.line == 3 && at.column == 2 && at.length == 3);
	std::string text;
	TEST_EXPECT(document->span_text(span(1, 2, 5), text) && text == "ne\r\nt");
	TEST_EXPECT(!document->span_text(span(4, 1, 1), text));
	size_t line = 0, column = 0;
	TEST_EXPECT(TextDocument::locator(12, 5) == "12:5" && TextDocument::read_locator("12:5", line, column) &&
	            line == 12 && column == 5);
	TEST_EXPECT(!TextDocument::read_locator("0:1", line, column) && !TextDocument::read_locator("12", line, column) &&
	            !TextDocument::read_locator("a:b", line, column));
	TEST_EXPECT(text_of(*document) == document.get() && !records_of(*document));
	TEST_EXPECT(text_document("")->line_count() == 1);
	return 0;
}

// Span replacements: a batch one step, each against the text as the ones before left it; a span
// outside the text or a change of another kind refuses the batch with nothing committed; undo and
// redo byte for byte; a span given its own text no step.
static int test_span_edits() {
	const std::string original = "alpha beta\r\ngamma\r\n";
	const auto document = text_document(original);
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(1, 7, 4), "BETA"), TextDocument::replace(span(2, 1, 5), "g")}));
	TEST_EXPECT(document->text() == "alpha BETA\r\ng\r\n" && document->revision() == 1 && document->dirty());
	TEST_EXPECT(document->serialize().text == document->text());
	// A line end inserted moves the lines after it.
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(1, 6, 1), "\r\n")}));
	TEST_EXPECT(document->line_count() == 4 && document->line(2) == "BETA" && document->line(3) == "g");
	const std::string edited = document->text();
	// Refused: a span past its line, past the text, a change of another kind; nothing committed.
	Diagnostic error;
	const uint64_t revision = document->revision();
	TEST_EXPECT(!document->apply({TextDocument::replace(span(1, 1, 1), "x"), TextDocument::replace(span(2, 9, 0), "y")},
	                             error) &&
	            error.code() == "document.span");
	TEST_EXPECT(!document->apply(TextDocument::replace(span(3, 1, 99), "z"), error) && error.code() == "document.span");
	Edit other;
	other.operation = EditOperation::Apply;
	other.payload = std::make_shared<OtherChange>();
	TEST_EXPECT(!document->apply(other, error) && error.code() == "document.payload");
	Edit set;
	set.operation = EditOperation::Set;
	TEST_EXPECT(!document->apply(set, error) && error.code() == "document.payload");
	TEST_EXPECT(document->text() == edited && document->revision() == revision);
	// Its own text: no step.
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(2, 1, 4), "BETA")}) && document->revision() == revision);
	// Undo and redo, byte for byte.
	document->undo();
	TEST_EXPECT(document->text() == "alpha BETA\r\ng\r\n");
	document->undo();
	TEST_EXPECT(document->text() == original && !document->dirty() && !document->can_undo());
	document->redo();
	document->redo();
	TEST_EXPECT(document->text() == edited && !document->can_redo() && document->line(2) == "BETA");
	return 0;
}

// Folding: a gesture's batches one step, a typing burst's (coalesced) one step, until the group
// ends; never over the saved checkpoint. changes_since: the spans that changed since a state read,
// after an edit, its undo and its redo, a fold read mid-way; false for a discarded branch and
// another load.
static int test_folding_and_changes() {
	const auto document = text_document("abc\r\ndef\r\n");
	const uint64_t load = document->load_generation();
	const uint64_t gesture = next_edit_gesture();
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(1, 1, 1), "A", false, gesture)}));
	const uint64_t mid = document->revision();
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(2, 3, 1), "F", false, gesture)}));
	TEST_EXPECT(document->text() == "Abc\r\ndeF\r\n");
	document->undo();
	TEST_EXPECT(document->text() == "abc\r\ndef\r\n" && !document->can_undo()); // one step
	document->redo();
	ChangeSet changes;
	// From the start: two spans; from the fold's middle state: the second alone.
	TEST_EXPECT(document->changes_since(load, 0, changes) && spans_of(changes) &&
	            spans_of(changes)->spans.size() == 2);
	TEST_EXPECT(spans_of(changes)->spans[0].line == 1 && spans_of(changes)->spans[0].column == 1 &&
	            spans_of(changes)->spans[1].line == 2 && spans_of(changes)->spans[1].column == 3);
	TEST_EXPECT(document->changes_since(load, mid, changes) && spans_of(changes)->spans.size() == 1 &&
	            spans_of(changes)->spans[0].line == 2);
	TEST_EXPECT(document->changes_since(load, document->revision(), changes) && spans_of(changes)->spans.empty());
	// Typing: coalesced batches fold until the group ends.
	document->end_edit_group();
	const uint64_t before_typing = document->revision();
	for (const char *typed : {"x", "y", "z"})
		TEST_EXPECT(apply(*document, {TextDocument::replace(span(1, 4, 0), typed, true)}));
	TEST_EXPECT(document->line(1) == "Abczyx");
	document->undo();
	TEST_EXPECT(document->line(1) == "Abc" && document->revision() == before_typing);
	document->redo();
	// The span typed into reads as one run since the state before the burst.
	TEST_EXPECT(document->changes_since(load, before_typing, changes) && spans_of(changes)->spans.size() == 1 &&
	            spans_of(changes)->spans[0].column == 4 && spans_of(changes)->spans[0].length == 3);
	// After its undo, the place it was taken from (a span of no length).
	const uint64_t typed = document->revision();
	document->undo();
	TEST_EXPECT(document->changes_since(load, typed, changes) && spans_of(changes)->spans.size() == 1 &&
	            spans_of(changes)->spans[0].length == 0);
	// A discarded branch: an edit after an undo.
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(2, 1, 0), "!")}));
	TEST_EXPECT(!document->changes_since(load, typed, changes));
	// Another load.
	Diagnostic error;
	TEST_EXPECT(document->load_bytes(bytes_of("abc\r\n"), "t.wac", AssetKind::Script, "jo", error));
	TEST_EXPECT(!document->changes_since(load, 0, changes));
	// The saved checkpoint is never folded over: a gesture's batch after a save is a step of its own,
	// whose undo gives the saved text back, clean; one more undo the text before, unsaved.
	editor_test::TempProjectDir dir("opennova_editor_text_checkpoint");
	const std::string file = dir.file("checkpoint.cfg");
	TEST_EXPECT(editor_test::write_text(file, "one\r\n"));
	TextDocument saved;
	TEST_EXPECT(saved.load(file, "checkpoint.cfg", AssetKind::Config, "jo", error));
	const uint64_t drag = next_edit_gesture();
	TEST_EXPECT(apply(saved, {TextDocument::replace(span(1, 1, 0), "a", false, drag)}));
	TEST_EXPECT(saved.save(error) && !saved.dirty());
	TEST_EXPECT(apply(saved, {TextDocument::replace(span(1, 2, 0), "b", false, drag)}));
	TEST_EXPECT(saved.line(1) == "abone" && saved.dirty());
	saved.undo();
	TEST_EXPECT(saved.line(1) == "aone" && !saved.dirty() && saved.can_undo());
	saved.undo();
	TEST_EXPECT(saved.line(1) == "one" && saved.dirty() && !saved.can_undo());
	return 0;
}

// What changed since a state, where a later edit changed the length of the text before a range it
// tracks: the range moves with the text, the insert a span of its own.
static int test_changes_moved() {
	const auto document = text_document("abc\r\ndef\r\n");
	const uint64_t load = document->load_generation();
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(2, 1, 3), "DEF")}));
	const uint64_t middle = document->revision();
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(1, 1, 0), "XYZW")}));
	TEST_EXPECT(document->text() == "XYZWabc\r\nDEF\r\n");
	ChangeSet changes;
	TEST_EXPECT(document->changes_since(load, 0, changes) && spans_of(changes) && spans_of(changes)->spans.size() == 2);
	if (!spans_of(changes) || spans_of(changes)->spans.size() != 2) return 1;
	const TextSpan &insert = spans_of(changes)->spans[0], &moved = spans_of(changes)->spans[1];
	TEST_EXPECT(insert.line == 1 && insert.column == 1 && insert.length == 4);
	TEST_EXPECT(moved.line == 2 && moved.column == 1 && moved.length == 3);
	TEST_EXPECT(document->changes_since(load, middle, changes) && spans_of(changes)->spans.size() == 1 &&
	            spans_of(changes)->spans[0].length == 4);
	return 0;
}

// The history keeps at most its budget (64 MiB) of replacements, its oldest steps given up first and
// never the last; dirty stays exact.
static int test_history_budget() {
	const auto document = text_document("x");
	const size_t eight = size_t(8) << 20;
	for (int i = 0; i < 10; ++i)
		TEST_EXPECT(apply(*document, {TextDocument::replace(span(1, 1, document->text().size()),
		                                                    std::string(eight, char('a' + i)))}));
	TEST_EXPECT(document->history_bytes() <= (size_t(64) << 20) && document->history_bytes() >= 2 * eight);
	size_t undone = 0;
	while (document->can_undo()) {
		document->undo();
		++undone;
		TEST_EXPECT(document->dirty());
	}
	std::printf("text history: 10 steps of 8 MiB, %zu kept within the 64 MiB budget\n", undone);
	TEST_EXPECT(undone >= 1 && undone < 10);
	// What changed since the start is no longer said: the history gave that state up.
	ChangeSet changes;
	TEST_EXPECT(!document->changes_since(document->load_generation(), 0, changes));
	// One step past the budget is kept.
	const auto big = text_document("y");
	TEST_EXPECT(apply(*big, {TextDocument::replace(span(1, 1, 1), std::string(size_t(70) << 20, 'b'))}));
	TEST_EXPECT(big->can_undo());
	big->undo();
	TEST_EXPECT(big->text() == "y");
	return 0;
}

// A snapshot shares the document's identity, load and revision, serializes its text, and is read
// only; a save writes the text and moves the checkpoint.
static int test_snapshot_and_save() {
	editor_test::TempProjectDir dir("opennova_editor_text_save");
	const std::string file = dir.file("game.cfg");
	TEST_EXPECT(editor_test::write_text(file, "name = one\r\n"));
	auto document = std::make_unique<TextDocument>();
	Diagnostic error;
	TEST_EXPECT(document->load(file, "game.cfg", AssetKind::Config, "jo", error));
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(1, 8, 3), "two")}));
	const std::unique_ptr<DocumentBase> snapshot = document->snapshot();
	TEST_EXPECT(snapshot->is_snapshot() && snapshot->identity() == document->identity() &&
	            snapshot->load_generation() == document->load_generation() &&
	            snapshot->revision() == document->revision() && snapshot->serialize().text == "name = two\r\n");
	TEST_EXPECT(!snapshot->apply(TextDocument::replace(span(1, 1, 0), "x"), error) &&
	            error.code() == "document.snapshot");
	// The text alone is copied: the history's state (dirty as its document), none of its steps.
	ChangeSet changes;
	TEST_EXPECT(snapshot->dirty() == document->dirty() && !snapshot->can_undo() && !snapshot->can_redo() &&
	            snapshot->history_bytes() == 0 && document->history_bytes() > 0);
	TEST_EXPECT(snapshot->changes_since(document->load_generation(), document->revision(), changes) &&
	            spans_of(changes) && spans_of(changes)->spans.empty() &&
	            !snapshot->changes_since(document->load_generation(), 0, changes));
	TEST_EXPECT(document->save(error) && !document->dirty() && document->rewrite_need() == DocumentBase::RewriteNeed::None);
	TEST_EXPECT(test_io::read_file_text(file) == "name = two\r\n");
	// A file changed outside the editor: the save is refused.
	TEST_EXPECT(apply(*document, {TextDocument::replace(span(1, 8, 3), "six")}));
	TEST_EXPECT(editor_test::write_text(file, "name = ten\r\n"));
	TEST_EXPECT(!document->save(error) && error.code() == "document.conflict");
	return 0;
}

// The script type: the compiler's reports at their place but a name other files hold, and its
// operands' names with their spans.
static int test_script_type() {
	const std::string fixture = repo_file("wac/text_document.wac");
	TEST_EXPECT(!fixture.empty());
	const DocumentType *type = document_type_for(AssetKind::Script);
	TEST_EXPECT(type && type->id == DocumentTypeId::Script && document_content(*type) == DocumentContent::Text &&
	            type->references && type->fields(0).empty() && !type->project_check);
	const std::unique_ptr<DocumentBase> made = type->make();
	Diagnostic error;
	TEST_EXPECT(made->load_bytes(bytes_of(fixture), "scripts/text_document.wac", AssetKind::Script, "jo", error));
	const TextDocument &script = *text_of(*made);
	TEST_EXPECT(made->serialize().text == fixture && made->rewrite_need() == DocumentBase::RewriteNeed::None);
	// The compiler reports every lookup a miss (no catalog): none is a finding of the file.
	const opennova::wac::Program program = compile_script(script);
	size_t table = 0;
	for (const opennova::wac::Diagnostic &d : program.diagnostics) table += d.table ? 1 : 0;
	// One compile of the text for its findings and its references.
	const size_t compiles = opennova::editor::script_compile_count();
	const std::vector<Diagnostic> findings = type->validate_file(*made);
	const size_t validated = opennova::editor::script_compile_count();
	std::vector<TextReference> once;
	type->references(script, once);
	TEST_EXPECT(validated <= compiles + 1 && opennova::editor::script_compile_count() == validated);
	TEST_EXPECT(type->validate_file(*made).size() == findings.size() &&
	            opennova::editor::script_compile_count() == validated);
	std::printf("script: %zu compiler reports, %zu of names other files hold, %zu findings\n",
	            program.diagnostics.size(), table, findings.size());
	TEST_EXPECT(table >= 4 && findings.empty());
	// Its references: the effect, the sound set, two ammo (the second looked up as ammo_satchel
	// after satchel) and the text key, each at its span, the names as written.
	std::vector<TextReference> references;
	type->references(script, references);
	TEST_EXPECT(references.size() == 5);
	if (references.size() != 5) return 1;
	const auto is = [&](size_t i, ReferenceKind kind, const char *value, size_t line, size_t column) {
		const TextReference &r = references[i];
		std::string written;
		return r.kind == kind && r.value == value && r.span.line == line && r.span.column == column &&
		       r.span.length == r.value.size() && script.span_text(r.span, written) && written == value;
	};
	TEST_EXPECT(is(0, ReferenceKind::Particle, "Buildup", 3, 12));
	TEST_EXPECT(is(1, ReferenceKind::Sound, "EXPLO_BASE", 4, 15));
	TEST_EXPECT(is(2, ReferenceKind::Ammo, "AT_CONTRACT", 5, 16) && references[2].fallback == "ammo_AT_CONTRACT");
	TEST_EXPECT(is(3, ReferenceKind::Ammo, "satchel", 6, 16) && references[3].fallback == "ammo_satchel");
	TEST_EXPECT(is(4, ReferenceKind::TextId, "MISSION_START", 7, 15) && references[4].fallback.empty());
	// A text key of a script of a mission's name reads that mission's table (its stem's .bin, else
	// medmssn.bin, never both) and then gametext.bin (S14), where the project has the mission (the
	// owner): a use Rename everywhere rewrites there, as the others are (the graph's, below).
	const std::vector<std::string> tables_after = {"GAMETEXT.BIN"};
	TEST_EXPECT(references[4].scope == "TEXT_DOCUMENT.BIN" && references[4].scope_alternate == "MEDMSSN.BIN" &&
	            references[4].scope_owner == "TEXT_DOCUMENT.BMS" && references[4].scopes_after == tables_after);
	TEST_EXPECT(references[4].rewritable && references[0].rewritable && references[3].rewritable);
	// game.wac runs with every mission: its key reads whichever table plays, any table here, and no
	// rename rewrites it.
	{
		const auto shared = text_document("If true(bluekills) then\r\n\tssnname 1 TT_MISSION_START\r\nendif\r\n", "game.wac");
		std::vector<TextReference> keys;
		script_references(*shared, keys);
		TEST_EXPECT(keys.size() == 1 && keys[0].kind == ReferenceKind::TextId && keys[0].scope.empty() &&
		            keys[0].scopes_after.empty() && keys[0].scope_alternate.empty() && keys[0].scope_owner.empty() &&
		            !keys[0].rewritable);
	}
	// The files a script names (S14): a wave by its string (past the quote), a RUN's script by the name
	// written, the compiler's own name (to the first '.', then ".wac") its second where the written one
	// does not reach it; each a word the device colours.
	{
		const auto files = text_document("If true(bluekills) then\r\n\twave \"intro.wav\"\r\n\tSSNwave 1, radio.WAV, 50\r\nendif\r\n"
		                                 "RUN other.txt\r\nrun patrol\r\n",
		                                 "first.wac");
		std::vector<TextReference> named;
		script_references(*files, named);
		TEST_EXPECT(named.size() == 4);
		if (named.size() != 4) return 1;
		const auto file_at = [&](size_t i, ReferenceKind kind, const char *value, size_t line, size_t column, const char *fallback) {
			const TextReference &r = named[i];
			std::string written;
			return r.kind == kind && r.value == value && r.span.line == line && r.span.column == column &&
			       r.span.length == r.value.size() && files->span_text(r.span, written) && written == value &&
			       r.fallback == fallback && r.rewritable;
		};
		TEST_EXPECT(file_at(0, ReferenceKind::Wave, "intro.wav", 2, 8, ""));
		TEST_EXPECT(file_at(1, ReferenceKind::Wave, "radio.WAV", 3, 13, ""));
		TEST_EXPECT(file_at(2, ReferenceKind::Script, "other.txt", 5, 5, "OTHER.wac"));
		TEST_EXPECT(file_at(3, ReferenceKind::Script, "patrol", 6, 5, ""));
		const opennova::wac::Program naming = compile_script(*files);
		TEST_EXPECT(naming.file_uses.size() == 4 && naming.file_uses[0].kind == opennova::wac::FileUse::Kind::Wave &&
		            naming.file_uses[0].name == "intro.wav" && naming.file_uses[3].kind == opennova::wac::FileUse::Kind::Run &&
		            naming.file_uses[3].name == "PATROL.wac");
		std::vector<TextHighlight> words;
		script_highlights(*files, words);
		size_t keywords = 0, commands = 0, operands = 0;
		for (const TextHighlight &word : words) {
			keywords += word.kind == TextHighlightKind::Keyword;
			commands += word.kind == TextHighlightKind::Command;
			operands += word.kind == TextHighlightKind::Operand;
		}
		// If, then, endif, RUN, run; true, wave, SSNwave; the two waves' tokens and the two RUNs' names.
		TEST_EXPECT(keywords == 5 && commands == 3 && operands == 4);
	}
	// A declared name is a name the script gives, never one it looks up: no reference (a VAR's, a
	// CHEAT's; each refused as a name in use, the pool answering its leg).
	for (const char *text : {"VAR AMMO_COUNT\r\n", "CHEAT FX_GLOW\r\n"}) {
		const auto declared = text_document(text, "declared.wac");
		std::vector<TextReference> none;
		script_references(*declared, none);
		const opennova::wac::Program declaring = compile_script(*declared);
		TEST_EXPECT(none.empty() && declaring.catalog_lookups.size() == 1 &&
		            declaring.catalog_lookups[0].declaration);
	}
	// A compile error: a finding at its line and column, a Warning (the game runs the script as it
	// compiled). A RUN names a file it does not read: no report.
	const auto flawed = text_document("RUN other\r\nfxrain FX_Buildup )\r\n", "flawed.wac");
	const std::vector<Diagnostic> reported = validate_script_file(*flawed);
	TEST_EXPECT(reported.size() == 1);
	if (reported.empty()) return 1;
	std::printf("script: the flawed one reports \"%s\" at %zu:%zu\n", reported[0].message.c_str(), reported[0].line,
	            reported[0].column);
	TEST_EXPECT(reported[0].code() == "script.compile" && reported[0].severity == DiagnosticSeverity::Warning &&
	            reported[0].line == 2 && reported[0].column == 19 && reported[0].asset == "flawed.wac" &&
	            reported[0].message.find("Unexpected )") != std::string::npos);
	return 0;
}

// A line end a type's reader reads otherwise: a script's LF alone (its reader ends a line at a CR and
// reads an LF as a blank) lets a comment run on into the lines after it; a finding a Rewrite fixes,
// Save writing every line CR LF and the document taking that as a step (its undo the old line ends,
// unsaved). A CR alone in a script, an LF alone in a credits text; a text's file written as it holds
// it.
static int test_line_ends() {
	editor_test::TempProjectDir dir("opennova_editor_text_line_ends");
	const std::string file = dir.file("ends.wac");
	const std::string original = "If true(bluekills) then\r\n\tfxrain FX_Buildup\r\nendif\r\n";
	TEST_EXPECT(editor_test::write_text(file, original));
	const DocumentType *type = document_type_for(AssetKind::Script);
	std::unique_ptr<DocumentBase> made = type->make();
	Diagnostic error;
	TEST_EXPECT(made->load(file, "ends.wac", AssetKind::Script, "jo", error));
	TextDocument &script = *text_of(*made);
	TEST_EXPECT(script.line_ends() == TextLineEnds::Cr && type->validate_file(*made).empty());
	const auto effects = [&] {
		std::vector<TextReference> named;
		type->references(script, named);
		return size_t(std::count_if(named.begin(), named.end(),
		                            [](const TextReference &r) { return r.kind == ReferenceKind::Particle; }));
	};
	TEST_EXPECT(effects() == 1);
	// A comment and a line put in with LFs alone: the editor shows them as lines; the reader runs the
	// comment on to the next CR, over both effect lines.
	TEST_EXPECT(apply(*made, {TextDocument::replace(span(2, 1, 0), "; note\n\tfxrain FX_Smoke\n")}));
	TEST_EXPECT(script.line_count() == 6 && script.line(3) == "\tfxrain FX_Smoke" && effects() == 0);
	const std::vector<Diagnostic> findings = type->validate_file(*made);
	const auto ending = std::find_if(findings.begin(), findings.end(),
	                                 [](const Diagnostic &d) { return d.code() == "script.line_ending"; });
	TEST_EXPECT(ending != findings.end() && ending->line == 2 && ending->column == 7 && ending->row() &&
	            ending->row()->fixes == FindingFix::Rewrite && ending->severity == DiagnosticSeverity::Warning &&
	            ending->message.find("2 line ends") != std::string::npos);
	TEST_EXPECT(made->rewrite_need() == DocumentBase::RewriteNeed::Rewrite);
	// Save: every line CR LF, the document holding what it wrote as a step, clean; its effects read.
	const uint64_t before_save = made->revision();
	TEST_EXPECT(made->save(error) && !made->dirty() && made->revision() != before_save);
	TEST_EXPECT(test_io::read_file_text(file) ==
	            "If true(bluekills) then\r\n; note\r\n\tfxrain FX_Smoke\r\n\tfxrain FX_Buildup\r\nendif\r\n");
	TEST_EXPECT(script.text() == test_io::read_file_text(file) && effects() == 2 &&
	            type->validate_file(*made).empty() && made->rewrite_need() == DocumentBase::RewriteNeed::None);
	// Its undo gives the old line ends back, unsaved; the next the text before the insert.
	made->undo();
	TEST_EXPECT(script.line(2) == "; note" && made->dirty() && script.odd_line_end() != std::string::npos);
	made->undo();
	TEST_EXPECT(script.text() == original && made->dirty());
	// A CR alone in a script: the reader ends a line there.
	std::unique_ptr<DocumentBase> lone = type->make();
	TEST_EXPECT(lone->load_bytes(bytes_of("a\rb\r\n"), "lone.wac", AssetKind::Script, "jo", error));
	const std::vector<Diagnostic> crs = type->validate_file(*lone);
	TEST_EXPECT(has_code(crs, "script.line_ending") && lone->serialize().text == "a\r\nb\r\n");
	// A credits text's LF alone: the ConfigFile reader ends a line at CR LF (the line-ends rule's).
	const DocumentType *credits = document_type_for(AssetKind::Credits);
	std::unique_ptr<DocumentBase> listed = credits->make();
	TEST_EXPECT(listed->load_bytes(bytes_of("[ENV]\nscroll_rate = 0.5\n"), "lf.kda", AssetKind::Credits, "jo", error));
	const std::vector<Diagnostic> lfs = line_end_findings(*listed, "jo");
	TEST_EXPECT(has_code(lfs, "document.line_ends") && lfs[0].line == 1 && lfs[0].column == 6 &&
	            listed->serialize().text == "[ENV]\r\nscroll_rate = 0.5\r\n");
	// A text's file is written as it holds it.
	std::unique_ptr<DocumentBase> plain = document_type_for(AssetKind::Text)->make();
	TEST_EXPECT(plain->load_bytes(bytes_of("a\nb\r"), "notes.txt", AssetKind::Text, "jo", error) &&
	            plain->serialize().text == "a\nb\r" && text_of(*plain)->odd_line_end() == std::string::npos);
	return 0;
}

// A project holding the script, the minted particle file, an ammo table and the string table of the
// script's name (the mission text a script of that mission's name reads its keys from, S14).
struct ScriptProject {
	editor_test::TempProjectDir dir{"opennova_editor_text_project"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root() const { return dir.file("project"); }
	const SessionView &view() const { return session.view(); }
	const AssetGraph &graph() const { return *session.view().findings.graph; }
};

bool make_script_project(ScriptProject &project) {
	editor_test::handle_to_end(project.session, request::new_project(project.root(), "Texts"));
	editor_test::create_missing_files(project.session);
	opennova::rtxt::File table;
	table.sections.push_back({"mission", 1});
	table.entries.push_back({"MISSION_START", "The mission begins.", {}, 0});
	std::vector<uint8_t> strings;
	std::string error;
	if (!opennova::rtxt::write(table, strings, error)) return false;
	const std::string root = project.root();
	const bool written =
	        editor_test::write_text(root + "/scripts/text_document.wac", repo_file("wac/text_document.wac")) &&
	        editor_test::write_text(root + "/particles/effects.ptl", repo_file("particle/synth_minimal_effect.ptl")) &&
	        editor_test::write_text(root + "/defs/ammo.def",
	                                "ammo AT_CONTRACT\nmax_age 1.5\nend\nammo ammo_satchel\nmax_age 2\nend\n"
	                                "ammo bomb\nmax_age 3\nend\n") &&
	        editor_test::write_bytes(root + "/strings/text_document.bin", strings) &&
	        editor_test::write_bytes(root + "/sounds/game.lwf", editor_test::sound_bank_of({"EXPLO_BASE"}));
	editor_test::handle_to_end(project.session, request::rescan());
	return written && project.view().findings.graph;
}

const GraphEdge *edge_at(const AssetGraph &graph, const std::string &file, const std::string &locator) {
	for (const GraphEdge *edge : graph.references_of(file))
		if (edge->locator == locator) return edge;
	return nullptr;
}

// The script's names resolve; a definition's "Referenced by" reaches its span; the open script stands
// in for its file; Rename everywhere rewrites the span and the compile still resolves it.
static int test_graph_and_rename() {
	ScriptProject project;
	TEST_EXPECT(make_script_project(project));
	const std::string script = "scripts/text_document.wac";
	const AssetGraph &graph = project.graph();
	// The graph reads a script's operands' names, its RUN and its waves (S14): an import follows the
	// whole of it.
	TEST_EXPECT(graph_reads_kind(AssetKind::Script) && !graph_reads_kind(AssetKind::Text) &&
	            !references_unread(AssetKind::Script));
	const std::vector<const GraphEdge *> edges = graph.references_of(script);
	TEST_EXPECT(edges.size() == 5);
	const GraphEdge *fx = edge_at(graph, script, "3:12");
	const GraphEdge *ammo = edge_at(graph, script, "5:16");
	const GraphEdge *fallback = edge_at(graph, script, "6:16");
	const GraphEdge *key = edge_at(graph, script, "7:15");
	TEST_EXPECT(fx && ammo && fallback && key);
	if (!fx || !ammo || !fallback || !key) return 1;
	TEST_EXPECT(fx->span.line == 3 && fx->span.length == 7 && fx->rewritable && fx->record.empty());
	TEST_EXPECT(graph.resolve(*fx) == ReferenceStatus::Present && graph.resolve(*ammo) == ReferenceStatus::Present &&
	            graph.resolve(*fallback) == ReferenceStatus::Present && graph.resolve(*key) == ReferenceStatus::Present);
	// The key reads the table of the script's name (the mission text), where this project defines it.
	TEST_EXPECT(key->scope == "TEXT_DOCUMENT.BIN" && key->rewritable && graph.symbol_reached(*key) &&
	            graph.symbol_reached(*key)->file == "strings/text_document.bin");
	// The fallback reaches ammo_satchel: its target, and the definition's users.
	const std::vector<const GraphSymbol *> satchel = graph.symbols_named(ReferenceKind::Ammo, "ammo_satchel");
	TEST_EXPECT(satchel.size() == 1 && graph.symbol_reached(*fallback) == satchel.front());
	const std::vector<const GraphEdge *> satchel_users = graph.users_of(*satchel.front());
	TEST_EXPECT(satchel_users.size() == 1 && satchel_users.front()->locator == "6:16");
	// "Referenced by" from the effect to the script's span, and where its Go to leads.
	const std::vector<const GraphSymbol *> buildup = graph.symbols_named(ReferenceKind::Particle, "Buildup");
	TEST_EXPECT(buildup.size() == 1);
	const std::vector<const GraphEdge *> users = graph.users_of(*buildup.front());
	TEST_EXPECT(users.size() == 1 && users.front()->source == script && users.front()->span.column == 12);
	const ReferenceTarget target = usage_target(*project.view().project.scan, *users.front());
	TEST_EXPECT(target.file == script && target.locator == "3:12" && target.editable);
	// On the wire: the edge's span, and the fallback of the second ammo.
	const JsonValue fx_json = graph_edge_to_json(graph, *fx);
	const JsonValue *fx_span = fx_json.get("span");
	TEST_EXPECT(fx_span && fx_span->get_number("line", 0) == 3 && fx_span->get_number("column", 0) == 12 &&
	            fx_span->get_number("length", 0) == 7 && fx_json.get_string("status", "") == "present");
	TEST_EXPECT(graph_edge_to_json(graph, *fallback).get_string("fallback", "") == "ammo_satchel");
	// The script's references have no missing one; the project's validation is clean of the script.
	for (const GraphEdge *missing : graph.missing()) TEST_EXPECT(missing->source != script);
	// The open script stands in for its file: a span renamed to a name nothing defines is a missing
	// reference at its line and column, its undo resolved again.
	editor_test::handle_to_end(project.session, request::open_document(script));
	TextDocument *open = text_of(*project.session.document_base_for(script));
	TEST_EXPECT(open != nullptr);
	if (!open) return 1;
	editor_test::handle_to_end(project.session, request::edit_record(script, {TextDocument::replace(span(3, 12, 7), "Nothing")}));
	TEST_EXPECT(project.session.last_edit_ok() && open->line(3) == "\tfxrain FX_Nothing");
	const auto missing_at = [&](size_t line, size_t column) {
		for (const Diagnostic &d : project.view().findings.diagnostics)
			if (d.code() == "reference.missing" && d.asset == script && d.line == line && d.column == column) return true;
		return false;
	};
	TEST_EXPECT(missing_at(3, 12));
	editor_test::handle_to_end(project.session, request::undo(script));
	TEST_EXPECT(open->line(3) == "\tfxrain FX_Buildup" && !missing_at(3, 12));
	// Rename everywhere: the ammo AT_CONTRACT, defined in ammo.def, used by the script's span.
	const std::vector<const GraphSymbol *> contract = project.graph().symbols_named(ReferenceKind::Ammo, "AT_CONTRACT");
	TEST_EXPECT(contract.size() == 1);
	if (contract.empty()) return 1;
	const GraphSymbol definition = *contract.front();
	const SymbolRenamePlan plan =
	        plan_symbol_rename_project(*project.view().project.scan, project.graph(), *contract.front(), "AT_RENAMED");
	TEST_EXPECT(plan.ok() && plan.sites.size() == 2 && plan.sites[1].file == script &&
	            plan.sites[1].span.line == 5 && plan.sites[1].field.empty());
	// The preview's text site: its span, the name it holds as UTF-8.
	editor_test::handle_to_end(project.session, request::preview_rename(definition.file, definition.locator, definition.field, "AT_RENAMED"));
	const JsonValue dialogs = view_section_to_json(project.view(), ViewSection::Dialogs);
	const JsonValue *preview = dialogs.get("rename_preview");
	const JsonValue *sites = preview ? preview->get("sites") : nullptr;
	TEST_EXPECT(sites && sites->array.size() == 2);
	if (!sites || sites->array.size() != 2) return 1;
	const JsonValue *site_span = sites->array[1].get("span");
	TEST_EXPECT(site_span && site_span->get_number("line", 0) == 5 && site_span->get_number("column", 0) == 16 &&
	            site_span->get_number("length", 0) == 11 && sites->array[1].get_string("before", "") == "AT_CONTRACT" &&
	            sites->array[1].get_string("after", "") == "AT_RENAMED" && !sites->array[0].get("span"));
	// A stale site: the open script changed at its place since the plan, the staging finds it no
	// more (rename.partial), nothing written.
	editor_test::handle_to_end(project.session, request::edit_record(script, {TextDocument::replace(span(5, 16, 11), "AT_ELSEWHERE")}));
	std::vector<Diagnostic> stale;
	TEST_EXPECT(!check_symbol_rename(ProjectPaths::for_root(project.root()), *project.view().project.document,
	                                 *project.view().project.scan, project.graph(), plan, project.view().documents.open,
	                                 stale) &&
	            has_code(stale, "rename.partial"));
	editor_test::handle_to_end(project.session, request::undo(script));
	TEST_EXPECT(open->line(5) == "\tammoarea AMMO_AT_CONTRACT 8");
	// A text key's use in the script of a mission's name the project does not have: the script runs with
	// whichever mission's table plays, so its key resolves in any table and no rename rewrites it (a
	// rename of the definition is refused at that use, as game.wac's is).
	const std::vector<const GraphSymbol *> start = project.graph().symbols_named(ReferenceKind::TextId, "MISSION_START");
	const GraphEdge *key_now = edge_at(project.graph(), script, "7:15");
	TEST_EXPECT(start.size() == 1 && key_now && key_now->scope_owner == "TEXT_DOCUMENT.BMS" &&
	            project.graph().lookup_scope(*key_now).empty() && !project.graph().rewrites(*key_now) &&
	            project.graph().resolve(*key_now) == ReferenceStatus::Present);
	if (start.empty()) return 1;
	const SymbolRenamePlan unowned =
	        plan_symbol_rename_project(*project.view().project.scan, project.graph(), *start.front(), "MISSION_GO");
	TEST_EXPECT(!unowned.ok() && has_code(unowned.refusals, "rename.site") && unowned.refusals.size() == 1 &&
	            unowned.refusals[0].asset == script && unowned.refusals[0].message.find("at 7:15") != std::string::npos);
	// With the mission in the project, the key reads its table (S14: the graph scopes the lookup as the
	// game makes it) and is rewritten with its definition: the plan reaches the script's span.
	{
		opennova::bms::File mission;
		opennova::mission::make_default(mission);
		std::vector<uint8_t> bytes;
		std::string error;
		TEST_EXPECT(opennova::bms::write(mission, bytes, error) &&
		            editor_test::write_bytes(project.root() + "/missions/text_document.bms", bytes));
		editor_test::handle_to_end(project.session, request::rescan());
	}
	const std::vector<const GraphSymbol *> owned = project.graph().symbols_named(ReferenceKind::TextId, "MISSION_START");
	key_now = edge_at(project.graph(), script, "7:15");
	TEST_EXPECT(owned.size() == 1 && key_now && project.graph().lookup_scope(*key_now) == "TEXT_DOCUMENT.BIN" &&
	            project.graph().rewrites(*key_now) && graph_edge_to_json(project.graph(), *key_now).get_bool("rewritable", false));
	if (owned.empty()) return 1;
	const SymbolRenamePlan keyed =
	        plan_symbol_rename_project(*project.view().project.scan, project.graph(), *owned.front(), "MISSION_GO");
	TEST_EXPECT(keyed.ok() && keyed.sites.size() == 2 && keyed.sites[1].file == script && keyed.sites[1].span.line == 7 &&
	            keyed.sites[1].span.column == 15 && keyed.sites[1].before == "MISSION_START" && keyed.sites[1].after == "MISSION_GO");
	// A rename that would take over a use reaching another ammo through its fallback: bomb renamed
	// satchel would catch the script's ammo_satchel (its lookup's first name), refused.
	const std::vector<const GraphSymbol *> bomb = project.graph().symbols_named(ReferenceKind::Ammo, "bomb");
	TEST_EXPECT(bomb.size() == 1);
	if (bomb.empty()) return 1;
	const SymbolRenamePlan captures =
	        plan_symbol_rename_project(*project.view().project.scan, project.graph(), *bomb.front(), "satchel");
	TEST_EXPECT(!captures.ok() && has_code(captures.refusals, "rename.exists") &&
	            captures.refusals[0].message.find("ammo_satchel") != std::string::npos);
	TEST_EXPECT(editor_test::handle_to_end(project.session, request::rename_symbol(definition.file, definition.locator, definition.field, "AT_RENAMED")).done());
	const std::string written = test_io::read_file_text(project.root() + "/" + script);
	TEST_EXPECT(written.find("\tammoarea AMMO_AT_RENAMED 8\r\n") != std::string::npos &&
	            written.find("AT_CONTRACT") == std::string::npos);
	// The compile still resolves the renamed name: the edge at its span present.
	const GraphEdge *renamed = edge_at(project.graph(), script, "5:16");
	TEST_EXPECT(renamed && renamed->value == "AT_RENAMED" && project.graph().resolve(*renamed) == ReferenceStatus::Present);
	// The fallback's definition renamed: the span takes the new name, which its lookup reaches.
	const std::vector<const GraphSymbol *> satchel_now = project.graph().symbols_named(ReferenceKind::Ammo, "ammo_satchel");
	TEST_EXPECT(satchel_now.size() == 1);
	if (satchel_now.empty()) return 1;
	const GraphSymbol satchel_definition = *satchel_now.front();
	TEST_EXPECT(editor_test::handle_to_end(project.session, request::rename_symbol(satchel_definition.file, satchel_definition.locator,
	                                              satchel_definition.field, "ammo_charge")).done());
	// The prefix kept once: the span reads "charge", its lookup's second name the renamed ammo.
	TEST_EXPECT(test_io::read_file_text(project.root() + "/" + script).find("\tammo2tgt(ammo_charge, 3)\r\n") !=
	            std::string::npos);
	const GraphEdge *charged = edge_at(project.graph(), script, "6:16");
	TEST_EXPECT(charged && charged->value == "charge" && charged->fallback == "ammo_charge" &&
	            project.graph().resolve(*charged) == ReferenceStatus::Present && project.graph().symbol_reached(*charged) &&
	            project.graph().symbol_reached(*charged)->name == graph_names::symbol_name(ReferenceKind::Ammo, "ammo_charge"));
	// Where the rest names another definition (bomb), the whole new name, which its first name finds.
	const std::vector<const GraphSymbol *> charge_now = project.graph().symbols_named(ReferenceKind::Ammo, "ammo_charge");
	TEST_EXPECT(charge_now.size() == 1);
	if (charge_now.empty()) return 1;
	const GraphSymbol charge_definition = *charge_now.front();
	TEST_EXPECT(editor_test::handle_to_end(project.session, request::rename_symbol(charge_definition.file, charge_definition.locator,
	                                              charge_definition.field, "ammo_bomb")).done());
	TEST_EXPECT(test_io::read_file_text(project.root() + "/" + script).find("\tammo2tgt(ammo_ammo_bomb, 3)\r\n") !=
	            std::string::npos);
	const GraphEdge *whole = edge_at(project.graph(), script, "6:16");
	TEST_EXPECT(whole && whole->value == "ammo_bomb" && project.graph().symbol_reached(*whole) &&
	            project.graph().symbol_reached(*whole)->name == graph_names::symbol_name(ReferenceKind::Ammo, "ammo_bomb"));
	// A name the script's text would not read whole is refused, nothing written.
	const std::vector<const GraphSymbol *> renamed_now = project.graph().symbols_named(ReferenceKind::Ammo, "AT_RENAMED");
	TEST_EXPECT(renamed_now.size() == 1);
	if (renamed_now.empty()) return 1;
	std::vector<Diagnostic> refusals;
	const SymbolRenamePlan split =
	        plan_symbol_rename_project(*project.view().project.scan, project.graph(), *renamed_now.front(), "TWO WORDS");
	TEST_EXPECT(split.ok() && !check_symbol_rename(ProjectPaths::for_root(project.root()), *project.view().project.document,
	                                               *project.view().project.scan, project.graph(), split, {}, refusals));
	TEST_EXPECT(has_code(refusals, "rename.name"));
	return 0;
}

// A Go to opens the script at a span (a RevealText event); a compile report is a Problems row at
// its line and column, whose place opens the script there.
static int test_go_to_and_problems() {
	ScriptProject project;
	TEST_EXPECT(make_script_project(project));
	const std::string script = "scripts/text_document.wac";
	const uint64_t before = project.view().events.next_seq();
	editor_test::handle_to_end(project.session, request::open_document(script, "5:16"));
	const std::vector<ViewEvent> reveals = editor_test::events_after(project.view(), before - 1, ViewEventKind::RevealText);
	TEST_EXPECT(reveals.size() == 1 && reveals.front().path == script && reveals.front().locator == "5:16");
	TEST_EXPECT(project.view().documents.active == script && project.view().documents.selection.records.empty());
	// A compile report as the open script now holds it.
	editor_test::handle_to_end(project.session, request::edit_record(script, {TextDocument::replace(span(3, 19, 0), " )")}));
	const Diagnostic *report = nullptr;
	for (const Diagnostic &d : project.view().findings.diagnostics)
		if (d.code() == "script.compile" && d.asset == script) report = &d;
	TEST_EXPECT(report && report->line == 3 && report->column == 20);
	if (!report) return 1;
	const ProblemLocation location = problem_location(*report, project.view());
	TEST_EXPECT(location.locator == "3:20" && !location.in_files);
	const EditorRequest go = location.request();
	TEST_EXPECT(go.kind == EditorRequestKind::OpenDocument && go.locator == "3:20" && go.path == script);
	const JsonValue finding = diagnostic_to_json(*report);
	TEST_EXPECT(finding.get_number("line", 0) == 3 && finding.get_number("column", 0) == 20);
	return 0;
}

// The wire: an edit_record's spans in the batch form (apply, its payload "text.span"), read back as
// written; its refusals by the edit's place; the document query's lines a page at a time.
static int test_wire() {
	ScriptProject project;
	TEST_EXPECT(make_script_project(project));
	const std::string script = "scripts/text_document.wac";
	editor_test::handle_to_end(project.session, request::open_document(script));
	std::string error;
	const auto send = [&](const std::string &text) {
		JsonValue json;
		opennova::io::json_parse(text, json, error);
		const JsonValue answer = project.session.handle_json(json);
		project.session.run_operations();
		return answer;
	};
	JsonValue answer = send(R"({"kind": "edit_record", "path": ")" + script +
	                        R"(", "edits": [{"op": "apply", "payload": "text.span", "line": 3, "column": 12, "length": 7, "text": "Spark"}, {"op": "apply", "payload": "text.span", "line": 1, "column": 1, "length": 0, "text": "; café\r\n"}]})");
	const TextDocument *open = text_of(*project.session.document_base_for(script));
	TEST_EXPECT(answer.get_bool("ok", false) && open && open->line(4) == "\tfxrain FX_Spark");
	TEST_EXPECT(open && open->line(1) == std::string("; caf\xE9")); // stored in the code page
	TEST_EXPECT(open && open->revision() == 1);
	// Refused as it is read, by its place, nothing applied.
	const auto refused = [&](const std::string &edit, const char *says) {
		const JsonValue out = send(R"({"kind": "edit_record", "path": ")" + script + R"(", "edits": [)" + edit + "]}");
		const std::string why = out.get_string("error", "");
		if (out.get_bool("ok", true) || why.find(says) == std::string::npos) {
			std::fprintf(stderr, "refusal: %s\n", why.c_str());
			return false;
		}
		return open->revision() == 1;
	};
	TEST_EXPECT(refused(R"({"op": "set", "id": 1, "field": "x", "value": 1})", "edits[0]: a text document takes \"apply\""));
	TEST_EXPECT(refused(R"({"op": "apply", "payload": "blob.replace", "line": 1, "column": 1})", "\"text.span\""));
	TEST_EXPECT(refused(R"({"op": "apply", "payload": "text.span", "column": 1})", "\"line\""));
	TEST_EXPECT(refused(R"({"op": "apply", "payload": "text.span", "line": 1, "column": 1, "text": "✓"})",
	                    "Windows-1252"));
	TEST_EXPECT(refused(R"({"op": "apply", "payload": "text.span", "line": 1, "column": 1, "colour": 1})",
	                    "Unknown edits[0] member"));
	// A span outside the text reads, and the document refuses it.
	answer = send(R"({"kind": "edit_record", "path": ")" + script +
	              R"(", "edits": [{"op": "apply", "payload": "text.span", "line": 99, "column": 1}]})");
	TEST_EXPECT(answer.get_bool("ok", false) && !project.session.last_edit_ok() && open->revision() == 1);
	// A request's spans written and read back as they were.
	EditorRequest request = request::edit_record(script, {TextDocument::replace(span(2, 3, 4), "na\xEFve", true, 7)});
	const JsonValue written = editor_request_to_json(request);
	EditorRequest read;
	RequestNames names;
	names.text = true;
	TEST_EXPECT(editor_request_from_json(written, read, error, &names));
	const auto *payload = read.edits.size() == 1 ? dynamic_cast<const TextSpanEdit *>(read.edits[0].payload.get()) : nullptr;
	TEST_EXPECT(payload && payload->span.line == 2 && payload->span.column == 3 && payload->span.length == 4 &&
	            payload->text == "na\xEFve" && read.edits[0].coalesce && read.edits[0].gesture == 7);
	// A closed text file's edits read by its path's type (open_first).
	JsonValue closed;
	opennova::io::json_parse(R"({"kind": "edit_record", "path": "game.cfg", "open_first": true, "edits": [{"op": "apply", "payload": "text.span", "line": 1, "column": 1}]})",
	                         closed, error);
	EditorRequest closed_read;
	TEST_EXPECT(editor_request_from_json(closed, closed_read, error) && closed_read.edits.size() == 1);
	// A closed music script, a .bin its name alone does not type: its spans read by the kind the scan
	// read (its content), the document open after its first read.
	TEST_EXPECT(editor_test::write_text(project.root() + "/gamemus.bin", repo_file("mus/synth_gamemus.bin")));
	editor_test::handle_to_end(project.session, request::rescan());
	answer = send(R"({"kind": "edit_record", "path": "gamemus.bin", "open_first": true, "edits": [{"op": "apply", "payload": "text.span", "line": 6, "column": 1, "text": "// note\n"}]})");
	const DocumentBase *music = project.session.document_base_for("gamemus.bin");
	TEST_EXPECT(answer.get_bool("ok", false) && music && text_of(*music) && text_of(*music)->line(6) == "// note");
	if (!answer.get_bool("ok", false)) std::fprintf(stderr, "music: %s\n", answer.get_string("error", "").c_str());
	// The document query: its lifecycle, its line count and a page of its lines.
	JsonValue args = JsonValue::make_object();
	args.set("path", opennova::io::json_string(script));
	args.set("offset", opennova::io::json_number(2));
	args.set("limit", opennova::io::json_number(3));
	const JsonValue document = project.session.query("document", args, error);
	TEST_EXPECT(document.get_number("line_count", 0) == double(open->line_count()) &&
	            document.get_number("count", 0) == double(open->line_count()));
	const JsonValue *lines = document.get("lines");
	TEST_EXPECT(lines && lines->array.size() == 3 && lines->array[0].get_number("line", 0) == 3 &&
	            lines->array[1].get_string("text", "") == "\tfxrain FX_Spark");
	TEST_EXPECT(!document.get("rows"));
	return 0;
}

// The credits type: a CBIN file through its text form, byte for byte; an edit written back in the
// form; one the text form cannot carry (the minted synth_nlist.kda, whose lines hold spaces the
// text form would read as separators) held read only; a text file its own text.
static int test_credits() {
	const std::vector<uint8_t> minted = minted_credits();
	const std::string kda(minted.begin(), minted.end());
	TEST_EXPECT(!kda.empty());
	const DocumentType *type = document_type_for(AssetKind::Credits);
	TEST_EXPECT(type && type->id == DocumentTypeId::Credits);
	std::unique_ptr<DocumentBase> made = type->make();
	Diagnostic error;
	TEST_EXPECT(made->load_bytes(bytes_of(kda), "nlist.kda", AssetKind::Credits, "jo", error) && !made->blocked());
	const TextDocument &credits = *text_of(*made);
	std::printf("credits: %zu bytes as %zu lines of text; first lines \"%s\", \"%s\"\n", kda.size(),
	            credits.line_count(), std::string(credits.line(1)).c_str(), std::string(credits.line(2)).c_str());
	TEST_EXPECT(credits.line(1) == "[ENV]" && credits.line(2) == "scroll_rate = 0.5" &&
	            credits.line(8) == "text = Joint_Operations:, Serpen24" && made->serialize().text == kda &&
	            made->rewrite_need() == DocumentBase::RewriteNeed::None && type->validate_file(*made).empty());
	// A text line's value changed: written in the CBIN form, which reads back as the edited text.
	size_t text_line = 0;
	for (size_t i = 1; i <= credits.line_count() && !text_line; ++i)
		if (opennova::strutil::starts_with_icase(std::string(credits.line(i)), "text =")) text_line = i;
	TEST_EXPECT(text_line > 0);
	const std::string line = std::string(credits.line(text_line));
	TEST_EXPECT(apply(*made, {TextDocument::replace(span(text_line, 1, line.size()), "text = Edited_by_D9, font")}));
	const SerializeResult written = made->serialize();
	TEST_EXPECT(written.ok() && written.text != kda && opennova::cbin::is_cbin(
	                                                          reinterpret_cast<const uint8_t *>(written.text.data()),
	                                                          written.text.size()));
	std::unique_ptr<DocumentBase> again = type->make();
	TEST_EXPECT(again->load_bytes(bytes_of(written.text), "nlist.kda", AssetKind::Credits, "jo", error) &&
	            !again->blocked() && text_of(*again)->text() == credits.text());
	// An entry of three values does not go in the form: a finding, its Save refused.
	TEST_EXPECT(apply(*made, {TextDocument::replace(span(text_line, 1, 0), "text = a, b, c\r\n")}));
	TEST_EXPECT(!made->serialize().ok() && has_code(type->validate_file(*made), "credits.unserializable"));
	// A CBIN value its text form would read as two: held read only.
	std::unique_ptr<DocumentBase> spaced = type->make();
	TEST_EXPECT(spaced->load_bytes(bytes_of(repo_file("cbin/synth_nlist.kda")), "spaced.kda", AssetKind::Credits,
	                               "jo", error) &&
	            spaced->blocked() && has_code(type->validate_file(*spaced), "credits.invalid_input"));
	TEST_EXPECT(!spaced->apply(TextDocument::replace(span(1, 1, 0), "x"), error) && error.code() == "document.parse");
	// A text file is its own text.
	std::unique_ptr<DocumentBase> plain = type->make();
	TEST_EXPECT(plain->load_bytes(bytes_of("[ENV]\r\nscroll_rate = 0.5\r\n"), "plain.kda", AssetKind::Credits, "jo",
	                              error) &&
	            plain->serialize().text == "[ENV]\r\nscroll_rate = 0.5\r\n");
	return 0;
}

// The CBIN form keeps what the reader reads and nothing else: a line it reads none of or only part of
// is refused at its line (never saved short); an LF alone put into a line is written CR LF, the two
// lines both kept.
static int test_credits_unread() {
	const std::vector<uint8_t> minted = minted_credits();
	const DocumentType *type = document_type_for(AssetKind::Credits);
	Diagnostic error;
	const auto loaded = [&] {
		std::unique_ptr<DocumentBase> made = type->make();
		made->load_bytes(minted, "nlist.kda", AssetKind::Credits, "jo", error);
		return made;
	};
	size_t text_line = 0;
	{
		const std::unique_ptr<DocumentBase> probe = loaded();
		const TextDocument &credits = *text_of(*probe);
		for (size_t i = 1; i <= credits.line_count() && !text_line; ++i)
			if (opennova::strutil::starts_with_icase(std::string(credits.line(i)), "text =")) text_line = i;
	}
	TEST_EXPECT(text_line > 0);
	// An LF alone put into an existing line: two lines, both written.
	std::unique_ptr<DocumentBase> extra = loaded();
	TEST_EXPECT(apply(*extra, {TextDocument::replace(span(text_line, 1, 0), "text = Extra\n")}));
	const std::vector<Diagnostic> ended = type->validate_file(*extra);
	TEST_EXPECT(has_code(line_end_findings(*extra, "jo"), "document.line_ends") &&
	            !has_code(ended, "credits.unserializable"));
	const SerializeResult written = extra->serialize();
	std::unique_ptr<DocumentBase> back = type->make();
	TEST_EXPECT(written.ok() && back->load_bytes(bytes_of(written.text), "nlist.kda", AssetKind::Credits, "jo", error));
	TEST_EXPECT(text_of(*back)->line(text_line) == "text = Extra" &&
	            text_of(*back)->line(text_line + 1) == text_of(*loaded())->line(text_line));
	// What the reader does not read whole: refused at its line, nothing written.
	const auto refused_at = [&](const std::string &inserted, size_t line, const char *says) {
		std::unique_ptr<DocumentBase> made = loaded();
		if (!apply(*made, {TextDocument::replace(span(line, 1, 0), inserted)})) return false;
		const std::vector<Diagnostic> found = type->validate_file(*made);
		for (const Diagnostic &d : found)
			if (d.code() == "credits.unserializable" && d.line == line && d.message.find(says) != std::string::npos)
				return !made->serialize().ok() && made->rewrite_need() == DocumentBase::RewriteNeed::Unserializable;
		for (const Diagnostic &d : found) std::fprintf(stderr, "credits: %s at %zu\n", d.message.c_str(), d.line);
		return false;
	};
	TEST_EXPECT(refused_at("; a comment\r\n", text_line, "comment"));
	TEST_EXPECT(refused_at("text = kept ; and a comment\r\n", text_line, "';' comment"));
	TEST_EXPECT(refused_at("[text]\r\n", text_line, "opens no section"));
	TEST_EXPECT(refused_at("before = 1\r\n", 1, "outside any section"));
	TEST_EXPECT(refused_at("text =\r\n", text_line, "no value"));
	TEST_EXPECT(refused_at("no equals here\r\n", text_line, "no '='"));
	TEST_EXPECT(refused_at("text = a, b, c\r\n", text_line, "3 values"));
	TEST_EXPECT(refused_at("text = a\rb\r\n", text_line, "CR alone"));
	std::vector<SourceIssue> issues;
	TEST_EXPECT(!credits_text_readable("[ENV]\r\nrate = 1\nmore = 2\r\n", issues) && issues.size() == 1 &&
	            issues[0].line == 2 && issues[0].message.find("LF alone") != std::string::npos);
	return 0;
}

// The music script type: its MUS text byte for byte (the minted script's MessageHandler its `handler`); a
// file of two scripts held read only; a text that does not compile refused at its line and column.
static int test_music_script() {
	const std::string bin = repo_file("mus/synth_gamemus.bin");
	TEST_EXPECT(!bin.empty());
	const DocumentType *type = document_type_for(AssetKind::MusicScript);
	TEST_EXPECT(type && type->id == DocumentTypeId::MusicScript);
	std::unique_ptr<DocumentBase> made = type->make();
	Diagnostic error;
	TEST_EXPECT(made->load_bytes(bytes_of(bin), "gamemus.bin", AssetKind::MusicScript, "jo", error) && !made->blocked());
	const TextDocument &music = *text_of(*made);
	TEST_EXPECT(music.text() == repo_file("mus/golden_synth_gamemus.mus.txt"));
	TEST_EXPECT(made->serialize().text == bin && type->validate_file(*made).empty());
	// A text that does not compile: refused at its line and column, its Save refused.
	TEST_EXPECT(apply(*made, {TextDocument::replace(span(7, 1, 0), "@@@ ")}));
	const std::vector<Diagnostic> findings = type->validate_file(*made);
	// Where the compiler stops: after the token it refused (its own column).
	TEST_EXPECT(findings.size() == 1 && findings[0].code() == "music_script.unserializable" && findings[0].line == 7 &&
	            findings[0].column == 2 && findings[0].severity == DiagnosticSeverity::Error);
	TEST_EXPECT(!made->serialize().ok() && made->rewrite_need() == DocumentBase::RewriteNeed::Unserializable);
	made->undo();
	TEST_EXPECT(made->serialize().text == bin);
	TEST_EXPECT(music.text().find("handler MessageHandler(msgtype, source)\n") != std::string::npos);
	// Two scripts in one file: read only, a finding.
	std::unique_ptr<DocumentBase> handled = type->make();
	TEST_EXPECT(handled->load_bytes(script_in_two_chunks(), "gamemus.bin", AssetKind::MusicScript, "jo", error) &&
	            handled->blocked());
	const std::vector<Diagnostic> held = type->validate_file(*handled);
	TEST_EXPECT(has_code(held, "music_script.invalid_input") && held[0].severity == DiagnosticSeverity::Warning &&
	            held[0].message.find("holds 2 scripts") != std::string::npos);
	TEST_EXPECT(handled->rewrite_need() == DocumentBase::RewriteNeed::Unserializable);
	return 0;
}

// The shader type: the shader loader's SCR form byte for byte; a plain shader a finding, which Save
// fixes by writing the form; one in the form of another version does not load; the text type's
// file its own text.
static int test_shader_and_text() {
	const DocumentType *type = document_type_for(AssetKind::Shader);
	TEST_EXPECT(type && type->id == DocumentTypeId::Shader);
	const std::string source = "// a shader\r\nfloat4 main() : COLOR { return 0; }\r\n";
	const std::vector<uint8_t> stored = scr_shader(source);
	std::unique_ptr<DocumentBase> made = type->make();
	Diagnostic error;
	TEST_EXPECT(made->load_bytes(stored, "glass.fx", AssetKind::Shader, "jo", error) && !made->blocked());
	TEST_EXPECT(text_of(*made)->text() == source && made->serialize().text == std::string(stored.begin(), stored.end()) &&
	            type->validate_file(*made).empty());
	std::unique_ptr<DocumentBase> plain = type->make();
	TEST_EXPECT(plain->load_bytes(bytes_of(source), "plain.fx", AssetKind::Shader, "jo", error));
	const std::vector<Diagnostic> findings = type->validate_file(*plain);
	TEST_EXPECT(findings.size() == 1 && findings[0].code() == "shader.form" && findings[0].row() &&
	            findings[0].row()->fixes == FindingFix::Rewrite);
	TEST_EXPECT(plain->rewrite_need() == DocumentBase::RewriteNeed::Rewrite &&
	            plain->serialize().text == std::string(stored.begin(), stored.end()));
	std::unique_ptr<DocumentBase> other = type->make();
	TEST_EXPECT(!other->load_bytes(bytes_of(std::string("SCR\x02", 4) + "xyz"), "two.fx", AssetKind::Shader, "jo", error) &&
	            error.code() == "document.parse");
	// The plain form is a fact of the stored file, no line Save drops; its Rewrite writes the form and
	// the finding goes with the file it was about.
	TEST_EXPECT(plain->ignored_lines() == 0 && plain->issues().empty() && !plain->blocked());
	editor_test::TempProjectDir dir("opennova_editor_text_shader");
	const std::string file = dir.file("plain.fx");
	TEST_EXPECT(editor_test::write_text(file, source));
	std::unique_ptr<DocumentBase> rewritten = type->make();
	TEST_EXPECT(rewritten->load(file, "plain.fx", AssetKind::Shader, "jo", error) && type->validate_file(*rewritten).size() == 1);
	TEST_EXPECT(rewritten->save(error) && type->validate_file(*rewritten).empty() &&
	            test_io::read_file(file) == stored && !rewritten->dirty());
	// The text type: its file its text, no finding for a configuration (no reader the editor models); its
	// table the engine readers' two codes (DI-06).
	const DocumentType *text = document_type_for(AssetKind::Config);
	TEST_EXPECT(text && text->id == DocumentTypeId::Text && document_type_for(AssetKind::Text) == text);
	std::unique_ptr<DocumentBase> config = text->make();
	TEST_EXPECT(config->load_bytes(bytes_of("a = 1\nb = 2\n"), "game.cfg", AssetKind::Config, "jo", error) &&
	            config->serialize().text == "a = 1\nb = 2\n" && text->validate_file(*config).empty() &&
	            text->findings().count == 2);
	return 0;
}

// Every text file opens in the editor (the deep-integration plan's DI-06): the text kinds no structured
// type edits are the text type's, a text with undo and save; where the engine has a reader of the kind,
// its findings are the file's (the avatar reader's notes at their lines; the score table's reader),
// listed; a kind with none makes none. SndProf.def, the particle file (DI-14), the environment (DI-19a),
// the HUD layout (DI-20) and the character attributes (DI-09's charattr follow-up) are left to their own
// types (the specific type owns its kind), the mission text to no type (the build leaves it out). An open
// avatar table stands in for its file in the graph, read by the engine's own reader: its names follow its
// edits.
static int test_text_readers() {
	const DocumentType *text = document_type(DocumentTypeId::Text);
	TEST_EXPECT(text != nullptr);
	if (!text) return 1;
	for (const AssetKind kind : {AssetKind::AiProfile,
	                             AssetKind::HudFxDefs, AssetKind::AvatarDefs, AssetKind::OtherDefs,
	                             AssetKind::Score, AssetKind::NovaWorldScreen})
		TEST_EXPECT(document_type_for(kind) == text && is_editable_kind(kind));
	TEST_EXPECT(document_type_for(AssetKind::SoundProfileDefs) != text && document_type_for(AssetKind::MissionText) == nullptr &&
	            document_type_for(AssetKind::Particles) == document_type(DocumentTypeId::Particles) &&
	            document_type_for(AssetKind::HudPosDefs) == document_type(DocumentTypeId::HudLayout) &&
	            document_type_for(AssetKind::CharAttrDefs) == document_type(DocumentTypeId::CharAttrs));
	TEST_EXPECT(document_type_for(AssetKind::Environment) == document_type(DocumentTypeId::Environment));
	// The graph still reads a native kind through the engine's reader, not the text type; the character
	// attributes through their own type's references.
	TEST_EXPECT(graph_reads_kind(AssetKind::Particles) &&
	            graph_reads_kind(AssetKind::HudPosDefs) && graph_reads_kind(AssetKind::AvatarDefs) &&
	            graph_reads_kind(AssetKind::CharAttrDefs) && !graph_reads_kind(AssetKind::Score));
	Diagnostic error;
	bool texts_held = true; // each file loads as its text and writes back as it was
	const auto findings_of = [&](const std::string &source, const char *name, AssetKind kind) {
		std::unique_ptr<DocumentBase> document = text->make();
		const bool loaded = document->load_bytes(bytes_of(source), name, kind, "jo", error);
		texts_held = texts_held && loaded && document->serialize().text == source;
		return loaded ? text->validate_file(*document) : std::vector<Diagnostic>();
	};
	// The avatar reader's notes, each at its line.
	const std::vector<Diagnostic> avatars =
			findings_of("nationality N00 FIRST\r\n{\r\n}\r\nnationality N00 DUP_NAT\r\n{\r\n}\r\n", "Avatars.def", AssetKind::AvatarDefs);
	TEST_EXPECT(avatars.size() == 1 && avatars[0].code() == "text.reader" && avatars[0].severity == DiagnosticSeverity::Warning &&
	            avatars[0].line == 4 && avatars[0].message.find("Duplicate nationality") != std::string::npos);
	// The score table's reader reads past a statement it does not know, as the game does (master's #987,
	// formats/score): no finding.
	const std::vector<Diagnostic> scores = findings_of("VERSION 1\nNONSENSE 2\n", "score.ini", AssetKind::Score);
	TEST_EXPECT(scores.empty());
	// Kinds whose reader refuses nothing, or that the editor models no reader of.
	TEST_EXPECT(findings_of("primary_ammo 5\n", "tank.aip", AssetKind::AiProfile).empty());
	TEST_EXPECT(texts_held);

	// In a session: the avatar table opens as a text; an edit of a head's model moves the graph's reference
	// with it, unsaved; the table's note is the text type's, at its line.
	editor_test::TempProjectDir dir("opennova_editor_text_readers");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Readers"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	TEST_EXPECT(editor_test::write_text(root + "/Avatars.def",
	                                    "nationality N00 FIRST\r\n{\r\n}\r\nnationality N00 DUP_NAT\r\n{\r\n}\r\n\r\n"
	                                    "define head HEAD_A\r\n{\r\n\tgraphic\t\thead_a.3di\r\n\tcamo\t\t0 0 0\r\n"
	                                    "\tvoice\t\t1\r\n\tsex\t\tm\r\n}\r\n"));
	editor_test::handle_to_end(session, request::rescan());
	const auto names = [&session](const std::string &value) {
		for (const GraphEdge *edge : session.view().findings.graph->references_of("Avatars.def"))
			if (edge->kind == ReferenceKind::Model && edge->value == value) return true;
		return false;
	};
	TEST_EXPECT(names("head_a.3di"));
	size_t noted = 0;
	for (const Diagnostic &d : session.view().findings.diagnostics)
		noted += d.code() == "text.reader" && d.asset == "Avatars.def" && d.severity == DiagnosticSeverity::Warning && d.line == 4;
	TEST_EXPECT(noted == 1);
	editor_test::handle_to_end(session, request::open_document("Avatars.def"));
	TextDocument *open = text_of(*session.document_base_for("Avatars.def"));
	TEST_EXPECT(open != nullptr);
	if (!open) return 1;
	TEST_EXPECT(open->line(10) == "\tgraphic\t\thead_a.3di");
	editor_test::handle_to_end(session, request::edit_record("Avatars.def", {TextDocument::replace(span(10, 11, 6), "head_b")}));
	TEST_EXPECT(session.last_edit_ok() && open->line(10) == "\tgraphic\t\thead_b.3di" && open->dirty());
	TEST_EXPECT(names("head_b.3di") && !names("head_a.3di"));
	editor_test::handle_to_end(session, request::undo("Avatars.def"));
	TEST_EXPECT(names("head_a.3di") && !open->dirty());
	return 0;
}

// The project's notes open as texts (asset_kinds' Notes row): a README in Markdown and a licence with no
// extension, in a session, each its text with no finding; Problems says nothing of them, where a file of no
// kind beside them is said; an edit and a save write the note.
static int test_project_notes() {
	const DocumentType *text = document_type(DocumentTypeId::Text);
	TEST_EXPECT(text && document_type_for(AssetKind::Notes) == text && is_editable_kind(AssetKind::Notes));
	editor_test::TempProjectDir dir("opennova_editor_text_notes");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Notes"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	const std::string readme = "# Notes\r\n\r\nWhat this project holds.\r\n";
	TEST_EXPECT(editor_test::write_text(root + "/README.md", readme));
	TEST_EXPECT(editor_test::write_text(root + "/LICENSE", "MIT License\n"));
	TEST_EXPECT(editor_test::write_bytes(root + "/blob", {0x00, 0x01, 0x02}));
	editor_test::handle_to_end(session, request::rescan());
	size_t unknown = 0, about_notes = 0;
	for (const Diagnostic &d : session.view().findings.diagnostics) {
		unknown += d.code() == "asset.kind.unknown" && d.asset == "blob";
		about_notes += d.asset == "README.md" || d.asset == "LICENSE";
	}
	TEST_EXPECT(unknown == 1 && about_notes == 0);
	for (const char *name : {"README.md", "LICENSE"}) {
		const AssetEntry *entry = session.view().project.scan->find(name);
		TEST_EXPECT(entry && entry->kind == AssetKind::Notes);
	}
	editor_test::handle_to_end(session, request::open_document("README.md"));
	TextDocument *open = text_of(*session.document_base_for("README.md"));
	TEST_EXPECT(open != nullptr);
	if (!open) return 1;
	TEST_EXPECT(open->text() == readme && text->validate_file(*open).empty());
	editor_test::handle_to_end(session, request::open_document("LICENSE"));
	TEST_EXPECT(text_of(*session.document_base_for("LICENSE")) != nullptr);
	editor_test::handle_to_end(session, request::edit_record("README.md", {TextDocument::replace(span(3, 1, 4), "Each")}));
	TEST_EXPECT(session.last_edit_ok() && open->dirty());
	editor_test::handle_to_end(session, request::save("README.md"));
	const std::vector<uint8_t> saved = test_io::read_file(root + "/README.md");
	TEST_EXPECT(!open->dirty() &&
	            std::string(saved.begin(), saved.end()).find("Each this project holds.") != std::string::npos);
	return 0;
}

// The tags a shader registers, which a model material's shader names (ReferenceKind::Shader): _ffp.fx the
// renderer's twelve fixed-function tags and their #UV twins whatever its EffectTag says [orig:
// HLSLEffect_InitFixedFunctionShaders @ 0x5AF790]; another effect its EffectTag and, where EffectAlt_UV
// asks, the twin; an include the archive walk skips none [orig: HLSLEffect_LoadAllFromPFFArchive @
// 0x5AFF6E]. The annotations are read with the comments left out. The editor's own shaders: _ffp.fx by
// its role and an effect per tag it makes, in the loader's form, each defining what it says, sharing the
// eight system textures the renderer binds once a frame and never the dead TexAngleMap slot.
static std::vector<std::string> shader_tags_of(const std::vector<uint8_t> &bytes, const char *name) {
	std::vector<std::string> out;
	const DocumentType *type = document_type_for(AssetKind::Shader);
	std::unique_ptr<DocumentBase> document = type->make();
	Diagnostic error;
	if (!type->definitions || !document->load_bytes(bytes, name, AssetKind::Shader, "jo", error)) return out;
	std::vector<TextDefinition> definitions;
	type->definitions(*text_of(*document), definitions);
	for (const TextDefinition &definition : definitions)
		if (definition.kind == ReferenceKind::Shader) out.push_back(definition.name);
	return out;
}

static int test_shader_definitions() {
	using Names = std::vector<std::string>;
	const std::string source =
			"// string EffectTag = \"COMMENTED\";\r\n/* string EffectInfo < string EffectTag = \"BLOCK\"; > */\r\n"
			"string EffectInfo <\r\n\tstring EffectName = \"x > y\";\r\n\tstring EffectTag = \"VS_TEST\";\r\n"
			"\tbool EffectAlt_UV = true;\r\n>;\r\n";
	// The renderer's reading of the EffectInfo is runtime/renderer's (tests/renderer/shader_effect_info_test).
	TEST_EXPECT(shader_tags_of(scr_shader(source), "shaders/test.fx") == Names({"VS_TEST", "VS_TEST#UV"}));
	// The definition sits on the tag as written.
	const DocumentType *type = document_type_for(AssetKind::Shader);
	std::unique_ptr<DocumentBase> document = type->make();
	Diagnostic error;
	TEST_EXPECT(document->load_bytes(scr_shader(source), "test.fx", AssetKind::Shader, "jo", error));
	std::vector<TextDefinition> definitions;
	type->definitions(*text_of(*document), definitions);
	TEST_EXPECT(definitions.size() == 2 && definitions[0].span.line == 5 && definitions[0].span.length == 7);
	// Without the twin; an include; no EffectInfo at all.
	TEST_EXPECT(shader_tags_of(scr_shader("string EffectInfo < string EffectTag = \"VS_ONE\"; bool EffectAlt_UV = false; >;"),
	                           "one.fx") == Names({"VS_ONE"}));
	TEST_EXPECT(shader_tags_of(scr_shader(source), "_vsinc.fx").empty());
	TEST_EXPECT(shader_tags_of(scr_shader("float4 main() : COLOR { return 0; }"), "none.fx").empty());
	// _ffp.fx: the fixed-function tags, whatever its own tag says.
	const Names ff = shader_tags_of(scr_shader(source), "shaders/_FFP.FX");
	const auto has = [](const Names &names, const char *name) {
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	TEST_EXPECT(ff.size() == 24 && has(ff, "FF_ST_OP") &&
	            has(ff, "FF_MT_AD_LUM#UV") && has(ff, "FF_ST_AB_LUM") && !has(ff, "VS_TEST"));
	// The editor's own.
	std::vector<uint8_t> bytes;
	BlankRequest ffp;
	ffp.logical_name = "_ffp.fx";
	ffp.role = "ffp_shader";
	TEST_EXPECT(make_blank(ffp, AssetKind::Shader, bytes, error) && bytes.size() > 4 &&
	            std::string(bytes.begin(), bytes.begin() + 4) == std::string("SCR\x01", 4));
	TEST_EXPECT(shader_tags_of(bytes, "_ffp.fx").size() == 24);
	const char *shared[] = {"TexCubeNormalize", "TexCubeEnvironment", "TexCubeRotSpecular", "TexPhongMap",
	                        "TexClip1D",        "TexSpot2D",          "TexDepthGradWrite",  "TexDepthGradTest"};
	const auto crlf = [](const std::string &text) {
		std::string out;
		for (char c : text) out += c == '\n' ? std::string("\r\n") : std::string(1, c);
		return out;
	};
	const auto check_text = [&](const std::vector<uint8_t> &made, const std::string &tag, const char *name) {
		std::unique_ptr<DocumentBase> shader = type->make();
		Diagnostic load_error;
		TEST_EXPECT(shader->load_bytes(made, name, AssetKind::Shader, "jo", load_error) &&
		            type->validate_file(*shader).empty());
		const std::string text = text_of(*shader) ? text_of(*shader)->text() : std::string();
		TEST_EXPECT(!text.empty() && text == crlf(blank_shader_text(tag, name)));
		for (const char *texture : shared)
			TEST_EXPECT(text.find(std::string("shared texture ") + texture + ";") != std::string::npos);
		TEST_EXPECT(text.find("TexAngleMap") == std::string::npos);
		return 0;
	};
	TEST_EXPECT(check_text(bytes, std::string(), "_ffp.fx") == 0);
	TEST_EXPECT(blank_shader_tags() == Names({"VS_PHONGT", "VS_DOT3DIFF2", "VS_SKBUMPPHONGT", "VS_SKBUMPDIFFT"}));
	for (const std::string &tag : blank_shader_tags()) {
		BlankRequest request;
		request.logical_name = "shaders/new.fx";
		request.values = {{"tag", tag}};
		TEST_EXPECT(make_blank(request, AssetKind::Shader, bytes, error));
		const bool skinned = tag.find("_SK") != std::string::npos;
		TEST_EXPECT(shader_tags_of(bytes, "new.fx") == (skinned ? Names({tag}) : Names({tag, tag + "#UV"})));
		TEST_EXPECT(check_text(bytes, tag, "new.fx") == 0);
	}
	// Refused: a tag it makes no effect for, a name the archive walk skips.
	BlankRequest refused;
	refused.logical_name = "glass.fx";
	refused.values = {{"tag", "VS_GLASS"}};
	TEST_EXPECT(!make_blank(refused, AssetKind::Shader, bytes, error) && error.code() == "blank.shader");
	refused.logical_name = "_glass.fx";
	refused.values = {{"tag", "VS_PHONGT"}};
	TEST_EXPECT(!make_blank(refused, AssetKind::Shader, bytes, error) && error.code() == "blank.shader");
	return 0;
}

// gt.ssc, which the game reads by its name decoded under its key chain: shown decoded, written back
// as stored byte for byte (its line end kept), an edit written encoded; a file that does not decode
// (the game skips it) shown as stored and written in the form; any other configuration its own text,
// encoded bytes included.
static int test_gate_tag_config() {
	const DocumentType *text = document_type_for(AssetKind::Config);
	TEST_EXPECT(text != nullptr);
	if (!text) return 1;
	const std::string stored = opennova::encode_key_chain(bytes_of("jop:cus2"), "jop:2:oyez") + "\r\n";
	Diagnostic error;
	std::unique_ptr<DocumentBase> tag = text->make();
	TEST_EXPECT(tag->load_bytes(bytes_of(stored), "GT.SSC", AssetKind::Config, "jo", error) && !tag->blocked());
	TEST_EXPECT(text_of(*tag) && text_of(*tag)->text() == "jop:cus2" && tag->serialize().text == stored &&
	            tag->rewrite_need() == DocumentBase::RewriteNeed::None);
	TEST_EXPECT(apply(*tag, {TextDocument::replace(span(1, 5, 4), "abcd")}) && text_of(*tag)->text() == "jop:abcd");
	std::vector<uint8_t> back;
	TEST_EXPECT(opennova::decode_key_chain(tag->serialize().text, "jop:2:oyez", back) &&
	            std::string(back.begin(), back.end()) == "jop:abcd" &&
	            tag->serialize().text == opennova::encode_key_chain(bytes_of("jop:abcd"), "jop:2:oyez") + "\r\n");
	// Not in the form: shown as stored, Save writes the form.
	std::unique_ptr<DocumentBase> plain = text->make();
	TEST_EXPECT(plain->load_bytes(bytes_of("jop:cus2"), "gt.ssc", AssetKind::Config, "jo", error) &&
	            text_of(*plain)->text() == "jop:cus2" &&
	            plain->serialize().text == opennova::encode_key_chain(bytes_of("jop:cus2"), "jop:2:oyez"));
	// Another configuration holding the same bytes: its own text.
	std::unique_ptr<DocumentBase> other = text->make();
	TEST_EXPECT(other->load_bytes(bytes_of(stored), "game.cfg", AssetKind::Config, "jo", error) &&
	            text_of(*other)->text() == stored && other->serialize().text == stored);
	return 0;
}

// A shader imported from an archive: copied as stored, its loader's own SCR form (the text readers'
// decode would make it noise), the project's file opening with no finding; the import plan's origin
// reads it so.
static int test_import_shader() {
	editor_test::TempProjectDir dir("opennova_editor_text_import");
	const std::string root = dir.file("project");
	ProjectDocument project;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Import", "jo", project, error));
	const std::vector<uint8_t> stored = scr_shader("float4 main() : COLOR { return 0; }\r\n");
	const std::string archive = dir.file("shaders.pff");
	const opennova::pff::PffWriteEntry entries[] = {{"glass.fx", stored.data(), uint32_t(stored.size()), 0, 0, 0}};
	TEST_EXPECT(opennova::pff::pff_write_archive(archive.c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 1) ==
	            opennova::pff::PFF_WRITE_OK);
	ImportChoice source;
	source.path = archive;
	source.entry = "glass.fx";
	const ImportResult result = import_assets({source}, ProjectPaths::for_root(root), project, false);
	TEST_EXPECT(result.imported.size() == 1);
	if (result.imported.size() != 1) return 1;
	const std::string imported = root + "/" + result.imported[0];
	TEST_EXPECT(test_io::read_file(imported) == stored);
	const DocumentType *type = document_type_for(AssetKind::Shader);
	std::unique_ptr<DocumentBase> made = type->make();
	TEST_EXPECT(made->load(imported, result.imported[0], AssetKind::Shader, "jo", error) &&
	            type->validate_file(*made).empty() && text_of(*made)->text() == "float4 main() : COLOR { return 0; }\r\n");
	ImportOrigin origin;
	std::string why;
	std::vector<uint8_t> read;
	TEST_EXPECT(origin.open(ImportOrigin::Kind::Archive, archive, project, why) && origin.read("glass.fx", read) &&
	            read == stored);
	// The extract and the editor read the one rule of which files their loader takes as stored.
	TEST_EXPECT(opennova::vfs_loader_takes_stored("x.fx") && !opennova::vfs_loader_takes_stored("x.wac"));
	return 0;
}

// The retail leg (OPENNOVA_JO_DIR): the install's scripts, music scripts, credits and shaders read
// and validated as the editor reads them. Each script compiled as the runtime's corpus test compiles
// it (no catalog, a RUN's file from the install) writes the listing the original compiler wrote
// (wac_corpus's committed vectors): the editor's compiler is the game's, and so are its reports; a
// script that RUNs no other file makes exactly those reports a finding each, at their places, but
// the names other files hold. Both music scripts (gamemus's MessageHandler among them) go through their MUS
// text and back byte for byte, as do the credits file and every shader.
static int test_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's scripts, music scripts, credits and shaders)");
		return 0;
	}
	ProjectDocument project;
	project.target_game = "jo";
	InstallView view;
	std::string view_error;
	TEST_EXPECT(view.open(install_spec(install, project), view_error));
	const opennova::Vfs &mount = view.vfs();
	size_t scripts = 0, compiled = 0, findings = 0, references = 0, music = 0, credits = 0, shaders = 0;
	size_t witnessed = 0, alone = 0, shader_tags = 0;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		const AssetKind kind = classify_asset(name, nullptr);
		std::vector<uint8_t> bytes;
		if (kind != AssetKind::Script && kind != AssetKind::Credits && kind != AssetKind::Shader &&
		    !opennova::strutil::ends_with_icase(name, ".bin"))
			continue;
		if (!mount.read_file_raw(name, bytes)) continue; // as stored: the document decodes it
		const AssetKind typed = classify_asset(name, &bytes);
		const DocumentType *type = document_type_for(typed);
		if (!type || document_content(*type) != DocumentContent::Text) continue;
		std::unique_ptr<DocumentBase> document = type->make();
		Diagnostic error;
		const bool loaded = document->load_bytes(bytes, name, typed, "jo", error);
		if (!loaded) std::fprintf(stderr, "retail: %s does not load: %s\n", name.c_str(), error.message.c_str());
		TEST_EXPECT(loaded);
		const std::vector<Diagnostic> found = type->validate_file(*document);
		const std::string stored(bytes.begin(), bytes.end());
		switch (typed) {
		case AssetKind::Script: {
			++scripts;
			TEST_EXPECT(document->serialize().text == stored);
			// As the runtime's corpus test compiles it: the original compiler's listing.
			opennova::wac::CompileEnv env;
			env.source_names = {"script.wac"};
			env.load_source = [&mount](const std::string &file, std::string &text) {
				std::vector<uint8_t> run;
				if (!mount.read_file_raw(file, run)) return false;
				text.assign(run.begin(), run.end());
				return true;
			};
			const opennova::wac::Program corpus = opennova::wac::compile_source(stored, env);
			const std::string source_hash = sha256(stored);
			const auto vector = std::find_if(std::begin(kCorpus), std::end(kCorpus),
			                                 [&](const CorpusVector &v) { return source_hash == v.source_sha256; });
			if (vector != std::end(kCorpus)) {
				++witnessed;
				TEST_EXPECT(sha256(wac_listing::document(corpus)) == vector->listing_sha256);
			}
			// The editor's findings: the reports of the text alone but the names other files hold, each
			// on the line the compiler counted (its CRs: every shipped script ends its lines CR LF, so
			// the editor's lines are the compiler's).
			const opennova::wac::Program program = compile_script(*text_of(*document));
			std::vector<const opennova::wac::Diagnostic *> own;
			for (const opennova::wac::Diagnostic &d : program.diagnostics)
				if (!d.table && d.source == 0) own.push_back(&d);
			TEST_EXPECT(found.size() == own.size() && text_of(*document)->odd_line_end() == std::string::npos);
			for (size_t i = 0; i < found.size() && i < own.size(); ++i) {
				if (found[i].line != size_t(own[i]->line))
					std::fprintf(stderr, "retail: %s: \"%s\" at line %zu, the compiler's %d\n", name.c_str(),
					             own[i]->message.c_str(), found[i].line, own[i]->line);
				TEST_EXPECT(found[i].line == size_t(own[i]->line) && found[i].column >= 1);
			}
			// One that RUNs nothing reports what the corpus compile reports of it.
			if (corpus.source_names.size() == 1) {
				size_t corpus_own = 0;
				for (const opennova::wac::Diagnostic &d : corpus.diagnostics) corpus_own += d.table ? 0 : 1;
				TEST_EXPECT(corpus_own == own.size());
				++alone;
			}
			compiled += program.diagnostics.size();
			findings += found.size();
			std::vector<TextReference> named;
			type->references(*text_of(*document), named);
			references += named.size();
			break;
		}
		case AssetKind::MusicScript:
			// Both shipped scripts: their text, MDEdit's layout written again, byte for byte.
			++music;
			TEST_EXPECT(!document->blocked() && document->serialize().text == stored && found.empty());
			break;
		case AssetKind::Credits:
			++credits;
			TEST_EXPECT(!document->blocked() && document->serialize().text == stored && found.empty());
			break;
		case AssetKind::Shader: {
			++shaders;
			TEST_EXPECT(!document->blocked() && document->serialize().text == stored && found.empty());
			std::vector<TextDefinition> tags;
			type->definitions(*text_of(*document), tags);
			shader_tags += tags.size();
			break;
		}
		default: break;
		}
	}
	std::printf("retail: %zu scripts (%zu with the original compiler's listing matched, %zu running no other "
	            "file; %zu compiler reports, %zu findings, %zu references), %zu music scripts, %zu credits files, "
	            "%zu shaders (%zu shader tags)\n",
	            scripts, witnessed, alone, compiled, findings, references, music, credits, shaders, shader_tags);
	// The install's counts, pinned (Joint Operations: Combined Arms).
	TEST_EXPECT(scripts == 23 && witnessed == 23 && alone == 23 && compiled == 47 && findings == 11 &&
	            references == 36 && music == 2 && credits == 1 && shaders == 44);
	// The tags the shaders register, as the renderer's registry holds them: _ffp.fx's 24 and the shipped
	// effects' (render-material-re.md, the 46-tag registry).
	TEST_EXPECT(shader_tags == 46);
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_lines_and_places();
	failures += test_span_edits();
	failures += test_folding_and_changes();
	failures += test_changes_moved();
	failures += test_history_budget();
	failures += test_snapshot_and_save();
	failures += test_script_type();
	failures += test_line_ends();
	failures += test_graph_and_rename();
	failures += test_go_to_and_problems();
	failures += test_wire();
	failures += test_credits();
	failures += test_credits_unread();
	failures += test_music_script();
	failures += test_shader_and_text();
	failures += test_text_readers();
	failures += test_project_notes();
	failures += test_shader_definitions();
	failures += test_gate_tag_config();
	failures += test_import_shader();
	failures += test_retail();
	if (failures == 0) std::printf("editor_text_document: all passed\n");
	return failures == 0 ? 0 : 1;
}
