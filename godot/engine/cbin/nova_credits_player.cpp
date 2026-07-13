#include "nova_credits_player.h"

#include "fnt/nova_fnt_resource.h"

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

namespace {

// Position a control horizontally based on justification within a text area.
// text_area_left: left boundary of the text area
// text_area_right: right boundary of the text area
constexpr float kRightMargin = 10.0f;  // Margin to prevent text clipping on right edge.

void position_control_horizontal(Control *node, float node_width, HorizontalAlignment alignment,
                                  float text_area_left, float text_area_right) {
	float x = 0.0f;
	float text_area_width = text_area_right - text_area_left;
	float center_x = text_area_left + text_area_width / 2.0f;

	switch (alignment) {
		case HORIZONTAL_ALIGNMENT_LEFT:
			// Left-aligned: position at left edge of text area.
			x = text_area_left;
			break;
		case HORIZONTAL_ALIGNMENT_CENTER:
			// Centered: position so center of node is at center of text area.
			x = center_x - node_width / 2.0f;
			break;
		case HORIZONTAL_ALIGNMENT_RIGHT:
			// Right-aligned: position so right edge is at right edge of text area minus margin.
			x = text_area_right - node_width - kRightMargin;
			break;
		default:
			x = center_x - node_width / 2.0f;
			break;
	}
	node->set_position(Vector2(x, node->get_position().y));
}

}  // namespace

void NovaCreditsPlayer::_bind_methods() {
	// Resource property.
	ClassDB::bind_method(D_METHOD("set_credits_resource", "resource"), &NovaCreditsPlayer::set_credits_resource);
	ClassDB::bind_method(D_METHOD("get_credits_resource"), &NovaCreditsPlayer::get_credits_resource);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "credits_resource", PROPERTY_HINT_RESOURCE_TYPE,
	                          "CbinCreditsResource"),
	             "set_credits_resource", "get_credits_resource");

	// Path properties.
	ClassDB::bind_method(D_METHOD("set_font_base_path", "path"), &NovaCreditsPlayer::set_font_base_path);
	ClassDB::bind_method(D_METHOD("get_font_base_path"), &NovaCreditsPlayer::get_font_base_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "font_base_path", PROPERTY_HINT_DIR), "set_font_base_path",
	             "get_font_base_path");

	ClassDB::bind_method(D_METHOD("set_texture_base_path", "path"), &NovaCreditsPlayer::set_texture_base_path);
	ClassDB::bind_method(D_METHOD("get_texture_base_path"), &NovaCreditsPlayer::get_texture_base_path);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "texture_base_path", PROPERTY_HINT_DIR), "set_texture_base_path",
	             "get_texture_base_path");

	// Speed scale.
	ClassDB::bind_method(D_METHOD("set_speed_scale", "scale"), &NovaCreditsPlayer::set_speed_scale);
	ClassDB::bind_method(D_METHOD("get_speed_scale"), &NovaCreditsPlayer::get_speed_scale);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "speed_scale"), "set_speed_scale", "get_speed_scale");

	// Playback control.
	ClassDB::bind_method(D_METHOD("play"), &NovaCreditsPlayer::play);
	ClassDB::bind_method(D_METHOD("stop"), &NovaCreditsPlayer::stop);
	ClassDB::bind_method(D_METHOD("pause"), &NovaCreditsPlayer::pause);
	ClassDB::bind_method(D_METHOD("resume"), &NovaCreditsPlayer::resume);
	ClassDB::bind_method(D_METHOD("is_playing"), &NovaCreditsPlayer::is_playing);

	// Scroll offset.
	ClassDB::bind_method(D_METHOD("set_scroll_offset", "offset"), &NovaCreditsPlayer::set_scroll_offset);
	ClassDB::bind_method(D_METHOD("get_scroll_offset"), &NovaCreditsPlayer::get_scroll_offset);

	// Rebuild.
	ClassDB::bind_method(D_METHOD("rebuild"), &NovaCreditsPlayer::rebuild);
	ClassDB::bind_method(D_METHOD("_rebuild_content_if_needed"), &NovaCreditsPlayer::_rebuild_content_if_needed);

	// Highlight.
	ClassDB::bind_method(D_METHOD("highlight_entry", "index"), &NovaCreditsPlayer::highlight_entry);
	ClassDB::bind_method(D_METHOD("clear_highlight"), &NovaCreditsPlayer::clear_highlight);

	// Autoplay.
	ClassDB::bind_method(D_METHOD("set_autoplay", "autoplay"), &NovaCreditsPlayer::set_autoplay);
	ClassDB::bind_method(D_METHOD("get_autoplay"), &NovaCreditsPlayer::get_autoplay);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autoplay"), "set_autoplay", "get_autoplay");

	// Entry-lookup helpers (for editor scroll sync).
	ClassDB::bind_method(D_METHOD("entry_index_at_scroll_center"), &NovaCreditsPlayer::entry_index_at_scroll_center);
	ClassDB::bind_method(D_METHOD("content_y_for_entry", "index"), &NovaCreditsPlayer::content_y_for_entry);

	// Signals.
	ADD_SIGNAL(MethodInfo("finished"));
	ADD_SIGNAL(MethodInfo("started"));
	ADD_SIGNAL(MethodInfo("scroll_offset_changed", PropertyInfo(Variant::FLOAT, "offset")));
}

