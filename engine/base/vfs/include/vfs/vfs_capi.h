#ifndef OPENNOVA_VFS_CAPI_H
#define OPENNOVA_VFS_CAPI_H

/* Flat C ABI over opennova::Vfs for FFI consumers (Python ctypes, etc.). The handle is
   opaque, so callers never mirror a C++ struct layout. Exported from opennova_shared. */

#include <stddef.h>
#include <stdint.h>

#include <io/export.h>
#define VFS_EXPORT OPENNOVA_API

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OpennovaVfs OpennovaVfs;

VFS_EXPORT OpennovaVfs *opennova_vfs_create(void);
VFS_EXPORT void opennova_vfs_destroy(OpennovaVfs *vfs);

/* Mount API. Each returns 1 on success, 0 on failure. */
VFS_EXPORT int opennova_vfs_add_search_path(OpennovaVfs *vfs, const char *dir);
VFS_EXPORT int opennova_vfs_set_primary_archive(OpennovaVfs *vfs, const char *pff_path);
VFS_EXPORT int opennova_vfs_add_secondary_archive(OpennovaVfs *vfs, const char *pff_path);

/* Mount mode for opennova_vfs_mount_game: matches opennova::VfsMountMode. */
#define OPENNOVA_VFS_MODE_LOOSE_ONLY 0               /* loose search paths only */
#define OPENNOVA_VFS_MODE_PACKED 1                   /* PFF archives only */
#define OPENNOVA_VFS_MODE_PACKED_WITH_LOOSE 2        /* both, loose overrides archives */

/* Game-faithful auto-config. `expansion` may be NULL or "" for base-game mounting. `mode` is
   one of the OPENNOVA_VFS_MODE_* constants; an out-of-range value falls back to packed+loose. */
VFS_EXPORT int opennova_vfs_mount_game(OpennovaVfs *vfs, const char *game_root,
                                       const char *expansion, int mode);

VFS_EXPORT void opennova_vfs_clear(OpennovaVfs *vfs);

/* Choose how read_file keys SCR payloads. `policy` is a gameprofile ScrPolicy value
   (0 = version-detect default, 1 = force DEFAULT/JO-Demo, 2 = force JO_DFX2, 3 = force shaders).
   Persists across mounts. Resolve a game's policy via gameprofile_scr_policy_for_code(). */
VFS_EXPORT void opennova_vfs_set_scr_policy(OpennovaVfs *vfs, int policy);

/* Resolution. has_file returns 1/0. read_file returns 1 on success and allocates *out
   (free with opennova_vfs_free); the bytes are SCR/BFC1-decoded. read_file_raw returns the
   stored bytes without payload decoding. */
VFS_EXPORT int opennova_vfs_has_file(const OpennovaVfs *vfs, const char *name);
VFS_EXPORT int opennova_vfs_read_file(OpennovaVfs *vfs, const char *name,
                                      uint8_t **out, size_t *out_size);
VFS_EXPORT int opennova_vfs_read_file_raw(OpennovaVfs *vfs, const char *name,
                                          uint8_t **out, size_t *out_size);
VFS_EXPORT void opennova_vfs_free(uint8_t *buf);

/* Enumeration. Indices are valid until the next mount mutation. The returned strings are
   owned by the VFS and remain valid until then. source is "loose" or "pff". */
VFS_EXPORT int opennova_vfs_file_count(const OpennovaVfs *vfs);
VFS_EXPORT const char *opennova_vfs_file_name_at(const OpennovaVfs *vfs, int index);
VFS_EXPORT const char *opennova_vfs_file_source_at(const OpennovaVfs *vfs, int index);
VFS_EXPORT const char *opennova_vfs_file_archive_at(const OpennovaVfs *vfs, int index);

VFS_EXPORT const char *opennova_vfs_last_error(const OpennovaVfs *vfs);

#ifdef __cplusplus
}
#endif

#endif // OPENNOVA_VFS_CAPI_H
