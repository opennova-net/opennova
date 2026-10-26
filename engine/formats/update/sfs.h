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
#include <cstdio>
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

// Where an entry's bytes lie in a package file, for a reader that never holds the package: a
// whole game's release is hundreds of megabytes.
struct SfsEntrySpan {
	std::string name;
	uint64_t offset = 0; // from the start of the file
	uint32_t size = 0;
};

struct SfsSectionSpans {
	std::string expansion;
	std::vector<SfsEntrySpan> entries;
};

struct SfsLayout {
	std::string product;
	std::vector<SfsSectionSpans> sections;
};

// The package the open file ends with, walked as sfs_read walks it, each entry located and its
// bytes left in the file. False with `error` set as sfs_read's.
bool sfs_locate(std::FILE *file, SfsLayout &out, std::string &error);

// The package's bytes, to append to an updater executable (or to stand alone, which the service
// accepts). The first section is written without a PACKAGEBREAK when its expansion is empty.
// Empty when it is not sfs_writable.
std::vector<uint8_t> sfs_write(const SfsPackage &package);

// Whether sfs_write writes the package: every name fits its field, no entry is named
// PACKAGEBREAK, only the first section has no expansion, and the whole package fits the
// trailer's 32-bit offset (2 GiB). `bytes`, when given, gets the package's size.
bool sfs_writable(const SfsPackage &package, uint64_t *bytes = nullptr);

// sfs_write's bytes written to the open file at its position, a piece at a time, never all in
// memory; false when the package is not sfs_writable or a write fails.
bool sfs_write_file(const SfsPackage &package, std::FILE *file);

} // namespace opennova::update
