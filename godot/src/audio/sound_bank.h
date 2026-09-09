#pragma once

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include <formats/lwf/lwf.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/audio/sound_selector.h>
#include <runtime/audio/volume_law.h>

#include "audio/ambient_layer.h"

namespace godot {

class LwfData;
class ResourceRoot;
class Simulation;

// Runtime playback over one or more loaded .lwf banks (the former
// sound_bank.gd, ADR 0043 d9). Indexes sound sets by name (case-insensitive
// across all banks -- the engine's SoundSetIndex), decodes member .wav bytes
// via WavLoader (cached), and spawns AudioStreamPlayer3D voices. The
// member-selection state machine (engine-faithful: sequential / random-anchor
// cycle / random default, one shared ROL-LCG stream) lives in portable C++
// (engine/runtime/audio, via SoundSelector [orig: SoundBank_PlayTriggerEntries @
// 0x75ccd0]); the one-shot fire decision (the set-range cull, the occlusion
// inflate and recheck, the per-layer member pick and the fire-time volume) is
// the engine's oneshot_play plan over the banks read as the engine's
// formats/lwf structs (LwfData::engine_file). This is the device half: the
// wave cache, the bus guards, the looping stream copies and the spawned
// players. See docs/audio/lwf-dbf-sound-re.md.
class SoundBank : public RefCounted {
	GDCLASS(SoundBank, RefCounted)

public:
	// The engine volume byte ceiling and its dB law (engine/runtime/audio/volume_law.h).
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
	// C++-side seam (not bound): the loaded-chain set index the dialog
	// resolution filters against (runtime/audio/dialog_queue.h).
	const opennova::audio::SoundSetIndex &set_index() const { return index_; }

	// Return lightweight layer descriptors for the mission ambient mixer. No WAV
	// is read or decoded here: placed emitters remain data until one of the eight
	// physical channels actually needs the layer. The emitter path always uses
	// member 0 of each layer [orig: SoundEmitter_UpdateAndMixTop8 @ 0x528649].
	TypedArray<AmbientLayer> describe_ambient(const String &p_name);
	// Resolve a descriptor returned by describe_ambient(). Description and decode
	// are separate so hundreds of candidates can be ranked while only selected
	// channel entrants cause VFS reads and WAV decoding.
	Ref<AudioStreamWAV> resolve_ambient_stream(const Ref<AmbientLayer> &p_layer);
	// Bind a resolved stream to a reusable physical ambient channel. Playback is
	// owned by MissionAudio: incumbents continue while new bindings restart.
	static void configure_ambient_player(AudioStreamPlayer3D *p_player,
			const Ref<AudioStreamWAV> &p_stream, const Ref<AmbientLayer> &p_layer,
			const StringName &p_bus);

	// Spawn the looping ambient voices for the named sound set at `world_pos`,
	// parented under `parent`. One AudioStreamPlayer3D per layer, playing the
	// layer's FIRST member -- the emitter path does not run the selection machine
	// [orig: SoundEmitter_UpdateAndMixTop8 @ 0x528649 reads layer+16 = member 0].
	// Voices spawn SILENT and paused; the caller's mix tick (MissionAudio)
	// owns audibility via the witnessed distance model over its own candidate
	// records. Returns the holder Node3D, or null if the set is unknown or no
	// member resolves to audio.
	Node3D *spawn_ambient(Node3D *p_parent, const Vector3 &p_world_pos, const String &p_name,
			const StringName &p_bus);
	// Fire a one-shot voice for the named set at a world position: the engine's
	// oneshot_play plan (cull, occlusion, per-layer pick and fire-time volume)
	// over `listener_pos`, then one auto-freeing player per planned voice whose
	// wave resolves. Pass Vector3.INF to play distance-flat (menu / tests).
	// Returns true if anything played.
	bool play_oneshot_3d(Node3D *p_parent, const Vector3 &p_world_pos, const String &p_name,
			const StringName &p_bus, const Vector3 &p_listener_pos = Vector3(INFINITY, INFINITY, INFINITY),
			int p_source_bms_id = 0, const String &p_exclusive_key = String());
    // Direct WAC/weather trigger: explicit distance controls layer gain; the
    // position only supplies Godot's panner (D-SND-8). No 3D cull or occlusion.
    bool play_oneshot_at_distance(Node3D *p_parent, const Vector3 &p_pan_position,
            const String &p_name, const StringName &p_bus, int64_t p_dist_q16);
    // Stop this bank's one-shot children on mission retry (the retail reset
    // stops channels and frees sample buffers, never a loaded bank). Ambient
    // and dialog channels retain their own owners; the per-layer selection
    // state is untouched because the banks stay loaded across a round restart.
    void reset_oneshots(Node3D *p_parent);
	// Spawn a one-shot, NON-positional voice for the named set (mission dialog/voice
	// is centered and full-volume, not 3D-attenuated) and RETURN its
	// AudioStreamPlayer (the first resolvable layer's voice) without auto-freeing it
	// -- the caller owns its lifetime and listens for `finished`. Used by the
	// serialized dialog queue (the engine plays one dialog audio channel at a time:
	// Dialog_UpdatePlayback @ 0x44e470 only advances when the active channel frees).
	// Returns null if the set is unknown or no member resolves to audio.
	AudioStreamPlayer *spawn_oneshot_2d(Node *p_parent, const String &p_name, const StringName &p_bus);

