#include "vfs/vfs_capi.h"

#include "vfs/vfs.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// Opaque handle: a Vfs plus a cached enumeration snapshot so the file_*_at accessors can
// return stable const char* into owned storage. The snapshot is invalidated on any mutation.
struct OpennovaVfs {
    opennova::Vfs vfs;
    mutable std::vector<opennova::VfsFileLocation> snapshot;
    mutable bool snapshot_valid = false;

    void invalidate() { snapshot_valid = false; }
    void ensure_snapshot() const {
        if (!snapshot_valid) {
            snapshot = vfs.list_files();
            snapshot_valid = true;
        }
    }
};

namespace {

int read_common(OpennovaVfs *vfs, const char *name, uint8_t **out, size_t *out_size, bool decode) {
    if (!vfs || !name || !out || !out_size) return 0;
    *out = nullptr;
    *out_size = 0;
    std::vector<uint8_t> buf;
    const bool ok = decode ? vfs->vfs.read_file(name, buf) : vfs->vfs.read_file_raw(name, buf);
    if (!ok) return 0;
    uint8_t *mem = static_cast<uint8_t *>(std::malloc(buf.empty() ? 1 : buf.size()));
    if (!mem) return 0;
    if (!buf.empty()) std::memcpy(mem, buf.data(), buf.size());
    *out = mem;
    *out_size = buf.size();
    return 1;
}

} // namespace

extern "C" {

OpennovaVfs *opennova_vfs_create(void) {
    return new (std::nothrow) OpennovaVfs();
}

void opennova_vfs_destroy(OpennovaVfs *vfs) {
    delete vfs;
}

int opennova_vfs_add_search_path(OpennovaVfs *vfs, const char *dir) {
    if (!vfs || !dir) return 0;
    vfs->invalidate();
    return vfs->vfs.add_search_path(dir) ? 1 : 0;
}

int opennova_vfs_set_primary_archive(OpennovaVfs *vfs, const char *pff_path) {
    if (!vfs || !pff_path) return 0;
    vfs->invalidate();
    return vfs->vfs.set_primary_archive(pff_path) ? 1 : 0;
}

int opennova_vfs_add_secondary_archive(OpennovaVfs *vfs, const char *pff_path) {
    if (!vfs || !pff_path) return 0;
    vfs->invalidate();
    return vfs->vfs.add_secondary_archive(pff_path) ? 1 : 0;
}

int opennova_vfs_mount_game(OpennovaVfs *vfs, const char *game_root, const char *expansion) {
    if (!vfs || !game_root) return 0;
    vfs->invalidate();
    return vfs->vfs.mount_game(game_root, expansion ? std::string(expansion) : std::string()) ? 1 : 0;
}

void opennova_vfs_clear(OpennovaVfs *vfs) {
    if (!vfs) return;
    vfs->invalidate();
    vfs->vfs.clear();
}

int opennova_vfs_has_file(const OpennovaVfs *vfs, const char *name) {
    if (!vfs || !name) return 0;
    return vfs->vfs.has_file(name) ? 1 : 0;
}

int opennova_vfs_read_file(OpennovaVfs *vfs, const char *name, uint8_t **out, size_t *out_size) {
    return read_common(vfs, name, out, out_size, true);
}

int opennova_vfs_read_file_raw(OpennovaVfs *vfs, const char *name, uint8_t **out, size_t *out_size) {
    return read_common(vfs, name, out, out_size, false);
}

void opennova_vfs_free(uint8_t *buf) {
    std::free(buf);
}

int opennova_vfs_file_count(const OpennovaVfs *vfs) {
    if (!vfs) return 0;
    vfs->ensure_snapshot();
    return static_cast<int>(vfs->snapshot.size());
}

const char *opennova_vfs_file_name_at(const OpennovaVfs *vfs, int index) {
    if (!vfs) return nullptr;
    vfs->ensure_snapshot();
    if (index < 0 || static_cast<size_t>(index) >= vfs->snapshot.size()) return nullptr;
    return vfs->snapshot[static_cast<size_t>(index)].logical_name.c_str();
}

const char *opennova_vfs_file_source_at(const OpennovaVfs *vfs, int index) {
    if (!vfs) return nullptr;
    vfs->ensure_snapshot();
    if (index < 0 || static_cast<size_t>(index) >= vfs->snapshot.size()) return nullptr;
    return vfs->snapshot[static_cast<size_t>(index)].source == opennova::VfsSource::Archive
               ? "pff"
               : "loose";
}

const char *opennova_vfs_file_archive_at(const OpennovaVfs *vfs, int index) {
    if (!vfs) return nullptr;
    vfs->ensure_snapshot();
    if (index < 0 || static_cast<size_t>(index) >= vfs->snapshot.size()) return nullptr;
    const opennova::VfsFileLocation &loc = vfs->snapshot[static_cast<size_t>(index)];
    return loc.source == opennova::VfsSource::Archive ? loc.source_path.c_str() : "";
}

const char *opennova_vfs_last_error(const OpennovaVfs *vfs) {
    if (!vfs) return "";
    return vfs->vfs.last_error().c_str();
}

} // extern "C"
