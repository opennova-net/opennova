#include "nova_mnu_menu.h"

#include "nova_mnu_builder.h"
#include "nova_mnu_screen.h"
#include "resource_index/nova_resource_root.h"

#include "audio/nova_music_director.h"
#include "audio/nova_sbf_audio_stream.h"

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

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

void NovaMnuMenu::set_sound_bank(const Ref<NovaSbfBank> &p_bank) {
	sound_bank_ = p_bank;
}

void NovaMnuMenu::set_music_director(NovaMusicDirector *p_director) {
	if (music_director_ == p_director) {
		return;
	}
	// The menu does not own the director; track its tree_exiting so a host that
	// frees it cannot leave us with a dangling pointer to deref on the next
	// screen change.
	const Callable cb = callable_mp(this, &NovaMnuMenu::on_director_exiting);
	if (music_director_ != nullptr && music_director_->is_connected("tree_exiting", cb)) {
		music_director_->disconnect("tree_exiting", cb);
	}
	music_director_ = p_director;
	if (music_director_ != nullptr && !music_director_->is_connected("tree_exiting", cb)) {
		music_director_->connect("tree_exiting", cb);
	}
}

void NovaMnuMenu::on_director_exiting() {
	music_director_ = nullptr;
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
	// Remove only screen nodes; the lazily created sound-player pool persists
	// across rebuilds (so a rebuild does not leave sound_players_ dangling).
	for (int i = get_child_count() - 1; i >= 0; --i) {
		Node *child = get_child(i);
		if (Object::cast_to<NovaMnuScreen>(child) != nullptr) {
			remove_child(child);
			child->queue_free();
		}
	}
}

void NovaMnuMenu::build() {
	clear();
	nav_stack_.clear();
	current_music_var_ = -1;
	if (menu_.is_null()) {
		unresolved_asset_count_ = 0;
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
			// Add hidden so each screen's _ready does not apply its cursor while
			// transiently visible; apply_screen_visibility() below then shows only
			// the current screen, firing exactly one cursor application.
			screen_node->set_visible(false);
			add_child(screen_node);
		}
	}
	unresolved_asset_count_ = static_cast<int>(ctx.unresolved_assets.size());

	// Default the visible screen to current_screen, else the first screen;
	// reset it if a doc swap dropped the previously shown screen.
	if (current_screen_.is_empty() || !has_screen(current_screen_)) {
		current_screen_ = doc.screens.empty()
				? String()
				: String::utf8(doc.screens.front().name.c_str());
	}
	apply_screen_visibility();
	if (!current_screen_.is_empty()) {
		on_screen_shown(current_screen_);
	}
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

NovaMnuScreen *NovaMnuMenu::find_screen(const String &p_name) const {
	for (int i = 0; i < get_child_count(); ++i) {
		NovaMnuScreen *screen = Object::cast_to<NovaMnuScreen>(get_child(i));
		if (screen != nullptr && screen->get_screen_name() == p_name) {
			return screen;
		}
	}
	return nullptr;
}

bool NovaMnuMenu::has_screen(const String &p_name) const {
	return find_screen(p_name) != nullptr;
}

void NovaMnuMenu::on_screen_shown(const String &p_name) {
	emit_signal("screen_changed", p_name);
	apply_music_for_screen(find_screen(p_name));
}

