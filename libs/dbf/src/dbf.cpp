// DBF (DLG0) dialog-bank parser. Ported from the pre-repo prototype.
// [orig: DialogManager_LoadFromFile @ 0x44e650 — header 0x1C, groups 0x34 each
//  followed by 68-byte lines (re-read as one 68*n+52 blob @ 0x44e79d)]
#include "dbf/dbf.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <io/strutil.h>

namespace opennova {
namespace dbf {

namespace {

std::string read_cstring(const char *data, size_t max_len) {
	size_t len = 0;
	while (len < max_len && data[len] != '\0') {
		++len;
	}
	return std::string(data, len);
}

using opennova::strutil::iequals;

}  // namespace

bool parse_dbf_memory(const uint8_t *data, size_t size, File &out, std::string &error) {
	out = File{};

	if (size < sizeof(Header)) {
		error = "File too small for header";
		return false;
	}

	std::memcpy(&out.header, data, sizeof(Header));

	if (out.header.magic != kMagic) {
		error = "Invalid magic: expected DLG0";
		return false;
	}
	if (out.header.header_size != 28) {
		error = "Invalid header size: expected 28, got " + std::to_string(out.header.header_size);
		return false;
	}
	if (out.header.id_def_count != 0) {
		// The engine reads 32 * id_def_count bytes at id_defs_offset ("DlgMgr
		// IDDEFS" [orig: DialogManager_LoadFromFile @ 0x44e6f7]); no JO mission
		// .DBF ships id-defs and this parser does not model the table, so
		// re-encoding would drop it. Reject loudly instead of corrupting.
		error = "id-def table not supported (header dword 3 nonzero)";
		return false;
	}

	// Groups start at entries_offset [orig: seek to dialogs_offset @ 0x44e723];
	// id_defs_offset points at the (empty) id-def table, not the groups.
	size_t offset = out.header.entries_offset;
	for (uint32_t i = 0; i < out.header.entry_count; ++i) {
		if (offset + sizeof(RawGroupRecord) > size) {
			error = "Truncated at group " + std::to_string(i);
			return false;
		}

		RawGroupRecord raw_group;
		std::memcpy(&raw_group, data + offset, sizeof(RawGroupRecord));
		offset += sizeof(RawGroupRecord);

		if (raw_group.record_size != 52) {
			error = "Invalid group record size at group " + std::to_string(i) +
					": expected 52, got " + std::to_string(raw_group.record_size);
			return false;
		}

		Group group;
		group.group_name = read_cstring(raw_group.group_name, sizeof(raw_group.group_name));
		group.idlist_count = raw_group.idlist_count;
		group.def_id_indices.assign(raw_group.def_id_indices,
				raw_group.def_id_indices + sizeof(raw_group.def_id_indices));

		for (uint32_t j = 0; j < raw_group.line_count; ++j) {
			if (offset + sizeof(RawLineRecord) > size) {
				error = "Truncated at line " + std::to_string(j) + " of group " + std::to_string(i);
				return false;
			}

			RawLineRecord raw_line;
			std::memcpy(&raw_line, data + offset, sizeof(RawLineRecord));
			offset += sizeof(RawLineRecord);

			Line line;
			line.line_flags = raw_line.line_flags;
			line.def_id_name = read_cstring(raw_line.def_id_name, sizeof(raw_line.def_id_name));
			line.sequence = read_cstring(raw_line.sequence, sizeof(raw_line.sequence));
			line.def_id_index = raw_line.def_id_index;
			line.delay = raw_line.delay;
			line.param = raw_line.param;
			line.resd0 = raw_line.resd0;
			line.resd1 = raw_line.resd1;

			group.lines.push_back(std::move(line));
		}

		out.groups.push_back(std::move(group));
	}

	return true;
}

bool parse_dbf(const std::string &path, File &out, std::string &error) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "Failed to open file: " + path;
		return false;
	}
	f.seekg(0, std::ios::end);
	auto size = f.tellg();
	f.seekg(0, std::ios::beg);
	std::vector<uint8_t> buf(static_cast<size_t>(size));
	f.read(reinterpret_cast<char *>(buf.data()), size);
	if (!f) {
		error = "Failed to read file: " + path;
		return false;
	}
	return parse_dbf_memory(buf.data(), buf.size(), out, error);
}

