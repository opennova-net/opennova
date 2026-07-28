#include "rtxt_string_file.h"

#include "util/nova_cp1252.h"
#include "util/nova_data_format.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace godot;

namespace {

// Retail string tables are cp1252, not UTF-8 (67 of the 98 JO bins carry bytes
// >= 0x80: curly quotes, accented characters). Decode UTF-8 when the bytes are
// valid UTF-8 (covers ASCII and anything we wrote ourselves), otherwise fall
// back to cp1252; encode back to cp1252 whenever every character fits so edits
// to retail files keep the game-readable encoding. Unedited entries never pass
// through String at all — parse/write preserve their bytes exactly.

bool is_valid_utf8(const std::string &s) {
	size_t i = 0;
	while (i < s.size()) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		size_t len = 0;
		if (c < 0x80) {
			len = 1;
		} else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
			len = 2;
		} else if ((c & 0xF0) == 0xE0) {
			len = 3;
		} else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
			len = 4;
		} else {
			return false;
		}
		if (i + len > s.size()) {
			return false;
		}
		for (size_t k = 1; k < len; ++k) {
			if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
				return false;
			}
		}
		i += len;
	}
	return true;
}

String std_to_gd(const std::string &s) {
	if (is_valid_utf8(s)) {
		return String::utf8(s.c_str(), static_cast<int>(s.length()));
	}
	String out;
	for (const char raw : s) {
		const unsigned char c = static_cast<unsigned char>(raw);
		out += opennova::cp1252_decode_byte(c);
	}
	return out;
}

std::string gd_to_std(const String &s) {
	// Prefer cp1252 so edited entries in retail tables stay engine-readable.
	std::string cp1252;
	cp1252.reserve(static_cast<size_t>(s.length()));
	bool fits = true;
	for (int i = 0; i < s.length(); ++i) {
		const char32_t cp = s[i];
		std::uint8_t encoded = 0;
		if (!opennova::cp1252_encode_codepoint(cp, encoded)) {
			fits = false;
			break;
		}
		cp1252.push_back(static_cast<char>(encoded));
	}
	if (fits) {
		return cp1252;
	}
	const CharString utf8 = s.utf8();
	return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

} // namespace

RtxtStringFile::RtxtStringFile() {
	file_.build_lookup();
}

void RtxtStringFile::_refresh() {
	// Keep each section's string_count consistent with the entries that point at
	// it, then rebuild the case-insensitive lookup. Called after every mutation;
	// never on plain load, so an untouched file saves back byte-for-byte.
	for (size_t s = 0; s < file_.sections.size(); ++s) {
		uint32_t count = 0;
		for (const auto &entry : file_.entries) {
			if (entry.section_index == s) {
				++count;
			}
		}
		file_.sections[s].string_count = count;
	}
	file_.build_lookup();
}

// --- Read ---

String RtxtStringFile::get_string(const StringName &p_key) const {
	return std_to_gd(file_.get(gd_to_std(String(p_key))));
}

bool RtxtStringFile::has_string(const StringName &p_key) const {
	return file_.has(gd_to_std(String(p_key)));
}

String RtxtStringFile::get_string_in_section(const String &p_section, const StringName &p_key) const {
	return std_to_gd(file_.get_in_section(gd_to_std(p_section), gd_to_std(String(p_key))));
}

bool RtxtStringFile::has_string_in_section(const String &p_section, const StringName &p_key) const {
	return file_.find_in_section(gd_to_std(p_section), gd_to_std(String(p_key))) != nullptr;
}

int RtxtStringFile::find_entry_in_section(const String &p_section, const StringName &p_key) const {
	const opennova::rtxt::Entry *entry =
			file_.find_in_section(gd_to_std(p_section), gd_to_std(String(p_key)));
	if (entry == nullptr) {
		return -1;
	}
	return static_cast<int>(entry - file_.entries.data());
}

Vector2i RtxtStringFile::get_position(const StringName &p_key) const {
	const int idx = find_entry_by_key(p_key);
	if (idx < 0) {
		return Vector2i();
	}
	return get_entry_position(idx);
}

