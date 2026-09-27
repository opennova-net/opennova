// The BAD clip construction seam (bad_build.h): the frame maps, the derived
// bone table, and the assembly the writer serializes.

#include <formats/bad/bad_build.h>

#include <formats/adm/adm.h>
#include <formats/bad/bad_write.h>
#include <formats/threedi/threedi_build.h>

#include <base/io/strutil.h>
#include <base/io/tick_rate.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace opennova::bad {

namespace {

// Row-major 3x3 in doubles; the file stores the same layout in floats.
struct Mat3 {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

Mat3 transpose(const Mat3 &a) {
    Mat3 t{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) t.m[r * 3 + c] = a.m[c * 3 + r];
    return t;
}

Mat3 rows_of(const BadBuildQuat &q) {
    Mat3 o{};
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    o.m[0] = 1.0 - 2.0 * (y * y + z * z);
    o.m[1] = 2.0 * (x * y - z * w);
    o.m[2] = 2.0 * (x * z + y * w);
    o.m[3] = 2.0 * (x * y + z * w);
    o.m[4] = 1.0 - 2.0 * (x * x + z * z);
    o.m[5] = 2.0 * (y * z - x * w);
    o.m[6] = 2.0 * (x * z - y * w);
    o.m[7] = 2.0 * (y * z + x * w);
    o.m[8] = 1.0 - 2.0 * (x * x + y * y);
    return o;
}

// Shepperd's branch: pick the largest diagonal term so no square root of a
// value near zero decides the result.
BadBuildQuat quat_of(const Mat3 &r) {
    const double trace = r.m[0] + r.m[4] + r.m[8];
    BadBuildQuat q{};
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q.w = 0.25 * s;
        q.x = (r.m[7] - r.m[5]) / s;
        q.y = (r.m[2] - r.m[6]) / s;
        q.z = (r.m[3] - r.m[1]) / s;
    } else if (r.m[0] > r.m[4] && r.m[0] > r.m[8]) {
        const double s = std::sqrt(1.0 + r.m[0] - r.m[4] - r.m[8]) * 2.0;
        q.w = (r.m[7] - r.m[5]) / s;
        q.x = 0.25 * s;
        q.y = (r.m[1] + r.m[3]) / s;
        q.z = (r.m[2] + r.m[6]) / s;
    } else if (r.m[4] > r.m[8]) {
        const double s = std::sqrt(1.0 + r.m[4] - r.m[0] - r.m[8]) * 2.0;
        q.w = (r.m[2] - r.m[6]) / s;
        q.x = (r.m[1] + r.m[3]) / s;
        q.y = 0.25 * s;
        q.z = (r.m[5] + r.m[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + r.m[8] - r.m[0] - r.m[4]) * 2.0;
        q.w = (r.m[3] - r.m[1]) / s;
        q.x = (r.m[2] + r.m[6]) / s;
        q.y = (r.m[5] + r.m[7]) / s;
        q.z = 0.25 * s;
    }
    return q;
}

BadBuildVec3 apply(const Mat3 &r, const BadBuildVec3 &v) {
    return BadBuildVec3{r.m[0] * v.x + r.m[1] * v.y + r.m[2] * v.z,
                        r.m[3] * v.x + r.m[4] * v.y + r.m[5] * v.z,
                        r.m[6] * v.x + r.m[7] * v.y + r.m[8] * v.z};
}

Mat3 rows_from_floats(const float rows[9]) {
    Mat3 o{};
    for (int i = 0; i < 9; ++i) o.m[i] = static_cast<double>(rows[i]);
    return o;
}

bool fail(std::string *error, const std::string &what) {
    if (error != nullptr) *error = what;
    return false;
}

// The key as the file will hold it: every derivation reads these floats, not
// the author's doubles, so build(scene(x)) re-derives the same bytes.
BadQuaternion stored_key(const BadBuildQuat &mission) {
    const BadBuildQuat c = bad_clip_from_mission(mission);
    BadQuaternion q{};
    q.x = static_cast<float>(c.x);
    q.y = static_cast<float>(c.y);
    q.z = static_cast<float>(c.z);
    q.w = static_cast<float>(c.w);
    return q;
}

BadBuildQuat quat_from_stored(const BadQuaternion &q) {
    return BadBuildQuat{static_cast<double>(q.x), static_cast<double>(q.y), static_cast<double>(q.z),
                        static_cast<double>(q.w)};
}

} // namespace

BadBuildVec3 bad_clip_from_mission(const BadBuildVec3 &m) {
    const threedi::ThreediBuildVec3 c =
            threedi::threedi_mission_to_presentation(threedi::ThreediBuildVec3{m.x, m.y, m.z});
    return BadBuildVec3{c.x, c.y, c.z};
}

BadBuildVec3 bad_mission_from_clip(const BadBuildVec3 &c) {
    const threedi::ThreediBuildVec3 m =
            threedi::threedi_presentation_to_mission(threedi::ThreediBuildVec3{c.x, c.y, c.z});
    return BadBuildVec3{m.x, m.y, m.z};
}

// Conjugating a rotation by the permutation turns its axis and keeps its angle,
// so a quaternion takes the vector map on its vector part and keeps w. That is
// exact in both directions, which is what keeps build(scene(x)) byte for byte.
BadBuildQuat bad_clip_from_mission(const BadBuildQuat &m) {
    const BadBuildVec3 axis = bad_clip_from_mission(BadBuildVec3{m.x, m.y, m.z});
    return BadBuildQuat{axis.x, axis.y, axis.z, m.w};
}

BadBuildQuat bad_mission_from_clip(const BadBuildQuat &c) {
    const BadBuildVec3 axis = bad_mission_from_clip(BadBuildVec3{c.x, c.y, c.z});
    return BadBuildQuat{axis.x, axis.y, axis.z, c.w};
}

void bad_quat_to_rows(const BadBuildQuat &q, float rows[9]) {
    const Mat3 r = rows_of(q);
    for (int i = 0; i < 9; ++i) rows[i] = static_cast<float>(r.m[i]);
}

BadBuildQuat bad_rows_to_quat(const float rows[9]) { return quat_of(rows_from_floats(rows)); }

void bad_rows_transpose(const float in[9], float out[9]) {
    float copy[9];
    std::memcpy(copy, in, sizeof(copy));
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = copy[c * 3 + r];
}

BadBuildVec3 bad_bone_position(const float parent_bind_rows[9], const BadBuildVec3 &rel_mission) {
    return apply(rows_from_floats(parent_bind_rows), bad_clip_from_mission(rel_mission));
}

BadBuildVec3 bad_bone_rel(const float parent_bind_rows[9], const float position[3]) {
    const BadBuildVec3 stored{static_cast<double>(position[0]), static_cast<double>(position[1]),
                              static_cast<double>(position[2])};
    return bad_mission_from_clip(apply(transpose(rows_from_floats(parent_bind_rows)), stored));
}

void bad_derive_bind_rows(const BadBuildClip &clip, const BadBuildClip *reset, size_t bone,
                          float rows[9]) {
    const BadBuildBone *source = bone < clip.bones.size() ? &clip.bones[bone] : nullptr;
    if (reset != nullptr && bone < reset->bones.size() && !reset->bones[bone].keys.empty())
        source = &reset->bones[bone];
    float first[9];
    bad_quat_to_rows(source != nullptr && !source->keys.empty()
                             ? quat_from_stored(stored_key(source->keys.front()))
                             : BadBuildQuat{},
                     first);
    bad_rows_transpose(first, rows);
}

void bad_derive_bone_table(const BadBuildClip &clip, const BadBuildClip *reset,
                           std::vector<BadBone> &rows) {
    const size_t bones = clip.bones.size();
    rows.assign(bones, BadBone{});
    for (size_t i = 0; i < bones; ++i) {
        const BadBuildBone &bone = clip.bones[i];
        BadBone &row = rows[i];
        std::memset(&row, 0, sizeof(row));
        std::memcpy(row.name, bone.name.c_str(), std::min(bone.name.size(), sizeof(row.name) - 1));
        row.parent_index = bone.parent;
        row.length = static_cast<float>(bone.length);
        bad_derive_bind_rows(clip, nullptr, i, row.rotation);

        const bool root = bone.parent < 0 || static_cast<size_t>(bone.parent) >= i;
        BadBuildVec3 p;
        if (bone.position_given) {
            p = bone.position_stored;
        } else if (root) {
            p = bad_clip_from_mission(bone.pivot);
        } else {
            const size_t up = static_cast<size_t>(bone.parent);
            const BadBuildVec3 rel{bone.pivot.x - clip.bones[up].pivot.x,
                                   bone.pivot.y - clip.bones[up].pivot.y,
                                   bone.pivot.z - clip.bones[up].pivot.z};
            float bind[9];
            bad_derive_bind_rows(clip, reset, up, bind);
            p = bad_bone_position(bind, rel);
        }
        row.position[0] = static_cast<float>(p.x);
        row.position[1] = static_cast<float>(p.y);
        row.position[2] = static_cast<float>(p.z);
    }
}

bool bad_build_assemble(const BadBuildClip &clip, const BadBuildClip *reset, BadAssembled &out,
                        std::string *error) {
    out = BadAssembled{};
    const size_t bones = clip.bones.size();
    if (bones == 0) return fail(error, "a clip holds no bones");
    if (bones > kBadMaxBones)
        return fail(error, "a clip holds " + std::to_string(bones) + " bones; the game's bone arrays hold " +
                                   std::to_string(kBadMaxBones));
    if (clip.frame_count == 0) return fail(error, "a clip holds no frames");
    // A loop steps fps / (62 * frames) of its cycle a tick and takes one away
    // once at the wrap, so a step of a whole cycle or more never plays: at
    // exactly one it shows its first frame for ever, past one its time runs on
    // past the clip's rows. [orig: AnimChannel_InitFromData @0x410560, the step
    // fps / 62 / frames @0x4105BA; AnimChannel_AdvancePlayback @0x40B140, t -= 1
    // once @0x40B199; sub_4102D0 @0x4102D0 reads row trunc(frames * t) unbounded]
    if ((clip.flags & BAD_FLAG_LOOP) != 0 &&
            static_cast<uint64_t>(clip.fps) >=
                    static_cast<uint64_t>(io::kTicksPerSecondInt) * clip.frame_count)
        return fail(error, "a looping clip at " + std::to_string(clip.fps) + " fps over " +
                                   std::to_string(clip.frame_count) + " frames steps its whole cycle or more "
                                   "each tick; a loop's fps stays under " +
                                   std::to_string(io::kTicksPerSecondInt) + " times its frame count");
    // The two event record shapes the loader knows: version 1 (24 bytes, with
    // the trigger word) and version 0 (20 bytes, none).
    if (clip.version > 1) return fail(error, "a clip's version is 0 or 1");
    const size_t keys = static_cast<size_t>(clip.frame_count) + 1;
    const bool translated = (clip.flags & BAD_FLAG_TRANSLATION) != 0;

    for (size_t i = 0; i < bones; ++i) {
        const BadBuildBone &bone = clip.bones[i];
        if (bone.name.size() > 31)
            return fail(error, "bone " + std::to_string(i) + " name is over 31 characters");
        if (i == 0) {
            if (bone.parent >= 0) return fail(error, "bone 0 carries a parent");
        } else if (bone.parent < 0 || static_cast<size_t>(bone.parent) >= i) {
            return fail(error, "bone " + std::to_string(i) + " parent is not a lower index");
        }
        // A bone keys every frame, or keys sparsely and says how long each key
        // lasts: the channel walks its own duration table, and only the header's
        // frame count sets the clip's length
        // [orig: BoneAnim_FindKeyframeAtTime @0x410220]. Retail keys densely in
        // 476 of 477 clips; DVFLEE1E.BAD is the sparse one.
        if (bone.keys.empty())
            return fail(error, "bone " + std::to_string(i) + " holds no key");
        if (bone.durations.empty() && bone.keys.size() != keys)
            return fail(error, "bone " + std::to_string(i) + " holds " +
                                       std::to_string(bone.keys.size()) +
                                       " keys, not " + std::to_string(keys) +
                                       ", and no key duration table");
        if (!bone.durations.empty() && bone.durations.size() != bone.keys.size())
            return fail(error, "bone " + std::to_string(i) + " duration count is not its key count");
        for (const uint16_t d : bone.durations) {
            if (d == 0) return fail(error, "bone " + std::to_string(i) + " holds a zero duration");
        }
        for (const BadBuildQuat &q : bone.keys) {
            const double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (!(std::fabs(len - 1.0) <= 1e-3))
                return fail(error, "bone " + std::to_string(i) + " holds a key that is not a unit "
                                                                 "quaternion");
        }
        if (!translated && !bone.translations.empty())
            return fail(error, "bone " + std::to_string(i) +
                                       " holds translations the clip's flags do not carry");
        if (translated && bone.translations.size() != keys)
            return fail(error, "bone " + std::to_string(i) + " holds " +
                                       std::to_string(bone.translations.size()) +
                                       " translations, not " + std::to_string(keys));
    }
    if (!clip.events.empty() && clip.events.size() != keys)
        return fail(error, "a clip holds " + std::to_string(clip.events.size()) + " events, not " +
                                   std::to_string(keys));
    for (const BadBuildEvent &ev : clip.events) {
        if (clip.version == 0 && ev.trigger != 0)
            return fail(error, "a version 0 clip's event carries no trigger word");
    }

    // Channels first: the bone table's rotation is the transpose of the stored
    // first key, and a bone's position turns through its parent's.
    out.rotations.resize(bones);
    out.durations.resize(bones);
    for (size_t i = 0; i < bones; ++i) {
        const BadBuildBone &bone = clip.bones[i];
        const size_t count = bone.keys.size();
        out.rotations[i].resize(count);
        out.durations[i].resize(count);
        for (size_t f = 0; f < count; ++f) {
            out.rotations[i][f] = stored_key(bone.keys[f]);
            out.durations[i][f] = bone.durations.empty() ? uint16_t{1} : bone.durations[f];
        }
    }

    bad_derive_bone_table(clip, reset, out.bones);

    out.channels.resize(bones);
    for (size_t i = 0; i < bones; ++i) {
        BadChannel &ch = out.channels[i];
        ch.frame_count = static_cast<uint32_t>(out.rotations[i].size());
        ch.frame_lengths_offset = 0;
        ch.rotations_offset = 0;
        ch.frame_lengths = out.durations[i].data();
        ch.rotations = out.rotations[i].data();
    }

    // Every event as the author states it: its heights are measured from the
    // ground, which the clip does not hold.
    out.events.resize(clip.events.size());
    for (size_t f = 0; f < clip.events.size(); ++f) {
        const BadBuildEvent &ev = clip.events[f];
        const BadBuildVec3 v = bad_clip_from_mission(ev.velocity);
        BadEvent &row = out.events[f];
        row.velocity[0] = static_cast<float>(v.x);
        row.velocity[1] = static_cast<float>(v.y);
        row.velocity[2] = static_cast<float>(v.z);
        row.bottom = static_cast<float>(ev.bottom);
        row.top = static_cast<float>(ev.top);
        row.trigger = ev.trigger;
    }

    if (translated) {
        out.translations.resize(bones * keys);
        for (size_t f = 0; f < keys; ++f) {
            for (size_t i = 0; i < bones; ++i) {
                const BadBuildVec3 t = bad_clip_from_mission(clip.bones[i].translations[f]);
                std::array<float, 3> &row = out.translations[f * bones + i];
                row[0] = static_cast<float>(t.x);
                row[1] = static_cast<float>(t.y);
                row[2] = static_cast<float>(t.z);
            }
        }
    }

    BadFile &file = out.file;
    file.version = clip.version;
    file.header_size = 80;
    file.fps = clip.fps;
    file.frame_count = clip.frame_count;
    file.flags = clip.flags;
    file.bone_count = static_cast<uint32_t>(bones);
    file.bones = out.bones.data();
    file.num_bones = bones;
    file.channels = out.channels.data();
    file.num_channels = bones;
    file.events = out.events.empty() ? nullptr : out.events.data();
    file.num_events = out.events.size();
    file.translations = out.translations.empty()
                                ? nullptr
                                : reinterpret_cast<float(*)[3]>(out.translations.data());
    file.num_translations = out.translations.size();
    return true;
}

bool bad_build_mint(const BadBuildClip &clip, const BadBuildClip *reset, std::vector<uint8_t> &out,
                    std::string *error) {
    BadAssembled assembled;
    if (!bad_build_assemble(clip, reset, assembled, error)) return false;
    if (bad_write_buffer(&assembled.file, out) != 0)
        return fail(error, "the writer refused the clip");
    // The loader refuses a file over 500,000 bytes [orig: BoneFile_Load
    // @0x40fff0, the 0x7A120 size gate]; the largest retail clip is 298,172.
    if (out.size() > kBadFileMaxBytes)
        return fail(error, "the clip is " + std::to_string(out.size()) +
                                   " bytes, over the loader's 500000");
    return true;
}

std::string bad_build_clip_stem(const std::string &variant) {
    if (variant.size() > 4 && strutil::ends_with_icase(variant, ".bad"))
        return variant.substr(0, variant.size() - 4);
    return variant;
}

bool bad_build_bare_stem(const std::string &name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || std::strchr("/\\:|*?<>\"", c) != nullptr)
            return false;
    }
    return true;
}

