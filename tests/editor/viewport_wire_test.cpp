// S13 V7 (ADR 0046 S13): edits in a viewport over the wire, edit_in_viewport through a real session's
// JSON requests, as the editor MCP raises them. Its drags and commands: refused as they are read (ok
// false) or as they are served (viewport.refused, nothing written); a drag one batch under one gesture
// over every selected window it moves, a command one request. The wire's gestures: consecutive drags
// of one handle on a document, their batches one undo step; a sample's `by` goes on from where the
// gesture's samples took the handle, so a snapped gesture lands where a canvas's would; a token no
// gesture of the wire's holds on the document refused (a forged one, one another request ended);
// another request on the document, another gesture, its document closing, a refused last sample and
// 10 s with no sample each end one, a request on another document none. The menu's `by` rounded, its
// `to` over a selection; a model's drag that moves nothing writes nothing, its frame of a record that
// is no marker refused, a frame beside any operation and a drag refused while one holds the
// documents. The clock set with no document named, whatever is active. A model read at a size a
// canvas set: its markers' pixels and its hit agree.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

using editor_test::FakeDevices;
using editor_test::NoProcess;

std::string synth(const char *name) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/" + name;
}

// MAIN over the whole design, BOX, OTHER and the 12-unit TINY inside it, every edge written.
const char *const kLayoutMenu = "<SCREEN>\r\n"
								"\t<NAME>LAYOUT</NAME>\r\n"
								"\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
								"\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP>"
								"<RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
								"\t\t<WINDOW type=\"window\" name=\"BOX\">\r\n"
								"\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP>"
								"<RIGHT>300</RIGHT><BOTTOM>200</BOTTOM></POSITION>\r\n"
								"\t\t</WINDOW>\r\n"
								"\t\t<WINDOW type=\"checkbox\" name=\"OTHER\">\r\n"
								"\t\t\t<POSITION><LEFT>400</LEFT><TOP>300</TOP>"
								"<RIGHT>600</RIGHT><BOTTOM>400</BOTTOM></POSITION>\r\n"
								"\t\t</WINDOW>\r\n"
								"\t\t<WINDOW type=\"checkbox\" name=\"TINY\">\r\n"
								"\t\t\t<POSITION><LEFT>600</LEFT><TOP>104</TOP>"
								"<RIGHT>612</RIGHT><BOTTOM>116</BOTTOM></POSITION>\r\n"
								"\t\t</WINDOW>\r\n"
								"\t</WINDOW>\r\n"
								"</SCREEN>\r\n";

JsonValue parse(const std::string &text) {
	JsonValue json;
	std::string error;
	opennova::io::json_parse(text, json, error);
	return json;
}

// A whole-number field of a record (-1: none).
int64_t field_of(const Document &document, const NodeAddress &address, const char *field) {
	Value value;
	return document.get(address, field, value) && std::holds_alternative<int64_t>(value) ? std::get<int64_t>(value)
																						  : -1;
}

NodeAddress named(const Document &document, const char *name) {
	NodeAddress address;
	find_definition(AssetGraph(), document, name, address);
	return address;
}

// A window's four edges: left, top, right, bottom.
std::vector<int64_t> edges(const Document &document, const NodeAddress &window) {
	return { field_of(document, window, "position.left"), field_of(document, window, "position.top"),
		field_of(document, window, "position.right"), field_of(document, window, "position.bottom") };
}

bool done(const JsonValue &answer) {
	const JsonValue *outcome = answer.get("outcome");
	return answer.get_bool("ok", false) && outcome && outcome->get_bool("done", false);
}

// A request read and refused as it was served: viewport.refused, its message saying `says`.
bool refused(const JsonValue &answer, const std::string &says) {
	const JsonValue *outcome = answer.get("outcome");
	const JsonValue *findings = outcome ? outcome->get("findings") : nullptr;
	const bool ok = answer.get_bool("ok", false) && outcome && !outcome->get_bool("done", true) && findings &&
			findings->array.size() == 1 && findings->array[0].get_string("code", "") == "viewport.refused" &&
			findings->array[0].get_string("message", "").find(says) != std::string::npos;
	if (!ok) std::printf("  refusal: %s\n", opennova::io::json_write(answer).c_str());
	return ok;
}

uint64_t gesture_of(const JsonValue &answer) {
	const JsonValue *outcome = answer.get("outcome");
	return outcome ? uint64_t(outcome->get_number("gesture", 0)) : 0;
}

