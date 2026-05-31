#include "nova_mnu_menu.h"

#include "nova_mnu_builder.h"
#include "nova_mnu_screen.h"
#include "resource_index/nova_resource_root.h"

#include <godot_cpp/classes/engine.hpp>

using namespace godot;

void NovaMnuMenu::_ready() {
	if (build_on_ready_ && menu_.is_valid()) {
		build();
	}
}

void NovaMnuMenu::set_menu(const Ref<NovaMnuDocument> &p_menu) {
	menu_ = p_menu;
	if (is_inside_tree()) {
		build();
	}
}

void NovaMnuMenu::set_resource_root(const Ref<NovaResourceRoot> &p_root) {
	resource_root_ = p_root;
}

void NovaMnuMenu::set_stylesheet(const Ref<MnsStyleSheet> &p_sheet) {
	stylesheet_ = p_sheet;
}

void NovaMnuMenu::set_text_resource(const Ref<RtxtStringFile> &p_text) {
	text_resource_ = p_text;
}

void NovaMnuMenu::set_current_screen(const String &p_name) {
	current_screen_ = p_name;
	apply_screen_visibility();
}

void NovaMnuMenu::set_edit_mode(bool p_edit) {
	edit_mode_ = p_edit;
	if (is_inside_tree()) {
		build();
	}
}

void NovaMnuMenu::clear() {
	for (int i = get_child_count() - 1; i >= 0; --i) {
		Node *child = get_child(i);
		remove_child(child);
		child->queue_free();
	}
}

void NovaMnuMenu::build() {
	clear();
	if (menu_.is_null()) {
		return;
	}

	MnuBuildContext ctx;
	ctx.root = resource_root_.ptr();
	ctx.stylesheet = stylesheet_.ptr();
	ctx.text = text_resource_.ptr();
	ctx.owner = this;
	ctx.edit_mode = edit_mode_;

	const mnu::Document &doc = menu_->get_native();
	for (const auto &screen : doc.screens) {
		Control *screen_node = mnu_build_screen(screen, ctx);
		if (screen_node) {
			add_child(screen_node);
		}
	}

	// Default the visible screen to current_screen, else the first screen.
	if (current_screen_.is_empty() && !doc.screens.empty()) {
		current_screen_ = String::utf8(doc.screens.front().name.c_str());
	}
	apply_screen_visibility();
}

void NovaMnuMenu::apply_screen_visibility() {
	const bool show_all = edit_mode_;
	for (int i = 0; i < get_child_count(); ++i) {
		NovaMnuScreen *screen = Object::cast_to<NovaMnuScreen>(get_child(i));
		if (screen == nullptr) {
			continue;
		}
		if (show_all) {
			screen->set_visible(true);
		} else {
			screen->set_visible(screen->get_screen_name() == current_screen_);
		}
	}
}

PackedStringArray NovaMnuMenu::get_screen_names() const {
	PackedStringArray out;
	if (menu_.is_null()) {
		return out;
	}
	const mnu::Document &doc = menu_->get_native();
	for (const auto &screen : doc.screens) {
		out.push_back(String::utf8(screen.name.c_str()));
	}
	return out;
}

bool NovaMnuMenu::show_screen(const String &p_name) {
	if (menu_.is_null()) {
		return false;
	}
	bool found = false;
	for (int i = 0; i < get_child_count(); ++i) {
		NovaMnuScreen *screen = Object::cast_to<NovaMnuScreen>(get_child(i));
		if (screen != nullptr && screen->get_screen_name() == p_name) {
			found = true;
			break;
		}
	}
	if (!found) {
		return false;
	}
	current_screen_ = p_name;
	apply_screen_visibility();
	return true;
}

void NovaMnuMenu::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_menu", "menu"), &NovaMnuMenu::set_menu);
	ClassDB::bind_method(D_METHOD("get_menu"), &NovaMnuMenu::get_menu);
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"), &NovaMnuMenu::set_resource_root);
	ClassDB::bind_method(D_METHOD("get_resource_root"), &NovaMnuMenu::get_resource_root);
	ClassDB::bind_method(D_METHOD("set_stylesheet", "stylesheet"), &NovaMnuMenu::set_stylesheet);
	ClassDB::bind_method(D_METHOD("get_stylesheet"), &NovaMnuMenu::get_stylesheet);
	ClassDB::bind_method(D_METHOD("set_text_resource", "text"), &NovaMnuMenu::set_text_resource);
	ClassDB::bind_method(D_METHOD("get_text_resource"), &NovaMnuMenu::get_text_resource);
	ClassDB::bind_method(D_METHOD("set_current_screen", "name"), &NovaMnuMenu::set_current_screen);
	ClassDB::bind_method(D_METHOD("get_current_screen"), &NovaMnuMenu::get_current_screen);
	ClassDB::bind_method(D_METHOD("set_edit_mode", "edit"), &NovaMnuMenu::set_edit_mode);
	ClassDB::bind_method(D_METHOD("get_edit_mode"), &NovaMnuMenu::get_edit_mode);
	ClassDB::bind_method(D_METHOD("set_build_on_ready", "value"), &NovaMnuMenu::set_build_on_ready);
	ClassDB::bind_method(D_METHOD("get_build_on_ready"), &NovaMnuMenu::get_build_on_ready);

	ClassDB::bind_method(D_METHOD("build"), &NovaMnuMenu::build);
	ClassDB::bind_method(D_METHOD("clear"), &NovaMnuMenu::clear);
	ClassDB::bind_method(D_METHOD("get_screen_names"), &NovaMnuMenu::get_screen_names);
	ClassDB::bind_method(D_METHOD("show_screen", "name"), &NovaMnuMenu::show_screen);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "menu", PROPERTY_HINT_RESOURCE_TYPE, "NovaMnuDocument"),
			"set_menu", "get_menu");
	// resource_root is a RefCounted (not a Resource), set via code (set_resource_root)
	// rather than the inspector, so it gets bound methods but no ADD_PROPERTY hint.
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "stylesheet", PROPERTY_HINT_RESOURCE_TYPE, "MnsStyleSheet"),
			"set_stylesheet", "get_stylesheet");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "text_resource", PROPERTY_HINT_RESOURCE_TYPE, "RtxtStringFile"),
			"set_text_resource", "get_text_resource");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "current_screen"), "set_current_screen", "get_current_screen");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "edit_mode"), "set_edit_mode", "get_edit_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "build_on_ready"), "set_build_on_ready", "get_build_on_ready");
}
