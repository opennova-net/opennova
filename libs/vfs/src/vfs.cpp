#include "vfs/vfs.h"

#include "vfs/vfs_decode.h"

#include <pff/pff.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace opennova {
namespace {

std::string to_lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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
    ArchiveMount() { std::memset(&ar, 0, sizeof(ar)); }
    ~ArchiveMount() { pff_close(&ar); }
    ArchiveMount(const ArchiveMount &) = delete;
    ArchiveMount &operator=(const ArchiveMount &) = delete;
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
    std::unique_ptr<ArchiveMount> primary;
    std::vector<std::unique_ptr<ArchiveMount>> secondaries; // add order
    std::string game_root;
    std::string last_error;

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
};

Vfs::Vfs() : impl_(new Impl()) {}
Vfs::~Vfs() = default;
Vfs::Vfs(Vfs &&) noexcept = default;
Vfs &Vfs::operator=(Vfs &&) noexcept = default;

bool Vfs::add_search_path(const std::string &dir) {
    if (dir.empty()) { impl_->last_error = "Empty search path"; return false; }
    impl_->search_paths.push_back(dir);
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

bool Vfs::mount_game(const std::string &game_root, const std::string &expansion) {
    clear();

    std::error_code ec;
    const fs::path root(game_root);
    if (game_root.empty() || !fs::is_directory(root, ec)) {
        impl_->last_error = "Game root is not a directory: " + game_root;
        return false;
    }
    impl_->game_root = root.string();

    bool have_expansion = false;
    fs::path exp_dir;
    if (!expansion.empty()) {
        exp_dir = root / "expansion" / expansion;
        if (fs::exists(exp_dir / (expansion + ".pff"), ec)) {
            have_expansion = true;
        }
        // A missing/unknown expansion silently falls back to base-game mounting.
    }

    if (have_expansion) {
        add_search_path(exp_dir.string());          // loose expansion files: highest
        add_search_path(root.string());             // engine CWD probe: base loose files
        const fs::path local = exp_dir / (expansion + "L.pff");
        if (fs::exists(local, ec)) set_primary_archive(local.string());
        add_secondary_archive((exp_dir / (expansion + ".pff")).string());
    } else {
        add_search_path(root.string());
    }

    // Base-root archives (resource.pff / localres.pff / language.pff / ...), appended after
    // any expansion archives so the expansion overrides the base. Sorted for determinism.
    // NOTE: the engine's exact base-archive slot assignment was not fully traced; these three
    // hold largely disjoint content (models / animations / audio), so their relative order
    // rarely affects resolution. See notes/vfs/phase0_ida_verification.md (open question).
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

void Vfs::clear() {
    impl_->search_paths.clear();
    impl_->primary.reset();
    impl_->secondaries.clear();
    impl_->game_root.clear();
    impl_->last_error.clear();
    impl_->index.clear();
    impl_->ordered.clear();
    impl_->index_valid = false;
}

bool Vfs::has_file(const std::string &name) const {
    return impl_->find(name) != nullptr;
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

bool Vfs::read_file(const std::string &name, std::vector<uint8_t> &out) const {
    if (!read_file_raw(name, out)) return false;
    if (!vfs_decode_payload(out)) {
        impl_->last_error = "Failed to decode payload (SCR/BFC1): " + name;
        return false;
    }
    return true;
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
