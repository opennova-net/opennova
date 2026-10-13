#include <formats/lwf/lwf.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cstring>

namespace opennova {
namespace lwf {
namespace {

#pragma pack(push, 1)
struct DiskHeader {
  uint32_t header_size;
  uint32_t magic;
  uint32_t single_count;
  uint32_t trigger_count;  // 12-byte records after singles [orig: SoundBank_OpenFile @ 0x75cb63]
  uint32_t multi_header_off;
  uint32_t string_pool_off;
  uint32_t reserved1;
};

struct DiskMultiHeader {
  uint32_t header_size;
  uint32_t multi_count;
  uint32_t multi_table_off;
  uint32_t reserved0;
  uint32_t reserved1;
};

struct DiskSingle {
  char name[kSingleNameBytes];
  uint8_t word32;  // no reader reads it
  uint8_t volume;  // +33
  uint16_t pad0;
  uint32_t reserved0[3];
  uint32_t path_offset;
};

// [orig: SoundBank_LoadTriggerSets @ 0x75c370 reads this 80-byte record (0x50)]
struct DiskMulti {
  uint32_t entry_size;
  char name[kMultiNameBytes];
  uint32_t pitch_base;
  uint32_t pitch_random_range;
  uint32_t playlist_count;
  uint32_t playlist_ids[kMaxPlaylistsPerMulti];  // offsets to playlists
  uint32_t target_id;
  uint32_t set_flags;
};

// [orig: SoundBank_LoadTriggerSets @ 0x75c548 reads this 48-byte record (0x30)]
struct DiskPlaylist {
  uint16_t member_count;     // the engine's u16 count [orig: @ 0x75bf58]
  uint16_t cursor;           // the engine's sequential cursor [orig: @ 0x75bf8b]: written zero
  uint16_t falloff_radius;   // tool "Falloff": audible falloff radius
  uint16_t min_distance;     // tool "Min distance": proximity fade radius
  uint32_t flags;
  uint32_t reserved0;
  uint32_t member_offsets[kMaxSndparmsPerPlaylist];  // offsets to sndparms
};

struct DiskSndparm {
  uint32_t single_index;
  uint32_t pitch_scaled;
  uint32_t random_pitch_scaled;
  uint32_t volume;
  uint32_t clamp_volume;
  uint32_t reserved0;
  uint32_t reserved1;
};
#pragma pack(pop)



template <typename T>
bool read_struct(const std::vector<uint8_t> &buf, size_t offset, T &out) {
  if (offset + sizeof(T) > buf.size()) {
    return false;
  }
  std::memcpy(&out, buf.data() + offset, sizeof(T));
  return true;
}

using strutil::fixed_string;

}  // namespace

// Helper to write a value to a buffer
template <typename T>
void write_val(std::vector<uint8_t> &buf, const T &val) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&val);
  buf.insert(buf.end(), p, p + sizeof(T));
}

// Helper to write a fixed-size string (padded with zeros)
void write_fixed_string(std::vector<uint8_t> &buf, const std::string &s, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    buf.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
  }
}

