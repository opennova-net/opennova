// ADR 0046 S14: what a drag in a mission's viewport writes (editor/preview/mission_handle_edit) and
// the mission camera (editor/preview/mission_camera), with no document type of the mission's: a
// record document of two kinds of row that carries the mission's field tokens (an entity's x, y, z
// and yaw; an area's six bounds), as the mission document does.
//
// The planners: a move on the plane with no device keeps z; on a fake ground, with stick, each
// entity keeps its height over the ground, without it z stands; a move snaps on the file's axes;
// three pressed records keep their offsets, an area moving its four x and y bounds; a sample within
// half a 16.16 step of what a row holds plans nothing; a lift snaps the grabbed one and lifts the
// rest as far, an area grabbed refused; a turn wraps 359 to 0 and snaps the grabbed one to 15, each
// about its own origin; an area's edge never passes its opposite, an entity's or a non-edge handle
// refused; the ground point from a camera's pixel, the device's terrain when it answers, else the
// plane. The camera: a mission point to the presentation frame and back; the wire's yaw the compass
// heading it looks along, its pitch the degrees it looks down, both against
// presentation_forward_from_angles; a look keeps the eye and turns the heading to the right; a fly
// moves eye and target together; the picture's middle meets the plane at the target; top is straight
// down, north up; the wire form round-trips, and refuses what it does not take.

#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <base/io/json.h>
#include <editor/model/document.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <runtime/world/presentation_frame.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;

namespace {

constexpr NodeKind kEntity = 0, kArea = 1;

struct MissionRow : Node {
	double x = 0.0, y = 0.0, z = 0.0;
	int64_t yaw = 0;
	double min[3] = { 0.0, 0.0, 0.0 }, max[3] = { 0.0, 0.0, 0.0 };
	std::shared_ptr<Node> clone() const override { return std::make_shared<MissionRow>(*this); }
	std::string name() const override { return kind == kEntity ? "entity" : "area"; }
	size_t footprint() const override { return sizeof(MissionRow); }
};

// "E x y z yaw" an entity, "A xmin xmax ymin ymax zmin zmax" an area, one a line.
class MissionRows : public Document {
public:
	const std::vector<RecordKindRow> &kinds() const override {
		static const std::vector<RecordKindRow> table = {
			{ kEntity, "entity", "Entity", "Add entity", true },
			{ kArea, "area", "Area", "Add area", true },
		};
		return table;
	}
	std::vector<Collection> collections(const Node &, const NodeAddress &) const override { return {}; }
	const std::vector<FieldSchema> &fields(NodeKind kind) const override {
		static const std::vector<FieldSchema> entity = { FieldSchema{ "x", FieldType::Real },
			FieldSchema{ "y", FieldType::Real }, FieldSchema{ "z", FieldType::Real },
			FieldSchema{ "yaw", FieldType::Integer } };
		static const std::vector<FieldSchema> area = { FieldSchema{ "x_min", FieldType::Real },
			FieldSchema{ "x_max", FieldType::Real }, FieldSchema{ "y_min", FieldType::Real },
			FieldSchema{ "y_max", FieldType::Real }, FieldSchema{ "z_min", FieldType::Real },
			FieldSchema{ "z_max", FieldType::Real } };
		static const std::vector<FieldSchema> none;
		return kind == kEntity ? entity : kind == kArea ? area : none;
	}
	SerializeResult serialize() const override {
		SerializeResult result;
		for (const auto &node : rows()) {
			const auto &row = static_cast<const MissionRow &>(*node);
			if (row.kind == kEntity)
				result.text += "E " + std::to_string(row.x) + " " + std::to_string(row.y) + " " + std::to_string(row.z) +
						" " + std::to_string(row.yaw) + "\n";
			else
				result.text += "A " + std::to_string(row.min[0]) + " " + std::to_string(row.max[0]) + " " +
						std::to_string(row.min[1]) + " " + std::to_string(row.max[1]) + " " + std::to_string(row.min[2]) +
						" " + std::to_string(row.max[2]) + "\n";
		}
		return result;
	}
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<MissionRows>(*this); }

protected:
	bool read(const Node &node, const NodeAddress &, const std::string &field, Value &out) const override {
		const auto &row = static_cast<const MissionRow &>(node);
		if (row.kind == kEntity) {
			if (field == "x") out = row.x;
			else if (field == "y") out = row.y;
			else if (field == "z") out = row.z;
			else if (field == "yaw") out = row.yaw;
			else return false;
			return true;
		}
		static const char *const names[] = { "x_min", "x_max", "y_min", "y_max", "z_min", "z_max" };
		for (int i = 0; i < 6; ++i)
			if (field == names[i]) {
				out = i % 2 == 0 ? row.min[i / 2] : row.max[i / 2];
				return true;
			}
		return false;
	}
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
			std::shared_ptr<const FileState> &, std::vector<SourceIssue> &, Diagnostic &error) override {
		std::istringstream in(std::string(bytes.begin(), bytes.end()));
		std::string tag;
		while (in >> tag) {
			auto row = std::make_shared<MissionRow>();
			bool read = false;
			if (tag == "E") {
				row->kind = kEntity;
				read = bool(in >> row->x >> row->y >> row->z >> row->yaw);
			} else if (tag == "A") {
				row->kind = kArea;
				read = bool(in >> row->min[0] >> row->max[0] >> row->min[1] >> row->max[1] >> row->min[2] >> row->max[2]);
			}
			if (!read) {
				error = editor_test::finding_of(DiagnosticSeverity::Error, "document.parse", "Not a row.", path());
				return false;
			}
			rows.push_back(row);
		}
		return true;
	}
	bool set_field(Node &node, const NodeAddress &, const std::string &field, const Value &value,
			std::string &error) override {
		auto &row = static_cast<MissionRow &>(node);
		const auto *real = std::get_if<double>(&value);
		const auto *whole = std::get_if<int64_t>(&value);
		if (row.kind == kEntity && real && field == "x") row.x = *real;
		else if (row.kind == kEntity && real && field == "y") row.y = *real;
		else if (row.kind == kEntity && real && field == "z") row.z = *real;
		else if (row.kind == kEntity && whole && field == "yaw") row.yaw = *whole;
		else if (row.kind == kArea && real && field.size() == 5 && field[1] == '_') {
			const int axis = field[0] - 'x';
			if (axis < 0 || axis > 2) {
				error = "Unknown field.";
				return false;
			}
			(field.substr(2) == "min" ? row.min : row.max)[axis] = *real;
		} else {
			error = "Unknown field.";
			return false;
		}
		return true;
	}
};

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// The document: two entities and an area.
//   entity 0 at (10, 20, 5) yaw 350; entity 1 at (13, 24, 1.5) yaw 90; area x 30..40, y -20..-10, z 0..8.
bool load_rows(MissionRows &document) {
	Diagnostic error;
	return document.load_bytes(bytes_of("E 10 20 5 350\nE 13 24 1.5 90\nA 30 40 -20 -10 0 8\n"), "rows.txt",
			AssetKind::Unknown, "jo", error);
}

