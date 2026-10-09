// Tests for the engine-faithful VFS: mount precedence, mount_game expansion override, and
// SCR decode-on-read. Fixtures are synthesized in a temp dir (no game data needed).
#include <algorithm>
#include <formats/rtxt/rtxt.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_paths.h"
#include "pff/pff_test_writer.h"
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>

namespace fs = std::filesystem;
using opennova::Vfs;
using opennova::VfsLookupPolicy;
using opennova::VfsSource;
using opennova::vfs_expansion_version_checksum;
using opennova::ExpansionInfo;
using opennova::vfs_expansion_info;
namespace rtxt = opennova::rtxt;
using opennova::vfs_version_crc;

// SCR keys (mirror engine/formats/scr/scr.h; vfs_test doesn't link opennova_scr).
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
// Terrain_LoadTileInfoFile @ 0x60a74e; Mission_LoadBMSFromPFF @ 0x40d43c]
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
    // The loose-first answer a .dds-sibling loader asks follows the same policy.
    CHECK(!v.loose_first_hit("shared.dat", VfsLookupPolicy::SessionDefault),
          "a packed session finds no loose-first hit");
    CHECK(v.loose_first_hit("shared.dat", VfsLookupPolicy::ForceLooseFirst),
          "a forced loose-first lookup finds the loose file");
    CHECK(!v.loose_first_hit("packed_only.dat", VfsLookupPolicy::ForceLooseFirst),
          "an archive-only entry is no loose hit");
    CHECK(!v.loose_first_hit("shared.dat", VfsLookupPolicy::ForceArchiveOnly),
          "archive-only never has a loose hit");

    CHECK(v.mount_game(root.string(), "", VfsMountMode::PackedWithLooseOverride),
          "mount /d runtime");
    CHECK(read_vfs(v, "shared.dat") == "LOOSE", "/d session default remains loose-first");
    CHECK(read_vfs(v, "shared.dat", VfsLookupPolicy::ForceArchiveOnly) == "ARCHIVE",
          "BMS-style force bypasses /d loose override");
    CHECK(read_vfs(v, "packed_only.dat", VfsLookupPolicy::ForceLooseFirst) == "PACKED_ONLY",
          "loose-first force still falls back to archives");
    CHECK(v.loose_first_hit("shared.dat", VfsLookupPolicy::SessionDefault),
          "the /d session finds the loose hit");
    CHECK(!v.loose_first_hit("shared.dat", VfsLookupPolicy::ForceArchiveOnly),
          "an archive-only force ignores /d's loose hit");
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

// D-NET-166: the expansion version-file checksum. vfs_version_crc is retail's
// CRC_ComputeCustomTable @ 0x53c820 (MSB-first CRC-32, poly 0x04C11DB7, init
// -1, no reflection/final xor — "CRC-32/MPEG-2", whose standard check value
// for "123456789" is 0x0376E6E7); vfs_expansion_version_checksum is
// g_ExpansionChecksum's producer over the loose expansion/<name>/version.txt
// [orig: Expansion_LoadAssets @ 0x4a4781..0x4a488a].
static int test_expansion_version_checksum() {
    const char *check = "123456789";
    CHECK(vfs_version_crc(reinterpret_cast<const uint8_t *>(check), 9) ==
                  static_cast<int32_t>(0x0376E6E7),
          "vfs_version_crc matches the CRC-32/MPEG-2 check value");

    fs::path root = fresh_dir("crc_game");
    fs::path exp = root / "expansion" / "jox01";
    fs::create_directories(exp);
    CHECK(vfs_expansion_version_checksum(root.string(), "jox01") == 0,
          "no loose version.txt keeps the checksum 0 [orig: @ 0x4a487b]");
    CHECK(vfs_expansion_version_checksum(root.string(), "") == 0,
          "an empty expansion never probes the file [orig: @ 0x4a4787]");

    write_loose(exp / "version.txt", "123456789");
    CHECK(vfs_expansion_version_checksum(root.string(), "jox01") ==
                  static_cast<int32_t>(0x0376E6E7),
          "the loose version.txt bytes feed the CRC [orig: @ 0x4a4885]");

    write_loose(exp / "version.txt", "");
    CHECK(vfs_expansion_version_checksum(root.string(), "jox01") == 0,
          "an empty version.txt is treated like an absent one (retail-UB guard)");
    return 1;
}