// [orig: SoundBank_OpenFile @ 0x75caa0 + SoundBank_LoadTriggerSets @ 0x75c370]
bool parse_lwf_buffer(const uint8_t *data, size_t size, File &out, std::string &error) {
  out = File{};
  error.clear();

  std::vector<uint8_t> buffer(data, data + size);

  DiskHeader raw_header{};
  if (!read_struct(buffer, 0, raw_header)) {
    error = "file too small for header";
    return false;
  }
  if (raw_header.header_size != 28) {
    error = "unexpected header size";
    return false;
  }
  // No magic is refused: the engine compares it only to gate the filename table
  // [orig: SoundBank_LoadTriggerSets @ 0x75c671] (below, the singles' paths).
  const bool has_filenames = raw_header.magic == kMagic;

  const size_t file_size = buffer.size();
  const size_t singles_off = raw_header.header_size;
  const size_t singles_size = static_cast<size_t>(raw_header.single_count) * sizeof(DiskSingle);
  if (singles_off + singles_size > file_size) {
    error = "singles table overruns file";
    return false;
  }
  // The trigger records follow the singles: read, kept, never interpreted
  // [orig: SoundBank_OpenFile @ 0x75cb63 / 0x75cb7a].
  const size_t triggers_off = singles_off + singles_size;
  const size_t triggers_size = static_cast<size_t>(raw_header.trigger_count) * kTriggerRecordBytes;
  if (triggers_off + triggers_size > file_size) {
    error = "trigger table overruns file";
    return false;
  }
  out.trigger_count = raw_header.trigger_count;

  DiskMultiHeader raw_multi_header{};
  if (!read_struct(buffer, raw_header.multi_header_off, raw_multi_header)) {
    error = "file too small for multi header";
    return false;
  }
  if (raw_multi_header.header_size != sizeof(DiskMultiHeader)) {
    error = "unexpected multi header size";
    return false;
  }
  if (raw_multi_header.multi_table_off != raw_header.multi_header_off + raw_multi_header.header_size) {
    error = "multi table offset mismatch";
    return false;
  }

  const size_t multi_table_off = raw_multi_header.multi_table_off;
  const size_t multi_table_size = static_cast<size_t>(raw_multi_header.multi_count) * sizeof(DiskMulti);
  if (multi_table_off + multi_table_size > file_size) {
    error = "multi table overruns file";
    return false;
  }

  // Derive playlist count from per-multi playlist counts (one-use per builder rules).
  uint32_t playlist_count = 0;
  std::vector<DiskMulti> disk_multis(raw_multi_header.multi_count);
  for (uint32_t i = 0; i < raw_multi_header.multi_count; ++i) {
    DiskMulti dm{};
    if (!read_struct(buffer, multi_table_off + i * sizeof(DiskMulti), dm)) {
      error = "failed to read multi entry";
      return false;
    }
    disk_multis[i] = dm;
    playlist_count += dm.playlist_count;
  }

  const size_t playlists_off = multi_table_off + multi_table_size;
  const size_t playlists_size = static_cast<size_t>(playlist_count) * sizeof(DiskPlaylist);
  const size_t sndparms_off = playlists_off + playlists_size;
  if (sndparms_off > file_size || sndparms_off > raw_header.string_pool_off) {
    error = "playlist region overruns file";
    return false;
  }

  if (raw_header.string_pool_off < sndparms_off) {
    error = "string pool offset precedes sndparm region";
    return false;
  }
  const size_t sndparm_region_size = raw_header.string_pool_off - sndparms_off;
  if (sndparm_region_size % sizeof(DiskSndparm) != 0) {
    error = "sndparm region not aligned to entry size";
    return false;
  }
  const uint32_t sndparm_count = static_cast<uint32_t>(sndparm_region_size / sizeof(DiskSndparm));

  const size_t string_pool_off = raw_header.string_pool_off;
  if (string_pool_off > file_size) {
    error = "string pool offset beyond file size";
    return false;
  }
  const size_t string_pool_size = file_size - string_pool_off;

  // Validate expected string pool size (builder used 0x100 bytes per single); a bank
  // without the filename table is never read for one.
  const size_t expected_pool = static_cast<size_t>(raw_header.single_count) * kPathSlotBytes;
  if (has_filenames && string_pool_size < expected_pool) {
    // Not fatal, but worth noting to the caller.
    error = "string pool shorter than expected 0x100 per single";
    return false;
  }

  // Fill header structures.
  out.header = Header{raw_header.header_size, raw_header.magic, raw_header.single_count,
                      raw_header.trigger_count, raw_header.multi_header_off, raw_header.string_pool_off};
  out.multi_header = MultiHeader{raw_multi_header.header_size, raw_multi_header.multi_count,
                                 raw_multi_header.multi_table_off};

  // Singles.
  out.singles.reserve(raw_header.single_count);
  for (uint32_t i = 0; i < raw_header.single_count; ++i) {
    DiskSingle ds{};
    read_struct(buffer, singles_off + i * sizeof(DiskSingle), ds);

    Single s{};
    s.name = fixed_string(ds.name, sizeof(ds.name));
    s.volume = ds.volume;
    if (!has_filenames && s.name.size() <= 28) {
      std::memcpy(&s.entry_word28, ds.name + 28, sizeof(s.entry_word28));
    }

    if (!has_filenames) {
      // No filename table: the single names no file [orig: @ 0x75c671].
      out.singles.push_back(std::move(s));
      continue;
    }
    // Slot i of the pool, whatever the entry's offset says [orig: @ 0x75c6e1..0x75c711]
    // (the pool's size checked above).
    s.path = fixed_string(reinterpret_cast<const char *>(buffer.data() + string_pool_off) + i * kPathSlotBytes,
                          kPathSlotBytes);

    out.singles.push_back(std::move(s));
  }

  // Multis.
  out.multis.reserve(raw_multi_header.multi_count);
  for (uint32_t i = 0; i < raw_multi_header.multi_count; ++i) {
    const DiskMulti &dm = disk_multis[i];
    Multi m{};
    m.name = fixed_string(dm.name, sizeof(dm.name));
    m.pitch_base = dm.pitch_base;
    m.pitch_random_range = dm.pitch_random_range;
    m.target_id = dm.target_id;
    m.set_flags = dm.set_flags;

    for (uint32_t j = 0; j < dm.playlist_count && j < kMaxPlaylistsPerMulti; ++j) {
      const uint32_t raw = dm.playlist_ids[j];
      if (raw == 0) continue;
      if (raw < playlists_off || raw >= sndparms_off || (raw - playlists_off) % sizeof(DiskPlaylist) != 0) {
        error = "multi playlist offset invalid";
        return false;
      }
      const uint32_t idx = static_cast<uint32_t>((raw - playlists_off) / sizeof(DiskPlaylist));
      if (idx >= playlist_count) {
        error = "multi playlist index out of bounds";
        return false;
      }
      m.playlist_indices.push_back(idx);
    }

    out.multis.push_back(std::move(m));
  }

  // Playlists.
  out.playlists.reserve(playlist_count);
  for (uint32_t i = 0; i < playlist_count; ++i) {
    DiskPlaylist dp{};
    if (!read_struct(buffer, playlists_off + i * sizeof(DiskPlaylist), dp)) {
      error = "failed to read playlist entry";
      return false;
    }

    Playlist p{};
    p.falloff_radius = dp.falloff_radius;
    p.min_distance = dp.min_distance;
    p.flags = dp.flags;
    if (dp.flags & 0x100u) p.anchor = static_cast<uint16_t>(dp.reserved0 & 0xFFFFu);

    for (uint32_t j = 0; j < dp.member_count && j < kMaxSndparmsPerPlaylist; ++j) {
      const uint32_t raw = dp.member_offsets[j];
      if (raw == 0) continue;
      if (raw < sndparms_off || raw >= string_pool_off || (raw - sndparms_off) % sizeof(DiskSndparm) != 0) {
        error = "playlist sndparm offset invalid";
        return false;
      }
      const uint32_t idx = static_cast<uint32_t>((raw - sndparms_off) / sizeof(DiskSndparm));
      if (idx >= sndparm_count) {
        error = "playlist sndparm index out of bounds";
        return false;
      }
      p.sndparm_indices.push_back(idx);
    }

    out.playlists.push_back(std::move(p));
  }

  // Sndparms.
  out.sndparms.reserve(sndparm_count);
  for (uint32_t i = 0; i < sndparm_count; ++i) {
    DiskSndparm ds{};
    if (!read_struct(buffer, sndparms_off + i * sizeof(DiskSndparm), ds)) {
      error = "failed to read sndparm entry";
      return false;
    }

    Sndparm s{};
    s.single_index = ds.single_index;
    s.pitch_scaled = ds.pitch_scaled;
    s.random_pitch_scaled = ds.random_pitch_scaled;
    s.volume = ds.volume;
    s.clamp_volume = ds.clamp_volume;
    if (s.single_index >= out.singles.size()) {
      error = "sndparm references out-of-range single";
      return false;
    }
    out.sndparms.push_back(std::move(s));
  }

  return true;
}