int RtxtStringFile::get_section_index_for_key(const StringName &p_key) const {
	const int idx = find_entry_by_key(p_key);
	if (idx < 0) {
		return -1;
	}
	return static_cast<int>(file_.entries[idx].section_index);
}

PackedStringArray RtxtStringFile::get_keys() const {
	PackedStringArray keys;
	keys.resize(static_cast<int64_t>(file_.entries.size()));
	for (size_t i = 0; i < file_.entries.size(); ++i) {
		keys.set(static_cast<int64_t>(i), std_to_gd(file_.entries[i].key));
	}
	return keys;
}

int RtxtStringFile::get_entry_count() const {
	return static_cast<int>(file_.entries.size());
}

// --- Section read ---

int RtxtStringFile::get_section_count() const {
	return static_cast<int>(file_.sections.size());
}

PackedStringArray RtxtStringFile::get_section_names() const {
	PackedStringArray names;
	names.resize(static_cast<int64_t>(file_.sections.size()));
	for (size_t i = 0; i < file_.sections.size(); ++i) {
		names.set(static_cast<int64_t>(i), std_to_gd(file_.sections[i].name));
	}
	return names;
}

String RtxtStringFile::get_section_name(int p_section_index) const {
	ERR_FAIL_INDEX_V(p_section_index, static_cast<int>(file_.sections.size()), String());
	return std_to_gd(file_.sections[p_section_index].name);
}

int RtxtStringFile::get_section_string_count(int p_section_index) const {
	ERR_FAIL_INDEX_V(p_section_index, static_cast<int>(file_.sections.size()), 0);
	return static_cast<int>(file_.sections[p_section_index].string_count);
}

PackedStringArray RtxtStringFile::get_section_keys(int p_section_index) const {
	PackedStringArray keys;
	for (const auto &entry : file_.entries) {
		if (static_cast<int>(entry.section_index) == p_section_index) {
			keys.push_back(std_to_gd(entry.key));
		}
	}
	return keys;
}

// --- Indexed entry access ---

String RtxtStringFile::get_entry_key(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, static_cast<int>(file_.entries.size()), String());
	return std_to_gd(file_.entries[p_index].key);
}

String RtxtStringFile::get_entry_text(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, static_cast<int>(file_.entries.size()), String());
	return std_to_gd(file_.entries[p_index].text);
}

Vector2i RtxtStringFile::get_entry_position(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, static_cast<int>(file_.entries.size()), Vector2i());
	const auto &pos = file_.entries[p_index].position;
	return Vector2i(pos.x, pos.y);
}

int RtxtStringFile::get_entry_section_index(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, static_cast<int>(file_.entries.size()), 0);
	return static_cast<int>(file_.entries[p_index].section_index);
}

