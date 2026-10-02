// BAD skeletal-animation parser. The writer is bad_write.h, the construction
// seam bad_build.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace opennova::bad {

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

// Whether bone `index`'s parent comes before it: a root's parent is -1, any other
// bone's a lower index. The rig's forward kinematics walks the bones in index
// order and builds each on its parent's pose, so a parent numbered after its
// child is not built yet when the child reads it [orig: BoneAnim_BuildWorldMatrices
// @0x40C400, the in-order walk @0x40C5EF..0x40C731, the parent's matrix read
// @0x40C674] (retail then poses the child without its ancestors' motion).
// runtime/anim's SkeletalClips accumulates its rest globals the same way and
// poses a rig only when every bone passes (fk_valid); the editor's clip
// validator reports a bone that does not.
inline bool bad_parent_in_order(int32_t parent_index, size_t index) {
    return parent_index == -1 || (parent_index >= 0 && static_cast<size_t>(parent_index) < index);
}

} // namespace opennova::bad
