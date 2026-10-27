/* engine/runtime/anim SkeletalClips — a body rig's pose against the body bone builder (D-INF-13).

   Every rig loader runs the rest-carrying factorization: the skeleton (the table's reset .bad)
   gives each row its bind rotation as the rest, a clip's channels give each row its absolute
   rotation, and the skin divides the posed globals by the rest globals. The body builder instead
   composes each clip's channels against the reset .bad's bind 3x3 (channel+44), conjugates the
   result into the render frame and re-anchors each row's pivot through its parent's finished row.
   This oracle ports the builder's composition and checks the deformation the skin and the
   collision pose share, through the collision provider's frame map, against it row for row, on
   body-shaped data where the two can disagree: the clip's frame-0 channels are not the skeleton's
   bind, its own bind records differ from the skeleton's (they must be ignored), every bind is a
   non-symmetric turn, and the pivots carry an X term the frame map negates. A translated clip over a
   translated reset (JOTAC's tiger.adm / shark.adm shape) poses the same rows on a body rig, whose builder
   never reads a translation row; a first-person rig moves by them.
   [orig: AnimChannel_ComputeBoneMatrices @0x410DA0 - bind = channel+44 @0x410DD8, its records'
    3x3 transposed @0x410E10, times the channel matrix @0x410E20, transposed into the 4x4
    @0x410E2B; BoneAnim_TransformBones @0x410360 - Math_QuaternionToMatrix3x3 @0x615A70 into the
    channel matrix @0x4103EF; Entity_BuildBoneTransformMatrices @0x4B1290 - the call @0x4B1388,
    the copy S * A^T * S with a zero translation row @0x4B140F..0x4B1494, the overlay multiply
    @0x4B1FE0, the pivot T(-p) * M @0x4B201E..0x4B20A2 and the parent's finished row carrying the
    pivot @0x4B20AE..0x4B2158] */

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/test_expect.h"

#include <formats/bad/bad.h>
#include <runtime/anim/anim_sample.h>
#include <runtime/anim/rig_files.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>

using namespace opennova;
using anim::PoseBone;
using anim::SkeletalClips;
using test_expect::expect;

