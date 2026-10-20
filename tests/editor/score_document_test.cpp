// The score table document (ADR 0046 S23 B; editor/documents/score_document.h): score.ini as rows over
// formats/score, a save writing the model alone in ScoreConfig_SaveFile's form (the S26 ruling): the file's version
// and fanfare first, then its GAMETYPE blocks, each its scoreboard columns (FIELD lines) and points (VAR lines); a
// value changed changes its own line, a column added goes after the block's last, a block added is the first game
// type the file lacks; a name no table has refused; the game's reading of the file as findings (a version other than
// 40, a fanfare it does not keep, a block of no game type's name, a game type's second block). A table in that
// writer's own form, as the game writes its score.ini (every game type's block), saved byte for byte with no note
// and no finding; the format's variants (its fixtures) through the document, saved as the writer writes what the
// reader reads, a fixed point.
#include <editor/documents/document_types.h>
#include <editor/documents/score_document.h>
#include <formats/score/score.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/variant_files.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kHeader = node_kind(ScoreKind::Header);
constexpr NodeKind kBlock = node_kind(ScoreKind::Block);
constexpr NodeKind kField = node_kind(ScoreKind::Field);
constexpr NodeKind kVar = node_kind(ScoreKind::Var);

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

using editor_test::has_code;
using editor_test::set_edit;

// The text a save of `text` writes: formats/score's writer of what its reader reads.
std::string written(const std::string &text) {
	score::File file;
	std::string error;
	std::vector<uint8_t> out;
	if (!score::parse(reinterpret_cast<const uint8_t *>(text.data()), text.size(), file, error) ||
	    !score::write(file, out, error))
		return std::string();
	return std::string(out.begin(), out.end());
}

std::string replaced(std::string text, const std::string &from, const std::string &to) {
	text.replace(text.find(from), from.size(), to);
	return text;
}

const ScoreBlockRow *block_named(const ScoreDocument &document, const std::string &name) {
	for (const auto &row : document.rows())
		if (row && row->kind == kBlock && row->name() == name) return static_cast<const ScoreBlockRow *>(row.get());
	return nullptr;
}

const std::string kFile =
		"// my scores\r\n"
		"VERSION 40\r\n"
		"EXP_FANFARE 1 2\r\n"
		"\r\n"
		"GAMETYPE \"TDM\"\r\n"
		"FIELD \"NUMENEMYKILLS\" 1\r\n"
		"FIELD \"NUMDEATHS\"   1   // shown\r\n"
		"\r\n"
		"VAR \"FIRE\" 0\r\n"
		"VAR \"ENEMYKILL\" 5\r\n"
		"\r\n"
		"GAMETYPE \"COOP\"\r\n"
		"VAR \"MEDICSAVE\" 2\r\n";

int test_rows() {
	ScoreDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of(kFile), "score.ini", AssetKind::Score, "jo", error));
	TEST_EXPECT(document.rows().size() == 3 && !document.blocked());
	TEST_EXPECT(!written(kFile).empty() && document.serialize().text == written(kFile));
	const NodeAddress header{document.rows()[0]->id, kHeader, 0};
	Value value;
	TEST_EXPECT(document.get(header, "version", value) && value == Value(int64_t(40)));
	TEST_EXPECT(document.get(header, "fanfare_high", value) && value == Value(int64_t(2)));
	const ScoreBlockRow *tdm = block_named(document, "TDM");
	TEST_EXPECT(tdm && tdm->block.fields.size() == 2 && tdm->block.vars.size() == 2);
	if (!tdm) return 1;
	TEST_EXPECT(document.record_title({tdm->id, kBlock, 0}) == "TDM (2 columns, 2 events)");
	const NodeAddress kill{tdm->id, kVar, tdm->ids.lists[1][1].id};
	TEST_EXPECT(document.get(kill, "value", value) && value == Value(int64_t(5)));
	// A value changed: its own line.
	TEST_EXPECT(document.apply(set_edit(kill, "value", int64_t(7)), error));
	std::string saved = document.serialize().text;
	TEST_EXPECT(saved == written(replaced(kFile, "VAR \"ENEMYKILL\" 5", "VAR \"ENEMYKILL\" 7")));
	// A name no table has is refused; a column's value is a byte.
	const NodeAddress deaths{tdm->id, kField, tdm->ids.lists[0][1].id};
	TEST_EXPECT(!document.apply(set_edit(deaths, "name", std::string("NUMNOTHING")), error));
	TEST_EXPECT(!document.apply(set_edit(deaths, "value", int64_t(300)), error));
	// A column added: after the block's last.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {tdm->id, kField, 0};
	add.field = "name";
	add.value = std::string("NUMSUICIDES");
	TEST_EXPECT(document.apply(add, error));
	saved = document.serialize().text;
	TEST_EXPECT(saved == written(replaced(replaced(kFile, "VAR \"ENEMYKILL\" 5", "VAR \"ENEMYKILL\" 7"),
	                                      "// shown\r\n", "// shown\r\nFIELD \"NUMSUICIDES\" 0\r\n")));
	// A block added: the first game type the file lacks (TKOTH), read back as the game reads it.
	Edit block;
	block.operation = EditOperation::Add;
	block.address = {0, kBlock, 0};
	TEST_EXPECT(document.apply(block, error));
	TEST_EXPECT(block_named(document, "TKOTH") != nullptr);
	saved = document.serialize().text;
	TEST_EXPECT(saved.find("GAMETYPE \"TKOTH\"") != std::string::npos);
	ScoreDocument again;
	TEST_EXPECT(again.load_bytes(bytes_of(saved), "score.ini", AssetKind::Score, "jo", error) && again.rows().size() == 4 &&
	            block_named(again, "TKOTH") != nullptr);
	// The file's own row is no row to add.
	Edit header_add;
	header_add.operation = EditOperation::Add;
	header_add.address = {0, kHeader, 0};
	TEST_EXPECT(!document.apply(header_add, error));
	std::printf("rows: the file's values and its blocks, a value's own line, a column added after the last, a block "
	            "added the first game type the file lacks\n");
	return 0;
}