NodeAddress row_at(const MissionRows &document, size_t i) {
	return NodeAddress{ document.rows()[i]->id, document.rows()[i]->kind, 0 };
}

// The records as a press finds them, from the document.
std::vector<MissionPressed> press_all(const MissionRows &document) {
	std::vector<MissionPressed> out;
	for (size_t i = 0; i < document.rows().size(); ++i) {
		const auto &row = static_cast<const MissionRow &>(*document.rows()[i]);
		MissionPressed pressed;
		pressed.record = row_at(document, i);
		pressed.area = row.kind == kArea;
		if (pressed.area) {
			for (int a = 0; a < 3; ++a) {
				pressed.min[a] = row.min[a];
				pressed.max[a] = row.max[a];
			}
			pressed.x = (row.min[0] + row.max[0]) * 0.5;
			pressed.y = (row.min[1] + row.max[1]) * 0.5;
			pressed.z = row.min[2];
		} else {
			pressed.x = row.x;
			pressed.y = row.y;
			pressed.z = row.z;
			pressed.yaw = int(row.yaw);
		}
		out.push_back(pressed);
	}
	return out;
}

// The value a batch sets on `record`'s `field` (NAN for none).
double set_of(const std::vector<Edit> &edits, const NodeAddress &record, const char *field) {
	for (const Edit &edit : edits)
		if (edit.operation == EditOperation::Set && edit.address == record && edit.field == field) {
			if (const auto *real = std::get_if<double>(&edit.value)) return *real;
			if (const auto *whole = std::get_if<int64_t>(&edit.value)) return double(*whole);
		}
	return std::nan("");
}

