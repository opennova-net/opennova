// Pins the occlusion frame camera build (runtime/world/occlusion_camera.h):
// the presentation -> mission/render remaps, the five inward frustum planes
// and their anchors, the Q22 view rows, and the environment words.

#include <runtime/world/occlusion_camera.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

bool near_eq(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

float plane_eval(const float plane[4], const float p[3]) {
	return plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2] + plane[3];
}

} // namespace

int main() {
	using namespace opennova::world;

	OcclusionViewSpec view;
	view.eye[0] = 10.0f;
	view.eye[1] = 5.0f;
	view.eye[2] = -20.0f;
	view.fov_y_deg = 90.0f; // tan_v = 1
	view.aspect = 2.0f;     // tan_h = 2
	view.near_units = 1.0f;
	view.viewport_width = 640.0f;
	view.fog_dist_units = 300.0f;
	view.water_z_units = 2.5f;
	view.local_blink_flags = 0x8;
	view.force_indoors = true;

	OcclusionFrameCamera cam;
	occlusion_camera_from_view(view, cam);

	// The eye: presentation (10, 5, -20) -> mission (10, 20, 5) in 16.16.
	check(cam.pos_fixed[0] == 10 << 16 && cam.pos_fixed[1] == 20 << 16 &&
					cam.pos_fixed[2] == 5 << 16,
			"eye presentation -> mission fixed");
	float pos_float[3];
	render_float_from_fixed(cam.pos_fixed, pos_float);
	check(near_eq(cam.pos_float[0], pos_float[0]) && near_eq(cam.pos_float[1], pos_float[1]) &&
					near_eq(cam.pos_float[2], pos_float[2]),
			"render float eye = the fixed mirror");

	// Render directions: presentation with x/z swapped.
	float f[3];
	render_dir_from_presentation(view.forward, f);
	check(near_eq(f[0], -1.0f) && near_eq(f[1], 0.0f) && near_eq(f[2], 0.0f),
			"forward -z presentation -> render -x");

	// Five planes; the near plane's normal is the forward, anchored one near
	// unit ahead; the side planes are unit and inward.
	check(cam.frustum_count == 5, "five planes");
	check(near_eq(cam.frustum[0][0], f[0]) && near_eq(cam.frustum[0][1], f[1]) &&
					near_eq(cam.frustum[0][2], f[2]),
			"near plane normal = forward");
	float near_point[3] = { cam.pos_float[0] + f[0], cam.pos_float[1] + f[1], cam.pos_float[2] + f[2] };
	check(near_eq(plane_eval(cam.frustum[0], near_point), 0.0f), "near plane passes through eye + near");
	for (int i = 1; i < 5; ++i) {
		const float len = std::sqrt(cam.frustum[i][0] * cam.frustum[i][0] +
				cam.frustum[i][1] * cam.frustum[i][1] + cam.frustum[i][2] * cam.frustum[i][2]);
		check(near_eq(len, 1.0f), "side normal is unit");
		check(near_eq(plane_eval(cam.frustum[i], cam.pos_float), 0.0f), "side plane passes through the eye");
	}
	// A point five units ahead is inside every plane; one behind fails the near.
	float ahead[3] = { cam.pos_float[0] + 5.0f * f[0], cam.pos_float[1] + 5.0f * f[1], cam.pos_float[2] + 5.0f * f[2] };
	for (int i = 0; i < 5; ++i) check(plane_eval(cam.frustum[i], ahead) >= 0.0f, "point ahead inside");
	float behind[3] = { cam.pos_float[0] - 5.0f * f[0], cam.pos_float[1] - 5.0f * f[1], cam.pos_float[2] - 5.0f * f[2] };
	check(plane_eval(cam.frustum[0], behind) < 0.0f, "point behind fails the near plane");
	// The horizontal half-angle is wider than the vertical (aspect 2): a point
	// 5 ahead and 8 to the right is inside the side planes, 8 up is not.
	float r[3], u[3];
	render_dir_from_presentation(view.right, r);
	render_dir_from_presentation(view.up, u);
	float wide[3] = { ahead[0] + 8.0f * r[0], ahead[1] + 8.0f * r[1], ahead[2] + 8.0f * r[2] };
	check(plane_eval(cam.frustum[2], wide) >= 0.0f, "8 right of 5 ahead inside the right plane (tan_h 2)");
	float tall[3] = { ahead[0] + 8.0f * u[0], ahead[1] + 8.0f * u[1], ahead[2] + 8.0f * u[2] };
	check(plane_eval(cam.frustum[4], tall) < 0.0f, "8 up of 5 ahead outside the top plane (tan_v 1)");

	// Q22 rows: presentation -z forward = mission +y; right = +x; up = +z.
	check(cam.view_rows_q22[0][0] == 0 && cam.view_rows_q22[0][1] == (1 << 22) && cam.view_rows_q22[0][2] == 0,
			"forward row = mission +y");
	check(cam.view_rows_q22[1][0] == (1 << 22) && cam.view_rows_q22[1][1] == 0 && cam.view_rows_q22[1][2] == 0,
			"right row = mission +x");
	check(cam.view_rows_q22[2][0] == 0 && cam.view_rows_q22[2][1] == 0 && cam.view_rows_q22[2][2] == (1 << 22),
			"up row = mission +z");

	// The projector focal: half the viewport width over tan(fov_h / 2),
	// rounded half up — 320 / 2 + 0.5 truncates to 160. [orig:
	// Viewport_BuildProjectionMatrix @ 0x410fe1..0x410ff7]
	check(cam.focal_pixels == 160, "focal = width/2 / tan_h, rounded");

	// The environment words and the force-indoors OR.
	check(cam.fog_dist == 300 << 16, "fog 16.16");
	check(cam.water_z == to_fixed(2.5), "water 16.16");
	check(cam.local_blink_flags == (0x8u | kBlinkIndoorsBit), "force indoors ORs the indoors bit");
	view.force_indoors = false;
	occlusion_camera_from_view(view, cam);
	check(cam.local_blink_flags == 0x8u, "no force keeps the local flags");

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::puts("occlusion_camera_test: ok");
	return EXIT_SUCCESS;
}