namespace {

constexpr size_t kRows = 4;

using Mat3 = std::array<float, 9>; // row-major

struct Quat4 {
    float x, y, z, w;
};

Quat4 axis_angle(float ax, float ay, float az, float rad) {
    const float n = std::sqrt(ax * ax + ay * ay + az * az);
    const float s = std::sin(rad * 0.5f) / n;
    return {ax * s, ay * s, az * s, std::cos(rad * 0.5f)};
}

// [orig: Math_QuaternionToMatrix3x3 @0x615A70]
Mat3 retail_quat_matrix(const Quat4 &q) {
    const double tx = q.x * 2.0, ty = q.y * 2.0, tz = 2.0 * q.z;
    const double xx = q.x * tx, xy = q.x * ty, xz = q.x * tz;
    const double yy = q.y * ty, yz = q.y * tz, zz = q.z * tz;
    const double wx = tx * q.w, wy = ty * q.w, wz = tz * q.w;
    return {float(1.0 - (zz + yy)), float(wz + xy), float(xz - wy),
            float(xy - wz), float(1.0 - (zz + xx)), float(wx + yz),
            float(xz + wy), float(yz - wx), float(1.0 - (xx + yy))};
}

// [orig: Math_MultiplyMatrix3x3_Float @0x616090: result = a * b]
Mat3 mul(const Mat3 &a, const Mat3 &b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    return r;
}

Mat3 transpose(const Mat3 &m) {
    return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

// A row's finished matrix: the 3x3 and the translation row, row-vector form (p * M + t).
struct Row {
    Mat3 m{};
    float t[3] = {0, 0, 0};
};

void apply(const Row &r, const float p[3], float out[3]) {
    for (int j = 0; j < 3; ++j)
        out[j] = p[0] * r.m[j] + p[1] * r.m[3 + j] + p[2] * r.m[6 + j] + r.t[j];
}

// The body builder's rows for one key: `channel` the clip's key rotations, `bind` the skeleton's
// stored records, `pivot` each row's model-space pivot in the render (file) frame, `parent` the
// model table's (the root its own), the overlays identity (an entity at the origin, unturned).
std::vector<Row> retail_body_rows(const std::vector<Quat4> &channel, const std::vector<Mat3> &bind,
        const std::vector<std::array<float, 3>> &pivot, const std::vector<int> &parent) {
    const size_t n = channel.size();
    std::vector<Row> slot(n);
    for (size_t k = 0; k < n; ++k) {
        // A = Transpose(Transpose(bind) * channel), then the copy S * A^T * S, translation 0.
        const Mat3 a = transpose(mul(transpose(bind[k]), retail_quat_matrix(channel[k])));
        const Mat3 at = transpose(a);
        Row o;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const float s = (i == 0 ? -1.0f : 1.0f) * (j == 0 ? -1.0f : 1.0f);
                o.m[i * 3 + j] = s * at[i * 3 + j];
            }
        slot[k] = o;
    }
    for (size_t k = 0; k < n; ++k) {
        // M = O * overlay (identity); W = T(-p) * M, then the parent's row as it stands carries
        // the pivot: a parent before the row its finished row, the root its own M.
        Row w;
        w.m = slot[k].m;
        const float neg[3] = {-pivot[k][0], -pivot[k][1], -pivot[k][2]};
        Row rot = w;
        apply(rot, neg, w.t);
        const Row &p = slot[static_cast<size_t>(parent[k])];
        float carried[3];
        apply(p, pivot[k].data(), carried);
        for (int j = 0; j < 3; ++j) w.t[j] += carried[j];
        slot[k] = w;
    }
    return slot;
}

// The collision provider's frame map (world/entity_pose.cpp render_matrix_from_deformation):
// S * D^T * S with the origin X negated.
Row render_row(const SkeletalClips::RestTransform &d) {
    Row r;
    const float *x = d.rows;
    r.m = {x[0], -x[3], -x[6], -x[1], x[4], x[7], -x[2], x[5], x[8]};
    r.t[0] = -d.origin.x;
    r.t[1] = d.origin.y;
    r.t[2] = d.origin.z;
    return r;
}

class MemoryFiles : public anim::RigFiles {
public:
    std::map<std::string, const bad::BadFile *> bads;
    std::shared_ptr<const adm::AdmFile> animation_map(const std::string &) const override { return nullptr; }
    std::shared_ptr<const bad::BadFile> bone_animation(const std::string &name) const override {
        const auto it = bads.find(name);
        return it == bads.end() ? nullptr
                                : std::shared_ptr<const bad::BadFile>(it->second, [](const bad::BadFile *) {});
    }
};

// One .bad of kRows bones: each bone's stored record rotation and its keys.
struct MemoryBad {
    std::vector<bad::BadBone> bones;
    std::vector<bad::BadChannel> channels;
    std::vector<std::vector<bad::BadQuaternion>> keys;
    std::vector<std::vector<uint16_t>> lengths;
    bad::BadFile file = {};

    std::vector<std::array<float, 3>> moves;