// A session over a new project with layout.mnu (kLayoutMenu) and models/armory.3di, the menu open.
struct Wired {
	editor_test::TempProjectDir dir;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	Document *menu = nullptr;
	NodeAddress box, other, tiny;
	std::string id_box, id_other, id_tiny;

	Wired(const char *name, ProcessPlatform &platform) : dir(name), session(platform, preferences) {
		session.handle(request::new_project(dir.file("project"), "Wire"));
		session.run_operations();
		const std::string root = session.view().project.root;
		editor_test::write_text(root + "/layout.mnu", kLayoutMenu);
		editor_test::write_bytes(root + "/models/armory.3di", test_io::read_file(synth("armory.3di")));
		session.handle(request::rescan());
		session.run_operations();
		session.handle(request::open_document("layout.mnu"));
		menu = session.document_for("layout.mnu");
		if (!menu) return;
		box = named(*menu, "BOX");
		other = named(*menu, "OTHER");
		tiny = named(*menu, "TINY");
		id_box = std::to_string(box.child);
		id_other = std::to_string(other.child);
		id_tiny = std::to_string(tiny.child);
	}
	// A request as the editor MCP raises it, its operation run to its end.
	JsonValue wire(const std::string &text) {
		const JsonValue answer = session.handle_json(parse(text));
		session.run_operations();
		return answer;
	}
	// A drag of the menu's window `id` (`members` the rest of its object), on the menu.
	JsonValue drag(const std::string &id, const std::string &members) {
		return wire(R"({"kind": "edit_in_viewport", "path": ")" + menu->path() + R"(", "drag": {"id": )" + id +
				", " + members + "}}");
	}
	// The gestures open, and whether one of the wire's is open in the menu.
	size_t open_gestures() const { return session.view().documents.gestures.size(); }
	bool wire_open() const { return session.view().documents.gesture_in(menu->path()).wire(); }
	// The menu's undo steps, taken back to clean: how many there were.
	int undo_all() {
		int steps = 0;
		while (menu->can_undo() && steps < 16) {
			session.handle(request::undo(menu->path()));
			++steps;
		}
		return steps;
	}
};

} // namespace

