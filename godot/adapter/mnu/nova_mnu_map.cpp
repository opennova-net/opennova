#include "nova_mnu_map.h"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>

using namespace godot;

static float clampf_(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

void NovaMnuMap::ensure_layers() {
	if (map_image_ == nullptr) {
		map_image_ = memnew(TextureRect);
		map_image_->set_name("MapImage");
		map_image_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		map_image_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
		if (map_texture_.is_valid()) {
			map_image_->set_texture(map_texture_);
		}
		add_child(map_image_);
	}
	if (markers_layer_ == nullptr) {
		markers_layer_ = memnew(Control);
		markers_layer_->set_name("Markers");
		markers_layer_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		add_child(markers_layer_);
	}
}

void NovaMnuMap::apply_view() {
	if (map_image_ != nullptr) {
		map_image_->set_position(pan_);
		map_image_->set_scale(Vector2(zoom_, zoom_));
		if (map_texture_.is_valid()) {
			map_image_->set_size(map_texture_->get_size());
		}
	}
	for (const Marker &m : markers_) {
		if (m.node != nullptr) {
			m.node->set_position(m.map_pos * zoom_ + pan_);
		}
	}
}

void NovaMnuMap::_ready() {
	set_clip_contents(true);
	ensure_layers();
	apply_view();
}

void NovaMnuMap::_gui_input(const Ref<InputEvent> &p_event) {
	if (behavior_.edit_mode) {
		return;
	}
	const Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_valid()) {
		if (mb->get_button_index() == MOUSE_BUTTON_LEFT) {
			dragging_ = mb->is_pressed();
			accept_event();
		} else if (mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP) {
			set_zoom(zoom_ * 1.1f);
			accept_event();
		} else if (mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN) {
			set_zoom(zoom_ / 1.1f);
			accept_event();
		}
		return;
	}
	const Ref<InputEventMouseMotion> mm = p_event;
	if (mm.is_valid() && dragging_) {
		pan_ += mm->get_relative();
		apply_view();
		accept_event();
	}
}

void NovaMnuMap::set_map_texture(const Ref<Texture2D> &p_tex) {
	map_texture_ = p_tex;
	ensure_layers();
	if (map_image_ != nullptr) {
		map_image_->set_texture(p_tex);
	}
	apply_view();
}

void NovaMnuMap::set_pan(const Vector2 &p_pan) {
	pan_ = p_pan;
	apply_view();
}

void NovaMnuMap::set_zoom(float p_zoom) {
	zoom_ = clampf_(p_zoom, min_zoom_, max_zoom_);
	apply_view();
}

void NovaMnuMap::center_on(const Vector2 &p_map_point) {
	pan_ = get_size() * 0.5f - p_map_point * zoom_;
	apply_view();
}

int NovaMnuMap::add_marker(const Ref<Texture2D> &p_icon, const Vector2 &p_map_pos) {
	ensure_layers();
	Marker m;
	m.id = next_marker_id_++;
	m.icon = p_icon;
	m.map_pos = p_map_pos;
	m.node = memnew(TextureRect);
	m.node->set_name(String("Marker") + String::num_int64(m.id));
	m.node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	if (p_icon.is_valid()) {
		m.node->set_texture(p_icon);
	}
	m.node->set_position(p_map_pos * zoom_ + pan_);
	markers_layer_->add_child(m.node);
	markers_.push_back(m);
	return m.id;
}

void NovaMnuMap::remove_marker(int p_id) {
	for (size_t i = 0; i < markers_.size(); ++i) {
		if (markers_[i].id == p_id) {
			if (markers_[i].node != nullptr) {
				markers_[i].node->queue_free();
			}
			markers_.erase(markers_.begin() + i);
			return;
		}
	}
}

void NovaMnuMap::clear_markers() {
	for (Marker &m : markers_) {
		if (m.node != nullptr) {
			m.node->queue_free();
		}
	}
	markers_.clear();
}

void NovaMnuMap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_map_texture", "tex"), &NovaMnuMap::set_map_texture);
	ClassDB::bind_method(D_METHOD("get_map_texture"), &NovaMnuMap::get_map_texture);
	ClassDB::bind_method(D_METHOD("set_pan", "pan"), &NovaMnuMap::set_pan);
	ClassDB::bind_method(D_METHOD("get_pan"), &NovaMnuMap::get_pan);
	ClassDB::bind_method(D_METHOD("set_zoom", "zoom"), &NovaMnuMap::set_zoom);
	ClassDB::bind_method(D_METHOD("get_zoom"), &NovaMnuMap::get_zoom);
	ClassDB::bind_method(D_METHOD("center_on", "map_point"), &NovaMnuMap::center_on);
	ClassDB::bind_method(D_METHOD("add_marker", "icon", "map_pos"), &NovaMnuMap::add_marker);
	ClassDB::bind_method(D_METHOD("remove_marker", "id"), &NovaMnuMap::remove_marker);
	ClassDB::bind_method(D_METHOD("clear_markers"), &NovaMnuMap::clear_markers);
	ClassDB::bind_method(D_METHOD("get_marker_count"), &NovaMnuMap::get_marker_count);

	ADD_SIGNAL(MethodInfo("marker_activated", PropertyInfo(Variant::INT, "marker_id")));
}
