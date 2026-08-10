#include "menu_video_underlay.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/video_stream_theora.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <menu/menu_frame.h>

namespace godot {

using opennova::menu::MenuVideoSlotSpec;
using opennova::menu::menu_video_resolve;
using opennova::menu::menu_video_slot_visible;
using opennova::menu::menu_video_slots;
using opennova::menu::menu_video_startup_screen;

namespace {

String ogv_sibling(const String &p_rel) {
	// Playback rides the chosen movie's converted sibling: main.bik ->
	// main.ogv beside it (onimport menu-movies).
	if (p_rel.get_extension().to_lower() != "bik") {
		return String();
	}
	return p_rel.substr(0, p_rel.length() - 3) + "ogv";
}

}  // namespace

void MenuVideoUnderlay::set_source(
		const String &p_root_dir, const String &p_expansion) {
	stop();
	unconverted_ = 0;
	if (p_root_dir.is_empty()) {
		return;
	}
	const std::string expansion(p_expansion.utf8().get_data());
	const auto exists = [&p_root_dir](const std::string &rel) {
		return FileAccess::file_exists(
				p_root_dir.path_join(String::utf8(rel.c_str())));
	};
	const auto &specs = menu_video_slots();
	for (int i = 0; i < static_cast<int>(specs.size()); ++i) {
		// The witnessed selection picks the .bik (expansion copy first); a
		// miss skips the slot silently (policy home: engine menu/menu_video.h).
		const std::string chosen =
				menu_video_resolve(expansion, specs[i].file, exists);
		if (chosen.empty()) {
			continue;
		}
		const String converted =
				ogv_sibling(String::utf8(chosen.c_str()));
		const String converted_abs = p_root_dir.path_join(converted);
		if (converted.is_empty() ||
				!FileAccess::file_exists(converted_abs)) {
			++unconverted_;
			continue;
		}
		Ref<VideoStreamTheora> stream;
		stream.instantiate();
		stream->set_file(converted_abs);
		VideoStreamPlayer *player = memnew(VideoStreamPlayer);
		player->set_stream(stream);
		// The players are decode surfaces only: this control draws their
		// textures itself, and a hidden slot keeps advancing — retail gates
		// the DRAW, not the decode (engine menu/menu_video.h).
		player->set_visible(false);
		// Movie audio plays as authored, independent of the menu music
		// volume (menu-re.md: no volume tie-in exists in the original).
		player->set_bus("Master");
		player->connect("finished",
				callable_mp(this, &MenuVideoUnderlay::on_slot_finished_)
						.bind(i));
		add_child(player);
		player->play();
		slots_[i].player = player;
		slots_[i].source = converted;
	}
	set_process(true);
	queue_redraw();
}

void MenuVideoUnderlay::set_screen(const String &p_screen_name) {
	startup_ = menu_video_startup_screen(
			std::string(p_screen_name.utf8().get_data()));
	queue_redraw();
}

void MenuVideoUnderlay::stop() {
	// Menu-mode exit closes every slot (lifecycle: engine
	// menu/menu_video.h).
	for (auto &slot : slots_) {
		if (slot.player != nullptr) {
			slot.player->stop();
			slot.player->queue_free();
			slot.player = nullptr;
		}
		slot.source = String();
	}
	set_process(false);
	queue_redraw();
}

int MenuVideoUnderlay::get_active_slot_count() const {
	int count = 0;
	for (const auto &slot : slots_) {
		if (slot.player != nullptr) {
			++count;
		}
	}
	return count;
}

int MenuVideoUnderlay::get_unconverted_count() const {
	return unconverted_;
}

bool MenuVideoUnderlay::is_startup_layout() const {
	return startup_;
}

String MenuVideoUnderlay::get_slot_source(int p_slot) const {
	if (p_slot < 0 ||
			p_slot >= static_cast<int>(slots_.size())) {
		return String();
	}
	return slots_[static_cast<size_t>(p_slot)].source;
}

void MenuVideoUnderlay::draw_slots_() {
	const Size2 size = get_size();
	if (size.x <= 0.0f || size.y <= 0.0f) {
		return;
	}
	// The same anamorphic 800x600 pair as every widget, each edge
	// int-truncated (engine menu/menu_video.h rect contract).
	const float sx =
			size.x / static_cast<float>(opennova::menu::kMenuDesignWidth);
	const float sy =
			size.y / static_cast<float>(opennova::menu::kMenuDesignHeight);
	const auto &specs = menu_video_slots();
	for (int i = 0; i < static_cast<int>(specs.size()); ++i) {
		VideoStreamPlayer *player = slots_[i].player;
		if (player == nullptr ||
				!menu_video_slot_visible(specs[i].slot, startup_)) {
			continue;
		}
		Ref<Texture2D> frame = player->get_video_texture();
		if (frame.is_null()) {
			continue;
		}
		const MenuVideoSlotSpec &spec = specs[i];
		const float x0 = static_cast<float>(
				static_cast<int>(spec.left * sx));
		const float y0 = static_cast<float>(
				static_cast<int>(spec.top * sy));
		const float x1 = static_cast<float>(
				static_cast<int>(spec.right * sx));
		const float y1 = static_cast<float>(
				static_cast<int>(spec.bottom * sy));
		// One stretched quad — no letterboxing, no aspect preservation
		// (menu-re.md "The menu backdrop").
		draw_texture_rect(frame, Rect2(x0, y0, x1 - x0, y1 - y0), false);
	}
}

void MenuVideoUnderlay::on_slot_finished_(int p_index) {
	// Looping is the frame-counter rewind (menu-re.md: the FrameNum = 0
	// poke).
	VideoStreamPlayer *player =
			slots_[static_cast<size_t>(p_index)].player;
	if (player != nullptr) {
		player->set_stream_position(0.0);
		player->play();
	}
}

void MenuVideoUnderlay::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PROCESS:
			// The menu redraws every frame; movie cadence is the stream's
			// own clock (menu-re.md: the dirty flag is set unconditionally;
			// the decoder paces itself).
			queue_redraw();
			break;
		case NOTIFICATION_DRAW:
			draw_slots_();
			break;
		default:
			break;
	}
}

void MenuVideoUnderlay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source", "root_dir", "expansion"),
			&MenuVideoUnderlay::set_source);
	ClassDB::bind_method(D_METHOD("set_screen", "screen_name"),
			&MenuVideoUnderlay::set_screen);
	ClassDB::bind_method(D_METHOD("stop"), &MenuVideoUnderlay::stop);
	ClassDB::bind_method(D_METHOD("get_active_slot_count"),
			&MenuVideoUnderlay::get_active_slot_count);
	ClassDB::bind_method(D_METHOD("get_unconverted_count"),
			&MenuVideoUnderlay::get_unconverted_count);
	ClassDB::bind_method(D_METHOD("is_startup_layout"),
			&MenuVideoUnderlay::is_startup_layout);
	ClassDB::bind_method(D_METHOD("get_slot_source", "slot"),
			&MenuVideoUnderlay::get_slot_source);
}

}  // namespace godot
