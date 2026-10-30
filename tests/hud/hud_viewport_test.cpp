// The HUD's projection (hud/hud_viewport.h): the main viewport's words off a view block, the
// projector's centre, side-plane clips and edge forcing, the near flag, the world point's
// depth; and the world-anchored labels it feeds (hud/hud_world_labels.h): the seat labels'
// clip skip and the friendly tags' anchor, distance and clip flag.
// [orig: Viewport_BuildProjectionMatrix @0x410FB0; HUD_ClipPointToFrustumAndProject
//  @0x591E50; HUD_DrawVehicleSeatAndArmoryLabels @0x5A3290; HUD_DrawEntityLabel @0x5A39B0]
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <runtime/hud/hud_capture_labels.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_viewport.h>
#include <runtime/hud/hud_world_labels.h>
#include <runtime/world/player_view.h>

using namespace opennova;
using namespace opennova::hud;

static int failures = 0;
#define CHECK(x)                                                                                   \
	do {                                                                                           \
		if (!(x)) {                                                                                \
			std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

constexpr int32_t u = 65536;

// A 90-degree view over the 1024 x 768 surface from `eye` (whole units) at `yaw_bam`.
HudViewport view_at(int32_t ex, int32_t ey, int32_t ez, int32_t yaw_bam) {
	HudViewBlock block;
	block.pos_q16 = { ex * u, ey * u, ez * u };
	block.yaw_bam = yaw_bam;
	block.fov_q16 = 90 * u;
	block.left = 0;
	block.top = 0;
	block.right = 1023;
	block.bottom = 767;
	return hud_viewport_build(block, 1.0f, 1024, 768);
}

HudProjectedPoint at(const HudViewport &vp, int32_t x, int32_t y, int32_t z) {
	return hud_project_world_point(vp, { x * u, y * u, z * u });
}

// The build's words: the rect's centre, the focal (half the width over tan 45 degrees, rounded
// half up), the near word, the side planes through the eye.
void viewport_words() {
	const HudViewport vp = view_at(0, 0, 0, 0);
	CHECK(vp.valid);
	CHECK(vp.center_x_q16 == 512 * u);
	CHECK(vp.center_y_q16 == 384 * u);
	CHECK(vp.focal_q16 == 512 * u);
	CHECK(vp.near_q16 == 0x800);
	for (const auto &plane : vp.planes)
		CHECK(plane[3] == 0);
	// The top / bottom planes lean on the up axis, the left / right on the left axis.
	CHECK(vp.planes[0][2] < 0 && vp.planes[1][2] > 0 && vp.planes[0][1] == 0);
	CHECK(vp.planes[2][1] < 0 && vp.planes[3][1] > 0 && vp.planes[2][2] == 0);
	// An empty rect builds nothing the projector draws through.
	HudViewBlock empty;
	CHECK(!hud_viewport_build(empty, 1.0f, 0, 0).valid);
	CHECK(!hud_project_world_point(HudViewport{}, { 10 * u, 0, 0 }).valid);
}

// Straight ahead lands on the centre; left and up land left of and above it, by the focal over
// the depth in whole pixels (512 / 10 = 51.2 px: 460.8 and 332.8 truncate down).
void projector_centre() {
	const HudViewport vp = view_at(0, 0, 0, 0);
	HudProjectedPoint p = at(vp, 10, 0, 0);
	CHECK(p.valid && p.clip == 0);
	CHECK(p.x == 512.0f && p.y == 384.0f);
	CHECK(p.depth_q16 == 10 * u);
	p = at(vp, 10, 1, 0);
	CHECK(p.clip == 0 && p.x == 460.0f && p.y == 384.0f);
	p = at(vp, 10, 0, 1);
	CHECK(p.clip == 0 && p.x == 512.0f && p.y == 332.0f);
	// The eye's words translate first: the same frame from (100, 50, 2).
	const HudViewport moved = view_at(100, 50, 2, 0);
	p = at(moved, 110, 50, 2);
	CHECK(p.clip == 0 && p.x == 512.0f && p.y == 384.0f);
	// The yaw turns the frame: heading 90 degrees looks down +Y, +X on the right.
	const HudViewport north = view_at(0, 0, 0, 0x40000000);
	p = at(north, 0, 10, 0);
	CHECK(p.clip == 0 && p.x == 512.0f && p.y == 384.0f);
	p = at(north, 1, 10, 0);
	CHECK(p.clip == 0 && p.x > 512.0f);
}

// Past each side plane the point takes its flag and its edge: x to 0 / the width, y to 0 / the
// height; short of the near distance it takes 16.
void projector_clips() {
	const HudViewport vp = view_at(0, 0, 0, 0);
	HudProjectedPoint p = at(vp, 10, 20, 0);
	CHECK(p.clip == 1 && p.x == 0.0f);
	p = at(vp, 10, -20, 0);
	CHECK(p.clip == 2 && p.x == 1024.0f);
	p = at(vp, 10, 0, 20);
	CHECK(p.clip == 4 && p.y == 0.0f);
	p = at(vp, 10, 0, -20);
	CHECK(p.clip == 8 && p.y == 768.0f);
	p = at(vp, -10, 0, 0);
	CHECK((p.clip & 16) != 0);
	CHECK(p.depth_q16 == -10 * u);
	// The stretch divides the up axis: a stretch of 2 halves the rise.
	HudViewBlock block;
	block.fov_q16 = 90 * u;
	block.right = 1023;
	block.bottom = 767;
	const HudViewport stretched = hud_viewport_build(block, 2.0f, 1024, 768);
	p = at(stretched, 10, 0, 2);
	CHECK(p.clip == 0 && p.y == 332.0f);
}

// The main viewport takes the composed camera's words and the view block's eye.
void main_viewport() {
	world::PlayerCameraPose camera;
	camera.pos_q16[0] = 4 * u;
	world::PlayerCameraPose block;
	block.pos_q16[0] = 3 * u;
	const HudViewport vp = hud_main_viewport(camera, block, 90.0f, -1, 1024, 768);
	CHECK(vp.valid);
	CHECK(vp.view_pos_q16[0] == 3 * u);
	const HudProjectedPoint p = at(vp, 14, 0, 0);
	CHECK(p.clip == 0 && p.x == 512.0f && p.depth_q16 == 10 * u);
	CHECK(!hud_main_viewport(camera, block, 90.0f, -1, 0, 768).valid);
}

// The seat labels: a clipped point is no label; a kept one carries its text and highlight.
void attach_labels() {
	const HudViewport vp = view_at(0, 0, 0, 0);
	const GameTextLookup lookup = [](const char *, const char *, const char *fallback) {
		return std::string(fallback);
	};
	std::vector<world::AttachLabel> labels(2);
	labels[0].type = world::SeatType::Passenger;
	labels[0].world_pos = { 10.0f, 0.0f, 0.0f };
	labels[0].nearest = true;
	labels[1].type = world::SeatType::Passenger;
	labels[1].world_pos = { -10.0f, 0.0f, 0.0f };
	std::vector<HudAttachLabel> out;
	project_attach_labels(labels, vp, lookup, out);
	CHECK(out.size() == 1);
	if (out.size() == 1) {
		CHECK(out[0].screen_x == 512.0f && out[0].screen_y == 384.0f);
		CHECK(out[0].nearest);
		CHECK(out[0].text == attach_label_text(lookup, world::SeatType::Passenger, ""));
	}
	project_attach_labels(labels, HudViewport{}, lookup, out);
	CHECK(out.size() == 1); // no viewport adds nothing
}

// The friendly tags: the anchor (eye offset + 0x4000 over the position), its distance from the
// view block's eye (the integer square root's floor), and the clip flag kept as `projected`.
void friendly_tags() {
	const HudViewport vp = view_at(0, 0, 0, 0);
	std::vector<world::FriendlyTagSource> tags(2);
	tags[0].position = { 10.0f, 0.0f, 0.0f };
	tags[0].eye_offset_z = 0x18000;
	tags[0].team = 1;
	tags[1].position = { -10.0f, 0.0f, 0.0f };
	std::vector<HudFriendlyTag> out;
	project_friendly_tags(tags, vp, out);
	CHECK(out.size() == 2);
	if (out.size() == 2) {
		CHECK(out[0].projected);
		CHECK(out[0].screen_x == 512.0f && out[0].screen_y < 384.0f);
		// sqrt(655360^2 + 0x1C000^2) = 665319.66...
		CHECK(out[0].dist_q16 == 665319);
		CHECK(out[0].team == 1);
		CHECK(!out[1].projected);
		CHECK(out[1].dist_q16 == 655564); // the lift alone: sqrt(655360^2 + 0x4000^2)
	}
}

// The capture-point labels through the projector: a zone ahead projects clean, one behind
// takes the near flag, which the walk's draw skips.
void capture_labels_through_the_projector() {
	const HudViewport north = view_at(0, 0, 0, 0x40000000);
	HudCaptureLabel ahead;
	ahead.position = { 0, 10 * u, 0x20000 };
	ahead.point = hud_project_world_point(north, ahead.position);
	CHECK(ahead.point.valid && ahead.point.clip == 0 && ahead.point.depth_q16 > 0);
	const HudViewport south = view_at(0, 0, 0, static_cast<int32_t>(0xC0000000u));
	HudCaptureLabel behind = ahead;
	behind.point = hud_project_world_point(south, behind.position);
	CHECK((behind.point.clip & 16) != 0 && behind.point.depth_q16 < 0);
}

} // namespace

int main() {
	viewport_words();
	projector_centre();
	projector_clips();
	main_viewport();
	attach_labels();
	friendly_tags();
	capture_labels_through_the_projector();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("hud_viewport_test: ok");
	return EXIT_SUCCESS;
}
