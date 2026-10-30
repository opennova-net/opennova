#pragma once

// The mission's loaded .lwf banks (ADR 0077 R4; the Godot binding's SoundBank until then): the name
// index over the banks in load order (SoundSetIndex: a set is searched for in the global chain's
// slots alone [orig: SoundBank_FindSetByNameAnyBank @ 0x5274f0]), the member selection state
// (SoundSelector), each wave decoded once into the mixer's buffers through the mounted files as the
// game's loader decodes it (formats/lwf wav_pcm), and the one-shot plays: the engine plans them
// (oneshot_play: the set-range cull, the occlusion inflate and recheck, the per-layer pick and the
// fire-time volume) and they open on the mixer's one-shot channels at the pan the sound calls give
// them. Witness record: docs/audio/lwf-dbf-sound-re.md.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <formats/lwf/lwf.h>
#include <runtime/audio/ambient_mixer.h> // OcclusionFn
#include <runtime/audio/mixer.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/audio/sound_selector.h>

namespace opennova::audio {

class SoundListener;

// What the sounds read of the mounted root: a file's presence and its bytes by its flat name (the
// mission kernel's mission::BootFileSource readers, or a tool's).
struct SoundFiles {
	std::function<bool(const std::string &name)> has_file;
	std::function<bool(const std::string &name, std::vector<uint8_t> &out)> read_file;
	bool valid() const { return has_file != nullptr && read_file != nullptr; }
};

class SoundBanks {
public:
	// The mounted files the banks and their waves read through (an empty source: none, an isolated
	// bank whose waves never resolve).
	void set_files(SoundFiles files) { files_ = std::move(files); }
	// The banks, their index, the selection state and the decoded waves forgotten.
	void clear();

	// A bank of the chain by its file name, read and indexed when the files hold it and it parses
	// (a missing file skips, as SoundBank_LoadIfExists does). True when it loaded.
	bool load(const std::string &name);
	// A parsed bank indexed behind the loaded ones: a set already indexed under its name keeps
	// its earlier bank, so the banks loaded first take precedence.
	void add(lwf::File bank);
	size_t bank_count() const { return banks_.size(); }

	bool has_set(const std::string &name) const { return index_.has(name); }
	// The loaded-chain set index (the dialog resolution filters against it).
	const SoundSetIndex &index() const { return index_; }

	// A set's layers as the emitter mix takes them as candidates (ambient_layer_rows: member 0 of
	// each layer whose wave the files hold); empty for a set the chain lacks. No wave is read here:
	// a layer decodes only once a channel takes it.
	std::vector<AmbientLayerRow> ambient_layer_rows(const std::string &name) const;
	// A wave as the mixer plays it (its basename through the files, decoded once, a miss cached):
	// null for none.
	MixWaveRef resolve_wave(const std::string &path);

	// Where a 3D one-shot is heard from: the listener's place in the presentation frame (null:
	// distance-flat, the menu's and a tool's), the sound layer's listener its bearing pan reads
	// (null: pan 0) and whether that listener rides the source (the pan centred).
	struct Hearing {
		const float *listener_place = nullptr;
		const SoundListener *listener = nullptr;
		bool ridden = false;
		OcclusionFn occlusion = nullptr;
		void *occlusion_ctx = nullptr;
	};
	// A one-shot of the named set at `world_pos` (the presentation frame), the engine's
	// plan_oneshot_3d at the sound-effects option's emitter volume, (255 * option) >> 8
	// (sound_options.h), its voices opened at the listener's bearing pan (open_oneshot_plan,
	// SoundListener::pan_of). `sound_id` is the own-channel reuse key (oneshot_sound_id).
	bool play_3d(Mixer &mixer, const std::string &name, const float world_pos[3], const Hearing &hearing,
			int32_t source_bms_id, uint32_t sound_id, uint8_t listener_view_flags, int32_t sound_fx_option);
	// A direct trigger-set play at a distance and bearing from the listener, at the option's
	// emitter volume, no cull or occlusion (plan_oneshot_at_distance).
	bool play_at_distance(Mixer &mixer, const std::string &name, int64_t dist_q16, uint8_t bearing,
			uint8_t listener_view_flags, int32_t sound_fx_option);
	// The interface play: distance 0, pan 0, the option's emitter volume.
	bool play_interface(Mixer &mixer, const std::string &name, uint8_t listener_view_flags,
			int32_t sound_fx_option);
	// The radio line's member file, or (`bank_member`) the entity voice's bank member with its
	// pitch jitter (select_radio_voice / select_entity_voice).
	std::optional<RadioVoice> select_voice(const std::string &name, uint8_t listener_view_flags,
			bool bank_member);

private:
	// A set the game asked to play that no loaded bank holds, while some bank is loaded (with none,
	// every set misses and the mission's own line says so): the resource-missing line.
	void note_missing_set(const std::string &name) const;
	const lwf::File &bank_at(const SetLocation &loc) const;
	static std::string member_wave_path(const lwf::File &bank, const lwf::Sndparm &member);
	bool play_plan(Mixer &mixer, const lwf::File &bank, const OneshotPlan &plan, uint8_t pan);

	SoundFiles files_;
	// The loaded banks in load order (the index's bank numbers).
	std::vector<lwf::File> banks_;
	SoundSetIndex index_;
	SoundSelector selector_;
	// wave basename (lowercase) -> buffer (null when it did not resolve or decode)
	std::unordered_map<std::string, MixWaveRef> waves_;
};

// The file a path names, its directory dropped (the files are one flat namespace, as the game's
// lookups reach them).
std::string sound_file_name(const std::string &path);

} // namespace opennova::audio
