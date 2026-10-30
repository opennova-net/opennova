#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <formats/lwf/lwf.h>
#include <runtime/audio/mixer.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/audio/sound_listener.h>
#include <runtime/audio/sound_options.h>
#include <runtime/audio/sound_selector.h>
#include <runtime/world/script_voice.h>

namespace godot {

class LwfData;
class ResourceRoot;
class Simulation;

// The loaded .lwf banks' device leg (ADR 0043 d9; ADR 0077): the name index over the banks
// (the engine's SoundSetIndex), the member selection state (SoundSelector), the waves decoded
// once into the mixer's buffers (runtime/audio/mixer.h MixWave) and the one-shot fires, which
// the engine plans (oneshot_play: the set-range cull, the occlusion inflate and recheck, the
// per-layer pick and the fire-time volume) and this opens on the mixer's one-shot channels at the
// pan and play factor the sound calls give them. See docs/audio/lwf-dbf-sound-re.md.
class SoundBank : public RefCounted {
	GDCLASS(SoundBank, RefCounted)

public:
	enum { VOLUME_BYTE_MAX = opennova::audio::kVolumeByteMax };

	// `resource_root` null = menu/isolated-bank use (no VFS reads).
	static Ref<SoundBank> create(const Ref<ResourceRoot> &p_resource_root);

	// Occlusion provider (the Simulation, or null): one-shot fire distances
	// inflate through the witnessed two-ray LOS so occluded sources fire quieter /
	// cull farther [orig: Sound_ApplyOcclusionDistance @ 0x529970, applied in
	// Sound_Play3DPositional @ 0x527d95]. Null (tests/menu) fires unoccluded.
	void set_occlusion_provider(const Ref<Simulation> &p_provider);
	// Test-injection seam: Callable(listener, source, dist_q16, source_bms_id) -> int,
	// consulted only when no Simulation provider is set.
	void set_occlusion_override(const Callable &p_override);

	// Index a loaded LwfData bank. Sets already indexed under a name win (banks
	// added first take precedence), matching "load mission bank, then global".
	void add_bank(const Ref<LwfData> &p_lwf);
	bool has_set(const String &p_name) const;
	PackedStringArray get_set_names() const;
	// The loaded-chain set index the dialog resolution filters against (runtime/audio/dialog_queue.h).
	const opennova::audio::SoundSetIndex &set_index() const { return index_; }

	// A set's layers as the emitter mix takes them as candidates (the engine's
	// ambient_layer_rows: member 0 of each layer whose wave the root holds); empty for a
	// set the chain lacks. No wave is read here: a layer decodes only once a channel
	// takes it.
	std::vector<opennova::audio::AmbientLayerRow> ambient_layer_rows(const std::string &p_name) const;
	// A wave as the mixer plays it (its basename through the VFS, decoded once as the game's
	// loader decodes it, a miss cached): null for none.
	opennova::audio::MixWaveRef resolve_wave(const String &p_wav_path);

	// Where a 3D fire is heard: the sound layer's listener and whether it rides the source.
	struct Hearing {
		const opennova::audio::SoundListener *listener = nullptr;
		bool ridden = false;
	};
	// A one-shot of the named set at `world_pos` (the presentation frame) heard from
	// `listener_pos`, the engine's oneshot_play plan at the sound-effects option's emitter
	// volume, (255 * option) >> 8 (runtime/audio/sound_options.h), its voices opened at the
	// listener's bearing pan (audio::open_oneshot_plan, SoundListener::pan_of). A listener at
	// INF plays distance-flat (the menu, tests). `p_sound_id` is the own-channel reuse key
	// (audio::oneshot_sound_id).
	bool play_oneshot_3d(const Vector3 &p_world_pos, const String &p_name, const Vector3 &p_listener_pos,
			const Hearing &p_hearing, int p_source_bms_id = 0, int p_sound_id = 0);
	// A direct trigger-set play at a distance and bearing from the listener, at the option's
	// emitter volume, no cull or occlusion (audio::plan_oneshot_at_distance).
	bool play_oneshot_at_distance(const String &p_name, int64_t p_dist_q16, uint8_t p_bearing);
	// The interface play: distance 0, pan 0, the option's emitter volume (runtime/audio/sound_options.h).
	bool play_interface_oneshot(const String &p_name);
	// The radio line's member file, or (bank_member) the entity voice's bank member with its
	// pitch jitter (engine audio select_entity_voice).
	std::optional<opennova::world::ScriptVoiceChannel::SetSelection>
			select_radio_set(const std::string &name, uint8_t listener_view_flags,
					bool p_bank_member = false);

protected:
	static void _bind_methods();

private:
	uint8_t listener_view_flags() const;
	struct FireContext {
		Simulation *sim = nullptr;
		const Callable *override = nullptr;
	};
	static int64_t occlusion_trampoline(void *p_ctx, const float p_listener[3],
			const float p_source[3], int64_t p_dist_q16, int64_t p_source_id);

	opennova::audio::SetLocation _find_set(const String &p_name) const;
	// A set the game asked to play that no loaded bank holds, while some bank is loaded (with none, every
	// set misses and the mission's own line says so): the log line the editor's Play reads back into a
	// Problems row on what names the set (ResourceRoot::report_missing, ADR 0046 DI-27).
	void _note_missing_set(const String &p_name) const;
	const opennova::lwf::File &_bank_at(const opennova::audio::SetLocation &p_loc) const;
	static String _member_wav_path(const opennova::lwf::File &p_bank,
			const opennova::lwf::Sndparm &p_member);
	// The plan's voices opened on the one-shot channels at `p_pan` (a positional layer's) or 0.
	bool _play_plan(const opennova::lwf::File &p_bank, const opennova::audio::OneshotPlan &p_plan,
			uint8_t p_pan);

	Ref<ResourceRoot> resource_root_; // null = menu/isolated-bank use (no VFS reads)
	ObjectID occlusion_provider_id_;
	Callable occlusion_override_;
	// The loaded banks as the engine's structs, in load order (the index's bank numbers).
	std::vector<opennova::lwf::File> banks_;
	opennova::audio::SoundSetIndex index_;
	// The portable member-selection state machine; holds the per-(bank,set,layer) state.
	opennova::audio::SoundSelector selector_;
	// wav basename (lower) -> buffer (null if it failed to resolve or decode)
	std::unordered_map<std::string, opennova::audio::MixWaveRef> waves_;
};

} // namespace godot
