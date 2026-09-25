// The BAD clip construction seam (bad_build.h): the frame maps, the derived
// bone table, and the assembly the writer serializes.

#include <formats/bad/bad_build.h>

#include <formats/adm/adm.h>
#include <formats/bad/bad_write.h>
#include <formats/threedi/threedi_build.h>

#include <base/io/strutil.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace opennova::bad {

namespace {

// Row-major 3x3 in doubles; the file stores the same layout in floats.
struct Mat3 {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

Mat3 multiply(const Mat3 &a, const Mat3 &b) {
    Mat3 o{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) s += a.m[r * 3 + k] * b.m[k * 3 + c];
            o.m[r * 3 + c] = s;
        }
    }
    return o;
}

BadBuildQuat conjugate(const BadBuildQuat &q) { return BadBuildQuat{-q.x, -q.y, -q.z, q.w}; }

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

namespace {

BadBuildQuat quat_product(const BadBuildQuat &a, const BadBuildQuat &b) {
    return BadBuildQuat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// A unit short-arc slerp: the capsule measure below is ours, so this is a
// plain one rather than the runtime's port of Math_QuaternionSlerp.
BadBuildQuat quat_blend(const BadBuildQuat &a, BadBuildQuat b, double t) {
    double dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0) {
        b = BadBuildQuat{-b.x, -b.y, -b.z, -b.w};
        dot = -dot;
    }
    double wa = 1.0 - t;
    double wb = t;
    if (dot < 0.9999) {
        const double omega = std::acos(dot > 1.0 ? 1.0 : dot);
        const double inv = 1.0 / std::sin(omega);
        wa = std::sin((1.0 - t) * omega) * inv;
        wb = std::sin(t * omega) * inv;
    }
    BadBuildQuat q{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                   a.w * wa + b.w * wb};
    const double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 0.0) q = BadBuildQuat{q.x / len, q.y / len, q.z / len, q.w / len};
    return q;
}

// The rotation a bone's channel holds at frame f, in the clip frame, as the
// file will store it: the key whose window holds f, walking the bone's own
// duration table, turned toward the next key by the fraction
// [orig: BoneAnim_FindKeyframeAtTime @0x410220]. Past the summed durations the
// last key holds, as runtime/anim's pose table does (the original returns key
// 0 there, @0x410290).
BadBuildQuat key_at(const BadBuildBone &bone, size_t f) {
    const size_t count = bone.keys.size();
    if (count == 0) return BadBuildQuat{};
    const auto stored = [&](size_t k) { return quat_from_stored(stored_key(bone.keys[k])); };
    size_t acc = 0;
    for (size_t k = 0; k < count; ++k) {
        const size_t dur = bone.durations.empty() ? 1 : bone.durations[k];
        if (acc + dur > f) {
            if (k + 1 >= count || f == acc) return stored(k);
            return quat_blend(stored(k), stored(k + 1),
                              static_cast<double>(f - acc) / static_cast<double>(dur));
        }
        acc += dur;
    }
    return stored(count - 1);
}

} // namespace

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

