// The face animation document (face_animation_document.h): the face's table over the engine's record (grm::File),
// its parse through the engine's reader and its save through the engine's writer from the row alone, and its
// findings. What the game does with each value is docs/world/world-wac-ai-re.md section 33.30's.
#include "face_animation_document.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/record_shift.h>
#include <editor/model/staged_rows.h>
#include <runtime/world/facial_animation.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kFace = node_kind(FaceAnimationKind::Face);
constexpr NodeKind kVertex = node_kind(FaceAnimationKind::Vertex);
constexpr NodeKind kTriangle = node_kind(FaceAnimationKind::Triangle);
constexpr NodeKind kGesture = node_kind(FaceAnimationKind::Gesture);
constexpr NodeKind kParameter = node_kind(FaceAnimationKind::Parameter);

using Triangle = std::array<int32_t, 3>;

// The bytes each name's field keeps, its terminator among them (grm.h's reader refuses a longer one, D-GRM-1): a
// texture's 260 [orig: FaceAnimConfig_ParseProperty @ 0x5886A0, the fields at +0, +260, +520], a vertex's and a
// parameter's group 31, a gesture's name 32, the author 256.
constexpr size_t kTextureBytes = 260, kGroupBytes = 31, kGestureBytes = 32, kAuthorBytes = 256;
// The parameters a gesture holds [orig: FaceAnimConfig_ParseProperty @ 0x5886A0, the parm array of 32].
constexpr size_t kParametersMost = 32;
// A triangle's corners' fields, in the tri line's order.
constexpr const char *kCornerIds[] = {"a", "b", "c"};

grm::File &face_of(const RecordHandle &r) { return r.as<grm::File>(); }
grm::Vertex &vertex_of(const RecordHandle &r) { return r.as<grm::Vertex>(); }
Triangle &triangle_of(const RecordHandle &r) { return r.as<Triangle>(); }
grm::Gesture &gesture_of(const RecordHandle &r) { return r.as<grm::Gesture>(); }
grm::Parameter &parameter_of(const RecordHandle &r) { return r.as<grm::Parameter>(); }

FieldSchema schema_of(const std::string &id, FieldType type, const char *label, const char *description) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.label = label;
	schema.description = description;
	return schema;
}

// What keeps a name from reading back as written: a character the reader splits a line at (a space, a comma, a
// tab, a quote; the writer writes a name bare [orig: the writer @ 0x588320]), one it reads as a format (it copies a
// name through sprintf, the name its format [orig: FaceAnimConfig_ParseProperty @ 0x5886A0]), a control character.
bool name_fault(const std::string &stored, std::string &why) {
	for (const char c : stored)
		if (c == ' ' || c == ',' || c == '\t' || c == '"' || c == '%' || static_cast<unsigned char>(c) < 0x20) {
			why = c == '%' ? "holds a %, which the game's reader reads as a format, not as part of the name"
			               : "holds a space, a comma, a tab, a quote or a control character, where the game's reader "
			                 "splits a line";
			return true;
		}
	return false;
}

bool set_name(std::string &field, size_t bytes, const char *what, const Value &value, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) {
		error = std::string(what) + " is a text.";
		return false;
	}
	std::string stored;
	if (!utf8_to_cp1252(*text, stored)) {
		error = "The game's text encoding (Windows-1252) has no character for part of this name.";
		return false;
	}
	if (stored.size() >= bytes) {
		error = std::string(what) + " holds at most " + std::to_string(bytes - 1) + " characters.";
		return false;
	}
	std::string why;
	if (name_fault(stored, why)) {
		error = std::string(what) + " " + why + ".";
		return false;
	}
	field = std::move(stored);
	return true;
}

bool number_of(const Value &value, double &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) {
		out = double(*whole);
		return true;
	}
	if (const auto *real = std::get_if<double>(&value)) {
		out = *real;
		return std::isfinite(out);
	}
	return false;
}

bool whole_of(const Value &value, int64_t &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) {
		out = *whole;
		return true;
	}
	if (const auto *real = std::get_if<double>(&value); real && std::isfinite(*real) && *real == std::floor(*real)) {
		out = int64_t(*real);
		return true;
	}
	return false;
}

// A coordinate the writer writes with four decimals [orig: the writer @ 0x588320], held as the float the reader
// makes of it (atof [orig: FaceAnimConfig_ParseProperty @ 0x5886A0]).
bool set_coordinate(float &field, const Value &value, std::string &error) {
	double number = 0.0;
	if (!number_of(value, number) || std::fabs(number) > 1.0e6) {
		error = "A coordinate is a number from -1000000 to 1000000 (the file writes four decimals).";
		return false;
	}
	field = static_cast<float>(number);
	return true;
}

