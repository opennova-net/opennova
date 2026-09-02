#include "mnu/menu_video_underlay.h"
#include "util/data_format.h"
#include <base/io/perf_clock.h>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <runtime/menu/menu_frame.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace godot {

using opennova::menu::MenuVideoSlotSpec;
using opennova::menu::menu_video_resolve;
using opennova::menu::menu_video_slot_visible;
using opennova::menu::menu_video_slots;
using opennova::menu::menu_video_startup_screen;

void MenuVideoUnderlay::set_source(
		const String &p_root_dir, const String &p_expansion) {
	stop();
	failed_ = 0;
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
		const std::string chosen =
				menu_video_resolve(expansion, specs[i].file, exists);
		if (chosen.empty()) {
			continue;
		}
		const String relative = String::utf8(chosen.c_str());
		const String absolute = p_root_dir.path_join(relative);
		Ref<FileAccess> file = FileAccess::open(absolute, FileAccess::READ);
		if (file.is_null()) {
			++failed_;
			continue;
		}

		opennova::bink::BinkSource source;
		source.size = file->get_length();
		source.read_at = [file](uint64_t p_offset, uint8_t *p_destination,
				size_t p_size) {
			if (p_size > static_cast<size_t>(
					std::numeric_limits<int64_t>::max())) {
				return false;
			}
			file->seek(p_offset);
			const PackedByteArray bytes =
					file->get_buffer(static_cast<int64_t>(p_size));
			if (bytes.size() != static_cast<int64_t>(p_size)) {
				return false;
			}
			if (p_size != 0) {
				std::memcpy(p_destination, bytes.ptr(), p_size);
			}
			return true;
		};
		std::string error;
		auto movie = opennova::bink::BinkMovie::open(
				std::move(source), &error);
		if (!movie || movie->decode_next() !=
				opennova::bink::BinkStatus::frame_ready) {
			file->close();
			++failed_;
			continue;
		}

		Slot &slot = slots_[static_cast<size_t>(i)];
		slot.file = file;
		slot.movie = std::move(movie);
		slot.source = relative;
		slot.elapsed_seconds = 0.0;
		if (!upload_frame_(slot)) {
			fail_slot_(slot);
		}
	}
	update_process_state_();
	queue_redraw();
}

void MenuVideoUnderlay::set_screen(const String &p_screen_name) {
	startup_ = menu_video_startup_screen(
			std::string(p_screen_name.utf8().get_data()));
	queue_redraw();
}

void MenuVideoUnderlay::stop() {
	for (auto &slot : slots_) {
		slot.movie.reset();
		if (slot.file.is_valid()) {
			slot.file->close();
			slot.file.unref();
		}
		slot.texture.unref();
		slot.source = String();
		slot.elapsed_seconds = 0.0;
	}
	set_process(false);
	queue_redraw();
}

int MenuVideoUnderlay::get_active_slot_count() const {
	int count = 0;
	for (const auto &slot : slots_) {
		if (slot.movie) {
			++count;
		}
	}
	return count;
}

int MenuVideoUnderlay::get_failed_count() const {
	return failed_;
}

bool MenuVideoUnderlay::is_startup_layout() const {
	return startup_;
}

void MenuVideoUnderlay::set_runtime_profiling_enabled(bool p_enabled) {
	runtime_profiling_enabled_ = p_enabled;
	last_process_us_ = 0;
}

int64_t MenuVideoUnderlay::consume_process_us() {
	const uint64_t sample = last_process_us_;
	last_process_us_ = 0;
	return static_cast<int64_t>(sample);
}

String MenuVideoUnderlay::get_slot_source(int p_slot) const {
	if (p_slot < 0 || p_slot >= static_cast<int>(slots_.size())) {
		return String();
	}
	return slots_[static_cast<size_t>(p_slot)].source;
}

bool MenuVideoUnderlay::upload_frame_(Slot &p_slot) {
	if (!p_slot.movie) {
		return false;
	}
	const opennova::bink::BinkFrame &frame = p_slot.movie->frame();
	if (frame.rgba.empty() || frame.width == 0 || frame.height == 0) {
		return false;
	}
	const PackedByteArray bytes = to_packed_bytes(frame.rgba);
	const Ref<Image> image = Image::create_from_data(
			static_cast<int32_t>(frame.width),
			static_cast<int32_t>(frame.height), false,
			Image::FORMAT_RGBA8, bytes);
	if (image.is_null()) {
		return false;
	}
	if (p_slot.texture.is_null()) {
		p_slot.texture = ImageTexture::create_from_image(image);
	} else {
		p_slot.texture->update(image);
	}
	return p_slot.texture.is_valid();
}

