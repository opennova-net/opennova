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
#include "vfs/vfs.h"
#include "vfs/vfs_decode.h"

namespace fs = std::filesystem;
using opennova::Vfs;
using opennova::VfsLookupPolicy;
using opennova::VfsSource;

// SCR keys (mirror libs/scr/include/scr/scr.h; vfs_test doesn't link opennova_scr).
static const uint32_t SCR_KEY_DEFAULT_C = 0xABEEFACEu; // JO Demo
static const uint32_t SCR_KEY_JO_DFX2_C = 0x2A5A8EADu; // retail JO/DFX2

// Produce the stored SCR form of `plaintext` under `key`: encryption is the inverse of
// scr_decrypt (which reverses then XORs), i.e. XOR with the keystream then reverse the bytes.
// The 4-byte "SCR" + version header is prepended by the caller.
static std::string scr_encrypt(const std::string &plaintext, uint32_t key) {
    std::string b = plaintext;
    uint32_t k = key;
    for (size_t i = 0; i < b.size(); ++i) {
        k = (((k + ((k << 11) | (k >> 21))) << 4) | ((k + ((k << 11) | (k >> 21))) >> 28)) ^ 1u;
        b[i] = static_cast<char>(static_cast<uint8_t>(b[i]) ^ static_cast<uint8_t>(k));
    }
    std::reverse(b.begin(), b.end());
    return b;
}

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

static std::string read_vfs(const Vfs &v, const char *name, VfsLookupPolicy policy) {
    std::vector<uint8_t> out;
    if (!v.read_file(name, out, policy)) return std::string("<none>");
    return std::string(out.begin(), out.end());
}

