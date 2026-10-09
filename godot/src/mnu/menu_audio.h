#pragma once

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/audio/sound_selector.h>
#include <runtime/menu/menu_sound.h>

namespace godot {

class ResourceRoot;

// The menu UI sound device leg: opens each <SOUND> element's .lwf bank
// through the VFS into the engine's bank collection (menu/menu_sound.h
// MenuBankCollection, one per menu-audio instance), plans the trigger's play
// there (plan_menu_sound: the set, each layer's member, its volume and pitch)
// and plays the voices through a pooled AudioStreamPlayer per original mixer
// channel. This node keeps only the bank and wave reads and the players.
class MenuAudio : public Node {
	GDCLASS(MenuAudio, Node)

public:
	void set_resource_root(const Ref<ResourceRoot> &p_root);

	// Play `trigger` from the .lwf bank named by `file`, that bank alone: a
	// bank that does not open plays nothing (menu/menu_sound.h carries the
	// witness). Returns true when at least one member played.
	bool play_widget_sound(const String &p_trigger, const String &p_file);

protected:
	static void _bind_methods();

private:
	const opennova::lwf::File *resolve_sound_bank_(const String &p_file, int32_t &r_bank_key);
	bool play_member_sound_(const String &p_wav_path, int p_vol255, double p_pitch_scale);
	void ensure_sound_pool_();

	Ref<ResourceRoot> resource_root_;
	opennova::menu::MenuBankCollection banks_;
	int master_volume_ = -1; // set from the engine default in the ctor
	opennova::audio::SoundSelector sound_selector_;
	Vector<AudioStreamPlayer *> sound_players_;
	int next_sound_player_ = 0;

public:
	MenuAudio();
};

} // namespace godot
