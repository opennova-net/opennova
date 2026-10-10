#include <editor/preview/menu_sounds.h>

#include <cstdio>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mnu_document.h>
#include <formats/lwf/lwf.h>
#include <formats/mnu/mnu.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/menu/menu_sound.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The window at `preorder` among the windows under `window` (itself first, then its children in order, not
// its parts), counting down `preorder`; null when the count runs past them.
const mnu::Window *preorder_window(const mnu::Window &window, int &preorder) {
	if (preorder == 0) return &window;
	--preorder;
	for (const mnu::Window &child : window.children)
		if (const mnu::Window *found = preorder_window(child, preorder)) return found;
	return nullptr;
}

} // namespace

io::JsonValue menu_sound_options_to_json(const MenuSoundOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("mute", JsonValue::make_bool(options.mute));
	return out;
}

bool read_menu_sound_options(const io::JsonValue &json, MenuSoundOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options.sound is an object {mute}.";
		return false;
	}
	MenuSoundOptions options = held;
	for (const io::JsonMember &member : json.object) {
		if (member.key != "mute") {
			error = "Unknown sound option \"" + member.key + "\" (it takes mute).";
			return false;
		}
		if (!member.value.is_bool()) {
			error = "options.sound.mute is true or false.";
			return false;
		}
		options.mute = member.value.boolean;
	}
	held = options;
	return true;
}

// --- MenuSoundBanks ----------------------------------------------------------------------------------

const PreviewBank *MenuSoundBanks::bank(const ProjectAssetSource &files, const std::string &name, int32_t *key) {
	if (key) *key = 0;
	if (name.empty()) return nullptr;
	// The menu's collection holds a bank once, its names compared without case [orig:
	// SoundBank_CollectionAddOrRef @ 0x652b40, stricmp].
	Held *held = nullptr;
	for (Held &each : held_)
		if (strutil::iequals(each.name, name)) held = &each;
	const uint64_t stamp = files.stamp(name);
	if (held && held->stamp == stamp) {
		if (key && held->read) *key = int32_t(held->index + 1);
		return held->read ? &banks_[held->index] : nullptr;
	}
	if (!held) {
		held_.push_back(Held());
		held = &held_.back();
		held->name = name;
		held->index = banks_.size();
		banks_.push_back(PreviewBank());
	}
	held->stamp = stamp;
	held->read = false;
	PreviewBank &bank = banks_[held->index];
	bank.name = name;
	bank.path = files.path_of(name);
	bank.file = lwf::File();
	std::vector<uint8_t> bytes;
	std::string error;
	held->read = files.read(name, bytes) && !bytes.empty() &&
	             lwf::parse_lwf_buffer(bytes.data(), bytes.size(), bank.file, error);
	if (!held->read) bank.file = lwf::File();
	if (key && held->read) *key = int32_t(held->index + 1);
	return held->read ? &bank : nullptr;
}

// --- a window's sound ----------------------------------------------------------------------------------

io::JsonValue menu_sound_fired_to_json(const MenuSoundFired &fired) {
	JsonValue row = JsonValue::make_object();
	row.set("seq", json_number(double(fired.sound.seq)));
	row.set("sound", json_string(menu::menu_sound_state_token(fired.state)));
	row.set("screen", json_string(fired.screen));
	row.set("window", json_number(double(fired.window)));
	row.set("name", json_string(fired.name));
	row.set("set", json_string(fired.sound.set));
	row.set("bank", json_string(fired.sound.bank));
	row.set("state", json_string(fired.sound.state));
	row.set("words", json_string(fired.sound.words));
	// The voices as a clip's sound writes them (wave, path, pitch, volume).
	JsonValue clip = clip_sound_fired_to_json(fired.sound);
	if (JsonValue *voices = clip.get("voices")) row.set("voices", std::move(*voices));
	return row;
}