// Build a one-entry modern PFF whose single file `name` holds `content`.
static void write_pff1(const fs::path &path, const char *name, const std::string &content) {
    PffTestEntry e = { name, reinterpret_cast<const uint8_t *>(content.data()),
                       static_cast<uint32_t>(content.size()), 0 };
    pff_test_write_modern(path.string().c_str(), &e, 1);
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
    CHECK(v.mounted_expansion() == "jox01", "the expansion that mounted is reported");
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
    // The fallback is silent in the return value, so the caller's only evidence is this:
    // an uninstalled expansion reports base game, never the name that was requested. The
    // LAN joiner's host-expansion reconcile depends on it (D-NET-178).
    CHECK(v.mounted_expansion().empty(), "the silent base fallback reports no expansion");
    CHECK(v.mount_game(root.string(), ""), "remount base");
    CHECK(v.mounted_expansion().empty(), "base game reports no expansion");
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

// D-VFS-1: retail keeps one session default (/d) but selected consumers save/set/restore
// the loose-first flag around a single lookup. A normal packed mount is archive-first;
// foliage/UI callers can force loose-first, and BMS-from-PFF can force archive-only even
// when /d made loose-first the session default. [orig: FileSystem_OpenFile @ 0x75b1c0;
// Terrain_LoadFoliageFile @ 0x60a74e; Mission_LoadBMSFromPFF @ 0x40d43c]
static int test_per_call_resolution_policy() {
    using opennova::VfsMountMode;
    fs::path root = fresh_dir("per_call_policy");
    write_loose(root / "shared.dat", "LOOSE");
    write_loose(root / "loose_only.dat", "LOOSE_ONLY");
    {
        PffTestEntry es[2] = {
            { "shared.dat", reinterpret_cast<const uint8_t *>("ARCHIVE"), 7, 0 },
            { "packed_only.dat", reinterpret_cast<const uint8_t *>("PACKED_ONLY"), 11, 0 },
        };
        pff_test_write_modern((root / "resource.pff").string().c_str(), es, 2);
    }

    Vfs v;
    CHECK(v.mount_game(root.string(), "", VfsMountMode::Packed), "mount normal packed runtime");
    CHECK(read_vfs(v, "shared.dat") == "ARCHIVE", "packed session default remains archive-only");
    CHECK(read_vfs(v, "shared.dat", VfsLookupPolicy::ForceLooseFirst) == "LOOSE",
          "one caller can force loose-first without remounting");
    CHECK(read_vfs(v, "shared.dat", VfsLookupPolicy::ForceArchiveOnly) == "ARCHIVE",
          "archive-only force reads the packed winner");
    CHECK(v.has_file("loose_only.dat", VfsLookupPolicy::ForceLooseFirst),
          "forced loose-first can reach a loose-only file");
    CHECK(!v.has_file("loose_only.dat", VfsLookupPolicy::ForceArchiveOnly),
          "archive-only force never falls through after an archive miss");

    CHECK(v.mount_game(root.string(), "", VfsMountMode::PackedWithLooseOverride),
          "mount /d runtime");
    CHECK(read_vfs(v, "shared.dat") == "LOOSE", "/d session default remains loose-first");
    CHECK(read_vfs(v, "shared.dat", VfsLookupPolicy::ForceArchiveOnly) == "ARCHIVE",
          "BMS-style force bypasses /d loose override");
    CHECK(read_vfs(v, "packed_only.dat", VfsLookupPolicy::ForceLooseFirst) == "PACKED_ONLY",
          "loose-first force still falls back to archives");
    return 1;
}

// D-VFS-3: the basename-strip switch is dead in retail. Loose probes receive the path
// verbatim (and therefore reach subdirectories), while the same qualified query does not
// alias a flat archive entry. Component matching is Win32-case-insensitive on every host.
// [orig: FileSystem_OpenFile @ 0x75b1c0; dead setter @ 0x75a590]
static int test_path_qualified_lookup_is_verbatim() {
    fs::path root = fresh_dir("qualified_lookup");
    write_loose(root / "MixedCase.DAT", "TOP");
    write_loose(root / "SubDir" / "MixedCase.DAT", "NESTED");
    write_pff1(root / "data.pff", "mixedcase.dat", "ARCHIVE");

    Vfs v;
    CHECK(v.add_search_path(root.string()), "add root search path");
    CHECK(v.add_secondary_archive((root / "data.pff").string()), "add flat archive entry");
    CHECK(read_vfs(v, "mixedcase.dat") == "TOP", "flat query resolves the top-level loose file");
    CHECK(read_vfs(v, "subdir\\mixedcase.dat", VfsLookupPolicy::SessionDefault) == "NESTED",
          "backslash-qualified query reaches the nested loose file case-insensitively");
    CHECK(read_vfs(v, "SUBDIR/MIXEDCASE.DAT", VfsLookupPolicy::SessionDefault) == "NESTED",
          "slash-qualified query has the same host-independent Win32 semantics");
    CHECK(read_vfs(v, "subdir\\mixedcase.dat", VfsLookupPolicy::ForceArchiveOnly) == "<none>",
          "qualified query never aliases a flat archive basename");
    return 1;
}

// D-VFS-10: unlike retail's unchecked path concatenation, the host confines every loose
// query to its mounted root. Root syntax, ADS/drive syntax, traversal, directory-shaped
// terminal syntax, and symlinks out of the root must all miss consistently in has/read.
// [orig: FileSystem_OpenFile @ 0x75b1c0; FileSystem_FileExists @ 0x75aa50]
static int test_retail_query_stays_inside_mounted_root() {
    fs::path root = fresh_dir("query_containment");
    fs::path outside = fresh_dir("query_outside");
    write_loose(root / "safe.dat", "SAFE");
    write_loose(outside / "secret.dat", "SECRET");

    Vfs v;
    CHECK(v.add_search_path(root.string()), "add contained search root");
    const auto rejected_by_has_and_read = [&](const std::string &query) {
        std::vector<uint8_t> bytes = { 0xA5u };
        const bool absent = !v.has_file(query, VfsLookupPolicy::SessionDefault);
        const bool unreadable = !v.read_file(query, bytes, VfsLookupPolicy::SessionDefault);
        return absent && unreadable && bytes.empty();
    };

    CHECK(read_vfs(v, "./safe.dat", VfsLookupPolicy::SessionDefault) == "SAFE",
          "an in-root relative dot component remains valid");
    CHECK(rejected_by_has_and_read((outside / "secret.dat").string()),
          "an absolute path cannot bypass the mounted root");
    CHECK(rejected_by_has_and_read("/secret.dat"), "a slash-rooted query is rejected");
    CHECK(rejected_by_has_and_read("\\secret.dat"), "a backslash-rooted query is rejected");
    CHECK(rejected_by_has_and_read("safe.dat:stream"), "colon/ADS syntax is rejected");
    CHECK(rejected_by_has_and_read("../query_outside/secret.dat"), "parent traversal is rejected");
    CHECK(rejected_by_has_and_read("safe.dat/"), "a terminal separator is not normalized to a file");
    CHECK(rejected_by_has_and_read("safe.dat/."), "a terminal dot component is not normalized to a file");

    std::error_code symlink_ec;
    fs::create_directory_symlink(outside, root / "escape", symlink_ec);
    if (!symlink_ec) {
        CHECK(rejected_by_has_and_read("escape/secret.dat"),
              "a directory symlink cannot escape the mounted root");
    }
    return 1;
}

// D-VFS-7: archive entries are uppercased in-place but otherwise compared exactly. The
// apparent trailing-space trim in PFF_FindEntry starts on the NUL and is dead as compiled;
// therefore a stored trailing space remains significant. [orig: PFF_FindEntry @ 0x7685d0]
static int test_archive_names_keep_trailing_spaces() {
    fs::path root = fresh_dir("archive_name_normalization");
    write_pff1(root / "resource.pff", "pad.dat ", "SPACED");

    Vfs v;
    CHECK(v.add_secondary_archive((root / "resource.pff").string()), "add archive");
    CHECK(read_vfs(v, "PAD.DAT ", VfsLookupPolicy::SessionDefault) == "SPACED",
          "case folds but the exact trailing space matches");
    CHECK(read_vfs(v, "pad.dat", VfsLookupPolicy::SessionDefault) == "<none>",
          "query without the stored space does not match");
    return 1;
}

// D-VFS-2 pin: the witnessed fixed boot table [orig: PFF_OpenAllArchives
// @ 0x4a4310, name table @ 0x829f90]. RetailTable (the default) mounts
// language.pff / localres.pff / resource.pff in slot order — slot order IS
// precedence — and NEVER an extra archive; ScanAll (the editor's browse
// index, a recorded deliberate divergence) sees everything.
static int test_mount_game_retail_table() {
    using opennova::VfsArchiveDiscovery;
    using opennova::VfsMountMode;
    fs::path root = fresh_dir("game_boot_table");
    // The same entry in two table slots pins slot order as precedence.
    write_pff1(root / "language.pff", "order.txt", "LANGUAGE");
    write_pff1(root / "localres.pff", "order.txt", "LOCALRES");
    write_pff1(root / "resource.pff", "res_only.txt", "RES");
    // Sorts first alphabetically — the old scan-all would have mounted it.
    write_pff1(root / "aa_extra.pff", "extra.txt", "EXTRA");

    {
        Vfs v;
        CHECK(v.mount_game(root.string()), "mount RetailTable default");
        CHECK(read_vfs(v, "order.txt") == "LANGUAGE", "slot order is precedence (language over localres)");
        CHECK(read_vfs(v, "res_only.txt") == "RES", "resource.pff mounted from its slot");
        CHECK(!v.has_file("extra.txt"), "an extra .pff never mounts in retail");
    }
    {
        Vfs v;
        CHECK(v.mount_game(root.string(), "", VfsMountMode::PackedWithLooseOverride,
                           VfsArchiveDiscovery::ScanAll), "mount ScanAll");
        CHECK(v.has_file("extra.txt"), "the editor's browse discovery sees the extra archive");
        CHECK(read_vfs(v, "order.txt") == "LANGUAGE", "table names still resolve under ScanAll");
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

// The SCR key follows the decode policy, not just the version byte. The JO Demo stamps version 1
// on files keyed with the DEFAULT key, while retail JO/DFX2 version-1 files use the JO_DFX2 key
// (which the version byte selects). So decoding a demo-style file needs FORCE_DEFAULT; plain
// version-detect picks the wrong key and yields garbage. Regression test for the demo .def bug.
static int test_scr_decode_policy() {
    const std::string plaintext = "begin \"Null\"\r\n  id 100000\r\n  type marker\r\nend\r\n";

    // A demo-style payload: "SCR" + version byte 1, body encrypted with the DEFAULT key.
    std::string demo = "SCR";
    demo.push_back('\x01');
    demo += scr_encrypt(plaintext, SCR_KEY_DEFAULT_C);

    auto as_bytes = [](const std::string &s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    };
    auto as_string = [](const std::vector<uint8_t> &v) {
        return std::string(v.begin(), v.end());
    };

    // FORCE_DEFAULT recovers the plaintext.
    std::vector<uint8_t> forced = as_bytes(demo);
    CHECK(opennova::vfs_decode_payload(forced, opennova::VFS_SCR_FORCE_DEFAULT), "decode FORCE_DEFAULT ok");
    CHECK(as_string(forced) == plaintext, "FORCE_DEFAULT recovers demo text");

    // VERSION_DETECT maps version 1 -> JO_DFX2 key: it still "decodes" (header stripped, same
    // length) but the bytes are wrong. This is exactly the broken default the demo hit.
    std::vector<uint8_t> detected = as_bytes(demo);
    CHECK(opennova::vfs_decode_payload(detected, opennova::VFS_SCR_VERSION_DETECT), "decode VERSION_DETECT ok");
    CHECK(detected.size() == plaintext.size(), "version-detect strips header, keeps length");
    CHECK(as_string(detected) != plaintext, "version-detect picks the wrong key for a demo file");

    // FORCE_JO_DFX2 on a real retail-style file (encrypted with the JO_DFX2 key) round-trips,
    // and that same key is what version-detect picks for version 1 — so retail is unaffected.
    std::string retail = "SCR";
    retail.push_back('\x01');
    retail += scr_encrypt(plaintext, SCR_KEY_JO_DFX2_C);
    std::vector<uint8_t> retail_detect = as_bytes(retail);
    CHECK(opennova::vfs_decode_payload(retail_detect, opennova::VFS_SCR_VERSION_DETECT), "decode retail version-detect ok");
    CHECK(as_string(retail_detect) == plaintext, "version-detect still correct for retail version-1 files");
    return 1;
}

// Vfs::set_scr_policy drives read_file's SCR keying end-to-end: a demo-style archived file
// (version 1, DEFAULT-keyed) reads back as plaintext only when the policy forces DEFAULT.
static int test_vfs_scr_policy() {
    const std::string plaintext = "begin \"Null\"\r\n  id 100000\r\nend\r\n";
    std::string stored = "SCR";
    stored.push_back('\x01');
    stored += scr_encrypt(plaintext, SCR_KEY_DEFAULT_C);

    fs::path d = fresh_dir("vfs_scr_policy");
    write_pff1(d / "data.pff", "demo.def", stored);

    // Default policy (version-detect) picks JO_DFX2 for version 1 -> not the plaintext.
    {
        Vfs v;
        v.add_secondary_archive((d / "data.pff").string());
        CHECK(read_vfs(v, "demo.def") != plaintext, "default policy mis-keys the demo file");
    }
    // FORCE_DEFAULT recovers it through the full read_file path.
    {
        Vfs v;
        v.add_secondary_archive((d / "data.pff").string());
        v.set_scr_policy(opennova::VFS_SCR_FORCE_DEFAULT);
        CHECK(read_vfs(v, "demo.def") == plaintext, "FORCE_DEFAULT decodes the demo file on read");
    }
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
    RUN_TEST(test_per_call_resolution_policy);
    RUN_TEST(test_path_qualified_lookup_is_verbatim);
    RUN_TEST(test_retail_query_stays_inside_mounted_root);
    RUN_TEST(test_archive_names_keep_trailing_spaces);
    RUN_TEST(test_mount_game_retail_table);
    RUN_TEST(test_scr_decode_on_read);
    RUN_TEST(test_scr_decode_policy);
    RUN_TEST(test_vfs_scr_policy);
    RUN_TEST(test_list_files);

    fs::remove_all(g_root, ec);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
