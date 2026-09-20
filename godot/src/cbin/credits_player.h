#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/templates/hash_map.hpp>

#include "cbin/cbin_credits_resource.h"

namespace godot {

// Node that renders and scrolls a CbinCreditsResource.
// Creates child Control nodes and animates them vertically.
// Uses manual positioning to match original game behavior where ~F images
// don't consume vertical space.
class CreditsPlayer : public Control {
	GDCLASS(CreditsPlayer, Control);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void _process(double p_delta);
	CreditsPlayer();
	~CreditsPlayer();

	// Autoplay - start scrolling when node enters tree.
	void set_autoplay(bool p_autoplay);

	// Resource to display.
	void set_credits_resource(const Ref<CbinCreditsResource> &p_resource);

	// Playback control.
	void play();
	void stop();
	void pause();
	void resume();
	bool is_playing() const;

	// Rebuild the visual tree from the resource.
	void rebuild();

	// Highlight/select a specific entry by index. Scrolls to show it and highlights it.
	void clear_highlight();

private:
	void _process_scroll(double p_delta);
	void _rebuild_content();
	void _rebuild_content_if_needed();
	void _clear_content();
	void _update_fade_nodes();  // Update alpha for ~F images based on viewport position.
	void _on_resource_changed();  // Connected to credits_resource_->changed signal.

	Ref<CbinCreditsResource> credits_resource_;

	Control *content_ = nullptr;
	float content_height_ = 0.0f;  // Total height of content for scroll bounds.
	float scroll_offset_ = 0.0f;
	float speed_scale_ = 1.0f;
	bool playing_ = false;
	bool paused_ = false;
	bool needs_rebuild_ = false;
	bool autoplay_ = false;

	// Maps entry index to the visual node created for it (if any).
	HashMap<int, Control *> entry_to_node_;
	int highlighted_entry_ = -1;
	Control *highlight_node_ = nullptr;
	Color original_color_ = Color(1, 1, 1);

	// ~F images: fixed overlays with fade effect.
	// Each entry tracks the node and its "trigger Y" (position in scroll stream for fade calc).
	struct FadeOverlay {
		Control *node = nullptr;
		float trigger_y = 0.0f;  // Y position in content space (determines fade)
	};
	Vector<FadeOverlay> fade_overlays_;
	// Uncited: the original ~F fade behavior/zone is unwitnessed (R5 in
	// docs/oned/workspace-maturity-program.md). Do not cite without a grill.
	static constexpr float kFadeZonePixels = 50.0f;
};

}  // namespace godot
