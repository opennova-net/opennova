#include "kda_resource_format.h"

#include "cbin_credits_resource.h"
#include "cbin_asset_lookup.h"
#include "util/nova_data_format.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <unordered_map>

#include "cbin/cbin.h"

namespace godot {

namespace {

// Replace underscores with spaces (game does this at display time).
String underscore_to_space(const std::string &s) {
	String result(s.c_str());
	return result.replace("_", " ");
}

// Replace spaces with underscores for CBIN format.
std::string space_to_underscore(const String &s) {
	String result = s.replace(" ", "_");
	return result.utf8().get_data();
}

String normalized_base_dir(const String &path) {
	return path.replace("\\", "/").get_base_dir();
}

// Convert Godot Color to CBIN color (RGB 24-bit).
uint32_t color_to_cbin(const Color &c) {
	uint32_t r = static_cast<uint32_t>(c.r * 255) & 0xFF;
	uint32_t g = static_cast<uint32_t>(c.g * 255) & 0xFF;
	uint32_t b = static_cast<uint32_t>(c.b * 255) & 0xFF;
	return (r << 16) | (g << 8) | b;
}

// Check if two colors are approximately equal.
bool colors_equal(const Color &a, const Color &b) {
	// Compare using 8-bit precision (what CBIN uses).
	return color_to_cbin(a) == color_to_cbin(b);
}

}  // namespace

PackedStringArray KdaResourceFormatLoader::_get_recognized_extensions() const {
	PackedStringArray extensions;
	extensions.push_back("kda");
	return extensions;
}

bool KdaResourceFormatLoader::_handles_type(const StringName &p_type) const {
	return p_type == StringName("CbinCreditsResource") || p_type == StringName("Resource");
}

String KdaResourceFormatLoader::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "kda") {
		return "CbinCreditsResource";
	}
	return String();
}