bool near(double a, double b, double slack = 1e-6) { return std::fabs(a - b) <= slack; }

int test_tokens() {
	MissionHandle handle = MissionHandle::Move;
	for (const char *token : { "move", "height", "yaw", "x_min", "x_max", "y_min", "y_max" }) {
		TEST_EXPECT(mission_handle_from_token(token, handle) && std::string(mission_handle_token(handle)) == token);
	}
	TEST_EXPECT(!mission_handle_from_token("place", handle) && !mission_handle_from_token("", handle));
	TEST_EXPECT(!mission_handle_is_edge(MissionHandle::Move) && !mission_handle_is_edge(MissionHandle::Yaw) &&
			mission_handle_is_edge(MissionHandle::XMin) && mission_handle_is_edge(MissionHandle::YMax));
	std::printf("test_tokens passed\n");
	return 0;
}

int test_move() {
	MissionRows document;
	TEST_EXPECT(load_rows(document));
	const std::vector<MissionPressed> pressed = press_all(document);
	const NodeAddress a = row_at(document, 0), b = row_at(document, 1), area = row_at(document, 2);
	std::vector<Edit> edits;
	// On the plane with no device: the grabbed one to the point, the others as far, z standing (no
	// Set of a z that stands); the area's four bounds moved.
	const double to[2] = { 10.5, -3.25 };
	TEST_EXPECT(mission_move_edits(document, pressed, 0, to, 0.0f, true, nullptr, 7, edits));
	TEST_EXPECT(edits.size() == 8 && near(set_of(edits, a, "x"), 10.5) && near(set_of(edits, a, "y"), -3.25) &&
			std::isnan(set_of(edits, a, "z")));
	TEST_EXPECT(near(set_of(edits, b, "x"), 13.5) && near(set_of(edits, b, "y"), 0.75) && std::isnan(set_of(edits, b, "z")));
	TEST_EXPECT(near(set_of(edits, area, "x_min"), 30.5) && near(set_of(edits, area, "x_max"), 40.5) &&
			near(set_of(edits, area, "y_min"), -43.25) && near(set_of(edits, area, "y_max"), -33.25) &&
			std::isnan(set_of(edits, area, "z_min")));
	for (const Edit &edit : edits) TEST_EXPECT(edit.gesture == 7 && edit.operation == EditOperation::Set);
	// Snapped on the file's axes: the grabbed one's point to the metre, the rest keeping their offsets.
	const double rough[2] = { 10.4, -3.3 };
	TEST_EXPECT(mission_move_edits(document, pressed, 1, rough, 1.0f, false, nullptr, 7, edits));
	TEST_EXPECT(near(set_of(edits, b, "x"), 10.0) && near(set_of(edits, b, "y"), -3.0) && near(set_of(edits, a, "x"), 7.0) &&
			near(set_of(edits, a, "y"), -7.0));
	// Grabbed past the pressed records, or none pressed: refused.
	TEST_EXPECT(!mission_move_edits(document, pressed, 3, to, 0.0f, false, nullptr, 7, edits) && edits.empty());
	TEST_EXPECT(!mission_move_edits(document, {}, 0, to, 0.0f, false, nullptr, 7, edits));
	// A sample whose 16.16 words are the rows' plans nothing: the same point, or one within a step
	// above it (the file's word truncates toward zero, bms::to_fixed_16_16: a step up stays within
	// a positive row's word and leaves a negative one's, so the entities alone, both positive).
	const double same[2] = { 10.0, 20.0 };
	TEST_EXPECT(mission_move_edits(document, pressed, 0, same, 0.0f, false, nullptr, 7, edits) && edits.empty());
	const std::vector<MissionPressed> entities = { pressed[0], pressed[1] };
	const double within[2] = { 10.0 + 1.0 / 262144.0, 20.0 + 1.0 / 262144.0 };
	TEST_EXPECT(mission_move_edits(document, entities, 0, within, 0.0f, false, nullptr, 7, edits) && edits.empty());
	// The area's negative bounds step to the next word toward zero: its two Sets alone.
	TEST_EXPECT(mission_move_edits(document, pressed, 0, within, 0.0f, false, nullptr, 7, edits) && edits.size() == 2);
	const double past[2] = { 10.0 + 1.5 / 65536.0, 20.0 };
	TEST_EXPECT(mission_move_edits(document, pressed, 0, past, 0.0f, false, nullptr, 7, edits) && edits.size() == 4);

	// On a fake ground (z = x / 2): with stick each entity keeps its height over the ground; without
	// it z stands; the area's z bounds stand either way.
	editor_test::FakeDevice device;
	device.ground = [](double x, double) { return 0.5 * x; };
	// Entity 0 stood 5 above ground 5 (height 0); at x 30 ground is 15: z 15. Entity 1 stood 1.5
	// over ground 6.5 (-5 below); at x 33 ground is 16.5: z 11.5.
	const double far[2] = { 30.0, 20.0 };
	TEST_EXPECT(mission_move_edits(document, pressed, 0, far, 0.0f, true, &device, 7, edits));
	TEST_EXPECT(near(set_of(edits, a, "x"), 30.0) && near(set_of(edits, a, "z"), 15.0) && near(set_of(edits, b, "z"), 11.5) &&
			std::isnan(set_of(edits, area, "z_min")) && std::isnan(set_of(edits, area, "z_max")));
	TEST_EXPECT(mission_move_edits(document, pressed, 0, far, 0.0f, false, &device, 7, edits));
	TEST_EXPECT(std::isnan(set_of(edits, a, "z")) && std::isnan(set_of(edits, b, "z")));
	// A device that answers no ground: z stands with stick.
	editor_test::FakeDevice blind;
	TEST_EXPECT(mission_move_edits(document, pressed, 0, far, 0.0f, true, &blind, 7, edits) && std::isnan(set_of(edits, a, "z")));
	std::printf("test_move passed\n");
	return 0;
}