NovaCreditsPlayer::NovaCreditsPlayer() {
	set_clip_contents(true);
}

NovaCreditsPlayer::~NovaCreditsPlayer() {}

void NovaCreditsPlayer::set_autoplay(bool p_autoplay) {
	autoplay_ = p_autoplay;
}

bool NovaCreditsPlayer::get_autoplay() const {
	return autoplay_;
}

void NovaCreditsPlayer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY:
			// Defer rebuild to ensure size is available after layout.
			if (credits_resource_.is_valid()) {
				call_deferred("rebuild");
				if (autoplay_) {
					call_deferred("play");
				}
			}
			break;
		case NOTIFICATION_RESIZED:
			// Rebuild when control size changes to update layout.
			if (credits_resource_.is_valid() && is_inside_tree()) {
				_rebuild_content();
			}
			break;
	}
}

void NovaCreditsPlayer::_process(double p_delta) {
	if (playing_ && !paused_) {
		_process_scroll(p_delta);
	}
}

void NovaCreditsPlayer::set_credits_resource(const Ref<CbinCreditsResource> &p_resource) {
	if (credits_resource_ == p_resource) return;

	if (credits_resource_.is_valid() &&
	    credits_resource_->is_connected("changed", callable_mp(this, &NovaCreditsPlayer::_on_resource_changed))) {
		credits_resource_->disconnect("changed", callable_mp(this, &NovaCreditsPlayer::_on_resource_changed));
	}

	credits_resource_ = p_resource;
	needs_rebuild_ = true;

	if (credits_resource_.is_valid()) {
		credits_resource_->connect("changed", callable_mp(this, &NovaCreditsPlayer::_on_resource_changed));
	}

	if (is_inside_tree()) {
		_rebuild_content();
	}
}

void NovaCreditsPlayer::_on_resource_changed() {
	if (!is_inside_tree()) {
		needs_rebuild_ = true;
		return;
	}
	if (!needs_rebuild_) {
		needs_rebuild_ = true;
		call_deferred("_rebuild_content_if_needed");
	}
}

void NovaCreditsPlayer::_rebuild_content_if_needed() {
	if (!needs_rebuild_) {
		return;
	}
	if (is_inside_tree()) {
		_rebuild_content();
	}
}

Ref<CbinCreditsResource> NovaCreditsPlayer::get_credits_resource() const {
	return credits_resource_;
}

void NovaCreditsPlayer::set_font_base_path(const String &p_path) {
	font_base_path_ = p_path;
	if (!font_base_path_.is_empty() && !font_base_path_.ends_with("/")) {
		font_base_path_ += "/";
	}
}

String NovaCreditsPlayer::get_font_base_path() const {
	return font_base_path_;
}

void NovaCreditsPlayer::set_texture_base_path(const String &p_path) {
	texture_base_path_ = p_path;
	if (!texture_base_path_.is_empty() && !texture_base_path_.ends_with("/")) {
		texture_base_path_ += "/";
	}
}

String NovaCreditsPlayer::get_texture_base_path() const {
	return texture_base_path_;
}

void NovaCreditsPlayer::play() {
	if (!credits_resource_.is_valid()) return;

	if (needs_rebuild_) {
		_rebuild_content();
	}

	scroll_offset_ = 0.0f;
	emit_signal("scroll_offset_changed", scroll_offset_);
	playing_ = true;
	paused_ = false;
	set_process(true);

	if (content_) {
		content_->set_position(Vector2(0, get_size().y));
	}

	emit_signal("started");
}