bool encode_lwf(const File &file, std::vector<uint8_t> &out, std::string &error) {
  out.clear();
  error.clear();

  // Calculate sizes and offsets.
  const uint32_t header_size = 28;
  const uint32_t single_entry_size = sizeof(DiskSingle);
  const uint32_t multi_header_size = sizeof(DiskMultiHeader);
  const uint32_t multi_entry_size = sizeof(DiskMulti);
  const uint32_t playlist_entry_size = sizeof(DiskPlaylist);
  const uint32_t sndparm_entry_size = sizeof(DiskSndparm);

  const uint32_t single_count = static_cast<uint32_t>(file.singles.size());
  const uint32_t multi_count = static_cast<uint32_t>(file.multis.size());

  // Count total playlists and sndparms.
  uint32_t playlist_count = 0;
  uint32_t sndparm_count = 0;
  for (const auto &m : file.multis) {
    playlist_count += static_cast<uint32_t>(m.playlist_indices.size());
  }
  for (const auto &pl : file.playlists) {
    sndparm_count += static_cast<uint32_t>(pl.sndparm_indices.size());
  }
  // Use actual counts from file structure.
  playlist_count = static_cast<uint32_t>(file.playlists.size());
  sndparm_count = static_cast<uint32_t>(file.sndparms.size());

  // Calculate offsets.
  const uint32_t singles_off = header_size;
  const uint32_t trigger_count = file.trigger_count;
  const uint32_t triggers_off = singles_off + single_count * single_entry_size;
  const uint32_t multi_header_off = triggers_off + trigger_count * static_cast<uint32_t>(kTriggerRecordBytes);
  const uint32_t multi_table_off = multi_header_off + multi_header_size;
  const uint32_t playlists_off = multi_table_off + multi_count * multi_entry_size;
  const uint32_t sndparms_off = playlists_off + playlist_count * playlist_entry_size;
  const uint32_t string_pool_off = sndparms_off + sndparm_count * sndparm_entry_size;

  // The string pool: a 256-byte slot per single, its path and zeros after it.
  std::vector<uint8_t> string_pool;
  for (const auto &s : file.singles) {
    for (size_t i = 0; i < kPathSlotBytes; ++i) {
      string_pool.push_back(i < s.path.size() ? static_cast<uint8_t>(s.path[i]) : 0);
    }
  }

  // Write header.
  DiskHeader hdr{};
  hdr.header_size = header_size;
  hdr.magic = file.header.magic;  // as read: the engine refuses none (lwf.h)
  hdr.single_count = single_count;
  hdr.trigger_count = trigger_count;
  hdr.multi_header_off = multi_header_off;
  hdr.string_pool_off = string_pool_off;
  write_val(out, hdr);

  // Write singles.
  for (uint32_t i = 0; i < single_count; ++i) {
    const auto &s = file.singles[i];
    DiskSingle ds{};
    std::memcpy(ds.name, s.name.c_str(), std::min(s.name.size(), sizeof(ds.name)));
    if (file.header.magic != kMagic && s.name.size() <= 28) {
      // At 28 characters the word's low byte is the name's terminator.
      const uint32_t word = s.name.size() == 28 ? s.entry_word28 & ~0xFFu : s.entry_word28;
      std::memcpy(ds.name + 28, &word, sizeof(word));
    }
    ds.volume = s.volume;
    // The path pointer: the slot's offset in an 'LWF1' bank, which the engine rebinds;
    // none in another, where a non-zero one would be taken as a path [orig: @ 0x75c671].
    ds.path_offset = file.header.magic == kMagic ? string_pool_off + i * static_cast<uint32_t>(kPathSlotBytes) : 0;
    write_val(out, ds);
  }

  // The trigger records (after the singles, before the multi header): their count
  // kept, each written zero (no reader interprets one).
  out.insert(out.end(), static_cast<size_t>(trigger_count) * kTriggerRecordBytes, uint8_t{0});

  // Write the multi header, its reserved words zero.
  DiskMultiHeader mhdr{};
  mhdr.header_size = multi_header_size;
  mhdr.multi_count = multi_count;
  mhdr.multi_table_off = multi_table_off;
  write_val(out, mhdr);

  // Write multis.
  for (const auto &m : file.multis) {
    DiskMulti dm{};
    dm.entry_size = multi_entry_size;
    std::memcpy(dm.name, m.name.c_str(), std::min(m.name.size(), sizeof(dm.name)));
    dm.pitch_base = m.pitch_base;
    dm.pitch_random_range = m.pitch_random_range;
    dm.playlist_count = static_cast<uint32_t>(m.playlist_indices.size());
    for (size_t i = 0; i < m.playlist_indices.size() && i < kMaxPlaylistsPerMulti; ++i) {
      dm.playlist_ids[i] = playlists_off + m.playlist_indices[i] * playlist_entry_size;
    }
    dm.target_id = m.target_id;
    dm.set_flags = m.set_flags;
    write_val(out, dm);
  }

  // Write playlists.
  for (const auto &pl : file.playlists) {
    DiskPlaylist dp{};
    dp.member_count = static_cast<uint16_t>(pl.sndparm_indices.size());
    dp.falloff_radius = pl.falloff_radius;
    dp.min_distance = pl.min_distance;
    dp.flags = pl.flags;
    if (pl.flags & 0x100u) dp.reserved0 = pl.anchor;
    for (size_t i = 0; i < pl.sndparm_indices.size() && i < kMaxSndparmsPerPlaylist; ++i) {
      dp.member_offsets[i] = sndparms_off + pl.sndparm_indices[i] * sndparm_entry_size;
    }
    write_val(out, dp);
  }

  // Write sndparms.
  for (const auto &sp : file.sndparms) {
    DiskSndparm ds{};
    ds.single_index = sp.single_index;
    ds.pitch_scaled = sp.pitch_scaled;
    ds.random_pitch_scaled = sp.random_pitch_scaled;
    ds.volume = sp.volume;
    ds.clamp_volume = sp.clamp_volume;
    write_val(out, ds);
  }

  // Write string pool.
  out.insert(out.end(), string_pool.begin(), string_pool.end());

  return true;
}

}  // namespace lwf
}  // namespace opennova
