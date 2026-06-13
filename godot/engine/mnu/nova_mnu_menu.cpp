#include "nova_mnu_menu.h"

#include "nova_mnu_builder.h"
#include "nova_mnu_button.h"
#include "nova_mnu_goto.h"
#include "nova_mnu_screen.h"
#include "resource_index/nova_resource_root.h"

#include "audio/nova_music_director.h"
#include "audio/nova_sbf_audio_stream.h"
#include "lwf/nova_wav_loader.h"

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void NovaMnuMenu::_ready() {
	// Receive keyboard accelerators (Esc/Enter) for the hotkey router; the handler
	// is inert in edit_mode, so enabling it unconditionally is safe for the editor
	// preview that reuses this node.
	set_process_unhandled_key_input(true);
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
	// Banks were resolved against the old root; drop them (and the per-layer
	// selection cursors keyed by their ids) so a new game dir reloads cleanly.
	lwf_banks_.clear();
	next_lwf_bank_id_ = 1;
	sound_selector_.reset();
}

void NovaMnuMenu::set_master_volume(int p_volume) {
	master_volume_ = CLAMP(p_volume, 0, 255);
}

void NovaMnuMenu::set_stylesheet(const Ref<MnsStyleSheet> &p_sheet) {
	stylesheet_ = p_sheet;
}

void NovaMnuMenu::set_text_resource(const Ref<RtxtStringFile> &p_text) {
	text_resource_ = p_text;
}

