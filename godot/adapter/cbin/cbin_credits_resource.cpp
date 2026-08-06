#include "cbin_credits_resource.h"

#include <cstdio>
#include <string>
#include <godot_cpp/core/class_db.hpp>

#include "cbin/cbin.h"

namespace godot {

// Build a credits resource from raw CBIN bytes. Mirrors the ENV + entry conversion in
// KdaResourceFormatLoader::_load (collapsing Color/Justify control codes into per-text
// state) but without disk-relative font/texture resolution, so it works from a PFF
// datasource [orig: marquee_load_credits_from_ini @ 0x65c5a0: [ENV] SCROLL_RATE/CENTER_X/
// VERTICAL_SPACE + [TEXT] ~C/~F/~I/~J/<CR>].
Ref<CbinCreditsResource> CbinCreditsResource::from_cbin_bytes(const PackedByteArray &p_data) {
	if (p_data.is_empty() || !cbin::is_cbin(p_data.ptr(), static_cast<size_t>(p_data.size()))) {
		return Ref<CbinCreditsResource>();
	}
	cbin::Credits credits;
	std::string error;
	if (!cbin::decode_credits(p_data.ptr(), static_cast<size_t>(p_data.size()), credits, error)) {
		return Ref<CbinCreditsResource>();
	}

	Ref<CbinCreditsResource> resource;
	resource.instantiate();
	resource->set_scroll_rate(credits.scroll_rate);
	resource->set_vertical_space(credits.vertical_space);
	resource->set_center_x(credits.center_x);
	if (credits.has_top_y) {
		resource->set_top_y(credits.top_y);
	}
	if (credits.has_bottom_y) {
		resource->set_bottom_y(credits.bottom_y);
	}

	Color current_color(1, 1, 1);
	CbinJustify current_justify = CBIN_JUSTIFY_CENTER;
	for (const auto &src : credits.entries) {
		switch (src.type) {
			case cbin::EntryType::Text: {
				Ref<CbinTextEntry> entry;
				entry.instantiate();
				entry->set_text(String(src.text.c_str()).replace("_", " "));
				entry->set_color(current_color);
				entry->set_justify(current_justify);
				if (!src.font.empty()) {
					entry->set_font_name(String(src.font.c_str()));
				}
				resource->add_entry(entry);
				break;
			}
			case cbin::EntryType::Color: {
				const float r = ((src.color >> 16) & 0xFF) / 255.0f;
				const float g = ((src.color >> 8) & 0xFF) / 255.0f;
				const float b = (src.color & 0xFF) / 255.0f;
				current_color = Color(r, g, b);
				break;
			}
			case cbin::EntryType::Newline: {
				Ref<CbinNewlineEntry> entry;
				entry.instantiate();
				resource->add_entry(entry);
				break;
			}
			case cbin::EntryType::Image: {
				Ref<CbinImageEntry> entry;
				entry.instantiate();
				if (!src.image_path.empty()) {
					entry->set_texture_name(String(src.image_path.c_str()));
				}
				entry->set_display_x(src.image_display_x);
				entry->set_display_y(src.image_display_y);
				entry->set_advances_y(src.use_simple_image_format);
				resource->add_entry(entry);
				break;
			}
			case cbin::EntryType::Justify: {
				switch (src.justify) {
					case cbin::Justify::Left:
						current_justify = CBIN_JUSTIFY_LEFT;
						break;
					case cbin::Justify::Center:
						current_justify = CBIN_JUSTIFY_CENTER;
						break;
					case cbin::Justify::Right:
						current_justify = CBIN_JUSTIFY_RIGHT;
						break;
				}
				break;
			}
		}
	}
	return resource;
}

// ============================================================================
// CbinEntry (base class)
// ============================================================================

void CbinEntry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_entry_type"), &CbinEntry::get_entry_type);

	BIND_ENUM_CONSTANT(CBIN_ENTRY_TEXT);
	BIND_ENUM_CONSTANT(CBIN_ENTRY_NEWLINE);
	BIND_ENUM_CONSTANT(CBIN_ENTRY_IMAGE);

	BIND_ENUM_CONSTANT(CBIN_JUSTIFY_LEFT);
	BIND_ENUM_CONSTANT(CBIN_JUSTIFY_CENTER);
	BIND_ENUM_CONSTANT(CBIN_JUSTIFY_RIGHT);
}

