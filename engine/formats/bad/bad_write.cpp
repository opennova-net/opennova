// BAD skeletal-animation writer -- the inverse of bad_parse_buffer (bad.cpp).
// [orig: BoneFile_Load @0x40fff0 -- header dword table and the relocation of
//  every file-relative offset; the layout is the loader's, retail ships no writer]

#include <formats/bad/bad_write.h>

#include <base/io/le.h>

#include <cstdio>
#include <cstring>

namespace opennova::bad {

namespace {

constexpr uint32_t kHeaderBytes = 80;
constexpr uint32_t kChannelRowBytes = 12;
constexpr uint32_t kBoneStride = 100;
constexpr uint32_t kTranslationStride = 12;
constexpr uint32_t kRotationStride = 16;
// The header words the reader never names, at the values the retail corpus
// ships (rAKM_RST, DT1RST, DT1runF, BINOC: every clip inspected).
constexpr uint32_t kHeaderWord8 = 0;
constexpr uint32_t kHeaderWord9 = 0;
constexpr uint32_t kHeaderWord10 = 8;
constexpr uint32_t kHeaderWord14 = 1;
constexpr uint32_t kHeaderWord17 = 1;
constexpr uint32_t kHeaderWord18 = 0;
constexpr uint32_t kHeaderWord19 = 0;

void put_u32(std::vector<uint8_t> &buf, uint32_t v) {
    uint8_t bytes[4];
    opennova::io::write_u32_le(bytes, v);
    buf.insert(buf.end(), bytes, bytes + 4);
}

void put_s32(std::vector<uint8_t> &buf, int32_t v) { put_u32(buf, static_cast<uint32_t>(v)); }

void put_u16(std::vector<uint8_t> &buf, uint16_t v) {
    uint8_t bytes[2];
    opennova::io::write_u16_le(bytes, v);
    buf.insert(buf.end(), bytes, bytes + 2);
}

void put_f32(std::vector<uint8_t> &buf, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(buf, bits);
}

void set_u32(std::vector<uint8_t> &buf, size_t off, uint32_t v) {
    opennova::io::write_u32_le(buf.data() + off, v);
}

void pad_to(std::vector<uint8_t> &buf, size_t alignment) {
    while (buf.size() % alignment != 0) buf.push_back(0);
}

} // namespace

int bad_write_buffer(const BadFile *bf, std::vector<uint8_t> &out) {
    out.clear();
    if (bf == nullptr) return -1;
    const uint32_t bone_count = bf->bone_count;
    if (bone_count > 0 && (bf->bones == nullptr || bf->num_bones < bone_count)) return -1;
    if (bone_count > 0 && (bf->channels == nullptr || bf->num_channels < bone_count)) return -1;
    const bool translated = (bf->flags & 2u) != 0;
    const size_t translation_rows = static_cast<size_t>(bone_count) * bf->frame_count;
    if (translated && translation_rows > 0 &&
        (bf->translations == nullptr || bf->num_translations < translation_rows)) {
        return -1;
    }

    std::vector<uint8_t> buf;
    buf.resize(kHeaderBytes, 0);
    const uint32_t channels_offset = static_cast<uint32_t>(buf.size());
    buf.resize(buf.size() + static_cast<size_t>(bone_count) * kChannelRowBytes, 0);

    // Per-channel key data: the u16 duration table, padded to 4, then the quaternions.
    std::vector<uint32_t> frame_lengths_offset(bone_count, 0);
    std::vector<uint32_t> rotations_offset(bone_count, 0);
    for (uint32_t i = 0; i < bone_count; ++i) {
        const BadChannel &ch = bf->channels[i];
        frame_lengths_offset[i] = static_cast<uint32_t>(buf.size());
        for (uint32_t k = 0; k < ch.frame_count; ++k) {
            put_u16(buf, ch.frame_lengths != nullptr ? ch.frame_lengths[k] : static_cast<uint16_t>(1));
        }
        pad_to(buf, 4);
        rotations_offset[i] = static_cast<uint32_t>(buf.size());
        for (uint32_t k = 0; k < ch.frame_count; ++k) {
            const BadQuaternion q = ch.rotations != nullptr ? ch.rotations[k] : BadQuaternion{0.0f, 0.0f, 0.0f, 1.0f};
            put_f32(buf, q.x);
            put_f32(buf, q.y);
            put_f32(buf, q.z);
            put_f32(buf, q.w);
        }
    }

    // Events: version 1 rows carry the trigger word.
    const uint32_t event_count = static_cast<uint32_t>(bf->num_events);
    uint32_t events_offset = 0;
    if (event_count > 0 && bf->events != nullptr) {
        events_offset = static_cast<uint32_t>(buf.size());
        for (uint32_t i = 0; i < event_count; ++i) {
            const BadEvent &ev = bf->events[i];
            put_f32(buf, ev.velocity[0]);
            put_f32(buf, ev.velocity[1]);
            put_f32(buf, ev.velocity[2]);
            put_f32(buf, ev.bottom);
            put_f32(buf, ev.top);
            if (bf->version == 1) put_s32(buf, ev.trigger);
        }
    }

    // Bone table: absolute child/parent addresses recomputed from parent_index.
    const uint32_t bones_offset = static_cast<uint32_t>(buf.size());
    for (uint32_t i = 0; i < bone_count; ++i) {
        const BadBone &b = bf->bones[i];
        const size_t start = buf.size();
        size_t name_len = 0;
        while (name_len < 31 && b.name[name_len] != '\0') ++name_len;
        buf.insert(buf.end(), b.name, b.name + name_len);
        buf.resize(start + 32, 0);
        buf.push_back(0);
        buf.push_back(0);
        buf.push_back(0);
        buf.push_back(static_cast<uint8_t>(i & 0xFFu));
        int32_t num_children = 0;
        int32_t first_child = -1;
        for (uint32_t c = 0; c < bone_count; ++c) {
            if (c != i && bf->bones[c].parent_index == static_cast<int32_t>(i)) {
                if (first_child < 0) first_child = static_cast<int32_t>(c);
                ++num_children;
            }
        }
        put_s32(buf, num_children);
        // A leaf's child address is 0, as retail's rows carry it (BINOC.bad bone 18).
        put_s32(buf, first_child >= 0
                         ? static_cast<int32_t>(bones_offset + static_cast<uint32_t>(first_child) * kBoneStride)
                         : 0);
        const bool root = i == 0 || b.parent_index < 0 || static_cast<uint32_t>(b.parent_index) >= bone_count;
        put_s32(buf, root ? 0
                          : static_cast<int32_t>(bones_offset + static_cast<uint32_t>(b.parent_index) * kBoneStride));
        put_f32(buf, b.length);
        put_f32(buf, b.position[0]);
        put_f32(buf, b.position[1]);
        put_f32(buf, b.position[2]);
        for (int r = 0; r < 9; ++r) put_f32(buf, b.rotation[r]);
        buf.resize(start + kBoneStride, 0);
    }

    // Translations follow the bone table (where the reader looks), frame-major.
    if (translated) {
        for (size_t row = 0; row < translation_rows; ++row) {
            put_f32(buf, bf->translations[row][0]);
            put_f32(buf, bf->translations[row][1]);
            put_f32(buf, bf->translations[row][2]);
        }
    }

    for (uint32_t i = 0; i < bone_count; ++i) {
        const size_t row = static_cast<size_t>(channels_offset) + static_cast<size_t>(i) * kChannelRowBytes;
        set_u32(buf, row + 0, bf->channels[i].frame_count);
        set_u32(buf, row + 4, frame_lengths_offset[i]);
        set_u32(buf, row + 8, rotations_offset[i]);
    }

    set_u32(buf, 0x00, bf->version);
    set_u32(buf, 0x04, bf->header_size != 0 ? bf->header_size : kHeaderBytes);
    set_u32(buf, 0x08, bf->fps);
    set_u32(buf, 0x0C, bf->frame_count);
    set_u32(buf, 0x10, bf->flags);
    set_u32(buf, 0x14, bone_count);
    set_u32(buf, 0x18, bones_offset);
    set_u32(buf, 0x1C, channels_offset);
    set_u32(buf, 0x20, kHeaderWord8);
    set_u32(buf, 0x24, kHeaderWord9);
    set_u32(buf, 0x28, kHeaderWord10);
    set_u32(buf, 0x2C, kBoneStride);
    set_u32(buf, 0x30, kTranslationStride);
    set_u32(buf, 0x34, kRotationStride);
    set_u32(buf, 0x38, kHeaderWord14);
    set_u32(buf, 0x3C, event_count);
    set_u32(buf, 0x40, events_offset);
    set_u32(buf, 0x44, kHeaderWord17);
    set_u32(buf, 0x48, kHeaderWord18);
    set_u32(buf, 0x4C, kHeaderWord19);

    out.swap(buf);
    return 0;
}

int bad_write(const char *path, const BadFile *bf) {
    if (path == nullptr) return -1;
    std::vector<uint8_t> bytes;
    if (bad_write_buffer(bf, bytes) != 0) return -1;
    FILE *f = std::fopen(path, "wb");
    if (f == nullptr) return -1;
    const size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return written == bytes.size() ? 0 : -1;
}

} // namespace opennova::bad
