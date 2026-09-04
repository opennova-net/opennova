/* engine/runtime/anim positions_from_model over the retail viewmodel corpus —
   the retail half of the former tests/test_bad_pos_derivation.py (its synthetic
   half lives in anim_sample_test.cpp). The corpus-witnessed export relation
   (docs/net/novaworld-net-re.md section 5.40):

       position[i] = bind_rows[parent(i)] . (-rel.x, rel.y, rel.z)

   1. healthy rigs (M16_1st, M24_1st, M21_1st, Frag_1st — intact shipped bone
      tables): the reconstruction matches BadBone.position within 5e-4 on every
      norm-consistent non-root bone, and at least 30 such bones exist per rig;
   2. the broken rig (ak47_1st ships the X-triplicated breakage: pos.x copied
      into all three slots): the reconstruction is model-consistent
      (|derived| == |rel|, an orthogonal transform) on at least 20 triplicated
      rows instead of echoing the shipped values.

   Asset-gated on OPENNOVA_JO_ASSETS (an extracted JO assets dir, e.g. JOX);
   reports Skipped without it (docs/asset-gated-tests.md). Rig lookup is the
   runtime pairing the pytest used: parts from the .3di LOD0 render-object
   table, bones from the .adm's anim_reset .bad, all names matched
   case-insensitively over the flat directory. */

#include <runtime/anim/anim_sample.h>

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/threedi/threedi_3di3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "common/retail_paths.h"

using namespace opennova::adm;

namespace fs = std::filesystem;
using opennova::anim::Vec3;

namespace {

constexpr const char *kHealthyRigs[] = { "M16_1st", "M24_1st", "M21_1st", "Frag_1st" };
constexpr const char *kBrokenRig = "ak47_1st";

std::string ascii_lower(std::string s) {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// The pytest's reset-name extraction: quotes become spaces, first token wins.
std::string first_token_unquoted(const char *value) {
    std::string s(value);
    for (char &c : s) {
        if (c == '"') c = ' ';
    }
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = begin;
    while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) ++end;
    return s.substr(begin, end - begin);
}

std::string trimmed(const char *value) {
    std::string s(value);
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

using FileMap = std::map<std::string, fs::path>;

const fs::path *find_file(const FileMap &files, const std::string &lower_name) {
    const auto it = files.find(lower_name);
    return it == files.end() ? nullptr : &it->second;
}

struct Rig {
    std::vector<Vec3> rel;   // LOD0 part pivots, parent-relative (ThreediRenderObject.rel)
    BadFile bad = {};        // the anim_reset .bad: bind rows, shipped positions, parents
};

// (bones, parts) for a viewmodel rig — parts from the .3di LOD0 table, bones
// from the .adm's anim_reset .bad. Any missing or unparseable file FAILS (the
// pytest errored there too); the gate is the env var, not the corpus content.
bool load_rig(const FileMap &files, const char *name, Rig &out) {
    const std::string lower = ascii_lower(name);
    const fs::path *model_path = find_file(files, lower + ".3di");
    const fs::path *adm_path = find_file(files, lower + ".adm");
    if (model_path == nullptr || adm_path == nullptr) {
        std::fprintf(stderr, "%s: no %s.3di / %s.adm under OPENNOVA_JO_ASSETS\n", name, name, name);
        return false;
    }

    AdmFile af = {};
    if (adm_parse(adm_path->string().c_str(), &af) != 0) {
        std::fprintf(stderr, "%s: adm_parse failed for %s\n", name, adm_path->string().c_str());
        return false;
    }
    std::string reset;
    for (size_t i = 0; i < af.count; ++i) {
        if (ascii_lower(trimmed(af.entries[i].key)) == "anim_reset") {
            if (af.entries[i].variant_count >= 1) {
                reset = first_token_unquoted(af.entries[i].variants[0]);
            }
            break;
        }
    }
    adm_free(&af);
    if (reset.empty()) {
        std::fprintf(stderr, "%s: no anim_reset in %s\n", name, adm_path->string().c_str());
        return false;
    }
    const std::string reset_lower = ascii_lower(reset);
    const fs::path *bad_path = find_file(files, reset_lower);
    if (bad_path == nullptr) bad_path = find_file(files, reset_lower + ".bad");
    if (bad_path == nullptr) {
        std::fprintf(stderr, "%s: anim_reset clip %s not found\n", name, reset.c_str());
        return false;
    }

    Threedi3di3 model = {};
    if (threedi_3di3_read(model_path->string().c_str(), &model) != 0) {
        std::fprintf(stderr, "%s: threedi_3di3_read failed for %s\n", name, model_path->string().c_str());
        return false;
    }
    if (model.lod_count < 1) {
        std::fprintf(stderr, "%s: no LOD table\n", name);
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

    if (bad_parse(bad_path->string().c_str(), &out.bad) != 0) {
        std::fprintf(stderr, "%s: bad_parse failed for %s\n", name, bad_path->string().c_str());
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
    RETAIL_REQUIRE_OR_SKIP(assets, retail::assets(),
            "OPENNOVA_JO_ASSETS (an extracted JO asset tree with the .3di/.adm corpus)");
    const char *jox = assets.c_str();

    FileMap files;
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(fs::path(jox), ec)) {
        if (entry.is_regular_file()) {
            files[ascii_lower(entry.path().filename().string())] = entry.path();
        }
    }
    if (ec || files.empty()) {
        std::fprintf(stderr, "anim_positions_from_model_corpus: cannot list OPENNOVA_JO_ASSETS=%s\n", jox);
        return 1;
    }

    bool ok = true;
    for (const char *name : kHealthyRigs) {
        Rig rig;
        if (!load_rig(files, name, rig)) {
            ok = false;
            continue;
        }
        ok = check_healthy(name, rig) && ok;
        bad_free(&rig.bad);
    }
    {
        Rig rig;
        if (!load_rig(files, kBrokenRig, rig)) {
            ok = false;
        } else {
            ok = check_broken(kBrokenRig, rig) && ok;
            bad_free(&rig.bad);
        }
    }
    if (!ok) {
        return 1;
    }
    std::printf("anim_positions_from_model_corpus: OK\n");
    return 0;
}