Variant KdaResourceFormatLoader::_load(const String &p_path, const String &p_original_path, bool p_use_sub_threads,
                                int32_t p_cache_mode) const {
	(void)p_use_sub_threads;
	(void)p_cache_mode;

	PackedByteArray data;
	if (!read_nova_payload_file(p_path, data)) {
		UtilityFunctions::push_error("KdaResourceFormatLoader: Cannot open file: ", p_path);
		return Variant();
	}

	// Check CBIN magic.
	if (!cbin::is_cbin(data.ptr(), data.size())) {
		UtilityFunctions::push_error("KdaResourceFormatLoader: Not a valid CBIN file: ", p_path);
		return Variant();
	}

	// Parse using cbin library.
	cbin::Credits credits;
	std::string error;
	if (!cbin::decode_credits(data.ptr(), data.size(), credits, error)) {
		UtilityFunctions::push_error("KdaResourceFormatLoader: Failed to parse CBIN file: ", p_path, " - ",
		                             String(error.c_str()));
		return Variant();
	}

	// Create CbinCreditsResource.
	Ref<CbinCreditsResource> resource;
	resource.instantiate();
	String resource_dir = normalized_base_dir(p_path);
	String original_dir = normalized_base_dir(p_original_path);
	if (!original_dir.is_empty()) {
		resource_dir = original_dir;
	}

	std::unordered_map<std::string, Ref<Resource>> font_cache;
	std::unordered_map<std::string, Ref<Resource>> texture_cache;
	auto find_font = [&](const String &font_name) -> Ref<Resource> {
		std::string key(font_name.to_lower().utf8().get_data());
		auto it = font_cache.find(key);
		if (it != font_cache.end()) {
			return it->second;
		}
		Ref<Resource> font = cbin_internal::find_font_by_name(font_name, resource_dir);
		font_cache.emplace(key, font);
		return font;
	};
	auto find_texture = [&](const String &texture_name) -> Ref<Resource> {
		std::string key(texture_name.to_lower().utf8().get_data());
		auto it = texture_cache.find(key);
		if (it != texture_cache.end()) {
			return it->second;
		}
		Ref<Resource> texture = cbin_internal::find_texture_by_name(texture_name, resource_dir);
		texture_cache.emplace(key, texture);
		return texture;
	};

	// Set ENV values.
	resource->set_scroll_rate(credits.scroll_rate);
	resource->set_vertical_space(credits.vertical_space);
	resource->set_center_x(credits.center_x);
	if (credits.has_top_y) {
		resource->set_top_y(credits.top_y);
	}
	if (credits.has_bottom_y) {
		resource->set_bottom_y(credits.bottom_y);
	}

	// Track current state for applying to text entries.
	// The CBIN format uses separate Color and Justify control codes that affect
	// subsequent text. We collapse these into properties on each TextEntry.
	Color current_color(1, 1, 1);  // Default white
	CbinJustify current_justify = CBIN_JUSTIFY_CENTER;

	// Convert entries - track state from Color/Justify entries and apply to Text entries.
	for (const auto &src : credits.entries) {
		switch (src.type) {
			case cbin::EntryType::Text: {
				Ref<CbinTextEntry> text_entry;
				text_entry.instantiate();
				// Replace underscores with spaces for display.
				text_entry->set_text(underscore_to_space(src.text));
				// Apply current state.
				text_entry->set_color(current_color);
				text_entry->set_justify(current_justify);
				if (!src.font.empty()) {
					String font_name(src.font.c_str());
					text_entry->set_font_name(font_name);
					Ref<Resource> font = find_font(font_name);
					if (font.is_valid()) {
						text_entry->set_font(font);
					}
				}
				resource->add_entry(text_entry);
				break;
			}
			case cbin::EntryType::Color: {
				// Update state - don't create an entry.
				float r = ((src.color >> 16) & 0xFF) / 255.0f;
				float g = ((src.color >> 8) & 0xFF) / 255.0f;
				float b = (src.color & 0xFF) / 255.0f;
				current_color = Color(r, g, b);
				break;
			}
			case cbin::EntryType::Newline: {
				Ref<CbinNewlineEntry> newline_entry;
				newline_entry.instantiate();
				resource->add_entry(newline_entry);
				break;
			}
			case cbin::EntryType::Image: {
				Ref<CbinImageEntry> image_entry;
				image_entry.instantiate();
				if (!src.image_path.empty()) {
					String texture_name = String(src.image_path.c_str());
					// Always record the original name so placeholders can display it.
					image_entry->set_texture_name(texture_name);
					Ref<Resource> texture = find_texture(texture_name);
					if (texture.is_valid()) {
						image_entry->set_texture(texture);
					}
				}
				// ~F format: display offsets from viewport
				image_entry->set_display_x(src.image_display_x);
				image_entry->set_display_y(src.image_display_y);
				// ~I images (simple format) scroll with content, ~F images are fixed overlays.
				image_entry->set_advances_y(src.use_simple_image_format);
				resource->add_entry(image_entry);
				break;
			}
			case cbin::EntryType::Justify: {
				// Update state - don't create an entry.
				switch (src.justify) {
					case cbin::Justify::Left: current_justify = CBIN_JUSTIFY_LEFT; break;
					case cbin::Justify::Center: current_justify = CBIN_JUSTIFY_CENTER; break;
					case cbin::Justify::Right: current_justify = CBIN_JUSTIFY_RIGHT; break;
				}
				break;
			}
		}
	}

	return resource;
}

