#ifndef OPENNOVA_NOVA_CREDITS_PLAYER_H
#define OPENNOVA_NOVA_CREDITS_PLAYER_H

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/templates/hash_map.hpp>

#include "cbin_credits_resource.h"

namespace godot {

// Node that renders and scrolls a CbinCreditsResource.
// Creates child Control nodes and animates them vertically.
// Uses manual positioning to match original game behavior where ~F images
// don't consume vertical space.
class NovaCreditsPlayer : public Control {
	GDCLASS(NovaCreditsPlayer, Control);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void _process(double p_delta);
	NovaCreditsPlayer();
	~NovaCreditsPlayer();

	// Autoplay - start scrolling when node enters tree.
	void set_autoplay(bool p_autoplay);
	bool get_autoplay() const;

	// Resource to display.
	void set_credits_resource(const Ref<CbinCreditsResource> &p_resource);
	Ref<CbinCreditsResource> get_credits_resource() const;

	// Base paths for loading fonts and textures.
	void set_font_base_path(const String &p_path);
	String get_font_base_path() const;

	void set_texture_base_path(const String &p_path);
	String get_texture_base_path() const;

	// Playback control.
	void play();
	void stop();
	void pause();
	void resume();
	bool is_playing() const;

	// Scroll speed override (multiplier on resource's scroll_rate).
	void set_speed_scale(float p_scale);
	float get_speed_scale() const;

	// Current scroll position.
	void set_scroll_offset(float p_offset);
	float get_scroll_offset() const;

	// Editor helpers: entry lookup by scroll position and vice versa.
	int entry_index_at_scroll_center() const;
	float content_y_for_entry(int p_index) const;

	// Rebuild the visual tree from the resource.
	void rebuild();

	// Highlight/select a specific entry by index. Scrolls to show it and highlights it.
	void highlight_entry(int p_index);
	void clear_highlight();

private:
	void _process_scroll(double p_delta);
	void _rebuild_content();
	void _rebuild_content_if_needed();
	void _clear_content();
	void _update_fade_nodes();  // Update alpha for ~F images based on viewport position.
	void _on_resource_changed();  // Connected to credits_resource_->changed signal.

	Ref<CbinCreditsResource> credits_resource_;
	String font_base_path_;
	String texture_base_path_;

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
	// Logical scroll-stream Y for every entry. Fixed overlays render elsewhere,
	// but editor sync still needs their position in the credits stream.
	Vector<float> entry_stream_y_;
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

#endif  // OPENNOVA_NOVA_CREDITS_PLAYER_H