// A point's two coordinates on one row (its `group`), the file's keyword their token.
template <class Record>
void point(TableKind &kind, const std::string &id, const char *keyword, const char *group, const char *description,
           grm::Point &(*at)(Record &record), const char *section = "") {
	static const char *const kAxes[] = {"x", "y"};
	for (int axis = 0; axis < 2; ++axis) {
		LabelledField field;
		field.schema = schema_of(id + "_" + kAxes[axis], FieldType::Real, axis == 0 ? "X" : "Y", description);
		field.schema.token = keyword;
		field.schema.group = group;
		field.schema.section = section;
		field.schema.step = 0.001;
		field.value.get = [at, axis](const RecordHandle &r, Value &out) {
			const grm::Point &p = at(r.as<Record>());
			return out = double(axis == 0 ? p.x : p.y), true;
		};
		field.value.set = [at, axis](const RecordHandle &r, const Value &v, std::string &e) {
			grm::Point &p = at(r.as<Record>());
			return set_coordinate(axis == 0 ? p.x : p.y, v, e);
		};
		kind.field(std::move(field));
	}
}

// A face's texture name (the base, an eye), as the file writes it.
LabelledField texture_field(const char *id, const char *label, const char *description,
                            std::string &(*name)(grm::File &file)) {
	FieldSchema schema = schema_of(id, FieldType::Text, label, description);
	schema.width = kTextureBytes;
	schema.code_page = true;
	schema.reference = ReferenceKind::Texture;
	schema.section = "Textures";
	return LabelledField{schema,
	                     {[name](const RecordHandle &r, Value &out) { return out = cp1252_to_utf8(name(face_of(r))), true; },
	                      [name](const RecordHandle &r, const Value &v, std::string &e) {
		                      return set_name(name(face_of(r)), kTextureBytes, "A texture's name", v, e);
	                      }}};
}

// A group name (a vertex's, a parameter's), as the file writes it.
LabelledField group_field(const char *description, std::string &(*name)(const RecordHandle &record)) {
	FieldSchema schema = schema_of("group", FieldType::Text, "Group", description);
	schema.width = kGroupBytes;
	schema.code_page = true;
	return LabelledField{schema,
	                     {[name](const RecordHandle &r, Value &out) { return out = cp1252_to_utf8(name(r)), true; },
	                      [name](const RecordHandle &r, const Value &v, std::string &e) {
		                      return set_name(name(r), kGroupBytes, "A group's name", v, e);
	                      }}};
}

// The save stamp's words, as the writer writes them ("10/9/2026, 14:05:09").
std::string stamp_words(const grm::SaveInfo &s) {
	char text[96];
	std::snprintf(text, sizeof(text), "%d/%d/%d, %d:%02d:%02d", s.month, s.day, s.year, s.hour, s.minute, s.second);
	return text;
}

bool set_stamp(grm::SaveInfo &s, const Value &value, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	grm::SaveInfo read = s;
	if (!text || std::sscanf(text->c_str(), "%d/%d/%d, %d:%d:%d", &read.month, &read.day, &read.year, &read.hour,
	                         &read.minute, &read.second) != 6) {
		error = "A save stamp reads month/day/year, hour:minute:second (10/9/2026, 14:05:09).";
		return false;
	}
	s = read;
	return true;
}

