// The dev tools' world-space overlays against a null ImGui backend: the
// mission-frame projection (centre and edge pixels, behind-camera rejection,
// near-plane clipping, the guard band), the overlay registry and toggles, a
// layer drawing over a fake game image through the Game window, the resize
// skip, the per-layer budgets and the 16-bit vertex guard, and the Entities
// layers over a pushed markers record.
#include "devtools_test_support.h"

#include <runtime/devtools/entities_window.h>
#include <runtime/devtools/entity_overlay.h>
#include <runtime/devtools/game_dev_tools.h>
#include <runtime/devtools/game_window.h>
#include <runtime/devtools/overlay_camera.h>
#include <runtime/devtools/overlay_canvas.h>

#include <imgui.h>

#include <cmath>
#include <cstring>

using namespace devtools_test;
using opennova::devtools::GameDevTools;
using opennova::devtools::OverlayCamera;
using opennova::devtools::OverlayCanvas;
using opennova::devtools::OverlayLayer;
using opennova::devtools::OverlayRect;
using opennova::world::Vec3;

namespace {

bool near_eq(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

// A camera at the mission origin looking north (+y), z up, with a symmetric
// perspective of the given vertical fov and aspect: the mission-frame
// view-projection composed by hand (column-major).
OverlayCamera north_camera(float fov_deg, int width, int height, float znear = 0.1f, float zfar = 1000.0f) {
	OverlayCamera c;
	c.valid = true;
	c.eye[0] = c.eye[1] = c.eye[2] = 0.0f;
	c.forward[0] = 0.0f;
	c.forward[1] = 1.0f;
	c.forward[2] = 0.0f;
	c.near_distance = znear;
	c.viewport_width = width;
	c.viewport_height = height;
	const float f = 1.0f / std::tan(fov_deg * 3.14159265f / 360.0f);
	const float aspect = static_cast<float>(width) / static_cast<float>(height);
	// View: camera right = +x, up = +z, looking along +y (view space looks
	// down -z_view): x_v = x, y_v = z, z_v = -y.
	// Projection (GL-style): x_c = f/aspect x_v, y_c = f y_v, w_c = -z_v = y.
	float *m = c.view_projection;
	for (int i = 0; i < 16; ++i) m[i] = 0.0f;
	m[0 * 4 + 0] = f / aspect;                         // x_c += (f/a) * x
	m[2 * 4 + 1] = f;                                  // y_c += f * z
	m[1 * 4 + 2] = (zfar + znear) / (zfar - znear);    // z_c from depth (unused by the mapping)
	m[3 * 4 + 2] = -2.0f * zfar * znear / (zfar - znear);
	m[1 * 4 + 3] = 1.0f;                               // w_c = y
	return c;
}

void test_projection_maps_like_unproject_position() {
	const OverlayCamera c = north_camera(90.0f, 200, 100);
	OverlayRect rect;
	rect.min_x = 10.0f;
	rect.min_y = 20.0f;
	rect.max_x = 210.0f;
	rect.max_y = 120.0f;
	float px[2];
	const float ahead[3] = {0.0f, 10.0f, 0.0f};
	CHECK(opennova::devtools::overlay_project_point(c, rect, ahead, px), "a point ahead projects");
	CHECK(near_eq(px[0], 110.0f) && near_eq(px[1], 70.0f), "straight ahead is the image centre");
	// fov 90: at depth 10, z = +10 is the top edge; x = +20 (aspect 2) the right.
	const float top[3] = {0.0f, 10.0f, 10.0f};
	CHECK(opennova::devtools::overlay_project_point(c, rect, top, px) && near_eq(px[1], 20.0f),
			"mission up is screen up (the top edge)");
	const float right[3] = {20.0f, 10.0f, 0.0f};
	CHECK(opennova::devtools::overlay_project_point(c, rect, right, px) && near_eq(px[0], 210.0f),
			"mission east is screen right for a north-facing camera");
	const float behind[3] = {0.0f, -5.0f, 0.0f};
	CHECK(!opennova::devtools::overlay_project_point(c, rect, behind, px), "a point behind is rejected");
	OverlayCamera invalid = c;
	invalid.valid = false;
	CHECK(!opennova::devtools::overlay_project_point(invalid, rect, ahead, px), "an invalid camera projects nothing");
}

void test_segments_clip_at_the_near_plane_and_the_guard_band() {
	const OverlayCamera c = north_camera(90.0f, 200, 100);
	OverlayRect rect;
	rect.max_x = 200.0f;
	rect.max_y = 100.0f;
	float a[2];
	float b[2];
	const float from_behind[3] = {0.0f, -50.0f, 0.0f};
	const float ahead[3] = {0.0f, 50.0f, 0.0f};
	CHECK(opennova::devtools::overlay_project_segment(c, rect, from_behind, ahead, a, b),
			"a segment crossing the near plane keeps its visible part");
	CHECK(std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(b[0]) && std::isfinite(b[1]),
			"and its clipped ends are finite");
	const float behind2[3] = {5.0f, -10.0f, 0.0f};
	CHECK(!opennova::devtools::overlay_project_segment(c, rect, from_behind, behind2, a, b),
			"a segment wholly behind the camera draws nothing");
	// Nearly grazing the near plane far off to the side: the projected end is
	// huge; the guard band (the rect widened by its own size) clamps it.
	const float grazing[3] = {400.0f, 0.2f, 0.0f};
	const float centre[3] = {0.0f, 10.0f, 0.0f};
	CHECK(opennova::devtools::overlay_project_segment(c, rect, centre, grazing, a, b),
			"a segment reaching far off-image still draws");
	CHECK(b[0] <= rect.max_x + rect.width() + 0.5f, "its far end stops at the guard band");
}

class CountingLayer : public OverlayLayer {
public:
	explicit CountingLayer(const char *group, int priority, bool on = false)
			: OverlayLayer(on), group_(group), priority_(priority) {}
	const char *group() const override { return group_; }
	const char *label() const override { return "counting"; }
	int draw_priority() const override { return priority_; }
	int line_budget() const override { return budget; }
	void draw(OverlayCanvas &canvas) override {
		++draws;
		order = next_order++;
		for (int i = 0; i < lines; ++i) {
			canvas.line(Vec3{-1.0f, 10.0f, 0.0f}, Vec3{1.0f, 10.0f, 0.0f}, 0xFFFFFFFFu);
		}
	}
	void on_enabled(bool enabled) override { edges += enabled ? 1 : 100; }

	int draws = 0;
	int order = -1;
	int lines = 1;
	int budget = 4000;
	int edges = 0;
	static int next_order;

private:
	const char *group_;
	int priority_;
};
int CountingLayer::next_order = 0;

// The layers draw over the fake image in priority order, only while on and
// only while the camera matches the image size; budgets cap each layer and
// the vertex guard stops a runaway list.
void test_layers_draw_over_the_game_image() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	FakeGameViewport viewport;
	tools.set_game_viewport(&viewport);
	CountingLayer high("Test", 50);
	CountingLayer low("Test", 5);
	tools.pass().register_overlay(high);
	tools.pass().register_overlay(low);
	tools.pass().set_open(true);
	tools.pass().set_overlay_enabled(tools.entities_window().selection_layer(), false);
	CHECK(!tools.needs_overlay_camera(), "every layer off: no camera wanted");
	tools.pass().set_overlay_enabled(high, true);
	tools.pass().set_overlay_enabled(low, true);
	CHECK(high.edges == 1 && low.edges == 1, "the enable edge fires once");
	tools.pass().set_overlay_enabled(high, true);
	CHECK(high.edges == 1, "re-enabling is not an edge");
	CHECK(tools.needs_overlay_camera(), "an enabled layer wants the camera");

	// The first frames settle the dock layout and size the fake image; the
	// camera is then built for it.
	for (uint64_t frame = 1; frame <= 3; ++frame) CHECK(draw_once(tools, frame), "a settling frame draws");
	CHECK(viewport.width > 1 && viewport.height > 1, "the image has a size");
	OverlayCamera camera = north_camera(90.0f, viewport.width, viewport.height);
	tools.set_overlay_camera(camera);
	CountingLayer::next_order = 0;
	CHECK(draw_once(tools, 2), "the second frame draws");
	CHECK(high.draws == 1 && low.draws == 1, "both layers draw over the image");
	CHECK(low.order < high.order, "the lower priority draws underneath (first)");
	CHECK(tools.game_window().overlay_draws() == 1, "one overlay pass");

	// A camera built for another size (a resize in flight) skips the frame.
	camera.viewport_width += 10;
	tools.set_overlay_camera(camera);
	CHECK(draw_once(tools, 3), "the resize frame draws");
	CHECK(high.draws == 1, "the overlay skips a frame whose camera size mismatches");

	// Budgets: a layer asking for more lines than its budget drops the rest.
	camera.viewport_width -= 10;
	tools.set_overlay_camera(camera);
	high.lines = 20;
	high.budget = 5;
	CHECK(draw_once(tools, 4), "the budget frame draws");
	CHECK(high.last_stats().lines == 5 && high.last_stats().dropped == 15, "the budget caps the layer");

	// The vertex guard: without the backend's vertex-offset support the canvas
	// stops well before 16-bit indices overflow.
	CHECK((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) == 0,
			"the null backend has no vertex offsets");
	high.lines = 30000;
	high.budget = 30000;
	CHECK(draw_once(tools, 5), "the flood frame draws");
	CHECK(high.last_stats().dropped > 0, "the vertex guard trips before the index space runs out");

	tools.pass().set_overlay_enabled(high, false);
	tools.pass().set_overlay_enabled(low, false);
	CHECK(high.edges == 101, "the disable edge fires");
	CHECK(high.last_stats().lines == 0, "a disabled layer's stats clear");
	tools.set_game_viewport(nullptr);
}

// The Entities layers: Selection is on by default and marks the selected row
// of the pushed markers record; Labels draws the rest; the markers query
// follows the layers.
void test_entity_layers_follow_the_selection() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	opennova::devtools::EntitiesWindow &entities = tools.entities_window();
	CHECK(entities.selection_layer().enabled(), "the Selection layer is on by default");
	CHECK(!entities.labels_layer().enabled(), "Labels starts off");
	tools.pass().set_open(true);
	CHECK(!tools.needs_entity_markers(), "no selection, no labels: no markers wanted");
	tools.select_entity(0x0003);
	CHECK(tools.needs_entity_markers(), "a selection wants its marker");
	opennova::world::inspect::EntityMarkerQuery query = tools.entity_marker_query(Vec3{1, 2, 3});
	CHECK(query.selected == 0x0003 && query.range_units == 0.0f, "the selection alone");
	tools.pass().set_overlay_enabled(entities.labels_layer(), true);
	query = tools.entity_marker_query(Vec3{1, 2, 3});
	CHECK(query.range_units == opennova::devtools::EntityLabelsLayer::kRangeUnits &&
					query.anchor.x == 1.0f,
			"labels widen the query around the camera");