	// An LWF member's / layer descriptor's base pitch as the player plays it: an
	// unauthored or degenerate value (<= 0.01) plays at unity.
	static double effective_base_pitch(double p_base_pitch);
	// dB for a 0..255 engine channel volume; 0 -> hard silent (the engine's law).
	static double volume_db_from_255(int p_vol255);
	// The witnessed distance volume curve [orig: SoundBank_CalcDistanceVolPan
	// @ 0x75ca20]: at or beyond `radius` the voice is HARD SILENT; inside it the
	// volume runs vol * (1 - d/r)^2, ceilinged by `clamp_vol`. The master-fade
	// global (g_SoundMasterFadeQ24, steady state 0xFF0000) folds in as
	// (vol * 255) >> 8; the underwater halving flag and the pan half of the packed
	// return are reimpl territory (bus volume / Godot's panner). Distances are Q16.16
	// like the original's; both arms of the engine pass a consistent scale so only
	// the ratio matters. The integer form lives in engine/runtime/audio
	// (ambient_mixer.cpp) -- one implementation for the native mix, the one-shot
	// path, and the GUT pins.
	static int calc_distance_volume(int64_t p_dist_q16, int64_t p_radius_q16, int p_vol255,
			int p_clamp_vol);
	// Layer volume for the looping ambient-emitter path. `dist_q16` is the
	// listener distance in Q16.16 units -- the arms subtract in Q16 FIRST and
	// truncate to whole units at the curve call, exactly like the original
	// (HIWORD(dist - min) is floor(d - m), NOT min - floor(d): the proximity arm
	// differs by a unit for fractional d) [orig: SoundEmitter_UpdateAndMixTop8
	// @ 0x528667..0x5286df]. `vol_byte` is the emitter volume 0..255 (the
	// time-of-day crossfade blend for placed markers); member volume and clamp
	// scale by it before the curve [orig: @ 0x5286b9]. With a min_distance the
	// falloff REBASES to run min..falloff; inside min_distance the volume RISES
	// as (d/min)^2 (the proximity fade); a bare falloff runs 0..falloff. The arm
	// forms live in engine/runtime/audio (ambient_mixer.cpp) beside the mix that
	// consumes them natively; this seam stays for the pins.
	static int emitter_layer_volume(int64_t p_dist_q16, int p_falloff_u, int p_min_u,
			int p_vol_byte, int p_member_vol, int p_clamp_vol);
	// One-shot volume at fire time -- the native two-stage composition
	// (engine/runtime/audio oneshot_layer_volume [orig: SoundBank_PlayTriggerEntries
	// @ 0x75cf14..0x75cf8b]: proximity feeds falloff un-rebased; a layer with no
	// falloff radius plays at the raw emitter volume). Reimpl emitter volume is
	// full (255): the engine's fire-time (vol * g_SoundVolumeOption) >> 8 folds the
	// options slider we map to bus volume (docs/audio/lwf-dbf-sound-re.md D-SND-8).
	int oneshot_distance_volume(int64_t p_dist_q16, const Ref<AmbientLayer> &p_layer) const;

protected:
	static void _bind_methods();

private:
	// The per-fire occlusion seam the engine plan calls: the Simulation when one
	// is set, else the test override, resolved fresh per fire.
	struct FireContext {
		Simulation *sim = nullptr;
		const Callable *override = nullptr;
	};
	static int64_t occlusion_trampoline(void *p_ctx, const float p_listener[3],
			const float p_source[3], int64_t p_dist_q16, int64_t p_source_id);

	opennova::audio::SetLocation _find_set(const String &p_name) const;
	const opennova::lwf::File &_bank_at(const opennova::audio::SetLocation &p_loc) const;
	static String _member_wav_path(const opennova::lwf::File &p_bank,
			const opennova::lwf::Sndparm &p_member);
	static double _member_base_pitch(const opennova::lwf::Sndparm &p_member);
	AudioStreamPlayer3D *_make_player(const Ref<AudioStreamWAV> &p_stream, double p_base_pitch,
			const StringName &p_bus, bool p_loop, int p_vol255);
    bool _play_oneshot_plan(Node3D *p_parent, const Vector3 &p_world_pos,
            const opennova::lwf::File &p_bank, const opennova::audio::OneshotPlan &p_plan,
            const StringName &p_bus, const String &p_exclusive_key = String());
	Ref<AudioStreamWAV> _resolve_stream(const String &p_wav_path);
	static int64_t _stream_frames(const Ref<AudioStreamWAV> &p_stream);

	Ref<ResourceRoot> resource_root_; // null = menu/isolated-bank use (no VFS reads)
	ObjectID occlusion_provider_id_;
	Callable occlusion_override_;
	// The loaded banks as the engine's structs, in load order (the index's bank
	// numbers).
	std::vector<opennova::lwf::File> banks_;
	opennova::audio::SoundSetIndex index_;
	// The portable member-selection state machine (engine/runtime/audio); holds
	// the per-(bank,set,layer) state.
	opennova::audio::SoundSelector selector_;
	// wav basename(lower) -> AudioStreamWAV (or null if it failed to resolve/decode)
	HashMap<String, Ref<AudioStreamWAV>> wav_cache_;
	// exclusive_key -> the gating voice of an exclusive one-shot (see
	// play_oneshot_3d); entries go stale harmlessly (resolved through ObjectDB
	// before use).
	HashMap<String, ObjectID> exclusive_;
    // Auto-freeing 3D voices owned by this bank. ObjectID protects against
    // parent teardown and slot reuse without storing metadata on the nodes.
    std::vector<ObjectID> oneshots_;
};

} // namespace godot