RecordTable make_table() {
	using RF = LabelledField;
	// --- the face -------------------------------------------------------------------------------
	TableKind face(RecordKindRow{kFace, "face", "Face", "", true});
	face.field(texture_field("base_texture", "Base texture",
	                         "The face's texture, which its triangles map: the game opens the name with its path stripped "
	                         "and its extension made .TGA, and its .MDT twin beside it, through the stage loader [orig: "
	                         "Shadow_DecalLoadTextures @ 0x588040, @ 0x5880EA, @ 0x588117].",
	                         [](grm::File &f) -> std::string & { return f.base_texture; }));
	face.field(texture_field("eye_texture_1", "First eye texture",
	                         "The first name of the eyetexture line (the face's +520), made .TGA as the base's is [orig: "
	                         "Shadow_DecalLoadTextures @ 0x58814A]. The line names both eyes or neither: the game reads a "
	                         "line naming the first alone as naming neither [orig: sub_587F20 @ 0x587F20].",
	                         [](grm::File &f) -> std::string & { return f.eye_textures[0]; }));
	face.field(texture_field("eye_texture_2", "Second eye texture",
	                         "The second name of the eyetexture line (the face's +260), made .TGA [orig: "
	                         "Shadow_DecalLoadTextures @ 0x588180].",
	                         [](grm::File &f) -> std::string & { return f.eye_textures[1]; }));
	point<grm::File>(face, "eye_size", "eyesize", "Eye size",
	                 "The eyes' size on the face's texture [orig: FaceAnimConfig_ParseProperty @ 0x5886A0, +864]; 0.04 x "
	                 "0.06 where the file says none [orig: FaceAnimConfig_InitEyeDefaults @ 0x588D20].",
	                 [](grm::File &f) -> grm::Point & { return f.eye_size; }, "Eyes");
	point<grm::File>(face, "eye1_center", "eye1center", "First eye's centre",
	                 "The first eye's centre on the face's texture [orig: FaceAnimConfig_ParseProperty @ 0x5886A0, +848]; "
	                 "0.35, 0.5 where the file says none [orig: FaceAnimConfig_InitEyeDefaults @ 0x588D20].",
	                 [](grm::File &f) -> grm::Point & { return f.eye_centers[0]; }, "Eyes");
	point<grm::File>(face, "eye2_center", "eye2center", "Second eye's centre",
	                 "The second eye's centre [orig: FaceAnimConfig_ParseProperty @ 0x5886A0, +856]; 0.65, 0.5 where the "
	                 "file says none [orig: FaceAnimConfig_InitEyeDefaults @ 0x588D20].",
	                 [](grm::File &f) -> grm::Point & { return f.eye_centers[1]; }, "Eyes");
	point<grm::File>(face, "eye_limits", "eyelimits", "Eye limits",
	                 "How far the eyes look [orig: FaceAnimConfig_ParseProperty @ 0x5886A0, +872]; 0.03, 0.01 where the "
	                 "file says none [orig: FaceAnimConfig_InitEyeDefaults @ 0x588D20]. The face's compositor does not "
	                 "read it.",
	                 [](grm::File &f) -> grm::Point & { return f.eye_limits; }, "Eyes");
	{
		FieldSchema saved = schema_of("saved", FieldType::Text, "Saved on",
				"The original tool's save stamp, month/day/year, hour:minute:second: a comment line the game's reader skips "
				"[orig: FaceAnimConfig_LoadFile @ 0x588BE0], written as held.");
		saved.section = "Save stamp";
		face.field(RF{saved,
		              {[](const RecordHandle &r, Value &out) { return out = stamp_words(face_of(r).saved), true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) { return set_stamp(face_of(r).saved, v, e); }}});
		FieldSchema author = schema_of("author", FieldType::Text, "Last edited by",
				"Who the original tool says last saved the face: a comment line the game's reader skips, written as held.");
		author.width = kAuthorBytes;
		author.code_page = true;
		author.section = "Save stamp";
		face.field(RF{author,
		              {[](const RecordHandle &r, Value &out) { return out = cp1252_to_utf8(face_of(r).saved.author), true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               const auto *text = std::get_if<std::string>(&v);
			               std::string stored;
			               if (!text || !utf8_to_cp1252(*text, stored) || stored.size() >= kAuthorBytes ||
			                   stored.find_first_of("\r\n") != std::string::npos) {
				               e = "The author is one line of at most 255 characters the game's encoding (Windows-1252) holds.";
				               return false;
			               }
			               face_of(r).saved.author = std::move(stored);
			               return true;
		               }}});
	}
	TableList vertices;
	vertices.spec = Document::CollectionSpec{kVertex, "Vertices", "group", false, Applicability::Reads, 0};
	vertices.spec.first_number = 0;
	vertices.ops = vector_list<grm::File, grm::Vertex>(kVertex, [](grm::File &f) -> std::vector<grm::Vertex> & {
		return f.vertices;
	});
	face.list(std::move(vertices));
	TableList triangles;
	triangles.spec = Document::CollectionSpec{kTriangle, "Triangles", "", false, Applicability::Reads, 0};
	triangles.spec.first_number = 0;
	triangles.ops = vector_list<grm::File, Triangle>(kTriangle, [](grm::File &f) -> std::vector<Triangle> & {
		return f.triangles;
	});
	face.list(std::move(triangles));
	TableList gestures;
	gestures.spec = Document::CollectionSpec{kGesture, "Gestures", "name", false, Applicability::Reads, 0};
	gestures.spec.first_number = 0;
	gestures.ops = vector_list<grm::File, grm::Gesture>(kGesture, [](grm::File &f) -> std::vector<grm::Gesture> & {
		return f.gestures;
	});
	face.list(std::move(gestures));

	// --- a vertex ---------------------------------------------------------------------------------
	TableKind vertex(RecordKindRow{kVertex, "vertex", "Vertex", "", false});
	point<grm::Vertex>(vertex, "uv", "vertex", "Position",
	                   "Where the vertex sits on the face's texture, 0 to 1 of its side; a gesture's offset of its group "
	                   "moves it [orig: sub_5890F0 @ 0x5890F0].",
	                   [](grm::Vertex &v) -> grm::Point & { return v.uv; });
	vertex.field(group_field("The group a gesture moves the vertex by: each gesture's first parameter of the group "
	                         "[orig: sub_589090 @ 0x589090]; xxx moves with none [orig: "
	                         "Model_CollectUniqueMaterialNames @ 0x588D90].",
	                         [](const RecordHandle &r) -> std::string & { return vertex_of(r).group; }));

	// --- a triangle --------------------------------------------------------------------------------
	TableKind triangle(RecordKindRow{kTriangle, "triangle", "Triangle", "", false});
	static const char *const kCornerLabels[] = {"First vertex", "Second vertex", "Third vertex"};
	for (int corner = 0; corner < 3; ++corner) {
		FieldSchema schema = schema_of(kCornerIds[corner], FieldType::Integer, kCornerLabels[corner],
				"A vertex of the face by its index: the game draws the triangle over its vertices as they stand, never "
				"testing the index [orig: Render_ScarDebugOverlay @ 0x589301..0x58931D].");
		schema.reference = ReferenceKind::FaceVertex;
		schema.group = "Vertices";
		schema.ranged = true;
		schema.min = 0;
		schema.max = double(INT32_MAX);
		triangle.field(RF{schema,
		                  {[corner](const RecordHandle &r, Value &out) { return out = int64_t(triangle_of(r)[size_t(corner)]), true; },
		                   [corner](const RecordHandle &r, const Value &v, std::string &e) {
			                   int64_t index = 0;
			                   if (!whole_of(v, index) || index < 0 || index > INT32_MAX) {
				                   e = "A triangle's corner is a vertex's index, a whole number from 0.";
				                   return false;
			                   }
			                   triangle_of(r)[size_t(corner)] = int32_t(index);
			                   return true;
		                   }}});
	}

	// --- a gesture ---------------------------------------------------------------------------------
	TableKind gesture(RecordKindRow{kGesture, "gesture", "Gesture", "", false});
	{
		FieldSchema name = schema_of("name", FieldType::Text, "Expression",
				"The expression the gesture is: the game plays NORMAL, HAPPY, SAD, SMIRK, ANGRY, SURPRISE, DISGUST, FEAR "
				"and AGGRESSIVE by name, without case [orig: AnimState_FindByName @ 0x5800B0, the table @ 0x7D7960], the "
				"last gesture of the name [orig: sub_588FE0 @ 0x588FE0].");
		name.width = kGestureBytes;
		name.code_page = true;
		for (int i = 0; i < 9; ++i) name.choices.push_back({world::kFacialExpressions[i], i, ""});
		name.open_choices = true;
		gesture.field(RF{name,
		                 {[](const RecordHandle &r, Value &out) { return out = cp1252_to_utf8(gesture_of(r).name), true; },
		                  [](const RecordHandle &r, const Value &v, std::string &e) {
			                  return set_name(gesture_of(r).name, kGestureBytes, "An expression's name", v, e);
		                  }}});
	}
	TableList parameters;
	parameters.spec = Document::CollectionSpec{kParameter, "Parameters", "group", false, Applicability::Reads,
	                                           kParametersMost};
	parameters.spec.first_number = 0;
	parameters.ops = vector_list<grm::Gesture, grm::Parameter>(kParameter,
			[](grm::Gesture &g) -> std::vector<grm::Parameter> & { return g.parameters; });
	gesture.list(std::move(parameters));

	// --- a parameter -------------------------------------------------------------------------------
	TableKind parameter(RecordKindRow{kParameter, "parameter", "Parameter", "", false});
	parameter.field(group_field("The group of vertices the parameter moves: the first parameter of a vertex's group in "
	                            "the gesture moves it [orig: sub_589090 @ 0x589090].",
	                            [](const RecordHandle &r) -> std::string & { return parameter_of(r).group; }));
	point<grm::Parameter>(parameter, "offset", "parm", "Offset",
	                      "How far the gesture moves its group's vertices on the face's texture, blended from one "
	                      "expression to the next [orig: sub_5890F0 @ 0x5890F0].",
	                      [](grm::Parameter &p) -> grm::Point & { return p.offset; });

	return RecordTable({std::move(face), std::move(vertex), std::move(triangle), std::move(gesture), std::move(parameter)});
}

