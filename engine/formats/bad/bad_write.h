// BAD skeletal-animation writer: serializes a BadFile into the on-disk binary
// from scratch (ADR 0003 -- never a passthrough of parsed bytes). The exact
// inverse of bad_parse_buffer (bad.cpp): a write followed by a parse
// reconstructs every field, and a parse of a file this writer produced writes
// the same bytes again (ctest bad_roundtrip).
//
// [orig: BoneFile_Load @0x40fff0 -- the on-disk layout is that loader's
//  relocation table (file-relative offsets in the header and channel rows,
//  absolute bone addresses in the bone table); retail ships no .bad writer,
//  so the layout below is pinned by the reader and the retail corpus]
//
// Layout the writer emits (the reader accepts any order; this is the order
// the retail corpus and our fixtures carry):
//   [0]    header            80 bytes (20 x u32)
//   [80]   channel table     bone_count x 12: num_frames, frame_lengths_off, rotations_off
//   ...    per channel       u16 frame_lengths[], pad to 4, f32 x,y,z,w per key
//   evt    events            per event f32 vx,vy,vz,bottom,top (+ i32 trigger when version 1)
//   bone   bone table        bone_count x 100: name[32], 3 pad + index byte, num_children,
//                            first_child_addr, parent_addr, length, position[3], rotation[9]
//   trn    translations      when flags & 2: (frame_count + 2) x bone_count x f32 x,y,z,
//                            frame-major: rows 0..frame_count, then a pad row
//                            repeating row frame_count (bad_write.cpp)
//
// Header words the reader never names carry the values every retail clip ships
// ([8] 0, [9] 0, [10] 8, [14] 1, [17] 1, [18] 0, [19] 0); a grill of the loader
// is what would type them on BadFile.
//
// Conventions the corpus pins: channels, events and translation rows carry
// frame_count + 1 entries (the header counts intervals); child/parent are absolute byte
// addresses recomputed from parent_index; a root bone's parent address and a
// leaf's child address are 0 (the reader's "no parent"); the bone's +35 byte
// is its own index.
#pragma once

#include <formats/bad/bad.h>

#include <cstdint>
#include <vector>

namespace opennova::bad {

// Serialize `bf` into `out` (replaced). Returns 0, or -1 when the file cannot be
// represented: a null input, a channel or bone table shorter than bone_count,
// a translation block (flags & 2) shorter than bone_count x (frame_count + 1),
// an event count with no events, or a bone name with no NUL in its 32 bytes.
int bad_write_buffer(const BadFile *bf, std::vector<uint8_t> &out);

// Serialize to a path. Returns 0 on success, -1 on a representation or I/O error.
int bad_write(const char *path, const BadFile *bf);

} // namespace opennova::bad
