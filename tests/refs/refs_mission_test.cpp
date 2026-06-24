// .bms extractor: edges must agree with what the bms parser itself reports for
// the committed retail fixture (terrain/environment header refs + the
// items.def edge when entities exist), and malformed bytes must fail soft.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "common/test_paths.h"
#include "mission/bms.h"
#include "mission/mission.h"
#include "refs/refs.h"

using opennova::refs::Reference;

static int fail_count = 0;

#define EXPECT_TRUE(cond)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL at line " << __LINE__ << ": " #cond << std::endl; \
            fail_count++;                                                        \
        }                                                                        \
    } while (0)

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

static const Reference* find_site(const std::vector<Reference>& edges, const std::string& site) {
    for (const Reference& r : edges) {
        if (r.site == site) return &r;
    }
    return nullptr;
}

static std::string temp_path(const char* name) {
    const std::filesystem::path dir =
        std::filesystem::path(test_paths_repo_root(__FILE__)) / "build" / "test-output";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

static void test_fixture_header_and_item_edges() {
    const char* repo_root = test_paths_repo_root(__FILE__);
    const std::string path = std::string(repo_root) + "/fixtures/bms/ash_i5b.reference.bms";
    const std::vector<uint8_t> bytes = read_file(path);
    EXPECT_TRUE(!bytes.empty());

    opennova::bms::File file;
    std::string error;
    EXPECT_TRUE(opennova::bms::parse(bytes.data(), bytes.size(), file, error));
    const std::string expected_env(file.header.environment,
                                   strnlen(file.header.environment, sizeof(file.header.environment)));
    EXPECT_TRUE(!file.get_terrain().empty());
    EXPECT_TRUE(!expected_env.empty());
    const size_t entity_count =
        file.items.size() + file.buildings.size() + file.markers.size() + file.organics.size();
    EXPECT_TRUE(entity_count > 0);

    std::vector<Reference> edges;
    EXPECT_TRUE(opennova::refs::extract("ash_i5b.reference.bms", bytes.data(), bytes.size(), edges, error));
    EXPECT_TRUE(edges.size() == 3u);  // terrain + environment + items.def

    const Reference* terrain = find_site(edges, "header.terrain");
    EXPECT_TRUE(terrain != nullptr);
    if (terrain != nullptr) {
        EXPECT_TRUE(terrain->source_kind == "mission");
        EXPECT_TRUE(terrain->target_kind == "terrain");
        EXPECT_TRUE(terrain->target_name == file.get_terrain());
    }
    const Reference* env = find_site(edges, "header.environment");
    EXPECT_TRUE(env != nullptr);
    if (env != nullptr) {
        EXPECT_TRUE(env->target_kind == "environment");
        EXPECT_TRUE(env->target_name == expected_env);
    }
    const Reference* items = find_site(edges, "entity item table");
    EXPECT_TRUE(items != nullptr);
    if (items != nullptr) {
        EXPECT_TRUE(items->target_kind == "item_defs");
        EXPECT_TRUE(items->target_name == "items.def");
    }
}

static void test_writer_mis_header_and_item_edges() {
    using namespace opennova::mission;

    MissionDocument doc;
    doc.create_default();
    EXPECT_TRUE(doc.set_header_string("terrain", "dvxi5"));
    EXPECT_TRUE(doc.set_header_string("environment", "full_00"));

    EntityTransform transform;
    transform.x = 1.0f;
    EntityRecord added;
    EXPECT_TRUE(doc.add_entity(EntityKind::Item, 101291, transform, &added));

    const std::string path = temp_path("refs_writer_mission.mis");
    EXPECT_TRUE(doc.save_mis_file(path));
    const std::vector<uint8_t> bytes = read_file(path);
    EXPECT_TRUE(!bytes.empty());

    std::vector<Reference> edges;
    std::string error;
    EXPECT_TRUE(opennova::refs::extract("refs_writer_mission.mis", bytes.data(), bytes.size(), edges, error));
    EXPECT_TRUE(edges.size() == 3u);  // terrain + environment + items.def

    const Reference* terrain = find_site(edges, "header.terrain");
    EXPECT_TRUE(terrain != nullptr);
    if (terrain != nullptr) {
        EXPECT_TRUE(terrain->source_kind == "mission");
        EXPECT_TRUE(terrain->target_kind == "terrain");
        EXPECT_TRUE(terrain->target_name == "dvxi5");
    }
    const Reference* env = find_site(edges, "header.environment");
    EXPECT_TRUE(env != nullptr);
    if (env != nullptr) {
        EXPECT_TRUE(env->target_kind == "environment");
        EXPECT_TRUE(env->target_name == "full_00");
    }
    const Reference* items = find_site(edges, "entity item table");
    EXPECT_TRUE(items != nullptr);
    if (items != nullptr) {
        EXPECT_TRUE(items->target_kind == "item_defs");
        EXPECT_TRUE(items->target_name == "items.def");
    }
}

static void test_malformed_bms_fails_soft() {
    const uint8_t junk[] = {0x42, 0x4D, 0x53, 0x00, 0x01};
    std::vector<Reference> edges;
    std::string error;
    EXPECT_TRUE(!opennova::refs::extract("broken.bms", junk, sizeof(junk), edges, error));
    EXPECT_TRUE(!error.empty());
    EXPECT_TRUE(edges.empty());
}

int main() {
    test_fixture_header_and_item_edges();
    test_writer_mis_header_and_item_edges();
    test_malformed_bms_fails_soft();
    if (fail_count > 0) {
        std::cerr << fail_count << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "refs_mission_test: all passed" << std::endl;
    return 0;
}
