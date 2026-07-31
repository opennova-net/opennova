#include "vfs/vfs.h"

#include "vfs/vfs_decode.h"

#include <pff/pff.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <io/strutil.h>

namespace fs = std::filesystem;

namespace opennova {
namespace {

using opennova::strutil::to_lower;

// Flat, lowercased lookup key from a possibly path-qualified name (matches the engine's
// basename-for-archive behavior and the existing flat asset model).
std::string flat_key(const std::string &name) {
    std::string fn = fs::path(name).filename().string();
    if (fn.empty()) fn = name;
    return to_lower(fn);
}

std::string pff_entry_name(const PffEntry &e) {
    size_t len = 0;
    while (len < PFF_NAME_SIZE && e.filename[len] != '\0') ++len;
    while (len > 0 && e.filename[len - 1] == ' ') --len;
    return std::string(e.filename, e.filename + len);
}

char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

bool ascii_case_equal(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_upper(a[i]) != ascii_upper(b[i])) return false;
    }
    return true;
}

// Retail copies the query into a 32-byte local buffer, uppercases it, and compares it
// against the uppercased raw directory name. The apparent trailing-space trim begins on
// the terminating NUL and is dead, so spaces remain significant.
// [orig: PFF_FindEntry @ 0x7685d0]
bool retail_archive_query_key(const std::string &name, std::string &key) {
    constexpr size_t kRetailQueryCapacity = 31;
    key.clear();
    if (name.empty()) return false;
    key.assign(name.data(), std::min(name.size(), kRetailQueryCapacity));
    for (char &c : key) c = ascii_upper(c);
    return true;
}

std::string retail_archive_entry_key(const PffEntry &entry) {
    size_t len = 0;
    while (len < PFF_NAME_SIZE && entry.filename[len] != '\0') ++len;
    std::string key(entry.filename, entry.filename + len);
    for (char &c : key) c = ascii_upper(c);
    return key;
}

// Validate once before either loose or archive resolution. Both slash styles are separators
// for loose probes, but the original spelling is retained for exact archive comparison.
// Retail's shared front doors concatenate the unchecked query; rejecting rooted-path syntax and
// traversal here is our mounted-root safety boundary.
// [orig: FileSystem_OpenFile @ 0x75b1c0; FileSystem_FileExists @ 0x75aa50]
bool split_retail_query(const std::string &name, std::vector<std::string> &components) {
    components.clear();
    if (name.empty() || name.find('\0') != std::string::npos) return false;
    if (name.front() == '/' || name.front() == '\\') return false;
    // A terminal separator or dot denotes a directory-shaped query. Do not silently
    // normalize it into the preceding regular file; Win32's file open would fail it.
    if (name.back() == '/' || name.back() == '\\') return false;
    // Reject drive-relative/absolute paths and NTFS alternate data streams. Loose lookups
    // are confined to a mounted search root and never interpret caller-supplied root syntax.
    if (name.find(':') != std::string::npos) return false;

    size_t begin = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
        const bool separator = i == name.size() || name[i] == '/' || name[i] == '\\';
        if (!separator) continue;
        if (i > begin) {
            std::string component = name.substr(begin, i - begin);
            if (component == "..") return false;
            if (component == ".") {
                if (i == name.size()) return false;
            } else {
                components.push_back(std::move(component));
            }
        }
        begin = i + 1;
    }
    return !components.empty();
}

bool path_is_within(const fs::path &root, const fs::path &candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end()) return false;
#ifdef _WIN32
        const bool same_component = ascii_case_equal(root_it->string(), candidate_it->string());
#else
        // Canonical POSIX paths are case-sensitive. Treating sibling roots that differ only
        // by case as equal would let an in-root symlink bypass the containment check.
        const bool same_component = root_it->string() == candidate_it->string();
#endif
        if (!same_component) {
            return false;
        }
    }
    return true;
}

