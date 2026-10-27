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

// The fixed row sizes the game walks a clip by: 100-byte bone rows, 12-byte channel rows,
// 16-byte rotation keys (a channel's key count is the first 16-bit word of its row) and
// 12-byte translation rows, bone_count of them a frame. The loader reads none of the header
// words +0x2C / +0x30 / +0x34; every retail clip holds 100 / 12 / 16 there, and the writer
// stores the same.
// [orig: BoneFile_Load @0x40FFF0 - the bone walk `add edi, 64h` @0x4100FB, the channel walk
//  `add eax, 0Ch` @0x410128; BoneAnim_FindKeyframeAtTime @0x410220 - the key count `movzx ebx,
//  word ptr [edi]` @0x41024F, the key step `add eax, 10h` @0x410284; BoneAnim_TransformBones
//  @0x410360 - the next translation row at +12 * bone_count @0x4104A0..0x4104A5]
inline constexpr uint32_t kBadBoneRowBytes = 100;
inline constexpr uint32_t kBadChannelRowBytes = 12;
inline constexpr uint32_t kBadRotationKeyBytes = 16;
inline constexpr uint32_t kBadTranslationRowBytes = 12;

// The largest clip file the game loads: a bigger one does not load, so a table's token
// plays failsafe.bad in its place (anim::adm_token_clip). The game's loads by name apply it
// (assets::AssetStore::bone_animation); the parser reads any size.
// [orig: BoneFile_Load @0x40FFF0 - `cmp edi, 7A120h; jbe` @0x41005A..0x410060]
inline constexpr size_t kBadLoadMaxBytes = 0x7A120;

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