const FaceAnimationRow &face_row_of(const Node &node) { return static_cast<const FaceAnimationRow &>(node); }

// The record's index in its list (the address's identity among the row's ids), npos for none.
size_t index_in(const RecordIds &ids, size_t list, NodeId id) {
	if (list >= ids.lists.size()) return SIZE_MAX;
	for (size_t i = 0; i < ids.lists[list].size(); ++i)
		if (ids.lists[list][i].id == id) return i;
	return SIZE_MAX;
}

constexpr size_t kVertexList = 0, kTriangleList = 1, kGestureList = 2;

} // namespace

const RecordTable &face_animation_table() {
	static const RecordTable table = make_table();
	return table;
}

bool is_face_animation_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::FaceAnimation; }

// --- the row -------------------------------------------------------------------------------------

FaceAnimationRow::FaceAnimationRow() { kind = kFace; }

RecordHandle FaceAnimationRow::record() const { return {kFace, const_cast<grm::File *>(&file)}; }

size_t FaceAnimationRow::footprint() const {
	size_t bytes = sizeof(*this) + footprint_of(file.base_texture) + footprint_of(file.eye_textures[0]) +
	               footprint_of(file.eye_textures[1]) + footprint_of(file.saved.author) + footprint_of(file.vertices) +
	               footprint_of(file.triangles) + footprint_of(file.gestures) + ids_footprint();
	for (const grm::Vertex &v : file.vertices) bytes += footprint_of(v.group);
	for (const grm::Gesture &g : file.gestures) {
		bytes += footprint_of(g.name) + footprint_of(g.parameters);
		for (const grm::Parameter &p : g.parameters) bytes += footprint_of(p.group);
	}
	return bytes;
}

