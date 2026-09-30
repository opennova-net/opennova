#pragma once

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_minimap.h>

#include <vector>

namespace godot {

// The textures one map pass samples: the colormap atlas, the depthspin water
// mask, the texture-slot table the sprites index (kHudTexMapIcons + the
// sprite's texture offset) and the font page table the map glyphs index.
struct HudMapPassTextures {
	Ref<Texture2D> terrain;
	Ref<Texture2D> water;
	const Ref<Texture2D> *slots = nullptr;
	int slot_count = 0;
	const Ref<Texture2D> *pages = nullptr;
	size_t page_count = 0;
};

// The device leg of one compiled map pass (engine/runtime/hud/hud_minimap.h
// HudMapPass): four pinned-order child canvas items under a parent item —
// the base (a pass's rect clear + terrain), the additive terrain resubmission (the
// retail decal stage's x4 output split across two 1x items), the depthspin
// water cutout, and the top item (grid rules, footprints, sprites, lines,
// glyphs, then any over-lines). The corner spinmap, the M-cycle big map and
// the DEATH MAP window each own one; the materials stay their owner's.
class HudMapPassRenderer {
public:
	HudMapPassRenderer() = default;
	~HudMapPassRenderer();
	HudMapPassRenderer(const HudMapPassRenderer &) = delete;
	HudMapPassRenderer &operator=(const HudMapPassRenderer &) = delete;

	// Create the four items under `parent` (idempotent) at draw indices
	// first_draw_index..+3, optionally behind the parent's own commands.
	void ensure(const RID &parent, int first_draw_index, bool behind_parent,
			const RID &additive_material, const RID &water_material);
	bool is_ready() const;
	// Clear every item's commands (the owner's per-draw reset).
	void clear();
	// Free the items (the owner's teardown).
	void release();
	// Place every item under a transform (a host whose pass coordinates are
	// its parent's, not its own).
	void set_transform(const Transform2D &transform);
	void render(const opennova::hud::HudMapPass &map,
			const std::vector<opennova::hud::GameFontQuad> &glyphs,
			const HudMapPassTextures &textures,
			const std::vector<opennova::hud::HudMapLine> *over_lines = nullptr);

	bool water_sampling_configured() const { return water_sampling_configured_; }
	bool top_sampling_configured() const { return top_sampling_configured_; }

private:
	RID base_item_;
	RID add_item_;
	RID water_item_;
	RID top_item_;
	bool water_sampling_configured_ = false;
	bool top_sampling_configured_ = false;
};

} // namespace godot
