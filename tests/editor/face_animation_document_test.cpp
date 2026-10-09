// Face animations (ADR 0046, round S23 lane A): a .grm as a record document over the engine's own reader and
// from-scratch writer (grm::parse, grm::write): the authored fixtures/grm/person.grm read and written back byte for
// byte; every field edited through the table and written; a triangle's corners following their vertices (a Record
// reference: a vertex moved, added before them or removed); the type's findings; the references (each texture name
// made .TGA by the stage loader, the base's .MDT twin derived) and the blank. JO ships no face animation (a scan of
// every PFF of the install found none, docs/world/world-wac-ai-re.md section 33.30): the retail leg
// (OPENNOVA_JO_DIR) scans the install's archives again and reads any .grm it finds through the document.
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/documents/face_animation_document.h>
#include <editor/documents/texture_load_rules.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_edge.h>
#include <editor/graph/reference_kinds.h>
#include <formats/grm/grm.h>
#include <formats/pff/pff.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr NodeKind kFace = node_kind(FaceAnimationKind::Face);
constexpr NodeKind kVertex = node_kind(FaceAnimationKind::Vertex);
constexpr NodeKind kTriangle = node_kind(FaceAnimationKind::Triangle);
constexpr NodeKind kGesture = node_kind(FaceAnimationKind::Gesture);
constexpr NodeKind kParameter = node_kind(FaceAnimationKind::Parameter);

std::vector<uint8_t> person() {
	return test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/grm/person.grm");
}

Edit set_edit(NodeAddress address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; });
}

size_t count_code(const std::vector<Diagnostic> &findings, const char *code) {
	return size_t(std::count_if(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; }));
}

const FaceAnimationRow &face_of(const FaceAnimationDocument &document) { return *document.face_row(); }

NodeAddress child(const FaceAnimationDocument &document, size_t list, size_t index, NodeKind kind) {
	const FaceAnimationRow &face = face_of(document);
	return {face.id, kind, face.ids.lists[list][index].id};
}

bool load(FaceAnimationDocument &document, const std::vector<uint8_t> &bytes, Diagnostic &error) {
	return document.load_bytes(bytes, "anims/person.grm", AssetKind::FaceAnimation, "jo", error);
}

grm::File written(const FaceAnimationDocument &document) {
	const SerializeResult result = document.serialize();
	grm::File file;
	std::string message;
	if (!result.ok() ||
	    !grm::parse(reinterpret_cast<const uint8_t *>(result.text.data()), result.text.size(), file, message))
		file.base_texture = "<unwritten>";
	return file;
}

// The fixture through the document and back, byte for byte; its records as the reader read them.
int test_reads_and_writes_back() {
	const std::vector<uint8_t> bytes = person();
	TEST_EXPECT(!bytes.empty());
	FaceAnimationDocument face;
	Diagnostic error;
	TEST_EXPECT(load(face, bytes, error));
	TEST_EXPECT(face.rows().size() == 1 && face.face_row() != nullptr && !face.blocked());
	TEST_EXPECT(face.issues().empty());
	Value value;
	const NodeAddress row{face_of(face).id, kFace, 0};
	TEST_EXPECT(face.get(row, "base_texture", value) && std::get<std::string>(value) == "face.tga");
	TEST_EXPECT(face.get(row, "eye_texture_2", value) && std::get<std::string>(value) == "iris_alpha.tga");
	TEST_EXPECT(face.get(row, "eye2_center_x", value) && std::get<double>(value) == double(0.65f));
	TEST_EXPECT(face.get(row, "saved", value) && std::get<std::string>(value) == "9/9/2026, 14:05:06");
	TEST_EXPECT(face.get(row, "author", value) && std::get<std::string>(value) == "OpenNova");
	TEST_EXPECT(face.get(child(face, 0, 1, kVertex), "group", value) && std::get<std::string>(value) == "brow");
	TEST_EXPECT(face.get(child(face, 1, 1, kTriangle), "c", value) && std::get<int64_t>(value) == 3);
	TEST_EXPECT(face.get(child(face, 2, 1, kGesture), "name", value) && std::get<std::string>(value) == "HAPPY");
	TEST_EXPECT(face.record_title(child(face, 0, 2, kVertex)) == "Vertex 2: mouth");
	TEST_EXPECT(face.record_title(child(face, 1, 1, kTriangle)) == "Triangle 1: 0, 2, 3");
	const SerializeResult result = face.serialize();
	TEST_EXPECT(result.ok() && std::vector<uint8_t>(result.text.begin(), result.text.end()) == bytes);
	TEST_EXPECT(validate_face_animation_file(face).empty());
	return 0;
}

