#pragma once

// What a packed game directory holds: the rules a packer follows to turn a
// loose asset tree into the archive layout retail's boot mounts (the boot
// archive table itself is vfs.h kBootArchiveTable). Policy only; the walk and
// the writes are the packer's.

#include <base/vfs/vfs.h>
#include <formats/pff/pff.h>

namespace opennova {

// The archive a packer writes the loose tree into: the localres slot of the
// boot table.
inline constexpr const char *kPackArchiveName = "localres.pff";

// Files that must stay LOOSE in the game dir rather than going into the
// archive:
//  - `.sbf` music banks stream by path and never resolve through the
//    archives [orig: AudioVM_InitMenuMusicStreaming @0x56aa60].
//  - `earlyerr.txt` is the pre-archive error text, read before any mount
//    [orig: Game_ShowEarlyError @0x4a68a0] — inside an archive it could never
//    be read.
inline constexpr const char *kPackLooseExtensions[] = {".sbf", ".txt"};

// Never packed: the retail runtime, its own writes, and any archive already
// present (packing an archive into an archive). Retail staging accepts PFFs;
// runtime binaries still come only from the configured retail install. The
// repo-metadata tail lives beside the assets but is not game data.
inline constexpr const char *kPackExcludedExtensions[] = {
    ".pff", ".exe", ".dll", ".sav", ".log", ".ini", ".cfg",
    ".md", ".gitignore", ".gitattributes",
};

// Subdirectories of the asset root that hold source material, not game
// files: retail resolves bare filenames at the root, so nothing under `src/`
// is loadable. Any OTHER subdirectory is reported by the packer: it walks the
// root only, and a data directory that silently vanished from the pack is
// the bug that gets debugged in retail.
inline constexpr const char *kPackExcludedDirs[] = {"src/"};

// PFF entry names are 16 BYTES (formats/pff PFF_NAME_SIZE); a name that fits
// in 16 characters can still overflow in UTF-8.
inline constexpr int kPffNameBytes = PFF_NAME_SIZE;

} // namespace opennova
