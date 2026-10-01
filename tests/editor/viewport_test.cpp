// S13 V5 (ADR 0046 S13): the viewport seam, over devices that draw nothing (viewport_test_support.h)
// and a real session. What a device takes as its viewport follows the session: the picture made when
// a device attaches, then Keep while nothing moves; Rebuild for an edit of a menu, a file its picture
// read moving its stamp, a menu's options; Update for a model's options and an edit only its overlays
// show; Clear for a document the game could not read, kept so until it changes (the failure latch);
// what changed in the document (ChangeClass None, Unknown, Loaded) read once a follow. The one
// preview clock. Two menus keep their own options; the device cache gives up its least recently used
// device (four held), its viewport keeping its state (a camera) for the next, which makes the picture
// again. The envelope and its pages. A SetViewport, a drag and a command through a real session, the
// refused answering false. One undo step per gesture through a real session: a menu drag of four
// samples, a drag of three selected windows moving all three, a gesture its canvas ends by not
// drawing, a model marker's drag (two selected markers moving as one). S13 V7: a SetViewport with no
// path the active document's; an edit in a viewport over the wire (edit_in_viewport), its drags of one
// gesture one undo step and its command one request.

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/menu_viewport.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/preview_clock.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/editor_queries.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
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

using editor_test::FakeDevice;
using editor_test::FakeDevices;
using editor_test::NoProcess;
using Actions = std::vector<ViewportAction>;

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

JsonValue parse(const char *text) {
	JsonValue json;
	std::string error;
	opennova::io::json_parse(text, json, error);
	return json;
}

void set(ProjectSession &session, const Document &document, const NodeAddress &address, const char *field,
		Value value) {
	EditorRequest request = request::edit_record(document.path(), Edit());
	request.edits[0].address = address;
	request.edits[0].field = field;
	request.edits[0].value = std::move(value);
	session.handle(request);
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

template <typename Kind>
const Kind *viewport_of(ProjectSession &session, const std::string &path, ViewportKind kind) {
	return static_cast<const Kind *>(session.viewports().find(path, kind));
}

// A canvas's requests served as they are raised, as the windows raise theirs; what was raised kept,
// and whether the session refused one.
struct Served final : CanvasRequests {
	ProjectSession &session;
	std::vector<EditorRequest> raised;
	bool refused = false;
	explicit Served(ProjectSession &owner) : session(owner) {}
	void request(EditorRequest request) override {
		session.handle(request);
		refused = refused || !session.outcome().done();
		raised.push_back(std::move(request));
	}
	size_t count(EditorRequestKind kind) const {
		size_t n = 0;
		for (const EditorRequest &request : raised) n += request.kind == kind ? 1 : 0;
		return n;
	}
	// Every batch carries one gesture's token (0: none raised, or not one).
	uint64_t gesture() const {
		uint64_t token = 0;
		for (const EditorRequest &request : raised)
			for (const Edit &edit : request.edits) {
				if (request.kind != EditorRequestKind::EditRecord) continue;
				if (!edit.gesture || (token && edit.gesture != token)) return 0;
				token = edit.gesture;
			}
		return token;
	}
};

// A canvas over a viewport of a real session, a frame at a time as its view draws it: the Shell's
// pump (its devices following the session), then the kind's half of the canvas over the viewport at
// the canvas's size, every request served as it is raised; then the frame's bracket.
struct CanvasRig {
	ProjectSession &session;
	FakeDevices &devices;
	std::string path;
	ViewportKind kind;
	// The grid a drag snaps to, as the kind's toolbar sets it (a menu's Snap on: 1; a model's metres).
	float snap = 0.0f;
	int width = 800;
	int height = 600;
	std::unique_ptr<CanvasHalf> half;
	Served out{ session };

	const ViewportModel *viewport() { return session.viewports().find(path, kind); }
	CanvasInput at(float x, float y) const {
		CanvasInput in;
		in.width = width;
		in.height = height;
		in.mouse = CanvasPoint{ x, y };
		in.screen = CanvasPoint{ x, y };
		in.hovered = true;
		return in;
	}
	// A frame the canvas draws.
	void frame(const CanvasInput &in) {
		devices.sync(session);
		const ViewportModel *model = viewport();
		if (!model) return;
		if (!half) half = model->make_canvas();
		ViewportContext context = viewport_context(session.view(), *model, snap);
		context.width = in.width;
		context.height = in.height;
		half->follow(*model, context, out);
		half->input(context, in, out);
		half->end_frame(out);
	}
	// A frame it does not draw (its window hidden, its tab not shown).
	void hidden() {
		devices.sync(session);
		if (half) half->end_frame(out);
	}
	// A press at (x, y), four samples to (x + dx, y + dy), let go there.
	void drag(float x, float y, float dx, float dy, CanvasKeys keys = CanvasKeys()) {
		CanvasInput in = at(x, y);
		in.keys = keys;
		in.pressed = in.down = true;
		frame(in);
		for (int i = 1; i <= 4; ++i) {
			in = at(x + dx * float(i) / 4.0f, y + dy * float(i) / 4.0f);
			in.keys = keys;
			in.down = true;
			frame(in);
		}
		in.down = false;
		frame(in);
	}
};

} // namespace