// FileSystem_OpenFile passes the complete query to each loose probe; the basename-strip
// setter exists but is dead. Walk one component at a time to reproduce Win32-insensitive
// matching on every platform, canonicalizing each match so symlinks cannot escape the root.
// [orig: FileSystem_OpenFile @ 0x75b1c0; dead setter @ 0x75a590]
bool resolve_retail_loose_file(const std::string &search_root,
                               const std::vector<std::string> &components,
                               fs::path &resolved_file) {
    std::error_code ec;
    const fs::path canonical_root = fs::canonical(fs::path(search_root), ec);
    if (ec || !fs::is_directory(canonical_root, ec)) return false;

    fs::path current = canonical_root;
    for (size_t component_index = 0; component_index < components.size(); ++component_index) {
        const std::string &wanted = components[component_index];
        fs::path selected;

        // Prefer the exact spelling (and let Windows perform its native case-insensitive
        // probe); the directory walk supplies the same ASCII-insensitive behavior elsewhere.
        const fs::path direct = current / fs::path(wanted);
        const fs::file_status direct_status = fs::symlink_status(direct, ec);
        if (!ec && fs::exists(direct_status)) selected = direct;
        ec.clear();

        if (selected.empty()) {
            fs::directory_iterator it(current, fs::directory_options::skip_permission_denied, ec);
            const fs::directory_iterator end;
            if (ec) return false;
            for (; it != end; it.increment(ec)) {
                if (ec) return false;
                if (ascii_case_equal(it->path().filename().string(), wanted)) {
                    selected = it->path();
                    break;
                }
            }
            if (ec) return false;
        }
        if (selected.empty()) return false;

        current = fs::canonical(selected, ec);
        if (ec || !path_is_within(canonical_root, current)) return false;
        if (component_index + 1 < components.size() && !fs::is_directory(current, ec)) {
            return false;
        }
    }

    if (!fs::is_regular_file(current, ec)) return false;
    resolved_file = current;
    return true;
}

bool read_whole_file(const fs::path &path, std::vector<uint8_t> &out) {
    out.clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff sz = f.tellg();
    if (sz < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (out.empty()) return true;
    f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<size_t>(f.gcount()) == out.size();
}

bool has_pff_ext(const fs::path &p) {
    return to_lower(p.extension().string()) == ".pff";
}

} // namespace

// An opened archive. Non-copyable; owns its PffArchive handle. Stored behind unique_ptr so
// its address (and the PffEntry pointers within) stay stable across vector growth.
struct ArchiveMount {
    std::string path;
    PffArchive ar;
    std::unordered_map<std::string, const PffEntry *> retail_entries;
    ArchiveMount() { std::memset(&ar, 0, sizeof(ar)); }
    ~ArchiveMount() { pff_close(&ar); }
    ArchiveMount(const ArchiveMount &) = delete;
    ArchiveMount &operator=(const ArchiveMount &) = delete;

    void build_retail_index() {
        retail_entries.clear();
        retail_entries.reserve(ar.entry_count);
        for (uint32_t i = 0; i < ar.entry_count; ++i) {
            const std::string key = retail_archive_entry_key(ar.entries[i]);
            if (!key.empty()) retail_entries.emplace(key, &ar.entries[i]);
        }
    }
};

struct ResolvedEntry {
    VfsSource source = VfsSource::LooseDir;
    std::string logical_name;       // original case
    std::string source_path;        // loose dir OR archive path
    int precedence = 0;
    std::string loose_full_path;    // when source == LooseDir
    ArchiveMount *archive = nullptr; // when source == Archive
    const PffEntry *entry = nullptr; // when source == Archive
};