void NovaMnuMenu::set_sound_profile(const Ref<NovaLwfData> &p_profile) {
	sound_profile_ = p_profile;
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

void NovaMnuMenu::set_interactive(bool p_on) {
	if (interactive_ == p_on) {
		return;
	}
	interactive_ = p_on;
	// Rebuild so the builder re-applies its edit-mode suppressions with the new
	// interactive state (navigators enabled + clickable when on) and authored window
	// visibility (the `hidden` flags) resets; then collapse to a single visible screen.
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
	ctx.document = menu_.ptr();
	ctx.edit_mode = edit_mode_;
	ctx.interactive = interactive_;

	const mnu::Document &doc = menu_->get_native();
	// get_screen_ids()[i] aligns with doc.screens[i] (both built in the same order by
	// rebuild_ids), so the i-th built screen's root window id is screen_ids[i]'s root.
	const PackedInt32Array screen_ids = menu_->get_screen_ids();
	int screen_index = 0;
	for (const auto &screen : doc.screens) {
		const int root_window_id = screen_index < screen_ids.size()
				? menu_->get_screen_root_id(screen_ids[screen_index])
				: -1;
		++screen_index;
		Control *screen_node = mnu_build_screen(screen, ctx, root_window_id);
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
	// Author mode shows every screen at once (a flat canvas of all screens); the
	// interactive preview behaves like runtime and shows only the current screen.
	const bool show_all = edit_mode_ && !interactive_;
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
		// Popping past the root is a host-level back/quit; in the interactive preview
		// there is no host, so it is a silent no-op rather than a quit.
		if (!interactive_) {
			emit_signal("quit_requested");
		}
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

// Maps a Godot keycode to the JO virtual-key name used in <HOTKEY> elements. Only
// the keys shipped menus actually bind are mapped (ESCAPE / ENTER); extend as new
// hotkeys appear in real content.
static String vk_name_for_keycode(Key p_keycode) {
	switch (p_keycode) {
		case Key::KEY_ESCAPE:
			return "VK_ESCAPE";
		case Key::KEY_ENTER:
		case Key::KEY_KP_ENTER:
			return "VK_RETURN";
		default:
			return String();
	}
}

// VK_RETURN and VK_ENTER are interchangeable spellings in MNU content.
static bool hotkey_matches(const String &p_stored, const String &p_pressed) {
	if (p_stored == p_pressed) {
		return true;
	}
	const bool stored_enter = p_stored == "VK_RETURN" || p_stored == "VK_ENTER";
	const bool pressed_enter = p_pressed == "VK_RETURN" || p_pressed == "VK_ENTER";
	return stored_enter && pressed_enter;
}

Node *NovaMnuMenu::find_hotkey_target(Node *p_node, const String &p_vk) const {
	if (p_node == nullptr) {
		return nullptr;
	}
	// A hidden Control (and its whole subtree) is unreachable by the keyboard, just
	// as it is unclickable: skip it so a hidden BACK/cancel does not eat the key and
	// the visible target wins (e.g. a closed modal's controls stay inert). Shipped
	// menus rely on this -- jo_cmap/jo_game hide confirm dialogs that carry VK_ESCAPE.
	Control *ctrl = Object::cast_to<Control>(p_node);
	if (ctrl != nullptr && !ctrl->is_visible()) {
		return nullptr;
	}
	if (p_node->has_meta("mnu_hotkey")) {
		const String hk = p_node->get_meta("mnu_hotkey");
		if (hotkey_matches(hk, p_vk)) {
			return p_node;
		}
	}
	for (int i = 0; i < p_node->get_child_count(); ++i) {
		Node *found = find_hotkey_target(p_node->get_child(i), p_vk);
		if (found != nullptr) {
			return found;
		}
	}
	return nullptr;
}

// Activates a hotkey target as a mouse click would. Returns true only when the
// activation has a real effect worth consuming the key for (a dispatched MNU
// action, a state toggle, or a Goto), so a hotkey on an actionless button leaves
// the key free to propagate (e.g. Esc falling through to the in-game pause).
bool NovaMnuMenu::trigger_hotkey_target(Node *p_target) {
	BaseButton *button = Object::cast_to<BaseButton>(p_target);
	if (button != nullptr) {
		bool effect = false;
		if (button->is_toggle_mode()) {
			// Checkbox/radio: flip state so the "toggled" handler runs (a bare
			// emit "pressed" does not toggle a NovaMnuCheckBox).
			button->set_pressed(!button->is_pressed());
			effect = true;
		}
		NovaMnuButton *nova = Object::cast_to<NovaMnuButton>(button);
		if (nova != nullptr && nova->get_action_count() > 0) {
			effect = true;
		}
		// Always emit so the button's own actions and any host-wired (name-keyed)
		// handler run; `effect` only governs whether the key is consumed.
		button->emit_signal("pressed");
		return effect;
	}
	NovaMnuGoto *go = Object::cast_to<NovaMnuGoto>(p_target);
	if (go != nullptr) {
		go->trigger();
		return true;
	}
	return false;
}

bool NovaMnuMenu::handle_hotkey(const String &p_vk) {
	if (edit_mode_ || p_vk.is_empty()) {
		return false;
	}
	NovaMnuScreen *screen = find_screen(current_screen_);
	if (screen == nullptr) {
		return false;
	}
	Node *target = find_hotkey_target(screen, p_vk);
	if (target == nullptr) {
		return false;
	}
	// Only report handled when the activation actually did something, so a matched
	// but inert widget does not swallow the key from the host (e.g. Esc-to-resume).
	return trigger_hotkey_target(target);
}

bool NovaMnuMenu::handle_key_input(const Ref<InputEventKey> &p_key) {
	// A hidden/backgrounded menu kept in the tree (the menu front-end during
	// gameplay) must not route or consume keys, else it steals Esc from the world.
	if (edit_mode_ || !is_visible_in_tree()) {
		return false;
	}
	if (p_key.is_null() || !p_key->is_pressed() || p_key->is_echo()) {
		return false;
	}
	const String vk = vk_name_for_keycode(p_key->get_keycode());
	if (vk.is_empty()) {
		return false;
	}
	return handle_hotkey(vk);
}

void NovaMnuMenu::_unhandled_key_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && handle_key_input(key) && get_viewport() != nullptr) {
		get_viewport()->set_input_as_handled();
	}
}

bool NovaMnuMenu::dispatch_action(const String &p_type, const String &p_target,
		const String &p_file, const String &p_window_state) {
	emit_signal("action_dispatched", p_type, p_target);
	const String type = p_type.to_lower();
	if (type == "window") {
		return handle_window_action(p_target, p_window_state);
	}
	if (type == "screen") {
		// Shipped menus spell same-file jumps with their own filename
		// (mp.mnu: <ACTION type="SCREEN" file="mp.mnu">MULTI_PLAYER_HOST</ACTION>;
		// a screen action with NO file at all crashes the original engine), so a
		// file matching this menu's own name is same-file navigation.
		if (p_file.is_empty() ||
				(!menu_file_.is_empty() && p_file.nocasecmp_to(menu_file_) == 0)) {
			return navigate_to_screen(p_target);
		}
		// Cross-.mnu jumps are host policy; the interactive preview has no host to load
		// another file, so the jump is consumed as a no-op instead of escaping.
		if (interactive_) {
			return true;
		}
		navigate_to_menu(p_file, p_target);
		return true;
	}
	if (type == "pop" || type == "pop_screen") {
		return pop_screen();
	}
	if (type == "quit" || type == "quit_game") {
		// Must not bubble out of the editor's interactive preview to the host.
		if (interactive_) {
			return true;
		}
		quit_game();
		return true;
	}
	if (type == "url") {
		// type="URL" actions (shipped menus' website/buy buttons) are host policy:
		// the runtime opens them externally. EXTERNAL_BROWSER is preserved on the
		// model for round-trip; the runtime always routes URLs to the host. The
		// interactive preview swallows them so an authoring click never opens a browser.
		if (interactive_) {
			return true;
		}
		emit_signal("url_requested", p_target);
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
	// One pooled player per original mixer channel: a set play opens up to 8
	// channels, one per layer [orig: 8-slot open loop @ 0x75ce06].
	const int pool_size = 8;
	for (int i = 0; i < pool_size; ++i) {
		AudioStreamPlayer *player = memnew(AudioStreamPlayer);
		player->set_name(String("_MnuSound") + String::num_int64(i));
		add_child(player);
		sound_players_.push_back(player);
	}
}

void NovaMnuMenu::play_widget_sound(const String &p_trigger, const String &p_file) {
	emit_signal("sound_requested", p_file, p_trigger);
	if (edit_mode_) {
		return;
	}
	// Faithful path: the <SOUND> element text names a .lwf bank; the trigger
	// names a sound set inside it. [orig: widget_process_mouse_event @ 0x647a00
	//  -> collection play @ 0x652de0 -> SoundBank_FindTriggerByName @ 0x75be90
	//  -> SoundBank_PlayTriggerEntries @ 0x75ccd0]
	int bank_id = 0;
	Ref<NovaLwfData> bank = resolve_sound_bank(p_file, bank_id);
	if (bank.is_null() && sound_profile_.is_valid()) {
		// Authoring fallback: the host profile services file-less or unresolved
		// <SOUND> nodes (the original fails the element parse / stays silent).
		bank = sound_profile_;
		bank_id = 0;
	}
	if (play_lwf_set(bank, bank_id, p_trigger)) {
		return;
	}
	// Legacy fallback: an SBF bank keyed by trigger / file stem (pre-LWF wiring).
	play_sbf_sound(p_trigger, p_file);
}

Ref<NovaLwfData> NovaMnuMenu::resolve_sound_bank(const String &p_file, int &r_bank_id) {
	r_bank_id = 0;
	if (p_file.is_empty() || resource_root_.is_null()) {
		return Ref<NovaLwfData>();
	}
	// The original collection dedups bank entries with stricmp on the name
	// [orig: @ 0x652b6b]; lowercase keys give the same fold.
	const String key = p_file.to_lower();
	if (const LwfBankEntry *cached = lwf_banks_.getptr(key)) {
		r_bank_id = cached->id;
		return cached->bank;
	}
	LwfBankEntry entry;
	Ref<NovaLwfData> bank;
	bank.instantiate();
	if (bank->open_from_resource_root(resource_root_, p_file) == OK && bank->is_loaded()) {
		entry.bank = bank;
		entry.id = next_lwf_bank_id_++;
	}
	// A failed open caches the null bank: the original leaves the element's
	// bank id 0 and the element simply never sounds [orig: @ 0x652c95].
	lwf_banks_.insert(key, entry);
	r_bank_id = entry.id;
	return entry.bank;
}

bool NovaMnuMenu::play_lwf_set(const Ref<NovaLwfData> &p_bank, int p_bank_id, const String &p_trigger) {
	if (p_bank.is_null() || resource_root_.is_null() || p_trigger.is_empty()) {
		return false;
	}
	const String want = p_trigger.to_upper();
	const int set_count = p_bank->get_set_count();
	for (int si = 0; si < set_count; ++si) {
		const Dictionary set_d = p_bank->get_set(si);
		if (String(set_d.get("name", "")).to_upper() != want) {
			continue;
		}
		// Set-level pitch composes multiplicatively with the member pitch
		// (Q16; 0xFFFF ~ 1.0) [orig: (member * set) >> 16 @ 0x75c0be]. The
		// per-play pitch/volume jitter draws are not reproduced (same accepted
		// divergence as the mission sound host; see docs/audio/lwf-dbf-sound-re.md).
		const double set_pitch = double((int64_t)set_d.get("pitch_base", 0xFFFF)) / 65536.0;
		const Array layers = set_d.get("layers", Array());
		bool played = false;
		for (int li = 0; li < layers.size(); ++li) {
			const Dictionary layer_d = layers[li];
			const Array members = layer_d.get("members", Array());
			if (members.is_empty()) {
				continue;
			}
			const int mode = (int)layer_d.get("selection_mode", (int)NovaLwfData::SELECTION_RANDOM);
			const int idx = sound_selector_.select(
					opennova::audio::SoundSelector::make_key(p_bank_id, si, li),
					members.size(), mode);
			if (idx < 0) {
				continue;
			}
			const Dictionary member = members[idx];
			// Channel volume [orig: @ 0x75cf25..0x75cf6e]: a layer with no
			// distances (every shipped menu layer) plays at the menu's master
			// volume and the member volume is not consulted; a layer with an
			// inner radius scales member volume by (master+1)/256 toward its
			// clamp (distance-curve global @ 0x85A3E4 approximated at the UI
			// emitter's distance 0).
			const int inner = (int)layer_d.get("inner_distance", 0);
			int vol255 = master_volume_;
			if (inner > 0) {
				int att = ((int)member.get("volume", 255) * (master_volume_ + 1)) >> 8;
				const int clampv = (int)member.get("clamp_volume", 255);
				if (clampv > 0 && att > clampv) {
					att = clampv;
				}
				vol255 = att;
			}
			double pitch = double(member.get("base_pitch", 1.0)) * set_pitch;
			if (pitch <= 0.01) {
				pitch = 1.0;
			}
			played = play_member_sound(member, vol255, pitch) || played;
		}
		return played;
	}
	return false;
}

bool NovaMnuMenu::play_member_sound(const Dictionary &p_member, int p_vol255, double p_pitch_scale) {
	const String wav_path = p_member.get("wav_path", "");
	if (wav_path.is_empty()) {
		return false;
	}
	// LWF paths are Windows-style (e.g. "SFX\\MENU\\MSOVR_2.wav"); the resource
	// root resolves the loose .wav by basename.
	const String name = wav_path.replace("\\", "/").get_file();
	if (name.is_empty()) {
		return false;
	}
	const PackedByteArray bytes = resource_root_->read_file(name);
	if (bytes.is_empty()) {
		return false;
	}
	Ref<AudioStreamWAV> stream = NovaWavLoader::from_bytes(bytes);
	if (stream.is_null()) {
		return false;
	}

	ensure_sound_pool();
	if (sound_players_.is_empty()) {
		return false;
	}
	AudioStreamPlayer *player = sound_players_[next_sound_player_];
	next_sound_player_ = (next_sound_player_ + 1) % sound_players_.size();
	player->set_stream(stream);
	player->set_pitch_scale((float)p_pitch_scale);
	double lin = (double)p_vol255 / 255.0;
	lin = lin < 0.0 ? 0.0 : (lin > 1.0 ? 1.0 : lin);
	player->set_volume_db(p_vol255 > 0 ? (float)UtilityFunctions::linear_to_db(lin) : -80.0f);
	player->play();
	return true;
}

bool NovaMnuMenu::play_sbf_sound(const String &p_trigger, const String &p_file) {
	if (sound_bank_.is_null()) {
		return false;
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
		return false;
	}

	Ref<NovaSbfAudioStream> stream = sound_bank_->get_stream(entry);
	if (stream.is_null()) {
		return false;
	}

	ensure_sound_pool();
	if (sound_players_.is_empty()) {
		return false;
	}
	AudioStreamPlayer *player = sound_players_[next_sound_player_];
	next_sound_player_ = (next_sound_player_ + 1) % sound_players_.size();
	player->set_stream(stream);
	player->play();
	return true;
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
	ClassDB::bind_method(D_METHOD("set_sound_profile", "profile"), &NovaMnuMenu::set_sound_profile);
	ClassDB::bind_method(D_METHOD("get_sound_profile"), &NovaMnuMenu::get_sound_profile);
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
	ClassDB::bind_method(D_METHOD("set_interactive", "on"), &NovaMnuMenu::set_interactive);
	ClassDB::bind_method(D_METHOD("get_interactive"), &NovaMnuMenu::get_interactive);
	ClassDB::bind_method(D_METHOD("set_build_on_ready", "value"), &NovaMnuMenu::set_build_on_ready);
	ClassDB::bind_method(D_METHOD("get_build_on_ready"), &NovaMnuMenu::get_build_on_ready);

	ClassDB::bind_method(D_METHOD("build"), &NovaMnuMenu::build);
	ClassDB::bind_method(D_METHOD("clear"), &NovaMnuMenu::clear);
	ClassDB::bind_method(D_METHOD("get_screen_names"), &NovaMnuMenu::get_screen_names);
	ClassDB::bind_method(D_METHOD("show_screen", "name"), &NovaMnuMenu::show_screen);
	ClassDB::bind_method(D_METHOD("navigate_to_screen", "name"), &NovaMnuMenu::navigate_to_screen);
	ClassDB::bind_method(D_METHOD("pop_screen"), &NovaMnuMenu::pop_screen);
	ClassDB::bind_method(D_METHOD("navigate_to_menu", "file", "target_screen"), &NovaMnuMenu::navigate_to_menu);
	ClassDB::bind_method(D_METHOD("set_menu_file", "file"), &NovaMnuMenu::set_menu_file);
	ClassDB::bind_method(D_METHOD("get_menu_file"), &NovaMnuMenu::get_menu_file);
	ClassDB::bind_method(D_METHOD("quit_game"), &NovaMnuMenu::quit_game);
	ClassDB::bind_method(D_METHOD("handle_window_action", "target", "state"), &NovaMnuMenu::handle_window_action);
	ClassDB::bind_method(D_METHOD("dispatch_action", "type", "target", "file", "window_state"),
			&NovaMnuMenu::dispatch_action);
	ClassDB::bind_method(D_METHOD("handle_hotkey", "vk"), &NovaMnuMenu::handle_hotkey);
	ClassDB::bind_method(D_METHOD("handle_key_input", "key"), &NovaMnuMenu::handle_key_input);
	ClassDB::bind_method(D_METHOD("clear_navigation_stack"), &NovaMnuMenu::clear_navigation_stack);
	ClassDB::bind_method(D_METHOD("play_widget_sound", "trigger", "file"), &NovaMnuMenu::play_widget_sound);
	ClassDB::bind_method(D_METHOD("set_master_volume", "volume"), &NovaMnuMenu::set_master_volume);
	ClassDB::bind_method(D_METHOD("get_master_volume"), &NovaMnuMenu::get_master_volume);
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
	ADD_PROPERTY(PropertyInfo(Variant::INT, "master_volume", PROPERTY_HINT_RANGE, "0,255,1"),
			"set_master_volume", "get_master_volume");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "current_screen"), "set_current_screen", "get_current_screen");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "edit_mode"), "set_edit_mode", "get_edit_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "interactive"), "set_interactive", "get_interactive");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "build_on_ready"), "set_build_on_ready", "get_build_on_ready");

	ADD_SIGNAL(MethodInfo("screen_changed", PropertyInfo(Variant::STRING, "screen_name")));
	ADD_SIGNAL(MethodInfo("menu_requested", PropertyInfo(Variant::STRING, "file"),
			PropertyInfo(Variant::STRING, "target_screen")));
	ADD_SIGNAL(MethodInfo("music_changed", PropertyInfo(Variant::INT, "music_var")));
	ADD_SIGNAL(MethodInfo("quit_requested"));
	ADD_SIGNAL(MethodInfo("url_requested", PropertyInfo(Variant::STRING, "url")));
	ADD_SIGNAL(MethodInfo("sound_requested", PropertyInfo(Variant::STRING, "file"),
			PropertyInfo(Variant::STRING, "trigger")));
	ADD_SIGNAL(MethodInfo("action_dispatched", PropertyInfo(Variant::STRING, "type"),
			PropertyInfo(Variant::STRING, "target")));
	ADD_SIGNAL(MethodInfo("widget_value_changed", PropertyInfo(Variant::STRING, "widget_name"),
			PropertyInfo(Variant::STRING, "kind"), PropertyInfo(Variant::INT, "index"),
			PropertyInfo(Variant::STRING, "value")));
}
