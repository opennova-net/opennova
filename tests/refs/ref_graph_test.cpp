// RefGraph over an in-memory file map: referrers as the inverse of references,
// case-insensitive keys, the (size, mtime) memo (zero-mtime PFF rows always
// re-extract), dropped files, fail-soft errors, and broken-reference flags
// through a stub exists callback.
#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "cbin/cbin.h"
#include "env/env.h"
#include "refs/ref_graph.h"

using opennova::refs::BuildStats;
using opennova::refs::GraphFileInfo;
using opennova::refs::RefGraph;
using opennova::refs::Reference;

static int fail_count = 0;

#define EXPECT_TRUE(cond)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL at line " << __LINE__ << ": " #cond << std::endl; \
            fail_count++;                                                        \
        }                                                                        \
    } while (0)

#define EXPECT_EQ_SZ(a, b)                                                                       \
    do {                                                                                         \
        const size_t va = (a), vb = (b);                                                         \
        if (va != vb) {                                                                          \
            std::cerr << "FAIL at line " << __LINE__ << ": " << va << " != " << vb << std::endl; \
            fail_count++;                                                                        \
        }                                                                                        \
    } while (0)

// --- In-memory root ---------------------------------------------------------

struct MemRoot {
    std::map<std::string, std::vector<uint8_t>> files;  // path -> bytes
    uint64_t mtime = 100;

    void put(const std::string& path, const std::string& bytes) {
        files[path] = std::vector<uint8_t>(bytes.begin(), bytes.end());
    }

    std::vector<GraphFileInfo> listing(uint64_t mtime_override = 0) const {
        std::vector<GraphFileInfo> out;
        for (const auto& [path, bytes] : files) {
            GraphFileInfo info;
            info.path = path;
            info.size_bytes = bytes.size();
            info.modified_time = mtime_override != 0 ? mtime_override : mtime;
            out.push_back(info);
        }
        return out;
    }

    opennova::refs::ReadFileFn reader() const {
        return [this](const std::string& path, std::vector<uint8_t>& out) {
            auto it = files.find(path);
            if (it == files.end()) return false;
            out = it->second;
            return true;
        };
    }
};

static std::string env_bytes(const std::string& sky1) {
    opennova::env::Config cfg;
    cfg.sky_map1 = sky1;
    std::ostringstream body;
    std::string error;
    opennova::env::save_env(body, cfg, error);
    return body.str();
}

static std::string kda_bytes(const std::string& font) {
    cbin::Credits credits;
    credits.entries.push_back(cbin::Entry::make_text("Someone", font));
    std::vector<uint8_t> bytes;
    std::string error;
    cbin::encode(credits, bytes, error);
    return std::string(bytes.begin(), bytes.end());
}

static MemRoot demo_root() {
    MemRoot root;
    root.put("skies/Desert.env", env_bytes("MyClouds.pcx"));
    root.put("Credits.kda", kda_bytes("BigFont"));
    root.put("items.def",
             "begin \"Crate\"\n  id 10\n  type object\n  graphic Wcrate5\nend\n");
    root.put("notes.txt", "not a recognized format");
    return root;
}

// --- Tests --------------------------------------------------------------------

static void test_build_and_inverse_queries() {
    MemRoot root = demo_root();
    RefGraph graph;
    const BuildStats stats = graph.build(root.listing(), root.reader());
    EXPECT_EQ_SZ(stats.files, 4u);
    EXPECT_EQ_SZ(stats.recognized, 3u);  // .txt skipped
    EXPECT_EQ_SZ(stats.extracted, 3u);
    EXPECT_EQ_SZ(stats.failed, 0u);

    // Case-insensitive source query, verbatim records.
    const std::vector<Reference> env_refs = graph.references_of("SKIES/DESERT.ENV");
    EXPECT_TRUE(!env_refs.empty());
    bool saw_sky = false;
    for (const Reference& r : env_refs) {
        if (r.site == "sky_map1") {
            saw_sky = true;
            EXPECT_TRUE(r.target_name == "MyClouds.pcx");
        }
    }
    EXPECT_TRUE(saw_sky);

    // referrers_of is the inverse of references_of, for every edge in the graph.
    for (const std::string path : {"skies/Desert.env", "Credits.kda", "items.def"}) {
        for (const Reference& edge : graph.references_of(path)) {
            bool found = false;
            for (const Reference& back : graph.referrers_of(edge.target_name)) {
                if (back.source_path == edge.source_path && back.site == edge.site) found = true;
            }
            EXPECT_TRUE(found);
        }
    }
    // ...and matches case-insensitively on the target.
    EXPECT_TRUE(!graph.referrers_of("BIGFONT").empty());
    EXPECT_TRUE(graph.referrers_of("bigfont")[0].source_path == "Credits.kda");
}