CbinEntry::CbinEntry() {}
CbinEntry::~CbinEntry() {}

// ============================================================================
// CbinTextEntry
// ============================================================================

void CbinTextEntry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_text", "text"), &CbinTextEntry::set_text);
	ClassDB::bind_method(D_METHOD("get_text"), &CbinTextEntry::get_text);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "text"), "set_text", "get_text");

	ClassDB::bind_method(D_METHOD("set_font", "font"), &CbinTextEntry::set_font);
	ClassDB::bind_method(D_METHOD("get_font"), &CbinTextEntry::get_font);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "font", PROPERTY_HINT_RESOURCE_TYPE, "Resource"),
	             "set_font", "get_font");
	ClassDB::bind_method(D_METHOD("set_font_name", "name"), &CbinTextEntry::set_font_name);
	ClassDB::bind_method(D_METHOD("get_font_name"), &CbinTextEntry::get_font_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "font_name"), "set_font_name", "get_font_name");

	ClassDB::bind_method(D_METHOD("set_color", "color"), &CbinTextEntry::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &CbinTextEntry::get_color);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color"), "set_color", "get_color");

	ClassDB::bind_method(D_METHOD("set_justify", "justify"), &CbinTextEntry::set_justify);
	ClassDB::bind_method(D_METHOD("get_justify"), &CbinTextEntry::get_justify);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "justify", PROPERTY_HINT_ENUM, "Left:-1,Center:0,Right:1"),
	             "set_justify", "get_justify");

}

CbinTextEntry::CbinTextEntry() {}
CbinTextEntry::~CbinTextEntry() {}

void CbinTextEntry::set_text(const String &p_text) {
	text_ = p_text;
	emit_changed();
}

String CbinTextEntry::get_text() const {
	return text_;
}

void CbinTextEntry::set_font(const Ref<Resource> &p_font) {
	font_ = p_font;
	if (font_.is_valid()) {
		String path = font_->get_path();
		if (!path.is_empty()) {
			font_name_ = path.get_file().get_basename();
		}
	} else {
		font_name_ = String();
	}
	emit_changed();
}

Ref<Resource> CbinTextEntry::get_font() const {
	return font_;
}

void CbinTextEntry::set_font_name(const String &p_name) {
	font_name_ = p_name;
	font_ = Ref<Resource>();
	emit_changed();
}

void CbinTextEntry::set_color(const Color &p_color) {
	color_ = p_color;
	emit_changed();
}

Color CbinTextEntry::get_color() const {
	return color_;
}

void CbinTextEntry::set_justify(CbinJustify p_justify) {
	justify_ = p_justify;
	emit_changed();
}

CbinJustify CbinTextEntry::get_justify() const {
	return justify_;
}

String CbinTextEntry::get_font_name() const {
	return font_name_;
}

// ============================================================================
// CbinNewlineEntry
// ============================================================================

void CbinNewlineEntry::_bind_methods() {
	// No properties - just a marker entry for vertical spacing.
}

CbinNewlineEntry::CbinNewlineEntry() {}
CbinNewlineEntry::~CbinNewlineEntry() {}

// ============================================================================
// CbinImageEntry
// ============================================================================

void CbinImageEntry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_texture", "texture"), &CbinImageEntry::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture"), &CbinImageEntry::get_texture);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"),
	             "set_texture", "get_texture");

	ClassDB::bind_method(D_METHOD("set_display_x", "x"), &CbinImageEntry::set_display_x);
	ClassDB::bind_method(D_METHOD("get_display_x"), &CbinImageEntry::get_display_x);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "display_x"), "set_display_x", "get_display_x");

	ClassDB::bind_method(D_METHOD("set_display_y", "y"), &CbinImageEntry::set_display_y);
	ClassDB::bind_method(D_METHOD("get_display_y"), &CbinImageEntry::get_display_y);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "display_y"), "set_display_y", "get_display_y");

	ClassDB::bind_method(D_METHOD("set_advances_y", "advances"), &CbinImageEntry::set_advances_y);
	ClassDB::bind_method(D_METHOD("get_advances_y"), &CbinImageEntry::get_advances_y);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "advances_y"), "set_advances_y", "get_advances_y");

	ClassDB::bind_method(D_METHOD("set_texture_name", "name"), &CbinImageEntry::set_texture_name);
	ClassDB::bind_method(D_METHOD("get_texture_name"), &CbinImageEntry::get_texture_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "texture_name"), "set_texture_name", "get_texture_name");

	ClassDB::bind_method(D_METHOD("get_texture_path"), &CbinImageEntry::get_texture_path);
}

