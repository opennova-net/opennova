#include "sfs.h"

#include <base/io/le.h>
#include <base/io/os_path.h>

#include <cstring>

namespace opennova::update {

namespace {

std::string fixed_string(const uint8_t *data, size_t bytes) {
	size_t length = 0;
	while (length < bytes && data[length] != 0)
		++length;
	return std::string(reinterpret_cast<const char *>(data), length);
}

void append_fixed(std::vector<uint8_t> &out, std::string_view text, size_t bytes) {
	const size_t at = out.size();
	out.resize(at + bytes, 0);
	std::memcpy(out.data() + at, text.data(), text.size());
}

} // namespace

bool sfs_has_trailer(const uint8_t *data, size_t size) {
	return data != nullptr && size >= 4 && io::read_u32_le(data + size - 4) == kSfsMagic;
}

bool sfs_read(const uint8_t *data, size_t size, SfsPackage &out, std::string &error) {
	if (!sfs_has_trailer(data, size) || size < kSfsTrailerBytes) {
		error = "not a single-file update (no 1SFS trailer)";
		return false;
	}
	const int64_t offset = static_cast<int32_t>(io::read_u32_le(data + size - 8));
	const int64_t first = static_cast<int64_t>(size) + offset;
	const size_t entries_end = size - kSfsTrailerBytes;
	if (offset > -static_cast<int64_t>(kSfsTrailerBytes) || first < 0) {
		error = "the 1SFS trailer's offset is outside the file";
		return false;
	}
	SfsPackage package;
	package.product = fixed_string(data + entries_end, kSfsLongNameBytes);
	package.sections.push_back(SfsSection{});
	// [orig: Cupdate2Dlg_ExtractSingleFileUpdate @ 0x405450 (UPDATE.EXE): the loop while
	//  the running offset is below -268]
	size_t at = static_cast<size_t>(first);
	while (at < entries_end) {
		if (entries_end - at < kSfsEntryHeaderBytes) {
			error = "a 1SFS entry header runs into the trailer";
			return false;
		}
		const std::string name = fixed_string(data + at, kSfsNameBytes);
		const uint32_t entry_size = io::read_u32_le(data + at + kSfsNameBytes);
		at += kSfsEntryHeaderBytes;
		if (name == kSfsPackageBreak) {
			if (entries_end - at < kSfsLongNameBytes) {
				error = "a PACKAGEBREAK's expansion name runs into the trailer";
				return false;
			}
			SfsSection section;
			section.expansion = fixed_string(data + at, kSfsLongNameBytes);
			package.sections.push_back(std::move(section));
			at += kSfsLongNameBytes;
			continue;
		}
		if (entries_end - at < entry_size) {
			error = "a 1SFS entry runs into the trailer";
			return false;
		}
		SfsEntry entry;
		entry.name = name;
		entry.data.assign(data + at, data + at + entry_size);
		package.sections.back().entries.push_back(std::move(entry));
		at += entry_size;
	}
	if (package.sections.front().entries.empty() && package.sections.size() > 1)
		package.sections.erase(package.sections.begin());
	out = std::move(package);
	return true;
}

bool sfs_locate(std::FILE *file, SfsLayout &out, std::string &error) {
	uint8_t trailer[kSfsTrailerBytes];
	if (file == nullptr || io::fseek64(file, 0, SEEK_END) != 0) {
		error = "the package cannot be read";
		return false;
	}
	const int64_t size = io::ftell64(file);
	if (size < static_cast<int64_t>(kSfsTrailerBytes) ||
			io::fseek64(file, size - static_cast<int64_t>(kSfsTrailerBytes), SEEK_SET) != 0 ||
			std::fread(trailer, 1, sizeof(trailer), file) != sizeof(trailer) ||
			!sfs_has_trailer(trailer, sizeof(trailer))) {
		error = "not a single-file update (no 1SFS trailer)";
		return false;
	}
	const int64_t offset = static_cast<int32_t>(io::read_u32_le(trailer + kSfsTrailerBytes - 8));
	const int64_t first = size + offset;
	const int64_t entries_end = size - static_cast<int64_t>(kSfsTrailerBytes);
	if (offset > -static_cast<int64_t>(kSfsTrailerBytes) || first < 0) {
		error = "the 1SFS trailer's offset is outside the file";
		return false;
	}
	SfsLayout layout;
	layout.product = fixed_string(trailer, kSfsLongNameBytes);
	layout.sections.push_back(SfsSectionSpans{});
	// The walk of sfs_read: an entry header, then its bytes or a PACKAGEBREAK's expansion.
	int64_t at = first;
	while (at < entries_end) {
		uint8_t header[kSfsEntryHeaderBytes];
		if (entries_end - at < static_cast<int64_t>(kSfsEntryHeaderBytes) ||
				io::fseek64(file, at, SEEK_SET) != 0 ||
				std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
			error = "a 1SFS entry header runs into the trailer";
			return false;
		}
		const std::string name = fixed_string(header, kSfsNameBytes);
		const uint32_t entry_size = io::read_u32_le(header + kSfsNameBytes);
		at += kSfsEntryHeaderBytes;
		if (name == kSfsPackageBreak) {
			uint8_t expansion[kSfsLongNameBytes];
			if (entries_end - at < static_cast<int64_t>(kSfsLongNameBytes) ||
					std::fread(expansion, 1, sizeof(expansion), file) != sizeof(expansion)) {
				error = "a PACKAGEBREAK's expansion name runs into the trailer";
				return false;
			}
			SfsSectionSpans section;
			section.expansion = fixed_string(expansion, kSfsLongNameBytes);
			layout.sections.push_back(std::move(section));
			at += kSfsLongNameBytes;
			continue;
		}
		if (entries_end - at < static_cast<int64_t>(entry_size)) {
			error = "a 1SFS entry runs into the trailer";
			return false;
		}
		layout.sections.back().entries.push_back({ name, static_cast<uint64_t>(at), entry_size });
		at += entry_size;
	}
	if (layout.sections.front().entries.empty() && layout.sections.size() > 1)
		layout.sections.erase(layout.sections.begin());
	out = std::move(layout);
	return true;
}

bool sfs_writable(const SfsPackage &package, uint64_t *bytes) {
	if (package.product.size() >= kSfsLongNameBytes)
		return false;
	uint64_t size = kSfsTrailerBytes;
	for (size_t s = 0; s < package.sections.size(); ++s) {
		const SfsSection &section = package.sections[s];
		if (section.expansion.size() >= kSfsLongNameBytes || (section.expansion.empty() && s != 0))
			return false;
		if (!section.expansion.empty())
			size += kSfsEntryHeaderBytes + kSfsLongNameBytes;
		for (const SfsEntry &entry : section.entries) {
			if (entry.name.empty() || entry.name.size() >= kSfsNameBytes ||
					entry.name == kSfsPackageBreak || entry.data.size() > UINT32_MAX)
				return false;
			size += kSfsEntryHeaderBytes + entry.data.size();
		}
	}
	// The trailer's offset to the first entry is an i32.
	if (size > static_cast<uint64_t>(INT32_MAX) + 1)
		return false;
	if (bytes != nullptr)
		*bytes = size;
	return true;
}

namespace {

// The package's bytes in order, each piece handed to `emit` (false stops the write).
template <class Emit> bool emit_package(const SfsPackage &package, Emit emit) {
	uint64_t size = 0;
	if (!sfs_writable(package, &size))
		return false;
	std::vector<uint8_t> field;
	const auto fixed = [&](std::string_view text, size_t bytes) {
		field.clear();
		append_fixed(field, text, bytes);
		return emit(field.data(), field.size());
	};
	const auto u32 = [&](uint32_t value) {
		field.clear();
		io::append_u32_le(field, value);
		return emit(field.data(), field.size());
	};
	for (const SfsSection &section : package.sections) {
		if (!section.expansion.empty() &&
				!(fixed(kSfsPackageBreak, kSfsNameBytes) && u32(0) &&
						fixed(section.expansion, kSfsLongNameBytes)))
			return false;
		for (const SfsEntry &entry : section.entries)
			if (!fixed(entry.name, kSfsNameBytes) || !u32(static_cast<uint32_t>(entry.data.size())) ||
					!emit(entry.data.data(), entry.data.size()))
				return false;
	}
	const int32_t offset = static_cast<int32_t>(-static_cast<int64_t>(size));
	return fixed(package.product, kSfsLongNameBytes) && u32(static_cast<uint32_t>(offset)) &&
			u32(kSfsMagic);
}

} // namespace

std::vector<uint8_t> sfs_write(const SfsPackage &package) {
	std::vector<uint8_t> out;
	const bool written = emit_package(package, [&out](const uint8_t *data, size_t size) {
		out.insert(out.end(), data, data + size);
		return true;
	});
	if (!written)
		out.clear();
	return out;
}

bool sfs_write_file(const SfsPackage &package, std::FILE *file) {
	return file != nullptr && emit_package(package, [file](const uint8_t *data, size_t size) {
		return size == 0 || std::fwrite(data, 1, size, file) == size;
	});
}

} // namespace opennova::update
