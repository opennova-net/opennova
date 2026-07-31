// Portable SndProf.def sound-profile store.
//
// The engine's per-item sound table: a named profile carrying 51 sound-set
// slots (name + three numeric params each) plus 12 med/crs loop fade/pitch
// percent params. Profiles load once at boot from SndProf.def and every item
// definition binds one (or two — the female variant) by name; the runtime
// plays "slot N of this entity's profile" for footsteps, death screams,
// landing thumps, cloth foley, engine loops, impacts, chute and tumble
// sounds. Witnessed 2026-07-17 (docs/audio/lwf-dbf-sound-re.md §SndProf):
// [orig: SoundProfile_LoadAll @ 0x527490 (128 x 2152-byte slots, grow-on-
// demand), sound_profile_xml_callback @ 0x526fc0 (the begin/end line parser;
// the 51-entry name->slot table @ 0x82F3B0 runs slot 0..50 in order),
// SoundProfile_AllocSlot @ 0x526f80, SoundProfile_FindSlotByName @ 0x526e30
// (first stricmp match; miss returns the FIRST profile)].
//
// Slot resolution to playable sound-set ids happens at mission start against
// the loaded .lwf banks [orig: resolve_sound_profile_triggers @ 0x528210 ->
// SoundBank_FindTriggerByName]. The embedder resolves by NAME at play time
// (NovaSoundBank is name-keyed), so this store keeps the authored set names
// and the world emits them directly; an empty slot name is the id-0 no-op.
#ifndef OPENNOVA_AUDIO_SOUND_PROFILE_H
#define OPENNOVA_AUDIO_SOUND_PROFILE_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::audio {

// The 51 profile slots, in the engine's table order (index == slot)
// [orig: the name/index pair table @ 0x82F3B0-0x82F54C; names are the
// SndProf.def param keywords]. The infantry consumers:
//   7/8   death scream, night variant gated on the mission NVG attribute
//   15/16 landing while dead / alive
//   17-23 footsteps L/R by surface (ground, snow=surface-3, on-entity, water)
//   24-29 the .bad anim-event foley sounds (trigger bits 0x20..0x400)
// 30..50 are the vehicle family (engine loops, impacts, chute, tumble),
// consumed by the vehicle physics slice.
enum SoundProfileSlot {
    kSlotSoundLoop1 = 0, // Soundloop_1..7
    kSlotDeath = 7,      // sounddeath
    kSlotNightDeath = 8, // SSNightDead
    kSlotDoorOpen = 9,
    kSlotDoorClose = 10,
    kSlotShotDawn = 11, // dawnshot/dayshot/duskshot/nightshot
    kSlotShotDay = 12,
    kSlotShotDusk = 13,
    kSlotShotNight = 14,
    kSlotFallDead = 15,  // SSFallDead
    kSlotFallAlive = 16, // SSFallAlive
    kSlotFootLGround = 17,
    kSlotFootRGround = 18,
    kSlotFootLSnow = 19,
    kSlotFootRSnow = 20,
    kSlotFootLObject = 21,
    kSlotFootRObject = 22,
    kSlotFootWater = 23, // one slot for both feet
    kSlotAudio1 = 24,    // SSAudio1..6
    kSlotAudio6 = 29,
    kSlotEngineStart = 30,
    kSlotEngineStop = 31,
    kSlotEngineReverse = 32,
    kSlotEngineHighRev = 33,
    kSlotWarning = 34,
    kSlotImpact = 35,
    kSlotLand = 36,
    kSlotRollover = 37,
    kSlotImpactOrganic = 38,
    kSlotImpactWater = 39,
    kSlotRotorImpact = 40,
    kSlotChuteOpen = 41,
    kSlotChuteClose = 42,
    kSlotChuteFlap = 43,
    kSlotFreeFall = 44,
    kSlotDriveRepeat = 45,
    kSlotSwivelShift = 46,
    kSlotTumbleHitHard = 47,
    kSlotTumbleHitMed = 48,
    kSlotTumbleHitSoft = 49,
    kSlotTumbleSkid = 50,
    kSoundProfileSlotCount = 51,
};

// The engine's slot keyword for a slot index (the @ 0x82F3B0 table strings,
// e.g. 17 -> "SSLFootGND"). nullptr for an out-of-range slot.
const char *sound_profile_slot_keyword(int slot);

struct SoundProfile {
    std::string name; // begin "<name>" (engine buffer: 64 bytes)
    // Per-slot authored sound-set name (engine buffer: 24 bytes each; empty =
    // no sound, the resolved-id-0 no-op).
    std::array<std::string, kSoundProfileSlotCount> set_names{};
    // Param columns 2/3 are floats stored x65536 (16.16), column 4 is atol
    // [orig: the x 65536.0 (dbl_7C3CC0) and j__atol stores @ 0x527122-0x52718b].
    // JO data uses 2/3 as the loop pitch min/max band (e.g. "soundloop_2
    // V_APACHE_ILP .8 1.2").
    std::array<int32_t, kSoundProfileSlotCount> param2_q16{};
    std::array<int32_t, kSoundProfileSlotCount> param3_q16{};
    std::array<int32_t, kSoundProfileSlotCount> param4{};
    // The 12 med/crs loop fade/pitch percents, stored x655 (~Q16 percent)
    // [orig: the dedicated keyword chain @ 0x5270a9-0x527481, dwords 220-231]:
    // medloopfadein start/end, medloopfadeout start/end, medlooppitch
    // start/end, medlooppitch startp/endp, crsloopfadein start/end,
    // crslooppitch startp/endp.
    std::array<int32_t, 12> loop_params{};
};

// The loaded profile list. Load order is preserved; duplicate names keep the
// FIRST entry authoritative (find scans in order).
class SoundProfileTable {
public:
    // Parse SndProf.def text (the whole file). Returns the number of profiles
    // parsed. Repeated calls append, matching the expansion reload path
    // re-running the loader after a reset.
    size_t parse(const char *text, size_t len);
    void clear() { entries_.clear(); }

    // First case-insensitive name match; a miss falls back to the FIRST
    // profile [orig: SoundProfile_FindSlotByName @ 0x526e30 returns the array
    // base when not found]. Null only when no profiles are loaded.
    const SoundProfile *find(const char *name) const;
    // Same lookup as an index (for per-entity bindings): miss -> 0 when any
    // profile is loaded, -1 only on an empty table.
    int index_of(const char *name) const;

    const std::vector<SoundProfile> &entries() const { return entries_; }
    bool empty() const { return entries_.empty(); }

private:
    std::vector<SoundProfile> entries_;
};

} // namespace opennova::audio

#endif // OPENNOVA_AUDIO_SOUND_PROFILE_H