struct Vfs::Impl {
    std::vector<std::string> search_paths;                 // add order (highest precedence)
    // mount_game retains these even in Packed mode. Retail callers such as foliage/UI can
    // force one loose-first lookup without making loose data visible to the legacy index.
    // [orig: Terrain_LoadFoliageFile @ 0x60a74e; FileSystem_OpenFile @ 0x75b1c0]
    std::vector<std::string> retail_loose_probe_paths;
    std::unique_ptr<ArchiveMount> primary;
    std::vector<std::unique_ptr<ArchiveMount>> secondaries; // add order
    std::string game_root;
    // The expansion whose layers actually mounted (empty after the silent base fallback).
    std::string mounted_expansion;
    std::string last_error;
    int scr_policy = VFS_SCR_VERSION_DETECT; // how read_file keys SCR payloads (game-driven)
    VfsMountMode session_mount_mode = VfsMountMode::PackedWithLooseOverride;

    mutable bool index_valid = false;
    mutable std::unordered_map<std::string, ResolvedEntry> index;
    mutable std::vector<const ResolvedEntry *> ordered;     // stable enumeration order

    void invalidate() { index_valid = false; }

    std::unique_ptr<ArchiveMount> open_archive(const std::string &path) {
        std::unique_ptr<ArchiveMount> m(new ArchiveMount());
        m->path = path;
        if (pff_open(&m->ar, path.c_str()) != 0) {
            last_error = "Failed to open PFF archive: " + path;
            return nullptr;
        }
        m->build_retail_index();
        return m;
    }

    void scan_loose(const std::string &dir, int precedence) const {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;
        for (const fs::directory_entry &de :
             fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!de.is_regular_file(ec)) continue;
            const std::string fn = de.path().filename().string();
            const std::string key = to_lower(fn);
            if (key.empty() || index.count(key)) continue; // higher precedence already won
            ResolvedEntry e;
            e.source = VfsSource::LooseDir;
            e.logical_name = fn;
            e.source_path = dir;
            e.precedence = precedence;
            e.loose_full_path = de.path().string();
            index.emplace(key, std::move(e));
        }
    }

    void scan_archive(ArchiveMount *mount, int precedence) const {
        if (!mount) return;
        for (uint32_t i = 0; i < mount->ar.entry_count; ++i) {
            const PffEntry &pe = mount->ar.entries[i];
            const std::string name = pff_entry_name(pe);
            const std::string key = to_lower(name);
            if (key.empty() || index.count(key)) continue;
            ResolvedEntry e;
            e.source = VfsSource::Archive;
            e.logical_name = name;
            e.source_path = mount->path;
            e.precedence = precedence;
            e.archive = mount;
            e.entry = &pe;
            index.emplace(key, std::move(e));
        }
    }

    void ensure_index() const {
        if (index_valid) return;
        index.clear();
        ordered.clear();

        int precedence = 0;
        for (const std::string &dir : search_paths) scan_loose(dir, precedence++);
        if (primary) scan_archive(primary.get(), precedence++);
        for (const std::unique_ptr<ArchiveMount> &sec : secondaries) scan_archive(sec.get(), precedence++);

        ordered.reserve(index.size());
        for (const auto &kv : index) ordered.push_back(&kv.second);
        std::sort(ordered.begin(), ordered.end(),
                  [](const ResolvedEntry *a, const ResolvedEntry *b) {
                      return to_lower(a->logical_name) < to_lower(b->logical_name);
                  });
        index_valid = true;
    }

    const ResolvedEntry *find(const std::string &name) const {
        ensure_index();
        const std::string key = flat_key(name);
        if (key.empty()) return nullptr;
        auto it = index.find(key);
        return it == index.end() ? nullptr : &it->second;
    }

    bool find_retail_loose(const std::vector<std::string> &components,
                           ResolvedEntry &resolved) const {
        int precedence = 0;
        for (const std::string &dir : retail_loose_probe_paths) {
            fs::path loose_file;
            if (resolve_retail_loose_file(dir, components, loose_file)) {
                resolved = ResolvedEntry{};
                resolved.source = VfsSource::LooseDir;
                resolved.logical_name = loose_file.filename().string();
                resolved.source_path = dir;
                resolved.precedence = precedence;
                resolved.loose_full_path = loose_file.string();
                return true;
            }
            ++precedence;
        }
        return false;
    }

    bool find_retail_archive(const std::string &name, ResolvedEntry &resolved) const {
        std::string key;
        if (!retail_archive_query_key(name, key)) return false;

        int precedence = static_cast<int>(retail_loose_probe_paths.size());
        const auto find_in_mount = [&](ArchiveMount *mount, int mount_precedence) {
            if (!mount) return false;
            const auto found = mount->retail_entries.find(key);
            if (found == mount->retail_entries.end()) return false;
            resolved = ResolvedEntry{};
            resolved.source = VfsSource::Archive;
            resolved.logical_name = name;
            resolved.source_path = mount->path;
            resolved.precedence = mount_precedence;
            resolved.archive = mount;
            resolved.entry = found->second;
            return true;
        };

        if (find_in_mount(primary.get(), precedence++)) return true;
        for (const std::unique_ptr<ArchiveMount> &secondary : secondaries) {
            if (find_in_mount(secondary.get(), precedence++)) return true;
        }
        return false;
    }

    bool find_retail(const std::string &name, VfsLookupPolicy policy,
                     ResolvedEntry &resolved) const {
        std::vector<std::string> components;
        if (!split_retail_query(name, components)) return false;

        switch (policy) {
        case VfsLookupPolicy::ForceLooseFirst:
            return find_retail_loose(components, resolved)
                || find_retail_archive(name, resolved);
        case VfsLookupPolicy::ForceArchiveOnly:
            return find_retail_archive(name, resolved);
        case VfsLookupPolicy::SessionDefault:
            switch (session_mount_mode) {
            case VfsMountMode::LooseOnly:
                return find_retail_loose(components, resolved);
            case VfsMountMode::Packed:
                // The archive-only branch is gated by an archive actually being online;
                // otherwise retail falls through to the ordinary loose-first skeleton.
                // [orig: FileSystem_OpenFile @ 0x75b1c0]
                if (primary || !secondaries.empty()) {
                    return find_retail_archive(name, resolved);
                }
                return find_retail_loose(components, resolved);
            case VfsMountMode::PackedWithLooseOverride:
                return find_retail_loose(components, resolved)
                    || find_retail_archive(name, resolved);
            }
            break;
        }
        return false;
    }
};