// An edit in a viewport over the wire: a drag the reader refuses is not read (ok false: by and to
// both, neither, a member a drag does not take, a command with no name), and nothing is asked of the
// session; one read and refused is not done, viewport.refused naming why (neither a drag nor a
// command, both, an unknown handle, a record the viewport does not show, a command it has not),
// nothing written. A drag of four samples under one gesture (the first's answer names it, the last
// ends it) is one undo step; a drag of one of three selected windows moves the three in one batch; a
// drag to a point puts the handle there; while an operation holds the documents it is refused,
// nothing written. A command over three windows is one request, one undo step. A model's marker
// dragged by pixels lands where a drag to that pixel puts it; its frame command moves the camera and
// makes no undo step.
static int test_edits_in_viewport() {
	NoProcess platform;
	Wired wired("opennova_editor_viewport_wire_edits", platform);
	ProjectSession &session = wired.session;
	const SessionView &view = session.view();
	Document *menu = wired.menu;
	TEST_EXPECT(menu != nullptr && wired.box.child && wired.other.child && wired.tiny.child);
	if (!menu) return 1;
	const NodeAddress box = wired.box, other = wired.other, tiny = wired.tiny;
	const std::string &id_box = wired.id_box, &id_other = wired.id_other, &id_tiny = wired.id_tiny;
	const auto wire = [&wired](const std::string &text) { return wired.wire(text); };
	const uint64_t untouched = menu->revision();

	// Not read: nothing asked of the session.
	for (const std::string &unread : {
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move", "by": [8, 0], "to": [1, 1]}})",
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move"}})",
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move", "by": [8, 0], "colour": 1}})",
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move", "by": [8]}})",
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move", "by": [1e300, 0]}})",
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move", "by": [8, 0], "kind": "map"}})",
				 std::string(R"({"kind": "edit_in_viewport", "command": {"ids": [1]}})"),
				 std::string(R"({"kind": "edit_in_viewport", "viewport": {}})") }) {
		const JsonValue answer = wire(unread);
		TEST_EXPECT(!answer.get_bool("ok", true) && !answer.get_string("error", "").empty());
	}
	TEST_EXPECT(menu->revision() == untouched && !menu->dirty());
	// Read, and refused as it is served: viewport.refused, nothing written.
	TEST_EXPECT(refused(wire(R"({"kind": "edit_in_viewport"})"), "one of them"));
	TEST_EXPECT(refused(wire(R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box +
							R"(, "handle": "move", "by": [1, 1]}, "command": {"name": "align_left"}})"),
			"one of them"));
	TEST_EXPECT(refused(wire(R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box +
							R"(, "handle": "middle", "by": [1, 1]}})"),
			"Unknown handle"));
	TEST_EXPECT(refused(wire(R"({"kind": "edit_in_viewport", "drag": {"id": 999999, "handle": "move", "by": [1, 1]}})"),
			"no window of the screen"));
	TEST_EXPECT(refused(wire(R"({"kind": "edit_in_viewport", "command": {"name": "align_middle", "ids": [)" + id_box +
							", " + id_other + "]}}"),
			"Unknown menu command"));
	TEST_EXPECT(refused(wire(R"({"kind": "edit_in_viewport", "path": "nosuch.mnu", "command": {"name": "align_left"}})"),
			"No document is open at nosuch.mnu"));
	// A kind that does not show the document: refused, naming it.
	TEST_EXPECT(refused(wire(R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box +
							R"(, "handle": "move", "by": [1, 1], "kind": "model"}})"),
			"does not show in a model viewport"));
	TEST_EXPECT(menu->revision() == untouched && !menu->dirty());

	// A drag of four samples, one gesture: the first names it, the last ends it; one undo step.
	session.handle(request::select_record(menu->path(), box));
	uint64_t gesture = 0;
	for (int sample = 0; sample < 4; ++sample) {
		std::string drag = R"({"id": )" + id_box + R"(, "handle": "move", "by": [10, 5])";
		if (gesture) drag += R"(, "gesture": )" + std::to_string(gesture);
		if (sample < 3) drag += R"(, "end": false)";
		const JsonValue answer = wire(R"({"kind": "edit_in_viewport", "drag": )" + drag + "}");
		TEST_EXPECT(done(answer));
		const uint64_t named_gesture = gesture_of(answer);
		TEST_EXPECT(named_gesture != 0 && (!gesture || named_gesture == gesture));
		gesture = named_gesture;
	}
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 140, 120, 340, 220 }) && menu->dirty());
	TEST_EXPECT(wired.open_gestures() == 0);
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 100, 100, 300, 200 }) && !menu->dirty() &&
			!menu->can_undo());

	// Three windows selected, BOX dragged: the three move in one batch, one undo step.
	session.handle(request::select_record(menu->path(), box, SelectMode::Replace, { box, other, tiny }));
	TEST_EXPECT(done(wire(R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box +
			R"(, "handle": "move", "by": [8, 8]}})")));
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 108, 108, 308, 208 }));
	TEST_EXPECT(edges(*menu, other) == (std::vector<int64_t>{ 408, 308, 608, 408 }));
	TEST_EXPECT(edges(*menu, tiny) == (std::vector<int64_t>{ 608, 112, 620, 124 }));
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!menu->dirty() && !menu->can_undo());

	// To a point: OTHER's bottom right corner to (650, 450), its left and top kept.
	TEST_EXPECT(done(wire(R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_other +
			R"(, "handle": "bottom_right", "to": [650, 450]}})")));
	TEST_EXPECT(edges(*menu, other) == (std::vector<int64_t>{ 400, 300, 650, 450 }));
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!menu->dirty());

	// An operation holding the documents: the drag refused (the viewport plans no edit the session
	// does not take), nothing written.
	TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	TEST_EXPECT(refused(session.handle_json(parse(R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box +
								R"(, "handle": "move", "by": [8, 8]}})")),
			"an operation holds the documents"));
	TEST_EXPECT(!menu->dirty());
	session.run_operations();

	// A command over three windows: one request (the planner's one batch), one undo step.
	{
		const ViewportModel *viewport = session.viewports().find(menu->path(), ViewportKind::Menu);
		TEST_EXPECT(viewport != nullptr);
		if (!viewport) return 1;
		editor_test::Gathered planned;
		std::string error;
		TEST_EXPECT(viewport->command(viewport_context(view, *viewport), "align_left",
				{ box.child, other.child, tiny.child }, planned, error));
		TEST_EXPECT(planned.requests.size() == 1 && planned.requests[0].kind == EditorRequestKind::EditRecord);
	}
	const uint64_t entries = session.handle_entries();
	const JsonValue aligned = wire(R"({"kind": "edit_in_viewport", "command": {"name": "align_left", "ids": [)" + id_box +
			", " + id_other + ", " + id_tiny + "]}}");
	TEST_EXPECT(done(aligned) && !aligned.get("outcome")->get("gesture") && session.handle_entries() == entries + 1);
	TEST_EXPECT(field_of(*menu, other, "position.left") == 100 && field_of(*menu, tiny, "position.left") == 100);
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!menu->dirty() && !menu->can_undo() && field_of(*menu, other, "position.left") == 400);

	// A model's marker by pixels, then to the pixel it reached: the same place; the frame command.
	session.handle(request::open_document("models/armory.3di"));
	session.run_operations();
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && document->model_row() && !document->model_row()->ids.lists[3].empty());
	if (!document || !document->model_row() || document->model_row()->ids.lists[3].empty()) return 1;
	const ModelRow &row = *document->model_row();
	const NodeAddress point{ row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][0].id };
	const auto place = [&]() {
		std::vector<double> at;
		for (const char *field : { "position.x", "position.y", "position.z" }) {
			Value value;
			at.push_back(document->get(point, field, value) ? std::get<double>(value) : 0.0);
		}
		return at;
	};
	const auto pixel = [&](float &x, float &y) {
		std::string error;
		const ViewportModel *found = session.viewports().resolve(view, document->path(), ViewportKind::kCount, error);
		const auto *model = static_cast<const ModelViewport *>(found);
		if (!model) return false;
		for (const ModelOverlay &overlay : model->overlays(session.viewports().clock()))
			if (overlay.kind == ModelOverlayKind::UserPoint && overlay.index == 0)
				return model->camera().project(overlay.at, model->size().width, model->size().height, x, y);
		return false;
	};
	const std::vector<double> was = place();
	float x = 0.0f, y = 0.0f;
	TEST_EXPECT(pixel(x, y));
	const std::string id_point = std::to_string(point.child);
	const JsonValue by = wire(R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_point +
			R"(, "handle": "place", "by": [24, -12]}})");
	TEST_EXPECT(done(by) && gesture_of(by) != 0 && place() != was);
	float moved_x = 0.0f, moved_y = 0.0f;
	TEST_EXPECT(pixel(moved_x, moved_y) && std::fabs(moved_x - (x + 24.0f)) < 0.5f && std::fabs(moved_y - (y - 12.0f)) < 0.5f);
	const std::vector<double> by_place = place();
	session.handle(request::undo(document->path()));
	TEST_EXPECT(place() == was && !document->dirty());
	char to[96];
	std::snprintf(to, sizeof(to), "[%.6f, %.6f]", double(x + 24.0f), double(y - 12.0f));
	TEST_EXPECT(done(wire(R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_point +
			R"(, "handle": "place", "to": )" + to + "}}")));
	const std::vector<double> to_place = place();
	for (size_t i = 0; i < 3; ++i) TEST_EXPECT(std::fabs(to_place[i] - by_place[i]) < 1e-3);
	session.handle(request::undo(document->path()));
	TEST_EXPECT(place() == was && !document->dirty() && !document->can_undo());
	// The frame command: the camera looks at the whole model again; no document step.
	const auto *model = static_cast<const ModelViewport *>(session.viewports().find(document->path(), ViewportKind::Model));
	TEST_EXPECT(model != nullptr);
	if (!model) return 1;
	const float framed = model->camera().distance;
	session.handle(request::set_viewport(document->path(), R"({"camera": {"distance": 5000}})"));
	TEST_EXPECT(model->camera().distance == 5000.0f);
	const uint64_t revision = document->revision();
	TEST_EXPECT(done(wire(R"({"kind": "edit_in_viewport", "command": {"name": "frame"}})")));
	TEST_EXPECT(std::fabs(model->camera().distance - framed) < 1e-3f && document->revision() == revision &&
			!document->can_undo());
	std::printf("test_edits_in_viewport passed\n");
	return 0;
}

