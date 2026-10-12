// The tile placement document (ADR 0046 S25; editor/documents/til_document.h): a .til's tiles as rows over
// formats/til in the file's order (the game's draw order), each its cell, its corner in mission units exact to
// 1/65536 (y the stored +4, the drawn square's north edge) and its orientation byte whole, MED's +10 layer carried
// and kept ascending (a tile put in, copied or moved takes the layer of the row before it); a save writes MED's
// header from the rows, an emptied placement the header alone; a header MED would not write and bytes past the
// records are source notes a save leaves out; a file the game reads no tile of (no til0 mark, under 16 bytes) or
// whose count runs past its bytes opens read only. The retail leg opens every JO:CA .til and saves it
// byte-identical with no finding.
#include <editor/blank/blank_factory.h>
#include <editor/documents/til_document.h>
#include <formats/til/til_io.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kTile = node_kind(TilKind::Tile);

std::vector<uint8_t> minted(const TilFile &file) {
	std::vector<uint8_t> out;
	std::string why;
	save_til(file, out, why);
	return out;
}

// Three tiles: whole and fractional corners, a flags byte with Nile's selection mark, MED's layers 0, 0, 2.
TilFile three() {
	TilFile file;
	file.entries = {make_til_entry(16 << 16, 32 << 16, 7, 0x05, 0),
	                make_til_entry((48 << 16) + 0x8000, (8 << 16) + 3, 19, 0x0A, 0),
	                make_til_entry(-(5 << 16), -(7 << 16), 63, 0x03, 2)};
	return file;
}

std::vector<uint16_t> layers(const TilDocument &document) {
	std::vector<uint16_t> out;
	for (const TilOverlayEntry &e : document.file().entries) out.push_back(e.layer);
	return out;
}

const Diagnostic *find_code(const std::vector<Diagnostic> &findings, const char *code) {
	for (const Diagnostic &d : findings)
		if (d.code() == code) return &d;
	return nullptr;
}

NodeAddress at(const TilDocument &document, size_t i) { return {document.rows()[i]->id, kTile, 0}; }

bool apply_one(TilDocument &document, const Edit &edit) {
	Diagnostic error;
	return document.apply(edit, error);
}

int test_fields() {
	const std::vector<uint8_t> bytes = minted(three());
	TilDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes, "missions/00TRa.til", AssetKind::TileInfo, "jo", error));
	TEST_EXPECT(!document.blocked() && document.issues().empty() && document.rows().size() == 3);
	TEST_EXPECT(document.serialize().text == std::string(bytes.begin(), bytes.end()));
	TEST_EXPECT(validate_til_file(document).empty());
	Value value;
	TEST_EXPECT(document.get(at(document, 0), "tile", value) && value == Value(int64_t(7)));
	TEST_EXPECT(document.get(at(document, 0), "x", value) && value == Value(16.0));
	TEST_EXPECT(document.get(at(document, 0), "y", value) && value == Value(32.0));
	TEST_EXPECT(document.get(at(document, 1), "x", value) && value == Value(48.5));
	TEST_EXPECT(document.get(at(document, 1), "y", value) && value == Value(8.0 + 3.0 / 65536.0));
	TEST_EXPECT(document.get(at(document, 1), "orientation", value) && value == Value(int64_t(0x0A)));
	TEST_EXPECT(document.record_title(at(document, 0)) == "Tile 7 at (16, 32)");
	// A corner set exact to 1/65536, the y the stored z; the record's other bytes as they were.
	Edit set;
	set.address = at(document, 1);
	set.field = "y";
	set.value = -100.25;
	TEST_EXPECT(apply_one(document, set));
	TilFile want = three();
	want.entries[1].z_fixed = int32_t(-100.25 * 65536);
	const std::vector<uint8_t> want_bytes = minted(want);
	TEST_EXPECT(document.serialize().text == std::string(want_bytes.begin(), want_bytes.end()));
	// The orientation byte whole: the selection mark kept beside a turn.
	set.field = "orientation";
	set.value = int64_t(0x0C);
	TEST_EXPECT(apply_one(document, set));
	TEST_EXPECT(document.file().entries[1].flags == 0x0C);
	// Refused: a cell past 255, a corner past a 16.16 word, a text.
	set.field = "tile";
	set.value = int64_t(256);
	TEST_EXPECT(!apply_one(document, set));
	set.field = "x";
	set.value = 40000.0;
	TEST_EXPECT(!apply_one(document, set));
	set.value = std::string("left");
	TEST_EXPECT(!apply_one(document, set));
	document.undo();
	document.undo();
	TEST_EXPECT(document.serialize().text == std::string(bytes.begin(), bytes.end()));
	std::printf("fields: the cell, the corner exact to 1/65536, the whole orientation byte, refusals, undo\n");
	return 0;
}

