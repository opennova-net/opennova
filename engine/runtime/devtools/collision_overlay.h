// The Rays and Physics windows' world-space layers (overlay_canvas.h): every
// captured raycast coloured by its category and faded over the capture's TTL,
// and every captured contact as a fading cross with a ring on the body it
// touched. Both draw rows the engine filters by the capture's own mask and
// fade window (world/collision_debug_rows.h), so the windows' category/kind
// checkboxes and the Rays "Fade ticks" slider decide what the Game view shows.
// The embedder pushes the rows per logic tick while a layer is on.
#pragma once

#include <runtime/devtools/overlay_canvas.h>
#include <runtime/world/collision_debug_rows.h>

#include <cstdint>
#include <vector>

namespace opennova::devtools {

struct RaysOverlayRecord {
	bool valid = false;
	uint64_t logic_tick = 0;
	int32_t ttl_ticks = 1;
	std::vector<world::RayDebugRow> rows;
};

struct ContactsOverlayRecord {
	bool valid = false;
	uint64_t logic_tick = 0;
	int32_t ttl_ticks = 1;
	std::vector<world::ContactDebugRow> rows;
};

class RaysOverlayLayer : public OverlayLayer {
public:
	explicit RaysOverlayLayer(const RaysOverlayRecord &record) : record_(record) {}
	const char *group() const override { return "Collision"; }
	const char *label() const override { return "Rays"; }
	const char *tooltip() const override {
		return "Every captured raycast, coloured by category and faded over the Rays window's Fade ticks; "
			   "a hit ends in a cross, a blocked query is dimmed. The Rays window's checkboxes filter it.";
	}
	int draw_priority() const override { return 30; }
	int line_budget() const override { return 8000; }
	void draw(OverlayCanvas &canvas) override;

private:
	const RaysOverlayRecord &record_;
};

class ContactsOverlayLayer : public OverlayLayer {
public:
	explicit ContactsOverlayLayer(const ContactsOverlayRecord &record) : record_(record) {}
	const char *group() const override { return "Collision"; }
	const char *label() const override { return "Contacts"; }
	const char *tooltip() const override {
		return "Every captured hit and contact as a cross fading over one second, with a ring on the body it "
			   "touched. The Physics window's checkboxes filter it.";
	}
	int draw_priority() const override { return 40; }
	void draw(OverlayCanvas &canvas) override;

private:
	const ContactsOverlayRecord &record_;
};

}  // namespace opennova::devtools