// What a device takes as its viewport follows the session.
static int test_actions() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_actions");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Viewport Test"));
	session.run_operations();
	editor_test::create_missing_files(session);

	// A menu opened: its viewport made as the Preview's target names it, no device until the Shell's
	// pump attaches one, whose first action makes the picture; then nothing moved: Keep.
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	const std::string path = menu ? menu->path() : std::string();
	const auto *shown = viewport_of<MenuViewport>(session, path, ViewportKind::Menu);
	TEST_EXPECT(menu && shown && !shown->attached() && shown->builds() == 0);
	if (!menu || !shown) return 1;
	devices.sync(session);
	FakeDevice *device = devices.held(path, ViewportKind::Menu);
	TEST_EXPECT(device && device->taken == (Actions{ ViewportAction::Rebuild }));
	if (!device) return 1;
	TEST_EXPECT(shown->attached() && shown->builds() == 1 && shown->configures() == 1);
	devices.sync(session);
	TEST_EXPECT(device->last() == ViewportAction::Keep && shown->configures() == 1);

	// An edit (ChangeClass Unknown): the screen configured again.
	const NodeAddress title = named(*menu, "TITLE"), exit = named(*menu, "EXIT");
	TEST_EXPECT(title.child && exit.child);
	set(session, *menu, title, "position.left", int64_t(8));
	size_t from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->configures() == 2);

	// The device's size, the viewport's state: nothing made again (the device sizes itself to it).
	session.handle(request::set_viewport(path, R"({"device": {"width": 640, "height": 480}})"));
	TEST_EXPECT(session.outcome().done());
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from).empty() && shown->state().width == 640 && shown->state().height == 480);
	TEST_EXPECT(shown->size().width == 640 && shown->size().height == 480 && !shown->canvas_sized());
	// A canvas drawing it at a size of its own (a picture fitted to the room it has): the device
	// reports that size, and a SetViewport of the device's size is refused, naming why; a pump with
	// no canvas drawing it, it is the state's size again and the SetViewport applies.
	ViewportPicture fitted;
	fitted.width = 720;
	fitted.height = 540;
	fitted.canvas_sized = true;
	device->draw(fitted);
	devices.sync(session);
	TEST_EXPECT(shown->canvas_sized() && shown->size().width == 720 && shown->size().height == 540 &&
			shown->state().width == 640);
	session.handle(request::set_viewport(path, R"({"device": {"width": 1024, "height": 768}})"));
	TEST_EXPECT(!session.outcome().done() && shown->state().width == 640 && !session.outcome().findings.empty() &&
			session.outcome().findings.front().message.find("canvas") != std::string::npos);
	// Drawn at the state's size (a menu at its Device size): not a canvas's own size.
	fitted.width = 640;
	fitted.height = 480;
	fitted.canvas_sized = false;
	device->draw(fitted);
	devices.sync(session);
	TEST_EXPECT(!shown->canvas_sized() && shown->size().width == 640);
	session.handle(request::set_viewport(path, R"({"device": {"width": 1024, "height": 768}})"));
	TEST_EXPECT(session.outcome().done() && shown->state().width == 1024);
	session.handle(request::set_viewport(path, R"({"device": {"width": 640, "height": 480}})"));
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	TEST_EXPECT(shown->size().width == 640 && shown->size().height == 480 && !shown->canvas_sized());
	// Its options: the screen configured again; the same options again, nothing.
	session.handle(request::set_viewport(path, R"({"options": {"show_hidden": true}})"));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->options().show_hidden);
	session.handle(request::set_viewport(path, R"({"kind": "menu", "options": {"show_hidden": true}})"));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from).empty() && shown->configures() == 3);

	// A file its picture read moved its stamp (the stylesheet, not open, written outside the
	// editor): the screen configured again.
	const AssetEntry *style_file = view.project.scan->find("menu_style.mns");
	TEST_EXPECT(style_file != nullptr);
	if (!style_file) return 1;
	const std::string style = view.project.root + "/" + style_file->relative_path;
	std::vector<uint8_t> bytes = test_io::read_file(style);
	TEST_EXPECT(!bytes.empty());
	bytes.push_back('\r');
	bytes.push_back('\n');
	TEST_EXPECT(editor_test::write_bytes(style, bytes));
	session.handle(request::rescan());
	session.run_operations();
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->configures() == 4);

	// A menu the game could not read: Clear, and kept so, no retry each pump (the failure latch),
	// until it changes; undone, the picture made again.
	set(session, *menu, exit, "string.justify", std::string("CEN\"TER"));
	from = device->taken.size();
	devices.sync(session);
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Clear }) && shown->status() == ViewportStatus::Failed);
	TEST_EXPECT(shown->configures() == 5);
	session.handle(request::undo(menu->path()));
	from = device->taken.size();
	devices.sync(session);
	TEST_EXPECT(device->since(from) == (Actions{ ViewportAction::Rebuild }) && shown->status() == ViewportStatus::Ready);

	// A model: its options are an Update (the picture stands, the state applied again), as is an
	// edit only its overlays show.
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("models/armory.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	devices.sync(session);
	FakeDevice *model_device = devices.held("models/armory.3di", ViewportKind::Model);
	const auto *model = viewport_of<ModelViewport>(session, "models/armory.3di", ViewportKind::Model);
	TEST_EXPECT(document && model && model_device && model_device->taken == (Actions{ ViewportAction::Rebuild }));
	if (!document || !model || !model_device) return 1;
	// The menu's device stays held, and follows (nothing moved in it).
	TEST_EXPECT(devices.held(path, ViewportKind::Menu) == device && device->last() == ViewportAction::Keep);
	session.handle(request::set_viewport("models/armory.3di", R"({"options": {"lod": 0}})"));
	devices.sync(session);
	TEST_EXPECT(model_device->since(1) == (Actions{ ViewportAction::Update }) && model->lod() == 0);

	// What changed in the document, read once a follow (the model's reads of its bytes): nothing
	// (None) reads nothing; an edit (Unknown), its undo and the document read again from its file
	// (Loaded) read it once each.
	const uint64_t reads = model->reads();
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads);
	const ModelRow *row = document->model_row();
	const NodeAddress point{ row->id, node_kind(ModelKind::UserPoint), row->ids.lists[3][0].id };
	Value value;
	TEST_EXPECT(document->get(point, "position.x", value));
	set(session, *document, point, "position.x", std::get<double>(value) + 1.0);
	from = model_device->taken.size();
	devices.sync(session);
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads + 1 && model_device->since(from) == (Actions{ ViewportAction::Update }));
	session.handle(request::undo(document->path()));
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads + 2 && !document->dirty());
	session.handle(request::reload_document("models/armory.3di"));
	session.run_operations();
	TEST_EXPECT(session.outcome().done());
	devices.sync(session);
	TEST_EXPECT(model->reads() == reads + 3 && model->status() == ViewportStatus::Ready);
	std::printf("test_actions passed\n");
	return 0;
}

// The one preview clock: it runs while it plays, `rate` times as fast as the Shell's frames pass,
// its milliseconds wrapping as the game's and its ticks held at their largest; every viewport reads
// it, so a SetViewport's clock on one is every viewport's.
static int test_clock() {
	PreviewClock clock;
	TEST_EXPECT(clock.playing() && clock.rate() == 1.0 && clock.ms() == 0 && clock.ticks() == 0);
	clock.advance(0.5);
	TEST_EXPECT(clock.ms() == 500 && clock.ticks() == 31);
	clock.set_rate(2.0);
	clock.advance(0.25);
	TEST_EXPECT(clock.ms() == 1000 && clock.ticks() == 62);
	clock.set_playing(false);
	clock.advance(1.0);
	TEST_EXPECT(clock.ms() == 1000 && clock.ticks() == 62);
	clock.set_playing(true);
	clock.set_rate(-3.0);
	clock.advance(1.0);
	TEST_EXPECT(clock.rate() == 0.0 && clock.ms() == 1000);
	clock.set_rate(1.0);
	clock.seek_ms(UINT32_MAX);
	clock.advance(0.002);
	TEST_EXPECT(clock.ms() == 1);
	clock.seek_ticks(INT32_MAX - 1);
	clock.advance(1.0);
	TEST_EXPECT(clock.ticks() == INT32_MAX);
	clock.seek_ticks(-5);
	TEST_EXPECT(clock.ticks() == 0);

	editor_test::TempProjectDir dir("opennova_editor_viewport_clock");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Clock Test"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("main.mnu"));
	session.handle(request::open_document("models/armory.3di"));
	session.advance(0.25);
	TEST_EXPECT(session.viewports().clock().ms() == 250);
	// Paused through the menu's viewport: the model's envelope reads the same clock.
	session.handle(request::set_viewport(session.document_for("main.mnu")->path(),
			R"({"clock": {"playing": false, "time_ms": 40}})"));
	TEST_EXPECT(session.outcome().done());
	session.advance(1.0);
	const JsonValue timing = *viewport_to_json(view, session.viewports().find("models/armory.3di", ViewportKind::Model),
			ViewportKind::Model, JsonPage()).get("clock");
	TEST_EXPECT(!timing.get_bool("playing", true) && timing.get_number("time_ms", 0) == 40.0);
	std::printf("test_clock passed\n");
	return 0;
}