CbinImageEntry::CbinImageEntry() {}
CbinImageEntry::~CbinImageEntry() {}

void CbinImageEntry::set_texture(const Ref<Resource> &p_texture) {
	texture_ = p_texture;
	emit_changed();
}

Ref<Resource> CbinImageEntry::get_texture() const {
	return texture_;
}

void CbinImageEntry::set_display_x(int p_x) {
	display_x_ = p_x;
	emit_changed();
}

int CbinImageEntry::get_display_x() const {
	return display_x_;
}

void CbinImageEntry::set_display_y(int p_y) {
	display_y_ = p_y;
	emit_changed();
}

int CbinImageEntry::get_display_y() const {
	return display_y_;
}

void CbinImageEntry::set_advances_y(bool p_advances) {
	advances_y_ = p_advances;
	emit_changed();
}

bool CbinImageEntry::get_advances_y() const {
	return advances_y_;
}

String CbinImageEntry::get_texture_path() const {
	if (texture_.is_valid()) {
		String path = texture_->get_path();
		if (!path.is_empty()) {
			return path.get_file();
		}
	}
	// Fall back to the stored original name (set at load time from the source file).
	return texture_name_;
}

void CbinImageEntry::set_texture_name(const String &p_name) {
	if (texture_name_ == p_name) {
		return;
	}
	texture_name_ = p_name;
	emit_changed();
}

String CbinImageEntry::get_texture_name() const {
	return texture_name_;
}

// ============================================================================
// CbinCreditsResource
// ============================================================================

void CbinCreditsResource::_bind_methods() {
	// ENV properties.
	ClassDB::bind_method(D_METHOD("set_scroll_rate", "rate"), &CbinCreditsResource::set_scroll_rate);
	ClassDB::bind_method(D_METHOD("get_scroll_rate"), &CbinCreditsResource::get_scroll_rate);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scroll_rate"), "set_scroll_rate", "get_scroll_rate");

	ClassDB::bind_method(D_METHOD("set_vertical_space", "space"), &CbinCreditsResource::set_vertical_space);
	ClassDB::bind_method(D_METHOD("get_vertical_space"), &CbinCreditsResource::get_vertical_space);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "vertical_space"), "set_vertical_space", "get_vertical_space");

	ClassDB::bind_method(D_METHOD("set_center_x", "center"), &CbinCreditsResource::set_center_x);
	ClassDB::bind_method(D_METHOD("get_center_x"), &CbinCreditsResource::get_center_x);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "center_x"), "set_center_x", "get_center_x");

	ClassDB::bind_method(D_METHOD("set_has_top_y", "has_top_y"), &CbinCreditsResource::set_has_top_y);
	ClassDB::bind_method(D_METHOD("has_top_y"), &CbinCreditsResource::has_top_y);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "has_top_y"), "set_has_top_y", "has_top_y");

	ClassDB::bind_method(D_METHOD("set_top_y", "top_y"), &CbinCreditsResource::set_top_y);
	ClassDB::bind_method(D_METHOD("get_top_y"), &CbinCreditsResource::get_top_y);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "top_y"), "set_top_y", "get_top_y");

	ClassDB::bind_method(D_METHOD("set_has_bottom_y", "has_bottom_y"), &CbinCreditsResource::set_has_bottom_y);
	ClassDB::bind_method(D_METHOD("has_bottom_y"), &CbinCreditsResource::has_bottom_y);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "has_bottom_y"), "set_has_bottom_y", "has_bottom_y");

	ClassDB::bind_method(D_METHOD("set_bottom_y", "bottom_y"), &CbinCreditsResource::set_bottom_y);
	ClassDB::bind_method(D_METHOD("get_bottom_y"), &CbinCreditsResource::get_bottom_y);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bottom_y"), "set_bottom_y", "get_bottom_y");

	// Entries array.
	ClassDB::bind_method(D_METHOD("set_entries", "entries"), &CbinCreditsResource::set_entries);
	ClassDB::bind_method(D_METHOD("get_entries"), &CbinCreditsResource::get_entries);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "entries", PROPERTY_HINT_ARRAY_TYPE, "CbinEntry"),
	             "set_entries", "get_entries");

	ClassDB::bind_method(D_METHOD("get_entry_count"), &CbinCreditsResource::get_entry_count);
	ClassDB::bind_method(D_METHOD("get_entry", "index"), &CbinCreditsResource::get_entry);
	ClassDB::bind_method(D_METHOD("add_entry", "entry"), &CbinCreditsResource::add_entry);
	ClassDB::bind_method(D_METHOD("insert_entry", "index", "entry"), &CbinCreditsResource::insert_entry);
	ClassDB::bind_method(D_METHOD("remove_entry", "index"), &CbinCreditsResource::remove_entry);
	ClassDB::bind_method(D_METHOD("clear_entries"), &CbinCreditsResource::clear_entries);

	// Text serialization.
	ClassDB::bind_method(D_METHOD("to_text"), &CbinCreditsResource::to_text);
	ClassDB::bind_method(D_METHOD("from_text", "text"), &CbinCreditsResource::from_text);

	ClassDB::bind_method(D_METHOD("_on_entry_changed"), &CbinCreditsResource::_on_entry_changed);

	ADD_SIGNAL(MethodInfo("entries_structure_changed"));
}

