#include "sfs.h"

#include <base/io/le.h>

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

std::vector<uint8_t> sfs_write(const SfsPackage &package) {
	if (package.product.size() >= kSfsLongNameBytes)
		return {};
	std::vector<uint8_t> out;
	for (size_t s = 0; s < package.sections.size(); ++s) {
		const SfsSection &section = package.sections[s];
		if (section.expansion.size() >= kSfsLongNameBytes)
			return {};
		if (!section.expansion.empty()) {
			append_fixed(out, kSfsPackageBreak, kSfsNameBytes);
			io::append_u32_le(out, 0);
			append_fixed(out, section.expansion, kSfsLongNameBytes);
		} else if (s != 0) {
			return {};
		}
		for (const SfsEntry &entry : section.entries) {
			if (entry.name.empty() || entry.name.size() >= kSfsNameBytes ||
					entry.name == kSfsPackageBreak || entry.data.size() > UINT32_MAX)
				return {};
			append_fixed(out, entry.name, kSfsNameBytes);
			io::append_u32_le(out, static_cast<uint32_t>(entry.data.size()));
			out.insert(out.end(), entry.data.begin(), entry.data.end());
		}
	}
	const int64_t offset = -static_cast<int64_t>(out.size() + kSfsTrailerBytes);
	if (offset < INT32_MIN)
		return {};
	append_fixed(out, package.product, kSfsLongNameBytes);
	io::append_u32_le(out, static_cast<uint32_t>(static_cast<int32_t>(offset)));
	io::append_u32_le(out, kSfsMagic);
	return out;
}

} // namespace opennova::update