MenuSoundFired plan_menu_sound(const mnu::Sound &row, MenuSoundBanks &banks, const ProjectAssetSource &files,
                               const AssetScan *scan, audio::SoundSelector &selector, bool mute) {
	MenuSoundFired fired;
	ClipSoundFired &sound = fired.sound;
	sound.slot = -1;
	sound.set = row.trigger;
	sound.bank = row.file;
	// Its own bank alone [orig: sound_collection_play_trigger @ 0x652de0 plays from the row's bank entry].
	int32_t key = 0;
	const PreviewBank *bank = banks.bank(files, row.file, &key);
	if (!bank) {
		sound.state = "no_bank";
		sound.words = row.trigger + ": " +
		              (row.file.empty() ? std::string("the SOUND names no bank")
		                                : "the project has no sound bank " + row.file + " the game reads") +
		              ", so the game plays nothing.";
		return fired;
	}
	// The bank's first set of the name, without case [orig: SoundBank_FindTriggerByName @ 0x75be90], played as
	// the menu plays it: menu::plan_menu_sound, the game's own (no listener view filters its layers, no jitter
	// draws) [orig: SoundBank_PlayTriggerEntries @ 0x75ccd0].
	const int32_t index = row.trigger.empty() ? -1 : audio::find_bank_set(bank->file, row.trigger);
	if (index < 0) {
		sound.state = "missing";
		sound.words = row.trigger.empty() ? std::string("No set is named: the game plays nothing.")
		                                  : row.file + " has no set named " + row.trigger + ": the game plays nothing.";
		return fired;
	}
	sound.set = bank->file.multis[size_t(index)].name;
	std::string words;
	for (const menu::MenuSoundVoice &voice :
	     menu::plan_menu_sound(bank->file, key, row.trigger, menu::kMenuMasterVolumeDefault, selector)) {
		if (voice.path.empty()) continue; // a member naming no wave: nothing plays
		ClipSoundFired::Voice out;
		for (const lwf::Single &single : bank->file.singles)
			if (single.path == voice.path) {
				out.wave = single.name;
				break;
			}
		out.file = io::utf8_file_name(voice.path);
		out.pitch_q16 = lwf::pitch_to_q16(voice.pitch);
		out.volume = voice.volume;
		char pitch[32];
		std::snprintf(pitch, sizeof(pitch), "%.2f", voice.pitch);
		words += (words.empty() ? "" : "; ") + out.file + " at pitch " + pitch + ", volume " + std::to_string(out.volume);
		sound.voices.push_back(std::move(out));
	}
	sound.words = sound.set + " in " + bank->name +
	              (words.empty() ? std::string(": no layer has a member to play.") : ": " + words + ".");
	sound.state = sound.voices.empty() ? "silent" : mute ? "muted" : "played";
	if (scan) find_clip_sound_waves(sound, *scan);
	return fired;
}

std::vector<MenuWindowSound> menu_window_sounds(const mnu::Window &window) {
	std::vector<MenuWindowSound> out;
	for (const int state : {menu::kSoundMouseIn, menu::kSoundSelected, menu::kSoundMouseOut})
		if (const mnu::Sound *row = menu::menu_window_sound(window, state)) out.push_back({state, row->trigger, row->file});
	return out;
}

const char *menu_sound_state_words(int state) {
	switch (state) {
	case menu::kSoundMouseIn: return "on hover";
	case menu::kSoundSelected: return "on click";
	case menu::kSoundMouseOut: return "on leaving";
	default: return "";
	}
}

io::JsonValue menu_window_sounds_to_json(const std::vector<MenuWindowSound> &sounds) {
	JsonValue out = JsonValue::make_array();
	for (const MenuWindowSound &each : sounds) {
		JsonValue row = JsonValue::make_object();
		row.set("sound", json_string(menu::menu_sound_state_token(each.state)));
		row.set("when", json_string(menu_sound_state_words(each.state)));
		row.set("set", json_string(each.set));
		row.set("bank", json_string(each.bank));
		out.push(std::move(row));
	}
	return out;
}

const mnu::Window *menu_image_window(const MnuDocument &document, const NodeAddress &window,
                                     std::shared_ptr<const mnu::Document> &image) {
	image = document.saved_image();
	const int index = document.window_index(window);
	const size_t position = document.screen_position(window.row);
	if (!image || index < 0 || position >= image->screens.size()) return nullptr;
	int preorder = index;
	for (const mnu::Window &root : image->screens[position].roots)
		if (const mnu::Window *found = preorder_window(root, preorder)) return found;
	return nullptr;
}

} // namespace opennova::editor