int test_height_and_yaw() {
	MissionRows document;
	TEST_EXPECT(load_rows(document));
	const std::vector<MissionPressed> pressed = press_all(document);
	const NodeAddress a = row_at(document, 0), b = row_at(document, 1), area = row_at(document, 2);
	std::vector<Edit> edits;
	// A lift of 2.3 snapped to 0.5: the grabbed one from 5 to 7.5, the other as far (1.5 to 4), the
	// area untouched.
	TEST_EXPECT(mission_height_edits(document, pressed, 0, 2.3, 0.5f, 9, edits));
	TEST_EXPECT(edits.size() == 2 && near(set_of(edits, a, "z"), 7.5) && near(set_of(edits, b, "z"), 4.0) &&
			std::isnan(set_of(edits, area, "z_min")));
	TEST_EXPECT(mission_height_edits(document, pressed, 1, -1.5, 0.0f, 9, edits) && near(set_of(edits, b, "z"), 0.0) &&
			near(set_of(edits, a, "z"), 3.5));
	TEST_EXPECT(mission_height_edits(document, pressed, 0, 0.0, 0.0f, 9, edits) && edits.empty());
	TEST_EXPECT(!mission_height_edits(document, pressed, 2, 1.0, 0.0f, 9, edits) && edits.empty());
	// A turn: 350 by 20 wraps to 10, the other 90 to 110; snapped to 15 the grabbed one's 370 goes to
	// 375 (15), the other by the same 25 (115); a turn of less than half a degree plans nothing; a
	// turn the other way wraps below 0. Two entities turn together about their centre (S15: the
	// middle of their box, 11.5 east and 22 north), each position carried round it clockwise as a
	// compass heading turns, their heights standing; one alone turns about its own origin.
	const std::vector<MissionPressed> two = { pressed[0], pressed[1] };
	TEST_EXPECT(mission_yaw_edits(document, two, 0, 20.0, 0.0f, 9, edits));
	TEST_EXPECT(edits.size() == 6 && set_of(edits, a, "yaw") == 10.0 && set_of(edits, b, "yaw") == 110.0);
	const double c = std::cos(20.0 * 3.14159265358979323846 / 180.0), s = std::sin(20.0 * 3.14159265358979323846 / 180.0);
	TEST_EXPECT(near(set_of(edits, a, "x"), 11.5 + (-1.5 * c + -2.0 * s), 1e-9) &&
			near(set_of(edits, a, "y"), 22.0 + (-2.0 * c - -1.5 * s), 1e-9) &&
			near(set_of(edits, b, "x"), 11.5 + (1.5 * c + 2.0 * s), 1e-9) && near(set_of(edits, b, "y"), 22.0 + (2.0 * c - 1.5 * s), 1e-9) &&
			std::isnan(set_of(edits, a, "z")) && std::isnan(set_of(edits, b, "z")));
	for (const Edit &edit : edits) TEST_EXPECT(edit.gesture == 9);
	// A quarter turn clockwise: what lay south-west of the centre (1.5 west, 2 south) lies north-west
	// (2 west, 1.5 north).
	TEST_EXPECT(mission_yaw_edits(document, two, 0, 90.0, 0.0f, 9, edits) && set_of(edits, a, "yaw") == 80.0 &&
			near(set_of(edits, a, "x"), 9.5, 1e-9) && near(set_of(edits, a, "y"), 23.5, 1e-9));
	const std::vector<MissionPressed> alone = { pressed[0] };
	TEST_EXPECT(mission_yaw_edits(document, alone, 0, 20.0, 0.0f, 9, edits) && edits.size() == 1 &&
			std::holds_alternative<int64_t>(edits[0].value) && set_of(edits, a, "yaw") == 10.0);
	TEST_EXPECT(mission_yaw_edits(document, two, 0, 20.0, 15.0f, 9, edits) && set_of(edits, a, "yaw") == 15.0 &&
			set_of(edits, b, "yaw") == 115.0);
	TEST_EXPECT(mission_yaw_edits(document, two, 1, -100.0, 0.0f, 9, edits) && set_of(edits, b, "yaw") == 350.0 &&
			set_of(edits, a, "yaw") == 250.0);
	TEST_EXPECT(mission_yaw_edits(document, two, 0, 0.4, 0.0f, 9, edits) && edits.empty());
	TEST_EXPECT(!mission_yaw_edits(document, pressed, 2, 20.0, 0.0f, 9, edits));
	std::printf("test_height_and_yaw passed\n");
	return 0;
}