// Two menus open, each its own viewport and its own options; the Preview back on the first shows
// its own, and no device is made again.
static int test_two_menus() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_menus");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Two Menus"));
	session.run_operations();
	editor_test::create_missing_files(session);
	session.handle(request::open_document("main.mnu"));
	devices.sync(session);
	session.handle(request::create_file("extra.mnu", "menu"));
	const Document *main_menu = session.document_for("main.mnu");
	const Document *extra_menu = session.document_for("extra.mnu");
	TEST_EXPECT(main_menu && extra_menu);
	if (!main_menu || !extra_menu) return 1;
	const std::string main_path = main_menu->path(), extra_path = extra_menu->path();
	TEST_EXPECT(view.documents.previews[ViewportKind::Menu].path == extra_path);
	devices.sync(session);
	TEST_EXPECT(devices.made.size() == 2 && devices.cache.size() == 2);
	session.handle(request::set_viewport(main_path, R"({"options": {"show_hidden": true}})"));
	session.handle(request::set_viewport(extra_path, R"({"device": {"width": 1024, "height": 768}})"));
	const size_t from = devices.held(main_path, ViewportKind::Menu)->taken.size();
	devices.sync(session);
	// The menu no longer the Preview's keeps its device, which takes what its options ask.
	TEST_EXPECT(devices.held(main_path, ViewportKind::Menu)->since(from) == (Actions{ ViewportAction::Rebuild }));
	const auto *main = viewport_of<MenuViewport>(session, main_path, ViewportKind::Menu);
	const auto *extra = viewport_of<MenuViewport>(session, extra_path, ViewportKind::Menu);
	TEST_EXPECT(main && extra && main != extra);
	TEST_EXPECT(main->options().show_hidden && !extra->options().show_hidden);
	TEST_EXPECT(main->state().width == 800 && extra->state().width == 1024 && extra->state().height == 768);
	TEST_EXPECT(viewport_to_json(view, extra, ViewportKind::Menu, JsonPage()).get("options")->get_bool("show_hidden", true) ==
			false);
	session.handle(request::open_document("main.mnu"));
	devices.sync(session);
	TEST_EXPECT(view.documents.previews[ViewportKind::Menu].path == main_path && devices.made.size() == 2);
	TEST_EXPECT(viewport_to_json(view, session.viewports().find(main_path, ViewportKind::Menu), ViewportKind::Menu,
						JsonPage())
						.get("options")
						->get_bool("show_hidden", false));
	std::printf("test_two_menus passed\n");
	return 0;
}

// The device cache holds four: a fifth given its device gives up the least recently used, whose
// viewport keeps its state (its camera) and holds nothing; the Preview back on it, a device is made
// that makes the picture again at the camera it kept. A canvas asking for a device has it at the
// next pump.
static int test_device_cache() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_cache");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Device Cache"));
	session.run_operations();
	const std::vector<uint8_t> armory = test_io::read_file(synth("armory.3di"));
	std::vector<std::string> paths;
	for (int i = 0; i < 5; ++i) {
		paths.push_back("models/m" + std::to_string(i) + ".3di");
		TEST_EXPECT(editor_test::write_bytes(view.project.root + "/" + paths.back(), armory));
	}
	session.handle(request::rescan());
	session.run_operations();
	for (size_t i = 0; i < paths.size(); ++i) {
		session.handle(request::open_document(paths[i]));
		devices.sync(session);
		if (i == 0) {
			session.handle(request::set_viewport(paths[0],
					R"({"camera": {"yaw": 1.25, "pitch": 0.5, "distance": 12, "target": [1, 2, 3]}})"));
			TEST_EXPECT(session.outcome().done());
			devices.sync(session);
		}
	}
	TEST_EXPECT(kViewportDeviceCapacity == 4 && devices.made.size() == 5 && devices.cache.size() == 4);
	const auto *first = viewport_of<ModelViewport>(session, paths[0], ViewportKind::Model);
	TEST_EXPECT(first && !devices.held(paths[0], ViewportKind::Model) && !first->attached());
	if (!first) return 1;
	TEST_EXPECT(first->camera().yaw == 1.25f && first->camera().distance == 12.0f && first->builds() == 1);
	for (size_t i = 1; i < paths.size(); ++i) TEST_EXPECT(devices.held(paths[i], ViewportKind::Model));

	// The Preview back on the first: a device made, which makes the picture at the camera kept; the
	// next least recently used (the second) given up.
	session.handle(request::open_document(paths[0]));
	devices.sync(session);
	TEST_EXPECT(devices.made.size() == 6 && devices.held(paths[0], ViewportKind::Model) == devices.made.back());
	TEST_EXPECT(devices.made.back()->taken == (Actions{ ViewportAction::Rebuild }) && first->attached() && first->builds() == 2);
	TEST_EXPECT(first->camera().yaw == 1.25f && first->camera().distance == 12.0f && first->camera().target.z == 3.0f);
	TEST_EXPECT(!devices.held(paths[1], ViewportKind::Model) &&
			!session.viewports().find(paths[1], ViewportKind::Model)->attached());

	// A canvas asks for the second's device (its view drawn): none until the pump makes it, the
	// least recently used then the third.
	TEST_EXPECT(devices.cache.device(paths[1], ViewportKind::Model) == nullptr);
	devices.sync(session);
	TEST_EXPECT(devices.made.size() == 7 && devices.cache.device(paths[1], ViewportKind::Model) == devices.made.back());
	TEST_EXPECT(devices.made.back()->taken == (Actions{ ViewportAction::Rebuild }));
	TEST_EXPECT(!devices.held(paths[2], ViewportKind::Model) && devices.cache.size() == 4);

	// Closed, a document's viewport goes, and its device with it.
	session.handle(request::close_document(paths[0]));
	devices.sync(session);
	TEST_EXPECT(!session.viewports().find(paths[0], ViewportKind::Model) && !devices.held(paths[0], ViewportKind::Model) &&
			devices.cache.size() == 3);

	// Opened again and synced, closed and opened again between two syncs (a viewport made again, at
	// the defaults, attached to no device): the device of the one that went goes, and the one made
	// again is given a device of its own, which makes its picture.
	session.handle(request::open_document(paths[0]));
	devices.sync(session);
	const FakeDevice *reopened = devices.held(paths[0], ViewportKind::Model);
	TEST_EXPECT(reopened && session.viewports().find(paths[0], ViewportKind::Model)->attached());
	session.handle(request::close_document(paths[0]));
	session.handle(request::open_document(paths[0]));
	const ViewportModel *again = session.viewports().find(paths[0], ViewportKind::Model);
	TEST_EXPECT(again && !again->attached());
	devices.sync(session);
	TEST_EXPECT(again && again->attached() && again->status() == ViewportStatus::Ready && again->builds() == 1);
	TEST_EXPECT(devices.held(paths[0], ViewportKind::Model) == devices.made.back() &&
			devices.made.back()->taken == (Actions{ ViewportAction::Rebuild }));
	std::printf("test_device_cache passed\n");
	return 0;
}