int test_layers() {
	TilDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(minted(three()), "a.til", AssetKind::TileInfo, "jo", error));
	TEST_EXPECT(layers(document) == std::vector<uint16_t>({0, 0, 2}));
	// A tile added after the last takes its layer; one added first takes the layer of the row after it.
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kTile;
	TEST_EXPECT(apply_one(document, add) && layers(document) == std::vector<uint16_t>({0, 0, 2, 2}));
	add.position = 0;
	TEST_EXPECT(apply_one(document, add) && layers(document) == std::vector<uint16_t>({0, 0, 0, 2, 2}));
	// The last tile moved to the top: it takes the layer of the row after it (the first), the rows ascending.
	Edit move;
	move.operation = EditOperation::Move;
	move.address = at(document, 4);
	move.position = 0;
	TEST_EXPECT(apply_one(document, move) && layers(document) == std::vector<uint16_t>({0, 0, 0, 0, 2}));
	// A layer-0 tile moved past the layer-2 one: it lands after it and takes its layer (Nile's layer up).
	move.address = at(document, 3);
	move.position = 4;
	TEST_EXPECT(apply_one(document, move) && layers(document) == std::vector<uint16_t>({0, 0, 0, 2, 2}));
	// A duplicate lands beside its original, its layer.
	Edit duplicate;
	duplicate.operation = EditOperation::Duplicate;
	duplicate.address = at(document, 3);
	TEST_EXPECT(apply_one(document, duplicate) && layers(document) == std::vector<uint16_t>({0, 0, 0, 2, 2, 2}));
	// Undo takes each step back with the layers it set.
	document.undo();
	TEST_EXPECT(layers(document) == std::vector<uint16_t>({0, 0, 0, 2, 2}));
	document.undo();
	TEST_EXPECT(layers(document) == std::vector<uint16_t>({0, 0, 0, 0, 2}));
	document.redo();
	TEST_EXPECT(layers(document) == std::vector<uint16_t>({0, 0, 0, 2, 2}));
	// Every row removed: the save is MED's header alone.
	while (!document.rows().empty()) {
		Edit remove;
		remove.operation = EditOperation::Remove;
		remove.address = at(document, 0);
		TEST_EXPECT(apply_one(document, remove));
	}
	const std::string empty = document.serialize().text;
	TEST_EXPECT(empty.size() == 16 && empty[8] == 16 && empty.compare(0, 4, std::string("0lit")) == 0);
	std::printf("layers: an add, a move and a duplicate take the row before's layer (the first, the one after); undo; the empty save\n");
	return 0;
}

int test_source_notes() {
	// A header MED would not write and bytes past the records: notes a save leaves out.
	std::vector<uint8_t> foreign = minted(three());
	foreign[8] = 0x44;
	foreign[12] = 0x01;
	foreign.push_back(0xAB);
	TilDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(foreign, "a.til", AssetKind::TileInfo, "jo", error));
	TEST_EXPECT(!document.blocked() && document.issues().size() == 2 && document.rows().size() == 3);
	const std::vector<Diagnostic> findings = validate_til_file(document);
	TEST_EXPECT(findings.size() == 2 && find_code(findings, "til.ignored_input") &&
	            find_code(findings, "til.ignored_input")->severity == DiagnosticSeverity::Warning);
	const std::vector<uint8_t> meds = minted(three());
	TEST_EXPECT(document.serialize().text == std::string(meds.begin(), meds.end()));

	// No til0 mark: the game reads none of it (the terrain's tiles stand): read only, a listed warning.
	std::vector<uint8_t> unmarked = minted(three());
	unmarked[0] = 'X';
	TilDocument unread;
	TEST_EXPECT(unread.load_bytes(unmarked, "b.til", AssetKind::TileInfo, "jo", error));
	TEST_EXPECT(unread.blocked() && unread.rows().empty() && !unread.unread().empty());
	const std::vector<Diagnostic> none = validate_til_file(unread);
	TEST_EXPECT(none.size() == 1 && none[0].code() == "til.unread" && none[0].severity == DiagnosticSeverity::Warning &&
	            !finding_code(TilFinding::Unread).gates_build);
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kTile;
	TEST_EXPECT(!unread.apply(add, error));
	// Under 16 bytes: alike.
	TilDocument short_file;
	TEST_EXPECT(short_file.load_bytes(std::vector<uint8_t>(10, 0), "c.til", AssetKind::TileInfo, "jo", error));
	TEST_EXPECT(short_file.blocked() && validate_til_file(short_file)[0].code() == "til.unread");

	// A count past the bytes: the whole records there are, read only, an error the save is refused for.
	std::vector<uint8_t> over = minted(three());
	over[4] = 9;
	TilDocument past;
	TEST_EXPECT(past.load_bytes(over, "d.til", AssetKind::TileInfo, "jo", error));
	TEST_EXPECT(past.blocked() && past.rows().size() == 3 && past.unread().empty());
	const std::vector<Diagnostic> count = validate_til_file(past);
	TEST_EXPECT(count.size() == 1 && count[0].code() == "til.invalid_input" && count[0].severity == DiagnosticSeverity::Error);
	std::printf("source notes: MED's header written, trailing bytes dropped; no til0, short, a count past the bytes read only\n");
	return 0;
}