void NovaCreditsPlayer::stop() {
	playing_ = false;
	paused_ = false;
	scroll_offset_ = 0.0f;
	emit_signal("scroll_offset_changed", scroll_offset_);
	set_process(false);

	if (content_) {
		content_->set_position(Vector2(0, get_size().y));
	}
}

void NovaCreditsPlayer::pause() {
	paused_ = true;
}

void NovaCreditsPlayer::resume() {
	paused_ = false;
}

bool NovaCreditsPlayer::is_playing() const {
	return playing_ && !paused_;
}

void NovaCreditsPlayer::set_speed_scale(float p_scale) {
	speed_scale_ = p_scale;
}

float NovaCreditsPlayer::get_speed_scale() const {
	return speed_scale_;
}

void NovaCreditsPlayer::set_scroll_offset(float p_offset) {
	if (scroll_offset_ == p_offset) {
		return;
	}
	scroll_offset_ = p_offset;
	if (content_) {
		content_->set_position(Vector2(0, get_size().y - scroll_offset_));
		_update_fade_nodes();
	}
	emit_signal("scroll_offset_changed", scroll_offset_);
}

float NovaCreditsPlayer::get_scroll_offset() const {
	return scroll_offset_;
}

void NovaCreditsPlayer::rebuild() {
	_rebuild_content();
}

void NovaCreditsPlayer::_process_scroll(double p_delta) {
	if (!content_ || !credits_resource_.is_valid()) return;

	// The SCROLL_RATE value itself is witnessed (parsed per-frame pixels,
	// [orig: marquee_load_credits_from_ini @ 0x65c5a0]), but the frame CADENCE
	// this 60.0f converts it with is NOT — CMarqueeWnd's update rate is
	// unwitnessed (R5 in docs/oned/workspace-maturity-program.md; the engine
	// tick elsewhere is 62 Hz). Do not cite or change without a grill.
	float scroll_rate = credits_resource_->get_scroll_rate() * speed_scale_ * 60.0f;
	scroll_offset_ += scroll_rate * p_delta;
	emit_signal("scroll_offset_changed", scroll_offset_);

	float y_pos = get_size().y - scroll_offset_;
	content_->set_position(Vector2(0, y_pos));

	// Update fade effect for ~F images.
	_update_fade_nodes();

	// Check if scrolling is complete.
	if (scroll_offset_ > content_height_ + get_size().y) {
		playing_ = false;
		set_process(false);
		emit_signal("finished");
	}
}

