// LWF sound-profile container reader/writer (magic 'LWF1').
//
// Reverse-engineered from lwfbuilder.exe / lwf2sdf.exe / SoundTool.exe; ported
// byte-for-byte from the pre-repo prototype's engine/formats/lwf, then grilled
// against the engine reader in Jointops.exe (2026-06-09): every stride/offset
// below is witnessed by SoundBank_OpenFile @ 0x75caa0 + SoundBank_LoadTriggerSets
// @ 0x75c370, and the field semantics by SoundBank_PlayTriggerEntries @ 0x75ccd0.
// The on-disk layout is a pure index: Singles (audio refs, 52 B) -> optional
// trigger records (12 B, read and never interpreted; JO-era banks ship none) ->
// MultiHeader -> Multis (sound sets, 80 B) -> Playlists (layers, 48 B) ->
// Sndparms (members, 28 B) -> string pool (256-byte .wav path slots, one per
// single). No audio is embedded. The engine checks no header field on open
// [orig: SoundBank_OpenFile @ 0x75caa0 refuses only a file that does not open];
// what this reader still refuses that the engine would open is D-SND-3
// (docs/audio/lwf-dbf-sound-re.md).
//
// Round-trip is byte-exact on the *unmodified* parse->encode path: garbage in
// unused/reserved slots and the original string pool are preserved verbatim.
// See docs/audio/lwf-dbf-sound-re.md.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {
namespace lwf {

constexpr uint32_t kMagic = 0x4C574631;  // 'LWF1'

// The one Q16 pitch convention: 0x10000 == 1.0. Member and set pitches compose
// multiplicatively in Q16 — final = (member_pitch * set_pitch) >> 16
// [orig: SoundBank_SelectTriggerEntryFromBank @ 0x75c0be] — so every float
// conversion divides/multiplies by 0x10000, never 0xFFFF (authored set bases
// ship 0xFFFF, i.e. unity minus 1/65536, but the SCALE is still 2^16).
constexpr uint32_t kPitchUnityQ16 = 0x10000u;
// The authoring tools' stock set-level pitch base (~unity; see above).
constexpr uint32_t kAuthoredSetPitchBase = 0xFFFFu;

inline double pitch_from_q16(uint32_t q16) {
  return static_cast<double>(q16) / static_cast<double>(kPitchUnityQ16);
}

inline uint32_t pitch_to_q16(double pitch) {
  if (!(pitch > 0.0)) {
    return 0u;
  }
  return static_cast<uint32_t>(pitch * static_cast<double>(kPitchUnityQ16) + 0.5);
}

struct Header {
  uint32_t header_size = 0;       // expected 28
  // 'LWF1' in every shipped bank, but the engine refuses no other: the magic only
  // gates the 256-byte filename table [orig: SoundBank_LoadTriggerSets @ 0x75c671],
  // so a bank of another magic loads its sets, layers and members and its singles
  // name no file (Single::path empty). Written back as read.
  uint32_t magic = kMagic;
  uint32_t single_count = 0;
  // Count of 12-byte trigger records following the singles table (File::triggers).
  // The engine allocates and reads 12 * trigger_count bytes ("SNDTRIG TRIGGERS")
  // and nothing reads them again [orig: SoundBank_OpenFile @ 0x75cb63 / 0x75cb7a];
  // every known JO-era bank ships 0. The writer derives it from File::triggers.
  uint32_t trigger_count = 0;
  uint32_t multi_header_off = 0;  // offset to MultiHeader
  uint32_t string_pool_off = 0;   // offset to start of string pool
  uint32_t reserved1 = 0;         // for byte-perfect round-trip
};

struct MultiHeader {
  uint32_t header_size = 0;       // expected 20
  uint32_t multi_count = 0;
  uint32_t multi_table_off = 0;   // offset to first MultiEntry
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;
};

struct Single {
  std::string name;
  uint16_t value_hi = 0;          // high byte carries the numeric field
  uint32_t path_offset = 0;       // relative offset into string pool
  std::string path;               // parsed from string pool (256-byte slots); empty in a non-'LWF1' bank
  // Raw bytes for byte-perfect round-trip (includes garbage after null terminator).
  std::array<char, 32> raw_name{};
  uint16_t pad0 = 0;
  std::array<uint32_t, 3> reserved0{};
};

struct Multi {
  std::string name;
  // Set-level pitch in Q16 (0xFFFF ~= 1.0) composed multiplicatively with the
  // member pitch: final = (member_pitch * (pitch_base + jitter)) >> 16
  // [orig: SoundBank_SelectTriggerEntryFromBank @ 0x75c0be].
  uint32_t pitch_base = 0;          // observed 0xFFFF (unity)
  // Random pitch range added on every play: (range * rand8) >> 8
  // [orig: SoundBank_SelectTriggerEntryFromBank @ 0x75c09e].
  uint32_t pitch_random_range = 0;
  std::vector<uint32_t> playlist_indices;  // indices into playlists table
  // The set's 3D one-shot CULL RANGE in whole units: file dword 18 (the
  // authoring tool labels it "target id") lands in the in-memory set+72 and
  // gates every positional fire; NAME resolution never reads it.
  // [orig: SoundBank_LoadTriggerSets @ 0x75c489 -> Sound_Play3DPositional
  //  @ 0x527cd1..0x527da1]
  uint32_t target_id = 0;
  // Raw bytes for byte-perfect round-trip.
  std::array<char, 24> raw_name{};
  std::array<uint32_t, 8> raw_playlist_ids{};  // includes garbage in unused slots
  // Playback gate flags; bit0 = also require the layer's flag bit 0x20 to match
  // the listener view state [orig: SoundBank_PlayTriggerEntries @ 0x75cd54].
  uint32_t set_flags = 0;
};

// Playlist flags (bitmask). Engine member selection [orig:
// SoundBank_PlayTriggerEntries @ 0x75cd5c..0x75cdfc] tests ONLY kFlagSequential
// (0x10) then kFlagRandomSequential (0x80); anything else picks a uniformly
// random member -- kFlagRandom (0x08) is the authoring tool's explicit marker
// for that default and is never tested by the engine.
enum PlaylistFlags : uint32_t {
  kFlagHeading           = 0x0001,  // pan from the emitter heading [orig: @ 0x75ce56]
  kFlagInternal          = 0x0002,  // audible in internal (cockpit) view [orig: @ 0x75cd54]
  kFlagExternal          = 0x0004,  // audible in external view [orig: @ 0x75cd54]
  kFlagRandom            = 0x0008,
  kFlagSequential        = 0x0010,
  kFlagStoppable         = 0x0020,  // also matched against the listener view bit when Multi.set_flags bit0 is set
  kFlagPreload           = 0x0040,  // resolve+mark wave entries at bank load [orig: SoundBank_LoadTriggerSets @ 0x75c5e1]
  kFlagRandomSequential  = 0x0080,  // sign bit in original tool; random anchor then full in-order cycle
  kFlagDirectional       = 0x0200,
  kFlagLooping           = 0x0400,
  kFlagReverb            = 0x0800,
  kFlagRapid             = 0x1000,
};

struct Playlist {
  // Audible falloff radius (tool column "Falloff"): volume runs
  // vol * (1 - d/r)^2 and hits ZERO at this radius; it is also the ambient
  // emitter's cull range [orig: SoundBank_CalcDistanceVolPan @ 0x75ca20
  // (d >= r returns 0); SoundBank_PlayTriggerEntries @ 0x75cf5c;
  // SoundEmitter_UpdateAndMixTop8 @ 0x52856a caches it <<16 as the slot range].
  uint16_t falloff_radius = 0;
  // Proximity fade radius (tool column "Min distance"): inside it volume
  // RISES as (d/r)^2 - the sound fades out as the listener closes on the
  // emitter - and the falloff above is rebased to run from this radius out
  // [orig: SoundBank_PlayTriggerEntries @ 0x75cf1a..0x75cf55;
  // SoundEmitter_UpdateAndMixTop8 @ 0x528667..0x5286b9]. 0 = no proximity fade.
  uint16_t min_distance = 0;
  uint32_t flags = 0;                       // PlaylistFlags bitmask
  std::vector<uint32_t> sndparm_indices;    // indices into sndparm table
  // Raw bytes for byte-perfect round-trip. On disk this dword is scratch; the
  // engine overwrites it at runtime with the random-sequential cycle anchor.
  uint32_t reserved0 = 0;
  std::array<uint32_t, 8> raw_member_offsets{};  // includes garbage in unused slots
};

struct Sndparm {
  uint32_t single_index = 0;                // engine coerces out-of-range to 0 [orig: @ 0x75c5c8]
  uint32_t pitch_scaled = 0;                // member pitch base, Q16 (0x10000 = 1.0)
  uint32_t random_pitch_scaled = 0;         // additive jitter: (range * rand8) >> 8 [orig: @ 0x75cedc]
  uint32_t volume = 0;                      // 0..255 member volume [orig: @ 0x75cf25]
  uint32_t clamp_volume = 0;                // ceiling on the distance-scaled volume + pan amplitude [orig: SoundBank_CalcDistanceVolPan @ 0x75ca65]
  std::array<uint32_t, 2> reserved{};       // trailing dwords; unread by the engine player
};

// One 12-byte trigger record, kept as its three raw dwords: the engine reads the
// table and never interprets it [orig: SoundBank_OpenFile @ 0x75cb7a], so no field
// meaning is witnessed.
using TriggerRecord = std::array<uint32_t, 3>;

struct File {
  Header header;
  MultiHeader multi_header;
  std::vector<Single> singles;
  std::vector<TriggerRecord> triggers;     // after the singles, before the multi header
  std::vector<Multi> multis;
  std::vector<Playlist> playlists;
  std::vector<Sndparm> sndparms;
  std::vector<uint8_t> string_pool;        // raw blob from string_pool_off to EOF
};

// Parse an .LWF from memory into strongly typed structures. Returns false on
// validation errors, with `error` describing the issue.
bool parse_lwf_buffer(const uint8_t *data, size_t size, File &out, std::string &error);

// Encode a File structure to binary LWF format.
bool encode_lwf(const File &file, std::vector<uint8_t> &out, std::string &error);

}  // namespace lwf
}  // namespace opennova