Error KdaResourceFormatSaver::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<CbinCreditsResource> cbin_resource = p_resource;
	if (!cbin_resource.is_valid()) {
		return ERR_INVALID_PARAMETER;
	}

	// Convert to cbin::Credits.
	cbin::Credits credits;
	credits.scroll_rate = cbin_resource->get_scroll_rate();
	credits.vertical_space = cbin_resource->get_vertical_space();
	credits.center_x = cbin_resource->get_center_x();
	if (cbin_resource->has_top_y()) {
		credits.has_top_y = true;
		credits.top_y = cbin_resource->get_top_y();
	}
	if (cbin_resource->has_bottom_y()) {
		credits.has_bottom_y = true;
		credits.bottom_y = cbin_resource->get_bottom_y();
	}

	// Track current state - when a TextEntry has different state, emit control codes first.
	// Default state matches what the loader starts with.
	Color current_color(1, 1, 1);  // White
	CbinJustify current_justify = CBIN_JUSTIFY_CENTER;

	// Convert entries.
	int entry_count = cbin_resource->get_entry_count();
	credits.entries.reserve(entry_count * 2);  // May emit control codes

	for (int i = 0; i < entry_count; ++i) {
		Ref<CbinEntry> src = cbin_resource->get_entry(i);
		if (!src.is_valid()) continue;

		// Check actual type using dynamic cast.
		if (Ref<CbinTextEntry> text_entry = Object::cast_to<CbinTextEntry>(src.ptr()); text_entry.is_valid()) {
			// Check if we need to emit control codes before this text.
			Color entry_color = text_entry->get_color();
			CbinJustify entry_justify = text_entry->get_justify();

			// Emit color change if needed.
			if (!colors_equal(entry_color, current_color)) {
				cbin::Entry color_entry;
				color_entry.type = cbin::EntryType::Color;
				color_entry.color = color_to_cbin(entry_color);
				credits.entries.push_back(std::move(color_entry));
				current_color = entry_color;
			}

			// Emit justify change if needed.
			if (entry_justify != current_justify) {
				cbin::Entry justify_entry;
				justify_entry.type = cbin::EntryType::Justify;
				switch (entry_justify) {
					case CBIN_JUSTIFY_LEFT: justify_entry.justify = cbin::Justify::Left; break;
					case CBIN_JUSTIFY_CENTER: justify_entry.justify = cbin::Justify::Center; break;
					case CBIN_JUSTIFY_RIGHT: justify_entry.justify = cbin::Justify::Right; break;
				}
				credits.entries.push_back(std::move(justify_entry));
				current_justify = entry_justify;
			}

			// Now emit the text entry.
			cbin::Entry dst;
			dst.type = cbin::EntryType::Text;
			// Replace spaces with underscores for CBIN format.
			dst.text = space_to_underscore(text_entry->get_text());
			// Font name from resource path.
			String font_name = text_entry->get_font_name();
			if (!font_name.is_empty()) {
				dst.font = font_name.utf8().get_data();
			}
			credits.entries.push_back(std::move(dst));
		} else if (Ref<CbinNewlineEntry> newline_entry = Object::cast_to<CbinNewlineEntry>(src.ptr()); newline_entry.is_valid()) {
			cbin::Entry dst;
			dst.type = cbin::EntryType::Newline;
			credits.entries.push_back(std::move(dst));
		} else if (Ref<CbinImageEntry> image_entry = Object::cast_to<CbinImageEntry>(src.ptr()); image_entry.is_valid()) {
			cbin::Entry dst;
			dst.type = cbin::EntryType::Image;
			// Get image path from texture resource.
			String texture_path = image_entry->get_texture_path();
			if (!texture_path.is_empty()) {
				dst.image_path = texture_path.utf8().get_data();
			}
			dst.image_display_x = image_entry->get_display_x();
			dst.image_display_y = image_entry->get_display_y();
			// advances_y true = ~I format (scrolling), false = ~F format (fixed overlay)
			dst.use_simple_image_format = image_entry->get_advances_y();
			credits.entries.push_back(std::move(dst));
		} else {
			// Unknown entry type, skip.
			continue;
		}
	}

	// Encode to bytes.
	std::vector<uint8_t> data;
	std::string error;
	if (!cbin::encode(credits, data, error)) {
		UtilityFunctions::push_error("KdaResourceFormatSaver: Failed to encode CBIN file: ", String(error.c_str()));
		return ERR_CANT_CREATE;
	}

	// Write to file.
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (!file.is_valid()) {
		UtilityFunctions::push_error("KdaResourceFormatSaver: Cannot open file for writing: ", p_path);
		return ERR_CANT_OPEN;
	}

	PackedByteArray byte_array;
	byte_array.resize(data.size());
	memcpy(byte_array.ptrw(), data.data(), data.size());
	file->store_buffer(byte_array);
	file->close();

	return OK;
}

bool KdaResourceFormatSaver::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<CbinCreditsResource>(p_resource.ptr()) != nullptr;
}

PackedStringArray KdaResourceFormatSaver::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray extensions;
	if (_recognize(p_resource)) {
		extensions.push_back("kda");
	}
	return extensions;
}

}  // namespace godot
