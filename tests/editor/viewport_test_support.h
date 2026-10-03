#pragma once

// The viewports' tests' fakes (S13 V5): a device that draws nothing and records what its viewport
// asked, the device cache over such devices, and the requests a planner made, served through a
// session. The Shell's devices are godot/src/authoring's; these stand in for them in a ctest.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/project_asset_source.h>
#include <editor/preview/viewport_follow.h>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/menu_viewport.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_device_cache.h>
#include <editor/preview/viewport_json.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/session/project_session.h>
#include <editor/session/view/session_view.h>

#include "editor/editor_test_support.h"

namespace editor_test {

using opennova::editor::ViewportAction;

// A device that draws nothing: the actions its viewport asked of it, in order, and the size it was
// drawn at. It reports its size as the Shell's does: the one a canvas drew it at since the last pump
// (and whether that was a size of the canvas's own), else its viewport's state's. A menu's reports
// where its picture placed each widget: where the viewport's own compile did (the Shell's MenuFrame
// draws the same screen alike). It reads `reads` through the project's files as it makes its
// picture, as a model's device reads its textures, and reports them. It makes its picture whole as it
// takes a Rebuild, or (S13 V6) with `units` set builds it over that many steps, one unit a step as the
// Shell's frames step it, drawing the last picture it built (`shown`, a generation) until the build
// ends, failing at unit `fail_at` when that is set; a Rebuild drops the one in flight, a Clear drops it
// with the picture. A menu's keeps its frame's clock as the Shell's applier does (S13 V8,
// MenuViewportApplier::tick): the clock's time at each configure (a Rebuild, as V6's configure_ sets
// it), set at a tick only where menu_frame_clock says the frame draws otherwise; the times it set, in
// order, and the ticks it had. A ground a test gives it (`ground`: the height at a point of the
// viewport's space, z up; none: it has no surface) is what ground_at answers and surface_between
// finds a segment's first crossing of; the names in `missing` are what it reports it did not find.
struct FakeDevice final : opennova::editor::ViewportDevice {
	std::vector<ViewportAction> taken;
	std::vector<std::string> reads;
	uint32_t frame_ms = 0;
	std::vector<uint32_t> clock_sets;
	size_t ticks = 0;
	int draws = 0;
	int width = 0;
	int height = 0;
	bool drawn = false; // a canvas drew it since the last pump
	bool canvas_sized = false;
	// A build over steps (S13 V6): its units (0: made whole as it is taken) and the unit it fails at
	// (0: none); its build as build() says, the generation of the picture it draws (0: none), the
	// units it ran, and the Actions it took while a build ran.
	uint64_t units = 0;
	uint64_t fail_at = 0;
	opennova::editor::ViewportBuildReport built;
	uint64_t shown = 0;
	int steps = 0;
	std::vector<ViewportAction> taken_building;
	// What a model viewport's level option was as its last build ended (-2: none ended): the state
	// the build applied.
	int ended_lod = -2;
	std::function<double(double x, double y)> ground;
	std::vector<std::string> missing;
	// E13: its scene state, whether its draw asked to render this frame (a test says), the frame it
	// last rendered (the test's `frame` at its present), and what the arbitration did to it.
	uint64_t state = 0;
	bool asked = false;
	uint64_t frame = 0;
	uint64_t rendered = 0;
	int withheld = 0, published = 0, presented = 0;
	uint64_t scene_state() const override { return state; }
	bool render_asked() const override { return asked; }
	uint64_t rendered_frame() const override { return rendered; }
	void withhold_render() override {
		++withheld;
		asked = false;
	}
	void publish_scene_state() override { ++published; }
	void present(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) override {
		++presented;
		asked = false;
		rendered = frame;
	}
	bool ground_at(double x, double y, double &height) const override {
		if (!ground) return false;
		height = ground(x, y);
		return true;
	}
	bool surface_between(const double from[3], const double to[3], double point[3]) const override {
		return ground && editor_test::ground_crossing(ground, from, to, point);
	}
	// What a ray meets of the picture, as a test says (none: the device cannot say), and how often asked.
	std::function<opennova::editor::ViewportRayHit(const double from[3], const double to[3])> ray;
	mutable int rays = 0;
	opennova::editor::ViewportRayHit ray_between(const double from[3], const double to[3]) const override {
		++rays;
		return ray ? ray(from, to) : opennova::editor::ViewportRayHit();
	}
	void draw(const opennova::editor::ViewportPicture &picture) override {
		++draws;
		width = picture.width;
		height = picture.height;
		drawn = true;
		canvas_sized = picture.canvas_sized;
	}
	opennova::editor::ViewportBuildReport build() const override { return built; }
	// A picture to draw once a build ended (whole, or by its last unit): the generation it draws.
	bool holds_picture() const override { return shown != 0; }
	bool step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &) override {
		if (!built.loading) return false;
		++steps;
		++built.progress.done;
		if (fail_at != 0 && built.progress.done == fail_at) {
			built.loading = false;
			built.failed = true;
			built.message = "Unit " + std::to_string(fail_at) + " failed.";
		} else if (built.progress.done == built.progress.total) {
			built.loading = false;
			shown = built.generation;
			const auto *shown_model = dynamic_cast<const opennova::editor::ModelViewport *>(&model);
			ended_lod = shown_model ? shown_model->options().lod : -1;
		}
		return true;
	}
	void take(ViewportAction action, const opennova::editor::ViewportModel &model,
			const opennova::editor::SessionView &view, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override {
		taken.push_back(action);
		if (built.loading) taken_building.push_back(action);
		if (action == ViewportAction::Rebuild) {
			frame_ms = opennova::editor::menu_frame_time(clock);
			built = opennova::editor::ViewportBuildReport();
			built.generation = model.builds();
			if (units != 0) {
				built.loading = true;
				built.progress.total = units;
				built.progress.label = "units";
			} else {
				shown = built.generation;
			}
		} else if (action == ViewportAction::Clear) {
			built.loading = built.failed = false;
			shown = 0;
		}
		report.width = drawn ? width : model.state().width;
		report.height = drawn ? height : model.state().height;
		report.canvas_sized = drawn && canvas_sized;
		report.missing = missing;
		drawn = false;
		if (!reads.empty() && view.findings.assets) {
			opennova::editor::StampedFiles files(view.findings.assets);
			std::vector<uint8_t> bytes;
			for (const std::string &name : reads) files.read(name, bytes);
			report.files = files.stamps();
		}
		const auto *menu = dynamic_cast<const opennova::editor::MenuViewport *>(&model);
		if (!menu || menu->status() != opennova::editor::ViewportStatus::Ready) return;
		const opennova::menu::MenuFrameCompiler &compiler = menu->render().compiler();
		report.rects.resize(size_t(compiler.widget_count() > 0 ? compiler.widget_count() : 0));
		for (int index = 0; index < compiler.widget_count(); ++index) {
			opennova::mnu::RectEdges rect{};
			auto &placed = report.rects[size_t(index)];
			placed.placed = compiler.widget_rect(index, menu->render().state(), &rect);
			placed.left = rect.left;
			placed.top = rect.top;
			placed.right = rect.right;
			placed.bottom = rect.bottom;
		}
	}
	void tick(const opennova::editor::ViewportModel &model,
			const opennova::editor::PreviewClock &clock) override {
		++ticks;
		const auto *menu = dynamic_cast<const opennova::editor::MenuViewport *>(&model);
		uint32_t time = 0;
		if (!menu || menu->status() != opennova::editor::ViewportStatus::Ready ||
				!opennova::editor::menu_frame_clock(*menu, frame_ms, clock, time))
			return;
		frame_ms = time;
		clock_sets.push_back(time);
	}
	// The last action it took (Keep before any).
	ViewportAction last() const { return taken.empty() ? ViewportAction::Keep : taken.back(); }
	// The actions taken since `from`, Keep left out.
	std::vector<ViewportAction> since(size_t from) const {
		std::vector<ViewportAction> out;
		for (size_t i = from; i < taken.size(); ++i)
			if (taken[i] != ViewportAction::Keep) out.push_back(taken[i]);
		return out;
	}
};

// The Shell's devices over fakes: every device it made, still held or given up, and how many it
// made; the units each one it makes builds its picture over (S13 V6; 0: made whole as it is taken).
struct FakeDevices {
	std::vector<FakeDevice *> made;
	uint64_t units = 0;
	opennova::editor::ViewportDeviceCache cache{ [this](opennova::editor::ViewportKind) {
		auto device = std::make_unique<FakeDevice>();
		device->units = units;
		made.push_back(device.get());
		return std::unique_ptr<opennova::editor::ViewportDevice>(std::move(device));
	} };
	// The Shell's pump after the session's poll.
	void sync(opennova::editor::ProjectSession &session) { cache.sync(session.viewports(), session.view()); }
	// A frame of the Shell's (S13 V6): its pump, then the builds stepped, `units` units a frame in all
	// (the budget's, one at least, shared by the builds in flight, the most recently used first).
	void frame(opennova::editor::ProjectSession &session, int units = 1) {
		sync(session);
		int left = units;
		cache.step(session.viewports(), [&left] { return --left > 0; });
	}
	FakeDevice *held(const std::string &path, opennova::editor::ViewportKind kind) const {
		return static_cast<FakeDevice *>(cache.held(path, kind));
	}
	// The last action the device of (path, kind) took (Keep: none, or no device).
	ViewportAction last(const std::string &path, opennova::editor::ViewportKind kind) const {
		const FakeDevice *device = held(path, kind);
		return device ? device->last() : ViewportAction::Keep;
	}
};

// The requests a planner made, gathered.
struct Gathered final : opennova::editor::CanvasRequests {
	std::vector<opennova::editor::EditorRequest> requests;
	void request(opennova::editor::EditorRequest request) override { requests.push_back(std::move(request)); }
};

// The requests served in order through the session: true when there was one and each was done.
inline bool serve(opennova::editor::ProjectSession &session,
		const std::vector<opennova::editor::EditorRequest> &requests) {
	bool done = !requests.empty();
	for (const opennova::editor::EditorRequest &request : requests) {
		session.handle(request);
		done = done && session.outcome().done();
	}
	return done;
}

// The envelope of a viewport of `kind` over no document, followed once over `view`: what the kind's
// follow says with nothing of it to show (no project, none of its kind open, a menu with no screen
// selected). No wire reaches one (the viewport query reads a document's); the kind's follow keeps
// these reasons for a viewport whose document went.
inline opennova::io::JsonValue empty_viewport_json(
		const opennova::editor::SessionView &view, opennova::editor::ViewportKind kind) {
	using namespace opennova::editor;
	std::unique_ptr<ViewportModel> empty = viewport_kind_row(kind).make(std::string());
	PreviewClock clock;
	empty->follow(ViewportInput{ view, clock, nullptr, ChangeClass::Loaded }, clock);
	return viewport_to_json(view, *empty, JsonPage());
}

} // namespace editor_test