// The expansion scan's name/description read [orig: Expansion_ScanAndRegister @ 0x4a43d0]:
// [exp_info] EXP_NAME / EXP_DESC out of <n>.bin, loose first, then <n>L.pff, then <n>.pff,
// each key with its own fallback.
static std::string exp_info_bin(const char *name, const char *desc) {
    rtxt::File file;
    file.sections.push_back({"exp_info", 0});
    if (name) { file.entries.push_back({"EXP_NAME", name, {}, 0}); ++file.sections[0].string_count; }
    if (desc) { file.entries.push_back({"EXP_DESC", desc, {}, 0}); ++file.sections[0].string_count; }
    std::vector<uint8_t> out;
    std::string error;
    if (!rtxt::write(file, out, error)) return std::string();
    return std::string(out.begin(), out.end());
}

static int test_expansion_info() {
    fs::path root = fresh_dir("exp_info_game");
    fs::path exp = root / "expansion" / "jox01";
    fs::create_directories(exp);

    ExpansionInfo none = vfs_expansion_info(root.string(), "jox01");
    CHECK(none.name == "Unnamed Expansion" && none.description == "This expansion lacks a description.",
          "no <n>.bin at all -> both fallbacks [orig: @ 0x4a4670 / @ 0x4a46b2]");

    write_pff1(exp / "jox01.pff", "jox01.bin", exp_info_bin("Base Name", nullptr));
    ExpansionInfo base = vfs_expansion_info(root.string(), "jox01");
    CHECK(base.name == "Base Name" && base.description == "This expansion lacks a description.",
          "the base archive serves the .bin; a missing EXP_DESC falls back alone [orig: @ 0x4a4648]");

    write_pff1(exp / "jox01L.pff", "jox01.bin", exp_info_bin(nullptr, "L description"));
    ExpansionInfo l = vfs_expansion_info(root.string(), "jox01");
    CHECK(l.name == "Unnamed Expansion" && l.description == "L description",
          "the L archive wins over the base one (the last primary installed) [orig: @ 0x4a450a]; "
          "a missing EXP_NAME falls back alone [orig: @ 0x4a45c2]");

    write_loose(exp / "jox01.bin", exp_info_bin("Loose Name", "Loose description"));
    ExpansionInfo loose = vfs_expansion_info(root.string(), "jox01");
    CHECK(loose.name == "Loose Name" && loose.description == "Loose description",
          "a loose <n>.bin beside the archives wins (the scan's search path is loose-first) [orig: @ 0x4a446a]");

    CHECK(vfs_expansion_info(root.string(), "").name == "Unnamed Expansion",
          "an empty expansion name probes nothing");
    return 1;
}

