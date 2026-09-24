#include <runtime/devtools/entity_overlay.h>

#include <algorithm>
#include <cstdio>

namespace opennova::devtools {

namespace {

// Label anchors sit above the entity's origin (its feet for an organic).
constexpr float kLabelLift = 2.2f;

// The BMS name, else the items.def name (an unnamed placement), else "?".
const char *marker_name(const world::inspect::EntityMarker &m) {
	if (!m.name.empty()) return m.name.c_str();
	if (!m.item_name.empty()) return m.item_name.c_str();
	return "?";
}

void marker_text(const world::inspect::EntityMarker &m, char *buf, size_t size) {
	if (m.health_max > 0) {
		std::snprintf(buf, size, "%s  #%d  hp %d/%d", marker_name(m), m.bms_id, m.health, m.health_max);
	} else {
		std::snprintf(buf, size, "%s  #%d  hp %d", marker_name(m), m.bms_id, m.health);
	}
}

}  // namespace

void EntitySelectionLayer::draw(OverlayCanvas &canvas) {
	if (!record_.valid) return;
	const auto it = std::find_if(record_.rows.begin(), record_.rows.end(),
			[](const world::inspect::EntityMarker &m) { return m.selected; });
	if (it == record_.rows.end()) return;
	const world::inspect::EntityMarker &m = *it;
	const uint32_t color = overlay_rgba(1.0f, 0.85f, 0.2f);
	const world::Vec3 &p = m.mission_position;
	const float radius = std::max(m.bound_radius, 0.75f);
	canvas.diamond(p, radius, color);
	canvas.ground_circle(p, radius * 1.3f, overlay_fade(color, 0.5f), 24);
	canvas.line(p, {p.x, p.y, p.z + kLabelLift}, overlay_fade(color, 0.7f));
	canvas.cross({p.x, p.y, p.z + kLabelLift * 0.5f}, 0.3f, color);
	char text[160];
	marker_text(m, text, sizeof(text));
	canvas.text({p.x, p.y, p.z + kLabelLift + 0.3f}, text, color);
}

void EntityLabelsLayer::draw(OverlayCanvas &canvas) {
	if (!record_.valid) return;
	for (const world::inspect::EntityMarker &m : record_.rows) {
		if (m.selected) continue; // the Selection layer owns its label
		const uint32_t color = !m.alive ? overlay_rgba(0.55f, 0.55f, 0.55f)
				: m.team >= 0          ? overlay_index_color(m.team)
									   : overlay_rgba(0.85f, 0.85f, 0.85f);
		char text[160];
		marker_text(m, text, sizeof(text));
		if (m.team >= 0) {
			const size_t len = std::char_traits<char>::length(text);
			std::snprintf(text + len, sizeof(text) - len, "  T%d", m.team);
		}
		const world::Vec3 &p = m.mission_position;
		canvas.text({p.x, p.y, p.z + kLabelLift}, text, color);
	}
}

}  // namespace opennova::devtools
