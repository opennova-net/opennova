#pragma once

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/string.hpp>

#include <audio/sound_selector.h>

namespace godot {

class LwfData;
class ResourceRoot;

// The menu UI sound device leg: resolves each <SOUND> element's .lwf bank
// through the VFS (cached per menu-audio instance) and plays the trigger's
// set through a pooled AudioStreamPlayer per original mixer channel. The
// witnessed selection/volume/pitch decisions live engine-side in
// menu/menu_sound.h + audio/sound_selector.h; this node keeps only bank/wav
// loading and the players.
class MenuAudio : public Node {
	GDCLASS(MenuAudio, Node)

public:
	void set_resource_root(const Ref<ResourceRoot> &p_root);
	// Authoring fallback bank for file-less or unresolved <SOUND> elements
	// (the original fails the element parse / stays silent).
	void set_sound_profile(const Ref<LwfData> &p_profile);
	void set_master_volume(int p_volume);
	int get_master_volume() const;

	// Play `trigger` from the .lwf bank named by `file`. Returns true when at
	// least one member played.
	bool play_widget_sound(const String &p_trigger, const String &p_file);

protected:
	static void _bind_methods();

private:
	struct LwfBankEntry {
		Ref<LwfData> bank;
		int id = 0;
	};

	Ref<LwfData> resolve_sound_bank_(const String &p_file, int &r_bank_id);
	bool play_lwf_set_(const Ref<LwfData> &p_bank, int p_bank_id,
			const String &p_trigger);
	bool play_member_sound_(const Dictionary &p_member, int p_vol255,
			double p_pitch_scale);
	void ensure_sound_pool_();

	Ref<ResourceRoot> resource_root_;
	Ref<LwfData> sound_profile_;
	HashMap<String, LwfBankEntry> lwf_banks_;
	int next_lwf_bank_id_ = 1;
	int master_volume_ = -1; // set from the engine default in the ctor
	opennova::audio::SoundSelector sound_selector_;
	Vector<AudioStreamPlayer *> sound_players_;
	int next_sound_player_ = 0;

public:
	MenuAudio();
};

} // namespace godot
