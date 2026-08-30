#include <runtime/renderer/precipitation_frame.h>

#include <cmath>

namespace opennova::renderer {

namespace {

// mission 16.16 (x, y, z) -> render float (x, z, -y) [orig:
// Math_FixedPointToFloat3_YNegated @ 0x611210 under the D3D basis].
void to_render(const int32_t mission[3], float out[3]) {
	out[0] = static_cast<float>(mission[0]) / 65536.0f;
	out[1] = static_cast<float>(mission[2]) / 65536.0f;
	out[2] = static_cast<float>(-mission[1]) / 65536.0f;
}

void clamp_length(float v[3], float max_length) {
	const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (length > max_length) {
		const float scale = max_length / length;
		v[0] *= scale;
		v[1] *= scale;
		v[2] *= scale;
	}
}

void push_vertex(std::vector<float> &out, const float p[3], float u, float v) {
	out.push_back(p[0]);
	out.push_back(p[1]);
	out.push_back(p[2]);
	out.push_back(u);
	out.push_back(v);
}

} // namespace

void compile_precipitation_frame(env::PrecipitationField &field,
		int32_t rain_pct_q16, uint32_t precipitation_kind,
		uint32_t terrain_light_combined_rgb, const PrecipitationCamera &camera,
		PrecipitationDrawState &state, PrecipitationDrawFrame &out) {
	out.clear();
	out.vertices.reserve(static_cast<size_t>(env::PrecipitationField::kSlots) * 15u);
	out.snow = precipitation_kind == 1u;
	out.color_argb = (terrain_light_combined_rgb & 0x00FFFFFFu) | 0xFF000000u;
	// The drop gate [orig: @ 0x5dee48].
	if (rain_pct_q16 <= env::PrecipitationField::kRainGateQ16) {
		state.have_last_camera = true;
		for (int i = 0; i < 3; ++i) state.last_camera_q16[i] = camera.position_q16[i];
		return;
	}
	// The camera velocity: this frame's delta from the last drawn camera
	// [orig: @ 0x5dee7a..0x5deed8], zero on the first draw.
	float velocity[3] = {0.0f, 0.0f, 0.0f};
	if (state.have_last_camera) {
		const int32_t delta[3] = {
			camera.position_q16[0] - state.last_camera_q16[0],
			camera.position_q16[1] - state.last_camera_q16[1],
			camera.position_q16[2] - state.last_camera_q16[2],
		};
		to_render(delta, velocity);
	}
	state.have_last_camera = true;
	for (int i = 0; i < 3; ++i) state.last_camera_q16[i] = camera.position_q16[i];
	// The accumulated fall since the last draw, read then zeroed
	// [orig: dword_2C05A24..2C @ 0x5deede..0x5deef4].
	const int32_t fall_q16[3] = {0, 0, field.fall_accum_z};
	float fall[3];
	to_render(fall_q16, fall);
	field.fall_accum_z = 0;

	float trail[3];
	float right[3];
	if (out.snow) {
		// [orig: @ 0x5defb9..0x5df00f] snow drifts along the camera up.
		for (int i = 0; i < 3; ++i) {
			trail[i] = camera.up[i] * 0.05f;
			right[i] = camera.right[i] * 0.025f;
		}
	} else {
		// [orig: @ 0x5df01c..0x5df132] rain streaks up 0.1, plus the camera
		// motion (clamped 0.2), minus the fall (clamped 0.1).
		clamp_length(velocity, kPrecipitationVelocityClamp);
		clamp_length(fall, kPrecipitationFallClamp);
		trail[0] = velocity[0] - fall[0];
		trail[1] = 0.1f + velocity[1] - fall[1];
		trail[2] = velocity[2] - fall[2];
		for (int i = 0; i < 3; ++i) right[i] = camera.right[i] * 0.01f;
	}
	float cam[3];
	to_render(camera.position_q16, cam);

	const int active = env::PrecipitationField::active_count(rain_pct_q16);
	out.vertices.reserve(static_cast<size_t>(active) * 15u);
	for (int i = 0; i < active; ++i) {
		const env::PrecipitationSlot &slot = field.slots[static_cast<size_t>(i)];
		// Only drops at or above their floor draw [orig: @ 0x5df196].
		if (slot.z < slot.floor_z) continue;
		const int32_t mission[3] = {slot.x, slot.y, slot.z};
		float pos[3];
		to_render(mission, pos);
		float width = 1.0f;
		float length = 1.0f;
		if (!out.snow) {
			// [orig: @ 0x5df1da..0x5df23a] width 1 + d x 0.1, length 1 + d x 0.05.
			const float d[3] = {pos[0] - cam[0], pos[1] - cam[1], pos[2] - cam[2]};
			const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			width = dist * 0.1f + 1.0f;
			length = dist * 0.05f + 1.0f;
		}
		const float head[3] = {pos[0] + trail[0] * length, pos[1] + trail[1] * length,
			pos[2] + trail[2] * length};
		const float left[3] = {pos[0] - right[0] * width, pos[1] - right[1] * width,
			pos[2] - right[2] * width};
		const float rgt[3] = {pos[0] + right[0] * width, pos[1] + right[1] * width,
			pos[2] + right[2] * width};
		push_vertex(out.vertices, head, 0.5f, 0.0f);
		push_vertex(out.vertices, left, 0.0f, 1.0f);
		push_vertex(out.vertices, rgt, 1.0f, 1.0f);
		++out.drops;
	}
}

} // namespace opennova::renderer
