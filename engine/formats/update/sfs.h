#pragma once

// 1SFS, the single-file update: a package of update files appended to an updater executable,
// read from the end of the file. docs/update/updater-re.md §9.
// [orig: Cupdate2Dlg_ExtractSingleFileUpdate @ 0x405450 (UPDATE.EXE)]
//
// From the end: u32 magic "1SFS", i32 the first entry's offset from the end (negative),
// char product[260]. Before them the entries: char name[16]; u32 size; size bytes. An entry
// named PACKAGEBREAK is followed by char expansion[260] instead of a payload and switches the
// rest to that expansion's update directory.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::update {

inline constexpr uint32_t kSfsMagic = 0x53465331u; // bytes "1SFS"
inline constexpr size_t kSfsNameBytes = 16; // an entry name, its terminator among them
inline constexpr size_t kSfsLongNameBytes = 260; // the product, an expansion
inline constexpr size_t kSfsEntryHeaderBytes = 20; // name[16] + u32 size
inline constexpr size_t kSfsTrailerBytes = 268; // product[260] + i32 offset + u32 magic
inline constexpr std::string_view kSfsPackageBreak = "PACKAGEBREAK";

struct SfsEntry {
	std::string name; // at most 15 characters; the file written to <update dir>\<name>
	std::vector<uint8_t> data;
};

// The base product's entries (expansion empty), then one section per PACKAGEBREAK.
struct SfsSection {
	std::string expansion;
	std::vector<SfsEntry> entries;
};

struct SfsPackage {
	// HKLM\Software\Novalogic\<product>\1.00.0000 names the install the package updates.
	std::string product;
	std::vector<SfsSection> sections;
};

// True when `data` ends in a 1SFS trailer. [orig: Cupdate2App_IsSingleFileUpdate @ 0x4046f0
// (UPDATE.EXE)]
bool sfs_has_trailer(const uint8_t *data, size_t size);

// The package `data` ends with (an executable may precede it). False with `error` set when the
// trailer is absent or an entry runs past the trailer.
bool sfs_read(const uint8_t *data, size_t size, SfsPackage &out, std::string &error);

// The package's bytes, to append to an updater executable (or to stand alone, which the service
// accepts). The first section is written without a PACKAGEBREAK when its expansion is empty.
// Empty when a name does not fit its field, an entry is named PACKAGEBREAK, or a later section
// has no expansion.
std::vector<uint8_t> sfs_write(const SfsPackage &package);

} // namespace opennova::update