// A group turn carries a selected area (S15 review): the centre the middle of the box of the entities'
// positions and the area's middle; the area's middle carried round it, its box on the file's axes, its
// extents swapped by a quarter turn and kept by an eighth; one entity with no other turns alone. And
// what a cancelled drag writes: the pressed records' moved fields put back, nothing for what stands.
int test_group_turn_with_an_area() {
	MissionRows document;
	Diagnostic error;
	// The area 20 wide (x 30..50) and 10 deep (y -20..-10), its middle (40, -15).
	TEST_EXPECT(document.load_bytes(bytes_of("E 10 20 5 350\nE 13 24 1.5 90\nA 30 50 -20 -10 0 8\n"), "rows.txt",
			AssetKind::Unknown, "jo", error));
	const std::vector<MissionPressed> pressed = press_all(document);
	const NodeAddress a = row_at(document, 0), area = row_at(document, 2);
	// The centre: x 10..40, y -15..24, so (25, 4.5); one record has none.
	double centre[2] = { 0.0, 0.0 };
	TEST_EXPECT(mission_turn_centre(pressed, centre) && near(centre[0], 25.0) && near(centre[1], 4.5));
	TEST_EXPECT(!mission_turn_centre({ pressed[0] }, centre));
	// A quarter turn clockwise: (east, north) to (north, -east). The area's middle, 15 east and 19.5
	// south of the centre, goes to 19.5 west and 15 south: (5.5, -10.5); its extents swapped, 10 wide and
	// 20 deep.
	std::vector<Edit> edits;
	TEST_EXPECT(mission_yaw_edits(document, pressed, 0, 90.0, 0.0f, 9, edits));
	TEST_EXPECT(near(set_of(edits, area, "x_min"), 0.5, 1e-9) && near(set_of(edits, area, "x_max"), 10.5, 1e-9) &&
			near(set_of(edits, area, "y_min"), -20.5, 1e-9) && near(set_of(edits, area, "y_max"), -0.5, 1e-9));
	// The entity about the same centre: 15 west and 15.5 north of it goes to 15.5 east and 15 north.
	TEST_EXPECT(near(set_of(edits, a, "x"), 40.5, 1e-9) && near(set_of(edits, a, "y"), 19.5, 1e-9) &&
			set_of(edits, a, "yaw") == 80.0);
	// An eighth of a turn: the middle carried round, the box's extents as they were (20 by 10).
	TEST_EXPECT(mission_yaw_edits(document, pressed, 0, 45.0, 0.0f, 9, edits));
	TEST_EXPECT(near(set_of(edits, area, "x_max") - set_of(edits, area, "x_min"), 20.0, 1e-9) &&
			near(set_of(edits, area, "y_max") - set_of(edits, area, "y_min"), 10.0, 1e-9));
	// Restored as pressed: a record whose press differs from the row has its moved fields written back.
	std::vector<MissionPressed> moved = pressed;
	moved[0].x = 11.0;
	moved[2].min[1] = -21.0;
	mission_restore_edits(document, pressed, 4, edits);
	TEST_EXPECT(edits.empty());
	mission_restore_edits(document, moved, 4, edits);
	TEST_EXPECT(edits.size() == 2 && near(set_of(edits, a, "x"), 11.0) && near(set_of(edits, area, "y_min"), -21.0) &&
			edits[0].gesture == 4);
	std::printf("test_group_turn_with_an_area passed\n");
	return 0;
}

