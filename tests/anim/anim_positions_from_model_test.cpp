/* engine/runtime/anim positions_from_model over a first-person viewmodel rig
   read from its files (its synthetic exactness lives in anim_sample_test.cpp).
   The corpus-witnessed export relation (docs/net/novaworld-net-re.md section
   5.40; its witness pinned in bad_build_test):

       position[i] = bind_rows[parent(i)] . (-rel.x, rel.y, rel.z)

   1. a healthy rig (the minted 40-bone fixtures/threedi/synth/vm40.3di and its
      reset fixtures/anim/vm40_rst.bad, written by the clip construction seam
      through the same relation, as the stock M16/M24/M21/Frag _1st rigs are
      intact): the reconstruction matches BadBone.position within 5e-4 on every
      norm-consistent non-root bone, and at least 30 such bones exist;
   2. the broken rig (fixtures/anim/vm40_brk.bad, the X-triplicated breakage
      ak47_1st ships: pos.x copied into all three slots): the reconstruction is
      model-consistent (|derived| == |rel|, an orthogonal transform) on at least
      20 triplicated rows instead of echoing the stored values.

   The pairing is the runtime's: parts from the .3di LOD0 render-object table,
   bones from the reset .bad. */

#include <runtime/anim/anim_sample.h>

#include <formats/bad/bad.h>
#include <formats/threedi/threedi_3di3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "common/test_paths.h"

using namespace opennova::bad;
using namespace opennova::threedi;

using opennova::anim::Vec3;