CbinCreditsResource::CbinCreditsResource() {}
CbinCreditsResource::~CbinCreditsResource() {}

void CbinCreditsResource::set_scroll_rate(float p_rate) {
	scroll_rate_ = p_rate;
	emit_changed();
}

float CbinCreditsResource::get_scroll_rate() const {
	return scroll_rate_;
}

void CbinCreditsResource::set_vertical_space(int p_space) {
	vertical_space_ = p_space;
	emit_changed();
}

int CbinCreditsResource::get_vertical_space() const {
	return vertical_space_;
}

void CbinCreditsResource::set_center_x(int p_center) {
	center_x_ = p_center;
	emit_changed();
}

int CbinCreditsResource::get_center_x() const {
	return center_x_;
}

void CbinCreditsResource::set_has_top_y(bool p_has) {
	has_top_y_ = p_has;
	emit_changed();
}

bool CbinCreditsResource::has_top_y() const {
	return has_top_y_;
}

void CbinCreditsResource::set_top_y(int p_top_y) {
	top_y_ = p_top_y;
	has_top_y_ = true;
	emit_changed();
}

int CbinCreditsResource::get_top_y() const {
	return top_y_;
}

void CbinCreditsResource::set_has_bottom_y(bool p_has) {
	has_bottom_y_ = p_has;
	emit_changed();
}

bool CbinCreditsResource::has_bottom_y() const {
	return has_bottom_y_;
}

void CbinCreditsResource::set_bottom_y(int p_bottom_y) {
	bottom_y_ = p_bottom_y;
	has_bottom_y_ = true;
	emit_changed();
}

int CbinCreditsResource::get_bottom_y() const {
	return bottom_y_;
}

void CbinCreditsResource::set_entries(const TypedArray<CbinEntry> &p_entries) {
	for (int i = 0; i < entries_.size(); ++i) {
		_disconnect_entry(entries_[i]);
	}
	entries_.clear();
	for (int i = 0; i < p_entries.size(); ++i) {
		Ref<CbinEntry> e = p_entries[i];
		if (!e.is_valid() || _contains_entry_ref(e)) {
			continue;
		}
		entries_.push_back(e);
		_connect_entry(e);
	}
	emit_changed();
	emit_signal("entries_structure_changed");
}

TypedArray<CbinEntry> CbinCreditsResource::get_entries() const {
	TypedArray<CbinEntry> result;
	result.resize(entries_.size());
	for (int i = 0; i < entries_.size(); ++i) {
		result[i] = entries_[i];
	}
	return result;
}

int CbinCreditsResource::get_entry_count() const {
	return entries_.size();
}