// The cache never gives up a device used in the round it needs one (a sync and the asks since the
// one before): one a view asked for since the last sync is kept, the least recently used of the
// rest given up (the second here, the first bumped); five viewports a view asks for every frame keep
// four devices and the fifth waits, rather than thrashing. A workspace's cache pins the Preview's
// target of the kind the Preview window shows alone (the menu's while a menu is shown), a headless
// one every kind's.
static int test_cache_rounds() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_rounds");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Rounds"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::vector<uint8_t> armory = test_io::read_file(synth("armory.3di"));
	std::vector<std::string> paths;
	for (int i = 0; i < 5; ++i) {
		paths.push_back("models/m" + std::to_string(i) + ".3di");
		TEST_EXPECT(editor_test::write_bytes(view.project.root + "/" + paths.back(), armory));
	}
	session.handle(request::rescan());
	session.run_operations();
	for (size_t i = 0; i < 4; ++i) {
		session.handle(request::open_document(paths[i]));
		devices.sync(session);
	}
	TEST_EXPECT(devices.cache.size() == 4);
	// m0, the least recently used, asked for by a view: kept; m4 opened takes m1's device.
	FakeDevice *first = devices.held(paths[0], ViewportKind::Model);
	TEST_EXPECT(first && devices.cache.device(paths[0], ViewportKind::Model) == first);
	session.handle(request::open_document(paths[4]));
	devices.sync(session);
	TEST_EXPECT(devices.held(paths[0], ViewportKind::Model) == first && !devices.held(paths[1], ViewportKind::Model) &&
			devices.held(paths[4], ViewportKind::Model) && devices.cache.size() == 4);
	TEST_EXPECT(!session.viewports().find(paths[1], ViewportKind::Model)->attached());

	// Five asked for every round: four keep their devices, the fifth waits; nothing made again.
	const size_t made = devices.made.size();
	for (int round = 0; round < 3; ++round) {
		for (const std::string &path : paths) devices.cache.device(path, ViewportKind::Model);
		devices.sync(session);
	}
	TEST_EXPECT(devices.made.size() == made && devices.cache.size() == 4 && !devices.held(paths[1], ViewportKind::Model));

	// A workspace's cache (set_pin_all_targets(false)), over a project of its own: the Preview window
	// showing the menu, its target given a device and the model's not; showing the model, the model's
	// too (the menu's kept). A headless cache (the default) gives both kinds' targets one.
	editor_test::TempProjectDir pin_dir("opennova_editor_viewport_pins");
	ProjectSession pins(platform, preferences);
	const SessionView &pinned = pins.view();
	pins.handle(request::new_project(pin_dir.file("project"), "Pins"));
	pins.run_operations();
	editor_test::create_missing_files(pins);
	TEST_EXPECT(editor_test::write_bytes(pinned.project.root + "/models/armory.3di", armory));
	pins.handle(request::rescan());
	pins.run_operations();
	pins.handle(request::open_document("models/armory.3di"));
	pins.handle(request::open_document("main.mnu"));
	const Document *menu_document = pins.document_for("main.mnu");
	TEST_EXPECT(menu_document != nullptr);
	if (!menu_document) return 1;
	const std::string menu = menu_document->path(), model = "models/armory.3di";
	TEST_EXPECT(pinned.documents.preview_shown == ViewportKind::Menu &&
			pinned.documents.previews[ViewportKind::Model].path == model);
	{
		FakeDevices shown;
		shown.cache.set_pin_all_targets(false);
		shown.sync(pins);
		TEST_EXPECT(shown.held(menu, ViewportKind::Menu) && !shown.held(model, ViewportKind::Model) && shown.cache.size() == 1);
		TEST_EXPECT(!pins.viewports().find(model, ViewportKind::Model)->attached());
		pins.handle(request::open_document(model));
		TEST_EXPECT(pinned.documents.preview_shown == ViewportKind::Model);
		shown.sync(pins);
		TEST_EXPECT(shown.held(model, ViewportKind::Model) && shown.held(menu, ViewportKind::Menu) && shown.cache.size() == 2);
	}
	pins.handle(request::open_document("main.mnu"));
	FakeDevices headless;
	headless.sync(pins);
	TEST_EXPECT(headless.held(model, ViewportKind::Model) && headless.held(menu, ViewportKind::Menu));
	std::printf("test_cache_rounds passed\n");
	return 0;
}