// --- the document ----------------------------------------------------------------------------------

const FaceAnimationRow *FaceAnimationDocument::face_row() const {
	for (const auto &node : rows())
		if (node && node->kind == kFace) return &face_row_of(*node);
	return nullptr;
}

std::string FaceAnimationDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	if (!address.child) return "Face";
	const RecordHandle handle = record_in(*node, address);
	if (!handle) return record_name(address);
	const FaceAnimationRow &face = face_row_of(*node);
	switch (address.kind) {
	case kVertex: {
		const size_t at = index_in(face.ids, kVertexList, address.child);
		const std::string group = cp1252_to_utf8(vertex_of(handle).group);
		return "Vertex " + std::to_string(at) + (group.empty() ? std::string() : ": " + group);
	}
	case kTriangle: {
		const Triangle &t = triangle_of(handle);
		return "Triangle " + std::to_string(index_in(face.ids, kTriangleList, address.child)) + ": " + std::to_string(t[0]) +
		       ", " + std::to_string(t[1]) + ", " + std::to_string(t[2]);
	}
	case kGesture: {
		const std::string name = cp1252_to_utf8(gesture_of(handle).name);
		return name.empty() ? record_name(address) : name;
	}
	case kParameter: {
		const std::string group = cp1252_to_utf8(parameter_of(handle).group);
		return group.empty() ? record_name(address) : "Moves " + group;
	}
	default: break;
	}
	return record_name(address);
}

bool FaceAnimationDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                                  std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues, Diagnostic &error) {
	if (!is_face_animation_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a face animation.",
		                     path());
		return false;
	}
	auto row = std::make_shared<FaceAnimationRow>();
	std::string message;
	if (!grm::parse(bytes.data(), bytes.size(), row->file, message)) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error,
		                     "The face animation could not be read: " + message + ".", path());
		return false;
	}
	// A name the writer would not write back as the reader read it (a quoted one holding a separator, one holding a
	// %): the record holds it, the save cannot.
	std::string why;
	const auto name_issue = [&](const std::string &name, const std::string &record, const char *field) {
		if (!name_fault(name, why)) return;
		issues.push_back({true, 0, record, field,
		                  "The name '" + cp1252_to_utf8(name) + "' " + why +
		                          ": the writer writes a name bare, so it would read back as another. Rename it in the file."});
	};
	name_issue(row->file.base_texture, std::string(), "base_texture");
	name_issue(row->file.eye_textures[0], std::string(), "eye_texture_1");
	name_issue(row->file.eye_textures[1], std::string(), "eye_texture_2");
	for (size_t i = 0; i < row->file.vertices.size(); ++i)
		name_issue(row->file.vertices[i].group, "vertex " + std::to_string(i), "group");
	for (const grm::Gesture &g : row->file.gestures) {
		name_issue(g.name, cp1252_to_utf8(g.name), "name");
		for (const grm::Parameter &p : g.parameters) name_issue(p.group, cp1252_to_utf8(g.name), "group");
	}
	// The file's own layout: the writer writes the original tool's [orig: the writer @ 0x588320]; a file laid out
	// otherwise (its comments, a line the reader skips, more decimals than four) is written in it.
	std::vector<uint8_t> written;
	if (grm::write(row->file, written, message) && written != bytes)
		issues.push_back({false, 0, std::string(), std::string(),
		                  "The file is not laid out as the original tool writes a face: a save writes it so (its comments "
		                  "and the lines the game's reader skips left out, each coordinate with four decimals)."});
	shape(*row);
	rows.push_back(std::move(row));
	return true;
}