int RtxtStringFile::find_entry_by_key(const StringName &p_key) const {
	const std::string upper = opennova::rtxt::to_upper(gd_to_std(String(p_key)));
	for (size_t i = 0; i < file_.entries.size(); ++i) {
		if (opennova::rtxt::to_upper(file_.entries[i].key) == upper) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

// --- Entry mutations ---

int RtxtStringFile::add_entry(const String &p_key, const String &p_text, int p_section_index, const Vector2i &p_position) {
	opennova::rtxt::Entry entry;
	entry.key = gd_to_std(p_key);
	entry.text = gd_to_std(p_text);
	entry.section_index = static_cast<uint32_t>(p_section_index < 0 ? 0 : p_section_index);
	entry.position.x = static_cast<int16_t>(p_position.x);
	entry.position.y = static_cast<int16_t>(p_position.y);
	// Insert at the end of the section's contiguous run: the engine derives entry
	// indices by accumulating section string_counts and requires grouped entries
	// [orig: TextResource_FindEntryBySectionAndKey @ 0x75D250].
	const int index = _section_insert_index(entry.section_index);
	file_.entries.insert(file_.entries.begin() + index, std::move(entry));
	_refresh();
	emit_signal("entries_structure_changed");
	return index;
}

void RtxtStringFile::remove_entry(int p_index) {
	ERR_FAIL_INDEX(p_index, static_cast<int>(file_.entries.size()));
	file_.entries.erase(file_.entries.begin() + p_index);
	_refresh();
	emit_signal("entries_structure_changed");
}

void RtxtStringFile::set_entry_key(int p_index, const String &p_key) {
	ERR_FAIL_INDEX(p_index, static_cast<int>(file_.entries.size()));
	file_.entries[p_index].key = gd_to_std(p_key);
	_refresh();  // key feeds the lookup map
	emit_signal("entries_structure_changed");
}

void RtxtStringFile::set_entry_text(int p_index, const String &p_text) {
	ERR_FAIL_INDEX(p_index, static_cast<int>(file_.entries.size()));
	file_.entries[p_index].text = gd_to_std(p_text);
	emit_signal("entry_text_changed", p_index);
}

void RtxtStringFile::set_entry_position(int p_index, const Vector2i &p_position) {
	ERR_FAIL_INDEX(p_index, static_cast<int>(file_.entries.size()));
	file_.entries[p_index].position.x = static_cast<int16_t>(p_position.x);
	file_.entries[p_index].position.y = static_cast<int16_t>(p_position.y);
	emit_signal("entry_text_changed", p_index);
}

int RtxtStringFile::set_entry_section_index(int p_index, int p_section_index) {
	ERR_FAIL_INDEX_V(p_index, static_cast<int>(file_.entries.size()), -1);
	const uint32_t target = static_cast<uint32_t>(p_section_index < 0 ? 0 : p_section_index);
	if (file_.entries[p_index].section_index == target) {
		return p_index;
	}
	// Move the entry to the end of its new section's run so the file stays
	// grouped (see add_entry). Returns the entry's new index.
	opennova::rtxt::Entry moved = file_.entries[p_index];
	moved.section_index = target;
	file_.entries.erase(file_.entries.begin() + p_index);
	int index = _section_insert_index(target);
	file_.entries.insert(file_.entries.begin() + index, std::move(moved));
	_refresh();  // section totals change
	emit_signal("entries_structure_changed");
	return index;
}

int RtxtStringFile::_section_insert_index(uint32_t p_section_index) const {
	// One past the last entry that belongs to a section <= the target: the end
	// of the target section's run in a grouped file, and a sane append point in
	// a not-yet-normalized one.
	int index = 0;
	for (size_t i = 0; i < file_.entries.size(); ++i) {
		if (file_.entries[i].section_index <= p_section_index) {
			index = static_cast<int>(i) + 1;
		}
	}
	return index;
}

bool RtxtStringFile::is_grouped() const {
	return file_.is_grouped();
}

void RtxtStringFile::normalize_grouping() {
	if (file_.is_grouped()) {
		return;
	}
	file_.normalize_grouping();
	_refresh();
	emit_signal("entries_structure_changed");
}

// --- Section CRUD ---

int RtxtStringFile::add_section(const String &p_name) {
	opennova::rtxt::Section section;
	section.name = gd_to_std(p_name);
	section.string_count = 0;
	file_.sections.push_back(std::move(section));
	_refresh();
	emit_signal("sections_changed");
	return static_cast<int>(file_.sections.size()) - 1;
}

void RtxtStringFile::remove_section(int p_index, int p_reassign_to) {
	ERR_FAIL_INDEX(p_index, static_cast<int>(file_.sections.size()));
	const int section_count = static_cast<int>(file_.sections.size());
	const bool reassign = p_reassign_to >= 0 && p_reassign_to < section_count && p_reassign_to != p_index;

	// Reassign or drop entries that pointed at the removed section.
	std::vector<opennova::rtxt::Entry> kept;
	kept.reserve(file_.entries.size());
	for (auto &entry : file_.entries) {
		if (static_cast<int>(entry.section_index) == p_index) {
			if (!reassign) {
				continue;  // drop
			}
			entry.section_index = static_cast<uint32_t>(p_reassign_to);
		}
		kept.push_back(std::move(entry));
	}
	file_.entries = std::move(kept);

	// Drop the section, then shift any higher index references down by one.
	file_.sections.erase(file_.sections.begin() + p_index);
	for (auto &entry : file_.entries) {
		if (static_cast<int>(entry.section_index) > p_index) {
			entry.section_index -= 1;
		}
	}

	// Reassigning to a lower-numbered section can leave the moved run sitting
	// after sections it now precedes numerically; restore the grouping invariant.
	if (!file_.is_grouped()) {
		file_.normalize_grouping();
	}

	_refresh();
	emit_signal("sections_changed");
	emit_signal("entries_structure_changed");
}

void RtxtStringFile::rename_section(int p_index, const String &p_name) {
	ERR_FAIL_INDEX(p_index, static_cast<int>(file_.sections.size()));
	file_.sections[p_index].name = gd_to_std(p_name);
	emit_signal("sections_changed");
}

// --- I/O ---

Error RtxtStringFile::load_from_path(const String &p_path) {
	PackedByteArray packed;
	if (!read_nova_payload_file(p_path, packed)) {
		return ERR_FILE_CANT_OPEN;
	}
	return load_from_byte_array(packed);
}

Error RtxtStringFile::save_to_path(const String &p_path) const {
	const PackedByteArray packed = to_byte_array();
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_FILE_CANT_WRITE;
	}
	file->store_buffer(packed);
	file->close();
	return OK;
}

void RtxtStringFile::reset_empty() {
	file_ = opennova::rtxt::File{};
	file_.build_lookup();
}

// --- Snapshot ---

PackedByteArray RtxtStringFile::to_byte_array() const {
	std::vector<uint8_t> bytes;
	std::string error;
	PackedByteArray out;
	if (!opennova::rtxt::write(file_, bytes, error)) {
		UtilityFunctions::push_warning("RtxtStringFile: serialize failed: ", error.c_str());
		return out;
	}
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	}
	return out;
}

