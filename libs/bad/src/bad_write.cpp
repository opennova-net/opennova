// BAD animation file writer — serializes a BadFile back to the on-disk binary.
//
// This is the exact inverse of bad_parse (see bad.cpp): it lays out the header,
// frame (channel) table, per-channel keyframe data, events, bone table and the
// optional translation block so that a subsequent bad_parse reconstructs the
// same structure. It does not perform any coordinate math — callers (DCC
// gather layers) hand it data already in BAD coordinate space.
//
// Byte layout (matches the reader in bad.cpp):
//   [0]     header            80 bytes (20 * u32)
//   [80]    frame table       bone_count * 12  (num_frames u32, frame_len_off u32, rot_off u32)
//   [..]    per channel       frame_lengths u16[], pad to 4, then rotations f32 xyzw[] (rot_stride each)
//   [evt]   events (optional) per event: f32 vx,vy,vz,bottom,top, then i32 trigger when version==1
//   [bone]  bone table        bone_count * bone_stride (name[32], 3 pad + idx byte, num_children,
//                             child_addr, parent_addr, length, position[3], rotation[9])
//   [trn]   translations      when flags&2: num_translations * trans_stride (f32 x,y,z), frame-major
//
// Conventions preserved from the historical Blender exporter / stock assets:
//   * channels and events carry frame_count+1 entries (terminal-duplicate); the
//     writer simply emits whatever counts the caller provides.
//   * child/parent stored as absolute byte addresses recomputed from parent_index.
//   * root bones write parent_offset 0 (the parser's "no parent" convention), a
//     deliberate fix over the historical writer's -1 (which only ever parsed
//     correctly for bone 0 because the parser force-overrides it).

#include "bad/bad.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace {

const uint32_t kHeaderBytes = 80;

void put_u32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back((uint8_t)(v & 0xFF));
    buf.push_back((uint8_t)((v >> 8) & 0xFF));
    buf.push_back((uint8_t)((v >> 16) & 0xFF));
    buf.push_back((uint8_t)((v >> 24) & 0xFF));
}

void put_s32(std::vector<uint8_t> &buf, int32_t v) { put_u32(buf, (uint32_t)v); }

void put_u16(std::vector<uint8_t> &buf, uint16_t v) {
    buf.push_back((uint8_t)(v & 0xFF));
    buf.push_back((uint8_t)((v >> 8) & 0xFF));
}

void put_f32(std::vector<uint8_t> &buf, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_u32(buf, bits);
}