Vfs::Vfs() : impl_(new Impl()) {}
Vfs::~Vfs() = default;
Vfs::Vfs(Vfs &&) noexcept = default;
Vfs &Vfs::operator=(Vfs &&) noexcept = default;

bool Vfs::add_search_path(const std::string &dir) {
    if (dir.empty()) { impl_->last_error = "Empty search path"; return false; }
    impl_->search_paths.push_back(dir);
    impl_->retail_loose_probe_paths.push_back(dir);
    impl_->invalidate();
    return true;
}

bool Vfs::set_primary_archive(const std::string &pff_path) {
    std::unique_ptr<ArchiveMount> m = impl_->open_archive(pff_path);
    if (!m) return false;
    impl_->primary = std::move(m);
    impl_->invalidate();
    return true;
}

bool Vfs::add_secondary_archive(const std::string &pff_path) {
    std::unique_ptr<ArchiveMount> m = impl_->open_archive(pff_path);
    if (!m) return false;
    impl_->secondaries.push_back(std::move(m));
    impl_->invalidate();
    return true;
}

bool Vfs::mount_game(const std::string &game_root, const std::string &expansion, VfsMountMode mode,
                     VfsArchiveDiscovery discovery) {
    clear();

    std::error_code ec;
    const fs::path root(game_root);
    if (game_root.empty() || !fs::is_directory(root, ec)) {
        impl_->last_error = "Game root is not a directory: " + game_root;
        return false;
    }
    impl_->game_root = root.string();
    impl_->session_mount_mode = mode;

    const bool mount_loose = mode != VfsMountMode::Packed;
    const bool mount_archives = mode != VfsMountMode::LooseOnly;
    const auto retain_loose_probe = [&](const fs::path &dir) {
        if (mount_loose) {
            add_search_path(dir.string());
        } else {
            impl_->retail_loose_probe_paths.push_back(dir.string());
        }
    };

    bool have_expansion = false;
    fs::path exp_dir;
    if (!expansion.empty()) {
        exp_dir = root / "expansion" / expansion;
        if (fs::exists(exp_dir / (expansion + ".pff"), ec)) {
            have_expansion = true;
            // Record what actually mounted so callers can tell a real expansion mount from
            // the fallback below without re-deriving the predicate (D-NET-178).
            impl_->mounted_expansion = expansion;
        }
        // A missing/unknown expansion silently falls back to base-game mounting.
    }

    if (have_expansion) {
        retain_loose_probe(exp_dir);                    // loose expansion files: highest
        retain_loose_probe(root);                       // engine CWD probe: base loose files
        if (mount_archives) {
            const fs::path local = exp_dir / (expansion + "L.pff");
            if (fs::exists(local, ec)) set_primary_archive(local.string());
            add_secondary_archive((exp_dir / (expansion + ".pff")).string());
        }
    } else {
        retain_loose_probe(root);
    }

    if (!mount_archives) {
        return true;
    }

    if (discovery == VfsArchiveDiscovery::ScanAll) {
        // Authoring discovery (the editor's browse index): every base-root
        // *.pff, alphabetical for determinism. A deliberate divergence from
        // the retail table so modders' arbitrary archives are indexable
        // (docs/vfs/vfs-pff-mount-re.md D-VFS-2 records the decision).
        std::vector<fs::path> base_pffs;
        for (const fs::directory_entry &de :
             fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (de.is_regular_file(ec) && has_pff_ext(de.path())) base_pffs.push_back(de.path());
        }
        std::sort(base_pffs.begin(), base_pffs.end(), [](const fs::path &a, const fs::path &b) {
            return to_lower(a.filename().string()) < to_lower(b.filename().string());
        });
        for (const fs::path &p : base_pffs) add_secondary_archive(p.string());
        return true;
    }

    // The witnessed fixed boot table [orig: PFF_OpenAllArchives @ 0x4a4310,
    // name table @ 0x829f90 stride 260]: after the expansion pair (slots 0/1,
    // mounted above), slot 2 = language.pff, 3 = localres.pff,
    // 4 = resource.pff (slot 5 has no writer). Slot order IS lookup
    // precedence; extra .pff files in the root never mount in retail. Names
    // probe case-insensitively (retail opens via _lopen on a case-insensitive
    // filesystem); a missing archive just leaves its slot empty — only the
    // all-missing case is fatal at the caller (required-resources.md).
    static constexpr const char *kBootArchiveTable[] = {
        "language.pff",
        "localres.pff",
        "resource.pff",
    };
    for (const char *slot_name : kBootArchiveTable) {
        fs::path direct = root / slot_name;
        if (fs::exists(direct, ec)) {
            add_secondary_archive(direct.string());
            continue;
        }
        // Case-insensitive probe for case-sensitive filesystems.
        for (const fs::directory_entry &de :
             fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (de.is_regular_file(ec) && to_lower(de.path().filename().string()) == slot_name) {
                add_secondary_archive(de.path().string());
                break;
            }
        }
    }

    return true;
}

