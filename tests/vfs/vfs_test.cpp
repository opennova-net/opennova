// Tests for the engine-faithful VFS: mount precedence, mount_game expansion override, and
// SCR decode-on-read. Fixtures are synthesized in a temp dir (no game data needed).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pff/pff_test_writer.h"
#include "scr/scr.h"
#include "vfs/vfs.h"

namespace fs = std::filesystem;
using opennova::Vfs;
using opennova::VfsSource;

static int passed = 0;
static int failed = 0;

#define RUN_TEST(fn) do { \
    printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } \
} while (0)

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } \
} while (0)

static std::string g_root;

// Fresh, empty temp subdir for a test.
static fs::path fresh_dir(const char *name) {
    fs::path d = fs::path(g_root) / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

static void write_loose(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static std::string read_vfs(const Vfs &v, const char *name) {
    std::vector<uint8_t> out;
    if (!v.read_file(name, out)) return std::string("<none>");
    return std::string(out.begin(), out.end());
}

// Build a one-entry modern PFF whose single file `name` holds `content`.
static void write_pff1(const fs::path &path, const char *name, const std::string &content) {
    PffTestEntry e = { name, reinterpret_cast<const uint8_t *>(content.data()),
                       static_cast<uint32_t>(content.size()), 0 };
    pff_test_write_modern(path.string().c_str(), &e, 1);
}

static uint32_t rol32(uint32_t value, int shift) {
    return (value << shift) | (value >> (32 - shift));
}

static std::string make_scr(uint8_t version, uint32_t key, const std::string &plain) {
    std::string payload = plain;
    for (char &c : payload) {
        key = rol32(key + rol32(key, 11), 4) ^ 1u;
        c = static_cast<char>(static_cast<uint8_t>(c) ^ static_cast<uint8_t>(key));
    }
    std::reverse(payload.begin(), payload.end());

    std::string scr = "SCR";
    scr.push_back(static_cast<char>(version));
    scr += payload;
    return scr;
}

// Loose files shadow archived files.
static int test_loose_over_archive() {
    fs::path d = fresh_dir("loose_over_archive");
    write_loose(d / "foo.txt", "LOOSE");
    write_pff1(d / "data.pff", "foo.txt", "ARCHIVE");

    Vfs v;
    CHECK(v.add_search_path(d.string()), "add search path");
    CHECK(v.add_secondary_archive((d / "data.pff").string()), "add archive");
    CHECK(v.has_file("foo.txt"), "has_file");
    CHECK(read_vfs(v, "foo.txt") == "LOOSE", "loose shadows archive");
    CHECK(read_vfs(v, "FOO.TXT") == "LOOSE", "case-insensitive lookup");
    return 1;
}

// Primary archive beats secondary archives.
static int test_primary_over_secondary() {
    fs::path d = fresh_dir("primary_over_secondary");
    write_pff1(d / "primary.pff", "bar.txt", "PRIMARY");
    write_pff1(d / "secondary.pff", "bar.txt", "SECONDARY");

    Vfs v;
    CHECK(v.set_primary_archive((d / "primary.pff").string()), "set primary");
    CHECK(v.add_secondary_archive((d / "secondary.pff").string()), "add secondary");
    CHECK(read_vfs(v, "bar.txt") == "PRIMARY", "primary wins");
    return 1;
}

// Search paths are consulted in add order.
static int test_search_path_order() {
    fs::path base = fresh_dir("search_order");
    fs::path d1 = base / "first";
    fs::path d2 = base / "second";
    write_loose(d1 / "baz.txt", "FIRST");
    write_loose(d2 / "baz.txt", "SECOND");

    Vfs v;
    v.add_search_path(d1.string());
    v.add_search_path(d2.string());
    CHECK(read_vfs(v, "baz.txt") == "FIRST", "first search path wins");
    return 1;
}

// mount_game: full override chain loose(expansion) > L.pff(primary) > main.pff > base archives.
static int test_mount_game_expansion() {
    fs::path root = fresh_dir("game");
    fs::path exp = root / "expansion" / "jox01";
    fs::create_directories(exp);

    // base archive in the root
    {
        PffTestEntry es[3] = {
            { "shared.txt",     reinterpret_cast<const uint8_t *>("BASE"), 4, 0 },
            { "arch_shared.txt",reinterpret_cast<const uint8_t *>("BASE"), 4, 0 },
            { "base_only.txt",  reinterpret_cast<const uint8_t *>("BASE_ONLY"), 9, 0 },
        };
        pff_test_write_modern((root / "resource.pff").string().c_str(), es, 3);
    }
    // expansion main + local archives
    {
        PffTestEntry es[3] = {
            { "shared.txt",      reinterpret_cast<const uint8_t *>("MAIN"), 4, 0 },
            { "arch_shared.txt", reinterpret_cast<const uint8_t *>("MAIN"), 4, 0 },
            { "expmain.txt",     reinterpret_cast<const uint8_t *>("EXP_MAIN_ONLY"), 13, 0 },
        };
        pff_test_write_modern((exp / "jox01.pff").string().c_str(), es, 3);
    }
    {
        PffTestEntry es[2] = {
            { "shared.txt",      reinterpret_cast<const uint8_t *>("LOCAL"), 5, 0 },
            { "arch_shared.txt", reinterpret_cast<const uint8_t *>("LOCAL"), 5, 0 },
        };
        pff_test_write_modern((exp / "jox01L.pff").string().c_str(), es, 2);
    }
    // loose file in the expansion dir
    write_loose(exp / "shared.txt", "LOOSE");

    Vfs v;
    CHECK(v.mount_game(root.string(), "jox01"), "mount_game jox01");
    CHECK(v.game_root() == fs::path(root).string(), "game_root recorded");
    CHECK(read_vfs(v, "shared.txt") == "LOOSE", "loose expansion file wins");
    CHECK(read_vfs(v, "arch_shared.txt") == "LOCAL", "L.pff (primary) beats main + base");
    CHECK(read_vfs(v, "expmain.txt") == "EXP_MAIN_ONLY", "expansion main beats base");
    CHECK(read_vfs(v, "base_only.txt") == "BASE_ONLY", "base archive reachable");
    return 1;
}

// mount_game falls back to base-game mounting for a missing expansion.
static int test_mount_game_no_expansion() {
    fs::path root = fresh_dir("game_base");
    write_pff1(root / "resource.pff", "only.txt", "BASE");
    Vfs v;
    CHECK(v.mount_game(root.string(), "doesnotexist"), "mount_game falls back");
    CHECK(read_vfs(v, "only.txt") == "BASE", "base archive reachable without expansion");
    return 1;
}

// mount_game mode selects which layers mount: LooseOnly (editor), Packed (shipping
// runtime), PackedWithLooseOverride (runtime under /d).
static int test_mount_game_modes() {
    using opennova::VfsMountMode;
    fs::path root = fresh_dir("game_modes");
    write_loose(root / "shared.txt", "LOOSE");
    {
        PffTestEntry es[2] = {
            { "shared.txt",   reinterpret_cast<const uint8_t *>("ARCHIVE"), 7, 0 },
            { "packed.txt",   reinterpret_cast<const uint8_t *>("PACKED_ONLY"), 11, 0 },
        };
        pff_test_write_modern((root / "resource.pff").string().c_str(), es, 2);
    }

    // LooseOnly: loose files only; archive-only entries are invisible.
    {
        Vfs v;
        CHECK(v.mount_game(root.string(), "", VfsMountMode::LooseOnly), "mount LooseOnly");
        CHECK(read_vfs(v, "shared.txt") == "LOOSE", "loose readable in LooseOnly");
        CHECK(!v.has_file("packed.txt"), "archive entry hidden in LooseOnly");
    }
    // Packed: archives only; loose files do not shadow and loose-only entries are gone.
    {
        Vfs v;
        CHECK(v.mount_game(root.string(), "", VfsMountMode::Packed), "mount Packed");
        CHECK(read_vfs(v, "shared.txt") == "ARCHIVE", "archive wins in Packed (no loose override)");
        CHECK(read_vfs(v, "packed.txt") == "PACKED_ONLY", "archive-only entry readable in Packed");
    }
    // PackedWithLooseOverride: both; loose shadows the archive.
    {
        Vfs v;
        CHECK(v.mount_game(root.string(), "", VfsMountMode::PackedWithLooseOverride), "mount PackedWithLooseOverride");
        CHECK(read_vfs(v, "shared.txt") == "LOOSE", "loose overrides archive under /d");
        CHECK(read_vfs(v, "packed.txt") == "PACKED_ONLY", "archive entry still reachable under /d");
    }
    return 1;
}

// read_file applies SCR payload decoding; read_file_raw does not (header stays).
static int test_scr_decode_on_read() {
    fs::path d = fresh_dir("scr_decode");
    // "SCR" + version 0 + 8 payload bytes
    std::string scr = "SCR";
    scr.push_back('\0');
    scr += "ABCDEFGH";
    write_loose(d / "enc.dat", scr);

    Vfs v;
    v.add_search_path(d.string());
    std::vector<uint8_t> raw, decoded;
    CHECK(v.read_file_raw("enc.dat", raw), "read_file_raw");
    CHECK(raw.size() == scr.size(), "raw keeps SCR header + payload");
    CHECK(v.read_file("enc.dat", decoded), "read_file");
    CHECK(decoded.size() == scr.size() - 4, "SCR 4-byte header stripped on decode");
    return 1;
}

static int test_scr_decode_land_warrior_key_fallback() {
    fs::path d = fresh_dir("scr_decode_dflw");
    const std::string plain = "define PLAYER01\nclass infantry\n";
    write_loose(d / "player.def", make_scr(1, SCR_KEY_DFLW, plain));

    Vfs v;
    v.add_search_path(d.string());
    CHECK(read_vfs(v, "player.def") == plain, "version 1 SCR falls back to Land Warrior key");
    return 1;
}

// Enumeration reports each logical name with its winning source.
static int test_list_files() {
    fs::path d = fresh_dir("listing");
    write_loose(d / "loosefile.txt", "x");
    write_pff1(d / "data.pff", "archived.txt", "y");

    Vfs v;
    v.add_search_path(d.string());
    v.add_secondary_archive((d / "data.pff").string());
    auto files = v.list_files();
    bool found_loose = false, found_arch = false;
    for (const auto &f : files) {
        if (f.logical_name == "loosefile.txt" && f.source == VfsSource::LooseDir) found_loose = true;
        if (f.logical_name == "archived.txt" && f.source == VfsSource::Archive &&
            f.source_path == (d / "data.pff").string()) found_arch = true;
    }
    CHECK(found_loose, "loose file enumerated");
    CHECK(found_arch, "archive entry enumerated with source path");
    return 1;
}

int main() {
    std::error_code ec;
    g_root = (fs::temp_directory_path(ec) / "opennova_vfs_test").string();
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);

    RUN_TEST(test_loose_over_archive);
    RUN_TEST(test_primary_over_secondary);
    RUN_TEST(test_search_path_order);
    RUN_TEST(test_mount_game_expansion);
    RUN_TEST(test_mount_game_no_expansion);
    RUN_TEST(test_mount_game_modes);
    RUN_TEST(test_scr_decode_on_read);
    RUN_TEST(test_scr_decode_land_warrior_key_fallback);
    RUN_TEST(test_list_files);

    fs::remove_all(g_root, ec);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
