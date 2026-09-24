#include <runtime/devtools/overlay_canvas.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace opennova::devtools {

namespace {

// 16-bit draw indices address 65536 vertices per draw command; without the
// backend's vertex-offset support a list past that corrupts silently (release
// builds compile ImGui's assert out), so the canvas stops short of it.
constexpr int kVertexGuard = 60000;

constexpr float kPi = 3.14159265358979f;

struct P3 {
	float v[3];
	explicit P3(const world::Vec3 &p) : v{p.x, p.y, p.z} {}
};

}  // namespace

uint32_t overlay_rgba(float r, float g, float b, float a) {
	const auto byte = [](float v) {
		const int scaled = static_cast<int>(v * 255.0f + 0.5f);
		return static_cast<uint32_t>(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
	};
	return IM_COL32(byte(r), byte(g), byte(b), byte(a));
}

uint32_t overlay_index_color(int index, float alpha) {
	const double hue = std::fmod(static_cast<double>(index) * 0.618033988749895, 1.0);
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	ImGui::ColorConvertHSVtoRGB(static_cast<float>(hue), 0.75f, 1.0f, r, g, b);
	return overlay_rgba(r, g, b, alpha);
}

uint32_t overlay_fade(uint32_t rgba, float alpha) {
	const uint32_t a = (rgba >> IM_COL32_A_SHIFT) & 0xFFu;
	const float scaled = static_cast<float>(a) * std::clamp(alpha, 0.0f, 1.0f);
	return (rgba & ~IM_COL32_A_MASK) | (static_cast<uint32_t>(scaled + 0.5f) << IM_COL32_A_SHIFT);
}

OverlayCanvas::OverlayCanvas(ImDrawList *draw_list, const OverlayCamera &camera,
		const OverlayRect &rect)
		: draw_list_(draw_list), camera_(camera), rect_(rect) {}

bool OverlayCanvas::project(const world::Vec3 &p, float out[2]) const {
	return overlay_project_point(camera_, rect_, P3(p).v, out);
}

float OverlayCanvas::distance(const world::Vec3 &p) const {
	const float dx = p.x - camera_.eye[0];
	const float dy = p.y - camera_.eye[1];
	const float dz = p.z - camera_.eye[2];
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void OverlayCanvas::begin_layer(int line_budget, int text_budget) {
	line_budget_ = line_budget;
	text_budget_ = text_budget;
	stats_ = OverlayLayerStats{};
}

bool OverlayCanvas::has_room() const {
	if (draw_list_ == nullptr) return false;
	if ((ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0) return true;
	return draw_list_->VtxBuffer.Size < kVertexGuard;
}

bool OverlayCanvas::take_line() {
	if (stats_.lines >= line_budget_ || !has_room()) {
		++stats_.dropped;
		return false;
	}
	++stats_.lines;
	return true;
}

bool OverlayCanvas::take_text() {
	if (stats_.texts >= text_budget_ || !has_room()) {
		++stats_.dropped;
		return false;
	}
	++stats_.texts;
	return true;
}

void OverlayCanvas::line(const world::Vec3 &a, const world::Vec3 &b, uint32_t rgba, float thickness) {
	float sa[2];
	float sb[2];
	if (!overlay_project_segment(camera_, rect_, P3(a).v, P3(b).v, sa, sb)) return;
	if (!take_line()) return;
	draw_list_->AddLine(ImVec2(sa[0], sa[1]), ImVec2(sb[0], sb[1]), rgba, thickness);
}

void OverlayCanvas::cross(const world::Vec3 &p, float half_size, uint32_t rgba) {
	line({p.x - half_size, p.y, p.z}, {p.x + half_size, p.y, p.z}, rgba);
	line({p.x, p.y - half_size, p.z}, {p.x, p.y + half_size, p.z}, rgba);
	line({p.x, p.y, p.z - half_size}, {p.x, p.y, p.z + half_size}, rgba);
}

void OverlayCanvas::diamond(const world::Vec3 &c, float r, uint32_t rgba) {
	const world::Vec3 n{c.x, c.y + r, c.z};
	const world::Vec3 e{c.x + r, c.y, c.z};
	const world::Vec3 s{c.x, c.y - r, c.z};
	const world::Vec3 w{c.x - r, c.y, c.z};
	line(n, e, rgba);
	line(e, s, rgba);
	line(s, w, rgba);
	line(w, n, rgba);
}

void OverlayCanvas::ground_circle(const world::Vec3 &c, float r, uint32_t rgba, int segments) {
	segments = std::max(8, segments);
	world::Vec3 prev{c.x + r, c.y, c.z};
	for (int i = 1; i <= segments; ++i) {
		const float t = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(segments);
		const world::Vec3 next{c.x + r * std::cos(t), c.y + r * std::sin(t), c.z};
		line(prev, next, rgba);
		prev = next;
	}
}

void OverlayCanvas::box(const world::Vec3 &mn, const world::Vec3 &mx, uint32_t rgba) {
	const world::Vec3 c[8] = {
			{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
			{mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
	static constexpr int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
			{6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
	for (const auto &edge : kEdges) line(c[edge[0]], c[edge[1]], rgba);
}

void OverlayCanvas::triangle(const world::Vec3 &a, const world::Vec3 &b, const world::Vec3 &c,
		uint32_t rgba) {
	line(a, b, rgba);
	line(b, c, rgba);
	line(c, a, rgba);
}

void OverlayCanvas::sphere_outline(const world::Vec3 &center, float r, uint32_t rgba) {
	float sc[2];
	float st[2];
	if (!project(center, sc)) return;
	// The radius on screen: the centre raised by r along the mission up axis.
	if (!project({center.x, center.y, center.z + r}, st)) return;
	const float radius = std::hypot(st[0] - sc[0], st[1] - sc[1]);
	if (radius < 0.5f || radius > rect_.width() * 4.0f) return;
	if (!take_line()) return;
	draw_list_->AddCircle(ImVec2(sc[0], sc[1]), radius, rgba, 0, 1.2f);
}

void OverlayCanvas::text(const world::Vec3 &anchor, const char *label, uint32_t rgba) {
	float s[2];
	if (label == nullptr || label[0] == '\0' || !project(anchor, s)) return;
	if (s[0] < rect_.min_x || s[0] > rect_.max_x || s[1] < rect_.min_y || s[1] > rect_.max_y) return;
	if (!take_text()) return;
	const ImVec2 size = ImGui::CalcTextSize(label);
	const ImVec2 pos(s[0] - size.x * 0.5f, s[1] - size.y - 2.0f);
	draw_list_->AddRectFilled(ImVec2(pos.x - 3.0f, pos.y - 1.0f),
			ImVec2(pos.x + size.x + 3.0f, pos.y + size.y + 1.0f), IM_COL32(0, 0, 0, 150), 3.0f);
	draw_list_->AddText(pos, rgba, label);
}

}  // namespace opennova::devtools