void Vfs::clear() {
    impl_->search_paths.clear();
    impl_->retail_loose_probe_paths.clear();
    impl_->primary.reset();
    impl_->secondaries.clear();
    impl_->game_root.clear();
    impl_->mounted_expansion.clear();
    impl_->last_error.clear();
    impl_->session_mount_mode = VfsMountMode::PackedWithLooseOverride;
    impl_->index.clear();
    impl_->ordered.clear();
    impl_->index_valid = false;
}

bool Vfs::has_mounted_archive() const {
    return impl_->primary != nullptr || !impl_->secondaries.empty();
}

bool Vfs::has_file(const std::string &name) const {
    return impl_->find(name) != nullptr;
}

bool Vfs::has_file(const std::string &name, VfsLookupPolicy policy) const {
    ResolvedEntry resolved;
    return impl_->find_retail(name, policy, resolved);
}

bool Vfs::read_file_raw(const std::string &name, std::vector<uint8_t> &out) const {
    out.clear();
    const ResolvedEntry *e = impl_->find(name);
    if (!e) { impl_->last_error = "File not found: " + name; return false; }

    if (e->source == VfsSource::LooseDir) {
        if (!read_whole_file(e->loose_full_path, out)) {
            impl_->last_error = "Failed to read loose file: " + e->loose_full_path;
            return false;
        }
        return true;
    }
    // Archive: pff_extract applies any PFF container decryption.
    out.resize(e->entry->size);
    if (pff_extract(&e->archive->ar, e->entry, out.empty() ? nullptr : out.data(), out.size()) != 0) {
        out.clear();
        impl_->last_error = "Failed to extract from archive: " + e->source_path;
        return false;
    }
    return true;
}

