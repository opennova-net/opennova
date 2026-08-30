// The Entity Properties window (ADR 0042 d6): the Entities window's selected
// row as a card — identity, item, health, AI state — with its debug actions
// (set health, set position, teleport the local player here) and both
// items.def attrib words as keyword-labelled checkboxes, over the
// EntityDetailSnapshot the embedder pushes for that selection. Its own
// window so the list and the card dock independently (a long table beside
// the game, the card underneath, or torn out onto another monitor).
//
// The window owns no selection: it reads the Entities window's (selected
// row, pending handle) and queues its typed requests into that window's
// queue, so the embedder drains one queue. Visibility-armed: while hidden it
// drops its card and the embedder (gated on GameDevTools::needs_entity_detail)
// stops building new ones. A card that no longer names the selection reads
// as invalid (the selection moved; the embedder pushes the new card at once).
#pragma once

#include <runtime/devtools/debug_request.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/devtools/entity_detail_snapshot.h>

#include <cstdint>

namespace opennova::devtools {

class EntitiesWindow;

class EntityPropertiesWindow : public Window {
public:
	explicit EntityPropertiesWindow(EntitiesWindow &entities) : entities_(entities) {}

	const char *title() const override { return "Entity Properties"; }
	InitialDockPlacement initial_dock_placement() const override {
		return InitialDockPlacement::RightBottom;
	}
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// The pushed detail record for the selection, by value: accepted only
	// when its card names the selected handle (a late push for a previous
	// selection is dropped); an invalid card clears the pane. The attrib edit
	// words re-seed from every accepted push.
	void set_detail(EntityDetailSnapshot detail);
	// (pass open && window open && a selection): the embedder skips building
	// cards nobody shows.
	bool wants_detail() const;
	bool detail_valid() const;
	uint16_t detail_handle() const { return detail_.card.handle; }
	uint32_t detail_attrib() const { return attrib_edit_; }
	uint32_t detail_attrib2() const { return attrib2_edit_; }
	int32_t detail_health_max() const { return detail_.card.world.health_max; }
	const char *detail_item_name() const { return detail_.card.world.item_name.c_str(); }

	// One items.def attrib bit flipped on the selected entity: the edit word
	// changes and one SetEntityItemAttrib request carrying both full words
	// leaves through the Entities window's queue. The checkbox handlers call
	// these; they are also the headless test seam (clicking a checkbox needs
	// a real backend). No-ops without a valid detail card.
	void toggle_item_attrib(uint32_t bit);
	void toggle_item_attrib2(uint32_t bit);

private:
	void seed_edits_from_selection();
	void draw_actions();
	void draw_card();
	void draw_attrib_grid(const char *label, bool second_word);

	EntitiesWindow &entities_;
	EntityDetailSnapshot detail_{};
	uint32_t attrib_edit_ = 0;
	uint32_t attrib2_edit_ = 0;
	uint16_t seeded_handle_ = 0xFFFF; // the row the action edits were seeded from
	int32_t health_edit_ = 0;
	float pos_edit_[3] = {0.0f, 0.0f, 0.0f};
	float yaw_edit_ = 0.0f;
	float pitch_edit_ = 0.0f;
	bool shown_ = false;
};

}  // namespace opennova::devtools