bool MenuVideoUnderlay::advance_slot_(Slot &p_slot, double p_delta) {
	if (!p_slot.movie) {
		return false;
	}
	const double fps = p_slot.movie->info().frames_per_second();
	if (fps <= 0.0) {
		return false;
	}
	const double frame_seconds = 1.0 / fps;
	p_slot.elapsed_seconds += std::max(0.0, p_delta);
	// A short catch-up bound prevents a debugger pause from making one menu
	// tick decode an entire loop. Normal playback decodes once per cadence.
	int decoded = 0;
	while (p_slot.elapsed_seconds >= frame_seconds && decoded < 4) {
		p_slot.elapsed_seconds -= frame_seconds;
		auto status = p_slot.movie->decode_next();
		if (status == opennova::bink::BinkStatus::end_of_stream) {
			p_slot.movie->rewind();
			status = p_slot.movie->decode_next();
		}
		if (status != opennova::bink::BinkStatus::frame_ready ||
				!upload_frame_(p_slot)) {
			return false;
		}
		++decoded;
	}
	if (decoded == 4 && p_slot.elapsed_seconds >= frame_seconds) {
		p_slot.elapsed_seconds = 0.0;
	}
	return true;
}

void MenuVideoUnderlay::fail_slot_(Slot &p_slot) {
	++failed_;
	p_slot.movie.reset();
	if (p_slot.file.is_valid()) {
		p_slot.file->close();
		p_slot.file.unref();
	}
	p_slot.texture.unref();
	p_slot.source = String();
	p_slot.elapsed_seconds = 0.0;
}

void MenuVideoUnderlay::draw_slots_() {
	const Size2 size = get_size();
	if (size.x <= 0.0f || size.y <= 0.0f) {
		return;
	}
	const float sx =
			size.x / static_cast<float>(opennova::menu::kMenuDesignWidth);
	const float sy =
			size.y / static_cast<float>(opennova::menu::kMenuDesignHeight);
	const auto &specs = menu_video_slots();
	for (int i = 0; i < static_cast<int>(specs.size()); ++i) {
		const Slot &slot = slots_[static_cast<size_t>(i)];
		if (slot.texture.is_null() ||
				!menu_video_slot_visible(specs[i].slot, startup_)) {
			continue;
		}
		const MenuVideoSlotSpec &spec = specs[i];
		const float x0 = static_cast<float>(static_cast<int>(spec.left * sx));
		const float y0 = static_cast<float>(static_cast<int>(spec.top * sy));
		const float x1 = static_cast<float>(static_cast<int>(spec.right * sx));
		const float y1 = static_cast<float>(static_cast<int>(spec.bottom * sy));
		Ref<Texture2D> texture = slot.texture;
		draw_texture_rect(texture, Rect2(x0, y0, x1 - x0, y1 - y0), false);
	}
}

void MenuVideoUnderlay::update_process_state_() {
	set_process(is_inside_tree() && is_visible_in_tree() &&
			get_active_slot_count() != 0);
}

void MenuVideoUnderlay::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_VISIBILITY_CHANGED:
			update_process_state_();
			break;
		case NOTIFICATION_PROCESS: {
			const uint64_t process_start = runtime_profiling_enabled_
					? opennova::io::perf_now_us() : 0;
			if (!is_visible_in_tree()) {
				update_process_state_();
				if (runtime_profiling_enabled_)
					last_process_us_ = opennova::io::perf_now_us() - process_start;
				break;
			}
			const double delta = get_process_delta_time();
			for (Slot &slot : slots_) {
				if (slot.movie && !advance_slot_(slot, delta)) {
					fail_slot_(slot);
				}
			}
			if (get_active_slot_count() == 0) {
				set_process(false);
			}
			queue_redraw();
			if (runtime_profiling_enabled_)
				last_process_us_ = opennova::io::perf_now_us() - process_start;
			break;
		}
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
	ClassDB::bind_method(D_METHOD("get_failed_count"),
			&MenuVideoUnderlay::get_failed_count);
	ClassDB::bind_method(D_METHOD("is_startup_layout"),
			&MenuVideoUnderlay::is_startup_layout);
	ClassDB::bind_method(D_METHOD("set_runtime_profiling_enabled", "enabled"),
			&MenuVideoUnderlay::set_runtime_profiling_enabled);
	ClassDB::bind_method(D_METHOD("is_runtime_profiling_enabled"),
			&MenuVideoUnderlay::is_runtime_profiling_enabled);
	ClassDB::bind_method(D_METHOD("consume_process_us"),
			&MenuVideoUnderlay::consume_process_us);
	ClassDB::bind_method(D_METHOD("get_slot_source", "slot"),
			&MenuVideoUnderlay::get_slot_source);
}

}  // namespace godot