bool Vfs::read_file_raw(const std::string &name, std::vector<uint8_t> &out,
                        VfsLookupPolicy policy) const {
    out.clear();
    ResolvedEntry resolved;
    if (!impl_->find_retail(name, policy, resolved)) {
        impl_->last_error = "File not found: " + name;
        return false;
    }

    if (resolved.source == VfsSource::LooseDir) {
        if (!read_whole_file(resolved.loose_full_path, out)) {
            impl_->last_error = "Failed to read loose file: " + resolved.loose_full_path;
            return false;
        }
        return true;
    }

    // Archive extraction retains the legacy read_file_raw contract: PFF container
    // encryption is removed, while SCR/BFC1 payload decoding is left to read_file.
    out.resize(resolved.entry->size);
    if (pff_extract(&resolved.archive->ar, resolved.entry,
                    out.empty() ? nullptr : out.data(), out.size()) != 0) {
        out.clear();
        impl_->last_error = "Failed to extract from archive: " + resolved.source_path;
        return false;
    }
    return true;
}

bool Vfs::read_file(const std::string &name, std::vector<uint8_t> &out) const {
    if (!read_file_raw(name, out)) return false;
    if (!vfs_decode_payload(out, impl_->scr_policy)) {
        impl_->last_error = "Failed to decode payload (SCR/BFC1): " + name;
        return false;
    }
    return true;
}

bool Vfs::read_file(const std::string &name, std::vector<uint8_t> &out,
                    VfsLookupPolicy policy) const {
    if (!read_file_raw(name, out, policy)) return false;
    if (!vfs_decode_payload(out, impl_->scr_policy)) {
        impl_->last_error = "Failed to decode payload (SCR/BFC1): " + name;
        return false;
    }
    return true;
}

void Vfs::set_scr_policy(int scr_policy) {
    impl_->scr_policy = scr_policy;
}

std::vector<VfsFileLocation> Vfs::list_files() const {
    impl_->ensure_index();
    std::vector<VfsFileLocation> out;
    out.reserve(impl_->ordered.size());
    for (const ResolvedEntry *e : impl_->ordered) {
        VfsFileLocation loc;
        loc.logical_name = e->logical_name;
        loc.source = e->source;
        loc.source_path = e->source_path;
        loc.precedence = e->precedence;
        out.push_back(std::move(loc));
    }
    return out;
}

const std::string &Vfs::game_root() const { return impl_->game_root; }
const std::string &Vfs::mounted_expansion() const { return impl_->mounted_expansion; }
const std::string &Vfs::last_error() const { return impl_->last_error; }

std::vector<std::string> vfs_list_expansions(const std::string &game_root) {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path exp_root = fs::path(game_root) / "expansion";
    if (!fs::is_directory(exp_root, ec)) return out;
    for (const fs::directory_entry &de :
         fs::directory_iterator(exp_root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!de.is_directory(ec)) continue;
        const std::string name = de.path().filename().string();
        if (fs::exists(de.path() / (name + ".pff"), ec)) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace opennova