// The wire's gestures over the menu. A snapped gesture of four samples by 3 goes from where its
// samples took the handle: BOX on 112, where a canvas's drag lands (each sample taken from the
// window's place now would stop at 104), one undo step. A refused sample that keeps the gesture
// open leaves it open; a refused last sample ends it, its answer naming it. Two gestures interleaved:
// each begun ends the other's, whose next sample is refused, two undo steps. A forged token (one a
// batch of the record form carried, one no gesture holds) refused, folding into nothing. Another
// request on the document ends the gesture, one on another document does not; its document closing
// ends it. A first sample that moves nothing opens the gesture all the same.
static int test_wire_gestures() {
	NoProcess platform;
	Wired wired("opennova_editor_viewport_wire_gestures", platform);
	ProjectSession &session = wired.session;
	Document *menu = wired.menu;
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const NodeAddress box = wired.box, other = wired.other, tiny = wired.tiny;
	const std::string &id_box = wired.id_box, &id_other = wired.id_other, &id_tiny = wired.id_tiny;
	const auto named_gesture = [](uint64_t token) { return R"(, "gesture": )" + std::to_string(token); };
	session.handle(request::select_record(menu->path(), box));

	// Four samples of 3 on the grid: 103 (104), 106 (104), 109 (112), 112 (112); the top snapped
	// once (104).
	uint64_t gesture = 0;
	for (int sample = 0; sample < 4; ++sample) {
		const JsonValue answer = wired.drag(id_box, R"("handle": "move", "by": [3, 0], "snap": 1)" +
				(gesture ? named_gesture(gesture) : std::string()) + (sample < 3 ? R"(, "end": false)" : ""));
		TEST_EXPECT(done(answer));
		TEST_EXPECT(gesture_of(answer) != 0 && (!gesture || gesture_of(answer) == gesture));
		gesture = gesture_of(answer);
		TEST_EXPECT(wired.wire_open() == (sample < 3));
	}
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 112, 104, 312, 204 }));
	TEST_EXPECT(wired.open_gestures() == 0 && wired.undo_all() == 1 &&
			edges(*menu, box) == (std::vector<int64_t>{ 100, 100, 300, 200 }));

	// A refused sample that keeps the gesture open (another handle than its own) leaves it open; a
	// refused last one ends it, naming it.
	JsonValue answer = wired.drag(id_box, R"("handle": "right", "by": [10, 0], "end": false)");
	gesture = gesture_of(answer);
	TEST_EXPECT(done(answer) && gesture != 0 && field_of(*menu, box, "position.right") == 310 && wired.wire_open());
	answer = wired.drag(id_box, R"("handle": "move", "by": [10, 0], "end": false)" + named_gesture(gesture));
	TEST_EXPECT(refused(answer, "drags record " + id_box + "'s right handle") && gesture_of(answer) == gesture &&
			wired.wire_open());
	answer = wired.drag(id_box, R"("handle": "move", "by": [10, 0])" + named_gesture(gesture));
	TEST_EXPECT(refused(answer, "right handle") && gesture_of(answer) == gesture && !wired.wire_open() &&
			wired.open_gestures() == 0);
	TEST_EXPECT(refused(wired.drag(id_box, R"("handle": "right", "by": [10, 0])" + named_gesture(gesture)),
			"No open gesture " + std::to_string(gesture)));
	TEST_EXPECT(field_of(*menu, box, "position.right") == 310 && wired.undo_all() == 1);

	// Interleaved: B's first sample ends A, A's next ends B and is refused, B's next is refused.
	const JsonValue first_a = wired.drag(id_box, R"("handle": "move", "by": [8, 0], "end": false)");
	const JsonValue first_b = wired.drag(id_other, R"("handle": "move", "by": [0, 8], "end": false)");
	const uint64_t a = gesture_of(first_a), b = gesture_of(first_b);
	TEST_EXPECT(done(first_a) && done(first_b) && a && b && a != b);
	TEST_EXPECT(refused(wired.drag(id_box, R"("handle": "move", "by": [8, 0], "end": false)" + named_gesture(a)),
			"No open gesture " + std::to_string(a)));
	TEST_EXPECT(refused(wired.drag(id_other, R"("handle": "move", "by": [0, 8], "end": false)" + named_gesture(b)),
			"No open gesture " + std::to_string(b)));
	TEST_EXPECT(field_of(*menu, box, "position.left") == 108 && field_of(*menu, other, "position.top") == 308);
	TEST_EXPECT(wired.open_gestures() == 0 && wired.undo_all() == 2);

	// Forged: the token a record batch's gesture carried (open in the document, but no drag of the
	// wire's), and one no gesture holds; neither folds into anything.
	TEST_EXPECT(done(wired.wire(R"({"kind": "edit_record", "path": ")" + menu->path() + R"(", "edits": [{"op": "set", "id": )" +
			id_tiny + R"(, "field": "position.left", "value": 601, "gesture": 4242}]})")));
	TEST_EXPECT(wired.open_gestures() == 1 && !wired.wire_open());
	TEST_EXPECT(refused(wired.drag(id_tiny, R"("handle": "move", "by": [1, 0], "end": false)" + named_gesture(4242)),
			"No open gesture 4242"));
	TEST_EXPECT(refused(wired.drag(id_tiny, R"("handle": "move", "by": [1, 0])" + named_gesture(999999)),
			"No open gesture 999999"));
	TEST_EXPECT(field_of(*menu, tiny, "position.left") == 601);
	session.handle(request::end_edit(menu->path()));
	TEST_EXPECT(wired.open_gestures() == 0 && wired.undo_all() == 1);

	// Another document's request leaves the gesture open; one on its document ends it.
	answer = wired.drag(id_box, R"("handle": "move", "by": [8, 0], "end": false)");
	gesture = gesture_of(answer);
	TEST_EXPECT(done(answer) && wired.wire_open());
	TEST_EXPECT(done(wired.wire(R"({"kind": "open_document", "path": "models/armory.3di"})")) && wired.wire_open());
	TEST_EXPECT(done(wired.wire(R"({"kind": "set_viewport", "path": "models/armory.3di", "viewport": {"camera": {"yaw": 1.0}}})")) &&
			wired.wire_open());
	TEST_EXPECT(done(wired.drag(id_box, R"("handle": "move", "by": [8, 0], "end": false)" + named_gesture(gesture))));
	TEST_EXPECT(done(wired.wire(R"({"kind": "select_record", "path": ")" + menu->path() + R"(", "address": {"row": )" +
			std::to_string(box.row) + R"(, "kind": )" + std::to_string(box.kind) + R"(, "child": )" + id_box + "}}")));
	TEST_EXPECT(!wired.wire_open() && wired.open_gestures() == 0);
	TEST_EXPECT(refused(wired.drag(id_box, R"("handle": "move", "by": [8, 0])" + named_gesture(gesture)),
			"No open gesture"));
	TEST_EXPECT(field_of(*menu, box, "position.left") == 116 && wired.undo_all() == 1);

	// A first sample that moves nothing opens the gesture (its token named, nothing written); its
	// document closing ends it.
	answer = wired.drag(id_box, R"("handle": "move", "by": [0, 0], "end": false)");
	gesture = gesture_of(answer);
	TEST_EXPECT(done(answer) && gesture != 0 && wired.wire_open() && !menu->dirty() && !menu->can_undo());
	const std::string path = menu->path();
	session.handle(request::close_document(path));
	TEST_EXPECT(session.outcome().done() && wired.open_gestures() == 0 && !session.document_for(path));
	session.handle(request::open_document(path));
	wired.menu = menu = session.document_for(path);
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const NodeAddress reread = named(*menu, "BOX");
	TEST_EXPECT(refused(wired.drag(std::to_string(reread.child), R"("handle": "move", "by": [8, 0])" + named_gesture(gesture)),
			"No open gesture " + std::to_string(gesture)));
	TEST_EXPECT(!menu->dirty());
	(void)id_other;
	std::printf("test_wire_gestures passed\n");
	return 0;
}

