// The archive the retail packer writes, laid out byte by byte from integer data and apart from the
// writers under engine/formats/pff, so a test of those writers against it is never the writers
// against themselves: the 20-byte header (its size, the magic, the entry count, the record size,
// the directory's offset), the payloads back to back in the packer's list order, the directory in
// the same order (36 bytes a record: flags, offset, size, the +12 time, the name NUL-padded to 16
// bytes, the +32 checksum word), then the packer's 12-byte footer: the packing machine's IPv4
// address in network order, the word it keeps from the archive's earlier footer, and 'KING'
// [orig: sub_4040E0 @ 0x4041A3..0x40432F (pack.exe)] [orig: sub_403360 @ 0x403360 (pack.exe)]
// [orig: sub_4030E0 @ 0x40312B..0x40318A (pack.exe)].
//
// Every archive of the JO:CA install has this shape and nothing else (language, localres, resource,
// jox01 and jox01L.pff, 11,576 entries; given each one's entries, this builder lays out that archive
// byte for byte, checked once on 2026-10-19 before the retail row retired): PFF3, a 20-byte header,
// the payloads contiguous from offset 20 in directory order and never in name order, mixed-case
// names NUL-padded and none filling the field, no encrypted, empty or dead-space record, every
// entry stamped with a Unix time (473418166..1248404912) and a nonzero checksum word, and the
// footer c0a8a939 00000000 'KING' (formats/pff/pff.h PFF_RETAIL_FOOTER, D-VFS-15). The 2005
// build's packer stamps the checksum word with the plain CRC-32/BZIP2 of the stored bytes; the
// patch-era packer with the updater's file CRC (docs/update/updater-re.md §10).
#pragma once

#include <base/io/crc32_mpeg2.h>
#include <formats/pff/pff.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pff_packer {

struct Entry {
	std::string name;           // as the directory stores it, at most 16 bytes
	std::vector<uint8_t> data;  // the stored bytes
	uint32_t flags = 0;
	uint32_t time = 0;          // the +12 word
	uint32_t checksum = 0;      // the +32 word
};

// The 2005 build's checksum word: the plain (complemented) CRC-32/BZIP2 of the stored bytes.
inline uint32_t plain_checksum(const std::vector<uint8_t> &data) {
	return ~opennova::io::crc32_mpeg2(data.data(), data.size());
}

inline void put_le32(std::vector<uint8_t> &out, uint32_t v) {
	out.push_back(uint8_t(v));
	out.push_back(uint8_t(v >> 8));
	out.push_back(uint8_t(v >> 16));
	out.push_back(uint8_t(v >> 24));
}

// The packing machine every JO:CA archive names (192.168.169.57), in the footer's network order.
inline constexpr uint8_t kRetailAddress[4] = { 0xC0, 0xA8, 0xA9, 0x39 };

// The archive's bytes: `entries` in their list order, `magic` (PFF3 by default), the footer's kept word
// `kept` (0 in every JO:CA archive).
inline std::vector<uint8_t> archive(const std::vector<Entry> &entries,
		uint32_t magic = opennova::pff::PFF_MAGIC_PFF3, uint32_t kept = 0) {
	std::vector<uint8_t> out;
	uint32_t table = 20;
	for (const Entry &e : entries)
		table += uint32_t(e.data.size());
	put_le32(out, 20);
	put_le32(out, magic);
	put_le32(out, uint32_t(entries.size()));
	put_le32(out, 36);
	put_le32(out, table);
	for (const Entry &e : entries)
		out.insert(out.end(), e.data.begin(), e.data.end());
	uint32_t offset = 20;
	for (const Entry &e : entries) {
		put_le32(out, e.flags);
		put_le32(out, offset);
		put_le32(out, uint32_t(e.data.size()));
		put_le32(out, e.time);
		char name[16] = {};
		std::memcpy(name, e.name.data(), e.name.size() < 16 ? e.name.size() : 16);
		out.insert(out.end(), name, name + 16);
		put_le32(out, e.checksum);
		offset += uint32_t(e.data.size());
	}
	out.insert(out.end(), kRetailAddress, kRetailAddress + 4);
	put_le32(out, kept);
	out.insert(out.end(), { 'K', 'I', 'N', 'G' });
	return out;
}

// A payload of `size` bytes from integer data alone (`seed` picks the run), so every platform lays out
// the same archive.
inline std::vector<uint8_t> payload(uint32_t seed, size_t size) {
	std::vector<uint8_t> out(size);
	uint32_t x = seed * 2654435761u + 12345u;
	for (size_t i = 0; i < size; ++i) {
		x = x * 1103515245u + 12345u;
		out[i] = uint8_t(x >> 16);
	}
	return out;
}

} // namespace pff_packer