// Every field through the table, written and read back by the engine's reader; the names the reader would read
// otherwise refused; a gesture and its parameters added in one batch.
int test_edits() {
	FaceAnimationDocument face;
	Diagnostic error;
	TEST_EXPECT(load(face, person(), error));
	const NodeAddress row{face_of(face).id, kFace, 0};
	TEST_EXPECT(face.apply({set_edit(row, "base_texture", std::string("textures/smile.bmp")),
	                        set_edit(row, "eye_size_y", 0.08), set_edit(row, "author", std::string("Modder")),
	                        set_edit(row, "saved", std::string("10/9/2026, 8:01:02")),
	                        set_edit(child(face, 0, 0, kVertex), "uv_x", 0.25),
	                        set_edit(child(face, 2, 2, kGesture), "name", std::string("FEAR"))},
	                       error));
	grm::File file = written(face);
	TEST_EXPECT(file.base_texture == "textures/smile.bmp" && file.eye_size.y == 0.08f && file.saved.author == "Modder" &&
	            file.saved.month == 10 && file.saved.second == 2 && file.vertices[0].uv.x == 0.25f &&
	            file.gestures[2].name == "FEAR");
	// What the reader would read back as another name: a space, a comma, a quote, a % (its format), past its field.
	TEST_EXPECT(!face.apply(set_edit(row, "base_texture", std::string("my face.tga")), error));
	TEST_EXPECT(!face.apply(set_edit(row, "base_texture", std::string("100%.tga")), error));
	TEST_EXPECT(!face.apply(set_edit(child(face, 0, 1, kVertex), "group", std::string("a,b")), error));
	TEST_EXPECT(!face.apply(set_edit(child(face, 2, 0, kGesture), "name", std::string(32, 'G')), error));
	TEST_EXPECT(!face.apply(set_edit(row, "saved", std::string("yesterday")), error));
	TEST_EXPECT(!face.apply(set_edit(child(face, 1, 0, kTriangle), "a", int64_t(-1)), error));
	// A gesture and its two parameters in one batch.
	Edit gesture;
	gesture.operation = EditOperation::Add;
	gesture.address = {face_of(face).id, kGesture, 0};
	gesture.field = "name";
	gesture.value = std::string("ANGRY");
	Edit parameter;
	parameter.operation = EditOperation::Add;
	parameter.address = {face_of(face).id, kParameter, 0};
	parameter.parent = batch_made(0);
	parameter.field = "group";
	parameter.value = std::string("brow");
	Edit second = parameter;
	second.value = std::string("mouth");
	TEST_EXPECT(face.apply({gesture, parameter, second}, error));
	file = written(face);
	TEST_EXPECT(file.gestures.size() == 4 && file.gestures[3].name == "ANGRY" && file.gestures[3].parameters.size() == 2 &&
	            file.gestures[3].parameters[1].group == "mouth");
	// A gesture holds 32 parameters at the most.
	std::vector<Edit> many;
	for (int i = 0; i < 31; ++i) {
		Edit more = parameter;
		more.parent = child(face, 2, 3, kGesture).child;
		many.push_back(more);
	}
	TEST_EXPECT(!face.apply(many, error));
	// The face keeps its one row.
	Edit another;
	another.operation = EditOperation::Add;
	another.address = {0, kFace, 0};
	TEST_EXPECT(!face.apply(another, error));
	return 0;
}

// A triangle's corners name its vertices by index: a vertex moved, one added before them, one removed, the corners
// following; a vertex a triangle uses cannot be removed.
int test_corners_follow() {
	FaceAnimationDocument face;
	Diagnostic error;
	TEST_EXPECT(load(face, person(), error));
	// Vertex 3 moved to the front: the triangles' 0, 1, 2, 3 become 1, 2, 3, 0.
	Edit move;
	move.operation = EditOperation::Move;
	move.address = child(face, 0, 3, kVertex);
	move.position = 0;
	TEST_EXPECT(face.apply(move, error));
	grm::File file = written(face);
	TEST_EXPECT(file.vertices[0].group == "mouth" && file.vertices[1].group == "xxx");
	TEST_EXPECT((file.triangles[0] == std::array<int32_t, 3>{1, 2, 3}) && (file.triangles[1] == std::array<int32_t, 3>{1, 3, 0}));
	// A vertex added at the front: every corner one on.
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {face_of(face).id, kVertex, 0};
	add.position = 0;
	TEST_EXPECT(face.apply(add, error));
	file = written(face);
	TEST_EXPECT(file.vertices.size() == 5 && (file.triangles[1] == std::array<int32_t, 3>{2, 4, 1}));
	// Removed again: none uses it.
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = child(face, 0, 0, kVertex);
	TEST_EXPECT(face.apply(remove, error));
	file = written(face);
	TEST_EXPECT(file.vertices.size() == 4 && (file.triangles[1] == std::array<int32_t, 3>{1, 3, 0}));
	// A vertex a triangle uses: refused, nothing changed; undone, the file as it was.
	remove.address = child(face, 0, 2, kVertex);
	TEST_EXPECT(!face.apply(remove, error) && error.message.find("uses vertex 2") != std::string::npos);
	TEST_EXPECT(written(face).vertices.size() == 4);
	face.undo();
	face.undo();
	face.undo();
	const SerializeResult result = face.serialize();
	TEST_EXPECT(result.ok() && std::vector<uint8_t>(result.text.begin(), result.text.end()) == person());
	return 0;
}