bool bad_build_packable_name(const std::string &file_name) {
    if (file_name.empty() || file_name.size() > kBadPackedNameMax) return false;
    for (const char c : file_name) {
        if (static_cast<unsigned char>(c) >= 0x80) return false;
    }
    return true;
}

bool bad_build_check_set(const BadBuildSet &set, std::vector<std::string> &problems) {
    const size_t before = problems.size();
    if (set.rows.empty()) return true; // a lone clip: its output name is the file
    for (const BadBuildClip &clip : set.clips) {
        const std::string file = clip.name + ".bad";
        if (!bad_build_packable_name(file))
            problems.push_back("clip '" + clip.name + "' writes '" + file + "', " +
                               std::to_string(file.size()) + " bytes; the game packs a file name of at most " +
                               std::to_string(kBadPackedNameMax) + " ASCII bytes, its extension included");
    }
    const BadBuildClip *reset = bad_build_reset_clip(set);
    if (reset != nullptr && (reset->flags & BAD_FLAG_TRANSLATION) == 0) {
        for (const BadBuildClip &clip : set.clips) {
            if ((clip.flags & BAD_FLAG_TRANSLATION) != 0)
                problems.push_back("clip '" + clip.name + "' carries translations but the reset clip '" +
                                   reset->name + "' does not; the game moves a bone only when both do "
                                   "(give the reset clip translations, flag 0x2)");
        }
    }
    return problems.size() == before;
}