void NovaMnuMenu::apply_music_for_screen(NovaMnuScreen *p_screen) {
	if (p_screen == nullptr) {
		return;
	}
	const int music_var = p_screen->get_music_var();
	// A screen with no MUSICVAR (<= 0) declares no music and leaves the current
	// track alone; dedup repeats so re-showing a screen does not retrigger it.
	// Both match menu_manager.gd / mnu_screen.gd (act only on music_var > 0,
	// guarded by _current_music_var).
	if (music_var <= 0 || music_var == current_music_var_) {
		return;
	}
	current_music_var_ = music_var;
	emit_signal("music_changed", music_var);
	if (edit_mode_) {
		return;
	}
	if (music_director_ != nullptr) {
		music_director_->set_var(music_var_index_, music_var);
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
	if (!has_screen(p_name)) {
		return false;
	}
	current_screen_ = p_name;
	apply_screen_visibility();
	on_screen_shown(p_name);
	return true;
}

bool NovaMnuMenu::navigate_to_screen(const String &p_name) {
	if (!has_screen(p_name)) {
		return false;
	}
	if (p_name != current_screen_ && !current_screen_.is_empty()) {
		nav_stack_.push_back(current_screen_);
	}
	return show_screen(p_name);
}

bool NovaMnuMenu::pop_screen() {
	if (nav_stack_.is_empty()) {
		emit_signal("quit_requested");
		return false;
	}
	const String prev = nav_stack_[nav_stack_.size() - 1];
	nav_stack_.remove_at(nav_stack_.size() - 1);
	return show_screen(prev);
}

void NovaMnuMenu::navigate_to_menu(const String &p_file, const String &p_target_screen) {
	emit_signal("menu_requested", p_file, p_target_screen);
}

void NovaMnuMenu::quit_game() {
	emit_signal("quit_requested");
}

bool NovaMnuMenu::handle_window_action(const String &p_target, const String &p_state) {
	NovaMnuScreen *screen = find_screen(current_screen_);
	if (screen == nullptr) {
		return false;
	}
	Control *target = Object::cast_to<Control>(
			screen->find_child(p_target, true, false));
	if (target == nullptr) {
		return false;
	}
	const String state = p_state.to_lower();
	if (state == "hide") {
		target->set_visible(false);
	} else if (state == "toggle") {
		target->set_visible(!target->is_visible());
	} else {
		// Default ("show") matches the reference.
		target->set_visible(true);
	}
	return true;
}

bool NovaMnuMenu::dispatch_action(const String &p_type, const String &p_target,
		const String &p_file, const String &p_window_state) {
	emit_signal("action_dispatched", p_type, p_target);
	const String type = p_type.to_lower();
	if (type == "window") {
		return handle_window_action(p_target, p_window_state);
	}
	if (type == "screen") {
		if (p_file.is_empty()) {
			return navigate_to_screen(p_target);
		}
		navigate_to_menu(p_file, p_target);
		return true;
	}
	if (type == "pop" || type == "pop_screen") {
		return pop_screen();
	}
	if (type == "quit" || type == "quit_game") {
		quit_game();
		return true;
	}
	return false;
}

void NovaMnuMenu::clear_navigation_stack() {
	nav_stack_.clear();
}

void NovaMnuMenu::ensure_sound_pool() {
	if (!sound_players_.is_empty()) {
		return;
	}
	const int pool_size = 4;
	for (int i = 0; i < pool_size; ++i) {
		AudioStreamPlayer *player = memnew(AudioStreamPlayer);
		player->set_name(String("_MnuSound") + String::num_int64(i));
		add_child(player);
		sound_players_.push_back(player);
	}
}

void NovaMnuMenu::play_widget_sound(const String &p_trigger, const String &p_file) {
	emit_signal("sound_requested", p_file, p_trigger);
	if (edit_mode_ || sound_bank_.is_null()) {
		return;
	}

	// Prefer the trigger as the bank key (the reference keys sounds by trigger);
	// fall back to the file's stem (e.g. "menu.lwf" -> "MENU").
	StringName entry;
	if (!p_trigger.is_empty() && sound_bank_->has_entry(StringName(p_trigger))) {
		entry = StringName(p_trigger);
	} else {
		const String stem = p_file.get_file().get_basename().to_upper();
		if (!stem.is_empty() && sound_bank_->has_entry(StringName(stem))) {
			entry = StringName(stem);
		}
	}
	if (entry == StringName()) {
		return;
	}

	Ref<NovaSbfAudioStream> stream = sound_bank_->get_stream(entry);
	if (stream.is_null()) {
		return;
	}

	ensure_sound_pool();
	if (sound_players_.is_empty()) {
		return;
	}
	AudioStreamPlayer *player = sound_players_[next_sound_player_];
	next_sound_player_ = (next_sound_player_ + 1) % sound_players_.size();
	player->set_stream(stream);
	player->play();
}

void NovaMnuMenu::notify_widget_value(const String &p_widget_name, const String &p_kind,
		int p_index, const String &p_value) {
	emit_signal("widget_value_changed", p_widget_name, p_kind, p_index, p_value);
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
	ClassDB::bind_method(D_METHOD("set_sound_bank", "bank"), &NovaMnuMenu::set_sound_bank);
	ClassDB::bind_method(D_METHOD("get_sound_bank"), &NovaMnuMenu::get_sound_bank);
	ClassDB::bind_method(D_METHOD("set_music_director", "director"), &NovaMnuMenu::set_music_director);
	ClassDB::bind_method(D_METHOD("get_music_director"), &NovaMnuMenu::get_music_director);
	ClassDB::bind_method(D_METHOD("set_music_var_index", "index"), &NovaMnuMenu::set_music_var_index);
	ClassDB::bind_method(D_METHOD("get_music_var_index"), &NovaMnuMenu::get_music_var_index);
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
	ClassDB::bind_method(D_METHOD("navigate_to_screen", "name"), &NovaMnuMenu::navigate_to_screen);
	ClassDB::bind_method(D_METHOD("pop_screen"), &NovaMnuMenu::pop_screen);
	ClassDB::bind_method(D_METHOD("navigate_to_menu", "file", "target_screen"), &NovaMnuMenu::navigate_to_menu);
	ClassDB::bind_method(D_METHOD("quit_game"), &NovaMnuMenu::quit_game);
	ClassDB::bind_method(D_METHOD("handle_window_action", "target", "state"), &NovaMnuMenu::handle_window_action);
	ClassDB::bind_method(D_METHOD("dispatch_action", "type", "target", "file", "window_state"),
			&NovaMnuMenu::dispatch_action);
	ClassDB::bind_method(D_METHOD("clear_navigation_stack"), &NovaMnuMenu::clear_navigation_stack);
	ClassDB::bind_method(D_METHOD("play_widget_sound", "trigger", "file"), &NovaMnuMenu::play_widget_sound);
	ClassDB::bind_method(D_METHOD("notify_widget_value", "widget_name", "kind", "index", "value"),
			&NovaMnuMenu::notify_widget_value);
	ClassDB::bind_method(D_METHOD("get_unresolved_asset_count"), &NovaMnuMenu::get_unresolved_asset_count);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "menu", PROPERTY_HINT_RESOURCE_TYPE, "NovaMnuDocument"),
			"set_menu", "get_menu");
	// resource_root / sound_bank / music_director are wired in code (set_* methods),
	// not via the inspector, so they get bound methods but no ADD_PROPERTY hint.
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "stylesheet", PROPERTY_HINT_RESOURCE_TYPE, "MnsStyleSheet"),
			"set_stylesheet", "get_stylesheet");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "text_resource", PROPERTY_HINT_RESOURCE_TYPE, "RtxtStringFile"),
			"set_text_resource", "get_text_resource");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "music_var_index"), "set_music_var_index", "get_music_var_index");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "current_screen"), "set_current_screen", "get_current_screen");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "edit_mode"), "set_edit_mode", "get_edit_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "build_on_ready"), "set_build_on_ready", "get_build_on_ready");

	ADD_SIGNAL(MethodInfo("screen_changed", PropertyInfo(Variant::STRING, "screen_name")));
	ADD_SIGNAL(MethodInfo("menu_requested", PropertyInfo(Variant::STRING, "file"),
			PropertyInfo(Variant::STRING, "target_screen")));
	ADD_SIGNAL(MethodInfo("music_changed", PropertyInfo(Variant::INT, "music_var")));
	ADD_SIGNAL(MethodInfo("quit_requested"));
	ADD_SIGNAL(MethodInfo("sound_requested", PropertyInfo(Variant::STRING, "file"),
			PropertyInfo(Variant::STRING, "trigger")));
	ADD_SIGNAL(MethodInfo("action_dispatched", PropertyInfo(Variant::STRING, "type"),
			PropertyInfo(Variant::STRING, "target")));
	ADD_SIGNAL(MethodInfo("widget_value_changed", PropertyInfo(Variant::STRING, "widget_name"),
			PropertyInfo(Variant::STRING, "kind"), PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::STRING, "value")));
}