Error RtxtStringFile::load_from_byte_array(const PackedByteArray &p_bytes) {
	std::vector<uint8_t> bytes(static_cast<size_t>(p_bytes.size()));
	if (!bytes.empty()) {
		std::memcpy(bytes.data(), p_bytes.ptr(), bytes.size());
	}
	opennova::rtxt::File parsed;
	std::string error;
	if (!opennova::rtxt::parse(bytes.data(), bytes.size(), parsed, error)) {
		UtilityFunctions::push_warning("RtxtStringFile: parse failed: ", error.c_str());
		return ERR_FILE_CORRUPT;
	}
	file_ = std::move(parsed);
	file_.build_lookup();
	return OK;
}

// --- Hotkey helpers ---

String RtxtStringFile::strip_hotkey(const String &p_text) {
	int idx = -1;
	return std_to_gd(opennova::rtxt::strip_hotkey(gd_to_std(p_text), idx));
}

Dictionary RtxtStringFile::strip_hotkey_with_index(const String &p_text) {
	int idx = -1;
	const std::string stripped = opennova::rtxt::strip_hotkey(gd_to_std(p_text), idx);
	Dictionary out;
	out["text"] = std_to_gd(stripped);
	out["index"] = idx;
	return out;
}

void RtxtStringFile::set_native(const opennova::rtxt::File &p_file) {
	file_ = p_file;
	file_.build_lookup();
}

