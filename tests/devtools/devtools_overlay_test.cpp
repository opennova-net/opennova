// The dev tools' world-space overlays against a null ImGui backend: the
// mission-frame projection (centre and edge pixels, behind-camera rejection,
// near-plane clipping, the guard band), the overlay registry and toggles, a
// layer drawing over a fake game image through the Game window, the resize
// skip, the per-layer budgets and the 16-bit vertex guard, and the Entities
// layers over a pushed markers record.
#include "devtools_test_support.h"

#include <runtime/devtools/ai_overlay.h>
#include <runtime/devtools/ai_window.h>
#include <runtime/devtools/collision_overlay.h>
#include <runtime/devtools/hitbox_overlay.h>
#include <runtime/devtools/physics_window.h>
#include <runtime/devtools/rays_window.h>
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

opennova::world::inspect::AiOverlayRow ai_row(const char *name, int32_t x, int32_t y, uint16_t handle) {
	opennova::world::inspect::AiOverlayRow row;
	row.name = name;
	row.handle = handle;
	row.alive = true;
	row.infantry = true;
	row.pos[0] = x << 16;
	row.pos[1] = y << 16;
	row.state_name = "GROUND_COMBAT";
	row.move_mode = 2;
	row.alert = 2;
	row.sight_range_q16 = 50 << 16;
	row.attack_range_q16 = 20 << 16;
	return row;
}

// The AI layers: the retired view's label text, labels within range only,
// routes only for walked channels, rings for the selection and engaged
// brains; a layer on keeps the AI record flowing with the window closed.
void test_ai_layers() {
	using Element = opennova::devtools::AiOverlayLayer::Element;
	opennova::world::inspect::AiOverlayRow labelled = ai_row("alpha", 0, 30, 0x0001);
	labelled.wp_channel = 3;
	labelled.wp_node = 1;
	labelled.target_valid = true;
	labelled.target_name = "BRAVO";
	labelled.fire_delay = 4;
	CHECK(opennova::devtools::AiOverlayLayer::label_text(labelled) ==
					"alpha\nGROUND_COMBAT  m2\nch 3 node 1\n-> BRAVO  fd 4",
			"the label reads name, state, move mode, route and target");
	opennova::world::inspect::AiOverlayRow dead = ai_row("", 0, 30, 0x0002);
	dead.ai_index = 7;
	dead.alive = false;
	CHECK(opennova::devtools::AiOverlayLayer::label_text(dead) == "ai 7\nDEAD", "a dead brain reads DEAD");

	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	opennova::devtools::AiWindow &ai = tools.ai_window();
	tools.pass().set_open(true);
	CHECK(!tools.needs_ai_debug(), "no window, no layer: no AI record");
	tools.pass().set_overlay_enabled(ai.layer(0), true);  // labels
	tools.pass().set_overlay_enabled(ai.layer(1), true);  // routes
	tools.pass().set_overlay_enabled(ai.layer(3), true);  // rings
	CHECK(tools.needs_ai_debug() && tools.needs_ai_overlay(), "a layer on wants the record");

	opennova::devtools::AiDebugSnapshot snapshot;
	snapshot.valid = true;
	snapshot.report.rows = {labelled, ai_row("far", 0, 900, 0x0003), ai_row("idle", 5, 40, 0x0004)};
	snapshot.report.rows[2].target_valid = false; // not engaged: no rings
	opennova::world::inspect::AiNavChannelRow walked;
	walked.index = 3;
	walked.followers = 1;
	walked.nodes.resize(3);
	walked.nodes[1].pos[1] = 10 << 16;
	walked.nodes[2].pos[1] = 20 << 16;
	opennova::world::inspect::AiNavChannelRow unwalked = walked;
	unwalked.index = 4;
	unwalked.followers = 0;
	snapshot.report.channels = {walked, unwalked};
	tools.set_ai_debug(snapshot);

	FakeGameViewport viewport;
	tools.set_game_viewport(&viewport);
	for (uint64_t frame = 1; frame <= 3; ++frame) CHECK(draw_once(tools, frame), "a settling frame draws");
	tools.set_overlay_camera(north_camera(90.0f, viewport.width, viewport.height));
	CHECK(draw_once(tools, 4), "the AI layers draw");
	CHECK(ai.layer(0).last_stats().texts == 2, "labels within 150 units only (the far brain is skipped)");
	CHECK(ai.layer(1).last_stats().lines > 0, "the walked route draws");
	const int route_lines = ai.layer(1).last_stats().lines;
	CHECK(ai.layer(3).last_stats().lines > 0, "the engaged brain gets its rings");

	// The route of a channel nobody walks stays off the view.
	snapshot.report.channels = {unwalked};
	tools.set_ai_debug(snapshot);
	CHECK(draw_once(tools, 5), "redraw");
	CHECK(ai.layer(1).last_stats().lines < route_lines, "an unwalked channel draws no route");

	// The window closed with a layer on keeps the record the layer draws.
	ai.open = true;
	CHECK(draw_once(tools, 6), "the window shows");
	ai.open = false;
	CHECK(draw_once(tools, 7), "the window hides");
	CHECK(ai.snapshot_valid(), "a layer on keeps the AI record through the window's close");
	for (int i = 0; i < opennova::devtools::AiWindow::kLayerCount; ++i) {
		tools.pass().set_overlay_enabled(ai.layer(i), false);
	}
	CHECK(!tools.needs_ai_debug(), "every layer off, window closed: no record wanted");
	tools.set_game_viewport(nullptr);
}

