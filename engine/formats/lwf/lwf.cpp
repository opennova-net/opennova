#include "lwf/lwf.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

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
  char name[32];
  uint16_t value_hi;
  uint16_t pad0;
  uint32_t reserved0[3];
  uint32_t path_offset;
};

// [orig: SoundBank_LoadTriggerSets @ 0x75c370 reads this 80-byte record (0x50)]
struct DiskMulti {
  uint32_t entry_size;
  char name[24];
  uint32_t pitch_base;
  uint32_t pitch_random_range;
  uint32_t playlist_count;
  uint32_t playlist_ids[8];  // offsets to playlists
  uint32_t target_id;
  uint32_t set_flags;
};

// [orig: SoundBank_LoadTriggerSets @ 0x75c548 reads this 48-byte record (0x30)]
struct DiskPlaylist {
  uint32_t member_count;
  uint16_t falloff_radius;   // tool "Falloff": audible falloff radius
  uint16_t min_distance;     // tool "Min distance": proximity fade radius
  uint32_t flags;
  uint32_t reserved0;
  uint32_t member_offsets[8];  // offsets to sndparms
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

std::string read_c_string(const char *data, size_t max_len) {
  size_t len = 0;
  while (len < max_len && data[len] != '\0') {
    ++len;
  }
  return std::string(data, len);
}

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

bool parse_lwf_buffer(const uint8_t *data, size_t size, File &out, std::string &error);

bool parse_lwf(const std::string &path, File &out, std::string &error) {
  out = File{};
  error.clear();

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "failed to open file";
    return false;
  }

  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size <= 0) {
    error = "empty file";
    return false;
  }
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  file.read(reinterpret_cast<char *>(buffer.data()), size);
  if (!file) {
    error = "failed to read file";
    return false;
  }

  return parse_lwf_buffer(buffer.data(), buffer.size(), out, error);
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
  if (raw_header.magic != kMagic) {
    error = "bad magic (expected 'LWF1')";
    return false;
  }
  if (raw_header.trigger_count != 0) {
    // The engine reads 12 * trigger_count bytes of trigger records after the
    // singles table [orig: SoundBank_OpenFile @ 0x75cb63]; no JO-era bank uses
    // them and this parser does not model the table, so re-encoding would drop
    // it. Reject loudly instead of corrupting.
    error = "trigger table not supported (header dword 3 nonzero)";
    return false;
  }

  const size_t file_size = buffer.size();
  const size_t singles_off = raw_header.header_size;
  const size_t singles_size = static_cast<size_t>(raw_header.single_count) * sizeof(DiskSingle);
  if (singles_off + singles_size > file_size) {
    error = "singles table overruns file";
    return false;
  }

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
  out.string_pool.assign(buffer.begin() + string_pool_off, buffer.end());

  // Validate expected string pool size (builder used 0x100 bytes per single).
  const size_t expected_pool = static_cast<size_t>(raw_header.single_count) * 0x100;
  if (out.string_pool.size() < expected_pool) {
    // Not fatal, but worth noting to the caller.
    error = "string pool shorter than expected 0x100 per single";
    return false;
  }

  // Fill header structures.
  out.header = Header{raw_header.header_size, raw_header.magic, raw_header.single_count,
                      raw_header.trigger_count, raw_header.multi_header_off, raw_header.string_pool_off,
                      raw_header.reserved1};
  out.multi_header = MultiHeader{raw_multi_header.header_size, raw_multi_header.multi_count,
                                 raw_multi_header.multi_table_off, raw_multi_header.reserved0,
                                 raw_multi_header.reserved1};

  // Singles.
  out.singles.reserve(raw_header.single_count);
  for (uint32_t i = 0; i < raw_header.single_count; ++i) {
    DiskSingle ds{};
    read_struct(buffer, singles_off + i * sizeof(DiskSingle), ds);

    Single s{};
    s.name = read_c_string(ds.name, sizeof(ds.name));
    s.value_hi = ds.value_hi;
    s.path_offset = ds.path_offset;
    // Store raw bytes for byte-perfect round-trip.
    std::memcpy(s.raw_name.data(), ds.name, sizeof(ds.name));
    s.pad0 = ds.pad0;
    s.reserved0 = {ds.reserved0[0], ds.reserved0[1], ds.reserved0[2]};

    size_t path_abs = static_cast<size_t>(ds.path_offset);
    if (path_abs >= file_size) {
      error = "path offset out of range";
      return false;
    }
    if (path_abs < string_pool_off) {
      error = "path offset not within string pool";
      return false;
    }
    const size_t path_rel = path_abs - string_pool_off;
    if (path_rel >= out.string_pool.size()) {
      error = "path offset not within string pool";
      return false;
    }
    const char *path_ptr = reinterpret_cast<const char *>(buffer.data() + path_abs);
    const size_t remaining = out.string_pool.size() - path_rel;
    s.path = read_c_string(path_ptr, std::min<size_t>(remaining, 0x100));

    out.singles.push_back(std::move(s));
  }

  // Multis.
  out.multis.reserve(raw_multi_header.multi_count);
  for (uint32_t i = 0; i < raw_multi_header.multi_count; ++i) {
    const DiskMulti &dm = disk_multis[i];
    Multi m{};
    m.name = read_c_string(dm.name, sizeof(dm.name));
    m.pitch_base = dm.pitch_base;
    m.pitch_random_range = dm.pitch_random_range;
    m.target_id = dm.target_id;
    // Store raw bytes for byte-perfect round-trip.
    std::memcpy(m.raw_name.data(), dm.name, sizeof(dm.name));
    std::memcpy(m.raw_playlist_ids.data(), dm.playlist_ids, sizeof(dm.playlist_ids));
    m.set_flags = dm.set_flags;

    for (uint32_t j = 0; j < dm.playlist_count && j < 8; ++j) {
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
    // Store raw bytes for byte-perfect round-trip.
    p.reserved0 = dp.reserved0;
    std::memcpy(p.raw_member_offsets.data(), dp.member_offsets, sizeof(dp.member_offsets));

    for (uint32_t j = 0; j < dp.member_count && j < 8; ++j) {
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
    s.reserved = {ds.reserved0, ds.reserved1};
    if (s.single_index >= out.singles.size()) {
      error = "sndparm references out-of-range single";
      return false;
    }
    out.sndparms.push_back(std::move(s));
  }

  return true;
}

bool lwf_to_sdf(const File &file,
                const std::string &wav_base_path,
                const std::string &source_lwf_path,
                const std::string &sdf_name,
                std::string &out_sdf,
                std::string &error) {
  out_sdf.clear();
  error.clear();

  auto append_line = [&](const std::string &s) { out_sdf.append(s).append("\r\n"); };

  // Basic validation: require at least one playlist and sndparm if present.
  // (We keep lenient to match original tool; headers are written regardless.)

  // Header lines.
  append_line("Description\tSDF Name\tLWF Name\tCopy Directory");
  append_line("<Blank Project>\t" + sdf_name + "\t" + source_lwf_path + "\t");
  append_line(
      "Sound Set Name\tLayer Name\tLayer Falloff\tLayer Min distance\tMember choice method\tMember ID\tMember filename\t"
      "Member pitch\tMember random pitch\tMember volume\tMember clamp volume\tIs looping?\tIs directional?\tAllow "
      "heading?\tPreload sound?\tIs Stoppable?\tIs Internal?\tIs External?\tUses Reverb?\tIs Rapid?\tComments");

  // Helper to format yes/no variants like the original tool.
  auto yes_no = [](bool v, const char *label, const char *neg = "Not ", const char *pos = "") -> std::string {
    std::string s;
    s.push_back('\t');
    if (!v && neg) s.append(neg);
    if (v && pos) s.append(pos);
    s.append(label);
    return s;
  };

  const auto flag_set = [](uint32_t flags, uint32_t bit) -> bool { return (flags & bit) != 0; };
  const auto fmt_float = [](double v) -> std::string {
    std::ostringstream oss;
    oss.setf(std::ios::fixed, std::ios::floatfield);
    oss.precision(6);
    oss << v;
    return oss.str();
  };

  // Build synthesized layer names A/B/... per multi.
  for (size_t mi = 0; mi < file.multis.size(); ++mi) {
    const auto &m = file.multis[mi];
    for (size_t pi = 0; pi < m.playlist_indices.size(); ++pi) {
      const uint32_t pidx = m.playlist_indices[pi];
      if (pidx >= file.playlists.size()) {
        error = "playlist index out of range";
        return false;
      }
      const auto &pl = file.playlists[pidx];
      const char layer_letter = static_cast<char>('A' + pi);

      // Iterate sndparms for this playlist.
      for (size_t si = 0; si < pl.sndparm_indices.size(); ++si) {
        const uint32_t sidx = pl.sndparm_indices[si];
        if (sidx >= file.sndparms.size()) {
          error = "sndparm index out of range";
          return false;
        }
        const auto &sp = file.sndparms[sidx];
        if (sp.single_index >= file.singles.size()) {
          error = "sndparm single index out of range";
          return false;
        }
        const auto &sg = file.singles[sp.single_index];

        // Columns in order.
        // Sound Set Name
        out_sdf.append(m.name);
        // Layer Name
        out_sdf.append("\t").append(m.name).push_back('_');
        out_sdf.push_back(layer_letter);
        // Layer Falloff / Min distance (the original tool's column names for
        // falloff_radius / min_distance; keep them so SDF output matches lwf2sdf).
        out_sdf.append("\t").append(std::to_string(pl.falloff_radius));
        out_sdf.append("\t").append(std::to_string(pl.min_distance));

        // Choice method
        std::string choice = "Unknown";
        if (flag_set(pl.flags, kFlagRandom)) {
          choice = "Random";
        } else if (flag_set(pl.flags, kFlagSequential)) {
          choice = "Sequential";
        } else if (flag_set(pl.flags, kFlagRandomSequential)) {
          choice = "Random Sequential";
        }
        out_sdf.append("\t").append(choice).append("\t");

        // Member ID
        out_sdf.append(sg.name);
        // Member filename
        std::string full_path = sg.path;
        if (!wav_base_path.empty()) {
          full_path = wav_base_path + sg.path;
        }
        out_sdf.append("\t").append(full_path);
        // Member pitch / random pitch
        out_sdf.append("\t").append(fmt_float(static_cast<double>(sp.pitch_scaled) / 65535.0));
        out_sdf.append("\t").append(fmt_float(static_cast<double>(sp.random_pitch_scaled) / 65535.0));
        // Member volume / clamp volume
        out_sdf.append("\t").append(std::to_string(sp.volume));
        out_sdf.append("\t").append(std::to_string(sp.clamp_volume));

        // Flags in order: Looping, Directional, Heading, Preload, Stoppable, Internal, External, Reverb, Rapid
        out_sdf.append(yes_no(flag_set(pl.flags, kFlagLooping), "Looping", "Not ", ""));
        out_sdf.append(yes_no(flag_set(pl.flags, kFlagDirectional), "Directional", "Not ", ""));
        out_sdf.append(yes_no(flag_set(pl.flags, kFlagHeading), "Heading", "No ", ""));
        out_sdf.append(yes_no(flag_set(pl.flags, kFlagPreload), "Preload", "No ", ""));
        out_sdf.append(yes_no(flag_set(pl.flags, kFlagStoppable), "Stoppable", "Not ", ""));
        out_sdf.append(yes_no(flag_set(pl.flags, kFlagInternal), "Internal", "Not ", ""));
        out_sdf.append(yes_no(flag_set(pl.flags, kFlagExternal), "External", "Not ", ""));
        out_sdf.append(yes_no(flag_set(pl.flags, kFlagReverb), "Reverb", "No ", ""));
        out_sdf.append(yes_no(flag_set(pl.flags, kFlagRapid), "Rapid", "Not ", ""));

        // Comment
        out_sdf.append("\tLWF2SDF by DH\r\n");
      }
    }
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
  const uint32_t multi_header_off = singles_off + single_count * single_entry_size;
  const uint32_t multi_table_off = multi_header_off + multi_header_size;
  const uint32_t playlists_off = multi_table_off + multi_count * multi_entry_size;
  const uint32_t sndparms_off = playlists_off + playlist_count * playlist_entry_size;
  const uint32_t string_pool_off = sndparms_off + sndparm_count * sndparm_entry_size;

  // Use original string pool if available for byte-perfect round-trip.
  // Otherwise, build a new one from paths.
  std::vector<uint8_t> string_pool;
  if (!file.string_pool.empty()) {
    // Use the original raw string pool.
    string_pool = file.string_pool;
  } else {
    // Build string pool (0x100 bytes per single).
    for (const auto &s : file.singles) {
      // Write path (up to 0x100 bytes, null-padded).
      for (size_t i = 0; i < 0x100; ++i) {
        string_pool.push_back(i < s.path.size() ? static_cast<uint8_t>(s.path[i]) : 0);
      }
    }
  }

  // Write header (preserve original reserved fields for byte-perfect round-trip).
  DiskHeader hdr{};
  hdr.header_size = header_size;
  hdr.magic = kMagic;
  hdr.single_count = single_count;
  hdr.trigger_count = file.header.trigger_count;
  hdr.multi_header_off = multi_header_off;
  hdr.string_pool_off = string_pool_off;
  hdr.reserved1 = file.header.reserved1;
  write_val(out, hdr);

  // Write singles.
  for (uint32_t i = 0; i < single_count; ++i) {
    const auto &s = file.singles[i];
    DiskSingle ds{};
    // Use raw bytes for byte-perfect round-trip, or fall back to name if empty.
    if (s.raw_name[0] != '\0') {
      std::memcpy(ds.name, s.raw_name.data(), sizeof(ds.name));
    } else {
      // Fall back to name string (null-padded).
      std::memset(ds.name, 0, sizeof(ds.name));
      std::memcpy(ds.name, s.name.c_str(), std::min(s.name.size(), sizeof(ds.name) - 1));
    }
    ds.value_hi = s.value_hi;
    ds.pad0 = s.pad0;
    ds.reserved0[0] = s.reserved0[0];
    ds.reserved0[1] = s.reserved0[1];
    ds.reserved0[2] = s.reserved0[2];
    ds.path_offset = string_pool_off + i * 0x100;
    write_val(out, ds);
  }

  // Write multi header (preserve original reserved fields for byte-perfect round-trip).
  DiskMultiHeader mhdr{};
  mhdr.header_size = multi_header_size;
  mhdr.multi_count = multi_count;
  mhdr.multi_table_off = multi_table_off;
  mhdr.reserved0 = file.multi_header.reserved0;
  mhdr.reserved1 = file.multi_header.reserved1;
  write_val(out, mhdr);

  // Write multis.
  for (const auto &m : file.multis) {
    DiskMulti dm{};
    dm.entry_size = multi_entry_size;
    // Use raw bytes for byte-perfect round-trip, or fall back to name if empty.
    if (m.raw_name[0] != '\0') {
      std::memcpy(dm.name, m.raw_name.data(), sizeof(dm.name));
    } else {
      std::memset(dm.name, 0, sizeof(dm.name));
      std::memcpy(dm.name, m.name.c_str(), std::min(m.name.size(), sizeof(dm.name) - 1));
    }
    dm.pitch_base = m.pitch_base;
    dm.pitch_random_range = m.pitch_random_range;
    dm.playlist_count = static_cast<uint32_t>(m.playlist_indices.size());
    // Start with raw playlist_ids (preserves garbage in unused slots).
    std::memcpy(dm.playlist_ids, m.raw_playlist_ids.data(), sizeof(dm.playlist_ids));
    // Overwrite the active slots with recalculated offsets.
    for (size_t i = 0; i < m.playlist_indices.size() && i < 8; ++i) {
      dm.playlist_ids[i] = playlists_off + m.playlist_indices[i] * playlist_entry_size;
    }
    dm.target_id = m.target_id;
    dm.set_flags = m.set_flags;
    write_val(out, dm);
  }

  // Write playlists.
  for (const auto &pl : file.playlists) {
    DiskPlaylist dp{};
    dp.member_count = static_cast<uint32_t>(pl.sndparm_indices.size());
    dp.falloff_radius = pl.falloff_radius;
    dp.min_distance = pl.min_distance;
    dp.flags = pl.flags;
    dp.reserved0 = pl.reserved0;
    // Start with raw member_offsets (preserves garbage in unused slots).
    std::memcpy(dp.member_offsets, pl.raw_member_offsets.data(), sizeof(dp.member_offsets));
    // Overwrite the active slots with recalculated offsets.
    for (size_t i = 0; i < pl.sndparm_indices.size() && i < 8; ++i) {
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
    ds.reserved0 = sp.reserved[0];
    ds.reserved1 = sp.reserved[1];
    write_val(out, ds);
  }

  // Write string pool.
  out.insert(out.end(), string_pool.begin(), string_pool.end());

  return true;
}

bool write_lwf(const File &file, const std::string &path, std::string &error) {
  std::vector<uint8_t> data;
  if (!encode_lwf(file, data, error)) {
    return false;
  }

  std::ofstream out_file(path, std::ios::binary);
  if (!out_file) {
    error = "failed to create file: " + path;
    return false;
  }

  out_file.write(reinterpret_cast<const char *>(data.data()), data.size());
  if (!out_file) {
    error = "failed to write file: " + path;
    return false;
  }

  return true;
}

}  // namespace lwf
}  // namespace opennova