SerializeResult FaceAnimationDocument::serialize() const {
	SerializeResult result;
	const FaceAnimationRow *face = face_row();
	if (!face) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The face animation holds no face."});
		return result;
	}
	// The second eye texture alone would be written as a line the reader refuses (face_animation.unserializable).
	if (face->file.eye_textures[0].empty() && !face->file.eye_textures[1].empty()) {
		result.issues.push_back({true, 0, std::string(), std::string(),
		                         "The face names its second eye texture alone, a line the game reads otherwise: name the "
		                         "first too, or neither."});
		return result;
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!grm::write(face->file, bytes, error)) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The face animation could not be written: " + error + "."});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

std::string FaceAnimationDocument::save_words() const {
	return "A save writes the face in the original tool's layout: every count line, each coordinate with four decimals, "
	       "the save stamp as held.";
}

std::shared_ptr<Node> FaceAnimationDocument::make_node(NodeKind, NodeId, const std::vector<std::shared_ptr<const Node>> &,
                                                       std::string &error) {
	error = "A face animation keeps its one face; add vertices, triangles and gestures inside it.";
	return nullptr;
}

bool FaceAnimationDocument::accept_step(const EditStep &, const StagedRows &staged, StepRefusal &refusal) const {
	if (staged.rows().size() == 1) return true;
	refusal.message = "A face animation keeps its one face.";
	return false;
}

bool FaceAnimationDocument::accept_list_edit(const Node &, const ListChange &change, std::string &error) const {
	// The gesture a parameter goes into: an Add's or a Duplicate's owner, a Move's destination.
	const Located *into = change.operation == EditOperation::Move ? change.destination : change.owner;
	if (!into || !into->record || into->record.kind != kGesture) return true;
	if (change.operation == EditOperation::Move && change.owner && change.owner->record.data == into->record.data) return true;
	if (gesture_of(into->record).parameters.size() < kParametersMost) return true;
	error = "A gesture holds " + std::to_string(kParametersMost) + " parameters at the most [orig: "
	        "FaceAnimConfig_ParseProperty @ 0x5886A0, its parm array].";
	return false;
}

void FaceAnimationDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	TableDocument::refine_field(address, use);
	// Each texture name made .TGA, its path stripped, and opened by stage as a face's texture (kTextureArgFaceTga).
	if (use.reference == ReferenceKind::Texture)
		use.loader_arg = texture_role_arg(renderer::TextureRoleId::FaceTexture, kTextureArgFaceTga);
}

bool FaceAnimationDocument::renumber_references(const StagedRows &rows, const RecordShift &shift,
                                                std::vector<Edit> &sites, std::string &error) const {
	if (shift.reference != ReferenceKind::FaceVertex) return Document::renumber_references(rows, shift, sites, error);
	for (const std::shared_ptr<const Node> &node : rows.rows()) {
		if (!node || node->kind != kFace) continue;
		const FaceAnimationRow &face = face_row_of(*node);
		if (face.ids.lists.size() <= kTriangleList) continue;
		const std::vector<RecordIds> &ids = face.ids.lists[kTriangleList];
		for (size_t t = 0; t < face.file.triangles.size() && t < ids.size(); ++t)
			for (int corner = 0; corner < 3; ++corner) {
				const int64_t index = face.file.triangles[t][size_t(corner)];
				const size_t now = shift.now(index);
				if (now == size_t(index)) continue;
				if (now == RecordShift::kRemoved) {
					error = "Triangle " + std::to_string(t) + " uses vertex " + std::to_string(index) +
					        ": give it another vertex, or remove the triangle, first.";
					return false;
				}
				Edit set;
				set.address = {node->id, kTriangle, ids[t].id};
				set.field = kCornerIds[corner];
				set.value = int64_t(now);
				sites.push_back(std::move(set));
			}
	}
	return true;
}