	FakeGameViewport viewport;
	tools.set_game_viewport(&viewport);
	for (uint64_t frame = 1; frame <= 3; ++frame) CHECK(draw_once(tools, frame), "a settling frame draws");
	tools.set_overlay_camera(north_camera(90.0f, viewport.width, viewport.height));
	opennova::devtools::EntityMarkersRecord record;
	record.valid = true;
	opennova::world::inspect::EntityMarker selected;
	selected.handle = 0x0003;
	selected.name = "alpha";
	selected.selected = true;
	selected.mission_position = Vec3{0, 20, 0};
	opennova::world::inspect::EntityMarker other;
	other.handle = 0x0004;
	other.name = "bravo";
	other.team = 1;
	other.alive = true;
	other.mission_position = Vec3{3, 25, 0};
	record.rows = {selected, other};
	tools.set_entity_markers(record);
	CHECK(draw_once(tools, 2), "the entity layers draw");
	CHECK(entities.selection_layer().last_stats().lines > 0 &&
					entities.selection_layer().last_stats().texts == 1,
			"the selection draws its marker and its label");
	CHECK(entities.labels_layer().last_stats().texts == 1, "labels skip the selected row");
	tools.clear_overlay_records();
	CHECK(!entities.markers().valid, "clearing drops the markers");
	tools.set_game_viewport(nullptr);
}

}  // namespace

int main() {
	test_projection_maps_like_unproject_position();
	test_segments_clip_at_the_near_plane_and_the_guard_band();
	test_layers_draw_over_the_game_image();
	test_entity_layers_follow_the_selection();
	return report("devtools_overlay_test");
}