int test_area_edges() {
	MissionRows document;
	TEST_EXPECT(load_rows(document));
	const std::vector<MissionPressed> pressed = press_all(document);
	const NodeAddress area = row_at(document, 2);
	std::vector<Edit> edits;
	TEST_EXPECT(mission_area_edge_edits(document, pressed[2], MissionHandle::XMin, 32.2, 0.0f, 3, edits));
	TEST_EXPECT(edits.size() == 1 && near(set_of(edits, area, "x_min"), 32.2) && edits[0].gesture == 3);
	// Snapped to 5, 32.2 is 30: what the row holds, so nothing is planned; 33 is 35.
	TEST_EXPECT(mission_area_edge_edits(document, pressed[2], MissionHandle::XMin, 32.2, 5.0f, 3, edits) && edits.empty());
	TEST_EXPECT(mission_area_edge_edits(document, pressed[2], MissionHandle::XMin, 33.0, 5.0f, 3, edits) &&
			near(set_of(edits, area, "x_min"), 35.0));
	// Never past its opposite edge: x_min to 50 stops at x_max 40, y_max to -100 at y_min -20.
	TEST_EXPECT(mission_area_edge_edits(document, pressed[2], MissionHandle::XMin, 50.0, 0.0f, 3, edits) &&
			near(set_of(edits, area, "x_min"), 40.0));
	TEST_EXPECT(mission_area_edge_edits(document, pressed[2], MissionHandle::YMax, -100.0, 0.0f, 3, edits) &&
			near(set_of(edits, area, "y_max"), -20.0));
	TEST_EXPECT(mission_area_edge_edits(document, pressed[2], MissionHandle::XMax, 41.0, 0.0f, 3, edits) &&
			near(set_of(edits, area, "x_max"), 41.0));
	TEST_EXPECT(mission_area_edge_edits(document, pressed[2], MissionHandle::YMin, -20.0, 0.0f, 3, edits) && edits.empty());
	// An entity has no edge; a move is no edge.
	TEST_EXPECT(!mission_area_edge_edits(document, pressed[0], MissionHandle::XMin, 1.0, 0.0f, 3, edits));
	TEST_EXPECT(!mission_area_edge_edits(document, pressed[2], MissionHandle::Move, 1.0, 0.0f, 3, edits));
	std::printf("test_area_edges passed\n");
	return 0;
}