// --- the references and the findings -------------------------------------------------------------------

void face_animation_references(const Document &document, Extracted &out) {
	const auto *face_document = dynamic_cast<const FaceAnimationDocument *>(&document);
	const FaceAnimationRow *face = face_document ? face_document->face_row() : nullptr;
	if (!face || face->file.base_texture.empty()) return;
	// The base texture's twin, which the stage loader opens beside its .TGA [orig: Shadow_DecalLoadTextures @
	// 0x588117]: derived from the name, never written, so no rename rewrites it.
	GraphEdge edge;
	edge.source = document.path();
	edge.address = {face->id, kFace, 0};
	edge.record = document.record_path(edge.address);
	edge.locator = document.locator(edge.address);
	edge.field = "base_texture.mdt";
	edge.kind = ReferenceKind::Texture;
	edge.value = cp1252_to_utf8(grm::texture_load_name(face->file.base_texture, grm::kTextureTwinExtension));
	edge.loader_arg = texture_role_arg(renderer::TextureRoleId::FaceTexture);
	edge.rewritable = false;
	out.edges.push_back(std::move(edge));
}

namespace {

constexpr FindingCodeEntry<FaceAnimationFinding> kFindingEntries[] = {
	{ FaceAnimationFinding::InvalidInput, { "face_animation.invalid_input", FindingFix::None, nullptr, true } },
	{ FaceAnimationFinding::IgnoredInput, { "face_animation.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput } },
	// The first eye texture named alone: the game reads the line as naming neither eye [orig: sub_587F20 @ 0x587F20];
	// it refuses nothing: listed. (The second named alone is face_animation.unserializable.)
	{ FaceAnimationFinding::EyeTextureAlone, listed_code("face_animation.eye_texture_alone") },
	{ FaceAnimationFinding::Unserializable, { "face_animation.unserializable", FindingFix::None, nullptr, true } },
	{ FaceAnimationFinding::GestureUnplayed, listed_code("face_animation.gesture_unplayed") },
	{ FaceAnimationFinding::GestureRepeated, listed_code("face_animation.gesture_repeated") },
	{ FaceAnimationFinding::ParameterUnmatched, listed_code("face_animation.parameter_unmatched") },
	{ FaceAnimationFinding::ParameterRepeated, listed_code("face_animation.parameter_repeated") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(FaceAnimationFinding::kCount),
              "every FaceAnimationFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
              "the face animation's rows follow FaceAnimationFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::FaceAnimations);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(FaceAnimationFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable face_animation_finding_codes() { return {kFindingRows.data(), kFindingRows.size()}; }

std::vector<Diagnostic> validate_face_animation_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *face_document = dynamic_cast<const FaceAnimationDocument *>(&document);
	if (!face_document) return findings;
	source_issue_findings(*face_document, finding_code(FaceAnimationFinding::InvalidInput),
	                      finding_code(FaceAnimationFinding::IgnoredInput), findings);
	if (document.blocked()) return findings;
	const FaceAnimationRow *face = face_document->face_row();
	if (!face) return findings;
	const grm::File &file = face->file;
	const auto add = [&](const NodeAddress &address, DiagnosticSeverity severity, FaceAnimationFinding code,
	                     const char *field, const std::string &message) {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		d.row_id = address.row;
		d.child_id = address.child;
		d.record_kind = address.kind;
		d.record = face_document->record_path(address);
		findings.push_back(std::move(d));
	};
	const auto child = [&](size_t list, size_t index, NodeKind kind) {
		const NodeId id = list < face->ids.lists.size() && index < face->ids.lists[list].size()
		                          ? face->ids.lists[list][index].id
		                          : 0;
		return NodeAddress{face->id, kind, id};
	};
	const NodeAddress at_face{face->id, kFace, 0};
	// The second eye texture named alone is written as the line's one name: the tokenizer collapses the separators
	// before it, so the game reads it as the first and the second from a token an earlier line left [orig:
	// FaceAnimConfig_TokenizeConfigLine @ 0x588AD0; sub_587F20 @ 0x587F20] (D-GRM-1's class), and the format's
	// reader refuses the line: the face is not saved so.
	if (file.eye_textures[0].empty() && !file.eye_textures[1].empty())
		add(at_face, DiagnosticSeverity::Error, FaceAnimationFinding::Unserializable, "eye_texture_1",
		    "The face names its second eye texture alone: the line is written with that one name, which the game reads "
		    "as the first eye's and the second from a token an earlier line left [orig: FaceAnimConfig_TokenizeConfigLine "
		    "@ 0x588AD0; sub_587F20 @ 0x587F20], so the face is not saved until it names the first too, or neither.");
	else if (!file.eye_textures[0].empty() && file.eye_textures[1].empty())
		add(at_face, DiagnosticSeverity::Warning, FaceAnimationFinding::EyeTextureAlone, "eye_texture_2",
		    "The face names its first eye texture alone: the game reads the line as naming neither eye [orig: "
		    "sub_587F20 @ 0x587F20]. Name both, or neither.");
	for (size_t t = 0; t < file.triangles.size(); ++t)
		for (int corner = 0; corner < 3; ++corner) {
			const int32_t index = file.triangles[t][size_t(corner)];
			if (index >= 0 && size_t(index) < file.vertices.size()) continue;
			add(child(kTriangleList, t, kTriangle), DiagnosticSeverity::Error, FaceAnimationFinding::Unserializable,
			    kCornerIds[corner],
			    "Triangle " + std::to_string(t) + " uses vertex " + std::to_string(index) + ", which the face has none of (" +
			            std::to_string(file.vertices.size()) +
			            " vertices): the game draws it over memory past them [orig: Render_ScarDebugOverlay @ "
			            "0x589301..0x58931D], so the face is not saved until it names one of them.");
		}
	std::unordered_map<std::string, size_t> expressions;
	std::unordered_map<std::string, bool> groups;
	for (const grm::Vertex &v : file.vertices) groups[strutil::to_lower(v.group)] = true;
	for (size_t g = 0; g < file.gestures.size(); ++g) {
		const grm::Gesture &gesture = file.gestures[g];
		const NodeAddress at = child(kGestureList, g, kGesture);
		const std::string name = cp1252_to_utf8(gesture.name);
		if (world::facial_expression_index(gesture.name) < 0)
			add(at, DiagnosticSeverity::Info, FaceAnimationFinding::GestureUnplayed, "name",
			    "No expression the game plays is named '" + name +
			            "' (NORMAL, HAPPY, SAD, SMIRK, ANGRY, SURPRISE, DISGUST, FEAR, AGGRESSIVE): the gesture is never "
			            "played [orig: AnimState_FindByName @ 0x5800B0].");
		else if (const auto earlier = expressions.find(strutil::to_lower(gesture.name)); earlier != expressions.end())
			add(child(kGestureList, earlier->second, kGesture), DiagnosticSeverity::Warning,
			    FaceAnimationFinding::GestureRepeated, "name",
			    "A later gesture is " + name + " too: the game plays the last gesture of a name [orig: sub_588FE0 @ "
			                                   "0x588FE0], so this one is never played.");
		expressions[strutil::to_lower(gesture.name)] = g;
		std::unordered_map<std::string, bool> seen;
		const std::vector<RecordIds> *parameter_ids =
				g < face->ids.lists[kGestureList].size() && !face->ids.lists[kGestureList][g].lists.empty()
						? &face->ids.lists[kGestureList][g].lists[0]
						: nullptr;
		for (size_t p = 0; p < gesture.parameters.size(); ++p) {
			const grm::Parameter &parameter = gesture.parameters[p];
			const NodeAddress at_parameter{face->id, kParameter,
			                               parameter_ids && p < parameter_ids->size() ? (*parameter_ids)[p].id : 0};
			const std::string key = strutil::to_lower(parameter.group);
			const std::string group = cp1252_to_utf8(parameter.group);
			if (!seen.emplace(key, true).second)
				add(at_parameter, DiagnosticSeverity::Warning, FaceAnimationFinding::ParameterRepeated, "group",
				    name + " moves the group " + group + " in an earlier parameter: a vertex takes the first of its group "
				                                         "[orig: sub_589090 @ 0x589090], so this one moves nothing.");
			else if (!groups.count(key) || strutil::iequals(parameter.group, "xxx"))
				add(at_parameter, DiagnosticSeverity::Info, FaceAnimationFinding::ParameterUnmatched, "group",
				    strutil::iequals(parameter.group, "xxx")
				            ? name + "'s parameter moves the group xxx, which no gesture moves [orig: "
				                     "Model_CollectUniqueMaterialNames @ 0x588D90]."
				            : name + "'s parameter moves the group " + group + ", which no vertex of the face is in.");
		}
	}
	return findings;
}

} // namespace opennova::editor