// What JO's mission editor makes of a file (S25 B5): notes, never the game's failure. Layers out of ascending
// order (its save regroups them, so the game then draws another order), and a file past its 6144 tiles.
int test_med_notes() {
	TilFile file;
	file.entries = {make_til_entry(0, 0, 1, 0, 2), make_til_entry(16 << 16, 0, 2, 0, 0)};
	TilDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(minted(file), "a.til", AssetKind::TileInfo, "jo", error));
	std::vector<Diagnostic> findings = validate_til_file(document);
	TEST_EXPECT(findings.size() == 1 && findings[0].code() == "til.layer_order" && findings[0].severity == DiagnosticSeverity::Info);
	TilFile many;
	many.entries.assign(6145, make_til_entry(0, 0, 1, 0, 0));
	TilDocument large;
	TEST_EXPECT(large.load_bytes(minted(many), "b.til", AssetKind::TileInfo, "jo", error));
	findings = validate_til_file(large);
	TEST_EXPECT(findings.size() == 1 && findings[0].code() == "til.med_cap" && findings[0].severity == DiagnosticSeverity::Info);
	std::printf("MED's notes: layers out of its order, past its 6144 tiles\n");
	return 0;
}

int test_blank() {
	const BlankFactory *factory = find_blank_factory_for_kind(AssetKind::TileInfo);
	TEST_EXPECT(factory != nullptr);
	BlankRequest request;
	request.logical_name = "newmission.til";
	std::vector<uint8_t> bytes;
	Diagnostic error;
	TEST_EXPECT(factory->make(request, bytes, error) && bytes.size() == 16);
	TilDocument document;
	TEST_EXPECT(document.load_bytes(bytes, "newmission.til", AssetKind::TileInfo, "jo", error) && document.rows().empty());
	TEST_EXPECT(validate_til_file(document).empty() && document.serialize().text == std::string(bytes.begin(), bytes.end()));
	std::printf("blank: no tile, opened clean\n");
	return 0;
}

int test_retail() {
	if (!retail::selected()) return 0;
	const std::string assets = retail::assets();
	if (assets.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (the shipped .til files)");
		return 0;
	}
	int swept = 0;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(assets)) {
		std::string ext = entry.path().extension().string();
		for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
		if (!entry.is_regular_file() || ext != ".til") continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().string());
		TilDocument document;
		Diagnostic error;
		const std::string name = entry.path().filename().string();
		TEST_EXPECT(document.load_bytes(bytes, name, AssetKind::TileInfo, "jo", error));
		TEST_EXPECT(!document.blocked() && document.issues().empty() && validate_til_file(document).empty());
		TEST_EXPECT(document.serialize().text == std::string(bytes.begin(), bytes.end()));
		++swept;
	}
	TEST_EXPECT(swept > 0);
	std::printf("retail: %d .til opened and saved byte-identical, no finding\n", swept);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	const int failures = test_fields() + test_layers() + test_source_notes() + test_med_notes() + test_blank() + test_retail();
	if (failures == 0) std::printf("editor_til_document: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
