// The Physics window's hit-mesh layer (overlay_canvas.h), the ImGui return of
// the retired GDScript hitbox view: the round hit-detection reality around
// the camera — the transformed CFAC bullet meshes of nearby statics and
// vehicles (the triangles the projectile raycast walks), coloured by face
// material, with their broad-phase bound spheres, and the posed person
// section spheres coloured by the damage zone they resolve to (orange x1.25
// torso, cyan x1.0, lime x0.5 limbs, magenta x3.0 head, dark red masked,
// amber the bounded stand-in). The engine oracle builds the report
// (mission/debug_oracles.h collect_debug_hitboxes); the embedder refreshes it
// at kRefreshHz while the layer is on.
#pragma once

#include <runtime/devtools/overlay_canvas.h>
#include <runtime/mission/debug_oracles.h>

#include <cstdint>

namespace opennova::devtools {

struct HitboxOverlayRecord {
	bool valid = false;
	uint64_t logic_tick = 0;
	mission::DebugHitboxReport report;
};

class HitboxOverlayLayer : public OverlayLayer {
public:
	static constexpr double kRefreshHz = 6.0;
	static constexpr int kLabelNearest = 12;

	explicit HitboxOverlayLayer(const HitboxOverlayRecord &record) : record_(record) {}
	const char *group() const override { return "Collision"; }
	const char *label() const override { return "Hit meshes"; }
	const char *tooltip() const override {
		return "The bullet meshes and person section spheres rounds resolve against, around the camera "
			   "(80 units, 96 bodies). Spheres are coloured by damage zone; amber marks a stand-in.";
	}
	int draw_priority() const override { return 0; }
	int line_budget() const override { return 12000; }
	int text_budget() const override { return kLabelNearest; }
	void draw(OverlayCanvas &canvas) override;

	// The damage-zone colour of a person section (masked wins, then the
	// stand-in, then the zone's multiplier class).
	static uint32_t section_color(int32_t section, bool masked, bool fallback);

private:
	const HitboxOverlayRecord &record_;
};

}  // namespace opennova::devtools