// A viewport's state moves by a SetViewport, or by one of the three things its follow derives (ADR
// 0046 S13 V5's derived changes): a menu's held window following the selection, a model framed
// when another is shown, the clock sought for a new clip or a selected event (model_viewport_test's
// test_animation pins the third). Each moves the view's Viewports concern, as a SetViewport does
// (a refused one moves nothing); syncs with nothing asked move no viewport's state, the clock or
// the concern.
static int test_state_moves() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_state");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "State"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	const auto moves = [&]() { return view.revisions.of(ViewConcern::Viewports); };

	// The menu: a SetViewport moves the concern, a refused one does not; the held window follows the
	// selection (derived), moving it at the sync that follows.
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	devices.sync(session);
	const NodeAddress title = named(*menu, "TITLE"), exit = named(*menu, "EXIT");
	uint64_t at = moves();
	session.handle(request::set_viewport(menu->path(), R"({"options": {"force_state": "mouseover", "force_id": )" +
			std::to_string(exit.child) + "}}"));
	TEST_EXPECT(session.outcome().done() && moves() == at + 1);
	at = moves();
	session.handle(request::set_viewport(menu->path(), R"({"options": {"force_state": "sideways"}})"));
	TEST_EXPECT(!session.outcome().done() && moves() == at);
	devices.sync(session);
	at = moves();
	session.handle(request::select_record(menu->path(), title));
	TEST_EXPECT(moves() == at);
	devices.sync(session);
	const auto *shown_menu = viewport_of<MenuViewport>(session, menu->path(), ViewportKind::Menu);
	TEST_EXPECT(shown_menu && shown_menu->options().force_window == title.child && moves() == at + 1);

	// The model: framed when it is first shown (derived).
	session.handle(request::open_document("models/armory.3di"));
	at = moves();
	devices.sync(session);
	const auto *model = viewport_of<ModelViewport>(session, "models/armory.3di", ViewportKind::Model);
	TEST_EXPECT(model && model->status() == ViewportStatus::Ready && moves() > at);
	if (!model) return 1;

	// Nothing asked: syncs and polls move no viewport's state, the clock or the concern.
	devices.sync(session);
	struct Seen {
		std::string menu_options, model_options, camera;
		int width = 0, height = 0;
		bool playing = false;
		uint32_t ms = 0;
		int32_t ticks = 0;
		double rate = 0.0;
		uint64_t moved = 0;
		bool operator==(const Seen &o) const {
			return menu_options == o.menu_options && model_options == o.model_options && camera == o.camera &&
					width == o.width && height == o.height && playing == o.playing && ms == o.ms &&
					ticks == o.ticks && rate == o.rate && moved == o.moved;
		}
	};
	const auto seen = [&]() {
		Seen out;
		out.menu_options = opennova::io::json_write(shown_menu->options_json());
		out.model_options = opennova::io::json_write(model->options_json());
		out.camera = opennova::io::json_write(model->camera_json());
		out.width = model->state().width;
		out.height = model->state().height;
		const PreviewClock &clock = session.viewports().clock();
		out.playing = clock.playing();
		out.ms = clock.ms();
		out.ticks = clock.ticks();
		out.rate = clock.rate();
		out.moved = moves();
		return out;
	};
	const Seen before = seen();
	for (int i = 0; i < 3; ++i) {
		session.poll();
		devices.sync(session);
	}
	TEST_EXPECT(seen() == before);
	std::printf("test_state_moves passed\n");
	return 0;
}

// The envelope: every member, a page of its items (and its notes by the same page), a model's camera,
// and the kind's empty viewport where none is kept.
static int test_envelope() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_envelope");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Envelope"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(viewport_to_json(view, nullptr, ViewportKind::Menu, JsonPage()).get_string("reason", "") == "no_menu");
	session.handle(request::open_document("main.mnu"));
	devices.sync(session);
	const std::string path = session.document_for("main.mnu")->path();
	const ViewportModel *menu = session.viewports().find(path, ViewportKind::Menu);
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const JsonValue whole = viewport_to_json(view, menu, ViewportKind::Menu, JsonPage());
	for (const char *key : { "kind", "path", "as_saved", "status", "reason", "message", "detail", "revision",
				 "shown_revision", "current", "builds", "units", "device", "options", "camera", "clock", "body",
				 "items", "notes", "count", "offset", "next_offset", "note_count" })
		TEST_EXPECT(whole.get(key) != nullptr);
	// The viewport query stamps it (S13 V7) with the view's clock at which what its row reads last
	// moved: a SetViewport moves it.
	TEST_EXPECT(!whole.get("view_revision"));
	const auto stamp_of = [&session]() {
		std::string error;
		JsonValue args = JsonValue::make_object();
		args.set("op", JsonValue::make_string("state"));
		return session.query("viewport", args, error).get_number("view_revision", -1);
	};
	const ConcernSet reads = editor_query_row(EditorQueryKind::Viewport).reads;
	const double before = stamp_of();
	TEST_EXPECT(before == double(view.revisions.stamp_of(reads)) && before > 0.0);
	session.handle(request::set_viewport(path, R"({"options": {"show_hidden": true}})"));
	TEST_EXPECT(stamp_of() > before && stamp_of() == double(view.revisions.stamp(ViewConcern::Viewports)));
	session.handle(request::set_viewport(path, R"({"options": {"show_hidden": false}})"));
	devices.sync(session);
	TEST_EXPECT(whole.get_string("kind", "") == "menu" && whole.get_string("path", "") == path &&
			whole.get_bool("as_saved", false) && whole.get_string("status", "") == "ready" &&
			whole.get_string("reason", "") == "ready" && whole.get_string("message", "x").empty());
	TEST_EXPECT(whole.get_bool("current", false) && whole.get_number("builds", 0) == 1.0 &&
			whole.get_string("units", "") == "design" && whole.get("camera")->is_null());
	TEST_EXPECT(whole.get_number("revision", -1) == whole.get_number("shown_revision", -2));
	const JsonValue &device = *whole.get("device");
	TEST_EXPECT(device.get_bool("attached", false) && device.get_number("width", 0) == 800 &&
			device.get_number("height", 0) == 600 && !device.get_bool("canvas_sized", true));
	const JsonValue &timing = *whole.get("clock");
	TEST_EXPECT(timing.get_bool("playing", false) && timing.get_number("rate", 0) == 1.0 &&
			timing.get_number("time_ms", -1) == 0.0 && timing.get_number("ticks", -1) == 0.0);
	TEST_EXPECT(whole.get("body")->get("screen")->get_string("name", "") == "STARTUP");
	const size_t total = whole.get("items")->array.size();
	TEST_EXPECT(total >= 3 && whole.get_number("count", 0) == double(total) && whole.get_number("offset", -1) == 0.0 &&
			whole.get("next_offset")->is_null());
	TEST_EXPECT(whole.get_number("note_count", -1) == double(whole.get("notes")->array.size()));
	// A page: two items from the second, the next page's offset after them.
	JsonPage page;
	page.offset = 1;
	page.limit = 2;
	const JsonValue paged = viewport_to_json(view, menu, ViewportKind::Menu, page);
	TEST_EXPECT(paged.get("items")->array.size() == 2 && paged.get_number("count", 0) == double(total) &&
			paged.get_number("offset", -1) == 1.0);
	TEST_EXPECT(total > 3 ? paged.get_number("next_offset", 0) == 3.0 : paged.get("next_offset")->is_null());
	TEST_EXPECT(paged.get("items")->array[0].get_string("name", "") ==
			whole.get("items")->array[1].get_string("name", "?"));

	// A model's: pixels, its camera with the field of view, its body's level, registers and animation.
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(viewport_to_json(view, nullptr, ViewportKind::Model, JsonPage()).get_string("reason", "") == "no_model");
	session.handle(request::open_document("models/armory.3di"));
	devices.sync(session);
	const JsonValue model = viewport_to_json(view, session.viewports().find("models/armory.3di", ViewportKind::Model),
			ViewportKind::Model, JsonPage());
	TEST_EXPECT(model.get_string("kind", "") == "model" && model.get_string("units", "") == "pixels" &&
			model.get_string("status", "") == "ready");
	const JsonValue *camera = model.get("camera");
	TEST_EXPECT(camera && camera->is_object() && camera->get("fov") && camera->get("target") &&
			camera->get("target")->array.size() == 3 && camera->get_number("distance", 0) > 0.0);
	TEST_EXPECT(model.get("body")->get("lod")->get("count") && model.get("body")->get("registers") &&
			model.get("body")->get("animation")->is_null());
	TEST_EXPECT(model.get("options")->get_string("lod", "") == "auto");
	// No viewport kept for a kind: its empty one (a model's, the Preview on no model's document).
	session.handle(request::close_document("models/armory.3di"));
	const JsonValue none = viewport_to_json(view, nullptr, ViewportKind::Model, JsonPage());
	TEST_EXPECT(none.get_string("status", "") == "empty" && none.get_string("reason", "") == "no_model" &&
			none.get_string("path", "x").empty() && !none.get("device")->get_bool("attached", true) &&
			none.get("items")->array.empty());

	// A hit: what lies under a point, in the viewport's units.
	ViewportHit hit;
	hit.index = 2;
	hit.id = 7;
	hit.name = "TITLE";
	hit.kind = "static";
	hit.current = true;
	const JsonValue hit_json = viewport_hit_to_json(hit);
	TEST_EXPECT(hit_json.get_number("index", 0) == 2.0 && hit_json.get_number("id", 0) == 7.0 &&
			hit_json.get_string("name", "") == "TITLE" && hit_json.get_string("kind", "") == "static" &&
			hit_json.get_bool("current", false));
	std::printf("test_envelope passed\n");
	return 0;
}

