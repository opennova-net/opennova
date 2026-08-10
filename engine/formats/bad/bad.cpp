// BAD animation file parser — pure C implementation.
// Reads the binary format directly into flat BadFile structs.

#include "bad/bad.h"

#include <io/le.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static int in_range(size_t offset, size_t size_needed, size_t buffer_size) {
    if (offset > buffer_size) return 0;
    return buffer_size - offset >= size_needed;
}

static uint32_t read_u32(const uint8_t *buf, size_t off) {
    return opennova::io::read_u32_le(buf + off);
}

static int32_t read_s32(const uint8_t *buf, size_t off) {
    return opennova::io::read_s32_le(buf + off);
}

static uint16_t read_u16(const uint8_t *buf, size_t off) {
    return opennova::io::read_u16_le(buf + off);
}

static float read_f32(const uint8_t *buf, size_t off) {
    return opennova::io::read_f32_le(buf + off);
}

// --------------------------------------------------------------------------
// On-disk channel layout (12 bytes)
// --------------------------------------------------------------------------

typedef struct {
    uint32_t num_frames;
    uint32_t frame_len_off;
    uint32_t rotation_off;
} ChannelDisk;

#define CHANNEL_DISK_SIZE 12

// --------------------------------------------------------------------------
// API
// --------------------------------------------------------------------------

