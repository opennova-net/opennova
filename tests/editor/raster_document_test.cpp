// The raster document (ADR 0046 S25, lane A1): a raster read through the codec its picker chose, an unedited
// one serializing to its own bytes; its edits (Apply payloads writing on the canvas) one step a batch, a
// gesture's one step, the undo and redo of its tiles and side state, its changes as regions of tiles (each
// plane's rows of changed tiles one region, cut at the plane's edge) and side parts, a whole change it cannot
// say regions of; a bake's landing joining its stroke's step and refused once that step is gone; a refused
// payload putting back what it wrote; a cancelled gesture dropped; a snapshot read-only and apart; a save
// written and checked against; a file no codec reads refused; and a type of it counted as raster content.
#include <editor/documents/document_types.h>
#include <editor/model/core_finding.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/raster_document.h>
#include <editor/model/text_document.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "common/test_expect.h"
#include "editor/raster_blob_document.h"

using namespace opennova::editor;
using editor_test::raster_blob_file;
using editor_test::raster_edit;
using editor_test::RasterFill;
using editor_test::RasterGrow;
using editor_test::RasterNote;
using editor_test::RasterRefuse;
using editor_test::RasterSetNote;

namespace {

std::unique_ptr<RasterDocument> open_blob(const std::vector<uint8_t> &bytes) {
	auto document = std::make_unique<RasterDocument>(editor_test::pick_raster_blob);
	Diagnostic error;
	if (!document->load_bytes(bytes, "maps/test.rb", AssetKind::Unknown, "jo", error)) return nullptr;
	return document;
}

std::vector<uint8_t> written(const DocumentBase &document) {
	const SerializeResult out = document.serialize();
	return std::vector<uint8_t>(out.text.begin(), out.text.end());
}

const RasterChanges *raster_changes(const ChangeSet &set) { return std::get_if<RasterChanges>(&set); }

int test_read_edit_undo() {
	const std::vector<uint8_t> file = raster_blob_file(130, 70, RasterFormat::U8, 3);
	std::unique_ptr<RasterDocument> doc = open_blob(file);
	TEST_EXPECT(doc && doc->as_raster() == doc.get() && doc->codec() && doc->plane_count() == 1);
	TEST_EXPECT(doc->plane(0).width() == 130 && doc->plane(0).u8(129, 69) == 3 && doc->revision() == 0 && !doc->dirty());
	TEST_EXPECT(written(*doc) == file); // unedited: its own bytes
	TEST_EXPECT(doc->save_words() == "Writes the raster as it holds it.");

	Diagnostic error;
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(60, 5, 10, 5, 9)), error));
	TEST_EXPECT(doc->dirty() && doc->revision() == 1 && doc->plane(0).u8(60, 5) == 9 && doc->plane(0).u8(59, 5) == 3);
	ChangeSet changes;
	TEST_EXPECT(doc->changes_since(doc->load_generation(), 0, changes));
	const RasterChanges *raster = raster_changes(changes);
	TEST_EXPECT(raster && raster->regions.size() == 1 && raster->parts == 0);
	TEST_EXPECT(raster->regions[0].x == 0 && raster->regions[0].y == 0 && raster->regions[0].width == 128 &&
	            raster->regions[0].height == 64 && raster->regions[0].plane == 0);
	// The last column of tiles and the last row, cut at the plane's edge.
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(129, 69, 1, 1, 5)), error));
	TEST_EXPECT(doc->changes_since(doc->load_generation(), 1, changes) && raster_changes(changes)->regions.size() == 1);
	const RasterRegion corner = raster_changes(changes)->regions[0];
	TEST_EXPECT(corner.x == 128 && corner.width == 2 && corner.y == 64 && corner.height == 6);
	TEST_EXPECT(!doc->changes_since(doc->load_generation() + 1, 1, changes)); // another load's state

	doc->undo();
	doc->undo();
	TEST_EXPECT(!doc->dirty() && doc->revision() == 0 && written(*doc) == file);
	doc->redo();
	TEST_EXPECT(doc->plane(0).u8(60, 5) == 9 && doc->plane(0).u8(129, 69) == 3);

	// A gesture's batches: one step, its link the gesture.
	const uint64_t gesture = next_edit_gesture();
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(0, 0, 2, 2, 7), gesture), error));
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(100, 60, 2, 2, 8), gesture), error));
	TEST_EXPECT(doc->undo_mark().link == gesture && doc->undo_mark().serial);
	doc->undo();
	TEST_EXPECT(doc->plane(0).u8(0, 0) == 3 && doc->plane(0).u8(100, 60) == 3 && doc->plane(0).u8(60, 5) == 9);
	doc->redo();

	// The side state: its parts named, undone with its step.
	const uint64_t before_note = doc->revision();
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterSetNote>("other")), error));
	TEST_EXPECT(doc->changes_since(doc->load_generation(), before_note, changes) &&
	            raster_changes(changes)->regions.empty() && raster_changes(changes)->parts == 1u);
	TEST_EXPECT(static_cast<const RasterNote *>(doc->side())->text == "other");
	doc->undo();
	TEST_EXPECT(static_cast<const RasterNote *>(doc->side())->text == "note");
	doc->redo();
	TEST_EXPECT(doc->history_bytes() > 0);

	// A whole change: undone whole, never said in regions.
	const uint64_t before_grow = doc->revision();
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterGrow>(200, 80)), error));
	TEST_EXPECT(doc->plane(0).width() == 200 && doc->plane(0).u8(60, 5) == 9 && doc->plane(0).u8(199, 79) == 0);
	TEST_EXPECT(!doc->changes_since(doc->load_generation(), before_grow, changes));
	doc->undo();
	TEST_EXPECT(doc->plane(0).width() == 130 && doc->plane(0).u8(60, 5) == 9);
	return 0;
}