namespace {

struct Rig {
    std::vector<Vec3> rel;   // LOD0 part pivots, parent-relative (ThreediRenderObject.rel)
    BadFile bad = {};        // the reset .bad: bind rows, stored positions, parents
};

// (bones, parts) for a viewmodel rig: parts from the .3di LOD0 render-object
// table, bones from its reset .bad. A file that does not read FAILS.
bool load_rig(const std::string &model_path, const std::string &bad_path, Rig &out) {
    Threedi3di3 model = {};
    if (threedi_3di3_read(model_path.c_str(), &model) != 0) {
        std::fprintf(stderr, "threedi_3di3_read failed for %s\n", model_path.c_str());
        return false;
    }
    if (model.lod_count < 1) {
        std::fprintf(stderr, "%s: no LOD table\n", model_path.c_str());
        threedi_3di3_free(&model);
        return false;
    }
    out.rel.clear();
    const ThreediLod &lod = model.lods[0];
    for (size_t i = 0; i < lod.render_object_count; ++i) {
        const ThreediRenderObject &part = lod.render_objects[i];
        out.rel.push_back(Vec3{ part.rel[0], part.rel[1], part.rel[2] });
    }
    threedi_3di3_free(&model);

    if (bad_parse(bad_path.c_str(), &out.bad) != 0) {
        std::fprintf(stderr, "bad_parse failed for %s\n", bad_path.c_str());
        return false;
    }
    return true;
}

double sq(double v) { return v * v; }
double norm3(const Vec3 &v) { return std::sqrt(sq(v.x) + sq(v.y) + sq(v.z)); }
double norm3(const float p[3]) { return std::sqrt(sq(p[0]) + sq(p[1]) + sq(p[2])); }

// The pytest's inputs, index-paired: ALL bind rows (the whole .bad), parents
// and rels trimmed to the paired range.
std::vector<Vec3> derive(const Rig &rig, size_t npair, std::vector<int> &parents) {
    parents.assign(npair, -1);
    for (size_t i = 0; i < npair; ++i) {
        parents[i] = rig.bad.bones[i].parent_index;
    }
    const std::vector<Vec3> rels(rig.rel.begin(), rig.rel.begin() + static_cast<std::ptrdiff_t>(npair));
    return opennova::anim::positions_from_model(rig.bad, parents, rels);
}

bool check_healthy(const char *name, const Rig &rig) {
    const size_t npair = std::min(rig.bad.num_bones, rig.rel.size());
    std::vector<int> parents;
    const std::vector<Vec3> derived = derive(rig, npair, parents);
    if (derived.size() != npair) {
        std::fprintf(stderr, "%s: derived %zu rows for %zu pairs\n", name, derived.size(), npair);
        return false;
    }
    bool ok = true;
    size_t checked = 0;
    double max_err = 0.0;
    for (size_t i = 0; i < npair; ++i) {
        if (parents[i] < 0 || static_cast<size_t>(parents[i]) == i) {
            continue; // root carries no parent-frame constraint
        }
        const float *shipped = rig.bad.bones[i].position;
        const double nd = norm3(rig.rel[i]);
        if (nd > 1e-6 && std::fabs(norm3(shipped) - nd) / nd >= 5e-3) {
            continue; // shipped value is stale/broken (norm-inconsistent)
        }
        const double err = std::sqrt(sq(derived[i].x - shipped[0]) + sq(derived[i].y - shipped[1]) +
                                     sq(derived[i].z - shipped[2]));
        max_err = std::max(max_err, err);
        if (err > 5e-4) {
            std::fprintf(stderr, "%s bone %zu: |derived-shipped|=%.2e\n", name, i, err);
            ok = false;
        }
        ++checked;
    }
    std::printf("[%s] healthy: checked=%zu of %zu pairs, max_err=%.2e\n", name, checked, npair, max_err);
    if (checked < 30) {
        std::fprintf(stderr, "%s: only %zu bones exercised\n", name, checked);
        ok = false;
    }
    return ok;
}

bool check_broken(const char *name, const Rig &rig) {
    const size_t npair = std::min(rig.bad.num_bones, rig.rel.size());
    std::vector<int> parents;
    const std::vector<Vec3> derived = derive(rig, npair, parents);
    if (derived.size() != npair) {
        std::fprintf(stderr, "%s: derived %zu rows for %zu pairs\n", name, derived.size(), npair);
        return false;
    }
    bool ok = true;
    size_t fixed = 0;
    for (size_t i = 0; i < npair; ++i) {
        if (parents[i] < 0 || static_cast<size_t>(parents[i]) == i) {
            continue;
        }
        const float *pos = rig.bad.bones[i].position;
        const double nd = norm3(rig.rel[i]);
        const bool triplicated = pos[0] == pos[1] && pos[1] == pos[2]; // bit-copied breakage
        if (triplicated && nd > 1e-3) {
            const double dn = norm3(derived[i]);
            if (!(std::fabs(dn - nd) < 5e-4 * std::max(1.0, nd))) {
                std::fprintf(stderr, "%s bone %zu: |derived|=%.6f vs |rel|=%.6f\n", name, i, dn, nd);
                ok = false;
            }
            ++fixed;
        }
    }
    std::printf("[%s] broken: triplicated rows reconstructed=%zu of %zu pairs\n", name, fixed, npair);
    if (fixed < 20) {
        std::fprintf(stderr, "%s: only %zu triplicated bones reconstructed\n", name, fixed);
        ok = false;
    }
    return ok;
}

} // namespace

int main() {
    const std::string model = test_paths_fixture("threedi/synth/vm40.3di");
    bool ok = true;
    {
        Rig rig;
        if (!load_rig(model, test_paths_fixture("anim/vm40_rst.bad"), rig)) {
            ok = false;
        } else {
            ok = check_healthy("vm40_rst", rig) && ok;
            bad_free(&rig.bad);
        }
    }
    {
        Rig rig;
        if (!load_rig(model, test_paths_fixture("anim/vm40_brk.bad"), rig)) {
            ok = false;
        } else {
            ok = check_broken("vm40_brk", rig) && ok;
            bad_free(&rig.bad);
        }
    }
    if (!ok) {
        return 1;
    }
    std::printf("anim_positions_from_model: OK\n");
    return 0;
}
