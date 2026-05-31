#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "mns_stylesheet.h"
#include "nova_mnu_document.h"
#include "rtxt/rtxt_string_file.h"

namespace godot {

class NovaResourceRoot;

// Runtime menu node: point it at a NovaMnuDocument and it builds the live widget
// tree (one NovaMnuScreen child per screen). Assets resolve through an optional
// NovaResourceRoot / MnsStyleSheet / RtxtStringFile (wired in M4); interactivity
// and audio land in M5. edit_mode makes the tree inert + click-through so the
// ONED editor can reuse this exact node as a WYSIWYG preview canvas.
class NovaMnuMenu : public Control {
	GDCLASS(NovaMnuMenu, Control)

private:
	Ref<NovaMnuDocument> menu_;
	Ref<NovaResourceRoot> resource_root_;
	Ref<MnsStyleSheet> stylesheet_;
	Ref<RtxtStringFile> text_resource_;
	String current_screen_;
	bool edit_mode_ = false;
	bool build_on_ready_ = true;

	void apply_screen_visibility();

protected:
	static void _bind_methods();

public:
	void _ready() override;

	// --- Properties ---
	void set_menu(const Ref<NovaMnuDocument> &p_menu);
	Ref<NovaMnuDocument> get_menu() const { return menu_; }

	void set_resource_root(const Ref<NovaResourceRoot> &p_root);
	Ref<NovaResourceRoot> get_resource_root() const { return resource_root_; }

	void set_stylesheet(const Ref<MnsStyleSheet> &p_sheet);
	Ref<MnsStyleSheet> get_stylesheet() const { return stylesheet_; }

	void set_text_resource(const Ref<RtxtStringFile> &p_text);
	Ref<RtxtStringFile> get_text_resource() const { return text_resource_; }

	void set_current_screen(const String &p_name);
	String get_current_screen() const { return current_screen_; }

	void set_edit_mode(bool p_edit);
	bool get_edit_mode() const { return edit_mode_; }

	void set_build_on_ready(bool p_value) { build_on_ready_ = p_value; }
	bool get_build_on_ready() const { return build_on_ready_; }

	// --- Build / navigation ---
	void build();
	void clear();
	PackedStringArray get_screen_names() const;
	bool show_screen(const String &p_name);
};

} // namespace godot