void RtxtStringFile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_string", "key"), &RtxtStringFile::get_string);
	ClassDB::bind_method(D_METHOD("has_string", "key"), &RtxtStringFile::has_string);
	ClassDB::bind_method(D_METHOD("get_string_in_section", "section", "key"), &RtxtStringFile::get_string_in_section);
	ClassDB::bind_method(D_METHOD("has_string_in_section", "section", "key"), &RtxtStringFile::has_string_in_section);
	ClassDB::bind_method(D_METHOD("find_entry_in_section", "section", "key"), &RtxtStringFile::find_entry_in_section);
	ClassDB::bind_method(D_METHOD("is_grouped"), &RtxtStringFile::is_grouped);
	ClassDB::bind_method(D_METHOD("normalize_grouping"), &RtxtStringFile::normalize_grouping);
	ClassDB::bind_method(D_METHOD("get_position", "key"), &RtxtStringFile::get_position);
	ClassDB::bind_method(D_METHOD("get_section_index_for_key", "key"), &RtxtStringFile::get_section_index_for_key);
	ClassDB::bind_method(D_METHOD("get_keys"), &RtxtStringFile::get_keys);
	ClassDB::bind_method(D_METHOD("get_entry_count"), &RtxtStringFile::get_entry_count);

	ClassDB::bind_method(D_METHOD("get_section_count"), &RtxtStringFile::get_section_count);
	ClassDB::bind_method(D_METHOD("get_section_names"), &RtxtStringFile::get_section_names);
	ClassDB::bind_method(D_METHOD("get_section_name", "section_index"), &RtxtStringFile::get_section_name);
	ClassDB::bind_method(D_METHOD("get_section_string_count", "section_index"), &RtxtStringFile::get_section_string_count);
	ClassDB::bind_method(D_METHOD("get_section_keys", "section_index"), &RtxtStringFile::get_section_keys);

	ClassDB::bind_method(D_METHOD("get_entry_key", "index"), &RtxtStringFile::get_entry_key);
	ClassDB::bind_method(D_METHOD("get_entry_text", "index"), &RtxtStringFile::get_entry_text);
	ClassDB::bind_method(D_METHOD("get_entry_position", "index"), &RtxtStringFile::get_entry_position);
	ClassDB::bind_method(D_METHOD("get_entry_section_index", "index"), &RtxtStringFile::get_entry_section_index);
	ClassDB::bind_method(D_METHOD("find_entry_by_key", "key"), &RtxtStringFile::find_entry_by_key);

	ClassDB::bind_method(D_METHOD("add_entry", "key", "text", "section_index", "position"), &RtxtStringFile::add_entry);
	ClassDB::bind_method(D_METHOD("remove_entry", "index"), &RtxtStringFile::remove_entry);
	ClassDB::bind_method(D_METHOD("set_entry_key", "index", "key"), &RtxtStringFile::set_entry_key);
	ClassDB::bind_method(D_METHOD("set_entry_text", "index", "text"), &RtxtStringFile::set_entry_text);
	ClassDB::bind_method(D_METHOD("set_entry_position", "index", "position"), &RtxtStringFile::set_entry_position);
	ClassDB::bind_method(D_METHOD("set_entry_section_index", "index", "section_index"), &RtxtStringFile::set_entry_section_index);

	ClassDB::bind_method(D_METHOD("add_section", "name"), &RtxtStringFile::add_section);
	ClassDB::bind_method(D_METHOD("remove_section", "index", "reassign_to"), &RtxtStringFile::remove_section, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("rename_section", "index", "name"), &RtxtStringFile::rename_section);

	ClassDB::bind_method(D_METHOD("load_from_path", "path"), &RtxtStringFile::load_from_path);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &RtxtStringFile::save_to_path);
	ClassDB::bind_method(D_METHOD("reset_empty"), &RtxtStringFile::reset_empty);

	ClassDB::bind_method(D_METHOD("to_byte_array"), &RtxtStringFile::to_byte_array);
	ClassDB::bind_method(D_METHOD("load_from_byte_array", "bytes"), &RtxtStringFile::load_from_byte_array);

	ClassDB::bind_static_method("RtxtStringFile", D_METHOD("strip_hotkey", "text"), &RtxtStringFile::strip_hotkey);
	ClassDB::bind_static_method("RtxtStringFile", D_METHOD("strip_hotkey_with_index", "text"), &RtxtStringFile::strip_hotkey_with_index);

	ADD_SIGNAL(MethodInfo("entries_structure_changed"));
	ADD_SIGNAL(MethodInfo("entry_text_changed", PropertyInfo(Variant::INT, "index")));
	ADD_SIGNAL(MethodInfo("sections_changed"));
}
