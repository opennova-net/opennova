#include <editor/preview/effect_schedule.h>

#include <algorithm>
#include <cmath>

#include <base/io/bam.h>
#include <base/io/tick_rate.h>
#include <runtime/renderer/render_order.h>

namespace opennova::editor {

namespace {

constexpr float kDegreesToRadians = float(io::kRadiansPerDegree);

// The plane through `eye` (or offset along its normal by `offset`) whose inside is along `normal`, normalized.
void set_plane(float out[4], PreviewVec3 normal, const PreviewVec3 &eye, float offset) {
	const float length = std::sqrt(dot(normal, normal));
	if (length > 0.0f) {
		normal.x /= length;
		normal.y /= length;
		normal.z /= length;
	}
	out[0] = normal.x;
	out[1] = normal.y;
	out[2] = normal.z;
	out[3] = -dot(normal, eye) - offset;
}

PreviewVec3 combine(const PreviewVec3 &a, float scale, const PreviewVec3 &b, float sign) {
	return PreviewVec3{ a.x * scale + b.x * sign, a.y * scale + b.y * sign, a.z * scale + b.z * sign };
}

// The six planes at `camera`'s eye and axes, `near_z` and `far_z` ahead of it.
particle::ParticleViewFrustum view_frustum(
		const OrbitCamera &camera, int width, int height, float near_z, float far_z) {
	particle::ParticleViewFrustum out;
	if (width <= 0 || height <= 0) return out;
	PreviewVec3 right, up, back;
	camera.axes(right, up, back);
	const PreviewVec3 eye = camera.eye();
	const PreviewVec3 forward{ -back.x, -back.y, -back.z };
	const float across = std::tan(camera.fov_degrees() * 0.5f * kDegreesToRadians);
	const float down = across * float(height) / float(width);
	if (!std::isfinite(across) || across <= 0.0f) return out;
	// Near and far: ahead of the eye by the near plane, short of the far one.
	set_plane(out.planes[0], forward, eye, near_z);
	set_plane(out.planes[1], back, eye, -far_z);
	// The sides: a point ahead whose offset across (or up) is within the field's tangent of its depth.
	set_plane(out.planes[2], combine(forward, across, right, 1.0f), eye, 0.0f);
	set_plane(out.planes[3], combine(forward, across, right, -1.0f), eye, 0.0f);
	set_plane(out.planes[4], combine(forward, down, up, 1.0f), eye, 0.0f);
	set_plane(out.planes[5], combine(forward, down, up, -1.0f), eye, 0.0f);
	out.valid = true;
	return out;
}

} // namespace

particle::ParticleViewFrustum preview_view_frustum(
		const OrbitCamera &camera, int width, int height) {
	return view_frustum(camera, width, height, camera.near_plane, camera.far_plane);
}

particle::ParticleViewFrustum scene_pass_view_frustum(
		const OrbitCamera &camera, int width, int height, float fog_distance) {
	if (!(fog_distance > 0.0f))
		return particle::ParticleViewFrustum();
	return view_frustum(camera, width, height, renderer::kScenePassNearZ,
			renderer::scene_far_plane(fog_distance));
}

void EffectSchedule::hold(int32_t tick) {
	tick_ = std::max(tick, 0);
}

void EffectSchedule::reset() {
	tick_ = 0;
	played_ = false;
	jump_ = true;
}

bool EffectSchedule::jumps_to(int32_t tick) const {
	tick = std::max(tick, 0);
	return !(played_ && !jump_ && tick >= tick_ && tick - tick_ <= int32_t(particle::kEffectAdvanceTickLimit));
}

bool EffectSchedule::play_to(particle::EffectScene &scene, int32_t tick, const particle::ParticleViewFrustum &frustum,
		EffectTimedSpawns &timed, const Make &make, const Jumped &jumped) {
	tick = std::max(tick, 0);
	timed.made.resize(timed.ticks.size(), false);
	if (jumps_to(tick)) {
		// A jump: the scene emptied, what the owner holds standing made again, each timed spawn due made again at its
		// age then.
		scene.reset_runtime_state();
		std::fill(timed.made.begin(), timed.made.end(), false);
		if (jumped) jumped(tick);
		for (size_t i = 0; i < timed.ticks.size(); ++i)
			if (timed.ticks[i] <= tick) {
				timed.made[i] = true;
				make(i, tick - timed.ticks[i]);
			}
		jump_ = false;
		played_ = true;
		tick_ = tick;
		return true;
	}
	// A spawn added after the clock passed its tick: made at once, pre-aged by its age.
	for (size_t i = 0; i < timed.ticks.size(); ++i)
		if (!timed.made[i] && timed.ticks[i] <= tick_) {
			timed.made[i] = true;
			make(i, tick_ - timed.ticks[i]);
		}
	// A game tick at a time (the scene's own fixed step), each spawn made on its tick.
	particle::EffectAdvanceRequest step;
	step.delta_seconds = 1.0f / static_cast<float>(io::kTickHz);
	step.frustum = frustum;
	for (int32_t at = tick_ + 1; at <= tick; ++at) {
		scene.advance_simulation(step);
		for (size_t i = 0; i < timed.ticks.size(); ++i)
			if (timed.ticks[i] == at && !timed.made[i]) {
				timed.made[i] = true;
				make(i, 0);
			}
	}
	const bool moved = tick != tick_;
	tick_ = tick;
	return moved;
}

} // namespace opennova::editor