void NovaCreditsPlayer::_rebuild_content() {
	clear_highlight();
	_clear_content();
	needs_rebuild_ = false;
	entry_to_node_.clear();
	entry_stream_y_.clear();
	fade_overlays_.clear();
	content_height_ = 0.0f;

	if (!credits_resource_.is_valid()) return;

	int vertical_space = credits_resource_->get_vertical_space();
	int entry_count = credits_resource_->get_entry_count();
	Vector2 viewport_size = get_size();

	// First pass: find ~F images to calculate how much horizontal space they occupy.
	// Text will be positioned in the remaining space to the right.
	float image_right_edge = 0.0f;
	for (int i = 0; i < entry_count; ++i) {
		Ref<CbinEntry> entry = credits_resource_->get_entry(i);
		if (!entry.is_valid()) continue;

		if (Ref<CbinImageEntry> image_entry = Object::cast_to<CbinImageEntry>(entry.ptr()); image_entry.is_valid()) {
			if (!image_entry->get_advances_y()) {
				// ~F image - calculate its right edge after scaling.
				Ref<Resource> texture_res = image_entry->get_texture();
				if (texture_res.is_valid()) {
					Ref<Texture2D> texture = texture_res;
					if (texture.is_valid()) {
						Vector2 img_size = texture->get_size();
						// Apply same scaling logic as rendering.
						if (img_size.x > 0 && img_size.y > 0) {
							float scale_x = viewport_size.x / img_size.x;
							float scale_y = viewport_size.y / img_size.y;
							float scale = Math::min(scale_x, scale_y);
							if (scale < 1.0f) {
								img_size = img_size * scale;
							}
						}
						float img_x = static_cast<float>(image_entry->get_display_x());
						float right = img_x + img_size.x;
						image_right_edge = Math::max(image_right_edge, right);
					}
				}
			}
		}
	}

	// Calculate text area: from image_right_edge to viewport right.
	float text_area_left = image_right_edge;
	float text_area_right = viewport_size.x;

	// Create container for scrolling content.
	content_ = memnew(Control);
	content_->set_name("Content");
	content_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	add_child(content_);

	// Current Y position for layout (scroll stream position).
	float current_y = 0.0f;

	int label_count = 0, spacer_count = 0, image_count = 0;

	for (int i = 0; i < entry_count; ++i) {
		Ref<CbinEntry> entry = credits_resource_->get_entry(i);
		entry_stream_y_.push_back(current_y);
		if (!entry.is_valid()) continue;

		Control *created_node = nullptr;

		// Check entry type by casting to specific type.
		if (Ref<CbinTextEntry> text_entry = Object::cast_to<CbinTextEntry>(entry.ptr()); text_entry.is_valid()) {
			Label *label = memnew(Label);
			label->set_text(text_entry->get_text());
			label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);

			// Get alignment from entry.
			HorizontalAlignment alignment = HORIZONTAL_ALIGNMENT_CENTER;
			switch (text_entry->get_justify()) {
				case CBIN_JUSTIFY_LEFT:
					alignment = HORIZONTAL_ALIGNMENT_LEFT;
					break;
				case CBIN_JUSTIFY_CENTER:
					alignment = HORIZONTAL_ALIGNMENT_CENTER;
					break;
				case CBIN_JUSTIFY_RIGHT:
					alignment = HORIZONTAL_ALIGNMENT_RIGHT;
					break;
			}
			label->set_horizontal_alignment(alignment);

			// Get color from entry.
			label->set_modulate(text_entry->get_color());

			// Use only the font explicitly assigned to this text entry.
			Ref<Resource> font_res = text_entry->get_font();
			if (font_res.is_valid()) {
				Ref<Font> font = font_res;
				if (!font.is_valid()) {
					Ref<NovaFntResource> nova_fnt = font_res;
					if (nova_fnt.is_valid()) {
						font = nova_fnt->to_font_file();
					}
				}
				if (font.is_valid()) {
					label->add_theme_font_override("font", font);
				}
			}

			label->set_name(String("Label_") + String::num_int64(++label_count));
			content_->add_child(label);

			// Force size calculation.
			label->reset_size();
			Vector2 label_size = label->get_size();

			// Position vertically at current_y.
			label->set_position(Vector2(0, current_y));

			// Position horizontally based on alignment within text area.
			position_control_horizontal(label, label_size.x, alignment, text_area_left, text_area_right);

			// Text entries always advance Y by their height.
			current_y += label_size.y;

			created_node = label;
		} else if (Ref<CbinNewlineEntry> newline_entry = Object::cast_to<CbinNewlineEntry>(entry.ptr()); newline_entry.is_valid()) {
			// Newlines are just spacers - advance Y by vertical_space.
			// Create an invisible control for entry mapping.
			Control *spacer = memnew(Control);
			spacer->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			spacer->set_position(Vector2(0, current_y));
			spacer->set_custom_minimum_size(Vector2(0, vertical_space));
			spacer->set_name(String("Spacer_") + String::num_int64(++spacer_count));
			content_->add_child(spacer);

			current_y += vertical_space;
			created_node = spacer;
		} else if (Ref<CbinImageEntry> image_entry = Object::cast_to<CbinImageEntry>(entry.ptr()); image_entry.is_valid()) {
			// Use texture directly from the resource.
			Ref<Resource> texture_res = image_entry->get_texture();
			Ref<Texture2D> texture;
			if (texture_res.is_valid()) {
				texture = texture_res;
			}

			// Placeholder sizes when no texture is available.
			// ~I (scrolling): match a typical credits-image height.
			// ~F (fixed overlay): shorter bar so it doesn't block the scroll area.
			constexpr float kMissingI_W = 160.0f;
			constexpr float kMissingI_H = 90.0f;
			constexpr float kMissingF_W = 160.0f;
			constexpr float kMissingF_H = 40.0f;

			// Bright red-orange so a missing image is impossible to overlook.
			const Color kMissingColor(0.9f, 0.4f, 0.2f, 0.7f);

			if (!texture.is_valid()) {
				// --- Loud missing-image placeholder ---
				// A Control container holding a ColorRect background and a Label.
				Control *placeholder = memnew(Control);
				placeholder->set_name(String("MissingImage_") + String::num_int64(++image_count));
				placeholder->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);

				float pw = image_entry->get_advances_y() ? kMissingI_W : kMissingF_W;
				float ph = image_entry->get_advances_y() ? kMissingI_H : kMissingF_H;
				placeholder->set_custom_minimum_size(Vector2(pw, ph));
				placeholder->set_size(Vector2(pw, ph));

				ColorRect *bg = memnew(ColorRect);
				bg->set_anchors_preset(Control::PRESET_FULL_RECT);
				bg->set_color(kMissingColor);
				bg->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
				placeholder->add_child(bg);

				Label *lbl = memnew(Label);
				String display_name = image_entry->get_texture_path();  // falls back to texture_name
				if (display_name.is_empty()) {
					display_name = "<unknown>";
				}
				lbl->set_text("MISSING IMAGE\n" + display_name);
				lbl->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
				lbl->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
				lbl->set_anchors_preset(Control::PRESET_FULL_RECT);
				lbl->set_modulate(Color(1, 1, 1, 1));
				lbl->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
				placeholder->add_child(lbl);

				if (image_entry->get_advances_y()) {
					content_->add_child(placeholder);
					placeholder->set_position(Vector2(0, current_y));
					position_control_horizontal(placeholder, pw, HORIZONTAL_ALIGNMENT_CENTER, text_area_left, text_area_right);
					current_y += ph + vertical_space;
				} else {
					add_child(placeholder);
					move_child(placeholder, 0);
					float img_x = static_cast<float>(image_entry->get_display_x());
					int display_y_val = image_entry->get_display_y();
					float img_y = (display_y_val != 0)
					    ? static_cast<float>(display_y_val)
					    : (viewport_size.y - ph) / 2.0f;
					placeholder->set_position(Vector2(img_x, img_y));

					FadeOverlay overlay;
					overlay.node = placeholder;
					overlay.trigger_y = current_y;
					fade_overlays_.push_back(overlay);
				}

				created_node = placeholder;
			} else {
				// --- Normal path: texture resolved ---
				TextureRect *tex_rect = memnew(TextureRect);
				tex_rect->set_texture(texture);
				tex_rect->set_stretch_mode(TextureRect::STRETCH_KEEP);
				tex_rect->set_name(String("Image_") + String::num_int64(++image_count));
				tex_rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);

				Vector2 image_size = texture->get_size();
				tex_rect->set_size(image_size);

				if (image_entry->get_advances_y()) {
					// ~I images: scroll with content, centered in text area.
					content_->add_child(tex_rect);
					tex_rect->set_position(Vector2(0, current_y));
					position_control_horizontal(tex_rect, image_size.x, HORIZONTAL_ALIGNMENT_CENTER, text_area_left, text_area_right);
					current_y += image_size.y + vertical_space;
				} else {
					// ~F images: fixed overlay, vertically centered in viewport.
					// display_x is X position, image is centered vertically.
					// Auto-scale down if image is larger than viewport.
					// Added as sibling of content_ so it doesn't scroll.
					// Insert before content_ so text renders on top.
					add_child(tex_rect);
					move_child(tex_rect, 0);  // Move to back (render behind text)

					Vector2 final_size = image_size;

					// Scale down if image is larger than viewport (maintain aspect ratio).
					if (image_size.x > 0 && image_size.y > 0) {
						float scale_x = viewport_size.x / image_size.x;
						float scale_y = viewport_size.y / image_size.y;
						float scale = Math::min(scale_x, scale_y);
						if (scale < 1.0f) {
							final_size = image_size * scale;
							tex_rect->set_size(final_size);
							tex_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT);
						}
					}

					float img_x = static_cast<float>(image_entry->get_display_x());
					int display_y_val = image_entry->get_display_y();
					float img_y = (display_y_val != 0)
					    ? static_cast<float>(display_y_val)
					    : (viewport_size.y - final_size.y) / 2.0f;  // 0 means default-centered
					tex_rect->set_position(Vector2(img_x, img_y));

					// Track for fade effect. Trigger Y is current position in scroll stream.
					FadeOverlay overlay;
					overlay.node = tex_rect;
					overlay.trigger_y = current_y;
					fade_overlays_.push_back(overlay);

					// ~F images don't advance the scroll position.
				}

				created_node = tex_rect;
			}
		}

		if (created_node) {
			entry_to_node_[i] = created_node;
		}
	}

	// Store total content height.
	content_height_ = current_y;

	// Position at top for preview. play() will reposition to bottom to start scrolling.
	content_->set_position(Vector2(0, 0));

	// Initialize fade overlay alpha based on current position.
	_update_fade_nodes();
}