// A SetViewport over the wire (the session's JSON requests): read, served, done; with no path the
// active document's (S13 V7), refused when it shows in no viewport (a stylesheet), a document by its
// logical name; refused whole (a Problems row, viewport.refused, the request's outcome), the viewport
// as it was. A drag and a command planned by the viewport (the MCP's) and served: one undo step
// each; a drag the session refuses as it is served answers false, the menu as it was.
static int test_requests() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_requests");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	session.handle(request::new_project(dir.file("project"), "Requests"));
	session.run_operations();
	editor_test::create_missing_files(session);
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	devices.sync(session);
	const std::string path = menu ? menu->path() : std::string();
	const auto *shown = viewport_of<MenuViewport>(session, path, ViewportKind::Menu);
	TEST_EXPECT(menu && shown && shown->status() == ViewportStatus::Ready);
	if (!menu || !shown) return 1;

	const auto wire = [&](std::string text) {
		for (size_t at = text.find("MAIN_PATH"); at != std::string::npos; at = text.find("MAIN_PATH"))
			text.replace(at, 9, path);
		return session.handle_json(parse(text.c_str()));
	};
	JsonValue answer = wire(
			R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"kind": "menu", "options": {"show_hidden": true}}})");
	TEST_EXPECT(answer.get_bool("ok", false) && answer.get_bool("served", false) &&
			answer.get("outcome")->get_bool("done", false) && shown->options().show_hidden);
	// No path: the active document's (S13 V7, A5's pathless rule), its kind the one it shows in.
	answer = wire(R"({"kind": "set_viewport", "viewport": {"options": {"show_hidden": false}}})");
	TEST_EXPECT(answer.get("outcome")->get_bool("done", false) && !shown->options().show_hidden);
	for (const char *refused : {
				 R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"options": {"bogus": 1}}})",
				 R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"kind": "model", "options": {"lod": 0}}})",
				 R"({"kind": "set_viewport", "viewport": {"kind": "model", "options": {"lod": 0}}})",
				 R"({"kind": "set_viewport", "path": "nosuch.mnu", "viewport": {"kind": "menu"}})",
				 R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"device": {"width": 0}}})",
				 R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": {"options": {"show_hidden": true}, "zoom": 2}})" }) {
		answer = wire(refused);
		const JsonValue *outcome = answer.get("outcome");
		TEST_EXPECT(answer.get_bool("ok", false) && outcome && !outcome->get_bool("done", true));
		TEST_EXPECT(outcome && outcome->get("findings")->array.size() == 1 &&
				outcome->get("findings")->array[0].get_string("code", "") == "viewport.refused" &&
				outcome->get("findings")->array[0].get_string("severity", "") == "error");
		TEST_EXPECT(!shown->options().show_hidden && shown->state().width == 800);
	}
	// A change that is no object is not read at all.
	answer = wire(R"({"kind": "set_viewport", "path": "MAIN_PATH", "viewport": 3})");
	TEST_EXPECT(!answer.get_bool("ok", true));
	// A stylesheet active (it feeds the menu's viewport, and shows in none): a pathless change refused,
	// naming why; the menu named by its logical name, changed.
	session.handle(request::open_document("menu_style.mns"));
	answer = wire(R"({"kind": "set_viewport", "viewport": {"options": {"show_hidden": true}}})");
	TEST_EXPECT(!answer.get("outcome")->get_bool("done", true) &&
			answer.get("outcome")->get("findings")->array[0].get_string("message", "").find("shows in no viewport") !=
					std::string::npos);
	answer = wire(R"({"kind": "set_viewport", "path": "MAIN.MNU", "viewport": {"options": {"show_hidden": true}}})");
	TEST_EXPECT(answer.get("outcome")->get_bool("done", false) && shown->options().show_hidden);
	answer = wire(R"({"kind": "set_viewport", "path": "MAIN.MNU", "viewport": {"options": {"show_hidden": false}}})");
	TEST_EXPECT(answer.get("outcome")->get_bool("done", false) && !shown->options().show_hidden);
	session.handle(request::open_document("main.mnu"));
	// A viewport the change is for is made only once the change applies: a refused one makes none.
	{
		Viewports fresh;
		std::string error;
		TEST_EXPECT(!fresh.set(session.view(), path, parse(R"({"kind": "menu", "options": {"bogus": 1}})"), error) &&
				!error.empty() && fresh.size() == 0);
		error.clear();
		TEST_EXPECT(fresh.set(session.view(), path, parse(R"({"kind": "menu", "options": {"show_hidden": true}})"), error) &&
				error.empty() && fresh.size() == 1 && fresh.find(path, ViewportKind::Menu));
	}

	// The MCP's drag, planned by the viewport and served: one undo step.
	const NodeAddress title = named(*menu, "TITLE"), exit = named(*menu, "EXIT");
	devices.sync(session);
	ViewportDrag drag;
	drag.id = title.child;
	drag.handle = "move";
	drag.x = 16;
	drag.y = 8;
	drag.snap = 1;
	editor_test::Gathered planned;
	std::string error;
	TEST_EXPECT(shown->drag(viewport_context(session.view(), *shown), drag, planned, error));
	TEST_EXPECT(editor_test::serve(session, planned.requests) && menu->dirty());
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!menu->dirty() && !menu->can_undo());
	// Refused as it is served (an operation began holding the documents after the plan): false,
	// nothing written.
	devices.sync(session);
	planned.requests.clear();
	TEST_EXPECT(shown->drag(viewport_context(session.view(), *shown), drag, planned, error));
	const uint64_t revision = menu->revision();
	TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	TEST_EXPECT(!editor_test::serve(session, planned.requests));
	TEST_EXPECT(menu->revision() == revision && !menu->dirty());
	session.run_operations();

	// The command (an arrange): EXIT's left edge to TITLE's, one batch, one undo step.
	devices.sync(session);
	editor_test::Gathered arranged;
	TEST_EXPECT(shown->command(viewport_context(session.view(), *shown), "align_left", { title.child, exit.child },
			arranged, error));
	TEST_EXPECT(arranged.requests.size() == 1 && editor_test::serve(session, arranged.requests));
	TEST_EXPECT(field_of(*menu, exit, "position.left") == field_of(*menu, title, "position.left") || menu->dirty());
	session.handle(request::undo(menu->path()));
	TEST_EXPECT(!menu->dirty() && !menu->can_undo());
	std::printf("test_requests passed\n");
	return 0;
}