int test_findings() {
	ScoreDocument clean;
	Diagnostic error;
	TEST_EXPECT(clean.load_bytes(bytes_of(kFile), "score.ini", AssetKind::Score, "jo", error));
	TEST_EXPECT(validate_score_file(clean).empty());
	// The game's own EXP_FANFARE 0 0 is the file's fanfare line: no finding (the game holds what it says), and a set
	// of the pair rewrites that line, no second one (the #997 review's item 6).
	const std::string shipped = "VERSION 40\r\n\r\nEXP_FANFARE 0 0\r\n\r\nGAMETYPE \"DM\"\r\nVAR \"FIRE\" 5\r\n";
	ScoreDocument zeros;
	TEST_EXPECT(zeros.load_bytes(bytes_of(shipped), "score.ini", AssetKind::Score, "jo", error) &&
	            validate_score_file(zeros).empty());
	const NodeAddress zeros_header{zeros.rows()[0]->id, node_kind(ScoreKind::Header), 0};
	Edit low;
	low.address = zeros_header;
	low.field = "fanfare_low";
	low.value = int64_t(2);
	Edit high_byte = low;
	high_byte.field = "fanfare_high";
	high_byte.value = int64_t(5);
	TEST_EXPECT(zeros.apply(std::vector<Edit>{low, high_byte}, error));
	const std::string set = zeros.serialize().text;
	TEST_EXPECT(set == written(replaced(shipped, "EXP_FANFARE 0 0", "EXP_FANFARE 2 5")) &&
	            set.find("EXP_FANFARE 2 5\r\n") != std::string::npos && validate_score_file(zeros).empty());
	std::string fields;
	for (int i = 0; i < 35; ++i) fields += "FIELD \"NUMDEATHS\" 1\r\n";
	const std::string flawed = "VERSION 40\r\nGAMETYPE \"NOPE\"\r\nGAMETYPE \"tdm\"\r\nGAMETYPE \"TDM\"\r\n" + fields;
	ScoreDocument document;
	TEST_EXPECT(document.load_bytes(bytes_of(flawed), "score.ini", AssetKind::Score, "jo", error));
	std::vector<Diagnostic> findings = validate_score_file(document);
	TEST_EXPECT(has_code(findings, "score.unknown_game_type") && has_code(findings, "score.game_type_repeated") &&
	            !has_code(findings, "score.version"));
	// The 35th column the reader refuses (a row holds 34) is none of the model's: the second TDM block's.
	const ScoreBlockRow *tdm = nullptr;
	for (const auto &row : document.rows())
		if (row && row->kind == kBlock && row->name() == "TDM") tdm = static_cast<const ScoreBlockRow *>(row.get());
	TEST_EXPECT(tdm && tdm->block.fields.size() == score::kMaxFields);
	// A fanfare the game does not keep, set in the document (a file's line failing the gate is read for nothing): its
	// save refused (the writer writes none the reader would not read back).
	const NodeAddress header{document.rows()[0]->id, node_kind(ScoreKind::Header), 0};
	Edit high;
	high.address = header;
	high.field = "fanfare_low";
	high.value = int64_t(2);
	TEST_EXPECT(document.apply(high, error));
	TEST_EXPECT(has_code(validate_score_file(document), "score.fanfare_unkept") && !document.serialize().ok());
	// A file of another version: the game reads none of it, nor does the document (no block), and a save writes its
	// version alone; its version shown, not set.
	const std::string old = "VERSION 39\r\nEXP_FANFARE 2 1\r\nGAMETYPE \"TDM\"\r\nVAR \"FIRE\" 5\r\n";
	ScoreDocument older;
	TEST_EXPECT(older.load_bytes(bytes_of(old), "score.ini", AssetKind::Score, "jo", error) && older.rows().size() == 1);
	findings = validate_score_file(older);
	TEST_EXPECT(has_code(findings, "score.version") && !has_code(findings, "score.fanfare_unkept"));
	TEST_EXPECT(older.serialize().ok() && older.serialize().text == written(old) &&
	            older.serialize().text.find("GAMETYPE") == std::string::npos);
	Edit version;
	version.address = NodeAddress{older.rows()[0]->id, node_kind(ScoreKind::Header), 0};
	version.field = "version";
	version.value = int64_t(40);
	TEST_EXPECT(!older.apply(version, error));
	// A second block that changes nothing its row holds is no matter (the game's own writer writes COOP twice).
	ScoreDocument alike;
	TEST_EXPECT(alike.load_bytes(bytes_of("VERSION 40\r\nGAMETYPE \"COOP\"\r\nVAR \"FIRE\" 1\r\nGAMETYPE \"COOP\"\r\nVAR \"FIRE\" 1\r\n"),
	                             "score.ini", AssetKind::Score, "jo", error) &&
	            validate_score_file(alike).empty());
	// None gates a build: the game reads any file. A fanfare the game does not keep blocks the save alone.
	for (const FindingCodeRow &row : score_finding_codes())
		TEST_EXPECT(!row.gates_build && row.blocks_save == (std::string(row.token) == "score.fanfare_unkept"));
	std::printf("findings: a version other than 40, a fanfare not kept, a block of no game type, a second block\n");
	return 0;
}