void NovaCreditsPlayer::highlight_entry(int p_index) {
	clear_highlight();

	if (!content_ || p_index < 0 || !credits_resource_.is_valid()) return;

	// Find the node for this entry.
	if (!entry_to_node_.has(p_index)) {
		// No visual node for this entry.
		// Find the next entry that has a visual node and scroll near it.
		for (int i = p_index + 1; i < credits_resource_->get_entry_count(); ++i) {
			if (entry_to_node_.has(i)) {
				p_index = i;
				break;
			}
		}
		if (!entry_to_node_.has(p_index)) return;
	}

	Control *node = entry_to_node_[p_index];
	if (!node) return;

	highlighted_entry_ = p_index;
	highlight_node_ = node;
	original_color_ = node->get_modulate();

	// Scroll to show the node. Get node's position relative to content, then offset content.
	float node_y = node->get_position().y;
	float view_height = get_size().y;

	// Center the node in the view if possible.
	float target_offset = node_y - view_height / 2 + node->get_size().y / 2;
	target_offset = Math::max(0.0f, target_offset);

	content_->set_position(Vector2(0, -target_offset));

	// Apply highlight effect - yellow tint.
	node->set_modulate(original_color_ * Color(1.0, 1.0, 0.5, 1.0));
}

void NovaCreditsPlayer::clear_highlight() {
	if (highlight_node_ && highlighted_entry_ >= 0) {
		// Restore original color.
		highlight_node_->set_modulate(original_color_);
	}
	highlighted_entry_ = -1;
	highlight_node_ = nullptr;
	original_color_ = Color(1, 1, 1);
}