// A client that went away: its gesture ends 10 s after its last sample (the platform's clock, at the
// poll), the Problems' validation no longer waiting on it; its next sample refused.
static int test_gesture_lapses() {
	editor_test::FakePlatform platform;
	Wired wired("opennova_editor_viewport_wire_lapses", platform);
	ProjectSession &session = wired.session;
	Document *menu = wired.menu;
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	JsonValue answer = wired.drag(wired.id_box, R"("handle": "move", "by": [8, 0], "end": false)");
	const uint64_t gesture = gesture_of(answer);
	TEST_EXPECT(done(answer) && gesture != 0 && wired.wire_open());
	const std::string named = R"(, "gesture": )" + std::to_string(gesture);
	platform.clock += kWireGestureLapseMs - 1;
	session.poll();
	TEST_EXPECT(wired.wire_open());
	TEST_EXPECT(done(wired.drag(wired.id_box, R"("handle": "move", "by": [8, 0], "end": false)" + named)));
	platform.clock += kWireGestureLapseMs - 1;
	session.poll();
	TEST_EXPECT(wired.wire_open());
	platform.clock += 1;
	session.poll();
	TEST_EXPECT(!wired.wire_open() && wired.open_gestures() == 0);
	TEST_EXPECT(refused(wired.drag(wired.id_box, R"("handle": "move", "by": [8, 0])" + named), "No open gesture"));
	TEST_EXPECT(field_of(*menu, wired.box, "position.left") == 116 && wired.undo_all() == 1);
	std::printf("test_gesture_lapses passed\n");
	return 0;
}

