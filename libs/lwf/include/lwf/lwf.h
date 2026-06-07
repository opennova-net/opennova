// LWF sound-profile container reader/writer (magic 'LWF1').
//
// Reverse-engineered from lwfbuilder.exe / lwf2sdf.exe / SoundTool.exe; ported
// byte-for-byte from the on-godot-oscarmike prototype's libs/lwf. The on-disk
// layout is a pure index: Singles (audio refs) -> Multis (sound sets) ->
// Playlists (layers) -> Sndparms (members) -> string pool (256-byte .wav path
// slots, one per single). No audio is embedded.
//
// Round-trip is byte-exact on the *unmodified* parse->encode path: garbage in
// unused/reserved slots and the original string pool are preserved verbatim.
// See RE notes lwfbuilder.md / lwf2sdf.md / soundtool.md.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova {
namespace lwf {

constexpr uint32_t kMagic = 0x4C574631;  // 'LWF1'

struct Header {
  uint32_t header_size = 0;       // expected 28
  uint32_t magic = 0;             // 'LWF1'
  uint32_t single_count = 0;
  uint32_t reserved0 = 0;
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
  std::string path;               // parsed from string pool (256-byte slots)
  // Raw bytes for byte-perfect round-trip (includes garbage after null terminator).
  std::array<char, 32> raw_name{};
  uint16_t pad0 = 0;
  std::array<uint32_t, 3> reserved0{};
};

struct Multi {
  std::string name;
  uint32_t unk28 = 0;             // observed 0xFFFF
  uint32_t unk32 = 0;             // unused
  std::vector<uint32_t> playlist_indices;  // indices into playlists table
  uint32_t target_id = 0;
  // Raw bytes for byte-perfect round-trip.
  std::array<char, 24> raw_name{};
  std::array<uint32_t, 8> raw_playlist_ids{};  // includes garbage in unused slots
  uint32_t reserved = 0;
};

// Playlist flags (bitmask)
enum PlaylistFlags : uint32_t {
  kFlagHeading           = 0x0001,
  kFlagInternal          = 0x0002,
  kFlagExternal          = 0x0004,
  kFlagRandom            = 0x0008,
  kFlagSequential        = 0x0010,
  kFlagStoppable         = 0x0020,
  kFlagPreload           = 0x0040,
  kFlagRandomSequential  = 0x0080,  // sign bit in original tool
  kFlagDirectional       = 0x0200,
  kFlagLooping           = 0x0400,
  kFlagReverb            = 0x0800,
  kFlagRapid             = 0x1000,
};

struct Playlist {
  uint16_t falloff = 0;
  uint16_t min_distance = 0;
  uint32_t flags = 0;                       // PlaylistFlags bitmask
  std::vector<uint32_t> sndparm_indices;    // indices into sndparm table
  // Raw bytes for byte-perfect round-trip.
  uint32_t reserved0 = 0;
  std::array<uint32_t, 8> raw_member_offsets{};  // includes garbage in unused slots
};

struct Sndparm {
  uint32_t single_index = 0;
  uint32_t pitch_scaled = 0;                // scaled by 65535
  uint32_t random_pitch_scaled = 0;         // scaled by 65535
  uint32_t volume = 0;
  uint32_t clamp_volume = 0;
  std::array<uint32_t, 2> reserved{};       // trailing dwords remain opaque
};

struct File {
  Header header;
  MultiHeader multi_header;
  std::vector<Single> singles;
  std::vector<Multi> multis;
  std::vector<Playlist> playlists;
  std::vector<Sndparm> sndparms;
  std::vector<uint8_t> string_pool;        // raw blob from string_pool_off to EOF
};

// Parse an on-disk .LWF into strongly typed structures.
// Returns false on validation errors, with `error` describing the issue.
bool parse_lwf(const std::string &path, File &out, std::string &error);

// Parse LWF from memory buffer.
bool parse_lwf_buffer(const uint8_t *data, size_t size, File &out, std::string &error);

// Encode a File structure to binary LWF format.
bool encode_lwf(const File &file, std::vector<uint8_t> &out, std::string &error);

// Write encoded LWF to disk.
bool write_lwf(const File &file, const std::string &path, std::string &error);

// Convert a parsed LWF to a tab-delimited SDF string. Returns false on error.
// wav_base_path: optional prefix to prepend to each single path (use "" to omit).
// source_lwf_path: path to the input LWF (for header row).
// sdf_name: desired SDF filename (for header row).
bool lwf_to_sdf(const File &file,
                const std::string &wav_base_path,
                const std::string &source_lwf_path,
                const std::string &sdf_name,
                std::string &out_sdf,
                std::string &error);

}  // namespace lwf
}  // namespace opennova