void NovaCreditsPlayer::_clear_content() {
	// Clean up fade overlay nodes (they're siblings of content_, not children).
	for (const FadeOverlay &overlay : fade_overlays_) {
		if (overlay.node) {
			overlay.node->queue_free();
		}
	}
	fade_overlays_.clear();

	if (content_) {
		content_->queue_free();
		content_ = nullptr;
	}
}

void NovaCreditsPlayer::_update_fade_nodes() {
	if (fade_overlays_.is_empty() || !content_) return;

	// Viewport bounds.
	float viewport_top = 0.0f;
	float viewport_bottom = get_size().y;

	// Content offset - content_->get_position().y gives the Y offset of content in viewport.
	// When scrolling: content starts at viewport_bottom, scrolls up (becomes negative).
	float content_y = content_->get_position().y;

	for (const FadeOverlay &overlay : fade_overlays_) {
		if (!overlay.node) continue;

		// Calculate where the trigger_y position is in viewport coordinates.
		// trigger_y is the Y position in the scroll stream where this ~F entry appeared.
		// As content scrolls, trigger_y moves through the viewport.
		float trigger_y_in_viewport = overlay.trigger_y + content_y;

		// Calculate distance from nearest viewport edge.
		float dist_from_top = trigger_y_in_viewport - viewport_top;
		float dist_from_bottom = viewport_bottom - trigger_y_in_viewport;
		float dist_from_edge = Math::min(dist_from_top, dist_from_bottom);

		// Calculate alpha: 0 when outside/at edge, 1.0 when 50px from edge.
		float alpha = 0.0f;
		if (dist_from_edge > 0.0f) {
			alpha = Math::clamp(dist_from_edge / kFadeZonePixels, 0.0f, 1.0f);
		}

		// Apply alpha via modulate (preserve RGB, just change alpha).
		Color current = overlay.node->get_modulate();
		current.a = alpha;
		overlay.node->set_modulate(current);
	}
}

int NovaCreditsPlayer::entry_index_at_scroll_center() const {
	if (entry_stream_y_.is_empty()) {
		return -1;
	}
	float center_content_y = scroll_offset_ - get_size().y * 0.5f;
	int best_index = -1;
	float best_distance = 1e9f;
	for (int i = 0; i < entry_stream_y_.size(); ++i) {
		float node_y = entry_stream_y_[i];
		float distance = Math::abs(node_y - center_content_y);
		if (distance < best_distance) {
			best_distance = distance;
			best_index = i;
		}
	}
	return best_index;
}

float NovaCreditsPlayer::content_y_for_entry(int p_index) const {
	if (p_index < 0 || p_index >= entry_stream_y_.size()) {
		return 0.0f;
	}
	return entry_stream_y_[p_index];
}

}  // namespace godot
