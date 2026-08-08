#ifndef OPENNOVA_CBIN_CREDITS_RESOURCE_H
#define OPENNOVA_CBIN_CREDITS_RESOURCE_H

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

// Entry type enum for identifying entry types.
enum CbinEntryType {
	CBIN_ENTRY_TEXT = 0,
	CBIN_ENTRY_NEWLINE = 1,
	CBIN_ENTRY_IMAGE = 2,
};

// Justify alignment values.
enum CbinJustify {
	CBIN_JUSTIFY_LEFT = -1,
	CBIN_JUSTIFY_CENTER = 0,
	CBIN_JUSTIFY_RIGHT = 1,
};

// Base class for all entry types.
class CbinEntry : public Resource {
	GDCLASS(CbinEntry, Resource);

protected:
	static void _bind_methods();

public:
	CbinEntry();
	virtual ~CbinEntry();

	virtual CbinEntryType get_entry_type() const { return CBIN_ENTRY_TEXT; }
};

// Text entry - displays text with font, color, and justification.
class CbinTextEntry : public CbinEntry {
	GDCLASS(CbinTextEntry, CbinEntry);

protected:
	static void _bind_methods();

public:
	CbinTextEntry();
	~CbinTextEntry();

	virtual CbinEntryType get_entry_type() const override { return CBIN_ENTRY_TEXT; }

	void set_text(const String &p_text);
	String get_text() const;

	void set_font(const Ref<Resource> &p_font);
	Ref<Resource> get_font() const;
	void set_font_name(const String &p_name);
	String get_font_name() const;

	void set_color(const Color &p_color);
	Color get_color() const;

	void set_justify(CbinJustify p_justify);
	CbinJustify get_justify() const;

private:
	String text_;
	Ref<Resource> font_;
	String font_name_;
	Color color_ = Color(1, 1, 1);
	CbinJustify justify_ = CBIN_JUSTIFY_CENTER;
};

// Newline entry - inserts vertical space.
class CbinNewlineEntry : public CbinEntry {
	GDCLASS(CbinNewlineEntry, CbinEntry);

protected:
	static void _bind_methods();

public:
	CbinNewlineEntry();
	~CbinNewlineEntry();

	virtual CbinEntryType get_entry_type() const override { return CBIN_ENTRY_NEWLINE; }
};

// Image entry - displays an image.
// Two formats:
// - ~I images: Scroll with content, centered, no fade
// - ~F images: Fixed viewport position overlay with fade effect based on scroll trigger
class CbinImageEntry : public CbinEntry {
	GDCLASS(CbinImageEntry, CbinEntry);

protected:
	static void _bind_methods();

public:
	CbinImageEntry();
	~CbinImageEntry();

	virtual CbinEntryType get_entry_type() const override { return CBIN_ENTRY_IMAGE; }

	void set_texture(const Ref<Resource> &p_texture);
	Ref<Resource> get_texture() const;

	// For ~F format: display X offset from viewport left
	void set_display_x(int p_x);
	int get_display_x() const;

	// For ~F format: display Y offset from viewport top.
	// A value of 0 is preserved as the legacy auto-center sentinel.
	void set_display_y(int p_y);
	int get_display_y() const;

	// Whether image advances Y position (true for ~I format, false for ~F format).
	// ~I images scroll with content. ~F images are fixed overlays with fade.
	void set_advances_y(bool p_advances);
	bool get_advances_y() const;

	// Helper to get texture path for serialization (extracts from resource path).
	String get_texture_path() const;

	// Store the original filename from the .kda/text source so the placeholder
	// can display it even when no texture was resolved.
	void set_texture_name(const String &p_name);
	String get_texture_name() const;

private:
	Ref<Resource> texture_;
	String texture_name_;  // Original filename from source (e.g. "cr1.png")
	int display_x_ = 0;  // ~F format: X offset from viewport left
	int display_y_ = 0;  // ~F format: Y offset from viewport top
	bool advances_y_ = true;  // Default true (~I behavior)
};

// Resource that holds parsed CBIN credits data.
class CbinCreditsResource : public Resource {
	GDCLASS(CbinCreditsResource, Resource);

protected:
	static void _bind_methods();

public:
	CbinCreditsResource();
	~CbinCreditsResource();

	// ENV section properties.
	void set_scroll_rate(float p_rate);
	float get_scroll_rate() const;

	void set_vertical_space(int p_space);
	int get_vertical_space() const;

	void set_center_x(int p_center);
	int get_center_x() const;

	void set_has_top_y(bool p_has);
	bool has_top_y() const;
	void set_top_y(int p_top_y);
	int get_top_y() const;

	void set_has_bottom_y(bool p_has);
	bool has_bottom_y() const;
	void set_bottom_y(int p_bottom_y);
	int get_bottom_y() const;

	// Entries (polymorphic array of CbinEntry subclasses).
	void set_entries(const TypedArray<CbinEntry> &p_entries);
	TypedArray<CbinEntry> get_entries() const;

	int get_entry_count() const;
	Ref<CbinEntry> get_entry(int p_index) const;
	void add_entry(const Ref<CbinEntry> &p_entry);
	void insert_entry(int p_index, const Ref<CbinEntry> &p_entry);
	void remove_entry(int p_index);
	void clear_entries();

	// Text serialization for editor.
	String to_text() const;
	bool from_text(const String &p_text);

	// Build a credits resource from raw CBIN bytes (the .kda/.cbin DATASOURCE of a
	// marquee_wnd, loadable from a PFF via the resource root). Converts the ENV section
	// and the TEXT entries (text / color / justify / newline / image); asset names are
	// recorded on the entries, but font/texture resolution is left to the caller.
	// Returns null when the bytes are not a valid CBIN file.
	static Ref<CbinCreditsResource> from_cbin_bytes(const PackedByteArray &p_data);

private:
	float scroll_rate_ = 0.5f;
	int vertical_space_ = 14;
	int center_x_ = 400;
	bool has_top_y_ = false;
	int top_y_ = 0;
	bool has_bottom_y_ = false;
	int bottom_y_ = 0;
	Vector<Ref<CbinEntry>> entries_;

	void _on_entry_changed();
	void _connect_entry(const Ref<CbinEntry> &p_entry);
	void _disconnect_entry(const Ref<CbinEntry> &p_entry);
	bool _contains_entry_ref(const Ref<CbinEntry> &p_entry) const;
};

}  // namespace godot

VARIANT_ENUM_CAST(godot::CbinEntryType);
VARIANT_ENUM_CAST(godot::CbinJustify);

#endif  // OPENNOVA_CBIN_CREDITS_RESOURCE_H
