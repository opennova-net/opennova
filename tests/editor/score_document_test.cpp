// The score table document (ADR 0046 S23 B; editor/documents/score_document.h): score.ini as rows over
// formats/score with the file's layout modeled (formats/textlayout): the file's version and fanfare first, then its
// GAMETYPE blocks, each its scoreboard columns (FIELD lines) and points (VAR lines); a value changed changes its own
// line, a column added goes after the block's last, a block added is the first game type the file lacks; a name no
// table has refused; the game's reading of the file as findings (a version other than 40, a fanfare it does not
// keep, a block of no game type's name, a game type's second block, more columns than a row holds). The retail leg
// (OPENNOVA_JO_DIR): the install's loose score.ini through the document, saved byte for byte, no finding.
#include <editor/documents/document_types.h>
#include <editor/documents/score_document.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kHeader = node_kind(ScoreKind::Header);
constexpr NodeKind kBlock = node_kind(ScoreKind::Block);
constexpr NodeKind kField = node_kind(ScoreKind::Field);
constexpr NodeKind kVar = node_kind(ScoreKind::Var);

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; });
}

Edit set_edit(const NodeAddress &at, const char *field, Value value) {
	Edit edit;
	edit.address = at;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
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
	TEST_EXPECT(document.serialize().text == kFile);
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
	std::string expected = kFile;
	expected.replace(expected.find("VAR \"ENEMYKILL\" 5"), 17, "VAR \"ENEMYKILL\" 7");
	TEST_EXPECT(saved == expected);
	// A name no table has is refused; a column's value is a byte.
	const NodeAddress deaths{tdm->id, kField, tdm->ids.lists[0][1].id};
	TEST_EXPECT(!document.apply(set_edit(deaths, "name", std::string("NUMNOTHING")), error));
	TEST_EXPECT(!document.apply(set_edit(deaths, "value", int64_t(300)), error));
	// A column added: after the block's last, in the writer's form, the file's line ends.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {tdm->id, kField, 0};
	add.field = "name";
	add.value = std::string("NUMSUICIDES");
	TEST_EXPECT(document.apply(add, error));
	saved = document.serialize().text;
	TEST_EXPECT(saved.find("FIELD \"NUMDEATHS\"   1   // shown\r\nFIELD \"NUMSUICIDES\" 0\r\n") != std::string::npos);
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
	TEST_EXPECT(zeros.apply(low, error) && zeros.apply(high_byte, error));
	const std::string set = zeros.serialize().text;
	TEST_EXPECT(set == "VERSION 40\r\n\r\nEXP_FANFARE 2 5\r\n\r\nGAMETYPE \"DM\"\r\nVAR \"FIRE\" 5\r\n" &&
	            validate_score_file(zeros).empty());
	std::string fields;
	for (int i = 0; i < 35; ++i) fields += "FIELD \"NUMDEATHS\" 1\r\n";
	const std::string flawed = "VERSION 40\r\nGAMETYPE \"NOPE\"\r\nGAMETYPE \"tdm\"\r\nGAMETYPE \"TDM\"\r\n" + fields;
	ScoreDocument document;
	TEST_EXPECT(document.load_bytes(bytes_of(flawed), "score.ini", AssetKind::Score, "jo", error));
	std::vector<Diagnostic> findings = validate_score_file(document);
	TEST_EXPECT(has_code(findings, "score.unknown_game_type") && has_code(findings, "score.game_type_repeated") &&
	            has_code(findings, "score.fields_past_34") && !has_code(findings, "score.version"));
	// A fanfare the game does not keep, set in the document (a file's line failing the gate is read for nothing).
	const NodeAddress header{document.rows()[0]->id, node_kind(ScoreKind::Header), 0};
	Edit high;
	high.address = header;
	high.field = "fanfare_low";
	high.value = int64_t(2);
	TEST_EXPECT(document.apply(high, error));
	TEST_EXPECT(has_code(validate_score_file(document), "score.fanfare_unkept"));
	// A file of another version: the game reads none of it, nor does the document (no block), and a save keeps its
	// lines; its version shown, not set.
	const std::string old = "VERSION 39\r\nEXP_FANFARE 2 1\r\nGAMETYPE \"TDM\"\r\nVAR \"FIRE\" 5\r\n";
	ScoreDocument older;
	TEST_EXPECT(older.load_bytes(bytes_of(old), "score.ini", AssetKind::Score, "jo", error) && older.rows().size() == 1);
	findings = validate_score_file(older);
	TEST_EXPECT(has_code(findings, "score.version") && !has_code(findings, "score.fanfare_unkept"));
	TEST_EXPECT(older.serialize().text == old);
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
	// None gates a build or blocks a save: the game reads any file.
	for (const FindingCodeRow &row : score_finding_codes()) TEST_EXPECT(!row.gates_build && !row.blocks_save);
	std::printf("findings: a version other than 40, a fanfare not kept, a block of no game type, a second block, 35 "
	            "columns\n");
	return 0;
}

int test_retail() {
	const std::string install = retail::install();
	const std::string path = install.empty() ? std::string() : retail::join(install, "score.ini");
	if (path.empty() || !retail::file_exists(path)) {
		retail::skip_leg("OPENNOVA_JO_DIR/score.ini (the install's score table through the document)");
		return 0;
	}
	const std::vector<uint8_t> bytes = test_io::read_file(path);
	ScoreDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes, "score.ini", AssetKind::Score, "jo", error));
	TEST_EXPECT(!document.blocked() && document.rows().size() > 1);
	const SerializeResult written = document.serialize();
	TEST_EXPECT(written.ok() && written.text == std::string(bytes.begin(), bytes.end()) && written.notes.empty());
	TEST_EXPECT(validate_score_file(document).empty());
	std::printf("retail: the install's score.ini through the document, %zu rows, saved byte for byte\n",
	            document.rows().size());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_rows() || test_findings() || test_retail()) return 1;
	return 0;
}