    MemoryBad(const std::vector<Mat3> &records, const std::vector<std::vector<Quat4>> &key_rotations,
              bool translated = false)
        : bones(kRows), channels(kRows), keys(kRows), lengths(kRows) {
        for (size_t i = 0; i < kRows; ++i) {
            bones[i].parent_index = i == 0 ? -1 : 0;
            for (int e = 0; e < 9; ++e) bones[i].rotation[e] = records[i][static_cast<size_t>(e)];
            for (const Quat4 &q : key_rotations[i]) keys[i].push_back({q.x, q.y, q.z, q.w});
            lengths[i].assign(keys[i].size(), 1);
            channels[i].frame_count = static_cast<uint32_t>(keys[i].size());
            channels[i].frame_lengths = lengths[i].data();
            channels[i].rotations = keys[i].data();
        }
        file.fps = 30;
        file.frame_count = static_cast<uint32_t>(key_rotations[0].size()) - 1;
        file.bone_count = kRows;
        file.bones = bones.data();
        file.num_bones = kRows;
        file.channels = channels.data();
        file.num_channels = kRows;
        if (translated) {
            // One row a bone a key, each a distinct move (the translation block is laid out by
            // the file's bone count).
            for (size_t key = 0; key < key_rotations[0].size(); ++key)
                for (size_t b = 0; b < kRows; ++b)
                    moves.push_back({0.05f * float(b + 1), -0.03f * float(key + 1), 0.02f * float(b + key + 1)});
            file.flags |= 0x02u;
            file.translations = reinterpret_cast<float (*)[3]>(moves.data());
            file.num_translations = moves.size();
        }
    }
};

bool same_row(const Row &a, const Row &b, float eps) {
    for (int i = 0; i < 9; ++i)
        if (std::fabs(a.m[static_cast<size_t>(i)] - b.m[static_cast<size_t>(i)]) > eps) return false;
    for (int j = 0; j < 3; ++j)
        if (std::fabs(a.t[j] - b.t[j]) > eps) return false;
    return true;
}

Mat3 turn(float ax, float ay, float az, float rad) {
    // A stored record as the exporter writes one: the transpose of the turn's channel matrix in
    // the column form our sampler reads (anim_sample.cpp: mat3(stored) * channel at reset is
    // identity), from a quaternion so it is a proper rotation.
    return retail_quat_matrix(axis_angle(ax, ay, az, rad));
}

} // namespace