void set_u32(std::vector<uint8_t> &buf, size_t off, uint32_t v) {
    buf[off + 0] = (uint8_t)(v & 0xFF);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    buf[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    buf[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}

// Length of a NUL-terminated name, capped at max.
size_t name_len(const char *name, size_t max) {
    size_t n = 0;
    while (n < max && name[n]) ++n;
    return n;
}

}  // namespace

extern "C" void bad_write_options_default(BadWriteOptions *opts) {
    if (!opts) return;
    opts->header_size = 80;
    opts->unknown1 = 0;
    opts->num_event_blocks = 0;
    opts->unknown3 = 8;
    opts->bone_stride = 100;
    opts->frame_rec_stride = 12;
    opts->rot_stride = 16;
    opts->pos_offsets_frame_len = 1;
    opts->unknown6 = 1;
    opts->unknown7 = 0;
    opts->unknown8 = 0;
    opts->zero_bn01_position = 0;
    opts->write_bone_index_byte = 1;
}

extern "C" int bad_write_buffer(const BadFile *bf, const BadWriteOptions *opts,
                                uint8_t **out, size_t *out_size) {
    if (!out || !out_size) return -1;
    *out = NULL;
    *out_size = 0;
    if (!bf) return -1;

    BadWriteOptions defaults;
    if (!opts) {
        bad_write_options_default(&defaults);
        opts = &defaults;
    }

    const uint32_t bone_count = bf->bone_count;
    const uint32_t bone_stride = opts->bone_stride ? opts->bone_stride : 100;
    const uint32_t rot_stride = opts->rot_stride ? opts->rot_stride : 16;
    const uint32_t trans_stride = opts->frame_rec_stride ? opts->frame_rec_stride : 12;

    std::vector<uint8_t> buf;

    // Header placeholder (patched at the end).
    buf.resize(kHeaderBytes, 0);
    const uint32_t frame_table_offset = (uint32_t)buf.size();  // == 80 (channels_offset)

    // Frame (channel) table placeholder.
    buf.resize(buf.size() + (size_t)bone_count * 12, 0);

    // The BAD header stores only bone_count; the parser always reconstructs
    // num_channels == bone_count and reads bone_count*frame_count translations.
    // Reject inputs that would make the parser read past what we emit. (A
    // larger num_translations is fine: stock files carry a terminal-duplicate
    // frame the parser ignores.)
    if ((bf->flags & 2u) != 0 && bf->translations &&
        bf->num_translations < (size_t)bone_count * bf->frame_count) {
        return -1;
    }

    // Per-channel keyframe data. Missing channels (num_channels < bone_count)
    // are emitted as empty (frame_count 0) so the channel table stays aligned
    // with the bone table; the parser will reconstruct them as zero-frame.
    std::vector<uint32_t> chan_frames(bone_count, 0);
    std::vector<uint32_t> frame_len_ptr(bone_count, 0);
    std::vector<uint32_t> rot_ptr(bone_count, 0);

    for (uint32_t i = 0; i < bone_count; ++i) {
        const BadChannel *ch = (i < bf->num_channels && bf->channels) ? &bf->channels[i] : NULL;
        const uint32_t nf = ch ? ch->frame_count : 0;
        chan_frames[i] = nf;

        frame_len_ptr[i] = (uint32_t)buf.size();
        for (uint32_t j = 0; j < nf; ++j) {
            const uint16_t fl = (ch && ch->frame_lengths) ? ch->frame_lengths[j] : (uint16_t)1;
            put_u16(buf, fl);
        }
        // Pad frame_lengths to 4-byte alignment before the rotation block.
        while (buf.size() % 4 != 0) buf.push_back(0);

        rot_ptr[i] = (uint32_t)buf.size();
        for (uint32_t j = 0; j < nf; ++j) {
            float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
            if (ch && ch->rotations) {
                x = ch->rotations[j].x;
                y = ch->rotations[j].y;
                z = ch->rotations[j].z;
                w = ch->rotations[j].w;
            }
            put_f32(buf, x);
            put_f32(buf, y);
            put_f32(buf, z);
            put_f32(buf, w);
            for (uint32_t p = 16; p < rot_stride; ++p) buf.push_back(0);  // pad to rot_stride
        }
    }

    // Events.
    uint32_t event_table_offset = 0;
    const uint32_t num_events = (uint32_t)bf->num_events;
    if (num_events > 0 && bf->events) {
        event_table_offset = (uint32_t)buf.size();
        for (uint32_t i = 0; i < num_events; ++i) {
            const BadEvent *ev = &bf->events[i];
            put_f32(buf, ev->velocity[0]);
            put_f32(buf, ev->velocity[1]);
            put_f32(buf, ev->velocity[2]);
            put_f32(buf, ev->bottom);
            put_f32(buf, ev->top);
            if (bf->version == 1) put_s32(buf, ev->trigger);  // v1 events are 24 bytes
        }
    }

    // Bone table.
    const uint32_t bones_offset = (uint32_t)buf.size();
    for (uint32_t i = 0; i < bone_count; ++i) {
        const BadBone *b = &bf->bones[i];
        const size_t bone_start = buf.size();

        // name[32]: up to 31 chars + NUL, zero-padded.
        const size_t nlen = name_len(b->name, 31);
        for (size_t k = 0; k < nlen; ++k) buf.push_back((uint8_t)b->name[k]);
        for (size_t k = nlen; k < 32; ++k) buf.push_back(0);

        // 3 zero bytes + per-bone index byte (parser skips +32..+35).
        buf.push_back(0);
        buf.push_back(0);
        buf.push_back(0);
        buf.push_back(opts->write_bone_index_byte ? (uint8_t)(i & 0xFF) : (uint8_t)0);

        // Children (recomputed from parent_index links).
        int32_t num_children = 0;
        int32_t first_child = -1;
        for (uint32_t c = 0; c < bone_count; ++c) {
            if (c != i && bf->bones[c].parent_index == (int32_t)i) {
                if (first_child < 0) first_child = (int32_t)c;
                ++num_children;
            }
        }
        put_s32(buf, num_children);
        put_s32(buf, first_child >= 0
                         ? (int32_t)(bones_offset + (uint32_t)first_child * bone_stride)
                         : -1);

        // Parent address: 0 == no parent (parser convention); else absolute addr.
        const int32_t parent_index = b->parent_index;
        if (i == 0 || parent_index < 0) {
            put_s32(buf, 0);
        } else {
            put_s32(buf, (int32_t)(bones_offset + (uint32_t)parent_index * bone_stride));
        }

        put_f32(buf, b->length);

        float px = b->position[0], py = b->position[1], pz = b->position[2];
        if (opts->zero_bn01_position && i == 0) {
            px = py = pz = 0.0f;
        }
        put_f32(buf, px);
        put_f32(buf, py);
        put_f32(buf, pz);

        for (int r = 0; r < 9; ++r) put_f32(buf, b->rotation[r]);

        while (buf.size() - bone_start < bone_stride) buf.push_back(0);  // pad to bone_stride
    }

    // Translation block (only when flags & 2). Placed immediately after the bone
    // table, i.e. at bones_offset + bone_count*bone_stride — exactly where the
    // parser looks. Emitted frame-major as the caller laid them out.
    if ((bf->flags & 2u) != 0 && bf->num_translations > 0 && bf->translations) {
        for (size_t idx = 0; idx < bf->num_translations; ++idx) {
            put_f32(buf, bf->translations[idx][0]);
            put_f32(buf, bf->translations[idx][1]);
            put_f32(buf, bf->translations[idx][2]);
            for (uint32_t p = 12; p < trans_stride; ++p) buf.push_back(0);
        }
    }

    // Patch the frame (channel) table.
    for (uint32_t i = 0; i < bone_count; ++i) {
        const size_t off = (size_t)frame_table_offset + (size_t)i * 12;
        set_u32(buf, off + 0, chan_frames[i]);
        set_u32(buf, off + 4, frame_len_ptr[i]);
        set_u32(buf, off + 8, rot_ptr[i]);
    }

    // Patch the header.
    set_u32(buf, 0x00, bf->version);
    set_u32(buf, 0x04, opts->header_size);
    set_u32(buf, 0x08, bf->fps);
    set_u32(buf, 0x0C, bf->frame_count);
    set_u32(buf, 0x10, bf->flags);
    set_u32(buf, 0x14, bone_count);
    set_u32(buf, 0x18, bones_offset);
    set_u32(buf, 0x1C, frame_table_offset);
    set_u32(buf, 0x20, opts->unknown1);
    set_u32(buf, 0x24, opts->num_event_blocks);
    set_u32(buf, 0x28, opts->unknown3);
    set_u32(buf, 0x2C, bone_stride);
    set_u32(buf, 0x30, opts->frame_rec_stride);
    set_u32(buf, 0x34, opts->rot_stride);
    set_u32(buf, 0x38, opts->pos_offsets_frame_len);
    set_u32(buf, 0x3C, num_events);
    set_u32(buf, 0x40, event_table_offset);
    set_u32(buf, 0x44, opts->unknown6);
    set_u32(buf, 0x48, opts->unknown7);
    set_u32(buf, 0x4C, opts->unknown8);

    uint8_t *result = (uint8_t *)malloc(buf.size() ? buf.size() : 1);
    if (!result) return -1;
    memcpy(result, buf.data(), buf.size());
    *out = result;
    *out_size = buf.size();
    return 0;
}

extern "C" int bad_write(const char *path, const BadFile *bf, const BadWriteOptions *opts) {
    if (!path || !bf) return -1;
    uint8_t *data = NULL;
    size_t size = 0;
    if (bad_write_buffer(bf, opts, &data, &size) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(data);
        return -1;
    }
    const size_t written = (size > 0) ? fwrite(data, 1, size, f) : 0;
    fclose(f);
    free(data);
    return (written == size) ? 0 : -1;
}

extern "C" void bad_write_buffer_free(uint8_t *buf) {
    free(buf);
}
