// The loaded-bank sound-set index and the 3D one-shot fire decision, pushed
// down from the Godot sound bank (godot/src/audio/sound_bank) so the
// engine core owns the law and the shell only decodes
// waves and spawns voices.
//
// Resolution is NAME-keyed and case-insensitive, matching the engine
// (SoundBank_FindTriggerByName @ 0x75be90 stricmp's set names;
// SoundBank_FindSetByNameAnyBank @ 0x5274f0); the .lwf Multi.target_id is
// never used for NAME resolution -- its runtime meaning is the 3D one-shot
// CULL RANGE in whole units [orig: Sound_Play3DPositional @ 0x527cd1 reads
// set+72] (every JOX set carries one; the field rename is a tracked
// follow-up). See docs/audio/lwf-dbf-sound-re.md.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>

#include <formats/lwf/lwf.h>
#include <runtime/audio/ambient_mixer.h>
#include <runtime/audio/sound_selector.h>

namespace opennova::audio {

// A set's home in the loaded bank chain: the bank's load index and the Multi
// index inside it. bank < 0 = no such set.
struct SetLocation {
	int32_t bank = -1;
	int32_t set = -1;
	// The set's in-memory +72 word (Multi.target_id): the 3D one-shot cull
	// range in whole units, also read by the ammo loader's whiz radius
	// [orig: AmmoDef_InitEffectsTable @0x40a050 / @0x40a072].
	int32_t cull_range = 0;
	bool valid() const { return bank >= 0; }
};

// The name -> set index over every loaded bank. Sets already indexed under a
// name win (banks added first take precedence), matching "load mission bank,
// then global" -- the file header carries the witness.
class SoundSetIndex {
public:
	// Index bank `bank_index`'s sets; a name already indexed keeps its earlier
	// location.
	void add_bank(int32_t bank_index, const lwf::File &bank);
	// The first-bank-wins location of `name` (case-insensitive), or an invalid
	// location when no loaded bank carries it.
	SetLocation find(const std::string &name) const;
	bool has(const std::string &name) const { return find(name).valid(); }
	// Every indexed name, lowercased, in first-seen order.
	const std::vector<std::string> &names() const { return names_; }
	void clear();

private:
	std::unordered_map<std::string, SetLocation> index_;
	std::vector<std::string> names_;
};

// The set's 3D one-shot cull range as Q16 (Multi dword 18, in-memory set+72;
// the file header carries the witness).
int64_t oneshot_cull_range_q16(const lwf::Multi &set);

// The playlists (layers) of a set and the sndparms (members) of a layer that
// the file's tables actually carry: an out-of-range index is skipped the way
// the format reader's tree drops it, so layer/member ordinals agree with that
// tree and with the selector key.
std::vector<uint32_t> set_layers(const lwf::File &bank, const lwf::Multi &set);
std::vector<uint32_t> layer_members(const lwf::File &bank, const lwf::Playlist &layer);

// Pick the member to play for one layer. The selection STATE MACHINE (mode +
// per-layer cursor/bag) is the SoundSelector's; the mode comes from the
// playlist flags (selection_mode_for_flags) and the state lives under the
// (bank, set, layer) key. Returns the ordinal into layer_members(), or -1 for
// an empty layer.
int32_t pick_layer_member(const lwf::File &bank, const SetLocation &loc,
		int32_t layer_index, uint32_t playlist_index, SoundSelector &selector);

// The first resolved member of a radio set, selected only after channel zero
// is found idle. [orig: Audio_StartAmbientSoundForPlayer @0x4ECE30]
struct RadioVoice {
    std::string filename;
    int32_t volume = 255;
    int32_t max_distance = 0;
};
std::optional<RadioVoice> select_radio_voice(const lwf::File &bank,
        const SetLocation &loc, SoundSelector &selector, uint8_t listener_view_flags);

// Q16 listener distance the way the shell measured it: the float length of
// the offset, scaled by the 16.16 one and truncated.
int64_t listener_distance_q16(const float world_pos[3], const float listener_pos[3]);

// One layer's voice of a planned fire: the layer ordinal, its playlist, the
// picked member's sndparm and the fire-time volume byte.
struct OneshotVoice {
	int32_t layer = 0;
	uint32_t playlist = 0;
	uint32_t sndparm = 0;
	int32_t vol255 = 255;
	uint32_t pitch_q16 = 0x10000u;
};

// Retail has 26 physical channels. The generic one-shot allocator scans
// channels 12..25; the first twelve belong to voice/music/ambient owners.
// [orig: Audio_FindAndOpenChannel @0x766E80; AudioChannel_Open @0x7668F0]
inline constexpr size_t kAudioChannelCount = 26;
inline constexpr size_t kFirstOneshotChannel = 12;
class OneshotChannelPool {
public:
	// First strictly lower score wins; ties retain slot order. Incoming gain
	// must exceed half the selected channel's six-byte gain sum.
	int acquire(uint64_t wave, uint8_t volume, uint32_t sound_id = 0);
	void release(size_t channel);
private:
	struct Channel { uint64_t wave = 0; uint32_t sound_id = 0; uint8_t volume = 0; };
	std::array<Channel, kAudioChannelCount> channels_{};
};

// The one-shot's own-channel reuse key. Retail hands the SOURCE ENTITY pointer
// to the open call as its sound_id, and the allocator scores a channel already
// playing the same wave for the same id at ZERO, so an entity re-firing a wave
// restarts its own voice instead of stealing the quietest other one
// [orig: Audio_FindAndOpenChannel @0x766F46 / @0x766F8E (score 0 on the
// (wave, sound_id) match; the 12*vol floor @0x766F0A never beats it);
// Sound_Play3DPositional @0x527E4A stores the entity, SoundBank_PlayTriggerEntries
// @0x75CE4F carries it into the open call (push @0x75CFD4, call @0x75CFE3),
// Entity_PlaySound3D_FullVolume @0x528E31 forwards it]. Our entities have no pointer identity: the packed
// handle is tagged into a nonzero word (pool 0 slot 0 packs to 0) and a
// handle-less caller falls back to its tagged BMS id. 0 = no identity, the
// retail NULL of the interface, weather and delayed-slot plays
// [orig: Sound_PlayPositional @0x528E02; Sound_PlayTriggerSetScaled @0x527BB4;
// Sound_PlayInterfaceTriggerSet @0x527C04].
inline constexpr uint16_t kNoSourceHandle = 0xFFFFu; // world::EntityHandle::kInvalid
inline uint32_t oneshot_sound_id(uint16_t packed_handle, int32_t bms_id) {
	if (packed_handle != kNoSourceHandle) return 0x10000u | packed_handle;
	if (bms_id != 0) return 0x80000000u | static_cast<uint32_t>(bms_id);
	return 0;
}

struct OneshotPlan {
	// The set passed the range cull (and the post-occlusion recheck).
	bool in_range = false;
	// The fire distance the volumes snapshot (occlusion-inflated).
	int64_t dist_q16 = 0;
	// The own-channel reuse key the allocator scores by (oneshot_sound_id);
	// 0 on the id-less plans.
	uint32_t sound_id = 0;
	std::vector<OneshotVoice> voices;
};

// Plan a direct trigger-set fire at an explicit listener distance. This path
// has no set-range cull or occlusion query; layer attenuation still applies.
// [orig: Sound_PlayTriggerSetScaled @0x527B90 -> SoundBank_PlayTriggerEntries
// @0x75CCD0]. The optional flat arm preserves menu playback without a listener.
OneshotPlan plan_oneshot_at_distance(const lwf::File &bank, const SetLocation &loc,
        int64_t dist_q16, SoundSelector &selector, uint8_t listener_view_flags,
        bool attenuate = true);

// Plan a one-shot fire of the set at `loc` at a world position (PlayWavList /
// event actions). Volume is computed ONCE at fire time from the witnessed
// distance model when the listener is known [orig: Sound_Play3DPositional
// @ 0x527cb0 -> SoundBank_PlayTriggerEntries @ 0x75ccd0 compute vol/pan at
// play, no per-frame update]; `has_listener` false plans distance-flat (menu /
// tests). Every view-admitted layer picks its member (advancing the selector) even when the
// shell later fails to resolve its wave, so the pick stream matches a full
// fire. Residual (D-SND-19, documented, not ported): retail performs the two
// ROL3 pitch draws (set jitter, member jitter) only when the picked member's
// wave handle is non-null [orig: SoundBank_PlayTriggerEntries @0x75CE1A (the
// `if (*sample_entry)` guard); the draws @0x75CE81 / @0x75CEBA]; the planner
// always draws them, because the wave is resolved later by the shell. Shipped
// banks resolve every wave, so the RNG stream matches on real data.
// `occl` may be null (an unoccluded fire). `sound_id` is the own-channel
// reuse key (oneshot_sound_id) the plan carries to the allocator.
OneshotPlan plan_oneshot_3d(const lwf::File &bank, const SetLocation &loc,
		const float world_pos[3], const float listener_pos[3], bool has_listener,
		int64_t source_bms_id, uint32_t sound_id, OcclusionFn occl, void *occl_ctx,
		SoundSelector &selector, uint8_t listener_view_flags);

} // namespace opennova::audio