// One undo step per gesture, through a real session.
static int test_gestures() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_gestures");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	FakeDevices devices;
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Gestures"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_text(view.project.root + "/layout.mnu", kLayoutMenu));
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("layout.mnu"));
	Document *menu = session.document_for("layout.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const NodeAddress box = named(*menu, "BOX"), other = named(*menu, "OTHER"), tiny = named(*menu, "TINY");
	TEST_EXPECT(box.child && other.child && tiny.child);
	const std::string path = menu->path();
	CanvasRig rig{ session, devices, path, ViewportKind::Menu, 1.0f };
	devices.sync(session);
	TEST_EXPECT(rig.viewport() && rig.viewport()->status() == ViewportStatus::Ready);

	// A drag of BOX, four samples on the grid: a batch a step under one token, its one end, one undo
	// step; BOX on 144/344/120/220.
	session.handle(request::select_record(path, box));
	rig.drag(200.0f, 150.0f, 40.0f, 21.0f);
	TEST_EXPECT(!rig.out.refused && rig.out.count(EditorRequestKind::EditRecord) >= 2 &&
			rig.out.count(EditorRequestKind::EndEdit) == 1 && rig.out.gesture() != 0);
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 144, 120, 344, 220 }));
	session.handle(request::undo(path));
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 100, 100, 300, 200 }) && !menu->dirty() &&
			!menu->can_undo());

	// Three windows selected, BOX dragged: all three move as BOX snaps, in one step.
	session.handle(request::select_record(path, box, SelectMode::Replace, { box, other, tiny }));
	rig.out.raised.clear();
	rig.drag(200.0f, 150.0f, 40.0f, 21.0f);
	TEST_EXPECT(!rig.out.refused && rig.out.count(EditorRequestKind::EndEdit) == 1 && rig.out.gesture() != 0);
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 144, 120, 344, 220 }));
	TEST_EXPECT(edges(*menu, other) == (std::vector<int64_t>{ 444, 320, 644, 420 }));
	TEST_EXPECT(edges(*menu, tiny) == (std::vector<int64_t>{ 644, 124, 656, 136 }));
	session.handle(request::undo(path));
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 100, 100, 300, 200 }));
	TEST_EXPECT(edges(*menu, other) == (std::vector<int64_t>{ 400, 300, 600, 400 }));
	TEST_EXPECT(edges(*menu, tiny) == (std::vector<int64_t>{ 600, 104, 612, 116 }));
	TEST_EXPECT(!menu->dirty() && !menu->can_undo());

	// A drag its canvas stops drawing (its window hidden mid-drag): its end raised once, then
	// nothing as the button comes up; one undo step for what it wrote.
	session.handle(request::select_record(path, box));
	rig.out.raised.clear();
	CanvasInput in = rig.at(200.0f, 150.0f);
	in.pressed = in.down = true;
	rig.frame(in);
	for (const float x : { 224.0f, 248.0f }) {
		in = rig.at(x, 150.0f);
		in.down = true;
		rig.frame(in);
	}
	TEST_EXPECT(rig.out.count(EditorRequestKind::EditRecord) == 2 && rig.out.count(EditorRequestKind::EndEdit) == 0);
	rig.hidden();
	TEST_EXPECT(rig.out.count(EditorRequestKind::EndEdit) == 1);
	in = rig.at(248.0f, 150.0f);
	rig.frame(in);
	TEST_EXPECT(rig.out.count(EditorRequestKind::EndEdit) == 1 && rig.out.count(EditorRequestKind::EditRecord) == 2);
	TEST_EXPECT(field_of(*menu, box, "position.left") != 100 && menu->dirty());
	session.handle(request::undo(path));
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 100, 100, 300, 200 }) && !menu->can_undo());

	// A model marker's drag: the selected user point's place, four samples, one step; with a second
	// user point selected, both move as far in the same step.
	session.handle(request::open_document("models/armory.3di"));
	auto *document = dynamic_cast<ModelDocument *>(session.document_for("models/armory.3di"));
	TEST_EXPECT(document && document->model_row());
	if (!document || !document->model_row()) return 1;
	const ModelRow &row = *document->model_row();
	TEST_EXPECT(row.ids.lists[3].size() >= 2);
	if (row.ids.lists[3].size() < 2) return 1;
	const NodeAddress point{ row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][0].id };
	const NodeAddress second{ row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][1].id };
	CanvasRig model_rig{ session, devices, "models/armory.3di", ViewportKind::Model };
	const auto place = [&](const NodeAddress &record) {
		std::vector<double> at;
		for (const char *field : { "position.x", "position.y", "position.z" }) {
			Value value;
			at.push_back(document->get(record, field, value) ? std::get<double>(value) : 0.0);
		}
		return at;
	};
	const auto marker_pixel = [&](int index, float &x, float &y) {
		const auto *model = viewport_of<ModelViewport>(session, "models/armory.3di", ViewportKind::Model);
		for (const ModelOverlay &overlay : model->overlays(session.viewports().clock()))
			if (overlay.kind == ModelOverlayKind::UserPoint && overlay.index == index)
				return model->camera().project(overlay.at, model->size().width, model->size().height, x, y);
		return false;
	};
	for (const bool both : { false, true }) {
		const std::vector<double> was = place(point), second_was = place(second);
		session.handle(both ? request::select_record(document->path(), point, SelectMode::Replace, { second, point })
							: request::select_record(document->path(), point));
		devices.sync(session);
		float x = 0.0f, y = 0.0f;
		TEST_EXPECT(marker_pixel(0, x, y));
		model_rig.out.raised.clear();
		CanvasKeys alt;
		alt.alt = true;
		model_rig.drag(x, y, 24.0f, -12.0f, alt);
		TEST_EXPECT(!model_rig.out.refused && model_rig.out.count(EditorRequestKind::EditRecord) >= 2 &&
				model_rig.out.count(EditorRequestKind::EndEdit) == 1 && model_rig.out.gesture() != 0);
		TEST_EXPECT(place(point) != was && document->dirty());
		TEST_EXPECT(both ? place(second) != second_was : place(second) == second_was);
		if (both) {
			// As far: the second's move is the first's.
			const std::vector<double> moved = place(point), second_moved = place(second);
			for (size_t i = 0; i < 3; ++i)
				TEST_EXPECT(std::fabs((moved[i] - was[i]) - (second_moved[i] - second_was[i])) < 1e-3);
		}
		session.handle(request::undo(document->path()));
		TEST_EXPECT(place(point) == was && place(second) == second_was && !document->dirty() && !document->can_undo());
	}
	// The MCP's drag (the viewport's planner) of a selected marker's place moves the other selected
	// markers as far, in one step, as the canvas's does; of a marker not selected, it alone.
	for (const bool selected : { true, false }) {
		const std::vector<double> was = place(point), second_was = place(second);
		session.handle(selected ? request::select_record(document->path(), point, SelectMode::Replace, { second, point })
								: request::select_record(document->path(), second));
		devices.sync(session);
		const auto *model = viewport_of<ModelViewport>(session, "models/armory.3di", ViewportKind::Model);
		float x = 0.0f, y = 0.0f;
		TEST_EXPECT(model && marker_pixel(0, x, y));
		if (!model) return 1;
		ViewportDrag drag;
		drag.id = point.child;
		drag.handle = "place";
		drag.by = false;
		drag.x = x + 24.0f;
		drag.y = y - 12.0f;
		editor_test::Gathered planned;
		std::string error;
		TEST_EXPECT(model->drag(viewport_context(session.view(), *model), drag, planned, error));
		TEST_EXPECT(editor_test::serve(session, planned.requests));
		TEST_EXPECT(place(point) != was && (selected ? place(second) != second_was : place(second) == second_was));
		if (selected) {
			const std::vector<double> moved = place(point), second_moved = place(second);
			for (size_t i = 0; i < 3; ++i)
				TEST_EXPECT(std::fabs((moved[i] - was[i]) - (second_moved[i] - second_was[i])) < 1e-3);
		}
		session.handle(request::undo(document->path()));
		TEST_EXPECT(place(point) == was && place(second) == second_was && !document->dirty() && !document->can_undo());
	}
	std::printf("test_gestures passed\n");
	return 0;
}