// The Mods list's records [orig: Expansion_ScanAndRegister @ 0x4a43d0]: every directory under
// expansion/ not starting with '.', whether or not its <n>.pff opens, in FindFirstFile's order over
// an NTFS directory (the names upper-cased), the first 16, each named by its <n>.bin.
static int test_expansion_records() {
    using opennova::ExpansionRecord;
    using opennova::vfs_expansion_records;
    fs::path root = fresh_dir("exp_records_game");
    CHECK(vfs_expansion_records(root.string()).empty(), "no expansion folder: no record");
    fs::path exp = root / "expansion";
    fs::create_directories(exp / "onx");
    write_pff1(exp / "onx" / "onxL.pff", "onx.bin", exp_info_bin("Storm", "A storm."));
    fs::create_directories(exp / "jox_a");
    fs::create_directories(exp / "joxb");
    fs::create_directories(exp / ".hidden");
    write_loose(exp / "afile.txt", "not a folder");
    std::vector<ExpansionRecord> records = vfs_expansion_records(root.string());
    CHECK(records.size() == 3, "the three folders, not the '.' one nor the file [orig: @ 0x4a445d]");
    CHECK(records[0].directory == "joxb" && records[1].directory == "jox_a" && records[2].directory == "onx",
          "upper-cased order: JOXB before JOX_A ('B' 0x42 < '_' 0x5F), which a lower-cased sort reverses");
    CHECK(records[2].info.name == "Storm" && records[2].info.description == "A storm.",
          "each record is named by its <n>.bin");
    CHECK(records[0].info.name == "Unnamed Expansion",
          "a folder with no <n>.pff is registered all the same, unnamed");
    for (int i = 0; i < 20; ++i) fs::create_directories(exp / ("z" + std::to_string(10 + i)));
    records = vfs_expansion_records(root.string());
    CHECK(records.size() == 16 && records.back().directory == "z22",
          "the scan registers 16 at most, in its order [orig: the count's compare @ 0x4a445d]");
    CHECK(opennova::expansion_info_from_bin({}).name == "Unnamed Expansion",
          "bytes that do not parse: both fallbacks");
    return 1;
}

// The expansion's text-override table [orig: Expansion_LoadAssets @ 0x4a49d4 ->
// TextResource_LoadOverrideTable @ 0x75d5c0 -> File_LoadResource @ 0x75b540]: only a
// loose expansion/<n>/<n>.bin serves it, never the archived copy; a load with the old
// archives open (the menu's and the join's switch) reaches it only under /d.
static int test_expansion_override_table() {
    using opennova::ExpansionLoadPoint;
    using opennova::vfs_expansion_override_table;
    const auto load = [](const fs::path &root, const char *exp, ExpansionLoadPoint point,
                         bool loose_first) {
        std::vector<uint8_t> out = {1, 2, 3};
        const bool ok = vfs_expansion_override_table(root.string(), exp, point, loose_first, out);
        if (!ok) return std::string(out.empty() ? "<none>" : "<none, output kept>");
        return std::string(out.begin(), out.end());
    };
    constexpr ExpansionLoadPoint kBoot = ExpansionLoadPoint::ArchivesClosed;
    constexpr ExpansionLoadPoint kSwitch = ExpansionLoadPoint::ArchivesOpen;

    fs::path root = fresh_dir("override_game");
    fs::path exp = root / "expansion" / "jox01";
    fs::create_directories(exp);
    write_loose(exp / "jox01.bin", "LOOSE");
    CHECK(load(root, "jox01", kBoot, false) == "<none>",
          "no <n>.pff: the expansion is cleared, and its table with it [orig: @ 0x4a4775, @ 0x4a482a]");

    write_pff1(exp / "jox01.pff", "other.txt", "x");
    write_pff1(exp / "jox01L.pff", "jox01.bin", "ARCHIVED");
    fs::remove(exp / "jox01.bin");
    CHECK(load(root, "jox01", kBoot, false) == "<none>",
          "the archived <n>.bin never serves the table (the whole query never matches an entry)");
    CHECK(load(root, "jox01", kBoot, true) == "<none>", "nor under /d");

    write_loose(exp / "jox01.bin", "LOOSE");
    CHECK(load(root, "jox01", kBoot, false) == "LOOSE",
          "at boot no archive is open: the loose walk serves expansion/<n>/<n>.bin [orig: @ 0x75b5a4]");
    CHECK(load(root, "jox01", kSwitch, false) == "<none>",
          "a switch with the old archives open walks only the archives [orig: @ 0x75b56c..0x75b57c]");
    CHECK(load(root, "jox01", kSwitch, true) == "LOOSE", "under /d the switch walks the loose file first");
    CHECK(load(root, "", kBoot, false) == "<none>", "no expansion, no table");

    write_loose(exp / "expansion" / "jox01" / "jox01.bin", "SEARCH");
    CHECK(load(root, "jox01", kBoot, false) == "SEARCH",
          "the search path's join comes first [orig: @ 0x4a49bb, @ 0x75b5b9]");
    fs::remove_all(exp / "expansion");

    write_loose(exp / "jox01.bin", "");
    CHECK(load(root, "jox01", kBoot, false) == "<none>", "an empty loose file is no table");
    return 1;
}