// The menu's drags: `by` rounded to the nearest design unit; `to` over a selection, the lead's handle
// to the point and the others moved as far.
static int test_menu_drags() {
	NoProcess platform;
	Wired wired("opennova_editor_viewport_wire_menu", platform);
	ProjectSession &session = wired.session;
	Document *menu = wired.menu;
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	TEST_EXPECT(done(wired.drag(wired.id_box, R"("handle": "move", "by": [-8.7, 4.6])")));
	TEST_EXPECT(edges(*menu, wired.box) == (std::vector<int64_t>{ 91, 105, 291, 205 }) && wired.undo_all() == 1);
	session.handle(request::select_record(menu->path(), wired.box, SelectMode::Replace, { wired.box, wired.other, wired.tiny }));
	TEST_EXPECT(done(wired.drag(wired.id_box, R"("handle": "move", "to": [150, 130])")));
	TEST_EXPECT(edges(*menu, wired.box) == (std::vector<int64_t>{ 150, 130, 350, 230 }));
	TEST_EXPECT(edges(*menu, wired.other) == (std::vector<int64_t>{ 450, 330, 650, 430 }));
	TEST_EXPECT(edges(*menu, wired.tiny) == (std::vector<int64_t>{ 650, 134, 662, 146 }));
	TEST_EXPECT(wired.undo_all() == 1);
	std::printf("test_menu_drags passed\n");
	return 0;
}

