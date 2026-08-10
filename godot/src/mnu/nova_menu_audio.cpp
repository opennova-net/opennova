#include "nova_menu_audio.h"

#include "lwf/nova_lwf_data.h"
#include "lwf/nova_wav_loader.h"
#include "resource_index/nova_resource_root.h"

#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <lwf/lwf.h>
#include <menu/menu_sound.h>

using namespace godot;

MenuAudio::MenuAudio() {
	master_volume_ = opennova::menu::kMenuMasterVolumeDefault;
}

void MenuAudio::set_resource_root(const Ref<ResourceRoot> &p_root) {
	if (resource_root_ == p_root) {
		return;
	}
	resource_root_ = p_root;
	// A new root invalidates the bank cache and the per-set selection state
	// (the same reset the Control-tree owner performed on root swaps).
	lwf_banks_.clear();
	next_lwf_bank_id_ = 1;
	sound_selector_.reset();
}

void MenuAudio::set_sound_profile(const Ref<LwfData> &p_profile) {
	sound_profile_ = p_profile;
}

void MenuAudio::set_master_volume(int p_volume) {
	master_volume_ = p_volume < 0 ? 0 : (p_volume > 255 ? 255 : p_volume);
}

int MenuAudio::get_master_volume() const {
	return master_volume_;
}

void MenuAudio::ensure_sound_pool_() {
	if (!sound_players_.is_empty()) {
		return;
	}
	// One pooled player per original mixer channel (menu/menu_sound.h carries
	// the witness).
	for (int i = 0; i < opennova::menu::kMenuSoundChannels; ++i) {
		AudioStreamPlayer *player = memnew(AudioStreamPlayer);
		player->set_name(String("_MenuSound") + String::num_int64(i));
		add_child(player);
		sound_players_.push_back(player);
	}
}

bool MenuAudio::play_widget_sound(const String &p_trigger,
		const String &p_file) {
	// The <SOUND> element text names a .lwf bank; the trigger names a sound
	// set inside it (menu/menu_sound.h carries the witnessed play path).
	int bank_id = 0;
	Ref<LwfData> bank = resolve_sound_bank_(p_file, bank_id);
	if (bank.is_null() && sound_profile_.is_valid()) {
		// Authoring fallback: the shell profile services file-less or
		// unresolved <SOUND> nodes (the original stays silent).
		bank = sound_profile_;
		bank_id = 0;
	}
	return play_lwf_set_(bank, bank_id, p_trigger);
}

Ref<LwfData> MenuAudio::resolve_sound_bank_(const String &p_file,
		int &r_bank_id) {
	r_bank_id = 0;
	if (p_file.is_empty() || resource_root_.is_null()) {
		return Ref<LwfData>();
	}
	// The original collection dedups bank entries case-insensitively;
	// lowercase keys give the same fold, and a failed open caches the null
	// bank so a bad name is tried once (menu/menu_sound.h witness block).
	const String key = p_file.to_lower();
	if (const LwfBankEntry *cached = lwf_banks_.getptr(key)) {
		r_bank_id = cached->id;
		return cached->bank;
	}
	LwfBankEntry entry;
	Ref<LwfData> bank;
	bank.instantiate();
	if (bank->open_from_resource_root(resource_root_, p_file) == OK &&
			bank->is_loaded()) {
		entry.bank = bank;
		entry.id = next_lwf_bank_id_++;
	}
	lwf_banks_.insert(key, entry);
	r_bank_id = entry.id;
	return entry.bank;
}

bool MenuAudio::play_lwf_set_(const Ref<LwfData> &p_bank, int p_bank_id,
		const String &p_trigger) {
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
		const double set_pitch = opennova::lwf::pitch_from_q16(static_cast<uint32_t>(
				(int64_t)set_d.get("pitch_base",
						static_cast<int64_t>(opennova::lwf::kAuthoredSetPitchBase))));
		const Array layers = set_d.get("layers", Array());
		bool played = false;
		for (int li = 0; li < layers.size(); ++li) {
			const Dictionary layer_d = layers[li];
			const Array members = layer_d.get("members", Array());
			if (members.is_empty()) {
				continue;
			}
			const int mode = (int)layer_d.get("selection_mode",
					(int)LwfData::SELECTION_RANDOM);
			const int idx = sound_selector_.select(
					opennova::audio::SoundSelector::make_key(p_bank_id, si, li),
					members.size(), mode);
			if (idx < 0) {
				continue;
			}
			const Dictionary member = members[idx];
			const int vol255 = opennova::menu::menu_channel_volume(
					master_volume_, (int)member.get("volume", 255),
					(int)member.get("clamp_volume", 255),
					(int)layer_d.get("falloff_radius", 0));
			const double pitch = opennova::menu::menu_effective_pitch(
					double(member.get("base_pitch", 1.0)), set_pitch);
			played = play_member_sound_(member, vol255, pitch) || played;
		}
		return played;
	}
	return false;
}

bool MenuAudio::play_member_sound_(const Dictionary &p_member, int p_vol255,
		double p_pitch_scale) {
	const String wav_path = p_member.get("wav_path", "");
	if (wav_path.is_empty()) {
		return false;
	}
	// LWF paths are Windows-style (e.g. "SFX\\MENU\\MSOVR_2.wav"); the
	// resource root resolves the loose .wav by basename.
	const String name = wav_path.replace("\\", "/").get_file();
	if (name.is_empty()) {
		return false;
	}
	const PackedByteArray bytes = resource_root_->read_file(name);
	if (bytes.is_empty()) {
		return false;
	}
	Ref<AudioStreamWAV> stream = WavLoader::from_bytes(bytes);
	if (stream.is_null()) {
		return false;
	}

	ensure_sound_pool_();
	if (sound_players_.is_empty()) {
		return false;
	}
	AudioStreamPlayer *player = sound_players_[next_sound_player_];
	next_sound_player_ = (next_sound_player_ + 1) % sound_players_.size();
	player->set_stream(stream);
	player->set_pitch_scale((float)p_pitch_scale);
	double lin = (double)p_vol255 / 255.0;
	lin = lin < 0.0 ? 0.0 : (lin > 1.0 ? 1.0 : lin);
	player->set_volume_db(p_vol255 > 0
					? (float)UtilityFunctions::linear_to_db(lin)
					: -80.0f);
	player->play();
	return true;
}

void MenuAudio::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"),
			&MenuAudio::set_resource_root);
	ClassDB::bind_method(D_METHOD("set_sound_profile", "profile"),
			&MenuAudio::set_sound_profile);
	ClassDB::bind_method(D_METHOD("set_master_volume", "volume"),
			&MenuAudio::set_master_volume);
	ClassDB::bind_method(D_METHOD("get_master_volume"),
			&MenuAudio::get_master_volume);
	ClassDB::bind_method(D_METHOD("play_widget_sound", "trigger", "file"),
			&MenuAudio::play_widget_sound);
}