// The type's findings: an eye texture named alone, a triangle past the vertices (the save refuses it), a gesture of
// no expression's name, two of one, a parameter of a group no vertex has, two of one group; a layout the writer
// writes otherwise; a name the writer cannot write back.
int test_findings() {
	FaceAnimationDocument face;
	Diagnostic error;
	TEST_EXPECT(load(face, person(), error));
	const NodeAddress row{face_of(face).id, kFace, 0};
	TEST_EXPECT(face.apply({set_edit(row, "eye_texture_2", std::string("")),
	                        set_edit(child(face, 1, 0, kTriangle), "c", int64_t(9)),
	                        set_edit(child(face, 2, 0, kGesture), "name", std::string("BLINK")),
	                        set_edit(child(face, 2, 2, kGesture), "name", std::string("happy"))},
	                       error));
	Edit stray;
	stray.operation = EditOperation::Add;
	stray.address = {face_of(face).id, kParameter, 0};
	stray.parent = child(face, 2, 1, kGesture).child;
	stray.field = "group";
	stray.value = std::string("chin");
	Edit again = stray;
	again.value = std::string("brow");
	TEST_EXPECT(face.apply({stray, again}, error));
	const std::vector<Diagnostic> findings = validate_face_animation_file(face);
	TEST_EXPECT(has_code(findings, "face_animation.eye_texture_alone"));
	TEST_EXPECT(has_code(findings, "face_animation.unserializable"));
	TEST_EXPECT(has_code(findings, "face_animation.gesture_unplayed"));
	TEST_EXPECT(has_code(findings, "face_animation.gesture_repeated"));
	TEST_EXPECT(has_code(findings, "face_animation.parameter_unmatched"));
	TEST_EXPECT(has_code(findings, "face_animation.parameter_repeated"));
	TEST_EXPECT(count_code(findings, "face_animation.parameter_unmatched") == 1);
	TEST_EXPECT(!face.serialize().ok());
	// A layout the writer writes otherwise (a comment, three decimals): listed, a save writes it the tool's way.
	const std::string text = "// a hand-written face\r\nbasetexture face.tga\r\nvertices 1\r\nvertex 0 0.5 0.5 xxx\r\n";
	FaceAnimationDocument hand;
	TEST_EXPECT(load(hand, std::vector<uint8_t>(text.begin(), text.end()), error));
	TEST_EXPECT(!hand.blocked() && has_code(validate_face_animation_file(hand), "face_animation.ignored_input"));
	// A quoted name holding a space: read, but the writer would write it bare: blocking.
	const std::string quoted = "basetexture \"my face.tga\"\r\n";
	FaceAnimationDocument spaced;
	TEST_EXPECT(load(spaced, std::vector<uint8_t>(quoted.begin(), quoted.end()), error));
	TEST_EXPECT(spaced.blocked() && has_code(validate_face_animation_file(spaced), "face_animation.invalid_input"));
	// A file the reader refuses (D-GRM-1): no document.
	const std::string unsafe = "vertices 1\r\nvertex 1 0 0 xxx\r\n";
	FaceAnimationDocument refused;
	TEST_EXPECT(!load(refused, std::vector<uint8_t>(unsafe.begin(), unsafe.end()), error));
	return 0;
}