Ref<CbinEntry> CbinCreditsResource::get_entry(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, entries_.size(), Ref<CbinEntry>());
	return entries_[p_index];
}

void CbinCreditsResource::add_entry(const Ref<CbinEntry> &p_entry) {
	if (!p_entry.is_valid() || _contains_entry_ref(p_entry)) {
		return;
	}
	_connect_entry(p_entry);
	entries_.push_back(p_entry);
	emit_changed();
	emit_signal("entries_structure_changed");
}

void CbinCreditsResource::insert_entry(int p_index, const Ref<CbinEntry> &p_entry) {
	ERR_FAIL_INDEX(p_index, entries_.size() + 1);
	if (!p_entry.is_valid() || _contains_entry_ref(p_entry)) {
		return;
	}
	_connect_entry(p_entry);
	entries_.insert(p_index, p_entry);
	emit_changed();
	emit_signal("entries_structure_changed");
}

void CbinCreditsResource::remove_entry(int p_index) {
	ERR_FAIL_INDEX(p_index, entries_.size());
	_disconnect_entry(entries_[p_index]);
	entries_.remove_at(p_index);
	emit_changed();
	emit_signal("entries_structure_changed");
}

void CbinCreditsResource::clear_entries() {
	for (int i = 0; i < entries_.size(); ++i) {
		_disconnect_entry(entries_[i]);
	}
	entries_.clear();
	emit_changed();
	emit_signal("entries_structure_changed");
}

// Helper to convert Color to hex string (rrggbb).
static String color_to_hex(const Color &c) {
	int r = static_cast<int>(c.r * 255) & 0xFF;
	int g = static_cast<int>(c.g * 255) & 0xFF;
	int b = static_cast<int>(c.b * 255) & 0xFF;
	char buf[8];
	snprintf(buf, sizeof(buf), "%02X%02X%02X", r, g, b);
	return String(buf);
}

// Helper to parse hex color (rrggbb) to Color.
static Color hex_to_color(const String &hex) {
	if (hex.length() < 6) return Color(1, 1, 1);
	int r = hex.substr(0, 2).hex_to_int();
	int g = hex.substr(2, 2).hex_to_int();
	int b = hex.substr(4, 2).hex_to_int();
	return Color(r / 255.0f, g / 255.0f, b / 255.0f);
}

String CbinCreditsResource::to_text() const {
	String result;

	// ENV section.
	result += "# Credits Text Format\n";
	result += "# Lines starting with # are comments\n";
	result += "# Control codes: ~Crrggbb (color), ~JL/~JC/~JR (justify)\n";
	result += "# Newline: <CR>\n";
	result += "# Images: ~Ipath (scrolling), ~Fx|y|path (fixed overlay)\n";
	result += "# Text with font: text [FONTNAME]\n";
	result += "\n";
	result += "[ENV]\n";
	result += "scroll_rate=" + String::num(scroll_rate_, 2) + "\n";
	result += "vertical_space=" + String::num_int64(vertical_space_) + "\n";
	result += "center_x=" + String::num_int64(center_x_) + "\n";
	if (has_top_y_) {
		result += "top_y=" + String::num_int64(top_y_) + "\n";
	}
	if (has_bottom_y_) {
		result += "bottom_y=" + String::num_int64(bottom_y_) + "\n";
	}
	result += "\n";
	result += "[TEXT]\n";

	// Track current state to emit control codes when they change.
	Color current_color(1, 1, 1);
	CbinJustify current_justify = CBIN_JUSTIFY_CENTER;

	for (int i = 0; i < entries_.size(); ++i) {
		Ref<CbinEntry> entry = entries_[i];
		if (!entry.is_valid()) continue;

		if (Ref<CbinTextEntry> text_entry = Object::cast_to<CbinTextEntry>(entry.ptr()); text_entry.is_valid()) {
			// Emit color change if needed.
			Color entry_color = text_entry->get_color();
			if (color_to_hex(entry_color) != color_to_hex(current_color)) {
				result += "~C" + color_to_hex(entry_color) + "\n";
				current_color = entry_color;
			}

			// Emit justify change if needed.
			CbinJustify entry_justify = text_entry->get_justify();
			if (entry_justify != current_justify) {
				switch (entry_justify) {
					case CBIN_JUSTIFY_LEFT: result += "~JL\n"; break;
					case CBIN_JUSTIFY_CENTER: result += "~JC\n"; break;
					case CBIN_JUSTIFY_RIGHT: result += "~JR\n"; break;
				}
				current_justify = entry_justify;
			}

			// Emit text with optional font.
			String text = text_entry->get_text();
			String font_name = text_entry->get_font_name();
			if (!font_name.is_empty()) {
				result += text + " [" + font_name + "]\n";
			} else {
				result += text + "\n";
			}
		} else if (Ref<CbinNewlineEntry> newline_entry = Object::cast_to<CbinNewlineEntry>(entry.ptr()); newline_entry.is_valid()) {
			result += "<CR>\n";
		} else if (Ref<CbinImageEntry> image_entry = Object::cast_to<CbinImageEntry>(entry.ptr()); image_entry.is_valid()) {
			String path = image_entry->get_texture_path();
			if (image_entry->get_advances_y()) {
				// ~I format (scrolling image).
				result += "~I" + path + "\n";
			} else {
				// ~F format (fixed overlay).
				result += "~F" + String::num_int64(image_entry->get_display_x()) + "|" +
				          String::num_int64(image_entry->get_display_y()) + "|" + path + "\n";
			}
		}
	}

	return result;
}

