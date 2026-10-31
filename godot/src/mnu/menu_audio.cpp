#include "mnu/menu_audio.h"

#include "audio/audio_out.h"
#include "resource_index/resource_root.h"
#include "util/string_convert.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <formats/lwf/lwf.h>
#include <formats/lwf/wav_pcm.h>
#include <runtime/menu/menu_sound.h>

#include <cmath>

using namespace godot;

MenuAudio::MenuAudio() {
	master_volume_ = opennova::menu::kMenuMasterVolumeDefault;
}

void MenuAudio::set_resource_root(const Ref<ResourceRoot> &p_root) {
	if (resource_root_ == p_root) {
		return;
	}
	resource_root_ = p_root;
	// A new root invalidates the bank collection and the per-set selection
	// state (the same reset the Control-tree owner performed on root swaps).
	banks_.clear();
	sound_selector_.reset();
}

bool MenuAudio::play_widget_sound(const String &p_trigger,
		const String &p_file) {
	// The <SOUND> element text names a .lwf bank; the trigger names a sound
	// set inside it (menu/menu_sound.h carries the witnessed play path).
	// A bank that did not open (or a file-less element) leaves the element no
	// bank, so its trigger plays nothing: no other bank stands in.
	int32_t bank_key = 0;
	const opennova::lwf::File *bank = resolve_sound_bank_(p_file, bank_key);
	if (bank == nullptr || resource_root_.is_null() || p_trigger.is_empty()) {
		return false;
	}
	bool played = false;
	for (const opennova::menu::MenuSoundVoice &voice : opennova::menu::plan_menu_sound(*bank,
				 bank_key, opennova::to_std(p_trigger), master_volume_, sound_selector_)) {
		played = play_member_sound_(opennova::to_gd(voice.path), voice.volume, voice.pitch) || played;
	}
	return played;
}

const opennova::lwf::File *MenuAudio::resolve_sound_bank_(const String &p_file,
		int32_t &r_bank_key) {
	r_bank_key = 0;
	if (p_file.is_empty() || resource_root_.is_null()) {
		return nullptr;
	}
	// The bank as the VFS holds it, by its file name; a failed open stays in
	// the collection as no bank, so a bad name is tried once.
	const Ref<ResourceRoot> root = resource_root_;
	return banks_.bank(opennova::to_std(p_file),
			[&root](const std::string &p_name, opennova::lwf::File &r_bank) {
				if (root->get_root_dir().is_empty()) {
					return false;
				}
				const String file = opennova::to_gd(p_name).get_file();
				if (file.is_empty()) {
					return false;
				}
				const PackedByteArray bytes = root->read_file(file);
				if (bytes.is_empty()) {
					return false;
				}
				std::string error;
				return opennova::lwf::parse_lwf_buffer(bytes.ptr(),
						static_cast<size_t>(bytes.size()), r_bank, error);
			},
			&r_bank_key);
}

bool MenuAudio::play_member_sound_(const String &p_wav_path, int p_vol255,
		double p_pitch_scale) {
	if (p_wav_path.is_empty()) {
		return false;
	}
	// LWF paths are Windows-style (e.g. "SFX\\MENU\\MSOVR_2.wav"); the
	// resource root resolves the loose .wav by basename.
	const String name = p_wav_path.replace("\\", "/").get_file();
	if (name.is_empty()) {
		return false;
	}
	const PackedByteArray bytes = resource_root_->read_file(name);
	opennova::lwf::WavPcm pcm;
	std::string error;
	if (bytes.is_empty() ||
			!opennova::lwf::wav_decode_pcm16(bytes.ptr(), static_cast<size_t>(bytes.size()), pcm, error)) {
		return false;
	}
	const opennova::audio::MixWaveRef wave = opennova::audio::mix_wave_from_pcm(pcm);
	if (!wave) {
		return false;
	}
	AudioOut::attach(this);
	// A layer's voice on the channel the allocator picks (audio::Mixer::find_and_open), at the
	// menu emitter's pan 0 and its play word, the truncated Q16 word menu_effective_pitch scales
	// (menu/menu_sound.h carries the witness), which the scale holds exactly.
	const uint32_t play = static_cast<uint32_t>(std::llround(p_pitch_scale * 65536.0));
	return AudioOut::mixer().find_and_open(wave, 0, static_cast<uint8_t>(p_vol255), play, 0) != 0;
}

void MenuAudio::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"),
			&MenuAudio::set_resource_root);
	ClassDB::bind_method(D_METHOD("play_widget_sound", "trigger", "file"),
			&MenuAudio::play_widget_sound);
}
