// BAD skeletal-animation parser. Runtime reads only.
#ifndef BAD_H
#define BAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BadBone {
    char name[33];
    int32_t num_children;
    int32_t child_offset;
    int32_t parent_offset;
    int32_t parent_index;
    float length;
    float position[3];
    float rotation[9];
} BadBone;

typedef struct BadQuaternion { float x, y, z, w; } BadQuaternion;

typedef struct BadChannel {
    uint32_t frame_count;
    uint32_t frame_lengths_offset;
    uint32_t rotations_offset;
    uint16_t *frame_lengths;
    BadQuaternion *rotations;
} BadChannel;

typedef struct BadEvent {
    float velocity[3];
    float bottom;
    float top;
    int32_t trigger;
} BadEvent;

typedef struct BadFile {
    uint32_t version;
    uint32_t header_size;
    uint32_t fps;
    uint32_t frame_count;
    uint32_t flags;
    uint32_t bone_count;
    BadBone *bones;
    size_t num_bones;
    BadChannel *channels;
    size_t num_channels;
    BadEvent *events;
    size_t num_events;
    float (*translations)[3];
    size_t num_translations;
} BadFile;

int bad_parse(const char *path, BadFile *out);
int bad_parse_buffer(const uint8_t *data, size_t data_size, BadFile *out);
void bad_free(BadFile *bf);

#ifdef __cplusplus
}
#endif

#endif // BAD_H