// Parse a BAD file already resident in memory. Does NOT take ownership of `data`
// (the caller frees it) -- this is the entry the VFS path uses, since assets live
// in PFF archives, not on disk.
int bad_parse_buffer(const uint8_t *data, size_t data_size, BadFile *out) {
    uint32_t bone_stride, rot_stride, trans_stride;
    uint32_t event_stride;
    uint32_t i;

    if (!data || !out) return -1;
    memset(out, 0, sizeof(BadFile));

    if (data_size < 0x50) return -1;

    // Parse header
    out->version     = read_u32(data, 0x00);
    out->header_size = read_u32(data, 0x04);
    out->fps         = read_u32(data, 0x08);
    out->frame_count = read_u32(data, 0x0C);
    out->flags       = read_u32(data, 0x10);
    out->bone_count  = read_u32(data, 0x14);

    {
        uint32_t bones_offset    = read_u32(data, 0x18);
        uint32_t channels_offset = read_u32(data, 0x1C);

        bone_stride  = read_u32(data, 0x2C);
        if (bone_stride == 0) bone_stride = 100;
        rot_stride   = read_u32(data, 0x34);
        if (rot_stride == 0) rot_stride = 16;
        trans_stride = read_u32(data, 0x30);
        if (trans_stride == 0) trans_stride = 12;

        // Bounds check channel and bone tables
        if (!in_range(channels_offset, (size_t)out->bone_count * CHANNEL_DISK_SIZE, data_size)) {
            return -1;
        }
        if (!in_range(bones_offset, (size_t)out->bone_count * bone_stride, data_size)) {
            return -1;
        }

        // ----- Channels -----
        out->num_channels = out->bone_count;
        if (out->num_channels > 0) {
            out->channels = (BadChannel *)calloc(out->num_channels, sizeof(BadChannel));
            if (!out->channels) { bad_free(out); return -1; }

            for (i = 0; i < out->bone_count; ++i) {
                size_t off = channels_offset + (size_t)i * CHANNEL_DISK_SIZE;
                uint32_t num_frames   = read_u32(data, off);
                uint32_t frame_len_off = read_u32(data, off + 4);
                uint32_t rotation_off  = read_u32(data, off + 8);

                out->channels[i].frame_count          = num_frames;
                out->channels[i].frame_lengths_offset  = frame_len_off;
                out->channels[i].rotations_offset      = rotation_off;
                out->channels[i].frame_lengths         = NULL;
                out->channels[i].rotations             = NULL;

                if (num_frames == 0) continue;

                // Frame lengths
                {
                    size_t fl_size = (size_t)num_frames * sizeof(uint16_t);
                    if (!in_range(frame_len_off, fl_size, data_size)) {
                        bad_free(out); return -1;
                    }
                    out->channels[i].frame_lengths = (uint16_t *)malloc(fl_size);
                    if (!out->channels[i].frame_lengths) {
                        bad_free(out); return -1;
                    }
                    {
                        uint32_t j;
                        for (j = 0; j < num_frames; ++j) {
                            out->channels[i].frame_lengths[j] =
                                read_u16(data, frame_len_off + j * sizeof(uint16_t));
                        }
                    }
                }

                // Rotations
                {
                    size_t rot_size = (size_t)num_frames * rot_stride;
                    if (!in_range(rotation_off, rot_size, data_size)) {
                        bad_free(out); return -1;
                    }
                    out->channels[i].rotations = (BadQuaternion *)malloc(
                        (size_t)num_frames * sizeof(BadQuaternion));
                    if (!out->channels[i].rotations) {
                        bad_free(out); return -1;
                    }
                    {
                        uint32_t j;
                        for (j = 0; j < num_frames; ++j) {
                            size_t r_off = rotation_off + (size_t)j * rot_stride;
                            out->channels[i].rotations[j].x = read_f32(data, r_off + 0);
                            out->channels[i].rotations[j].y = read_f32(data, r_off + 4);
                            out->channels[i].rotations[j].z = read_f32(data, r_off + 8);
                            out->channels[i].rotations[j].w = read_f32(data, r_off + 12);
                        }
                    }
                }
            }
        }

        // ----- Bones -----
        out->num_bones = out->bone_count;
        if (out->num_bones > 0) {
            out->bones = (BadBone *)calloc(out->num_bones, sizeof(BadBone));
            if (!out->bones) { bad_free(out); return -1; }

            for (i = 0; i < out->bone_count; ++i) {
                size_t off = bones_offset + (size_t)i * bone_stride;
                BadBone *b = &out->bones[i];
                size_t name_len;
                int r, c;

                memset(b->name, 0, sizeof(b->name));
                memcpy(b->name, &data[off], 32);
                // Determine actual name length
                name_len = 0;
                while (name_len < 32 && b->name[name_len]) ++name_len;
                b->name[name_len] = '\0';

                // Skip 4 unknown bytes at offset +32..+35
                b->num_children  = read_s32(data, off + 36);
                b->child_offset  = read_s32(data, off + 40);
                b->parent_offset = read_s32(data, off + 44);
                b->length        = read_f32(data, off + 48);
                b->position[0]   = read_f32(data, off + 52);
                b->position[1]   = read_f32(data, off + 56);
                b->position[2]   = read_f32(data, off + 60);
                for (r = 0; r < 3; ++r) {
                    for (c = 0; c < 3; ++c) {
                        b->rotation[r * 3 + c] =
                            read_f32(data, off + 64 + (size_t)(r * 3 + c) * sizeof(float));
                    }
                }

                if (i == 0) {
                    b->parent_index = -1;
                } else {
                    b->parent_index =
                        b->parent_offset == 0
                            ? -1
                            : (int32_t)((int32_t)(b->parent_offset) -
                                        (int32_t)(bones_offset)) /
                              (int32_t)(bone_stride);
                }
            }
        }

        // ----- Events -----
        {
            uint32_t event_count        = read_u32(data, 0x3C);
            uint32_t event_table_offset = read_u32(data, 0x40);
            event_stride = out->version == 1 ? 24u : 20u;

            out->num_events = event_count;
            if (event_count > 0) {
                if (!in_range(event_table_offset,
                              (size_t)event_count * event_stride, data_size)) {
                    bad_free(out); return -1;
                }
                out->events = (BadEvent *)malloc(
                    (size_t)event_count * sizeof(BadEvent));
                if (!out->events) { bad_free(out); return -1; }

                for (i = 0; i < event_count; ++i) {
                    size_t off = event_table_offset + (size_t)i * event_stride;
                    out->events[i].velocity[0] = read_f32(data, off + 0);
                    out->events[i].velocity[1] = read_f32(data, off + 4);
                    out->events[i].velocity[2] = read_f32(data, off + 8);
                    out->events[i].bottom      = read_f32(data, off + 12);
                    out->events[i].top         = read_f32(data, off + 16);
                    out->events[i].trigger     =
                        out->version == 1 ? read_s32(data, off + 20) : -1;
                }
            }
        }

        // ----- Translations (optional, when flags & 2) -----
        if ((out->flags & 2u) != 0) {
            size_t trans_off = bones_offset + (size_t)out->bone_count * bone_stride;
            size_t sample_count = (size_t)out->bone_count * out->frame_count;
            size_t expected = sample_count * trans_stride;

            if (!in_range(trans_off, expected, data_size)) {
                bad_free(out); return -1;
            }
            out->num_translations = sample_count;
            out->translations = (float (*)[3])malloc(sample_count * 3 * sizeof(float));
            if (!out->translations) { bad_free(out); return -1; }

            {
                size_t idx;
                for (idx = 0; idx < sample_count; ++idx) {
                    size_t base = trans_off + idx * trans_stride;
                    out->translations[idx][0] = read_f32(data, base + 0);
                    out->translations[idx][1] = read_f32(data, base + 4);
                    out->translations[idx][2] = read_f32(data, base + 8);
                }
            }
        }
    }

    return 0;
}

int bad_parse(const char *path, BadFile *out) {
    FILE *f;
    long file_len;
    uint8_t *data;
    size_t data_size;
    int rc;

    if (!path || !out) return -1;

    f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    file_len = ftell(f);
    if (file_len < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    data_size = (size_t)file_len;

    data = (uint8_t *)malloc(data_size ? data_size : 1);
    if (!data) { fclose(f); return -1; }
    if (data_size && fread(data, 1, data_size, f) != data_size) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    rc = bad_parse_buffer(data, data_size, out);
    free(data);
    return rc;
}

void bad_free(BadFile *bf) {
    size_t i;
    if (!bf) return;

    if (bf->channels) {
        for (i = 0; i < bf->num_channels; ++i) {
            free(bf->channels[i].frame_lengths);
            free(bf->channels[i].rotations);
        }
        free(bf->channels);
    }

    free(bf->bones);
    free(bf->events);
    free(bf->translations);

    memset(bf, 0, sizeof(BadFile));
}