// A model's edits over the wire: a drag by nothing writes nothing (the document clean, no gesture
// named); its frame of a record that is no marker refused; a frame beside an operation that holds
// the documents and beside a Rescan's refresh, done, where a drag is refused.
static int test_model_edits() {
	NoProcess platform;
	Wired wired("opennova_editor_viewport_wire_model", platform);
	ProjectSession &session = wired.session;
	session.handle(request::open_document("models/armory.3di"));
	session.run_operations();
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && document->model_row() && !document->model_row()->ids.lists[3].empty());
	if (!document || !document->model_row() || document->model_row()->ids.lists[3].empty()) return 1;
	const ModelRow &row = *document->model_row();
	const std::string id_point = std::to_string(row.ids.lists[3][0].id);
	const std::string drag = R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_point + R"(, "handle": "place", "by": )";
	const uint64_t revision = document->revision();
	const JsonValue still = wired.wire(drag + "[0, 0]}}");
	TEST_EXPECT(done(still) && gesture_of(still) == 0 && document->revision() == revision && !document->dirty());
	TEST_EXPECT(refused(wired.wire(R"({"kind": "edit_in_viewport", "command": {"name": "frame", "ids": [999999]}})"),
			"Record 999999 is no marker"));
	const auto *model = static_cast<const ModelViewport *>(session.viewports().find(document->path(), ViewportKind::Model));
	TEST_EXPECT(model != nullptr);
	if (!model) return 1;
	const auto far_off = [&]() {
		session.handle(request::set_viewport(document->path(), R"({"camera": {"distance": 5000}})"));
		return model->camera().distance == 5000.0f;
	};
	// Beside an operation that holds the documents: the frame is a set_viewport, which runs beside
	// any; the drag is refused, nothing written.
	TEST_EXPECT(far_off());
	TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	TEST_EXPECT(done(session.handle_json(parse(R"({"kind": "edit_in_viewport", "command": {"name": "frame"}})"))) &&
			model->camera().distance < 5000.0f);
	TEST_EXPECT(refused(session.handle_json(parse(drag + "[24, -12]}}")), "an operation holds the documents"));
	TEST_EXPECT(document->revision() == revision);
	session.run_operations();
	// Beside a Rescan's refresh.
	TEST_EXPECT(far_off());
	session.handle(request::rescan());
	TEST_EXPECT(session.view().activity.operation.running());
	TEST_EXPECT(done(session.handle_json(parse(R"({"kind": "edit_in_viewport", "command": {"name": "frame"}})"))) &&
			model->camera().distance < 5000.0f);
	session.run_operations();
	std::printf("test_model_edits passed\n");
	return 0;
}