int main() {
    // The model table: rows 1 under the root, 2 and 3 under row 1; the root at the model origin
    // (every shipped rig's), the others off it in all three axes.
    const std::vector<int> parents = {0, 0, 1, 1};
    const std::vector<anim::Vec3> rel = {{0.0f, 0.0f, 0.0f}, {0.12f, 0.9f, 0.05f},
                                         {-0.21f, 0.33f, 0.1f}, {0.17f, 0.26f, -0.14f}};
    std::vector<std::array<float, 3>> abs_pivot(kRows);
    for (size_t k = 0; k < kRows; ++k) {
        abs_pivot[k] = {rel[k].x, rel[k].y, rel[k].z};
        if (parents[k] != static_cast<int>(k))
            for (int j = 0; j < 3; ++j) abs_pivot[k][static_cast<size_t>(j)] += abs_pivot[static_cast<size_t>(parents[k])][static_cast<size_t>(j)];
    }

    // The skeleton: a non-symmetric bind on every row, its channels its binds' (a healthy reset).
    const std::vector<Mat3> skel_bind = {turn(0.3f, 1.0f, 0.2f, 0.9f), turn(1.0f, 0.1f, -0.4f, -0.6f),
                                         turn(-0.2f, 0.5f, 1.0f, 1.3f), turn(0.7f, -0.7f, 0.1f, 0.4f)};
    std::vector<std::vector<Quat4>> skel_keys(kRows);
    const Quat4 skel_q[kRows] = {axis_angle(0.3f, 1.0f, 0.2f, -0.9f), axis_angle(1.0f, 0.1f, -0.4f, 0.6f),
                                 axis_angle(-0.2f, 0.5f, 1.0f, -1.3f), axis_angle(0.7f, -0.7f, 0.1f, -0.4f)};
    for (size_t k = 0; k < kRows; ++k) skel_keys[k] = {skel_q[k], skel_q[k]};
    MemoryBad skel(skel_bind, skel_keys);

    // The clip: its own records unlike the skeleton's, and two keys per row, neither its bind.
    const std::vector<Mat3> clip_records = {turn(0.0f, 1.0f, 0.0f, 0.2f), turn(1.0f, 0.0f, 0.0f, 0.5f),
                                            turn(0.0f, 0.0f, 1.0f, -0.7f), turn(1.0f, 1.0f, 0.0f, 0.3f)};
    std::vector<std::vector<Quat4>> clip_keys = {
        {axis_angle(0.2f, 1.0f, 0.1f, 0.45f), axis_angle(0.0f, 1.0f, 0.3f, -0.8f)},
        {axis_angle(1.0f, -0.3f, 0.2f, 0.7f), axis_angle(0.4f, 0.6f, -1.0f, 1.1f)},
        {axis_angle(-0.5f, 0.2f, 1.0f, -0.35f), axis_angle(0.9f, 0.1f, 0.3f, 0.95f)},
        {axis_angle(0.3f, 0.3f, -1.0f, 1.2f), axis_angle(-1.0f, 0.4f, 0.2f, -0.55f)},
    };
    MemoryBad clip(clip_records, clip_keys);

    MemoryFiles files;
    files.bads["skel"] = &skel.file;
    files.bads["clip"] = &clip.file;
    SkeletalClips rig;
    if (!expect(rig.load_from_files(&files, "skel", {{"anim_clip", "clip"}}, rel, parents) &&
                        rig.fk_valid() && rig.bone_count() == kRows,
                "the body rig loads over the model table")) {
        return 1;
    }

    // Key 0 (the playhead at 0) and key 1 (held past the one-frame clip's end).
    const double playheads[2] = {0.0, 1.0};
    for (int key = 0; key < 2; ++key) {
        std::vector<PoseBone> pose;
        rig.eval_pose("anim_clip", playheads[key], 0, pose);
        std::vector<SkeletalClips::RestTransform> globals;
        anim::pose_globals(pose, rig.parents(), pose.size(), -1, globals);
        std::vector<Quat4> channel(kRows);
        for (size_t k = 0; k < kRows; ++k) channel[k] = clip_keys[k][static_cast<size_t>(key)];
        const std::vector<Row> retail = retail_body_rows(channel, skel_bind, abs_pivot, parents);
        for (size_t k = 0; k < kRows; ++k) {
            const Row ours = render_row(anim::rest_mul(globals[k], rig.rest_global_inverse()[k]));
            char what[96];
            std::snprintf(what, sizeof(what), "key %d row %zu: the deformation is the body builder's row", key,
                    k);
            if (!expect(same_row(ours, retail[k], 2e-4f), what)) {
                std::fprintf(stderr, "  ours   m=[%g %g %g | %g %g %g | %g %g %g] t=(%g %g %g)\n", ours.m[0],
                        ours.m[1], ours.m[2], ours.m[3], ours.m[4], ours.m[5], ours.m[6], ours.m[7], ours.m[8],
                        ours.t[0], ours.t[1], ours.t[2]);
                std::fprintf(stderr, "  retail m=[%g %g %g | %g %g %g | %g %g %g] t=(%g %g %g)\n", retail[k].m[0],
                        retail[k].m[1], retail[k].m[2], retail[k].m[3], retail[k].m[4], retail[k].m[5],
                        retail[k].m[6], retail[k].m[7], retail[k].m[8], retail[k].t[0], retail[k].t[1],
                        retail[k].t[2]);
            }
        }
    }

    // The clip's own bind records play no part: a clip of the same keys over identity records
    // poses the same rows.
    const std::vector<Mat3> identity(kRows, Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1});
    MemoryBad plain(identity, clip_keys);
    files.bads["plain"] = &plain.file;
    SkeletalClips rig2;
    if (expect(rig2.load_from_files(&files, "skel", {{"anim_clip", "plain"}}, rel, parents),
                "the identity-record clip loads")) {
        std::vector<PoseBone> a, b;
        rig.eval_pose("anim_clip", 0.0, 0, a);
        rig2.eval_pose("anim_clip", 0.0, 0, b);
        bool same = a.size() == b.size();
        for (size_t k = 0; same && k < a.size(); ++k)
            same = std::fabs(a[k].rotation.w - b[k].rotation.w) < 1e-6f &&
                   std::fabs(a[k].rotation.x - b[k].rotation.x) < 1e-6f &&
                   std::fabs(a[k].origin.y - b[k].origin.y) < 1e-6f;
        expect(same, "a clip's own bind records pose nothing");
    }

    // A root off the model origin turns about the origin, as the builder's pivot block leaves it
    // (its parent row is its own, so -p * M + p * M cancels).
    // [orig: Entity_BuildBoneTransformMatrices @0x4B1290 - @0x4B1FE7, @0x4B201E..0x4B2158]
    {
        std::vector<anim::Vec3> moved_rel = rel;
        moved_rel[0] = {0.05f, 0.4f, -0.03f};
        std::vector<std::array<float, 3>> moved_pivot(kRows);
        for (size_t k = 0; k < kRows; ++k) {
            moved_pivot[k] = {moved_rel[k].x, moved_rel[k].y, moved_rel[k].z};
            if (parents[k] != static_cast<int>(k))
                for (int j = 0; j < 3; ++j)
                    moved_pivot[k][static_cast<size_t>(j)] += moved_pivot[static_cast<size_t>(parents[k])][static_cast<size_t>(j)];
        }
        SkeletalClips rooted;
        if (expect(rooted.load_from_files(&files, "skel", {{"anim_clip", "clip"}}, moved_rel, parents, true),
                    "the off-origin rig loads")) {
            for (int key = 0; key < 2; ++key) {
                std::vector<PoseBone> pose;
                rooted.eval_pose("anim_clip", playheads[key], 0, pose);
                std::vector<SkeletalClips::RestTransform> globals;
                anim::pose_globals(pose, rooted.parents(), pose.size(), -1, globals);
                std::vector<Quat4> channel(kRows);
                for (size_t k = 0; k < kRows; ++k) channel[k] = clip_keys[k][static_cast<size_t>(key)];
                const std::vector<Row> retail = retail_body_rows(channel, skel_bind, moved_pivot, parents);
                bool same = true;
                for (size_t k = 0; k < kRows; ++k)
                    same = same && same_row(render_row(anim::rest_mul(globals[k], rooted.rest_global_inverse()[k])),
                                            retail[k], 2e-4f);
                expect(same, "a root off the origin turns about the model origin");
            }
        }
    }

    // A translated clip over a translated reset: the body rig poses the untranslated rows, the
    // builder's; the first-person rig (the default) moves them.
    // [orig: Entity_BuildBoneTransformMatrices @0x4B1290 - the copy's zero translation row
    //  @0x4B1480..0x4B1486; AnimChannel_ComputeBoneMatrices @0x410DA0 - the translation copy under
    //  the bind's flag @0x410DEE, @0x410EA0..0x410EB7, which the body's copy never reads]
    {
        MemoryBad moved_skel(skel_bind, skel_keys, true);
        MemoryBad moved_clip(clip_records, clip_keys, true);
        files.bads["mskel"] = &moved_skel.file;
        files.bads["mclip"] = &moved_clip.file;
        SkeletalClips body, first_person;
        const bool loaded =
                body.load_from_files(&files, "mskel", {{"anim_clip", "mclip"}}, rel, parents, /*body_builder=*/true) &&
                first_person.load_from_files(&files, "mskel", {{"anim_clip", "mclip"}}, rel, parents);
        if (expect(loaded, "the translated rigs load")) {
            std::vector<Quat4> channel(kRows);
            for (size_t k = 0; k < kRows; ++k) channel[k] = clip_keys[k][1];
            const std::vector<Row> retail = retail_body_rows(channel, skel_bind, abs_pivot, parents);
            const auto rows_of = [&](const SkeletalClips &rig) {
                std::vector<PoseBone> pose;
                rig.eval_pose("anim_clip", 1.0, 0, pose);
                std::vector<SkeletalClips::RestTransform> globals;
                anim::pose_globals(pose, rig.parents(), pose.size(), -1, globals);
                std::vector<Row> out(kRows);
                for (size_t k = 0; k < kRows; ++k)
                    out[k] = render_row(anim::rest_mul(globals[k], rig.rest_global_inverse()[k]));
                return out;
            };
            const std::vector<Row> body_rows = rows_of(body);
            const std::vector<Row> fp_rows = rows_of(first_person);
            bool body_same = true, fp_moved = false;
            for (size_t k = 0; k < kRows; ++k) {
                body_same = body_same && same_row(body_rows[k], retail[k], 2e-4f);
                fp_moved = fp_moved || !same_row(fp_rows[k], retail[k], 2e-4f);
            }
            expect(body_same, "a body rig drops a translated clip's rows, as the body builder does");
            expect(fp_moved, "a first-person rig moves by them, as its builder does");
        }
    }

    if (test_expect::failures == 0) std::printf("anim_body_bind_equivalence: OK\n");
    return test_expect::failures == 0 ? 0 : 1;
}