// The collision layers: a layer on keeps its capture armed with its window
// closed, and each draws its pushed rows (a hit ray ends in a cross, a
// contact rings its live body, a meshed body draws its faces).
void test_collision_layers() {
	NullBackend backend;
	GameDevTools tools;
	tools.pass().attach_imgui(backend.context, &test_alloc, &test_free, nullptr);
	tools.pass().set_open(true);
	opennova::devtools::RaysWindow &rays = tools.rays_window();
	opennova::devtools::PhysicsWindow &physics = tools.physics_window();
	CHECK(!tools.needs_rays_snapshot() && !tools.needs_physics_snapshot(), "closed windows arm nothing");
	tools.pass().set_overlay_enabled(rays.overlay_layer(), true);
	tools.pass().set_overlay_enabled(physics.contacts_layer(), true);
	tools.pass().set_overlay_enabled(physics.hitbox_layer(), true);
	CHECK(tools.needs_rays_snapshot() && tools.needs_rays_overlay(), "the rays layer arms the recording");
	CHECK(tools.needs_physics_snapshot() && tools.needs_contacts_overlay(), "the contacts layer arms the capture");
	CHECK(tools.needs_hitbox_overlay(), "the hit meshes want the oracle");

	const auto fixed = [](float v) { return static_cast<int32_t>(v * 65536.0f); };
	opennova::devtools::RaysOverlayRecord ray_record;
	ray_record.valid = true;
	ray_record.ttl_ticks = 60;
	opennova::world::RayDebugRow hit;
	hit.category = 1;
	hit.result = 1; // kRayDebugHit
	hit.start = {0, fixed(5.0f), 0};
	hit.hit = {0, fixed(15.0f), 0};
	hit.end = {0, fixed(25.0f), 0};
	ray_record.rows = {hit};
	tools.set_rays_overlay(ray_record);

	opennova::devtools::ContactsOverlayRecord contact_record;
	contact_record.valid = true;
	contact_record.ttl_ticks = 62;
	opennova::world::ContactDebugRow contact;
	contact.pos = {0, fixed(12.0f), 0};
	contact.target_live = true;
	contact.target_position = Vec3{0.0f, 12.0f, 0.0f};
	contact.target_bound_radius = 1.0f;
	contact_record.rows = {contact};
	tools.set_contacts_overlay(contact_record);

	opennova::devtools::HitboxOverlayRecord hitbox_record;
	hitbox_record.valid = true;
	opennova::world::CollisionWorld::DebugHitboxEntity body;
	body.pos[1] = fixed(20.0f);
	body.bound_radius = fixed(2.0f);
	body.has_faces = true;
	body.face_total = 1;
	opennova::world::CollisionWorld::DebugHitboxFace face;
	face.v[0][1] = face.v[1][1] = face.v[2][1] = fixed(20.0f);
	face.v[1][0] = fixed(1.0f);
	face.v[2][2] = fixed(1.0f);
	body.faces = {face};
	hitbox_record.report.entities = {body};
	tools.set_hitbox_overlay(hitbox_record);

	FakeGameViewport viewport;
	tools.set_game_viewport(&viewport);
	for (uint64_t frame = 1; frame <= 3; ++frame) CHECK(draw_once(tools, frame), "a settling frame draws");
	tools.set_overlay_camera(north_camera(90.0f, viewport.width, viewport.height));
	CHECK(draw_once(tools, 4), "the collision layers draw");
	CHECK(rays.overlay_layer().last_stats().lines >= 5, "the hit ray: its leg, its faint tail and a cross");
	CHECK(physics.contacts_layer().last_stats().lines > 3, "the contact cross and its body's ring");
	CHECK(physics.hitbox_layer().last_stats().lines >= 3, "the body's face and its bound sphere");
	CHECK(physics.hitbox_layer().last_stats().texts == 1, "the nearest meshed body is labelled");
	tools.clear_overlay_records();
	CHECK(!rays.overlay().valid && !physics.contacts_overlay().valid && !physics.hitbox_overlay().valid,
			"clearing drops every collision record");
	tools.set_game_viewport(nullptr);
}

}  // namespace

int main() {
	test_projection_maps_like_unproject_position();
	test_segments_clip_at_the_near_plane_and_the_guard_band();
	test_layers_draw_over_the_game_image();
	test_entity_layers_follow_the_selection();
	test_ai_layers();
	test_collision_layers();
	return report("devtools_overlay_test");
}
