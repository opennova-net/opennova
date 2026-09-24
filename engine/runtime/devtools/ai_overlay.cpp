#include <runtime/devtools/ai_overlay.h>

#include <runtime/devtools/entities_window.h>

#include <base/io/bam.h>
#include <base/io/fixed.h>

#include <cmath>
#include <string>
#include <unordered_map>

namespace opennova::devtools {

namespace {

world::Vec3 from_fixed(const int32_t p[3]) {
	return {static_cast<float>(p[0] / io::kFp16OneD), static_cast<float>(p[1] / io::kFp16OneD),
			static_cast<float>(p[2] / io::kFp16OneD)};
}

world::Vec3 lifted(const world::Vec3 &p, float dz) { return {p.x, p.y, p.z + dz}; }

float distance_sq(const world::Vec3 &a, const float eye[3]) {
	const float dx = a.x - eye[0];
	const float dy = a.y - eye[1];
	const float dz = a.z - eye[2];
	return dx * dx + dy * dy + dz * dz;
}

}  // namespace

uint32_t AiOverlayLayer::alert_color(int32_t alert, float alpha) {
	switch (alert) {
		case 1:
			return overlay_rgba(1.0f, 0.85f, 0.25f, alpha);
		case 2:
			return overlay_rgba(1.0f, 0.3f, 0.25f, alpha);
		default:
			return overlay_rgba(0.35f, 1.0f, 0.45f, alpha);
	}
}

const char *AiOverlayLayer::label() const {
	switch (element_) {
		case Element::Labels:
			return "Labels";
		case Element::Routes:
			return "Routes";
		case Element::Targets:
			return "Targets";
		case Element::Rings:
			return "Perception rings";
	}
	return "";
}

const char *AiOverlayLayer::tooltip() const {
	switch (element_) {
		case Element::Labels:
			return "State, move mode, route node and target over each brain within 150 units (the nearest 64).";
		case Element::Routes:
			return "The nav channels brains walk, node radii as diamonds, a line from each follower to its current node.";
		case Element::Targets:
			return "Brain -> combat target lines, the muzzle cross and a 10-unit aim ray.";
		case Element::Rings:
			return "Sight (faint) and attack (bright) ranges for the selected brain and up to 24 engaged brains nearby.";
	}
	return "";
}

int AiOverlayLayer::draw_priority() const {
	switch (element_) {
		case Element::Routes:
			return 10;
		case Element::Rings:
			return 20;
		case Element::Targets:
			return 50;
		case Element::Labels:
			return 70;
	}
	return 0;
}

std::string AiOverlayLayer::label_text(const world::inspect::AiOverlayRow &row) {
	std::string text = !row.name.empty() ? row.name : "ai " + std::to_string(row.ai_index);
	if (!row.alive) return text + "\nDEAD";
	// ai_state_name is "?" for the unnamed gaps; the number reads better.
	const std::string state = row.state_name.empty() || row.state_name == "?"
			? "state " + std::to_string(row.state)
			: row.state_name;
	text += "\n" + state;
	if (row.infantry) text += "  m" + std::to_string(row.move_mode);
	if (row.out_speed != 0) text += "  spd " + std::to_string(row.out_speed);
	if (row.wp_channel > 0) {
		text += "\nch " + std::to_string(row.wp_channel) + " node " + std::to_string(row.wp_node);
	}
	if (row.target_valid) {
		text += "\n-> " + (row.target_name.empty() ? std::string("?") : row.target_name);
		if (row.fire_delay > 0) text += "  fd " + std::to_string(row.fire_delay);
	}
	return text;
}

void AiOverlayLayer::draw(OverlayCanvas &canvas) {
	if (!snapshot_.valid) return;
	switch (element_) {
		case Element::Labels:
			draw_labels(canvas);
			break;
		case Element::Routes:
			draw_routes(canvas);
			break;
		case Element::Targets:
			draw_targets(canvas);
			break;
		case Element::Rings:
			draw_rings(canvas);
			break;
	}
}

void AiOverlayLayer::draw_labels(OverlayCanvas &canvas) const {
	const float range_sq = kLabelRange * kLabelRange;
	int drawn = 0;
	for (const world::inspect::AiOverlayRow &row : snapshot_.report.rows) {
		if (drawn >= kLabelMax) break;
		const world::Vec3 pos = from_fixed(row.pos);
		if (distance_sq(pos, canvas.camera().eye) > range_sq) continue;
		++drawn;
		const uint32_t color = row.alive ? alert_color(row.alert) : overlay_rgba(0.55f, 0.55f, 0.55f);
		canvas.text(lifted(pos, kLabelLift), label_text(row).c_str(), color);
	}
}

void AiOverlayLayer::draw_routes(OverlayCanvas &canvas) const {
	std::unordered_map<int32_t, const world::inspect::AiNavChannelRow *> by_index;
	for (const world::inspect::AiNavChannelRow &channel : snapshot_.report.channels) {
		by_index[channel.index] = &channel;
		// Only routes something walks: a mission authors far more channels
		// than its brains use (the AI window's table lists them all).
		if (channel.nodes.empty() || channel.followers <= 0) continue;
		const uint32_t color = overlay_index_color(channel.index);
		const uint32_t line_color = overlay_fade(color, 0.45f);
		const size_t n = channel.nodes.size();
		for (size_t k = 0; k + 1 < n; ++k) {
			canvas.line(from_fixed(channel.nodes[k].pos), from_fixed(channel.nodes[k + 1].pos), line_color);
		}
		// A looping path (loopflag bit0 clear) closes back on node 0.
		if ((channel.loopflag & 1) == 0 && n > 2) {
			canvas.line(from_fixed(channel.nodes[n - 1].pos), from_fixed(channel.nodes[0].pos), line_color);
		}
		for (const world::inspect::AiNavNodeRow &node : channel.nodes) {
			const float radius = static_cast<float>(node.radius_q16 / io::kFp16OneD);
			canvas.diamond(from_fixed(node.pos), radius > 0.35f ? radius : 0.35f, color);
		}
	}
	// Each follower's current node: a line from the brain and a bright cross.
	for (const world::inspect::AiOverlayRow &row : snapshot_.report.rows) {
		if (row.wp_channel <= 0) continue;
		const auto it = by_index.find(row.wp_channel);
		if (it == by_index.end()) continue;
		const auto &nodes = it->second->nodes;
		if (row.wp_node < 0 || row.wp_node >= static_cast<int32_t>(nodes.size())) continue;
		const uint32_t color = overlay_index_color(row.wp_channel);
		const world::Vec3 node = from_fixed(nodes[static_cast<size_t>(row.wp_node)].pos);
		canvas.line(from_fixed(row.pos), node, overlay_fade(color, 0.8f));
		canvas.cross(node, 0.5f, color);
	}
}

void AiOverlayLayer::draw_targets(OverlayCanvas &canvas) const {
	const uint32_t target_color = overlay_rgba(1.0f, 0.3f, 0.25f, 0.6f);
	const uint32_t aim_color = overlay_rgba(0.3f, 0.9f, 1.0f);
	for (const world::inspect::AiOverlayRow &row : snapshot_.report.rows) {
		if (!row.alive) continue;
		const world::Vec3 eye = lifted(from_fixed(row.pos), 1.5f);
		if (row.target_valid) canvas.line(eye, from_fixed(row.target_pos), target_color);
		const world::Vec3 muzzle = row.muzzle_valid ? from_fixed(row.muzzle) : eye;
		if (row.muzzle_valid) canvas.cross(muzzle, 0.15f, aim_color);
		if (row.aim_valid) {
			// The aim angles are BAM32 in the mission frame: bearing from +x
			// toward +y, pitch up from the ground plane.
			const double bearing = static_cast<double>(row.aim_heading) * io::kRadiansPerBam;
			const double pitch = static_cast<double>(row.aim_pitch) * io::kRadiansPerBam;
			const double cp = std::cos(pitch);
			const world::Vec3 end{
					muzzle.x + static_cast<float>(std::cos(bearing) * cp) * kAimRayLength,
					muzzle.y + static_cast<float>(std::sin(bearing) * cp) * kAimRayLength,
					muzzle.z + static_cast<float>(std::sin(pitch)) * kAimRayLength};
			canvas.line(muzzle, end, aim_color);
		}
	}
}

void AiOverlayLayer::draw_rings(OverlayCanvas &canvas) const {
	const uint16_t selected = entities_.selected_handle();
	const float range_sq = kLabelRange * kLabelRange;
	int rings = 0;
	for (const world::inspect::AiOverlayRow &row : snapshot_.report.rows) {
		if (!row.alive) continue;
		const world::Vec3 pos = from_fixed(row.pos);
		const bool is_selected = row.handle == selected && selected != world::EntityHandle::kInvalid;
		// The selection always; otherwise only engaged brains near the
		// camera, capped (a battlefield of circles is noise).
		if (!is_selected) {
			if (!row.target_valid || rings >= kRingMax) continue;
			if (distance_sq(pos, canvas.camera().eye) > range_sq) continue;
			++rings;
		}
		const float sight = static_cast<float>(row.sight_range_q16 / io::kFp16OneD);
		const float attack = static_cast<float>(row.attack_range_q16 / io::kFp16OneD);
		if (sight > 0.0f) canvas.ground_circle(pos, sight, alert_color(row.alert, 0.25f));
		if (attack > 0.0f) canvas.ground_circle(pos, attack, alert_color(row.alert, 0.7f));
	}
}

}  // namespace opennova::devtools