void bad_clip_extents(const BadBuildClip &clip, const BadBuildClip *reset,
                      std::vector<double> &bottom, std::vector<double> &top) {
    const size_t bones = clip.bones.size();
    const size_t keys = static_cast<size_t>(clip.frame_count) + 1;
    bottom.assign(keys, 0.0);
    top.assign(keys, 0.0);
    if (bones == 0) return;
    const bool translated = (clip.flags & BAD_FLAG_TRANSLATION) != 0;

    // The pose the capsule measures is the one the runtime draws: each bone's
    // key composed against the rig's bind, `key * bind^-1`. The bind is the
    // set's reset clip, which the runtime pins once per entity and every clip
    // of the rig composes against; a clip with no reset to name (a lone clip)
    // composes against its own first key, the runtime's fallback when no bind
    // is pinned [orig: AnimChannel_ComputeBoneMatrices @0x410da0, the bind from
    // channel+44 @0x410dd8 else the playing clip @0x410de3; AnimMap_RegisterEntity
    // @0x40bb60 pins slot 0's clip @0x40bbe3]. A bone past the reset clip's
    // own bones takes its own first key.
    std::vector<BadBuildQuat> bind_inverse(bones);
    for (size_t i = 0; i < bones; ++i) {
        const BadBuildBone *source = &clip.bones[i];
        if (reset != nullptr && i < reset->bones.size() && !reset->bones[i].keys.empty())
            source = &reset->bones[i];
        if (!source->keys.empty())
            bind_inverse[i] = conjugate(quat_from_stored(stored_key(source->keys.front())));
    }

    // The extents RULE below is ours: retail's exporter measured them, and no
    // tool is witnessed (docs/anim/adm-bad-format-re.md). It is the lowest and
    // highest bone origin about bone 0 over that pose.
    std::vector<BadBuildVec3> posed(bones);
    std::vector<BadBuildQuat> turn(bones);
    for (size_t f = 0; f < keys; ++f) {
        double low = 0.0;
        double high = 0.0;
        for (size_t i = 0; i < bones; ++i) {
            const BadBuildBone &bone = clip.bones[i];
            turn[i] = quat_product(key_at(bone, f), bind_inverse[i]);
            const int parent = bone.parent;
            if (parent < 0 || static_cast<size_t>(parent) >= i) {
                posed[i] = BadBuildVec3{};
            } else {
                const size_t up = static_cast<size_t>(parent);
                const BadBuildBone &above = clip.bones[up];
                const BadBuildVec3 rel = bad_clip_from_mission(BadBuildVec3{
                        bone.pivot.x - above.pivot.x, bone.pivot.y - above.pivot.y,
                        bone.pivot.z - above.pivot.z});
                const BadBuildVec3 turned = apply(rows_of(turn[up]), rel);
                posed[i] = BadBuildVec3{posed[up].x + turned.x, posed[up].y + turned.y,
                                        posed[up].z + turned.z};
            }
            if (translated && f < bone.translations.size()) {
                const BadBuildVec3 t = bad_clip_from_mission(bone.translations[f]);
                posed[i].x += t.x;
                posed[i].y += t.y;
                posed[i].z += t.z;
            }
            // The clip frame's y is up; the extents measure from bone 0.
            if (posed[i].y < low) low = posed[i].y;
            if (posed[i].y > high) high = posed[i].y;
        }
        bottom[f] = low < 0.0 ? -low : 0.0;
        top[f] = high - low;
    }
}

bool bad_build_assemble(const BadBuildClip &clip, const BadBuildClip *reset, BadAssembled &out,
                        std::string *error) {
    out = BadAssembled{};
    const size_t bones = clip.bones.size();
    if (bones == 0) return fail(error, "a clip holds no bones");
    if (clip.frame_count == 0) return fail(error, "a clip holds no frames");
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

    if (!clip.events.empty()) {
        std::vector<double> bottom;
        std::vector<double> top;
        bool derive = false;
        for (const BadBuildEvent &ev : clip.events) {
            if (!ev.extents_given && !clip.capsule_given) derive = true;
        }
        if (derive) bad_clip_extents(clip, reset, bottom, top);
        out.events.resize(clip.events.size());
        for (size_t f = 0; f < clip.events.size(); ++f) {
            const BadBuildEvent &ev = clip.events[f];
            const BadBuildVec3 v = bad_clip_from_mission(ev.velocity);
            BadEvent &row = out.events[f];
            row.velocity[0] = static_cast<float>(v.x);
            row.velocity[1] = static_cast<float>(v.y);
            row.velocity[2] = static_cast<float>(v.z);
            if (ev.extents_given) {
                row.bottom = static_cast<float>(ev.bottom);
                row.top = static_cast<float>(ev.top);
            } else if (clip.capsule_given) {
                row.bottom = static_cast<float>(clip.capsule_bottom);
                row.top = static_cast<float>(clip.capsule_top);
            } else {
                row.bottom = static_cast<float>(bottom[f]);
                row.top = static_cast<float>(top[f]);
            }
            row.trigger = ev.trigger;
        }
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

std::string bad_build_reset_stem(const std::vector<BadBuildRow> &rows) {
    const BadBuildRow *reset = nullptr;
    for (const BadBuildRow &row : rows) {
        if (row.key.size() > 5 && !row.variants.empty() &&
            strutil::iequals(std::string_view(row.key).substr(5), "reset"))
            reset = &row;
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
        return fail(error, "the table writer refused a row (a key outside the anim_ namespace or "
                           "not one plain token, no variants, or a variant it cannot quote)");
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