// An edit in a viewport over the wire (S13 V7: edit_in_viewport, the editor MCP's drag and command):
// a drag the reader refuses is not read (ok false: by and to both, neither, a member a drag does not
// take, a command with no name), and nothing is asked of the session; one read and refused is not
// done, viewport.refused naming why (neither a drag nor a command, both, an unknown handle, a record
// the viewport does not show, a command it has not), nothing written. A drag of four samples under
// one gesture (the first's answer names it, the last ends it) is one undo step; a drag of one of three
// selected windows moves the three in one batch; a drag to a point puts the handle there; an operation
// holding the documents refuses it at the gate. A command over three windows is one request, one undo
// step. A model's marker dragged by pixels lands where a drag to that pixel puts it; its frame command
// moves the camera and makes no undo step.
static int test_edits_in_viewport() {
	editor_test::TempProjectDir dir("opennova_editor_viewport_edits");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &view = session.view();
	session.handle(request::new_project(dir.file("project"), "Edits"));
	session.run_operations();
	TEST_EXPECT(editor_test::write_text(view.project.root + "/layout.mnu", kLayoutMenu));
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/armory.3di", test_io::read_file(synth("armory.3di"))));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("layout.mnu"));
	Document *menu = session.document_for("layout.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const NodeAddress box = named(*menu, "BOX"), other = named(*menu, "OTHER"), tiny = named(*menu, "TINY");
	TEST_EXPECT(box.child && other.child && tiny.child);
	const std::string id_box = std::to_string(box.child), id_other = std::to_string(other.child),
			id_tiny = std::to_string(tiny.child);
	const auto wire = [&session](const std::string &text) {
		const JsonValue answer = session.handle_json(parse(text.c_str()));
		session.run_operations();
		return answer;
	};
	const auto done = [](const JsonValue &answer) {
		const JsonValue *outcome = answer.get("outcome");
		return answer.get_bool("ok", false) && outcome && outcome->get_bool("done", false);
	};
	// A read request refused as it was served: viewport.refused, its message saying `says`.
	const auto refused = [](const JsonValue &answer, const char *says) {
		const JsonValue *outcome = answer.get("outcome");
		const JsonValue *findings = outcome ? outcome->get("findings") : nullptr;
		const bool ok = answer.get_bool("ok", false) && outcome && !outcome->get_bool("done", true) && findings &&
				findings->array.size() == 1 && findings->array[0].get_string("code", "") == "viewport.refused" &&
				findings->array[0].get_string("message", "").find(says) != std::string::npos;
		if (!ok) std::printf("  refusal: %s\n", opennova::io::json_write(answer).c_str());
		return ok;
	};
	const uint64_t untouched = menu->revision();

	// Not read: nothing asked of the session.
	for (const std::string &unread : {
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move", "by": [8, 0], "to": [1, 1]}})",
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move"}})",
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move", "by": [8, 0], "colour": 1}})",
				 R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box + R"(, "handle": "move", "by": [8]}})",
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
		const uint64_t named_gesture = uint64_t(answer.get("outcome")->get_number("gesture", 0));
		TEST_EXPECT(named_gesture != 0 && (!gesture || named_gesture == gesture));
		gesture = named_gesture;
	}
	TEST_EXPECT(edges(*menu, box) == (std::vector<int64_t>{ 140, 120, 340, 220 }) && menu->dirty());
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

	// An operation holding the documents: refused at the gate, nothing written.
	TEST_EXPECT(session.start_operation(std::make_unique<editor_test::HoldingOperation>()) != 0);
	const JsonValue held = session.handle_json(parse((R"({"kind": "edit_in_viewport", "drag": {"id": )" + id_box +
			R"(, "handle": "move", "by": [8, 8]}})").c_str()));
	TEST_EXPECT(held.get_bool("ok", false) && !held.get("outcome")->get_bool("done", true) && !menu->dirty());
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
		const ViewportModel *found = session.viewports().resolve(view, document->path(), error);
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
	TEST_EXPECT(done(by) && by.get("outcome")->get_number("gesture", 0) != 0.0 && place() != was);
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
	const auto *model = viewport_of<ModelViewport>(session, document->path(), ViewportKind::Model);
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

int main() {
	TEST_EXPECT(test_actions() == 0);
	TEST_EXPECT(test_clock() == 0);
	TEST_EXPECT(test_two_menus() == 0);
	TEST_EXPECT(test_device_cache() == 0);
	TEST_EXPECT(test_cache_rounds() == 0);
	TEST_EXPECT(test_state_moves() == 0);
	TEST_EXPECT(test_envelope() == 0);
	TEST_EXPECT(test_requests() == 0);
	TEST_EXPECT(test_gestures() == 0);
	TEST_EXPECT(test_edits_in_viewport() == 0);
	std::printf("editor_viewport: all tests passed\n");
	return 0;
}
