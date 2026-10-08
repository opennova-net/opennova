// Field equality of two parsed .bad clips: every field the reader keeps, floats
// bit for bit, except the bone table's child/parent addresses (the writer
// recomputes them from parent_index; retail's are stale and the runtime reads
// neither) and the channel offsets (layout). Infrastructure only.
#pragma once

#include <formats/bad/bad.h>

#include <cstring>

namespace bad_equal {

inline bool same_float(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

inline bool field_equal(const opennova::bad::BadFile &a, const opennova::bad::BadFile &b) {
    using namespace opennova::bad;
    if (a.version != b.version || a.header_size != b.header_size || a.fps != b.fps ||
        a.frame_count != b.frame_count || a.flags != b.flags || a.bone_count != b.bone_count ||
        a.num_bones != b.num_bones || a.num_channels != b.num_channels || a.num_events != b.num_events ||
        a.num_translations != b.num_translations) {
        return false;
    }
    for (size_t i = 0; i < a.num_bones; ++i) {
        const BadBone &x = a.bones[i];
        const BadBone &y = b.bones[i];
        if (std::strcmp(x.name, y.name) != 0 || x.parent_index != y.parent_index ||
            x.num_children != y.num_children || !same_float(x.length, y.length)) {
            return false;
        }
        for (int k = 0; k < 3; ++k)
            if (!same_float(x.position[k], y.position[k])) return false;
        for (int k = 0; k < 9; ++k)
            if (!same_float(x.rotation[k], y.rotation[k])) return false;
    }
    for (size_t i = 0; i < a.num_channels; ++i) {
        const BadChannel &x = a.channels[i];
        const BadChannel &y = b.channels[i];
        if (x.frame_count != y.frame_count) return false;
        for (uint32_t k = 0; k < x.frame_count; ++k) {
            if (x.frame_lengths[k] != y.frame_lengths[k]) return false;
            if (!same_float(x.rotations[k].x, y.rotations[k].x) || !same_float(x.rotations[k].y, y.rotations[k].y) ||
                !same_float(x.rotations[k].z, y.rotations[k].z) || !same_float(x.rotations[k].w, y.rotations[k].w)) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < a.num_events; ++i) {
        const BadEvent &x = a.events[i];
        const BadEvent &y = b.events[i];
        for (int k = 0; k < 3; ++k)
            if (!same_float(x.velocity[k], y.velocity[k])) return false;
        if (!same_float(x.bottom, y.bottom) || !same_float(x.top, y.top) || x.trigger != y.trigger) return false;
    }
    for (size_t i = 0; i < a.num_translations; ++i)
        for (int k = 0; k < 3; ++k)
            if (!same_float(a.translations[i][k], b.translations[i][k])) return false;
    return true;
}

} // namespace bad_equal