// An expansion's folder and archives by name [orig: Expansion_LoadAssets @ 0x4a4730; PFF_OpenAllArchives
// @ 0x4a4310], the files the game reads from its folder by path, the folders the Mods list
// registers, and a folder that holds a base game (one of the boot table's archives).
static int test_expansion_paths_and_names() {
    using namespace opennova;
    CHECK(vfs_expansion_dir("", "jxm") == "expansion/jxm", "an install's layout, relative");
    CHECK(vfs_expansion_dir("C:/JO", "jxm") == "C:/JO/expansion/jxm" &&
              vfs_expansion_dir("C:/JO/", "jxm") == "C:/JO/expansion/jxm",
          "under a root, its separator not doubled");
    CHECK(vfs_expansion_archive_name("jxm", true) == "jxmL.pff" && vfs_expansion_archive_name("jxm", false) == "jxm.pff",
          "<n>L.pff the language archive, <n>.pff the other");
    CHECK(vfs_expansion_archive_path("", "jxm", true) == "expansion/jxm/jxmL.pff" &&
              vfs_expansion_archive_path("", "jxm", false) == "expansion/jxm/jxm.pff",
          "the archives' paths in the folder");
    CHECK(vfs_expansion_archive_path("root", "x1", false) == "root/expansion/x1/x1.pff", "under a root");

    // Read from the folder by path: the table, the music banks, the five videos, gt.ssc (any case).
    for (const char *name : {"jxm.bin", "JXM.BIN", "Mjxm.sbf", "Gjxm.sbf", "main.bik", "HEADER.BIK", "footer.bik",
                             "prolog.bik", "intro.bik", "gt.ssc"})
        CHECK(vfs_read_from_expansion_folder(name, "jxm"), "a file the game reads from the expansion's folder");
    // Not the version text, a player's file, the music scripts (read through the archives) nor another
    // expansion's table or a video the game never plays.
    for (const char *name : {"version.txt", "weapon.sav", "Mjxm.bin", "Gjxm.bin", "x1.bin", "trailer.bik", "jxmL.lwf"})
        CHECK(!vfs_read_from_expansion_folder(name, "jxm"), "a file the game reads elsewhere, or never");

    CHECK(vfs_expansion_folder_listed("jox01") && vfs_expansion_folder_listed("a.b"), "a folder the Mods list registers");
    CHECK(!vfs_expansion_folder_listed(".mod") && !vfs_expansion_folder_listed(".") && !vfs_expansion_folder_listed(""),
          "a leading dot: never registered [orig: @ 0x4a444b]");
    CHECK(kExpansionRecordNameBytes == 64 && kExpansionRecordDescriptionBytes == 272, "the record's two text fields");

    const fs::path dir = fresh_dir("boot_archive_dir");
    CHECK(!vfs_has_boot_archive(dir.string()) && !vfs_has_boot_archive(""), "an empty folder holds no base game");
    write_loose(dir / "extra.pff", "x");
    fs::create_directories(dir / "resource.pff");
    CHECK(!vfs_has_boot_archive(dir.string()), "another archive, or a folder of the name, is none");
    write_loose(dir / "LocalRes.PFF", "x");
    CHECK(vfs_has_boot_archive(dir.string()), "one of the boot table's archives, in any case");
    CHECK(!vfs_has_boot_archive((dir / "missing").string()), "a folder that is not there");
    return 1;
}

int main() {
    std::error_code ec;
    g_root = (fs::temp_directory_path(ec) / test_paths_unique("opennova_vfs_test")).string();
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
    RUN_TEST(test_expansion_version_checksum);
    RUN_TEST(test_expansion_info);
    RUN_TEST(test_expansion_records);
    RUN_TEST(test_expansion_override_table);
    RUN_TEST(test_expansion_paths_and_names);

    fs::remove_all(g_root, ec);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