// The game's own score.ini is ScoreConfig_SaveFile's output [orig: @ 0x52CDD0]: a block for each of the table's
// twelve rows by index, row 2's put down first under index 0 [orig: @ 0x52CF4C], so COOP twice and the two alike
// (JO:CA's: 12 blocks, the document's 13 rows with the file's own). Minted in that form (each block its own
// columns and points), the document saves it byte for byte, with no note and no finding.
int test_writer_form() {
	score::File file;
	for (int index = 0; index < 12; ++index) {
		score::GameTypeBlock block;
		block.row = index == 0 ? 2 : index;
		const int seed = index == 0 ? 2 : index;
		block.fields.push_back({score::field_id("NUMENEMYKILLS"), 1});
		if (seed % 2) block.fields.push_back({score::field_id("NUMDEATHS"), 1});
		block.vars.push_back({score::var_id("FIRE"), 0});
		block.vars.push_back({score::var_id("ENEMYKILL"), 5 + seed});
		if (seed % 3 == 0) block.vars.push_back({score::var_id("FRIENDLYKILL"), -seed});
		file.blocks.push_back(block);
	}
	std::vector<uint8_t> bytes;
	std::string why;
	TEST_EXPECT(score::write(file, bytes, why));
	ScoreDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes, "score.ini", AssetKind::Score, "jo", error));
	TEST_EXPECT(!document.blocked() && document.rows().size() == 13);
	const SerializeResult saved = document.serialize();
	TEST_EXPECT(saved.ok() && saved.text == std::string(bytes.begin(), bytes.end()) && saved.notes.empty());
	TEST_EXPECT(validate_score_file(document).empty());
	std::printf("writer form: a score.ini of the game's twelve blocks, %zu rows, saved byte for byte\n",
	            document.rows().size());
	return 0;
}

// The format's variants: each score.ini its fixtures hold (the authored sample, the format's row's shapes) through
// the document, unblocked, saved as the writer writes what the reader reads, that save read again the same text;
// one in the writer's own form saved byte for byte with no note.
int test_variants() {
	const editor_test::VariantFiles variants = editor_test::variant_files({"fixtures/score"}, {".ini"});
	int failures = editor_test::report_missing(variants);
	for (const std::string &path : variants.files) {
		const std::vector<uint8_t> bytes = editor_test::repo_bytes(path);
		const std::string text(bytes.begin(), bytes.end());
		ScoreDocument document;
		Diagnostic error;
		TEST_EXPECT(!bytes.empty() && document.load_bytes(bytes, "score.ini", AssetKind::Score, "jo", error));
		TEST_EXPECT(!document.blocked() && document.rows().size() > 1);
		const SerializeResult saved = document.serialize();
		ScoreDocument again;
		if (!saved.ok() || written(text).empty() || saved.text != written(text) ||
		    !again.load_bytes(bytes_of(saved.text), "score.ini", AssetKind::Score, "jo", error) ||
		    again.serialize().text != saved.text || (written(text) == text && !saved.notes.empty())) {
			std::fprintf(stderr, "FAIL %s is not saved as the format's writer writes it\n", path.c_str());
			++failures;
			continue;
		}
		std::printf("variant: %s, %zu rows, saved as the writer writes it%s\n", path.c_str(), document.rows().size(),
		            written(text) == text ? ", byte for byte" : "");
	}
	TEST_EXPECT(!variants.files.empty());
	return failures;
}

} // namespace

int main() {
	if (test_rows() || test_findings() || test_writer_form() || test_variants()) return 1;
	return 0;
}