// What the face names: each texture by its field, made .TGA as the stage loader opens it; the base's .MDT twin, a
// name the loader derives, no rename's site.
int test_references() {
	FaceAnimationDocument face;
	Diagnostic error;
	TEST_EXPECT(load(face, person(), error));
	const NodeAddress row{face_of(face).id, kFace, 0};
	TEST_EXPECT(face.apply(set_edit(row, "base_texture", std::string("faces\\smile.bmp")), error));
	Extracted out;
	extract_from_document(face, out);
	const GraphEdge *base = nullptr, *twin = nullptr, *eye = nullptr;
	for (const GraphEdge &edge : out.edges) {
		if (edge.field == "base_texture") base = &edge;
		if (edge.field == "base_texture.mdt") twin = &edge;
		if (edge.field == "eye_texture_1") eye = &edge;
	}
	TEST_EXPECT(base && base->kind == ReferenceKind::Texture && base->value == "faces\\smile.bmp" && base->rewritable);
	TEST_EXPECT(twin && twin->value == "smile.MDT" && !twin->rewritable);
	TEST_EXPECT(eye && eye->value == "iris.tga" && (eye->loader_arg & kTextureArgFaceTga) != 0);
	renderer::TextureRoleId role = renderer::TextureRoleId::kCount;
	TEST_EXPECT(base && texture_arg_role(base->loader_arg, role) && role == renderer::TextureRoleId::FaceTexture);
	// The file the loader opens for the name: its path stripped, its extension made .TGA.
	const auto exists = [](const std::string &name) { return strutil::iequals(name, "smile.TGA"); };
	TEST_EXPECT(base && texture_reference_load(base->value, base->loader_arg, exists).file == "smile.TGA");
	// The triangles' corners name the vertices, the file's record set.
	TEST_EXPECT(face.targeted_collections().size() == 1 &&
	            face.targeted_collections()[0].reference == ReferenceKind::FaceVertex);
	return 0;
}

// The blank: the writer's file of an empty face, every count line written, opened clean.
int test_blank() {
	BlankRequest request;
	request.logical_name = "newface.grm";
	std::vector<uint8_t> bytes;
	Diagnostic error;
	TEST_EXPECT(make_blank(request, AssetKind::FaceAnimation, bytes, error));
	FaceAnimationDocument blank;
	TEST_EXPECT(blank.load_bytes(bytes, "anims/newface.grm", AssetKind::FaceAnimation, "jo", error));
	TEST_EXPECT(blank.face_row() && blank.face_row()->file.vertices.empty() && validate_face_animation_file(blank).empty());
	TEST_EXPECT(std::string(asset_kind_row(AssetKind::FaceAnimation).new_name) == "newface.grm");
	TEST_EXPECT(document_type_for(AssetKind::FaceAnimation) &&
	            document_type_for(AssetKind::FaceAnimation)->id == DocumentTypeId::FaceAnimation);
	return 0;
}

// The retail leg: JO ships no face animation; the install's archives are scanned again, and any .grm they hold
// (an expansion's) is read through the document and written back.
int test_retail() {
	if (!retail::selected()) return 0;
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the install's archives, scanned for .grm files)");
		return 0;
	}
	size_t archives = 0, entries = 0, faces = 0;
	std::error_code ec;
	for (const auto &entry : std::filesystem::recursive_directory_iterator(std::filesystem::u8path(install), ec)) {
		std::string ext = entry.path().extension().u8string();
		for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
		if (ext != ".pff") continue;
		pff::PffArchive archive{};
		if (pff::pff_open(&archive, entry.path().u8string().c_str()) != 0) continue;
		++archives;
		for (uint32_t i = 0; i < archive.entry_count; ++i) {
			++entries;
			const pff::PffEntry &stored = archive.entries[i];
			const std::string name = pff::pff_entry_stored_name(stored);
			if (name.size() < 4 || !strutil::iequals(name.substr(name.size() - 4), ".grm")) continue;
			++faces;
			std::vector<uint8_t> bytes(stored.size);
			TEST_EXPECT(pff::pff_extract(&archive, &stored, bytes.data(), bytes.size()) == 0);
			FaceAnimationDocument face;
			Diagnostic error;
			TEST_EXPECT(face.load_bytes(bytes, name, AssetKind::FaceAnimation, "jo", error) && face.serialize().ok());
		}
		pff::pff_close(&archive);
	}
	TEST_EXPECT(archives >= 3 && entries > 1000);
	std::printf("retail: %zu archives, %zu entries, %zu face animations (JO ships none)\n", archives, entries, faces);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failed = 0;
	failed += test_reads_and_writes_back();
	failed += test_edits();
	failed += test_corners_follow();
	failed += test_findings();
	failed += test_references();
	failed += test_blank();
	failed += test_retail();
	if (failed == 0) std::printf("editor face animation: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