bool encode_dbf(const File &file, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	(void)error;

	size_t total_size = sizeof(Header);
	for (const auto &group : file.groups) {
		total_size += sizeof(RawGroupRecord);
		total_size += sizeof(RawLineRecord) * group.lines.size();
	}

	out.resize(total_size, 0);
	size_t offset = 0;

	Header header = file.header;
	header.magic = kMagic;
	header.version = 0x100;
	header.header_size = 28;
	header.id_def_count = 0;
	header.id_defs_offset = 28;
	header.entry_count = static_cast<uint32_t>(file.groups.size());
	header.entries_offset = 28;

	std::memcpy(out.data() + offset, &header, sizeof(Header));
	offset += sizeof(Header);

	uint32_t seq_counter = 0;
	for (const auto &group : file.groups) {
		RawGroupRecord raw_group{};
		raw_group.record_size = 52;
		size_t name_len = std::min(group.group_name.size(), sizeof(raw_group.group_name) - 1);
		std::memcpy(raw_group.group_name, group.group_name.c_str(), name_len);
		raw_group.line_count = static_cast<uint32_t>(group.lines.size());
		raw_group.idlist_count = group.idlist_count;
		size_t indices_len = std::min(group.def_id_indices.size(), sizeof(raw_group.def_id_indices));
		std::memcpy(raw_group.def_id_indices, group.def_id_indices.data(), indices_len);
		std::memcpy(out.data() + offset, &raw_group, sizeof(RawGroupRecord));
		offset += sizeof(RawGroupRecord);

		for (const auto &line : group.lines) {
			RawLineRecord raw_line{};
			raw_line.line_flags = line.line_flags;
			size_t def_len = std::min(line.def_id_name.size(), sizeof(raw_line.def_id_name) - 1);
			std::memcpy(raw_line.def_id_name, line.def_id_name.c_str(), def_len);
			if (!line.sequence.empty()) {
				size_t seq_len = std::min(line.sequence.size(), sizeof(raw_line.sequence) - 1);
				std::memcpy(raw_line.sequence, line.sequence.c_str(), seq_len);
			} else {
				char seq_buf[8];
				std::snprintf(seq_buf, sizeof(seq_buf), "_%05u", seq_counter++);
				std::memcpy(raw_line.sequence, seq_buf, std::strlen(seq_buf));
			}
			raw_line.def_id_index = line.def_id_index;
			raw_line.delay = line.delay;
			raw_line.padding = 0;
			raw_line.param = line.param;
			raw_line.resd0 = line.resd0;
			raw_line.resd1 = line.resd1;
			std::memcpy(out.data() + offset, &raw_line, sizeof(RawLineRecord));
			offset += sizeof(RawLineRecord);
		}
	}

	return true;
}

bool write_dbf(const File &file, const std::string &path, std::string &error) {
	std::vector<uint8_t> buf;
	if (!encode_dbf(file, buf, error)) {
		return false;
	}
	std::ofstream f(path, std::ios::binary);
	if (!f) {
		error = "Failed to open file for writing: " + path;
		return false;
	}
	f.write(reinterpret_cast<const char *>(buf.data()), buf.size());
	if (!f) {
		error = "Failed to write file: " + path;
		return false;
	}
	return true;
}

const Group *find_group(const File &file, const std::string &name) {
	for (const auto &group : file.groups) {
		if (iequals(group.group_name, name)) {
			return &group;
		}
	}
	return nullptr;
}

std::string dump_dbf(const File &file) {
	std::ostringstream ss;
	ss << "DBF groups (" << file.groups.size() << "):\n";
	for (size_t i = 0; i < file.groups.size(); ++i) {
		const auto &group = file.groups[i];
		ss << "  [" << i << "] " << group.group_name << " (lines: " << group.lines.size() << ")\n";
		for (const auto &line : group.lines) {
			ss << "      " << line.def_id_name << " delay=" << static_cast<int>(line.delay) << "\n";
		}
	}
	return ss.str();
}

}  // namespace dbf
}  // namespace opennova