bool CbinCreditsResource::from_text(const String &p_text) {
	// Two-phase parse: build into temporaries, commit only on success.

	// Temporary state — initialized to current values so a partial [ENV]-only
	// input still preserves existing ENV when it commits.
	float parsed_scroll_rate = scroll_rate_;
	int parsed_vertical_space = vertical_space_;
	int parsed_center_x = center_x_;
	bool parsed_has_top_y = has_top_y_;
	int parsed_top_y = top_y_;
	bool parsed_has_bottom_y = has_bottom_y_;
	int parsed_bottom_y = bottom_y_;
	Vector<Ref<CbinEntry>> parsed_entries;
	bool parse_ok = true;

	// Track current state.
	Color current_color(1, 1, 1);
	CbinJustify current_justify = CBIN_JUSTIFY_CENTER;

	// Parse line by line.
	PackedStringArray lines = p_text.split("\n");
	bool in_text_section = false;

	for (int i = 0; i < lines.size(); ++i) {
		String line = lines[i].strip_edges();

		// Skip empty lines and comments.
		if (line.is_empty() || line.begins_with("#")) {
			continue;
		}

		// Section headers.
		if (line == "[ENV]") {
			in_text_section = false;
			continue;
		}
		if (line == "[TEXT]") {
			in_text_section = true;
			continue;
		}

		// ENV section parsing.
		if (!in_text_section) {
			int eq_pos = line.find("=");
			if (eq_pos > 0) {
				String key = line.substr(0, eq_pos).strip_edges();
				String value = line.substr(eq_pos + 1).strip_edges();
				if (key == "scroll_rate") {
					parsed_scroll_rate = value.to_float();
				} else if (key == "vertical_space") {
					parsed_vertical_space = value.to_int();
				} else if (key == "center_x") {
					parsed_center_x = value.to_int();
				} else if (key == "top_y") {
					parsed_top_y = value.to_int();
					parsed_has_top_y = true;
				} else if (key == "bottom_y") {
					parsed_bottom_y = value.to_int();
					parsed_has_bottom_y = true;
				}
			}
			continue;
		}

		// TEXT section parsing.
		// Check for control codes.
		if (line.begins_with("~C") || line.begins_with("~c")) {
			// Color code: ~Crrggbb
			if (line.length() >= 8) {
				current_color = hex_to_color(line.substr(2, 6));
			}
			continue;
		}

		if (line == "~JL" || line == "~jl") {
			current_justify = CBIN_JUSTIFY_LEFT;
			continue;
		}
		if (line == "~JC" || line == "~jc") {
			current_justify = CBIN_JUSTIFY_CENTER;
			continue;
		}
		if (line == "~JR" || line == "~jr") {
			current_justify = CBIN_JUSTIFY_RIGHT;
			continue;
		}

		if (line == "<CR>") {
			// Newline entry.
			Ref<CbinNewlineEntry> entry;
			entry.instantiate();
			parsed_entries.push_back(entry);
			continue;
		}

		if (line.begins_with("~I") || line.begins_with("~i")) {
			// Scrolling image: ~Ipath
			Ref<CbinImageEntry> entry;
			entry.instantiate();
			entry->set_advances_y(true);
			String texture_path = line.substr(2);
			entry->set_texture_name(texture_path);
			parsed_entries.push_back(entry);
			continue;
		}

		if (line.begins_with("~F") || line.begins_with("~f")) {
			// Fixed overlay image: ~Fx|y|path — requires exactly two '|' separators.
			String rest = line.substr(2);
			int first_pipe = rest.find("|");
			int second_pipe = (first_pipe >= 0) ? rest.find("|", first_pipe + 1) : -1;
			if (first_pipe < 0 || second_pipe <= first_pipe) {
				// Malformed ~F line: fewer than two pipe separators.
				parse_ok = false;
				break;
			}

			Ref<CbinImageEntry> entry;
			entry.instantiate();
			entry->set_advances_y(false);
			entry->set_display_x(rest.substr(0, first_pipe).to_int());
			entry->set_display_y(rest.substr(first_pipe + 1, second_pipe - first_pipe - 1).to_int());
			String texture_path = rest.substr(second_pipe + 1);
			entry->set_texture_name(texture_path);
			parsed_entries.push_back(entry);
			continue;
		}

		// Text entry: text [FONTNAME] or just text.
		Ref<CbinTextEntry> entry;
		entry.instantiate();
		entry->set_color(current_color);
		entry->set_justify(current_justify);

		// Check for font specifier [FONTNAME].
		int bracket_start = line.rfind("[");
		int bracket_end = line.rfind("]");
		if (bracket_start >= 0 && bracket_end > bracket_start) {
			String text = line.substr(0, bracket_start).strip_edges();
			String font_name = line.substr(bracket_start + 1, bracket_end - bracket_start - 1);
			entry->set_text(text);
			entry->set_font_name(font_name);
		} else {
			entry->set_text(line);
		}

		parsed_entries.push_back(entry);
	}

	// Failure condition: malformed ~F, or any input that produced zero entries.
	// This covers empty strings, whitespace-only input, and [ENV]-only input with no text.
	if (!parse_ok || parsed_entries.is_empty()) {
		return false;
	}

	// Disconnect old entries before swap.
	for (int i = 0; i < entries_.size(); ++i) {
		_disconnect_entry(entries_[i]);
	}

	// Atomic commit: swap parsed state into the resource.
	scroll_rate_ = parsed_scroll_rate;
	vertical_space_ = parsed_vertical_space;
	center_x_ = parsed_center_x;
	has_top_y_ = parsed_has_top_y;
	top_y_ = parsed_top_y;
	has_bottom_y_ = parsed_has_bottom_y;
	bottom_y_ = parsed_bottom_y;
	entries_ = parsed_entries;

	// Connect new entries so future mutations propagate.
	for (int i = 0; i < entries_.size(); ++i) {
		_connect_entry(entries_[i]);
	}

	emit_changed();
	emit_signal("entries_structure_changed");
	return true;
}

void CbinCreditsResource::_on_entry_changed() {
	emit_changed();
}

void CbinCreditsResource::_connect_entry(const Ref<CbinEntry> &p_entry) {
	if (!p_entry.is_valid()) {
		return;
	}
	Callable cb(this, "_on_entry_changed");
	if (!p_entry->is_connected("changed", cb)) {
		p_entry->connect("changed", cb);
	}
}

void CbinCreditsResource::_disconnect_entry(const Ref<CbinEntry> &p_entry) {
	if (!p_entry.is_valid()) {
		return;
	}
	Callable cb(this, "_on_entry_changed");
	if (p_entry->is_connected("changed", cb)) {
		p_entry->disconnect("changed", cb);
	}
}

bool CbinCreditsResource::_contains_entry_ref(const Ref<CbinEntry> &p_entry) const {
	if (!p_entry.is_valid()) {
		return false;
	}
	for (int i = 0; i < entries_.size(); ++i) {
		if (entries_[i] == p_entry) {
			return true;
		}
	}
	return false;
}

}  // namespace godot
