// The texture candidate rule (base/resource_index/texture_candidates.h): the
// file names a texture reference may resolve to, in the probe order the Godot
// resolvers and opennova-3di's `scene` share. Our loose-folder policy, not a
// retail port, so it is pinned here through the public API (ADR 0018).
#include <cstdio>
#include <string>
#include <vector>

#include <base/resource_index/texture_candidates.h>

using namespace opennova;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

using Names = std::vector<std::string>;

// Every fallback name of one stem, in the extension priority order.
Names with_every_extension(const std::string &stem) {
    Names names;
    for (const char *ext : {"tga", "dds", "dds.tga", "mdt", "pcx", "png", "jpg", "jpeg", "bmp"})
        names.push_back(stem + "." + ext);
    return names;
}

size_t index_of(const Names &names, const std::string &name) {
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return i;
    return names.size();
}

// The name as given, then the stem and its `_O` twin, each with every
// extension; the given name is not repeated where the stem list meets it.
void test_order_is_name_then_stem_then_twin() {
    Names expected = with_every_extension("wall");
    const Names twin = with_every_extension("wall_O");
    expected.insert(expected.end(), twin.begin(), twin.end());
    CHECK(texture_candidate_filenames("wall.tga") == expected);

    // A non-texture extension leads with the name as given, then falls back.
    Names other = {"wall.xyz"};
    const Names stems = with_every_extension("wall");
    other.insert(other.end(), stems.begin(), stems.end());
    other.insert(other.end(), twin.begin(), twin.end());
    CHECK(texture_candidate_filenames("wall.xyz") == other);

    // No extension at all: the bare name first.
    const Names bare = texture_candidate_filenames("wall");
    CHECK(bare.size() == 19 && bare.front() == "wall" && bare[1] == "wall.tga");
}

// A compound extension exposes each inner texture name, outermost first,
// straight after the name as given; a non-texture inner extension stops it.
void test_compound_extension_collapses() {
    const Names two = texture_candidate_filenames("x.dds.tga");
    CHECK(two.size() > 2 && two[0] == "x.dds.tga" && two[1] == "x.dds");
    // The stems drop only the last extension.
    CHECK(two[2] == "x.dds.dds");
    CHECK(index_of(two, "x.dds_O.tga") < two.size());

    const Names three = texture_candidate_filenames("a.tga.dds.tga");
    CHECK(three.size() > 3 && three[0] == "a.tga.dds.tga" && three[1] == "a.tga.dds" &&
          three[2] == "a.tga");

    // "bak" is no texture extension, so nothing inner is tried.
    const Names stopped = texture_candidate_filenames("x.bak.tga");
    CHECK(stopped.size() > 1 && stopped[0] == "x.bak.tga" && stopped[1] == "x.bak.dds");
    CHECK(index_of(stopped, "x.bak") == stopped.size());
}

// A stem gains an `_O` twin unless it already ends in `_O`, in either case.
void test_overlay_twin() {
    CHECK(index_of(texture_candidate_filenames("sign.tga"), "sign_O.bmp") < 18);
    const Names upper = texture_candidate_filenames("sign_O.tga");
    CHECK(upper == with_every_extension("sign_O"));
    const Names lower = texture_candidate_filenames("sign_o.tga");
    CHECK(lower == with_every_extension("sign_o"));
    CHECK(index_of(lower, "sign_o_O.tga") == lower.size());
}

// Directories go (either separator); the given name keeps its case, and the
// fallbacks are lower-case extensions (the caller's directory match folds case).
void test_directories_and_case() {
    const Names names = texture_candidate_filenames("textures/sub\\Wall.TGA");
    CHECK(names.size() == 19);
    CHECK(names[0] == "Wall.TGA" && names[1] == "Wall.tga" && names[2] == "Wall.dds");
    CHECK(names.back() == "Wall_O.bmp");
    CHECK(texture_candidate_filenames("").empty());
    CHECK(texture_candidate_filenames("textures/").empty());
}

} // namespace

int main() {
    test_order_is_name_then_stem_then_twin();
    test_compound_extension_collapses();
    test_overlay_twin();
    test_directories_and_case();
    if (failures == 0) std::printf("texture_candidates_test: ok\n");
    return failures == 0 ? 0 : 1;
}