static void test_memo_hits_and_mtime_invalidation() {
    MemRoot root = demo_root();
    RefGraph graph;
    graph.build(root.listing(), root.reader());

    // Same stamps -> everything memoized, nothing re-extracted.
    BuildStats again = graph.build(root.listing(), root.reader());
    EXPECT_EQ_SZ(again.memo_hits, 3u);
    EXPECT_EQ_SZ(again.extracted, 0u);

    // Touch one file (content + listing stamp): only it re-extracts.
    root.put("skies/Desert.env", env_bytes("NewSky.pcx"));
    std::vector<GraphFileInfo> listing = root.listing();
    for (GraphFileInfo& info : listing) {
        if (info.path == "skies/Desert.env") info.modified_time = 200;
    }
    BuildStats touched = graph.build(listing, root.reader());
    EXPECT_EQ_SZ(touched.extracted, 1u);
    EXPECT_EQ_SZ(touched.memo_hits, 2u);
    EXPECT_TRUE(graph.referrers_of("NewSky.pcx").size() == 1u);
    EXPECT_TRUE(graph.referrers_of("MyClouds.pcx").empty());
}

static void test_zero_mtime_always_reextracts() {
    MemRoot root = demo_root();
    RefGraph graph;
    graph.build(root.listing(0xFFFF), root.reader());
    // PFF members carry no mtime: a zero stamp must never memo-hit.
    std::vector<GraphFileInfo> listing = root.listing();
    for (GraphFileInfo& info : listing) info.modified_time = 0;
    BuildStats stats = graph.build(listing, root.reader());
    EXPECT_EQ_SZ(stats.memo_hits, 0u);
    EXPECT_EQ_SZ(stats.extracted, 3u);
}

static void test_removed_file_drops_out() {
    MemRoot root = demo_root();
    RefGraph graph;
    graph.build(root.listing(), root.reader());
    EXPECT_TRUE(!graph.references_of("Credits.kda").empty());
    root.files.erase("Credits.kda");
    graph.build(root.listing(), root.reader());
    EXPECT_TRUE(graph.references_of("Credits.kda").empty());
    EXPECT_TRUE(graph.referrers_of("BigFont").empty());
}

static void test_parse_failure_is_fail_soft() {
    MemRoot root = demo_root();
    root.put("broken.kda", std::string("CBIN\x01\x02", 6));
    RefGraph graph;
    BuildStats stats = graph.build(root.listing(), root.reader());
    EXPECT_EQ_SZ(stats.failed, 1u);
    EXPECT_EQ_SZ(stats.extracted, 3u);
    EXPECT_TRUE(graph.errors().count("broken.kda") == 1u);
    EXPECT_TRUE(graph.references_of("broken.kda").empty());
}

static void test_broken_references_via_exists_stub() {
    MemRoot root = demo_root();
    RefGraph graph;
    graph.build(root.listing(), root.reader());
    // Only the crate's graphic "exists"; every other target is broken.
    const std::vector<Reference> broken = graph.broken_references(
        [](const std::string& name, const std::string&) { return name == "Wcrate5"; });
    EXPECT_TRUE(!broken.empty());
    for (const Reference& r : broken) {
        EXPECT_TRUE(r.target_name != "Wcrate5");
    }
    size_t total_edges = 0;
    for (const std::string path : {"skies/Desert.env", "Credits.kda", "items.def"}) {
        total_edges += graph.references_of(path).size();
    }
    EXPECT_EQ_SZ(broken.size(), total_edges - 1u);
}

int main() {
    test_build_and_inverse_queries();
    test_memo_hits_and_mtime_invalidation();
    test_zero_mtime_always_reextracts();
    test_removed_file_drops_out();
    test_parse_failure_is_fail_soft();
    test_broken_references_via_exists_stub();
    if (fail_count > 0) {
        std::cerr << fail_count << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "ref_graph_test: all passed" << std::endl;
    return 0;
}