int test_amend_refusals_cancel() {
	std::unique_ptr<RasterDocument> doc = open_blob(raster_blob_file(130, 70, RasterFormat::U16, 0));
	TEST_EXPECT(doc);
	Diagnostic error;
	// A stroke, ended, then its bake landing inside its step: one undo takes both.
	const uint64_t stroke = next_edit_gesture();
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(10, 10, 4, 4, 1), stroke), error));
	doc->end_edit_group();
	TEST_EXPECT(doc->can_amend());
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(10, 10, 60, 4, 2, true)), error));
	TEST_EXPECT(doc->plane(0).u16(60, 12) == 0x0202 && doc->undo_mark().link == stroke && doc->revision() == 2);
	doc->undo();
	TEST_EXPECT(doc->plane(0).u16(10, 10) == 0 && doc->plane(0).u16(60, 12) == 0 && !doc->dirty());
	// The stroke's step undone: a bake for it no longer lands, the content as it was.
	TEST_EXPECT(!doc->apply(raster_edit(std::make_shared<RasterFill>(10, 10, 4, 4, 3, true)), error));
	TEST_EXPECT(error.code() == finding_code(CoreFinding::DocumentStale).token && doc->plane(0).u16(10, 10) == 0);
	doc->redo();

	// A payload that refuses after writing: what it wrote put back, no step.
	const uint64_t at = doc->revision();
	TEST_EXPECT(!doc->apply(std::vector<Edit>{raster_edit(std::make_shared<RasterFill>(0, 0, 1, 1, 5)),
	                                          raster_edit(std::make_shared<RasterRefuse>())},
	                        error));
	TEST_EXPECT(doc->revision() == at && doc->plane(0).u16(0, 0) == 0 && error.asset == "maps/test.rb");
	// Another kind's payload is refused.
	TEST_EXPECT(!doc->apply(TextDocument::replace(TextSpan{0, 0, 0}, "x"), error));
	TEST_EXPECT(error.code() == finding_code(CoreFinding::DocumentPayload).token && doc->revision() == at);

	// A cancelled gesture: its step dropped, nothing to redo.
	const uint64_t drag = next_edit_gesture();
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(80, 40, 3, 3, 4), drag), error));
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(90, 40, 3, 3, 4), drag), error));
	TEST_EXPECT(doc->cancel_gesture(drag) && doc->revision() == at && !doc->can_redo() && doc->plane(0).u16(80, 40) == 0);
	return 0;
}

int test_snapshot_save_refused() {
	std::unique_ptr<RasterDocument> doc = open_blob(raster_blob_file(64, 64, RasterFormat::Rgb8, 1));
	TEST_EXPECT(doc);
	Diagnostic error;
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(1, 1, 1, 1, 9)), error));
	const std::unique_ptr<DocumentBase> snap = doc->snapshot();
	TEST_EXPECT(snap->is_snapshot() && snap->as_raster() && snap->revision() == doc->revision());
	TEST_EXPECT(doc->apply(raster_edit(std::make_shared<RasterFill>(2, 2, 1, 1, 8)), error));
	TEST_EXPECT(snap->as_raster()->plane(0).texel(2, 2)[0] == 1 && snap->as_raster()->plane(0).texel(1, 1)[0] == 9);
	TEST_EXPECT(!snap->apply(raster_edit(std::make_shared<RasterFill>(0, 0, 1, 1, 7)), error));
	TEST_EXPECT(!snap->can_undo());

	// A save over a file: written, checked against, clean.
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "opennova_raster_document_test";
	std::filesystem::create_directories(dir);
	const std::filesystem::path file = dir / "map.rb";
	{
		const std::vector<uint8_t> bytes = raster_blob_file(64, 64, RasterFormat::Rgb8, 1);
		std::ofstream out(file, std::ios::binary);
		out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
	}
	RasterDocument saved(editor_test::pick_raster_blob);
	TEST_EXPECT(saved.load(file.string(), "map.rb", AssetKind::Unknown, "jo", error));
	TEST_EXPECT(saved.apply(raster_edit(std::make_shared<RasterFill>(3, 3, 2, 2, 6)), error));
	TEST_EXPECT(saved.save(error) && !saved.dirty() && saved.matches_file());
	std::ifstream in(file, std::ios::binary);
	const std::vector<uint8_t> on_disk((std::istreambuf_iterator<char>(in)), {});
	TEST_EXPECT(on_disk == written(saved) && on_disk[8 + 4 + (3 * 64 + 3) * 3] == 6);
	in.close();
	std::filesystem::remove_all(dir);

	// A file no codec reads.
	RasterDocument nothing(editor_test::pick_raster_blob);
	TEST_EXPECT(!nothing.load_bytes({'X', 'X', 1, 2}, "maps/odd.rb", AssetKind::Unknown, "jo", error));
	TEST_EXPECT(error.code() == finding_code(CoreFinding::DocumentDecode).token);
	return 0;
}

std::unique_ptr<DocumentBase> make_raster() { return std::make_unique<RasterDocument>(editor_test::pick_raster_blob); }
std::vector<Diagnostic> validate_nothing(const DocumentBase &) { return {}; }

int test_content() {
	DocumentType type;
	type.id = DocumentTypeId::Styles;
	type.name = "raster";
	type.make = make_raster;
	type.validate_file = validate_nothing;
	const DocumentTypeStandIn stand_in(type);
	TEST_EXPECT(document_content(type) == DocumentContent::Raster && validates_files(type));
	return 0;
}

} // namespace

int main() {
	int failures = 0;
	failures += test_read_edit_undo();
	failures += test_amend_refusals_cancel();
	failures += test_snapshot_save_refused();
	failures += test_content();
	if (failures == 0) std::printf("editor_raster_document: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
