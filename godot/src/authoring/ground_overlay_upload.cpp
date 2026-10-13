#include "authoring/ground_overlay_upload.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <cstring>

#include "terrain/terrain.h"

namespace godot {

namespace {

int64_t now_us() { return int64_t(Time::get_singleton()->get_ticks_usec()); }

} // namespace

void GroundOverlayUpload::apply(Terrain &terrain, const opennova::editor::MissionOverlayImage &overlay) {
	const int64_t start = now_us();
	if (overlay.kind == opennova::editor::MissionGroundOverlay::None) {
		terrain.clear_ground_overlay();
		reset();
		return;
	}
	Ref<Image> picture;
	last_rows_ = 0;
	if (!overlay.empty()) {
		// The buffer not handed over last time (a texture update a rendering thread runs later reads the other one).
		Buffer &buffer = buffers_[next_];
		next_ ^= 1;
		const size_t row = size_t(overlay.width) * 4;
		const bool whole = buffer.image.is_null() || buffer.made != overlay.made ||
				buffer.image->get_width() != overlay.width || buffer.image->get_height() != overlay.height;
		if (whole) buffer.image = Image::create_empty(overlay.width, overlay.height, false, Image::FORMAT_RGBA8);
		uint8_t *to = buffer.image->ptrw();
		for (int j = 0; j < overlay.height; ++j) {
			// The rows the patches stamped past the ones this buffer holds (every row of a picture copied whole).
			const bool moved = whole ||
					(size_t(j) < overlay.row_stamps.size() && overlay.row_stamps[size_t(j)] > buffer.stamp);
			if (!moved) continue;
			std::memcpy(to + size_t(j) * row, overlay.rgba.data() + size_t(j) * row, row);
			++last_rows_;
		}
		buffer.made = overlay.made;
		buffer.stamp = overlay.stamp;
		last_whole_ = whole;
		picture = buffer.image;
	} else {
		reset();
	}
	const Rect2 rect(float(overlay.west), float(-overlay.north), float(overlay.width * overlay.texel),
			float(overlay.height * overlay.texel));
	const uint8_t *outside = overlay.outside_rgba;
	terrain.set_ground_overlay(picture, rect,
			Color(outside[0] / 255.0f, outside[1] / 255.0f, outside[2] / 255.0f, outside[3] / 255.0f), overlay.outside);
	++uploads_;
	last_us_ = now_us() - start;
}

void GroundOverlayUpload::reset() {
	for (Buffer &buffer : buffers_) buffer = Buffer();
	next_ = 0;
}

opennova::io::JsonValue GroundOverlayUpload::to_json() const {
	opennova::io::JsonValue out = opennova::io::JsonValue::make_object();
	out.set("uploads", opennova::io::json_number(double(uploads_)));
	out.set("rows", opennova::io::json_number(double(last_rows_)));
	out.set("whole", opennova::io::JsonValue::make_bool(last_whole_));
	out.set("last_us", opennova::io::json_number(double(last_us_)));
	return out;
}

} // namespace godot