// The preview clock set with no document named: a change of the clock alone, pathless, sets it
// whatever is active (a stylesheet, which shows in no viewport; nothing at all); one with another
// member beside it names the active document's viewport, refused for a stylesheet (naming the types
// that show in one, or the kind it names).
static int test_pathless_clock() {
	NoProcess platform;
	Wired wired("opennova_editor_viewport_wire_clock", platform);
	ProjectSession &session = wired.session;
	editor_test::create_missing_files(session);
	session.handle(request::open_document("menu_style.mns"));
	TEST_EXPECT(done(wired.wire(R"({"kind": "set_viewport", "viewport": {"clock": {"playing": false, "time_ms": 120}}})")));
	TEST_EXPECT(!session.viewports().clock().playing() && session.viewports().clock().ms() == 120);
	TEST_EXPECT(refused(wired.wire(R"({"kind": "set_viewport", "viewport": {"device": {"width": 640}, "clock": {"time_ms": 40}}})"),
			"shows in no viewport (a menu, a model, an animation or an animation map does)"));
	TEST_EXPECT(refused(wired.wire(R"({"kind": "set_viewport", "viewport": {"kind": "menu", "clock": {"time_ms": 40}}})"),
			"does not show in a menu viewport"));
	TEST_EXPECT(session.viewports().clock().ms() == 120);
	TEST_EXPECT(refused(wired.wire(R"({"kind": "set_viewport", "viewport": {"clock": {"rate": -1}}})"), "clock.rate"));
	std::vector<std::string> open;
	for (const auto &document : session.view().documents.open) open.push_back(document->path());
	for (const std::string &path : open) session.handle(request::close_document(path));
	TEST_EXPECT(!open.empty() && session.view().documents.open.empty());
	TEST_EXPECT(done(wired.wire(R"({"kind": "set_viewport", "viewport": {"clock": {"ticks": 8}}})")) &&
			session.viewports().clock().ticks() == 8);
	std::printf("test_pathless_clock passed\n");
	return 0;
}

// A model read at a size a canvas set (the device's report: 640 x 360 against its state's 800 x
// 600): the pixel the items give a marker is where the hit finds it.
static int test_canvas_sized_reads() {
	NoProcess platform;
	Wired wired("opennova_editor_viewport_wire_sized", platform);
	ProjectSession &session = wired.session;
	FakeDevices devices;
	session.handle(request::open_document("models/armory.3di"));
	session.run_operations();
	devices.sync(session);
	editor_test::FakeDevice *device = devices.held("models/armory.3di", ViewportKind::Model);
	TEST_EXPECT(device != nullptr);
	if (!device) return 1;
	ViewportPicture picture;
	picture.width = 640;
	picture.height = 360;
	picture.canvas_sized = true;
	device->draw(picture);
	devices.sync(session);
	const auto ask = [&session](const std::string &args) {
		std::string error;
		const JsonValue answer = session.query("viewport", parse(args), error);
		if (!error.empty()) std::printf("  viewport %s: %s\n", args.c_str(), error.c_str());
		return answer;
	};
	const JsonValue state = ask(R"({"op": "state", "limit": 1})");
	const JsonValue *size = state.get("device");
	TEST_EXPECT(size && size->get_number("width", 0) == 640.0 && size->get_number("height", 0) == 360.0 &&
			size->get_bool("canvas_sized", false));
	const ViewportModel *model = session.viewports().find("models/armory.3di", ViewportKind::Model);
	TEST_EXPECT(model && model->state().width == 800 && model->state().height == 600);
	const JsonValue items = ask(R"({"op": "items", "limit": 200})");
	int checked = 0;
	for (const JsonValue &item : items.get("items")->array) {
		const JsonValue *screen = item.get("screen");
		if (item.get_string("kind", "") != "user_point" || !screen || !screen->is_array()) continue;
		const JsonValue hit = ask(R"({"op": "hit", "x": )" + std::to_string(screen->array[0].number) + R"(, "y": )" +
				std::to_string(screen->array[1].number) + "}");
		TEST_EXPECT(hit.get_string("viewport", "") == "model" && hit.get_string("path", "") == "models/armory.3di" &&
				hit.get_string("kind", "") == "user_point" && hit.get_number("id", 0) == item.get_number("id", -1));
		++checked;
	}
	TEST_EXPECT(checked > 0);
	std::printf("test_canvas_sized_reads passed\n");
	return 0;
}

int main() {
	TEST_EXPECT(test_edits_in_viewport() == 0);
	TEST_EXPECT(test_wire_gestures() == 0);
	TEST_EXPECT(test_gesture_lapses() == 0);
	TEST_EXPECT(test_menu_drags() == 0);
	TEST_EXPECT(test_model_edits() == 0);
	TEST_EXPECT(test_pathless_clock() == 0);
	TEST_EXPECT(test_canvas_sized_reads() == 0);
	std::printf("editor_viewport_wire: all tests passed\n");
	return 0;
}
