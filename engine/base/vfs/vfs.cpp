#include <base/vfs/vfs.h>

#include <base/vfs/vfs_decode.h>

#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <unordered_map>

#include <base/io/crc32_mpeg2.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>

using namespace opennova::pff;

namespace fs = std::filesystem;

// Paths are UTF-8 strings here (what the mount is given and what it reports); every OS
// call takes io::os_path(...) of one, and a name the system enumerates comes back
// through io::utf8_path, so a root past MAX_PATH or outside the ANSI code page mounts
// and nothing throws on a name the code page cannot hold (base/io/os_path.h).

namespace opennova {
namespace {

using opennova::strutil::to_lower;

// Flat, lowercased lookup key from a possibly path-qualified name (matches the engine's
// basename-for-archive behavior and the existing flat asset model).
std::string flat_key(const std::string &name) {
    std::string fn = io::utf8_file_name(name);
    if (fn.empty()) fn = name;
    return to_lower(fn);
}

std::string pff_entry_name(const PffEntry &e) {
    size_t len = 0;
    while (len < PFF_NAME_SIZE && e.filename[len] != '\0') ++len;
    while (len > 0 && e.filename[len - 1] == ' ') --len;
    return std::string(e.filename, e.filename + len);
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
    for (char &c : key) c = strutil::ascii_toupper(c);
    return true;
}

std::string retail_archive_entry_key(const PffEntry &entry) {
    size_t len = 0;
    while (len < PFF_NAME_SIZE && entry.filename[len] != '\0') ++len;
    std::string key(entry.filename, entry.filename + len);
    for (char &c : key) c = strutil::ascii_toupper(c);
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

// Whether `candidate` is `root` or below it, both canonical, compared component-wise as
// UTF-8 ('/' separators, any \\?\ prefix dropped).
bool path_is_within(const fs::path &root, const fs::path &candidate) {
    std::string r = io::utf8_generic_path(root);
    const std::string c = io::utf8_generic_path(candidate);
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    if (c.size() < r.size()) return false;
#ifdef _WIN32
    const bool same_prefix = strutil::iequals(c.substr(0, r.size()), r);
#else
    // Canonical POSIX paths are case-sensitive. Treating sibling roots that differ only
    // by case as equal would let an in-root symlink bypass the containment check.
    const bool same_prefix = c.compare(0, r.size(), r) == 0;
#endif
    return same_prefix && (c.size() == r.size() || r.back() == '/' || c[r.size()] == '/');
}

// FileSystem_OpenFile passes the complete query to each loose probe; the basename-strip
// setter exists but is dead. Walk one component at a time to reproduce Win32-insensitive
// matching on every platform, canonicalizing each match so symlinks cannot escape the root.
// [orig: FileSystem_OpenFile @ 0x75b1c0; dead setter @ 0x75a590]
bool resolve_retail_loose_file(const std::string &search_root,
                               const std::vector<std::string> &components,
                               fs::path &resolved_file) {
    std::error_code ec;
    fs::path canonical_root = fs::canonical(io::os_path(search_root), ec);
    if (ec) return false;
    canonical_root = io::os_path(canonical_root);
    if (!fs::is_directory(canonical_root, ec)) return false;

    fs::path current = canonical_root;
    for (size_t component_index = 0; component_index < components.size(); ++component_index) {
        const std::string &wanted = components[component_index];
        fs::path selected;

        // Prefer the exact spelling (and let Windows perform its native case-insensitive
        // probe); the directory walk supplies the same ASCII-insensitive behavior elsewhere.
        const fs::path direct = io::os_path(current / io::os_path(wanted));
        const fs::file_status direct_status = fs::symlink_status(direct, ec);
        if (!ec && fs::exists(direct_status)) selected = direct;
        ec.clear();

        if (selected.empty()) {
            fs::directory_iterator it(current, fs::directory_options::skip_permission_denied, ec);
            const fs::directory_iterator end;
            if (ec) return false;
            for (; it != end; it.increment(ec)) {
                if (ec) return false;
                if (strutil::iequals(io::utf8_path(it->path().filename()), wanted)) {
                    selected = it->path();
                    break;
                }
            }
            if (ec) return false;
        }
        if (selected.empty()) return false;

        current = fs::canonical(selected, ec);
        if (ec) return false;
        current = io::os_path(current);
        if (!path_is_within(canonical_root, current)) return false;
        if (component_index + 1 < components.size() && !fs::is_directory(current, ec)) {
            return false;
        }
    }

    if (!fs::is_regular_file(current, ec)) return false;
    resolved_file = current;
    return true;
}

bool read_whole_file(const std::string &path, std::vector<uint8_t> &out) {
    out.clear();
    std::ifstream f(io::os_path(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff sz = f.tellg();
    if (sz < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (out.empty()) return true;
    f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<size_t>(f.gcount()) == out.size();
}

bool has_pff_ext(const std::string &file_name) {
    return file_name.size() > 4 && to_lower(file_name.substr(file_name.size() - 4)) == ".pff";
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
    // force one loose-first lookup without making loose data visible to the flat index.
    // [orig: Terrain_LoadTileInfoFile @ 0x60a74e; FileSystem_OpenFile @ 0x75b1c0]
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
        auto m = std::make_unique<ArchiveMount>();
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
        const fs::path os_dir = io::os_path(dir);
        if (!fs::is_directory(os_dir, ec)) return;
        for (const fs::directory_entry &de :
             fs::directory_iterator(os_dir, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (!de.is_regular_file(ec)) continue;
            const std::string fn = io::utf8_path(de.path().filename());
            const std::string key = to_lower(fn);
            if (key.empty() || index.count(key)) continue; // higher precedence already won
            ResolvedEntry e;
            e.source = VfsSource::LooseDir;
            e.logical_name = fn;
            e.source_path = dir;
            e.precedence = precedence;
            e.loose_full_path = io::utf8_join(dir, fn);
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
                resolved.logical_name = io::utf8_path(loose_file.filename());
                resolved.source_path = dir;
                resolved.precedence = precedence;
                resolved.loose_full_path = io::utf8_path(loose_file);
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

Vfs::Vfs() : impl_(std::make_unique<Impl>()) {}
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
    const fs::path root = io::os_path(game_root);
    if (game_root.empty() || !fs::is_directory(root, ec)) {
        impl_->last_error = "Game root is not a directory: " + game_root;
        return false;
    }
    impl_->game_root = game_root;
    impl_->session_mount_mode = mode;

    const bool mount_loose = mode != VfsMountMode::Packed;
    const bool mount_archives = mode != VfsMountMode::LooseOnly;
    const auto retain_loose_probe = [&](const std::string &dir) {
        if (mount_loose) {
            add_search_path(dir);
        } else {
            impl_->retail_loose_probe_paths.push_back(dir);
        }
    };

    bool have_expansion = false;
    std::string exp_dir;
    if (!expansion.empty()) {
        exp_dir = io::utf8_join(io::utf8_join(game_root, "expansion"), expansion);
        if (fs::exists(io::os_path(io::utf8_join(exp_dir, expansion + ".pff")), ec)) {
            have_expansion = true;
            // Record what actually mounted so callers can tell a real expansion mount from
            // the fallback below without re-deriving the predicate (D-NET-178).
            impl_->mounted_expansion = expansion;
        }
        // A missing/unknown expansion silently falls back to base-game mounting.
    }

    if (have_expansion) {
        retain_loose_probe(exp_dir);                    // loose expansion files: highest
        retain_loose_probe(game_root);                  // engine CWD probe: base loose files
        if (mount_archives) {
            const std::string local = io::utf8_join(exp_dir, expansion + "L.pff");
            if (fs::exists(io::os_path(local), ec)) set_primary_archive(local);
            add_secondary_archive(io::utf8_join(exp_dir, expansion + ".pff"));
        }
    } else {
        retain_loose_probe(game_root);
    }

    if (!mount_archives) {
        return true;
    }

    if (discovery == VfsArchiveDiscovery::ScanAll) {
        // Explicit catalog discovery: every base-root *.pff, alphabetical for
        // determinism. A deliberate divergence from the retail table so
        // arbitrary mod archives are indexable
        // (docs/vfs/vfs-pff-mount-re.md D-VFS-2 records the decision).
        std::vector<std::string> base_pffs;
        for (const fs::directory_entry &de :
             fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            const std::string name = io::utf8_path(de.path().filename());
            if (de.is_regular_file(ec) && has_pff_ext(name)) base_pffs.push_back(name);
        }
        std::sort(base_pffs.begin(), base_pffs.end(), [](const std::string &a, const std::string &b) {
            return to_lower(a) < to_lower(b);
        });
        for (const std::string &name : base_pffs) add_secondary_archive(io::utf8_join(game_root, name));
        return true;
    }

    // The witnessed fixed boot table (vfs.h kBootArchiveTable) after the
    // expansion pair mounted above. Names probe case-insensitively (retail
    // opens via _lopen on a case-insensitive filesystem); a missing archive
    // just leaves its slot empty — only the all-missing case is fatal at the
    // caller (required-resources.md).
    for (const char *slot_name : kBootArchiveTable) {
        const std::string direct = io::utf8_join(game_root, slot_name);
        if (fs::exists(io::os_path(direct), ec)) {
            add_secondary_archive(direct);
            continue;
        }
        // Case-insensitive probe for case-sensitive filesystems.
        for (const fs::directory_entry &de :
             fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            const std::string name = io::utf8_path(de.path().filename());
            if (de.is_regular_file(ec) && to_lower(name) == slot_name) {
                add_secondary_archive(io::utf8_join(game_root, name));
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

bool Vfs::prefers_loose_file(const std::string &name) const {
    if (impl_->session_mount_mode == VfsMountMode::Packed) return false;
    ResolvedEntry resolved;
    return impl_->find_retail(name, VfsLookupPolicy::ForceLooseFirst, resolved) &&
        resolved.source == VfsSource::LooseDir;
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

    // Archive extraction retains the flat read_file_raw contract: PFF container
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
    const std::string exp_root = io::utf8_join(game_root, "expansion");
    const fs::path os_exp_root = io::os_path(exp_root);
    if (!fs::is_directory(os_exp_root, ec)) return out;
    for (const fs::directory_entry &de :
         fs::directory_iterator(os_exp_root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!de.is_directory(ec)) continue;
        const std::string name = io::utf8_path(de.path().filename());
        if (fs::exists(io::os_path(io::utf8_join(io::utf8_join(exp_root, name), name + ".pff")), ec))
            out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

// <n>.bin's bytes the way the scan resolves them (loose, then <n>L.pff, then <n>.pff).
bool read_expansion_text_bytes(const std::string &exp_dir, const std::string &expansion,
                               std::vector<uint8_t> &out) {
    const std::string bin_name = expansion + ".bin";
    std::error_code ec;
    const fs::path loose = io::os_path(io::utf8_join(exp_dir, bin_name));
    if (fs::is_regular_file(loose, ec)) {
        std::ifstream in(loose, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (!out.empty()) return true;
    }
    for (const std::string &archive : {expansion + "L.pff", expansion + ".pff"}) {
        PffArchive ar{};
        if (pff_open(&ar, io::utf8_join(exp_dir, archive).c_str()) != 0) continue;
        const PffEntry *entry = pff_find(&ar, bin_name.c_str());
        bool ok = false;
        if (entry != nullptr) {
            out.resize(entry->size);
            ok = pff_extract(&ar, entry, out.empty() ? nullptr : out.data(), out.size()) == 0 &&
                 !out.empty();
        }
        pff_close(&ar);
        if (ok) return true;
    }
    out.clear();
    return false;
}

// [orig: TextResource_FindEntryBySectionAndKey @ 0x75d250 — the section by name, then the
//  key within it; case-insensitive like every rtxt lookup]
const rtxt::Entry *find_in_section(const rtxt::File &file, const char *section,
                                   const char *key) {
    const std::string want_section = strutil::to_upper(section);
    const std::string want_key = strutil::to_upper(key);
    for (uint32_t s = 0; s < file.sections.size(); ++s) {
        if (strutil::to_upper(file.sections[s].name) != want_section) continue;
        for (const rtxt::Entry &e : file.entries) {
            if (e.section_index == s && strutil::to_upper(e.key) == want_key) return &e;
        }
    }
    return nullptr;
}

} // namespace

ExpansionInfo vfs_expansion_info(const std::string &game_root, const std::string &expansion) {
    ExpansionInfo info{kExpansionUnnamed, kExpansionNoDescription};
    if (expansion.empty()) return info;
    const std::string exp_dir = io::utf8_join(io::utf8_join(game_root, "expansion"), expansion);
    std::vector<uint8_t> bytes;
    if (!read_expansion_text_bytes(exp_dir, expansion, bytes)) return info;  // @ 0x4a4664
    rtxt::File file;
    std::string error;
    if (!rtxt::parse(bytes.data(), bytes.size(), file, error)) return info;
    if (const rtxt::Entry *e = find_in_section(file, "exp_info", "EXP_NAME"))  // @ 0x4a4578
        info.name = e->text;
    if (const rtxt::Entry *e = find_in_section(file, "exp_info", "EXP_DESC"))  // @ 0x4a45ef
        info.description = e->text;
    return info;
}

int32_t vfs_version_crc(const uint8_t *data, size_t size) {
    // [orig: CRC_ComputeCustomTable @ 0x53c820 — crc = table[byte ^ HIBYTE(crc)]
    //  ^ (crc << 8), init -1]. The table is retail's 256-entry MSB-first
    // CRC-32 table for polynomial 0x04C11DB7 (@ 0x830780; entries [0]=0,
    // [1]=0x04C11DB7, [2]=0x09823B6E, [31]=0x745E66CD verified): the shared
    // io::kCrc32Mpeg2Table, value-identical to the NAPI envelope's.
    return static_cast<int32_t>(io::crc32_mpeg2(data, size));
}

int32_t vfs_expansion_version_checksum(const std::string &game_root,
                                       const std::string &expansion) {
    // [orig: Expansion_LoadAssets — g_ExpansionChecksum = 0 @ 0x4a4781; only a
    //  live expansion probes the loose file @ 0x4a4787..0x4a488a]
    if (game_root.empty() || expansion.empty()) return 0;
    const std::string path = io::utf8_join(
            io::utf8_join(io::utf8_join(game_root, "expansion"), expansion), "version.txt");
    std::ifstream file(io::os_path(path), std::ios::binary);
    if (!file) return 0; // [orig: the File_LoadEntireFile -1 gate @ 0x4a487b]
    std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
    // An empty file is a reimpl guard: retail's do-while would read one byte
    // past a zero-length allocation (see vfs_version_crc), so there is no
    // stable original value to reproduce — treat it like an absent file.
    if (bytes.empty()) return 0;
    return vfs_version_crc(bytes.data(), bytes.size());
}

bool vfs_expansion_override_table(const std::string &game_root, const std::string &expansion,
                                  ExpansionLoadPoint point, bool loose_first,
                                  std::vector<uint8_t> &out) {
    out.clear();
    // [orig: Expansion_LoadAssets — File_CheckExists("expansion\<n>\<n>.pff") @ 0x4a4767,
    //  the name cleared @ 0x4a4775, TextResource_LoadOverrideTable(NULL) @ 0x4a482a]
    fs::path archive;
    if (game_root.empty() || expansion.empty() ||
        !resolve_retail_loose_file(game_root, {"expansion", expansion, expansion + ".pff"}, archive))
        return false;
    // [orig: File_LoadResource @ 0x75b540 — with an archive open and loose-first off only
    //  the archives are walked (@ 0x75b56c..0x75b57c), and none matches the whole query]
    if (point == ExpansionLoadPoint::ArchivesOpen && !loose_first) return false;
    // The search path `expansion\<n>` (@ 0x4a49bb) joined to the query (@ 0x75b5b9), then
    // the query itself (@ 0x75b5a4).
    const std::string bin = expansion + ".bin";
    const std::vector<std::vector<std::string>> walk = {
            {"expansion", expansion, "expansion", expansion, bin},
            {"expansion", expansion, bin},
    };
    for (const std::vector<std::string> &components : walk) {
        fs::path file;
        if (!resolve_retail_loose_file(game_root, components, file)) continue;
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) continue;
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !out.empty();
    }
    return false;
}

std::string vfs_country_code(const std::string &game_root) {
    // [orig: Game_ReadCCBinFile @ 0x4a5860 — fopen("CC.BIN", "rb") @ 0x4a58d0,
    //  one byte @ 0x4a58e6 then a second @ 0x4a58fe, the NUL @ 0x4a591e,
    //  Napi_CopyString(dst, cc_value, 8) @ 0x4a5921 (stops at an inner NUL)]
    if (game_root.empty()) return {};
    fs::path path;
    if (!resolve_retail_loose_file(game_root, {"CC.BIN"}, path)) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    char bytes[2] = {0, 0};
    std::string out;
    for (char &byte : bytes) {
        if (!file.get(byte) || byte == '\0') break;
        out.push_back(byte);
    }
    return out;
}

} // namespace opennova