int test_camera() {
	const double mission[3] = { 12.0, 34.0, 5.5 };
	const PreviewVec3 preview = mission_to_preview(mission);
	TEST_EXPECT(preview.x == 12.0f && preview.y == 5.5f && preview.z == -34.0f);
	double back[3];
	preview_to_mission(preview, back);
	TEST_EXPECT(near(back[0], 12.0) && near(back[1], 34.0) && near(back[2], 5.5));

	// The wire's yaw is the heading the camera looks along, its pitch the degrees it looks down:
	// the forward of presentation_forward_from_angles(heading, -pitch) is the camera's.
	OrbitCamera camera;
	camera.target = mission_to_preview(mission);
	camera.distance = 50.0f;
	for (const float yaw : { 0.0f, 0.7f, -2.3f, 4.0f }) {
		camera.yaw = yaw;
		camera.pitch = 0.4f;
		const double heading = mission_camera_heading(camera), down = mission_camera_pitch(camera);
		TEST_EXPECT(heading >= 0.0 && heading < 360.0 && near(down, 0.4 * 180.0 / 3.14159265358979, 1e-4));
		float forward[3];
		opennova::world::presentation_forward_from_angles(float(heading), float(-down), forward);
		PreviewVec3 right, up, camera_back;
		camera.axes(right, up, camera_back);
		TEST_EXPECT(near(forward[0], -camera_back.x, 1e-4) && near(forward[1], -camera_back.y, 1e-4) &&
				near(forward[2], -camera_back.z, 1e-4));
	}
	camera.yaw = 0.0f;
	TEST_EXPECT(near(mission_camera_heading(camera), 0.0));
	camera.yaw = -3.14159265f / 2.0f; // looking east
	TEST_EXPECT(near(mission_camera_heading(camera), 90.0, 1e-3));

	// A look keeps the eye and turns the heading to the right; a fly moves eye and target together.
	camera.yaw = 0.3f;
	camera.pitch = 0.5f;
	const PreviewVec3 eye = camera.eye();
	const double heading = mission_camera_heading(camera);
	mission_camera_look(camera, 40.0f, -10.0f);
	const PreviewVec3 after = camera.eye();
	TEST_EXPECT(near(after.x, eye.x, 1e-3) && near(after.y, eye.y, 1e-3) && near(after.z, eye.z, 1e-3));
	TEST_EXPECT(near(mission_camera_heading(camera), heading + 40.0 * kMissionLookRadiansPerPixel * 180.0 / 3.14159265358979, 1e-3));
	TEST_EXPECT(near(camera.pitch, 0.5f - 10.0f * kMissionLookRadiansPerPixel, 1e-5) && near(camera.distance, 50.0, 1e-4));
	const PreviewVec3 target = camera.target;
	PreviewVec3 right, up, camera_back;
	camera.axes(right, up, camera_back);
	mission_camera_fly(camera, 2.0f, 3.0f, 5.0f);
	TEST_EXPECT(near(camera.target.x, target.x + 2.0f * right.x - 5.0f * camera_back.x, 1e-4) &&
			near(camera.target.y, target.y + 2.0f * right.y + 3.0f - 5.0f * camera_back.y, 1e-4) &&
			near(camera.target.z, target.z + 2.0f * right.z - 5.0f * camera_back.z, 1e-4));
	const PreviewVec3 flown = camera.eye();
	TEST_EXPECT(near(flown.x - after.x, camera.target.x - target.x, 1e-3) && near(flown.y - after.y, camera.target.y - target.y, 1e-3));

	// The picture's middle meets the plane at the target's height at the target; a pixel above the
	// horizon meets nothing; top looks straight down, north up.
	camera.target = mission_to_preview(mission);
	camera.yaw = 0.9f;
	camera.pitch = 0.6f;
	double hit[3];
	TEST_EXPECT(mission_camera_on_height(camera, 320.0f, 240.0f, 640, 480, 5.5, hit));
	TEST_EXPECT(near(hit[0], 12.0, 1e-3) && near(hit[1], 34.0, 1e-3) && near(hit[2], 5.5));
	TEST_EXPECT(!mission_camera_on_height(camera, 320.0f, -5000.0f, 640, 480, 5.5, hit));
	TEST_EXPECT(!mission_camera_on_height(camera, 320.0f, 240.0f, 0, 480, 5.5, hit));
	mission_camera_top(camera);
	TEST_EXPECT(near(mission_camera_heading(camera), 0.0) && camera.pitch == kOrbitPitchLimit);

	// The ground point: with no device the plane; with a device that answers, its terrain.
	SessionView view;
	PreviewClock clock;
	ViewportContext plane{ ViewportInput{ view, clock, nullptr, ChangeClass::None }, 640, 480, 0.0f, nullptr };
	camera.yaw = 0.2f;
	camera.pitch = 0.7f;
	bool on_terrain = true;
	TEST_EXPECT(mission_ground_point(plane, camera, 320.0f, 240.0f, 5.5, hit, &on_terrain) && !on_terrain &&
			near(hit[0], 12.0, 1e-3) && near(hit[1], 34.0, 1e-3) && near(hit[2], 5.5));
	editor_test::FakeDevice device;
	device.ground = [](double, double) { return 20.0; }; // a flat ground above the target
	ViewportContext ground{ ViewportInput{ view, clock, nullptr, ChangeClass::None }, 640, 480, 0.0f, &device };
	TEST_EXPECT(mission_ground_point(ground, camera, 320.0f, 240.0f, 5.5, hit, &on_terrain) && on_terrain && near(hit[2], 20.0, 1e-3));
	// Where the ray meets the ground is on the ray toward the target: nearer the eye than the target.
	double eye_m[3];
	preview_to_mission(camera.eye(), eye_m);
	const double to_hit = std::hypot(hit[0] - eye_m[0], hit[1] - eye_m[1]), to_target = std::hypot(12.0 - eye_m[0], 34.0 - eye_m[1]);
	TEST_EXPECT(to_hit < to_target && to_hit > 0.0);
	editor_test::FakeDevice blind;
	ViewportContext none{ ViewportInput{ view, clock, nullptr, ChangeClass::None }, 640, 480, 0.0f, &blind };
	TEST_EXPECT(mission_ground_point(none, camera, 320.0f, 240.0f, 5.5, hit, &on_terrain) && !on_terrain && near(hit[2], 5.5));

	// The wire form: every member, read back equal; a member it does not take, a bad value, refused
	// with the camera as it was.
	camera.target = mission_to_preview(mission);
	camera.yaw = 0.3f;
	camera.pitch = 0.35f;
	camera.distance = 120.0f;
	const opennova::io::JsonValue json = mission_camera_to_json(camera);
	TEST_EXPECT(json.get("target") && json.get("eye") && json.get("fov") && near(json.get_number("distance", 0), 120.0) &&
			near(json.get_number("fov", 0), double(OrbitCamera::fov_horizontal_degrees())));
	opennova::io::JsonValue change = opennova::io::JsonValue::make_object();
	for (const char *key : { "target", "yaw", "pitch", "distance" }) change.set(key, *json.get(key));
	OrbitCamera read;
	std::string error;
	TEST_EXPECT(mission_camera_from_json(change, read, error) && error.empty());
	TEST_EXPECT(near(read.target.x, camera.target.x, 1e-4) && near(read.target.z, camera.target.z, 1e-4) &&
			near(mission_camera_heading(read), mission_camera_heading(camera), 1e-4) && near(read.pitch, camera.pitch, 1e-5) &&
			near(read.distance, 120.0, 1e-4));
	const auto refused = [&](const char *text, const char *says) {
		opennova::io::JsonValue bad;
		std::string parse_error;
		opennova::io::json_parse(text, bad, parse_error);
		OrbitCamera held = read;
		const bool ok = !mission_camera_from_json(bad, held, error) && error.find(says) != std::string::npos &&
				held.yaw == read.yaw && held.distance == read.distance;
		if (!ok) std::printf("  %s: %s\n", text, error.c_str());
		return ok;
	};
	TEST_EXPECT(refused(R"({"eye": [1, 2, 3]})", "Unknown camera member"));
	TEST_EXPECT(refused(R"({"fov": 90})", "Unknown camera member"));
	TEST_EXPECT(refused(R"({"distance": 0})", "more than 0"));
	TEST_EXPECT(refused(R"({"pitch": 120})", "camera.pitch"));
	TEST_EXPECT(refused(R"({"target": [1, 2]})", "camera.target"));
	TEST_EXPECT(refused(R"({"yaw": "north"})", "camera.yaw"));
	TEST_EXPECT(refused("[]", "object"));
	// A heading past 360 wraps; the limit pitch holds.
	opennova::io::JsonValue wrapped;
	opennova::io::json_parse(R"({"yaw": 450, "pitch": 88})", wrapped, error);
	TEST_EXPECT(mission_camera_from_json(wrapped, read, error) && near(mission_camera_heading(read), 90.0, 1e-3) &&
			read.pitch <= kOrbitPitchLimit);
	std::printf("test_camera passed\n");
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_tokens() == 0);
	TEST_EXPECT(test_move() == 0);
	TEST_EXPECT(test_height_and_yaw() == 0);
	TEST_EXPECT(test_group_turn_with_an_area() == 0);
	TEST_EXPECT(test_area_edges() == 0);
	TEST_EXPECT(test_camera() == 0);
	std::printf("editor_mission_handle_edit: all tests passed\n");
	return 0;
}