std::string bad_build_reset_stem(const std::vector<BadBuildRow> &rows) {
    const BadBuildRow *reset = nullptr;
    for (const BadBuildRow &row : rows) {
        if (!row.variants.empty() && adm::adm_key_names_slot(row.key, "reset")) reset = &row;
    }
    return reset != nullptr ? bad_build_clip_stem(reset->variants.back()) : std::string();
}

const BadBuildClip *bad_build_reset_clip(const BadBuildSet &set) {
    const std::string stem = bad_build_reset_stem(set.rows);
    if (stem.empty()) return nullptr;
    for (const BadBuildClip &clip : set.clips) {
        if (strutil::iequals(clip.name, stem)) return &clip;
    }
    return nullptr;
}

bool bad_build_mint_table(const BadBuildSet &set, std::string &out, std::string *error) {
    out.clear();
    // A table binds every clip to its reset row's clip; with none the game
    // faults loading it [orig: AnimMap_LoadAdmFile @0x40cc40, the read of slot
    // 0's head @0x40ce11..0x40ce16].
    if (bad_build_reset_stem(set.rows).empty())
        return fail(error, "the table has no reset row (a key naming slot 0, as `anim_reset` "
                           "does): every clip of a table binds to its clip, and the game "
                           "cannot load a table without one");
    std::vector<adm::AdmEntry> entries(set.rows.size());
    for (size_t i = 0; i < set.rows.size(); ++i) {
        const BadBuildRow &row = set.rows[i];
        adm::AdmEntry &entry = entries[i];
        std::memset(&entry, 0, sizeof(entry));
        if (row.key.size() >= sizeof(entry.key))
            return fail(error, "row key '" + row.key + "' is too long");
        std::memcpy(entry.key, row.key.c_str(), row.key.size());
        if (row.variants.size() > static_cast<size_t>(adm::ADM_MAX_VARIANTS))
            return fail(error, "row '" + row.key + "' holds over " +
                                       std::to_string(adm::ADM_MAX_VARIANTS) + " variants");
        entry.variant_count = row.variants.size();
        for (size_t v = 0; v < row.variants.size(); ++v) {
            if (row.variants[v].size() >= sizeof(entry.variants[v]))
                return fail(error, "row '" + row.key + "' names a clip that is too long");
            if (!bad_build_bare_stem(bad_build_clip_stem(row.variants[v])))
                return fail(error, "row '" + row.key + "' names '" + row.variants[v] +
                                           "', which is not a bare file name");
            std::memcpy(entry.variants[v], row.variants[v].c_str(), row.variants[v].size());
        }
    }
    adm::AdmFile table{};
    table.entries = entries.empty() ? nullptr : entries.data();
    table.count = entries.size();
    if (adm::adm_write_buffer(&table, out) != 0)
        return fail(error, "the table writer refused a row (a key of five characters or fewer, "
                           "which names no slot, or not one plain token, no variants, or a variant "
                           "it cannot quote)");
    // Parse-equality is the table's parity (ADR 0047): read the text back
    // through the parser and require every row as it went in.
    adm::AdmFile back{};
    if (adm::adm_parse_buffer(out.data(), out.size(), &back) != 0)
        return fail(error, "the table does not read back");
    std::string differs = back.count == entries.size() ? "" : "the table does not read back as written";
    for (size_t i = 0; differs.empty() && i < entries.size(); ++i) {
        const adm::AdmEntry &a = entries[i];
        const adm::AdmEntry &b = back.entries[i];
        bool same = std::strcmp(a.key, b.key) == 0 && a.variant_count == b.variant_count;
        for (size_t v = 0; same && v < a.variant_count; ++v)
            same = std::strcmp(a.variants[v], b.variants[v]) == 0;
        if (!same) differs = "row '" + set.rows[i].key + "' does not read back as written";
    }
    adm::adm_free(&back);
    if (!differs.empty()) {
        out.clear();
        return fail(error, differs);
    }
    return true;
}

} // namespace opennova::bad
