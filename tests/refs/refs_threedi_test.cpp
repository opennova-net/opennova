// .3di extractor: edges must equal the texture-slot names the 3DI3 parser
// itself reports for a committed retail fixture (self-consistent, no
// hand-maintained expectations), and malformed bytes must fail soft.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "common/test_paths.h"
#include "refs/refs.h"
#include "threedi/threedi_3di3.h"

using opennova::refs::Reference;

static int fail_count = 0;

#define EXPECT_TRUE(cond)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL at line " << __LINE__ << ": " #cond << std::endl; \
            fail_count++;                                                        \
        }                                                                        \
    } while (0)

static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> bytes;
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) return bytes;
    fseek(f, 0, SEEK_END);
    bytes.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    if (!bytes.empty() && fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
    fclose(f);
    return bytes;
}

static void test_fixture_edges_match_parser() {
    const char* repo_root = test_paths_repo_root(__FILE__);
    const std::string path = std::string(repo_root) + "/fixtures/threedi/mp5_1st.3di";
    const std::vector<uint8_t> bytes = read_file(path);
    EXPECT_TRUE(!bytes.empty());

    // Expected: the distinct (case-insensitive) texture names across every
    // material slot, straight from the format parser.
    Threedi3di3 model;
    std::memset(&model, 0, sizeof(model));
    EXPECT_TRUE(threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0);
    std::set<std::string> expected;
    for (uint32_t m = 0; m < model.material_count; ++m) {
        for (size_t t = 0; t < 24; ++t) {
            const ThreediMaterialTexture& tex = model.materials[m].textures[t];
            if (tex.name[0] != '\0') expected.insert(lower(tex.name));
        }
    }
    threedi_3di3_free(&model);
    EXPECT_TRUE(!expected.empty());

    std::vector<Reference> edges;
    std::string error;
    EXPECT_TRUE(opennova::refs::extract("mp5_1st.3di", bytes.data(), bytes.size(), edges, error));
    std::set<std::string> got;
    for (const Reference& r : edges) {
        EXPECT_TRUE(r.source_kind == "object_model");
        EXPECT_TRUE(r.target_kind == "texture");
        EXPECT_TRUE(r.site.rfind("material[", 0) == 0);
        got.insert(lower(r.target_name));
    }
    EXPECT_TRUE(got == expected);
    EXPECT_TRUE(edges.size() == expected.size());  // dedupe across materials
}

static void test_malformed_3di_fails_soft() {
    const uint8_t junk[] = {0x33, 0x44, 0x49, 0x33, 0x00, 0x01};
    std::vector<Reference> edges;
    std::string error;
    EXPECT_TRUE(!opennova::refs::extract("broken.3di", junk, sizeof(junk), edges, error));
    EXPECT_TRUE(!error.empty());
    EXPECT_TRUE(edges.empty());
}

int main() {
    test_fixture_edges_match_parser();
    test_malformed_3di_fails_soft();
    if (fail_count > 0) {
        std::cerr << fail_count << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "refs_threedi_test: all passed" << std::endl;
    return 0;
}
